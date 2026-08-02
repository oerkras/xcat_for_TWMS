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
#include "../features/invuln/invuln.h"
#include "../features/attack_accel/attack_accel.h"
#include "../features/kick_sniff/kick_sniff.h"
#include "../features/mob_scan/mob_scan.h"
#include "../features/pet_feed/pet_feed.h"
#include "../features/pet_loot/pet_loot.h"
#include "../features/buffs/buffs.h"
#include "../features/ccu/ccu.h"
#include "../features/multi_skill/multi_skill.h"
#include "../features/simple_combat/simple_combat.h"
#include "../features/auto_lie/auto_lie.h"
#include "../features/drop_alert_bypass/drop_alert_bypass.h"
#include "../features/ga_text_probe/ga_text_probe.h"
#include "../features/channel_hop/channel_hop.h"
#include "../features/encounter/encounter.h"
#include "../features/fly/fly.h"
#include "../features/timed_keys/timed_keys.h"
#include "../features/titlebar/titlebar.h"
#include "../features/titlebar/titlebar_win.h"
#include "../features/travel/travel.h"
#include "../features/worldmap_marker_travel/worldmap_marker_travel.h"
#include "../features/sellbag/sellbag.h"
#include "../features/attack_rpc/attack_rpc.h"
#include "../features/auto_supply/auto_supply.h"
#include "../ipc/payload_buffs.h"
#include "../ipc/payload_control.h"
#include "../ipc/payload_pet_loot.h"
#include "../ipc/payload_status.h"
#include "../ipc/payload_timed_keys.h"
#include "../runtime/bin_dir.h"
#include "../runtime/il2cpp_bind.h"
#include "../runtime/log.h"
#include "../runtime/main_thread_pump.h"

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
    x::features::channel_hop::StopWorker();
    x::features::ga_text_probe::StopWorker();
    x::features::drop_alert_bypass::StopWorker();
    x::features::auto_lie::StopWorker();
    xcat::sound::CancelPlayback();
    x::features::fly::StopWorker();
    x::features::simple_combat::StopWorker();
    x::features::auto_supply::StopWorker();
    x::features::sellbag::StopWorker();
    x::features::attack_rpc::StopWorker();
    x::features::worldmap_marker_travel::Shutdown();
    x::features::travel::StopWorker();
    x::features::multi_skill::StopWorker();
    x::features::buffs::StopWorker();
    x::features::timed_keys::StopWorker();
    x::features::pet_loot::StopWorker();
    x::features::pet_feed::StopWorker();
    x::features::autopot::StopWorker();
    x::features::mob_scan::StopWorker();
    x::features::titlebar::StopWorker();
    x::ipc::PayloadStatus_Stop();
    x::features::ccu::StopWorker();
    x::features::auto_enter::StopWorker();
    x::features::attack_accel::StopWorker();
    x::features::invuln::StopWorker();
    x::features::kick_sniff::StopWorker();
}

