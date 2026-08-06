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
#include "../features/ports/attack_input_port.h"
#include "../features/ports/world_port.h"
#include "../features/simple_combat/simple_combat.h"
#include "../features/mob_scan/mob_scan.h"
#include "../features/fly/fly.h"
#include "../features/drop_alert_bypass/drop_alert_bypass.h"
#include "../features/auction_town_bypass/auction_town_bypass.h"
#include "../features/player_hide/player_hide.h"
#include "../features/frame_lock/frame_lock.h"
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
    if (c.manualRejoinSeq == 0) return;
    const uint32_t diskLast = ReadLieSeqStamp("last_manual_rejoin_seq.txt");
    // 注入后首次 Apply：默认收养 stamp，禁止重放「进图前残留」F10（BIN reinject seq=10）。
    // 但若 ini seq > stamp 且 writeTickMs 落在本 DLL 加载之后，视为注入后新点击，必须放行
    // （BIN：App seq=1 已写盘，首拍无条件 return → 第一次点无反应）。
    static const uint64_t s_moduleStartMs = GetTickCount64();
    static std::atomic<bool> s_bootstrapped{false};
    static std::atomic<uint32_t> s_lastApplied{0};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        const bool newerThanStamp = c.manualRejoinSeq > diskLast;
        const bool writtenAfterInject =
            c.writeTickMs != 0 && c.writeTickMs >= s_moduleStartMs;
        if (newerThanStamp && writtenAfterInject) {
            s_lastApplied.store(c.manualRejoinSeq, std::memory_order_release);
            WriteLieSeqStamp("last_manual_rejoin_seq.txt", c.manualRejoinSeq);
            x::features::channel_hop::RequestManualRejoin(c.manualRejoinSeq);
            return;
        }
        const uint32_t adopt =
            c.manualRejoinSeq > diskLast ? c.manualRejoinSeq : diskLast;
        s_lastApplied.store(adopt, std::memory_order_release);
        if (adopt != diskLast) {
            WriteLieSeqStamp("last_manual_rejoin_seq.txt", adopt);
        }
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
        if (c.manualRejoinSeq <= prev) return;
        if (s_lastApplied.compare_exchange_weak(prev, c.manualRejoinSeq,
                                                std::memory_order_acq_rel)) {
            WriteLieSeqStamp("last_manual_rejoin_seq.txt", c.manualRejoinSeq);
            x::features::channel_hop::RequestManualRejoin(c.manualRejoinSeq);
            return;
        }
    }
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

void ApplyTeleportTestSeq(const xcat::PayloadControl& c) {
    if (c.teleportTestSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_test_seq.txt");
    static std::atomic<bool> s_bootstrapped{false};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        if (c.teleportTestSeq != last) {
            WriteLieSeqStamp("last_teleport_test_seq.txt", c.teleportTestSeq);
        }
        return;
    }
    if (c.teleportTestSeq <= last) return;
    WriteLieSeqStamp("last_teleport_test_seq.txt", c.teleportTestSeq);
    x::features::simple_combat::RequestTeleportToRandomMob();
}

void ApplyTeleportNativeTestSeq(const xcat::PayloadControl& c) {
    if (c.teleportNativeTestSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_native_test_seq.txt");
    static std::atomic<bool> s_bootstrapped{false};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        if (c.teleportNativeTestSeq != last) {
            WriteLieSeqStamp("last_teleport_native_test_seq.txt", c.teleportNativeTestSeq);
        }
        return;
    }
    if (c.teleportNativeTestSeq <= last) return;
    WriteLieSeqStamp("last_teleport_native_test_seq.txt", c.teleportNativeTestSeq);
    x::features::simple_combat::RequestNativeTeleportCall();
}

void ApplyTeleportKickStressSeq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_seq.txt");
    static std::atomic<bool> s_bootstrapped{false};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        if (c.teleportKickStressSeq != last) {
            WriteLieSeqStamp("last_teleport_kick_stress_seq.txt", c.teleportKickStressSeq);
        }
        return;
    }
    if (c.teleportKickStressSeq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_seq.txt", c.teleportKickStressSeq);
    x::features::simple_combat::RequestTeleportKickStress();
}

void ApplyTeleportKickStressFineSeq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressFineSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_fine_seq.txt");
    static std::atomic<bool> s_bootstrapped{false};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        if (c.teleportKickStressFineSeq != last) {
            WriteLieSeqStamp("last_teleport_kick_stress_fine_seq.txt", c.teleportKickStressFineSeq);
        }
        return;
    }
    if (c.teleportKickStressFineSeq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_fine_seq.txt", c.teleportKickStressFineSeq);
    x::features::simple_combat::RequestTeleportKickStressFine();
}

