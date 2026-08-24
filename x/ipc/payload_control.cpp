#include "payload_control.h"

#include "../../common/xcat_access_deny_sticky.h"
#include "../../common/xcat_payload_control.h"
#include "../features/auto_enter/auto_enter.h"
#include "../features/autopot/autopot.h"
#include "../features/invuln/invuln.h"
#include "../features/attack_accel/attack_accel.h"
#include "../features/final_attack_force/final_attack_force.h"
#include "../features/skill_max_level/skill_max_level.h"
#include "../features/multi_skill/multi_skill.h"
#include "../features/pet_feed/pet_feed.h"
#include "../features/auto_lie/auto_lie.h"
#include "../features/channel_hop/channel_hop.h"
#include "../features/encounter/encounter.h"
#include "../features/ports/attack_rpc_port.h"
#include "../features/ports/map_attack_port.h"
#include "../features/ports/mob_gather_port.h"
#include "../features/ports/mob_fh_ban.h"
#include "../features/ports/attack_input_port.h"
#include "../features/ports/curfh_gate_bypass.h"
#include "../features/ports/mob_prevpos_patch.h"
#include "../features/ports/ground_spoof.h"
#include "../features/ports/teleport_port.h"
#include "../features/ports/travel_port.h"
#include "../features/ports/world_port.h"
#include "../features/melee_veto/melee_veto.h"
#include "../features/simple_combat/simple_combat.h"
#include "../features/mob_scan/mob_scan.h"
#include "../features/mob_pool_observe/mob_pool_observe.h"
#include "../features/fly/fly.h"
#include "../features/drop_alert_bypass/drop_alert_bypass.h"
#include "../features/force_trade/force_trade.h"
#include "../features/auction_town_bypass/auction_town_bypass.h"
#include "../features/auction_gate_probe/auction_gate_probe.h"
#include "../features/ui_cheat_overlay/ui_cheat_overlay.h"
#include "../features/rest_mp_accel/rest_mp_accel.h"
#include "../features/infinite_stars/infinite_stars.h"
#include "../features/player_hide/player_hide.h"
#include "../features/frame_lock/frame_lock.h"
#include "../features/movepath_flush_probe/movepath_flush_probe.h"
#include "../features/galaxy_token_probe/galaxy_token_probe.h"
#include "../features/soft_login_probe/soft_login_probe.h"
#include "../runtime/log.h"
#include "../runtime/main_thread_pump.h"
#include "../runtime/managed_main.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace x::ipc {
namespace {

std::atomic<uint64_t> gLastAppliedTick{0};
std::atomic<bool> gHaveApplied{false};
// 冷启动 play-boot 完成前：invuln worker 已 Poll，但 F5/1ms/10×/FhBan 不得提前灌。
// BIN 13:02:34 一进图就 SetEnabled → WarmInstall ~280ms，随后 Init 又卸 BAN，ForceApply 再开。
std::atomic<bool> gPlayBootApplyReady{false};

// DLL 加载时刻（文件域静态，勿放进 Apply 首次非 0 才初始化——否则 App 写盘 tick
// 总早于 Apply 内 GetTickCount64，首点 afterInject 恒为 0，见本地 19:02:30 adopt）。
const uint64_t kManualRejoinModuleStartMs = GetTickCount64();
// App 写盘 → DLL 读到之间的正常抖动；仅放宽 afterInject，不放宽「注入前旧 tick」。
constexpr uint64_t kManualRejoinWriteSkewMs = 5000;

std::string PayloadBinDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PayloadBinDir), &self) ||
        !self)
        return {};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return {};
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    s.resize(slash);
    char narrow[MAX_PATH]{};
    if (!WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr))
        return {};
    return std::string(narrow);
}

// 落盘已消费 seq，避免「注入前点过按钮 / 残留 seq」被 init 吞掉或重复误触发。
std::string LieSeqStampPath(const char* name) {
    const std::string bin = PayloadBinDir();
    if (bin.empty()) return {};
    return bin + "\\state\\lie_ai\\" + name;
}

uint32_t ReadLieSeqStamp(const char* name) {
    const std::string path = LieSeqStampPath(name);
    if (path.empty()) return 0;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return 0;
    unsigned v = 0;
    (void)fscanf_s(fp, "%u", &v);
    fclose(fp);
    return static_cast<uint32_t>(v);
}

void WriteLieSeqStamp(const char* name, uint32_t seq) {
    const std::string path = LieSeqStampPath(name);
    if (path.empty()) return;
    // 确保目录存在
    const std::string dir = path.substr(0, path.find_last_of("\\/"));
    if (!dir.empty()) CreateDirectoryA(dir.c_str(), nullptr);
    // parent lie_ai may need state first
    const std::string state = PayloadBinDir() + "\\state";
    CreateDirectoryA(state.c_str(), nullptr);
    CreateDirectoryA((state + "\\lie_ai").c_str(), nullptr);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return;
    fprintf(fp, "%u\n", seq);
    fclose(fp);
}