// 每步可打断：DETACH 置 stop 后尽快 StopAll，避免半开 workers。
bool StartAllFeatureWorkers() {
    if (AbortRequested()) return false;

    x::runtime::LogI("Bootstrap",
                     "MainPump ready — start workers (invuln + attack_accel + kick_sniff + "
                     "auto_enter + titlebar + mob_scan + autopot + pet_feed + pet_loot + "
                     "timed_keys + buffs + multi_skill + travel + sellbag + attack_rpc + "
                     "auto_supply + simple_combat + fly + auto_lie + drop_alert + "
                     "ga_text_probe + channel_hop + encounter + ccu + payload_status)");

#define XCAT_BOOT_STEP(stmt)       \
    do {                           \
        if (AbortRequested()) {    \
            StopAllFeatureWorkers(); \
            return false;          \
        }                          \
        stmt;                      \
    } while (0)

    XCAT_BOOT_STEP(x::features::kick_sniff::Init());
    XCAT_BOOT_STEP(x::features::kick_sniff::StartWorker());
    XCAT_BOOT_STEP(x::features::invuln::Init());
    XCAT_BOOT_STEP(x::features::invuln::StartWorker());
    XCAT_BOOT_STEP(x::features::attack_accel::Init());
    XCAT_BOOT_STEP(x::features::attack_accel::StartWorker());
    XCAT_BOOT_STEP(x::features::auto_enter::Init());
    XCAT_BOOT_STEP(x::features::auto_enter::StartWorker());
    XCAT_BOOT_STEP(x::features::ccu::Init());
    XCAT_BOOT_STEP(x::features::ccu::StartWorker());
    XCAT_BOOT_STEP(x::ipc::PayloadStatus_Start());
    XCAT_BOOT_STEP(x::features::titlebar::Init());
    XCAT_BOOT_STEP(x::features::titlebar::StartWorker());
    XCAT_BOOT_STEP(x::features::mob_scan::Init());
    XCAT_BOOT_STEP(x::features::mob_scan::StartWorker());
    XCAT_BOOT_STEP(x::features::autopot::Init());
    XCAT_BOOT_STEP(x::features::autopot::StartWorker());
    XCAT_BOOT_STEP(x::features::pet_feed::Init());
    XCAT_BOOT_STEP(x::features::pet_feed::StartWorker());
    XCAT_BOOT_STEP(x::features::pet_loot::Init());
    XCAT_BOOT_STEP(x::ipc::PayloadPetLoot_ApplyInitial());
    XCAT_BOOT_STEP(x::features::pet_loot::StartWorker());
    XCAT_BOOT_STEP(x::features::timed_keys::Init());
    XCAT_BOOT_STEP(x::ipc::PayloadTimedKeys_ApplyInitial());
    XCAT_BOOT_STEP(x::features::timed_keys::StartWorker());
    XCAT_BOOT_STEP(x::features::buffs::Init());
    XCAT_BOOT_STEP(x::ipc::PayloadBuffs_ApplyInitial());
    XCAT_BOOT_STEP(x::features::buffs::StartWorker());
    XCAT_BOOT_STEP(x::features::multi_skill::Init());
    XCAT_BOOT_STEP(x::features::multi_skill::StartWorker());
    XCAT_BOOT_STEP(x::features::travel::Init());
    XCAT_BOOT_STEP(x::features::travel::StartWorker());
    XCAT_BOOT_STEP(x::features::worldmap_marker_travel::Init());
    XCAT_BOOT_STEP(x::features::sellbag::Init());
    XCAT_BOOT_STEP(x::features::sellbag::StartWorker());
    XCAT_BOOT_STEP(x::features::attack_rpc::Init());
    XCAT_BOOT_STEP(x::features::attack_rpc::StartWorker());
    XCAT_BOOT_STEP(x::features::auto_supply::Init());
    XCAT_BOOT_STEP(x::features::auto_supply::StartWorker());
    XCAT_BOOT_STEP(x::features::simple_combat::Init());
    XCAT_BOOT_STEP(x::features::simple_combat::StartWorker());
    XCAT_BOOT_STEP(x::features::fly::Init());
    XCAT_BOOT_STEP(x::features::fly::StartWorker());
    XCAT_BOOT_STEP(x::features::auto_lie::Init());
    XCAT_BOOT_STEP(x::features::auto_lie::StartWorker());
    XCAT_BOOT_STEP(x::features::drop_alert_bypass::Init());
    XCAT_BOOT_STEP(x::features::drop_alert_bypass::StartWorker());
    XCAT_BOOT_STEP(x::features::ga_text_probe::Init());
    XCAT_BOOT_STEP(x::features::ga_text_probe::StartWorker());
    XCAT_BOOT_STEP(x::features::channel_hop::Init());
    XCAT_BOOT_STEP(x::features::channel_hop::StartWorker());
    XCAT_BOOT_STEP(x::features::encounter::Init());
    XCAT_BOOT_STEP(x::features::encounter::StartWorker());
#undef XCAT_BOOT_STEP

    if (AbortRequested()) {
        StopAllFeatureWorkers();
        return false;
    }
    return true;
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
    x::runtime::LogI("Bootstrap",
                     "native settle ≥%us + UnityWndClass before FindClass (no timeout bypass)",
                     static_cast<unsigned>(kPreEnsureNativeMinMs / 1000u));
    while (!AbortRequested()) {
        const DWORD now = GetTickCount();
        const DWORD elapsed = now - start;
        HWND unity = x::features::titlebar::win::FindUnityWndClass();
        if (unity) sawUnity = true;
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
                     "cold-start gate: GA exports → native settle → MainPump "
                     "(no FindClass until UnityWndClass+settle; abort on detach)");

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
            int expect = static_cast<int>(Phase::Waiting);
            if (!gPhase.compare_exchange_strong(expect, static_cast<int>(Phase::Starting),
                                                std::memory_order_acq_rel)) {
                x::runtime::LogI("Bootstrap", "cold-start aborted (phase=%d)", expect);
                return 0;
            }
            const bool ok = StartAllFeatureWorkers();
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
            x::runtime::LogI("Bootstrap", "cold-start complete — workers running");
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
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(h);
        x::runtime::SetImageModule(h);
        x::runtime::LogInit();
        MarkReady();
        gBootstrapStop.store(false, std::memory_order_release);
        gInManagedProbe.store(false, std::memory_order_release);
        gPhase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
        x::runtime::LogI("Bootstrap",
                         "attached — defer feature workers until MainPump "
                         "(cold-start GC gate)");
        gBootstrapThread = CreateThread(nullptr, 0, &BootstrapThread, nullptr, 0, nullptr);
        if (!gBootstrapThread) {
            UnmarkReady();
            x::runtime::LogW("Bootstrap",
                             "CreateThread failed — refusing immediate StartWorker "
                             "(would risk GC fatal); probe-ready cleared");
        }
        break;
    case DLL_PROCESS_DETACH: {
        // Signal only; never join (loader lock).
        gBootstrapStop.store(true, std::memory_order_release);
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
