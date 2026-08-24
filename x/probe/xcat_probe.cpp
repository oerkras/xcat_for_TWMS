// TWMS Classic injected payload (xcat.dll).
// LoadLibraryW target for injector; hosts feature workers.
//
// Cold-start gate: DllMain must NOT StartWorker while Canvas/SceneLogin MI is
// missing — early workers touching managed heap → Unity GC fatal
// ("Collecting from unknown thread"). Bootstrap waits (no hard timeout) for
// MainPump; DETACH coordinates via stop + phase so mid-start aborts StopWorker.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <cstdio>

#include "../features/auto_enter/auto_enter.h"
#include "../features/autopot/autopot.h"
#include "../features/auto_stat/auto_stat.h"
#include "../features/auto_skill/auto_skill.h"
#include "../features/invuln/invuln.h"
#include "../features/attack_accel/attack_accel.h"
#include "../features/final_attack_force/final_attack_force.h"
#include "../features/melee_veto/melee_veto.h"
#include "../features/ports/hit_pin_port.h"
#include "../features/ports/hit_geom_port.h"
#include "../features/skill_max_level/skill_max_level.h"
#include "../features/kick_sniff/kick_sniff.h"
#include "../features/mob_scan/mob_scan.h"
#include "../features/mob_pool_observe/mob_pool_observe.h"
#include "../features/pet_feed/pet_feed.h"
#include "../features/pet_loot/pet_loot.h"
#include "../features/buffs/buffs.h"
#include "../features/ccu/ccu.h"
#include "../features/multi_skill/multi_skill.h"
#include "../features/simple_combat/simple_combat.h"
#include "../features/map_attack/map_attack.h"
#include "../features/mob_gather/mob_gather.h"
#include "../features/ports/keypad_walk_bin.h"
#include "../features/ports/unity_kbd_port.h"
#include "../features/ports/key_macro_bin.h"
#include "../features/ports/curfh_gate_bypass.h"
#include "../features/ports/mob_prevpos_patch.h"
#include "../features/ports/mob_inspect_probe.h"
#include "../features/auto_lie/auto_lie.h"
#include "../features/drop_alert_bypass/drop_alert_bypass.h"
#include "../features/force_trade/force_trade.h"
#include "../features/auction_town_bypass/auction_town_bypass.h"
#include "../features/auction_gate_probe/auction_gate_probe.h"
#include "../features/ui_cheat_overlay/ui_cheat_overlay.h"
#include "../features/rest_mp_accel/rest_mp_accel.h"
#include "../features/ports/security_attack_port.h"
#include "../features/infinite_stars/infinite_stars.h"
#include "../features/ga_text_probe/ga_text_probe.h"
#include "../features/galaxy_token_probe/galaxy_token_probe.h"
#include "../features/soft_login_probe/soft_login_probe.h"
#include "../features/channel_hop/channel_hop.h"
#include "../features/encounter/encounter.h"
#include "../features/player_hide/player_hide.h"
#include "../features/frame_lock/frame_lock.h"
#include "../features/fly/fly.h"
#include "../features/timed_keys/timed_keys.h"
#include "../features/titlebar/titlebar.h"
#include "../features/titlebar/titlebar_game.h"
#include "../features/titlebar/titlebar_win.h"
#include "../features/travel/travel.h"
#include "../features/worldmap_marker_travel/worldmap_marker_travel.h"
#include "../features/sellbag/sellbag.h"
#include "../features/attack_rpc/attack_rpc.h"
#include "../features/auto_supply/auto_supply.h"
#include "../features/char_boot/char_boot.h"
#include "../ipc/payload_buffs.h"
#include "../ipc/payload_control.h"
#include "../ipc/payload_pet_loot.h"
#include "../ipc/payload_auto_stat.h"
#include "../ipc/payload_auto_skill.h"
#include "../ipc/payload_status.h"
#include "../ipc/payload_timed_keys.h"
#include "xcat_payload_control.h"
#include "../features/crash_upload_guard/crash_upload_guard.h"
#include "../runtime/bin_dir.h"
#include "../runtime/hang_autopsy.h"
#include "../runtime/il2cpp_bind.h"
#include "../runtime/il2cpp_fault_probe.h"
#include "../runtime/il2cpp_network.h"
#include "../runtime/main_thread_pump.h"
#include "../runtime/log.h"
#include "../runtime/managed_main.h"
#include "../features/ports/world_port.h"
#include "../x_version.h"

#include "xcat_sound.h"