void ApplyManualRejoinSeq(const xcat::PayloadControl& c) {
    // 见过 seq=0：control 曾空闲/被清零；之后注入后新写的非 0 seq 视为新点击（即使 ≤ stamp）。
    static std::atomic<bool> s_sawZeroAfterInject{false};
    if (c.manualRejoinSeq == 0) {
        s_sawZeroAfterInject.store(true, std::memory_order_release);
        return;
    }
    const uint32_t diskLast = ReadLieSeqStamp("last_manual_rejoin_seq.txt");
    // 注入后首次 Apply：默认收养 stamp，禁止重放「进图前残留」F10（BIN reinject seq=10）。
    // 放行条件（须 writeTickMs 落在本 DLL 加载附近/之后）：
    //   · ini seq > stamp，或
    //   · ini seq < stamp（control 清零后重从 1 计），或
    //   · 本模块生命周期内曾见过 seq=0 再出现非 0（073f12：stamp≥1 且点出 seq=1）
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    const bool writtenAfterInject =
        c.writeTickMs != 0 &&
        (c.writeTickMs + kManualRejoinWriteSkewMs) >= kManualRejoinModuleStartMs;
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        const bool newerThanStamp = c.manualRejoinSeq > diskLast;
        const bool seqBelowStamp = diskLast != 0 && c.manualRejoinSeq < diskLast;
        const bool afterIdleZero = s_sawZeroAfterInject.load(std::memory_order_acquire);
        if (writtenAfterInject && (newerThanStamp || seqBelowStamp || afterIdleZero)) {
            s_lastApplied.store(c.manualRejoinSeq, std::memory_order_release);
            WriteLieSeqStamp("last_manual_rejoin_seq.txt", c.manualRejoinSeq);
            s_sawZeroAfterInject.store(false, std::memory_order_release);
            x::runtime::LogI("Control",
                             "manualRejoinSeq=%u → hop (boot fire stamp=%u newer=%d below=%d "
                             "idle0=%d writeTick=%llu modStart=%llu)",
                             c.manualRejoinSeq, diskLast, newerThanStamp ? 1 : 0,
                             seqBelowStamp ? 1 : 0, afterIdleZero ? 1 : 0,
                             (unsigned long long)c.writeTickMs,
                             (unsigned long long)kManualRejoinModuleStartMs);
            x::features::channel_hop::RequestManualRejoin(c.manualRejoinSeq);
            return;
        }
        const uint32_t adopt =
            c.manualRejoinSeq > diskLast ? c.manualRejoinSeq : diskLast;
        s_lastApplied.store(adopt, std::memory_order_release);
        if (adopt != diskLast) {
            WriteLieSeqStamp("last_manual_rejoin_seq.txt", adopt);
        }
        x::runtime::LogI("Control",
                         "manualRejoinSeq=%u adopt stamp=%u→%u (no hop, afterInject=%d "
                         "writeTick=%llu modStart=%llu)",
                         c.manualRejoinSeq, diskLast, adopt, writtenAfterInject ? 1 : 0,
                         (unsigned long long)c.writeTickMs,
                         (unsigned long long)kManualRejoinModuleStartMs);
        return;
    }
    // 进程内 CAS：防同 tick 双 Apply 都读到旧 stamp → 双 request（BIN seq=18）
    uint32_t prev = s_lastApplied.load(std::memory_order_acquire);
    if (prev == 0 && diskLast != 0) {
        uint32_t expect0 = 0;
        (void)s_lastApplied.compare_exchange_strong(expect0, diskLast,
                                                    std::memory_order_acq_rel);
        prev = s_lastApplied.load(std::memory_order_acquire);
    }
    for (;;) {
        // control 清零后重计：seq 回落到 prev 以下且写盘在注入后 → 放行（勿永卡在旧 stamp）
        if (c.manualRejoinSeq < prev && writtenAfterInject) {
            if (s_lastApplied.compare_exchange_weak(prev, c.manualRejoinSeq,
                                                    std::memory_order_acq_rel)) {
                WriteLieSeqStamp("last_manual_rejoin_seq.txt", c.manualRejoinSeq);
                s_sawZeroAfterInject.store(false, std::memory_order_release);
                x::runtime::LogI("Control",
                                 "manualRejoinSeq=%u → hop (reset below prev=%u)",
                                 c.manualRejoinSeq, prev);
                x::features::channel_hop::RequestManualRejoin(c.manualRejoinSeq);
                return;
            }
            continue;
        }
        if (c.manualRejoinSeq <= prev) return;
        if (s_lastApplied.compare_exchange_weak(prev, c.manualRejoinSeq,
                                                std::memory_order_acq_rel)) {
            WriteLieSeqStamp("last_manual_rejoin_seq.txt", c.manualRejoinSeq);
            s_sawZeroAfterInject.store(false, std::memory_order_release);
            x::features::channel_hop::RequestManualRejoin(c.manualRejoinSeq);
            return;
        }
    }
}

