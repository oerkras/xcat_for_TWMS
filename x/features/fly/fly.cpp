#include "fly.h"
#include "fly_bridge.h"

#include <atomic>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace fly {
namespace {

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

DWORD WINAPI FlyThread(LPVOID) {
    timeBeginPeriod(1);
    Beep(880, 120);
    Beep(1175, 120);
    twms_fly_impl::Log("TwmsFly worker start");
    // GRAP ban: no MovePath.Flush E9 / any .text INLINE HOOK.
    // Fly is data-plane only (Ap/CurPos/_forcedFlush). See docs/features/security/GRAP与枫星对齐.md §4.1.
    twms_fly_impl::Log("Flush INLINE HOOK removed — data-plane fly only");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gWorkerStop.load(); ++i)
        Sleep(50);

    if (gWorkerStop.load()) {
        timeEndPeriod(1);
        return 0;
    }

    if (!twms_fly_impl::BindApis()) {
        twms_fly_impl::Log("BindApis failed");
        Beep(400, 400);
        timeEndPeriod(1);
        return 1;
    }
    twms_fly_impl::ResolveActor(false);
    twms_fly_impl::Log("ready: F6 fly, F7 rebind, F8 cam. how=%s lu=%p",
                       twms_fly_impl::ResolveHow(), twms_fly_impl::LocalUserPtr());

    LARGE_INTEGER freq{}, last{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    DWORD lastHb = GetTickCount();
    while (!gWorkerStop.load()) {
        twms_fly_impl::PollF6();
        twms_fly_impl::PollF7Rebind();
        twms_fly_impl::PollF8PreferCamera();
        twms_fly_impl::WatchBinding();
        if (twms_fly_impl::IsFlyOn()) twms_fly_impl::TickFly();

        const DWORD nowMs = GetTickCount();
        if (nowMs - lastHb >= 5000) {
            lastHb = nowMs;
            ++twms_fly_impl::TickCountRef();
            twms_fly_impl::Log("heartbeat n=%lu fly=%d dead=%d how=%s tf=%p",
                               twms_fly_impl::TickCountRef(), twms_fly_impl::IsFlyOn() ? 1 : 0,
                               twms_fly_impl::IsSessionDead() ? 1 : 0, twms_fly_impl::ResolveHow(),
                               twms_fly_impl::TransformPtr());
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        constexpr double kDt = 1.0 / 60.0;
        if (elapsed < kDt) {
            const DWORD ms = static_cast<DWORD>((kDt - elapsed) * 1000.0);
            if (ms > 0) Sleep(ms);
        }
        QueryPerformanceCounter(&last);
    }
    twms_fly_impl::Log("TwmsFly worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    twms_fly_impl::OpenLogs();
    twms_fly_impl::Log("TwmsFly Init pid=%lu", GetCurrentProcessId());
}

void Shutdown() {
    StopWorker();
}

void StartWorker() {
    if (gWorkerThread.load() != nullptr) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, FlyThread, nullptr, 0, nullptr);
    if (!th) {
        twms_fly_impl::Log("CreateThread FAILED err=%lu", GetLastError());
        return;
    }
    gWorkerThread.store(th);
    twms_fly_impl::Log("CreateThread ok");
}

void StopWorker() {
    gWorkerStop.store(true);
    // Never WaitForSingleObject here — DllMain DETACH would deadlock the loader lock.
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) CloseHandle(th);
}

void SetDesired(bool on) {
    if (on != twms_fly_impl::IsFlyOn()) twms_fly_impl::ArmFly(on);
}
bool IsDesired() { return twms_fly_impl::IsFlyOn(); }
bool IsEnabled() { return twms_fly_impl::IsFlyOn() && !twms_fly_impl::IsSessionDead(); }
float GetSpeed() { return twms_fly_impl::GetFlySpeed(); }
void SetSpeed(float v) { twms_fly_impl::SetFlySpeed(v); }
void TickRealtime() {
    if (twms_fly_impl::IsFlyOn()) twms_fly_impl::TickFly();
}
bool PollFlyHotkey() {
    const bool before = twms_fly_impl::IsFlyOn();
    twms_fly_impl::PollF6();
    return before != twms_fly_impl::IsFlyOn();
}
void ToggleFly() { twms_fly_impl::ArmFly(!twms_fly_impl::IsFlyOn()); }
void ForceRebind() { twms_fly_impl::PollF7Rebind(); }
void PreferCameraBind() { twms_fly_impl::PollF8PreferCamera(); }

}  // namespace fly
}  // namespace features
}  // namespace x

#if defined(TWMS_FLY_STANDALONE)
BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        x::features::fly::Init();
        x::features::fly::StartWorker();
    } else if (reason == DLL_PROCESS_DETACH) {
        x::features::fly::StopWorker();
    }
    return TRUE;
}
#endif