void ApplyTeleportKickStressFine10Seq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressFine10Seq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_fine10_seq.txt");
    static std::atomic<bool> s_bootstrapped{false};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        if (c.teleportKickStressFine10Seq != last) {
            WriteLieSeqStamp("last_teleport_kick_stress_fine10_seq.txt",
                             c.teleportKickStressFine10Seq);
        }
        return;
    }
    if (c.teleportKickStressFine10Seq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_fine10_seq.txt", c.teleportKickStressFine10Seq);
    x::features::simple_combat::RequestTeleportKickStressFine10();
}

void ApplyTeleportKickStressLocalSeq(const xcat::PayloadControl& c) {
    if (c.teleportKickStressLocalSeq == 0) return;
    const uint32_t last = ReadLieSeqStamp("last_teleport_kick_stress_local_seq.txt");
    static std::atomic<bool> s_bootstrapped{false};
    if (!s_bootstrapped.exchange(true, std::memory_order_acq_rel)) {
        if (c.teleportKickStressLocalSeq != last) {
            WriteLieSeqStamp("last_teleport_kick_stress_local_seq.txt",
                             c.teleportKickStressLocalSeq);
        }
        return;
    }
    if (c.teleportKickStressLocalSeq <= last) return;
    WriteLieSeqStamp("last_teleport_kick_stress_local_seq.txt", c.teleportKickStressLocalSeq);
    x::features::simple_combat::RequestTeleportKickStressLocal();
}