void ApplySoftLoginDismissSeq(const xcat::PayloadControl& c) {
    if (c.softLoginDismissSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_soft_login_dismiss_seq.txt");
    if (c.softLoginDismissSeq <= last) return;
    WriteLieSeqStamp("last_soft_login_dismiss_seq.txt", c.softLoginDismissSeq);
    x::runtime::LogI("Control", "softLoginDismissSeq=%u → manual dismiss", c.softLoginDismissSeq);
    x::features::soft_login_probe::RequestManualDismiss();
}

void ApplyAutoLieAlarmTestSeq(const xcat::PayloadControl& c) {
    if (c.autoLieAlarmTestSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_alarm_seq.txt");
    if (c.autoLieAlarmTestSeq == last) return;
    WriteLieSeqStamp("last_alarm_seq.txt", c.autoLieAlarmTestSeq);
    x::features::auto_lie::StartAlarmTest();
}

void ApplyAutoLieMouseSmokeSeq(const xcat::PayloadControl& c) {
    if (c.autoLieMouseSmokeSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_mouse_smoke_seq.txt");
    if (c.autoLieMouseSmokeSeq == last) return;
    WriteLieSeqStamp("last_mouse_smoke_seq.txt", c.autoLieMouseSmokeSeq);
    x::features::auto_lie::StartMouseSmoke();
}

void ApplyAutoLieMouseRegionOverlay(const xcat::PayloadControl& c) {
    x::features::auto_lie::SetMouseRegionOverlay(c.autoLieMouseRegionOverlay != 0);
}

void ApplyAutoLieMouseSimSeq(const xcat::PayloadControl& c) {
    if (c.autoLieMouseSimSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_mouse_sim_seq.txt");
    if (c.autoLieMouseSimSeq == last) return;
    WriteLieSeqStamp("last_mouse_sim_seq.txt", c.autoLieMouseSimSeq);
    x::features::auto_lie::StartMouseSim(c.autoLieMouseSimSeq);
}

void ApplyTeleportTestSeq(const xcat::PayloadControl& c) {
    // 封禁风险：调试贴怪瞬移已禁用；只收养 seq，不开火。
    if (c.teleportTestSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_test_seq.txt");
    if (c.teleportTestSeq <= last) return;
    WriteLieSeqStamp("last_teleport_test_seq.txt", c.teleportTestSeq);
    x::runtime::LogW("Control", "teleportTestSeq=%u refused (native fill+Doing disabled)",
                     c.teleportTestSeq);
}

void ApplyTeleportNativeTestSeq(const xcat::PayloadControl& c) {
    if (c.teleportNativeTestSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_native_test_seq.txt");
    if (c.teleportNativeTestSeq <= last) return;
    WriteLieSeqStamp("last_teleport_native_test_seq.txt", c.teleportNativeTestSeq);
    x::runtime::LogW("Control", "teleportNativeTestSeq=%u refused (native fill+Doing disabled)",
                     c.teleportNativeTestSeq);
}

void ApplyTeleportKickStressSeq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_seq.txt");
    if (c.teleportKickStressSeq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_seq.txt", c.teleportKickStressSeq);
    x::runtime::LogW("Control", "teleportKickStressSeq=%u refused (native fill+Doing disabled)",
                     c.teleportKickStressSeq);
}

void ApplyTeleportKickStressFineSeq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressFineSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_fine_seq.txt");
    if (c.teleportKickStressFineSeq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_fine_seq.txt", c.teleportKickStressFineSeq);
    x::runtime::LogW("Control",
                     "teleportKickStressFineSeq=%u refused (native fill+Doing disabled)",
                     c.teleportKickStressFineSeq);
}

void ApplyTeleportKickStressFine10Seq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressFine10Seq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_fine10_seq.txt");
    if (c.teleportKickStressFine10Seq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_fine10_seq.txt", c.teleportKickStressFine10Seq);
    x::runtime::LogW("Control",
                     "teleportKickStressFine10Seq=%u refused (native fill+Doing disabled)",
                     c.teleportKickStressFine10Seq);
}

void ApplyTeleportKickStressLocalSeq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressLocalSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_local_seq.txt");
    if (c.teleportKickStressLocalSeq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_local_seq.txt", c.teleportKickStressLocalSeq);
    x::runtime::LogW("Control",
                     "teleportKickStressLocalSeq=%u refused (native fill+Doing disabled)",
                     c.teleportKickStressLocalSeq);
}

void ApplyImpactNockBackTestSeq(const xcat::PayloadControl& c) {
    if (c.impactNockBackTestSeq == 0) return;
    const uint32_t diskLast = ReadLieSeqStamp("last_impact_nockback_seq.txt");
    // 同 manualRejoin：首拍无条件收养会吞掉「注入后第一次点 A」（UI 已 toast 但 DLL 不开火）。
    static const uint64_t s_moduleStartMs = GetTickCount64();
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    auto fire = [&](uint32_t seq) {
        s_lastApplied.store(seq, std::memory_order_release);
        WriteLieSeqStamp("last_impact_nockback_seq.txt", seq);
        const int dir = xcat::ClampImpactImpulseDir(c.impactImpulseDir);
        const int vx = static_cast<int>(xcat::ClampImpactImpulseSpeed(c.impactImpulseVx));
        const int vy = static_cast<int>(xcat::ClampImpactImpulseSpeed(c.impactImpulseVy));
        x::runtime::LogI("PayloadControl", "impact A fire seq=%u dir=%d vx=%d vy=%d", seq, dir, vx,
                         vy);
        (void)x::features::ports::teleport::FireImpactNockBackTest(dir, vx, vy);
    };
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        const bool newerThanStamp = c.impactNockBackTestSeq > diskLast;
        const bool writtenAfterInject =
            c.writeTickMs != 0 && c.writeTickMs >= s_moduleStartMs;
        if (newerThanStamp && writtenAfterInject) {
            fire(c.impactNockBackTestSeq);
            return;
        }
        const uint32_t adopt =
            c.impactNockBackTestSeq > diskLast ? c.impactNockBackTestSeq : diskLast;
        s_lastApplied.store(adopt, std::memory_order_release);
        WriteLieSeqStamp("last_impact_nockback_seq.txt", adopt);
        x::runtime::LogI("PayloadControl", "impact A bootstrap adopt seq=%u (no fire)", adopt);
        return;
    }
    const uint32_t last = s_lastApplied.load(std::memory_order_acquire);
    const uint32_t gate = last > diskLast ? last : diskLast;
    if (c.impactNockBackTestSeq <= gate) return;
    fire(c.impactNockBackTestSeq);
}

void ApplyImpactSetNextTestSeq(const xcat::PayloadControl& c) {
    if (c.impactSetNextTestSeq == 0) return;
    const uint32_t diskLast = ReadLieSeqStamp("last_impact_setnext_seq.txt");
    static const uint64_t s_moduleStartMs = GetTickCount64();
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    auto fire = [&](uint32_t seq) {
        s_lastApplied.store(seq, std::memory_order_release);
        WriteLieSeqStamp("last_impact_setnext_seq.txt", seq);
        const int dir = xcat::ClampImpactImpulseDir(c.impactImpulseDir);
        const double vx = static_cast<double>(xcat::ClampImpactImpulseSpeed(c.impactImpulseVx));
        const double vy = static_cast<double>(xcat::ClampImpactImpulseSpeed(c.impactImpulseVy));
        const double svx = (dir == 0) ? vx : (static_cast<double>(dir) * vx);
        x::runtime::LogI("PayloadControl", "impact B fire seq=%u svx=%.0f vy=%.0f", seq, svx, vy);
        (void)x::features::ports::teleport::FireImpactSetNextTest(svx, vy);
    };
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        const bool newerThanStamp = c.impactSetNextTestSeq > diskLast;
        const bool writtenAfterInject =
            c.writeTickMs != 0 && c.writeTickMs >= s_moduleStartMs;
        if (newerThanStamp && writtenAfterInject) {
            fire(c.impactSetNextTestSeq);
            return;
        }
        const uint32_t adopt =
            c.impactSetNextTestSeq > diskLast ? c.impactSetNextTestSeq : diskLast;
        s_lastApplied.store(adopt, std::memory_order_release);
        WriteLieSeqStamp("last_impact_setnext_seq.txt", adopt);
        x::runtime::LogI("PayloadControl", "impact B bootstrap adopt seq=%u (no fire)", adopt);
        return;
    }
    const uint32_t last = s_lastApplied.load(std::memory_order_acquire);
    const uint32_t gate = last > diskLast ? last : diskLast;
    if (c.impactSetNextTestSeq <= gate) return;
    fire(c.impactSetNextTestSeq);
}

void ApplyImpactHopTestSeq(const xcat::PayloadControl& c) {
    if (c.impactHopTestSeq == 0) return;
    const uint32_t diskLast = ReadLieSeqStamp("last_impact_hop_seq.txt");
    static const uint64_t s_moduleStartMs = GetTickCount64();
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    auto fire = [&](uint32_t seq) {
        s_lastApplied.store(seq, std::memory_order_release);
        WriteLieSeqStamp("last_impact_hop_seq.txt", seq);
        x::features::ports::teleport::ImpactHopOpts opts{};
        opts.force = c.impactHopForce != 0;
        const int dx = static_cast<int>(xcat::ClampImpactHopDeltaX(c.impactHopDeltaX));
        x::runtime::LogI("PayloadControl", "impact hop fire seq=%u dx=%d force=%u", seq, dx,
                         (unsigned)c.impactHopForce);
        (void)x::features::ports::teleport::ImpactHopDeltaX(dx, opts);
    };
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        const bool newerThanStamp = c.impactHopTestSeq > diskLast;
        const bool writtenAfterInject =
            c.writeTickMs != 0 && c.writeTickMs >= s_moduleStartMs;
        if (newerThanStamp && writtenAfterInject) {
            fire(c.impactHopTestSeq);
            return;
        }
        const uint32_t adopt = c.impactHopTestSeq > diskLast ? c.impactHopTestSeq : diskLast;
        s_lastApplied.store(adopt, std::memory_order_release);
        WriteLieSeqStamp("last_impact_hop_seq.txt", adopt);
        x::runtime::LogI("PayloadControl", "impact hop bootstrap adopt seq=%u (no fire)", adopt);
        return;
    }
    const uint32_t last = s_lastApplied.load(std::memory_order_acquire);
    const uint32_t gate = last > diskLast ? last : diskLast;
    if (c.impactHopTestSeq <= gate) return;
    fire(c.impactHopTestSeq);
}

void ApplyUiCheatOverlaySeq(const xcat::PayloadControl& c) {
    if (c.uiCheatOverlaySeq == 0) return;
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    // 一次性脉冲。进图前 ini 里残留的 seq（BIN：seq=8 停在 21:29，21:46 重注入
    // 因 writeTickMs 被其它偏好写刷新，误判 writtenAfterInject → 默认开了 GM 台）。
    // 首拍只收养；真要点再按实验 TAB 按钮，seq+1 才会 fire。
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        s_lastApplied.store(c.uiCheatOverlaySeq, std::memory_order_release);
        x::runtime::LogI("PayloadControl",
                         "ui cheat overlay bootstrap adopt seq=%u (no fire) writeTick=%llu "
                         "modStart=%llu",
                         c.uiCheatOverlaySeq, (unsigned long long)c.writeTickMs,
                         (unsigned long long)kManualRejoinModuleStartMs);
        return;
    }
    if (c.uiCheatOverlaySeq <= s_lastApplied.load(std::memory_order_acquire)) return;
    s_lastApplied.store(c.uiCheatOverlaySeq, std::memory_order_release);
    x::runtime::LogI("PayloadControl", "ui cheat overlay fire seq=%u", c.uiCheatOverlaySeq);
    x::features::ui_cheat_overlay::RequestOpen();
}

void ApplyAuctionGateProbeSeq(const xcat::PayloadControl& c) {
    if (!xcat::kAuctionGateProbeUserEnabled) return;
    if (c.auctionGateProbeSeq == 0) return;
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        const bool writtenAfterInject =
            c.writeTickMs != 0 &&
            c.writeTickMs + kManualRejoinWriteSkewMs >= kManualRejoinModuleStartMs;
        s_lastApplied.store(c.auctionGateProbeSeq, std::memory_order_release);
        if (writtenAfterInject) {
            x::runtime::LogI("PayloadControl", "auction gate probe fire seq=%u (bootstrap)",
                             c.auctionGateProbeSeq);
            x::features::auction_gate_probe::RequestRun();
            return;
        }
        x::runtime::LogI("PayloadControl",
                         "auction gate probe bootstrap adopt seq=%u (no fire)",
                         c.auctionGateProbeSeq);
        return;
    }
    if (c.auctionGateProbeSeq <= s_lastApplied.load(std::memory_order_acquire)) return;
    s_lastApplied.store(c.auctionGateProbeSeq, std::memory_order_release);
    x::runtime::LogI("PayloadControl", "auction gate probe fire seq=%u", c.auctionGateProbeSeq);
    x::features::auction_gate_probe::RequestRun();
}

void ApplyEncounterFromControl(const xcat::PayloadControl& c) {
    // 遇人检测不得跟 play-boot 推迟战斗绑在一起：进图后别人出现必须立刻能停手。
    xcat::PayloadControl forced = c;
    xcat::ApplyMobGatherEncounterForce(forced);
    xcat::ApplyAttackNoCdEncounterForce(forced);
    x::features::encounter::SetEnabled(forced.mobGather != 0 || forced.autoRelogin != 0);
    x::features::encounter::SetStrategies(forced.autoReloginStopCombat != 0,
                                          forced.mobGather != 0 || forced.autoReloginReconnect != 0,
                                          forced.autoReloginGmEscalate != 0,
                                          forced.mobGather != 0 || forced.autoReloginStopGather != 0);
}

void ApplyControl(const xcat::PayloadControl& c) {
    // 登录阶段只灌 auto_enter：BIN 11:47 在 LOGIN worker 里 Poll→Apply 打开了
    // invuln/加速/打怪/Travel MI，随后进程闪退。play-ready 前禁止玩法开关。
    x::features::auto_enter::SetDesired(c.autoEnter != 0, c.worldId, c.worldName, c.charSlot);
    // 软重连必须在 play-ready 前武装：选频/选角 Session 闪断时 KickSniff 会 RequestAttempt；
    // 若仍 idle，守护把 disconnectSeq 当硬踢干净杀（upload caa553：soft Init idle，
    // 进图后才 UI enable；用户首页软重连实际已开）。银河 token 与软重连同开关，一并提前。
    x::features::galaxy_token_probe::SetEnabled(c.galaxyTokenProbe != 0);
    x::features::soft_login_probe::SetEnabled(c.softLoginProbe != 0);
    ApplySoftLoginDismissSeq(c);
    if (x::runtime::managed_main::IsLoginFrozen() ||
        !x::features::ports::world::IsPlayReady()) {
        static bool sLoggedDefer = false;
        if (!sLoggedDefer) {
            sLoggedDefer = true;
            x::runtime::LogI("PayloadControl",
                             "defer play features until play-ready (login-freeze=%d play=%d; "
                             "softLogin armed early=%d)",
                             x::runtime::managed_main::IsLoginFrozen() ? 1 : 0,
                             x::features::ports::world::IsPlayReady() ? 1 : 0,
                             c.softLoginProbe != 0 ? 1 : 0);
        }
        // 不 latch gHaveApplied：进图后需完整 Apply 一次。
        return;
    }

    // 落地空窗会挨打：无敌在 play-ready 后立刻灌。遇人检测同样立刻灌，不得等 ForceApply。
    // 战斗/飞/攻速仍等到 ForceApply，避免一进图就跟冷绑抢泵。
    x::features::invuln::SetDesired(c.invuln != 0);
    ApplyEncounterFromControl(c);
    if (!gPlayBootApplyReady.load()) {
        static bool sLoggedBootDefer = false;
        if (!sLoggedBootDefer) {
            sLoggedBootDefer = true;
            x::runtime::LogI("PayloadControl",
                             "defer combat/fly until play-boot ForceApply "
                             "(invuln+encounter applied)");
        }
        return;
    }
    const bool attackAccelOn =
        xcat::kAttackAccelUserEnabled && c.attackAccel != 0;
    const bool clearBusyOn = c.attackAccelClearBusy != 0;
    // 首页 attackAccel（暂关）或「攻击无CD」/attackAccelClearBusy → worker 写 ActionBusy=-1。
    x::features::attack_accel::SetDesired(attackAccelOn || clearBusyOn);
    x::features::attack_accel::SetBoosterDesired(
        xcat::kAttackAccelBoosterUserEnabled && c.attackAccelBooster != 0);
    x::features::attack_accel::SetActionSpeedDesired(c.attackAccelActionSpeed != 0);
    x::features::attack_accel::SetPartyBoosterValue(c.attackAccelPartyBoosterValue);
    x::features::attack_accel::SetPartyBoosterDesired(c.attackAccelPartyBooster != 0);
    x::features::attack_accel::SetBreakDegreeFloorLo(c.attackAccelBreakDegreeFloorLo);
    x::features::attack_accel::SetBreakDegreeFloorDesired(c.attackAccelBreakDegreeFloor != 0);
    x::features::attack_accel::SetCutLayerDesired(c.attackAccelCutLayer != 0);
    x::features::attack_accel::SetSkipPrepareDesired(c.attackAccelSkipPrepare != 0);
    x::features::final_attack_force::SetDesired(xcat::kFinalAttackForceUserEnabled &&
                                                c.finalAttackForce != 0);
    x::features::skill_max_level::SetDesired(xcat::kSkillMaxLevelUserEnabled &&
                                             c.skillMaxLevel != 0);
    // 下列副作用只跟首页 attackAccel 包；「攻击无CD」只写 ActionBusy。
    x::features::ports::attack::SetAnimBusyOverrideMs(attackAccelOn ? 0 : -1);
    x::features::ports::attack::SetImmediateUp(attackAccelOn);
    x::features::fly::SetMode(c.flyMode);
    x::features::fly::SetHopCdMs(c.flyHopCdMs);
    x::features::fly::SetSpeedPct(c.flySpeedPct);
    x::features::fly::SetArmed(c.fly != 0);
    x::features::autopot::SetHpEnabled(c.hpPotion != 0);
    x::features::autopot::SetMpEnabled(c.mpPotion != 0);
    x::features::autopot::SetHpThresholdPct(static_cast<int>(c.hpThresholdPct));
    x::features::autopot::SetMpThresholdPct(static_cast<int>(c.mpThresholdPct));
    x::features::pet_feed::SetDesired(xcat::kPetSummonUserEnabled && c.petSummon != 0);
    x::features::pet_feed::SetRequireFood(xcat::kPetSummonUserEnabled &&
                                          c.petSummonRequireFood != 0);
    x::features::multi_skill::SetConfig(c.multiSkill != 0, c.multiSkillGapMs,
                                        c.multiSkillSafeStagger != 0);
    x::features::multi_skill::SetSendUseRequest(c.multiSkillSendUseRequest != 0);
    x::features::simple_combat::SetAttackIntervalMs(
        xcat::EffectiveAttackIntervalForApply(c.simpleCombatAttackIntervalMs,
                                              attackAccelOn ? 1u : 0u, clearBusyOn ? 1u : 0u,
                                              c.attackAccelClearBusyMinIntervalMs));
    x::features::simple_combat::SetTickIntervalMs(
        xcat::ClampSimpleCombatTickMs(c.simpleCombatTickMs ? c.simpleCombatTickMs
                                                           : xcat::kSimpleCombatTickDefaultMs));
    x::features::mob_scan::SetCombatIntervalMs(
        xcat::ClampMobScanIntervalMs(c.mobScanIntervalMs ? c.mobScanIntervalMs
                                                         : xcat::kMobScanIntervalDefaultMs));
    x::features::mob_pool_observe::SetEnabled(c.mobPoolObserve != 0);
    x::features::ports::attack::SetAttackHoldMs(
        xcat::ClampAttackHoldMs(c.simpleCombatAttackHoldMs ? c.simpleCombatAttackHoldMs
                                                          : xcat::kAttackHoldDefaultMs));
    x::features::simple_combat::SetSmartInterval(c.simpleCombatSmartInterval != 0);
    x::features::simple_combat::SetClusterPriority(c.clusterWeight != 0);
    x::features::simple_combat::SetHitRotateN(c.simpleCombatHitRotateN);
    x::features::simple_combat::SetSkipAccMissN(c.simpleCombatSkipAccMissN);
    x::features::simple_combat::SetSkipAccMissEnabled(c.simpleCombatSkipAccMiss != 0);
    x::features::ports::map_attack::SetEnabled(c.mapAttack != 0);
    // 吸怪只认首页勾；站桩输出不代开。
    x::features::ports::mob_gather::SetEnabled(c.mobGather != 0);
    x::features::ports::mob_fh_ban::SetStrategy(c.mobGatherStrategy);
    x::features::ports::mob_fh_ban::SetLandOnArrive(c.mobGatherLandOnArrive != 0);
    x::features::ports::mob_fh_ban::SetHopPx(static_cast<float>(c.mobGatherHopPx));
    x::features::ports::mob_gather::SetSpeedPct(c.mobGatherSpeedPct);
    x::features::ports::mob_gather::SetAntiJitter(c.mobGatherAntiJitter != 0);
    x::features::ports::mob_gather::SetMaxHold(c.mobGatherMax);
    x::features::ports::mob_gather::SetFarInFlight(c.mobGatherFarInFlight);
    x::features::ports::mob_gather::SetRadiusPx(c.mobGatherRadiusPx);
    x::features::ports::mob_gather::SetLayerYPx(c.mobGatherLayerYPx);
    x::features::ports::mob_gather::SetDyLimPx(c.mobGatherDyLimPx);
    x::features::ports::mob_gather::SetWalkDx(c.mobGatherWalkDx);
    x::features::ports::mob_gather::SetFeetExemptPx(c.mobGatherFeetExemptPx);
    x::features::ports::mob_gather::SetHoldTimeoutMs(c.mobGatherHoldMs);
    x::features::ports::mob_gather::SetRecruitIntervalMs(c.mobGatherIntervalMs);
    x::features::ports::mob_gather::SetAimIntervalMs(c.mobGatherAimMs);
    x::features::ports::mob_gather::SetIgnoreQuiet(c.mobGatherIgnoreQuiet != 0);
    x::features::ports::mob_gather::SetQuietDelayMs(c.mobGatherQuietDelayMs);
    x::features::ports::mob_gather::SetApplyCtrl(c.mobGatherApplyCtrl != 0);
    x::features::ports::mob_gather::SetFirstGenOnly(c.mobGatherFirstGenOnly != 0);
    x::features::ports::mob_gather::SetSoftRelogin(c.mobGatherSoftRelogin != 0,
                                                  c.mobGatherSoftReloginSec);
    x::features::ports::mob_gather::SetHangupUnbindF5(c.mobGatherHangupUnbindF5 != 0);
    x::features::ports::mob_gather::SetHangupFires(c.mobGatherHangupFiresOn != 0,
                                                   c.mobGatherHangupFires);
    x::features::ports::mob_gather::SetHangupFiresUiUnlocked(c.gatherTabUnlocked != 0);
    x::features::ports::mob_gather::SetClearRelogin(c.mobGatherClearRelogin != 0);
    x::features::ports::mob_gather::SetSeekCluster(c.mobGatherSeekCluster != 0);
    x::features::ports::mob_gather::SetPatrolFar(c.mobGatherPatrolFar != 0);
    // 防举报已证伪：忽略 INI，硬关并还原可能残留的 GA .text 脏页。吸怪其余旋钮不受影响。
    (void)c.mobGatherAntiReport;
    x::features::ports::mob_prevpos_patch::SetEnabled(false);
    x::features::ports::mob_gather::SetHomeReturn(c.mobGatherHomeReturn != 0);
    x::features::channel_hop::SetReconnectHopEnabled(c.mobGatherReconnectHop != 0);
    x::features::ports::mob_gather::SetHomePos(
        c.mobGatherHomeX, c.mobGatherHomeY, c.mobGatherHomeMapId, c.mobGatherHomeValid != 0,
        c.mobGatherHomeHasMap != 0);
    x::features::ports::mob_fh_ban::SetActuatorParams(
        static_cast<float>(c.mobGatherKp),
        static_cast<float>(c.mobGatherDead),
        static_cast<float>(c.mobGatherCruiseR),
        static_cast<float>(c.mobGatherStationR),
        static_cast<float>(c.mobGatherMaxCmd),
        static_cast<float>(c.mobGatherGravity),
        static_cast<float>(c.mobGatherStickCreepPx),
        static_cast<float>(c.mobGatherStickStillV));
    x::features::ports::mob_fh_ban::SetMotionTiers(
        static_cast<float>(c.mobGatherCruiseV),
        static_cast<float>(c.mobGatherStationV),
        static_cast<float>(c.mobGatherHoldV));
    x::features::ports::mob_fh_ban::SetSettleTune(
        static_cast<float>(c.mobGatherSettleErr),
        static_cast<float>(c.mobGatherKpSettle),
        static_cast<float>(c.mobGatherBrakeMs),
        static_cast<float>(c.mobGatherCoastVy));
    if (c.mapAttack != 0) {
        // P1 互斥：组包 / 攻包探针绕过 FindHit；打中换怪会把名单滤成一只。
        x::features::simple_combat::SetHitRotateEnabled(false);
        x::features::simple_combat::SetForgeHitEnabled(false);
    } else {
        x::features::simple_combat::SetHitRotateEnabled(c.simpleCombatHitRotate != 0);
        x::features::simple_combat::SetForgeHitEnabled(c.simpleCombatForgeHit != 0);
    }
    x::features::simple_combat::SetTeleportEnabled(c.simpleCombatTeleport != 0);
    x::features::simple_combat::SetTeleportOneHit(c.simpleCombatTeleportOneHit != 0);
    x::features::simple_combat::SetImpactApproachEnabled(c.simpleCombatImpactApproach != 0);
    x::features::simple_combat::SetAntiJitterEnabled(c.simpleCombatAntiJitter != 0);
    x::features::simple_combat::SetAntiHugEnabled(c.simpleCombatAntiHug != 0);
    x::features::melee_veto::SetEnabled(c.meleeVeto != 0);
    x::features::simple_combat::SetFlySpeedPct(c.simpleCombatFlySpeedPct);
    x::features::simple_combat::SetHumanWalkEnabled(c.simpleCombatHumanWalk != 0);
    x::features::simple_combat::SetHiraishinEnabled(c.simpleCombatHiraishin != 0);
    x::features::simple_combat::SetHiraishinLootHoldMs(c.simpleCombatHiraishinLootHoldMs);
    x::features::simple_combat::SetHiraishinRangePx(c.simpleCombatHiraishinRangePx);
    x::features::simple_combat::SetHiraishinFrontBox(c.simpleCombatHiraishinFrontDx,
                                                    c.simpleCombatHiraishinFrontDy);
    x::features::simple_combat::SetForgeHitFrontBox(c.simpleCombatForgeHitFrontDx,
                                                   c.simpleCombatForgeHitFrontDy);
    x::features::simple_combat::SetLiveStepEnabled(c.simpleCombatLiveStep != 0);
    x::features::simple_combat::SetTeleportParams(
        c.simpleCombatTeleportMinDx, c.simpleCombatTeleportStandOff, c.simpleCombatTeleportCooldownMs,
        c.simpleCombatTeleportMaxHop ? c.simpleCombatTeleportMaxHop
                                    : xcat::kCombatTeleportMaxHopDefault);
    x::features::simple_combat::SetStandOffParams(
        c.simpleCombatStandOffCustom != 0,
        xcat::ClampCombatStandOffX(c.simpleCombatStandOffX),
        xcat::ClampCombatStandOffY(c.simpleCombatStandOffY));
    x::features::ports::mob_fh_ban::SetGatherStandOff(
        c.mobGatherStandOffCustom != 0,
        xcat::ClampMobGatherStandOffX(c.mobGatherStandOffX),
        xcat::ClampMobGatherStandOffY(c.mobGatherStandOffY));
    x::features::ports::mob_fh_ban::SetAimJitterPx(
        static_cast<float>(xcat::ClampMobGatherAimJitter(c.mobGatherAimJitterPx)));
    x::features::ports::mob_fh_ban::SetDispClamp(
        c.mobGatherDispClampOn != 0,
        static_cast<float>(xcat::ClampMobGatherDispCapPx(c.mobGatherDispCapPx)));
    x::features::ports::ground_spoof::SetEnabled(c.simpleCombatGroundSpoof != 0);
    x::features::simple_combat::SetOneshotParams(c.simpleCombatOneshotMaxHp,
                                                 c.simpleCombatOneshotMinBumps,
                                                 c.simpleCombatOneshotMinFires,
                                                 c.simpleCombatOneshotMinLagMs,
                                                 c.simpleCombatOneshotFoxFillGapMs);
    // 配置齐了再开 F5。BIN 17:05:29 ForceApply 先 SetEnabled（human=1/standOff=12）
    // 再灌 10× / 拟人关 / 站位，worker 可能吃到一拍默认值。
    x::features::simple_combat::SetEnabled(c.simpleCombat != 0);
    x::runtime::main_thread::SetCongestionThreshold(
        static_cast<int>(xcat::ClampPumpCongestion(c.pumpCongestionThreshold)));
    x::runtime::main_thread::SetDrainBudget(
        static_cast<int>(xcat::ClampPumpDrainBudget(c.pumpDrainBudget)));
    x::features::ports::travel::SetPortalAimLiftY(
        xcat::ClampTravelPortalAimLiftY(
            c.travelPortalAimLiftY ? c.travelPortalAimLiftY
                                   : xcat::kTravelPortalAimLiftDefault));
    x::features::ports::attack_rpc::SetMaxMobs(static_cast<int>(c.attackRpcMobs));
    x::features::ports::attack_rpc::SetIntervalMs(c.attackRpcIntervalMs);
    x::features::ports::attack_rpc::SetDamage(static_cast<int>(c.attackRpcDamage));
    x::features::ports::attack_rpc::SetEnabled(c.mapAttack == 0 && c.attackRpc != 0);
    x::features::ports::curfh_gate_bypass::SetEnabled(c.curFhGateBypass != 0);
    x::features::auto_lie::SetEnabled(c.autoLie != 0);
    x::features::auto_lie::SetDryRun(c.autoLieDryRun != 0);
    x::features::movepath_flush_probe::SetEnabled(c.movepathFlushProbe != 0);
    x::features::galaxy_token_probe::SetEnabled(c.galaxyTokenProbe != 0);
    x::features::soft_login_probe::SetEnabled(c.softLoginProbe != 0);
    x::features::drop_alert_bypass::SetEnabled(c.dropAlertBypass != 0);
    x::features::force_trade::SetEnabled(xcat::kForceTradeUserEnabled && c.forceTrade != 0);
    x::features::auction_town_bypass::SetEnabled(c.auctionTownBypass != 0);
    if (xcat::kAuctionGateProbeUserEnabled)
        ApplyAuctionGateProbeSeq(c);
    ApplyUiCheatOverlaySeq(c);
    x::features::rest_mp_accel::SetIntervalMs(c.restMpAccelIntervalMs);
    x::features::rest_mp_accel::SetEnabled(c.restMpAccel != 0);
    if (xcat::kInfiniteStarsUserEnabled)
        x::features::infinite_stars::SetEnabled(c.infiniteStars != 0);
    // 自动补给真源：user.ini [auto_supply]（HotReadConfig）；勿再用 core.autoSell 灌开关。
    ApplyEncounterFromControl(c);
    x::features::player_hide::SetEnabled(c.hideOtherPlayers != 0);
    x::features::frame_lock::SetTargetFps(c.frameLockFps);
    x::features::frame_lock::SetEnabled(c.frameLock != 0);
    ApplyAutoLieAlarmTestSeq(c);
    ApplyAutoLieMouseSmokeSeq(c);
    ApplyAutoLieMouseRegionOverlay(c);
    ApplyAutoLieMouseSimSeq(c);
    ApplyManualRejoinSeq(c);
    ApplyTeleportTestSeq(c);
    ApplyTeleportNativeTestSeq(c);
    ApplyTeleportKickStressSeq(c);
    ApplyTeleportKickStressFineSeq(c);
    ApplyTeleportKickStressFine10Seq(c);
    ApplyTeleportKickStressLocalSeq(c);
    ApplyImpactNockBackTestSeq(c);
    ApplyImpactSetNextTestSeq(c);
    ApplyImpactHopTestSeq(c);
    ApplySoftLoginDismissSeq(c);
    // 回城/中止改由 [auto_supply] manualSeq+manualKind 驱动，不再读 core.autoSell*Seq。
    gLastAppliedTick.store(c.writeTickMs);
    gHaveApplied.store(true);
}

bool ReadMergePublish(bool setCombat, bool combatOn, bool setFly, bool flyOn) {
    const std::string bin = PayloadBinDir();
    if (bin.empty()) return false;
    xcat::PayloadControl c{};
    (void)xcat::ReadPayloadControl(bin.c_str(), c);
    if (setCombat) c.simpleCombat = combatOn ? 1u : 0u;
    if (setFly) c.fly = flyOn ? 1u : 0u;
    c.writeTickMs = GetTickCount64();
    if (!xcat::WritePayloadControl(bin.c_str(), c)) return false;
    // 必须全量 Apply：否则 F5/F6 只改单一开关却把 writeTick 标成已应用，
    // 后续 Poll 会跳过，贴怪瞬移等其它 core 字段永远进不了 DLL。
    ApplyControl(c);
    return true;
}

}  // namespace