namespace {

HANDLE gReadyEvent = nullptr;
HANDLE gBootstrapThread = nullptr;
std::atomic_bool gBootstrapStop{false};
// Ensure/FindClass 进行中：DETACH 短等后再 Shutdown，避免边拆边查。
std::atomic_bool gInManagedProbe{false};

// Idle → Waiting → Starting → Running；DETACH 置 Stopping。
enum class Phase : int { Idle = 0, Waiting = 1, Starting = 2, Running = 3, Stopping = 4 };
std::atomic<int> gPhase{static_cast<int>(Phase::Idle)};

constexpr DWORD kColdStartPollMs = 1000;   // MainPump Ensure 最多 1Hz，降低 FindClass 压力
constexpr DWORD kColdStartLogMs = 5000;
// Native-only：GA 未进进程前不调 MainPump Ensure（避免无意义 FindClass）。
constexpr DWORD kGaPollMs = 200;
// 证据 fe04a3：workers 未开，但 Ensure→FindClass 冷域仍 GC fatal。
// 注入后至少这段时间只做原生等待（Unity 窗 / Sleep），禁止 domain/FindClass。
constexpr DWORD kPreEnsureNativeMinMs = 15000;
constexpr DWORD kUnityWndPollMs = 400;
// DllMain 内短等 Bootstrap 离开 Ensure（不可 join）。
constexpr DWORD kDetachProbeIdleMs = 500;
// 证据 5e3768：MI 已装但 SendWill 未跳就开 worker → pump idle / process-dead。
// age 上限必须 ≥ 持续窗口，否则低帧会饿死门闩（review）。
constexpr DWORD kPumpAliveMinMs = 2000;
constexpr DWORD kPumpTickAgeMaxMs = 3000;  // ≥ kPumpAliveMinMs
constexpr uint32_t kPumpAliveMinTicks = 10;
constexpr DWORD kPumpAlivePollMs = 200;
constexpr DWORD kPlayReadyPollMs = 400;
// play-ready 瞬间常仍 freeze=1（c9b8dc 第 2 局）；冻屏未散就开 PLAY → drain/job timeout。
// 要求连续未冻满此时长再放行。
constexpr DWORD kPlayUnfreezeStableMs = 300;

struct ManagedProbeGuard {
    ManagedProbeGuard() { gInManagedProbe.store(true, std::memory_order_release); }
    ~ManagedProbeGuard() { gInManagedProbe.store(false, std::memory_order_release); }
};

void WaitManagedProbeIdle(DWORD maxMs) {
    const DWORD t0 = GetTickCount();
    while (gInManagedProbe.load(std::memory_order_acquire)) {
        if (GetTickCount() - t0 >= maxMs) break;
        Sleep(1);
    }
}

void MarkReady() {
    wchar_t name[64]{};
    swprintf_s(name, L"Local\\XCatTwmsProbeReady_%lu", GetCurrentProcessId());
    gReadyEvent = CreateEventW(nullptr, TRUE, TRUE, name);
}

void UnmarkReady() {
    if (gReadyEvent) {
        CloseHandle(gReadyEvent);
        gReadyEvent = nullptr;
    }
}

bool AbortRequested() { return gBootstrapStop.load(std::memory_order_acquire); }

void StopAllFeatureWorkers() {
    x::features::encounter::StopWorker();
    x::features::player_hide::StopWorker();
    x::features::frame_lock::StopWorker();
    x::features::channel_hop::StopWorker();
    x::features::ga_text_probe::StopWorker();
    x::features::drop_alert_bypass::StopWorker();
    x::features::force_trade::Shutdown();
    x::features::auction_town_bypass::StopWorker();
    x::features::auction_gate_probe::Shutdown();
    x::features::ui_cheat_overlay::Shutdown();
    x::features::rest_mp_accel::StopWorker();
    x::features::ports::security_attack::StopWorker();
    x::features::ports::mob_inspect_probe::StopWorker();
    x::features::auto_lie::StopWorker();
    xcat::sound::CancelPlayback();
    x::features::fly::StopWorker();
    x::features::simple_combat::StopWorker();
    x::features::ports::keypad_walk_bin::Shutdown();
    x::features::ports::key_macro_bin::Shutdown();
    // 摘掉 Keyboard.PreProcessEvent 的 vtable 钩，否则卸载后游戏会调进已释放内存。
    x::features::ports::unity_kbd::Shutdown();
    x::features::char_boot::StopWorker();
    x::features::auto_supply::StopWorker();
    x::features::sellbag::StopWorker();
    x::features::attack_rpc::StopWorker();
    x::features::mob_gather::StopWorker();
    x::features::worldmap_marker_travel::Shutdown();
    x::features::travel::StopWorker();
    x::features::multi_skill::StopWorker();
    x::features::buffs::StopWorker();
    x::features::timed_keys::StopWorker();
    x::features::pet_loot::StopWorker();
    x::features::pet_feed::StopWorker();
    x::features::autopot::StopWorker();
    x::features::auto_stat::StopWorker();
    x::features::auto_skill::StopWorker();
    x::features::mob_pool_observe::StopWorker();
    x::features::mob_scan::StopWorker();
    x::features::titlebar::StopWorker();
    x::ipc::PayloadStatus_Stop();
    x::features::ccu::StopWorker();
    x::features::auto_enter::StopWorker();
    x::features::skill_max_level::StopWorker();
    x::features::final_attack_force::StopWorker();
    // Shutdown 而非只 StopWorker：这两个功能在 GameAssembly 函数头上写了 abs-jmp，
    // 不摘掉的话卸载后游戏会跳进已释放内存。
    x::features::infinite_stars::Shutdown();
    x::features::melee_veto::Shutdown();
    x::features::map_attack::Shutdown();
    x::features::ports::hit_pin::Shutdown();
    x::features::ports::hit_geom::Shutdown();
    // 同样改了 GameAssembly .text（三处判空跳转）。它只在 GA 内部翻分支、不跳进 xcat.dll，
    // 所以不会像上面那样卸载后跳进已释放内存；但不摘掉的话补丁会一直留在游戏里 ——
    // 卸载后既关不掉也擦不掉，脏页还留给完整性校验。
    x::features::ports::curfh_gate_bypass::SetEnabled(false);
    x::features::ports::mob_prevpos_patch::SetEnabled(false);
    x::features::ports::mob_inspect_probe::Shutdown();
    x::features::attack_accel::StopWorker();
    x::features::invuln::StopWorker();
    x::features::kick_sniff::StopWorker();
    x::features::soft_login_probe::StopWorker();
    x::features::galaxy_token_probe::Shutdown();
}

// 每步可打断：DETACH 置 stop 后尽快 StopAll，避免半开 workers。
#define XCAT_BOOT_STEP(stmt)         \
    do {                             \
        if (AbortRequested()) {      \
            StopAllFeatureWorkers(); \
            return false;            \
        }                            \
        stmt;                        \
    } while (0)

// play-ready 后齐开会把 MainPump 队列打满（实机：进场秒 131 LogI/s + job timeout → 卡顿）。
// 每步等泵排空再让出；批次之间再拉长。XCAT_PLAY_BOOT_STAGGER=0 可关回同步齐开。
// c72cff：固定 500ms settle 起步仍 q=3 → settle 内 job timeout；改为等 q=0（带上下限）。
constexpr DWORD kPlayBootSettleMinMs = 300;   // 至少让出切图帧
constexpr DWORD kPlayBootSettleMaxMs = 2500;  // 等 q=0 上限；到期仍堵则 cap 后继续
constexpr DWORD kPlayBootStepGapMs = 80;
constexpr DWORD kPlayBootBatchGapMs = 250;
constexpr DWORD kPlayBootDrainMaxMs = 2000;  // ≥ InvokeAndWait 默认 1500，避免半排空就下一步
constexpr DWORD kPlayBootDrainPollMs = 20;

bool EnvPlayBootStaggerOff() {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_PLAY_BOOT_STAGGER", buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return false;
    return buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' || buf[0] == 'f' || buf[0] == 'F';
}

// unfreeze 后等泵空闲再动第一批 survival（削 settle 窗 job timeout）。
bool WaitPlayBootSettleIdle() {
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    const int q0 = x::runtime::main_thread::QueuedJobCount();
    x::runtime::LogI("Bootstrap",
                     "play-boot settle wait q=0 (min=%ums max=%ums, q0=%d congested=%d)",
                     (unsigned)kPlayBootSettleMinMs, (unsigned)kPlayBootSettleMaxMs, q0,
                     x::runtime::main_thread::IsCongested() ? 1 : 0);
    const DWORD t0 = GetTickCount();
    DWORD lastLog = 0;
    while (!AbortRequested()) {
        const int q = x::runtime::main_thread::QueuedJobCount();
        const bool congested = x::runtime::main_thread::IsCongested();
        const DWORD elapsed = GetTickCount() - t0;
        if (elapsed >= kPlayBootSettleMinMs && q <= 0 && !congested) {
            x::runtime::LogI("Bootstrap", "play-boot settle idle after %ums (q=0)",
                             (unsigned)elapsed);
            return true;
        }
        if (elapsed >= kPlayBootSettleMaxMs) {
            x::runtime::LogI("Bootstrap",
                             "play-boot settle cap %ums q=%d congested=%d — proceed",
                             (unsigned)elapsed, q, congested ? 1 : 0);
            break;
        }
        if (lastLog == 0 || elapsed - lastLog >= 500) {
            lastLog = elapsed;
            x::runtime::LogI("Bootstrap", "play-boot settle… %ums q=%d congested=%d",
                             (unsigned)elapsed, q, congested ? 1 : 0);
        }
        Sleep(kPlayBootDrainPollMs);
    }
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    return true;
}

bool YieldAfterPlayBootStep(bool batchBoundary) {
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    if (EnvPlayBootStaggerOff()) return true;
    const DWORD t0 = GetTickCount();
    while (!AbortRequested()) {
        const int q = x::runtime::main_thread::QueuedJobCount();
        const bool congested = x::runtime::main_thread::IsCongested();
        if (q <= 0 && !congested) break;
        if (GetTickCount() - t0 >= kPlayBootDrainMaxMs) {
            x::runtime::LogI("Bootstrap", "play-boot drain cap q=%d congested=%d", q,
                             congested ? 1 : 0);
            break;
        }
        Sleep(kPlayBootDrainPollMs);
    }
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    Sleep(batchBoundary ? kPlayBootBatchGapMs : kPlayBootStepGapMs);
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    return true;
}

#define XCAT_PLAY_BOOT_STEP(stmt)                            \
    do {                                                     \
        if (AbortRequested()) {                              \
            StopAllFeatureWorkers();                         \
            return false;                                    \
        }                                                    \
        stmt;                                                \
        if (!YieldAfterPlayBootStep(false)) return false;    \
    } while (0)

#define XCAT_PLAY_BOOT_BATCH(tag)                                                 \
    do {                                                                          \
        if (!YieldAfterPlayBootStep(true)) return false;                          \
        x::runtime::LogI("Bootstrap", "play-boot batch %s (q=%d)", (tag),         \
                         x::runtime::main_thread::QueuedJobCount());              \
    } while (0)

// 登录期：只开过图/会话必需 + 纯磁盘预载。
// 禁止在此 FindClass/skill·input 冷绑——login-freeze=1 时会堵泵，分区 UI 进不去（da2b90）。
bool StartLoginPathWorkers() {
    if (AbortRequested()) return false;
    x::runtime::LogI("Bootstrap",
                     "MainPump alive — start LOGIN workers (session + disk preload only)");
    // BIN 10:11：KickSniff worker 冷 Ensure/FindClass → GC Fatal。泵上预热后再开 worker。
    XCAT_BOOT_STEP(x::runtime::il2cpp_network::WarmForLoginWorkers());
    XCAT_BOOT_STEP(x::features::kick_sniff::Init());
    XCAT_BOOT_STEP(x::features::kick_sniff::StartWorker());
    XCAT_BOOT_STEP(x::features::galaxy_token_probe::Init());
    XCAT_BOOT_STEP(x::features::soft_login_probe::Init());
    XCAT_BOOT_STEP(x::features::soft_login_probe::StartWorker());
    XCAT_BOOT_STEP(x::features::auto_enter::Init());
    XCAT_BOOT_STEP(x::features::auto_enter::StartWorker());
    XCAT_BOOT_STEP(x::features::ccu::Init());
    XCAT_BOOT_STEP(x::features::ccu::StartWorker());
    XCAT_BOOT_STEP(x::ipc::PayloadStatus_Start());
    // BIN 11:56 LOADING：Titlebar worker BindApis→FindClass(ItemData*) → GC Fatal。
    XCAT_BOOT_STEP(x::features::titlebar::game::WarmForLoginWorkers());
    XCAT_BOOT_STEP(x::features::titlebar::Init());
    XCAT_BOOT_STEP(x::features::titlebar::StartWorker());

    // 纯磁盘：不碰托管堆 / FindClass
    x::runtime::LogI("Bootstrap", "LOGIN disk preload (graph + sellbag catalog + cfg)");
    XCAT_BOOT_STEP(x::features::travel::PreloadGraph());
    XCAT_BOOT_STEP(x::features::sellbag::Init());
    XCAT_BOOT_STEP(x::ipc::PayloadBuffs_ApplyInitial());
    XCAT_BOOT_STEP(x::ipc::PayloadTimedKeys_ApplyInitial());

    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    return true;
}

// play-ready + unfreeze + settle idle 之后：FindClass 冷绑定（分片排空）。
// 勿在 LOGIN / settle 前调用——冻屏期堵分区（da2b90），settle 前齐开则进场尖峰回流。
bool StartPostSettleColdInits() {
    if (AbortRequested()) return false;
    x::runtime::LogI("Bootstrap",
                     "post-settle cold init (skill/input/consumable/travel_port/…)");
    XCAT_PLAY_BOOT_STEP(x::features::buffs::Init());
    XCAT_PLAY_BOOT_STEP(x::features::multi_skill::Init());
    XCAT_PLAY_BOOT_STEP(x::features::timed_keys::Init());
    XCAT_PLAY_BOOT_STEP(x::features::autopot::Init());
    XCAT_PLAY_BOOT_STEP(x::features::travel::Init());
    XCAT_PLAY_BOOT_STEP(x::features::worldmap_marker_travel::Init());
    XCAT_PLAY_BOOT_STEP(x::features::frame_lock::Init());
    XCAT_PLAY_BOOT_STEP(x::features::frame_lock::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::channel_hop::Init());
    XCAT_PLAY_BOOT_STEP(x::features::channel_hop::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::auto_lie::Init());
    XCAT_PLAY_BOOT_STEP(x::features::auto_lie::StartWorker());
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    return true;
}

// 进图后：settle → FindClass 冷绑 → survival StartWorker…
// LOGIN 仅磁盘预载；FindClass 禁在冻屏期（da2b90）。
bool StartPlayPathWorkers() {
    if (AbortRequested()) return false;
    const bool stagger = !EnvPlayBootStaggerOff();
    x::runtime::LogI("Bootstrap",
                     "play-ready — start PLAY workers %s (invuln first → settle → cold init → rest)",
                     stagger ? "staggered" : "sync(XCAT_PLAY_BOOT_STAGGER=0)");

    // 保命优先：无敌先于 settle/冷绑。落地空窗等 cold init 会挨打（用户反馈首次启动）。
    // Invuln MyUser 急钉不依赖 FindClass 冷绑。
    XCAT_PLAY_BOOT_STEP(x::features::invuln::Init());
    XCAT_PLAY_BOOT_STEP(x::features::invuln::StartWorker());

    // 切图刚落地：再等泵空，FindClass，开其余 survival。
    if (stagger) {
        if (!WaitPlayBootSettleIdle()) return false;
    }
    if (!StartPostSettleColdInits()) return false;

    // 遇人检测不得等 ForceApply / 战斗灌配置。冷绑后立刻开 worker：
    // 进图后别人出现必须能停手；若已 Pause，随后 combat Init 会 keep HardPause。
    XCAT_PLAY_BOOT_STEP(x::features::encounter::Init());
    XCAT_PLAY_BOOT_STEP(x::features::encounter::StartWorker());

    // 1) 其余保命：掉落报警 → 药/键/Buff 线程 → 攻速
    XCAT_PLAY_BOOT_STEP(x::features::drop_alert_bypass::Init());
    XCAT_PLAY_BOOT_STEP(x::features::drop_alert_bypass::StartWorker());
    if (xcat::kForceTradeUserEnabled) {
        XCAT_PLAY_BOOT_STEP(x::features::force_trade::Init());
    }
    // 入口关闭时只标灯 disabled，不 Init、不开线程。
    XCAT_PLAY_BOOT_STEP(x::features::force_trade::StartWorker());
    XCAT_PLAY_BOOT_BATCH("survival-skills");
    XCAT_PLAY_BOOT_STEP(x::features::autopot::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::auto_stat::Init());
    XCAT_PLAY_BOOT_STEP(x::ipc::PayloadAutoStat_ApplyInitial());
    XCAT_PLAY_BOOT_STEP(x::features::auto_stat::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::auto_skill::Init());
    XCAT_PLAY_BOOT_STEP(x::ipc::PayloadAutoSkill_ApplyInitial());
    XCAT_PLAY_BOOT_STEP(x::features::auto_skill::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::timed_keys::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::buffs::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::multi_skill::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::attack_accel::Init());
    XCAT_PLAY_BOOT_STEP(x::features::attack_accel::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::final_attack_force::Init());
    XCAT_PLAY_BOOT_STEP(x::features::final_attack_force::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::melee_veto::Init());
    XCAT_PLAY_BOOT_STEP(x::features::melee_veto::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::ports::hit_pin::Init());
    XCAT_PLAY_BOOT_STEP(x::features::ports::hit_pin::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::ports::hit_geom::Init());
    XCAT_PLAY_BOOT_STEP(x::features::map_attack::Init());
    XCAT_PLAY_BOOT_STEP(x::features::skill_max_level::Init());
    XCAT_PLAY_BOOT_STEP(x::features::skill_max_level::StartWorker());

    // 2) 扫描 / 宠物 / 赶路卖物（Init 已提前的只 StartWorker）
    XCAT_PLAY_BOOT_BATCH("scan+pets+travel");
    XCAT_PLAY_BOOT_STEP(x::features::mob_scan::Init());
    XCAT_PLAY_BOOT_STEP(x::features::mob_scan::StartWorker());
    // 刷怪感知：依赖 mob_scan::RequestImmediateScan；默认关，由 core.mobPoolObserve 武装
    XCAT_PLAY_BOOT_STEP(x::features::mob_pool_observe::Init());
    XCAT_PLAY_BOOT_STEP(x::features::mob_pool_observe::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::mob_gather::Init());
    XCAT_PLAY_BOOT_STEP(x::features::mob_gather::StartWorker());
    if (xcat::kPetSummonUserEnabled) {
        XCAT_PLAY_BOOT_STEP(x::features::pet_feed::Init());
    }
    // 入口关闭时不 Init、不开线程（StartWorker 内再闸一次）。
    XCAT_PLAY_BOOT_STEP(x::features::pet_feed::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::pet_loot::Init());
    XCAT_PLAY_BOOT_STEP(x::ipc::PayloadPetLoot_ApplyInitial());
    XCAT_PLAY_BOOT_STEP(x::features::pet_loot::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::travel::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::sellbag::StartWorker());

    // 3) 战斗与其余（teleport EnsureBound 改懒绑，Worker 入口不再扫方法表）
    XCAT_PLAY_BOOT_BATCH("combat+misc");
    XCAT_PLAY_BOOT_STEP(x::features::attack_rpc::Init());
    XCAT_PLAY_BOOT_STEP(x::features::attack_rpc::StartWorker());
    // 战斗必须先 Init：AutoSupply worker 一起来就会 PauseSystems。
    // BIN 14:50:54 旧序 = 补给先硬闸 → combat Init 把 mask 清 0 → ForceApply 再开 F5
    // → 续跑回图被 Travel combat_on 每 200ms 刷「请先关闭 F5」。
    XCAT_PLAY_BOOT_STEP(x::features::simple_combat::Init());
    XCAT_PLAY_BOOT_STEP(x::features::simple_combat::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::auto_supply::Init());
    XCAT_PLAY_BOOT_STEP(x::features::auto_supply::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::char_boot::Init());
    XCAT_PLAY_BOOT_STEP(x::features::char_boot::StartWorker());
    // 走路只读采证（默认开；XCAT_WALK_BIN=0 关）。请关 F5/拟人后手按左右。
    XCAT_PLAY_BOOT_STEP(x::features::ports::keypad_walk_bin::Init());
    // KeyMacroAnalyzer Put/句柄 BIN（默认开；XCAT_KEYMACRO_BIN=0 关）→ key_macro_bin.log
    XCAT_PLAY_BOOT_STEP(x::features::ports::key_macro_bin::Init());
    XCAT_PLAY_BOOT_STEP(x::features::fly::Init());
    XCAT_PLAY_BOOT_STEP(x::features::fly::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::auction_town_bypass::Init());
    XCAT_PLAY_BOOT_STEP(x::features::auction_town_bypass::StartWorker());
    if (xcat::kAuctionGateProbeUserEnabled) {
        XCAT_PLAY_BOOT_STEP(x::features::auction_gate_probe::Init());
    }
    XCAT_PLAY_BOOT_STEP(x::features::ui_cheat_overlay::Init());
    XCAT_PLAY_BOOT_STEP(x::features::rest_mp_accel::Init());
    XCAT_PLAY_BOOT_STEP(x::features::rest_mp_accel::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::ports::security_attack::Init());
    XCAT_PLAY_BOOT_STEP(x::features::ports::security_attack::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::ports::mob_inspect_probe::Init());
    XCAT_PLAY_BOOT_STEP(x::features::ports::mob_inspect_probe::StartWorker());
    if (xcat::kInfiniteStarsUserEnabled) {
        XCAT_PLAY_BOOT_STEP(x::features::infinite_stars::Init());
    }
    // 入口关闭时只标灯 disabled，不 Init、不开线程。
    XCAT_PLAY_BOOT_STEP(x::features::infinite_stars::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::ga_text_probe::Init());
    XCAT_PLAY_BOOT_STEP(x::features::ga_text_probe::StartWorker());
    XCAT_PLAY_BOOT_STEP(x::features::player_hide::Init());
    XCAT_PLAY_BOOT_STEP(x::features::player_hide::StartWorker());
    // play-boot 完成前 Poll 只灌无敌 + 遇人。Init 会清若干 desired；writeTick 不变则 Poll 跳过。
    // 此处才允许完整 Apply（战斗/飞/攻速/FhBan），避免一进图就跟冷绑抢泵。
    XCAT_PLAY_BOOT_STEP(x::ipc::PayloadControl_ForceApply());
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    return true;
}

#undef XCAT_BOOT_STEP
#undef XCAT_PLAY_BOOT_STEP
#undef XCAT_PLAY_BOOT_BATCH

// 自首次见到真实 tick 起墙钟满 minMs，且当前仍在跳动、累计 ticks 够 —— 不要求无间隙连续 streak。
bool WaitPumpTicking() {
    const uint32_t tick0 = x::runtime::main_thread::RealTickCount();
    DWORD firstTickWall = 0;
    DWORD lastLog = 0;
    x::runtime::LogI("Bootstrap",
                     "wait MainPump real ticks (≥%ums wall + %u ticks, age≤%ums) before login workers",
                     static_cast<unsigned>(kPumpAliveMinMs),
                     static_cast<unsigned>(kPumpAliveMinTicks),
                     static_cast<unsigned>(kPumpTickAgeMaxMs));
    while (!AbortRequested()) {
        if (!x::runtime::main_thread::IsInstalled()) return false;
        // 补挂 SceneLogin（Canvas 已装但过图停跳时的心跳兜底）。
        {
            ManagedProbeGuard probe;
            if (AbortRequested()) return false;
            (void)x::runtime::main_thread::Ensure();
        }
        const uint32_t ticks = x::runtime::main_thread::RealTickCount() - tick0;
        const bool live = x::runtime::main_thread::IsPumpTicking(kPumpTickAgeMaxMs);
        const DWORD now = GetTickCount();
        if (ticks > 0 && firstTickWall == 0) firstTickWall = now;
        if (live && firstTickWall != 0 && ticks >= kPumpAliveMinTicks &&
            (now - firstTickWall) >= kPumpAliveMinMs) {
            x::runtime::LogI("Bootstrap",
                             "MainPump alive (wall=%ums, ticks=%u) — login workers next",
                             static_cast<unsigned>(now - firstTickWall),
                             static_cast<unsigned>(ticks));
            return true;
        }
        if (lastLog == 0 || now - lastLog >= kColdStartLogMs) {
            lastLog = now;
            const DWORD age = x::runtime::main_thread::LastRealTickAgeMs();
            x::runtime::LogI("Bootstrap", "MainPump tick wait… ticks=%u ageMs=%u live=%d",
                             static_cast<unsigned>(ticks),
                             age == 0xFFFFFFFFu ? 0u : static_cast<unsigned>(age), live ? 1 : 0);
        }
        Sleep(kPumpAlivePollMs);
    }
    return false;
}

// 进图后再开玩法 workers：要 map+WM ready，且 login-freeze 已散（再稳一小段）。
bool WaitPlayReadyForWorkers() {
    DWORD lastLog = 0;
    DWORD unfreezeSince = 0;
    x::runtime::LogI("Bootstrap",
                     "wait play-ready + unfreeze (≥%ums) before PLAY workers",
                     static_cast<unsigned>(kPlayUnfreezeStableMs));
    while (!AbortRequested()) {
        const bool ready = x::features::ports::world::IsPlayReady();
        const bool frozen = x::runtime::managed_main::IsLoginFrozen();
        const DWORD now = GetTickCount();
        if (ready && !frozen) {
            if (!unfreezeSince) unfreezeSince = now;
            if (now - unfreezeSince >= kPlayUnfreezeStableMs) {
                x::runtime::LogI("Bootstrap",
                                 "play-ready + unfreeze ok (stable=%ums freeze=0) — PLAY next",
                                 static_cast<unsigned>(now - unfreezeSince));
                return true;
            }
        } else {
            unfreezeSince = 0;
        }
        if (lastLog == 0 || now - lastLog >= kColdStartLogMs) {
            lastLog = now;
            x::runtime::LogI("Bootstrap",
                             "play-ready wait… ready=%d freeze=%d unfreezeMs=%u pumpLive=%d scene=%d",
                             ready ? 1 : 0, frozen ? 1 : 0,
                             unfreezeSince ? static_cast<unsigned>(now - unfreezeSince) : 0u,
                             x::runtime::main_thread::IsPumpTicking(kPumpTickAgeMaxMs) ? 1 : 0,
                             static_cast<int>(x::features::ports::world::GetSceneState()));
        }
        Sleep(kPlayReadyPollMs);
    }
    return false;
}

bool BootFeatureWorkersTwoPhase() {
    if (!StartLoginPathWorkers()) return false;
    if (!WaitPlayReadyForWorkers()) {
        StopAllFeatureWorkers();
        return false;
    }
    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    // FindClass 冷绑在 StartPlayPathWorkers 内：settle idle 之后再分片跑（防进场尖峰）。
    return StartPlayPathWorkers();
}

bool WaitNativeGameAssembly() {
    while (!AbortRequested()) {
        if (GetModuleHandleW(L"GameAssembly.dll")) {
            // 仅解析导出（GetProcAddress / RVA），不碰托管堆。
            if (x::runtime::il2cpp::Ensure()) return true;
        }
        Sleep(kGaPollMs);
    }
    return false;
}

// 纯原生：等 UnityWndClass 出现，且自进入本阶段起满 kPreEnsureNativeMinMs。
// 期间绝不调用 domainGet / class_from_name（fe04a3 GC 根因）。
// 无窗则一直等（不超时放行 FindClass）；仅 detach 可中止。
bool WaitNativeBeforeFindClass() {
    const DWORD start = GetTickCount();
    DWORD lastLog = 0;
    bool sawUnity = false;
    bool snappedTopLeft = false;
    x::runtime::LogI("Bootstrap",
                     "native settle ≥%us + UnityWndClass before FindClass (no timeout bypass)",
                     static_cast<unsigned>(kPreEnsureNativeMinMs / 1000u));
    while (!AbortRequested()) {
        const DWORD now = GetTickCount();
        const DWORD elapsed = now - start;
        HWND unity = x::features::titlebar::win::FindUnityWndClass();
        if (unity) {
            sawUnity = true;
            if (!snappedTopLeft) {
                snappedTopLeft = x::features::titlebar::win::PositionGameTopLeft(unity);
                if (snappedTopLeft) {
                    x::runtime::LogI("Bootstrap", "game window snapped top-left hwnd=%p",
                                     (void*)unity);
                }
            }
        }
        if (elapsed >= kPreEnsureNativeMinMs && unity) {
            x::runtime::LogI("Bootstrap",
                             "native settle done (%us, UnityWndClass=%p) — begin MainPump poll",
                             static_cast<unsigned>(elapsed / 1000u), (void*)unity);
            return true;
        }
        if (lastLog == 0 || now - lastLog >= kColdStartLogMs) {
            lastLog = now;
            x::runtime::LogI("Bootstrap", "native settle… %us UnityWndClass=%d",
                             static_cast<unsigned>(elapsed / 1000u), sawUnity ? 1 : 0);
        }
        Sleep(kUnityWndPollMs);
    }
    return false;
}

DWORD WINAPI BootstrapThread(LPVOID) {
    gPhase.store(static_cast<int>(Phase::Waiting), std::memory_order_release);
    x::runtime::LogI("Bootstrap",
                     "cold-start gate: GA → settle → MainPump MI → real ticks → "
                     "LOGIN workers → play-ready → PLAY workers (abort on detach)");

    // 先于任何 feature 起看门狗：卡死可能发生在任意阶段，而它自己会等主泵
    // 真正 tick 过之后才开始判死，不会误伤冷启动。
    x::runtime::hang_autopsy::Start();
    // 同理可赶在 feature 之前：崩溃上传会在主线程上同步走 WinINet，网络不通时能把
    // 客户端冻死（2026-08-09 04:45 实测）。默认关；XCAT_CRASH_UPLOAD_GUARD=1 才套 IAT 超时。
    x::features::crash_upload_guard::Start();
    // 也要抢在所有 feature 之前挂上：元数据锁泄漏的源头是 il2cpp 内部的访问违例被
    // __except 吞掉，只有首次异常阶段能看见它。
    x::runtime::il2cpp_fault_probe::Start();

    if (!WaitNativeGameAssembly()) {
        x::runtime::LogI("Bootstrap", "cold-start aborted before GA (detach)");
        gPhase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
        return 0;
    }
    x::runtime::LogI("Bootstrap", "GameAssembly exports ready — native settle next");

    if (!WaitNativeBeforeFindClass()) {
        x::runtime::LogI("Bootstrap", "cold-start aborted during native settle (detach)");
        gPhase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
        return 0;
    }

    const DWORD start = GetTickCount();
    DWORD lastLog = 0;
    while (!AbortRequested()) {
        // 窗丢了：必须重新走 ≥15s 原生 settle，禁止立刻 FindClass。
        if (!x::features::titlebar::win::FindUnityWndClass()) {
            x::runtime::LogI("Bootstrap", "UnityWndClass lost — re-enter native settle");
            if (!WaitNativeBeforeFindClass()) {
                break;
            }
            lastLog = 0;
            continue;
        }
        bool installed = false;
        {
            ManagedProbeGuard probe;
            if (AbortRequested()) break;
            // MainPump::Ensure 不做 class_init；仅在原生 settle + 现窗之后调用。
            installed =
                x::runtime::main_thread::Ensure() && x::runtime::main_thread::IsInstalled();
        }
        if (AbortRequested()) break;
        if (installed) {
            if (!WaitPumpTicking()) {
                x::runtime::LogI("Bootstrap", "cold-start aborted waiting pump ticks (detach)");
                break;
            }
            if (AbortRequested()) break;
            int expect = static_cast<int>(Phase::Waiting);
            if (!gPhase.compare_exchange_strong(expect, static_cast<int>(Phase::Starting),
                                                std::memory_order_acq_rel)) {
                x::runtime::LogI("Bootstrap", "cold-start aborted (phase=%d)", expect);
                return 0;
            }
            const bool ok = BootFeatureWorkersTwoPhase();
            if (!ok || AbortRequested()) {
                // DETACH 可能已 StopAll；再 sweep 一次幂等收尾。
                StopAllFeatureWorkers();
                gPhase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
                x::runtime::LogI("Bootstrap", "worker start aborted (detach)");
                return 0;
            }
            int expectStart = static_cast<int>(Phase::Starting);
            if (!gPhase.compare_exchange_strong(expectStart, static_cast<int>(Phase::Running),
                                               std::memory_order_acq_rel)) {
                // DETACH 已置 Stopping：补一次 StopAll。
                StopAllFeatureWorkers();
                x::runtime::LogI("Bootstrap", "worker start lost race to detach");
                return 0;
            }
            x::runtime::LogI("Bootstrap", "cold-start complete — login+play workers running");
            return 0;
        }
        const DWORD now = GetTickCount();
        if (lastLog == 0 || now - lastLog >= kColdStartLogMs) {
            lastLog = now;
            x::runtime::LogI("Bootstrap", "cold-start waiting MainPump… %us",
                             static_cast<unsigned>((now - start) / 1000u));
        }
        Sleep(kColdStartPollMs);
    }

    gPhase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
    x::runtime::LogI("Bootstrap", "cold-start aborted (detach)");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(h);
        x::runtime::SetImageModule(h);
        x::runtime::LogInit();
        MarkReady();
        gBootstrapStop.store(false, std::memory_order_release);
        gInManagedProbe.store(false, std::memory_order_release);
        gPhase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
        // 自报身份：注入路径曾被 Debug 静默覆盖；launcher.log / x.jsonl 一眼能看出跑的是哪份。
#if defined(_DEBUG)
        constexpr const char* kBuiltCfg = "Debug";
#else
        constexpr const char* kBuiltCfg = "Release";
#endif
        x::runtime::LogI("Bootstrap",
                         "attached — %s build=%u built=%s %s %s — defer feature workers "
                         "until MainPump (cold-start GC gate)",
                         x::kVersionString, static_cast<unsigned>(x::kBuildId), kBuiltCfg,
                         __DATE__, __TIME__);
        gBootstrapThread = CreateThread(nullptr, 0, &BootstrapThread, nullptr, 0, nullptr);
        if (!gBootstrapThread) {
            UnmarkReady();
            x::runtime::LogW("Bootstrap",
                             "CreateThread failed — refusing immediate StartWorker "
                             "(would risk GC fatal); probe-ready cleared");
        }
        break;
    }
    case DLL_PROCESS_DETACH: {
        // Signal only; never join (loader lock).
        gBootstrapStop.store(true, std::memory_order_release);
        x::runtime::hang_autopsy::Stop();
        x::features::crash_upload_guard::Stop();
        x::runtime::il2cpp_fault_probe::Stop();
        const int prev = gPhase.exchange(static_cast<int>(Phase::Stopping),
                                         std::memory_order_acq_rel);
        if (gBootstrapThread) {
            CloseHandle(gBootstrapThread);
            gBootstrapThread = nullptr;
        }
        // Starting / Running：停掉已开或半开的 workers（Stop* 幂等）。
        if (prev == static_cast<int>(Phase::Starting) ||
            prev == static_cast<int>(Phase::Running)) {
            StopAllFeatureWorkers();
        }
        // 短等 Bootstrap 离开 Ensure，再 Shutdown，避免边拆边 FindClass。
        WaitManagedProbeIdle(kDetachProbeIdleMs);
        if (gInManagedProbe.load(std::memory_order_acquire)) {
            x::runtime::LogW("Bootstrap",
                             "detach: Ensure still in-flight after %ums — skip Shutdown "
                             "(process likely exiting)",
                             static_cast<unsigned>(kDetachProbeIdleMs));
        } else {
            x::runtime::main_thread::Shutdown();
        }
        UnmarkReady();
        x::runtime::LogI("Bootstrap", "detached (prev_phase=%d)", prev);
        x::runtime::LogShutdown();
        break;
    }
    default:
        break;
    }
    return TRUE;
}