void ApplyControl(const xcat::PayloadControl& c) {
    // 登录阶段只灌 auto_enter：BIN 11:47 在 LOGIN worker 里 Poll→Apply 打开了
    // invuln/加速/打怪/Travel MI，随后进程闪退。play-ready 前禁止玩法开关。
    x::features::auto_enter::SetDesired(c.autoEnter != 0, c.worldId, c.worldName, c.charSlot);
    if (x::runtime::managed_main::IsLoginFrozen() ||
        !x::features::ports::world::IsPlayReady()) {
        static bool sLoggedDefer = false;
        if (!sLoggedDefer) {
            sLoggedDefer = true;
            x::runtime::LogI("PayloadControl",
                             "defer play features until play-ready (login-freeze=%d play=%d)",
                             x::runtime::managed_main::IsLoginFrozen() ? 1 : 0,
                             x::features::ports::world::IsPlayReady() ? 1 : 0);
        }
        // 不 latch gHaveApplied：进图后需完整 Apply 一次。
        return;
    }

    x::features::invuln::SetDesired(c.invuln != 0);
    x::features::attack_accel::SetDesired(c.attackAccel != 0);
    x::features::attack_accel::SetBoosterDesired(c.attackAccelBooster != 0);
    x::features::attack_accel::SetCutLayerDesired(c.attackAccelCutLayer != 0);
    x::features::attack_accel::SetSkipPrepareDesired(c.attackAccelSkipPrepare != 0);
    x::features::final_attack_force::SetDesired(c.finalAttackForce != 0);
    x::features::skill_max_level::SetDesired(c.skillMaxLevel != 0);
    // 攻击加速：清动作忙锁，换怪贴身不再被 animBusy(220) 拖住。
    x::features::ports::attack::SetAnimBusyOverrideMs(c.attackAccel ? 0 : -1);
    // 加速：Down+Up 同泵，消 pending 跨 tick（刀间隔只跟面板 interval）。
    x::features::ports::attack::SetImmediateUp(c.attackAccel != 0);
    x::features::fly::SetMode(c.flyMode);
    x::features::fly::SetHopCdMs(c.flyHopCdMs);
    x::features::fly::SetArmed(c.fly != 0);
    x::features::autopot::SetHpEnabled(c.hpPotion != 0);
    x::features::autopot::SetMpEnabled(c.mpPotion != 0);
    x::features::autopot::SetHpThresholdPct(static_cast<int>(c.hpThresholdPct));
    x::features::autopot::SetMpThresholdPct(static_cast<int>(c.mpThresholdPct));
    x::features::pet_feed::SetDesired(c.petSummon != 0);
    x::features::pet_feed::SetRequireFood(c.petSummonRequireFood != 0);
    x::features::multi_skill::SetConfig(c.multiSkill != 0, c.multiSkillGapMs,
                                        c.multiSkillSafeStagger != 0);
    x::features::multi_skill::SetSendUseRequest(c.multiSkillSendUseRequest != 0);
    x::features::simple_combat::SetEnabled(c.simpleCombat != 0);
    x::features::simple_combat::SetAttackIntervalMs(
        xcat::EffectiveSimpleCombatAttackIntervalMs(c.simpleCombatAttackIntervalMs, c.attackAccel));
    x::features::simple_combat::SetTickIntervalMs(
        xcat::ClampSimpleCombatTickMs(c.simpleCombatTickMs ? c.simpleCombatTickMs
                                                           : xcat::kSimpleCombatTickDefaultMs));
    x::features::mob_scan::SetCombatIntervalMs(
        xcat::ClampMobScanIntervalMs(c.mobScanIntervalMs ? c.mobScanIntervalMs
                                                         : xcat::kMobScanIntervalDefaultMs));
    x::features::ports::attack::SetAttackHoldMs(
        xcat::ClampAttackHoldMs(c.simpleCombatAttackHoldMs ? c.simpleCombatAttackHoldMs
                                                          : xcat::kAttackHoldDefaultMs));
    x::features::simple_combat::SetSmartInterval(c.simpleCombatSmartInterval != 0);
    x::features::simple_combat::SetClusterPriority(c.clusterWeight != 0);
    x::features::simple_combat::SetTeleportEnabled(c.simpleCombatTeleport != 0);
    x::features::simple_combat::SetLiveStepEnabled(c.simpleCombatLiveStep != 0);
    x::features::simple_combat::SetTeleportParams(
        c.simpleCombatTeleportMinDx, c.simpleCombatTeleportStandOff, c.simpleCombatTeleportCooldownMs,
        c.simpleCombatTeleportMaxHop ? c.simpleCombatTeleportMaxHop
                                    : xcat::kCombatTeleportMaxHopDefault,
        c.simpleCombatCrossLayerFillGateMs, c.simpleCombatFillBudgetPx);
    x::features::simple_combat::SetOneshotParams(c.simpleCombatOneshotMaxHp,
                                                 c.simpleCombatOneshotMinBumps,
                                                 c.simpleCombatOneshotMinFires,
                                                 c.simpleCombatOneshotMinLagMs,
                                                 c.simpleCombatOneshotFoxFillGapMs);
    x::runtime::main_thread::SetCongestionThreshold(
        static_cast<int>(xcat::ClampPumpCongestion(c.pumpCongestionThreshold)));
    x::runtime::main_thread::SetDrainBudget(
        static_cast<int>(xcat::ClampPumpDrainBudget(c.pumpDrainBudget)));
    x::features::ports::attack_rpc::SetMaxMobs(static_cast<int>(c.attackRpcMobs));
    x::features::ports::attack_rpc::SetIntervalMs(c.attackRpcIntervalMs);
    x::features::ports::attack_rpc::SetDamage(static_cast<int>(c.attackRpcDamage));
    x::features::ports::attack_rpc::SetEnabled(c.attackRpc != 0);
    x::features::auto_lie::SetEnabled(c.autoLie != 0);
    x::features::auto_lie::SetDryRun(c.autoLieDryRun != 0);
    x::features::drop_alert_bypass::SetEnabled(c.dropAlertBypass != 0);
    x::features::auction_town_bypass::SetEnabled(c.auctionTownBypass != 0);
    // 自动补给真源：user.ini [auto_supply]（HotReadConfig）；勿再用 core.autoSell 灌开关。
    x::features::encounter::SetEnabled(c.autoRelogin != 0);
    x::features::encounter::SetStrategies(c.autoReloginStopCombat != 0,
                                          c.autoReloginReconnect != 0);
    x::features::player_hide::SetEnabled(c.hideOtherPlayers != 0);
    x::features::frame_lock::SetTargetFps(c.frameLockFps);
    x::features::frame_lock::SetEnabled(c.frameLock != 0);
    ApplyAutoLieAlarmTestSeq(c);
    ApplyAutoLieMouseSmokeSeq(c);
    ApplyManualRejoinSeq(c);
    ApplyTeleportTestSeq(c);
    ApplyTeleportNativeTestSeq(c);
    ApplyTeleportKickStressSeq(c);
    ApplyTeleportKickStressFineSeq(c);
    ApplyTeleportKickStressFine10Seq(c);
    ApplyTeleportKickStressLocalSeq(c);
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

}  // namespace x::ipc