void PayloadControl_ForceApply() {
    const std::string bin = PayloadBinDir();
    if (bin.empty()) return;
    xcat::PayloadControl c{};
    if (!xcat::ReadPayloadControl(bin.c_str(), c)) return;
    gPlayBootApplyReady.store(true);
    gHaveApplied.store(false);
    x::runtime::LogI("PayloadControl",
                     "force-apply after play-boot (rebind; Init must not leave desired=0)");
    ApplyControl(c);
}

void PayloadControl_Poll() {
    const std::string bin = PayloadBinDir();
    if (bin.empty()) return;

    // 启动器杀进程失败时的兜底：见本机粘性拒绝则结束游戏进程（DLL 所在进程）。
    {
        static ULONGLONG s_lastStickyCheckMs = 0;
        const ULONGLONG now = GetTickCount64();
        if (s_lastStickyCheckMs == 0 || now - s_lastStickyCheckMs >= 2000) {
            s_lastStickyCheckMs = now;
            if (xcat::AccessDenyStickyPresent(bin.c_str())) {
                x::runtime::LogW("AccessGate", "sticky deny → ExitProcess(2)");
                ExitProcess(2);
            }
        }
    }

    xcat::PayloadControl c{};
    if (!xcat::ReadPayloadControl(bin.c_str(), c)) return;
    if (gHaveApplied.load() && c.writeTickMs == gLastAppliedTick.load()) return;
    ApplyControl(c);
}

void PayloadControl_PublishSimpleCombat(bool on) {
    // ApplyControl 在 ReadMergePublish 内完成（含 teleport 等其它 core 字段）
    (void)ReadMergePublish(true, on, false, false);
}

void PayloadControl_PublishFly(bool on) {
    (void)ReadMergePublish(false, false, true, on);
}

void PayloadControl_PublishHomePos(int32_t x, int32_t y, int32_t mapId, bool hasMap) {
    const std::string bin = PayloadBinDir();
    if (bin.empty()) return;
    xcat::PayloadControl c{};
    (void)xcat::ReadPayloadControl(bin.c_str(), c);
    c.mobGatherHomeX = xcat::ClampMobGatherStandOffX(x);
    c.mobGatherHomeY = xcat::ClampMobGatherStandOffY(y);
    c.mobGatherHomeMapId = mapId;
    c.mobGatherHomeValid = 1u;
    c.mobGatherHomeHasMap = hasMap ? 1u : 0u;
    c.writeTickMs = GetTickCount64();
    if (!xcat::WritePayloadControl(bin.c_str(), c)) return;
    ApplyControl(c);
}

}  // namespace x::ipc
