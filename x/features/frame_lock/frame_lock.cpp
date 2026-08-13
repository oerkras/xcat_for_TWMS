// Classic TWMS — engine frame lock (NOT monitor refresh rate).
// QualitySettings.vSyncCount=0 → Application.targetFrameRate=N；主线程周期重应用。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "frame_lock.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "xcat_payload_control.h"

#include <atomic>
#include <climits>
#include <cstdint>

namespace x::features::frame_lock {
namespace {

using x::runtime::il2cpp::AtRva;

// remount 2026-08-06 dump.cs.runtime（相对 08-04 统一 +0x3FD0）
constexpr uint32_t kRvaSetTargetFps = 0x4E207F0;  // Application.set_targetFrameRate
constexpr uint32_t kRvaGetTargetFps = 0x4E207B0;  // Application.get_targetFrameRate
constexpr uint32_t kRvaSetVSync = 0x4E3BB50;      // QualitySettings.set_vSyncCount

constexpr DWORD kTickMsOn = 200;      // 轮询；真正重刷见 kReapplyMs / gApplyNow
constexpr DWORD kTickMsOff = 2000;
constexpr DWORD kReapplyMs = 1000;    // 开启态周期性再刷（防引擎重置）
constexpr DWORD kFailBackoffMs = 400; // Apply 失败退避，避免 10ms 打满主线程泵
constexpr DWORD kBindRetryMs = 1000;  // Bind 失败节流；methodPointer 空时允许重试
constexpr DWORD kJobWaitMs = 800;
constexpr DWORD kLogMs = 8000;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

// IL2CPP FreeFunction icall（IDA 实锤）：仅 rcx=int / 无 MethodInfo 末参。
// set_targetFrameRate@0x4E207F0 / set_vSyncCount@0x4E3BB50：保存 ecx → resolve → jmp icall。
using FnSetInt = void (*)(int value);
using FnGetInt = int (*)();

std::atomic<bool> gDesired{false};
std::atomic<bool> gWasOn{false};
std::atomic<bool> gStop{false};
std::atomic<bool> gApplyNow{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gTargetFps{xcat::kFrameLockFpsDefault};
std::atomic<int> gLastApplied{-1};
DWORD gLastApplyTick = 0;
DWORD gFailBackoffUntil = 0;
DWORD gLastBindTry = 0;

MethodInfoHead* gMiSetFps = nullptr;
MethodInfoHead* gMiGetFps = nullptr;
MethodInfoHead* gMiSetVSync = nullptr;

bool gOrigCaptured = false;
int gOrigTargetFps = -1;
// dump：vSyncCount 仅 set、无 get → 无法读原值。经典版默认通常为 1；关闭时还原 1，
// 避免留下 vSync=0 + target=-1 的无上限。若用户原本就关着 vSync，关功能后会被打开。
constexpr int kRestoreVSync = 1;

DWORD gLastLog = 0;
DWORD gLastMismatchLog = 0;

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

MethodInfoHead* ResolveUnityMi(void* klass, uint32_t rva, const char* plain,
                               const x::runtime::il2cpp_method::MethodShape& shape) {
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, nullptr);
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

bool BindMethods() {
    if (gMiSetFps && gMiSetFps->methodPointer && gMiSetVSync && gMiSetVSync->methodPointer)
        return true;

    // methodPointer 空：清掉 MI 指针，允许冷启动过早 Bind 后重试（不再 sticky fail）。
    if (gMiSetFps && !gMiSetFps->methodPointer) gMiSetFps = nullptr;
    if (gMiGetFps && !gMiGetFps->methodPointer) gMiGetFps = nullptr;
    if (gMiSetVSync && !gMiSetVSync->methodPointer) gMiSetVSync = nullptr;

    const DWORD now = GetTickCount();
    if (gLastBindTry != 0 && (now - gLastBindTry) < kBindRetryMs) return false;
    gLastBindTry = now;

    if (!x::runtime::il2cpp::Ensure()) return false;

    using namespace x::runtime::il2cpp_method;
    void* app = x::runtime::il2cpp::FindClass("UnityEngine", "Application");
    void* q = x::runtime::il2cpp::FindClass("UnityEngine", "QualitySettings");
    // void(int) 不唯一 → unique=false，靠明文+RVA
    constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::I32}};
    constexpr MethodShape kGet{0, TypeKind::I32, false, true, {}};

    if (app) {
        if (!gMiSetFps)
            gMiSetFps = ResolveUnityMi(app, kRvaSetTargetFps, "set_targetFrameRate", kSet);
        if (!gMiGetFps)
            gMiGetFps = ResolveUnityMi(app, kRvaGetTargetFps, "get_targetFrameRate", kGet);
    }
    if (q && !gMiSetVSync)
        gMiSetVSync = ResolveUnityMi(q, kRvaSetVSync, "set_vSyncCount", kSet);

    const bool ok = gMiSetFps && gMiSetFps->methodPointer && gMiSetVSync &&
                    gMiSetVSync->methodPointer;
    if (ok) {
        x::runtime::LogI("FrameLock",
                         "bound setFps=%p getFps=%p setVSync=%p", gMiSetFps->methodPointer,
                         gMiGetFps && gMiGetFps->methodPointer ? gMiGetFps->methodPointer
                                                               : nullptr,
                         gMiSetVSync->methodPointer);
    } else {
        x::runtime::LogWThrottled(81, 15000, "FrameLock",
                                  "bind miss setFps=%d setVSync=%d",
                                  gMiSetFps && gMiSetFps->methodPointer ? 1 : 0,
                                  gMiSetVSync && gMiSetVSync->methodPointer ? 1 : 0);
    }
    return ok;
}

bool CallSetInt(MethodInfoHead* mi, uint32_t rva, int v) {
    auto* fn = FnFromMi<FnSetInt>(mi, rva);
    if (!fn) return false;
    __try {
        fn(v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int CallGetInt(MethodInfoHead* mi, uint32_t rva) {
    auto* fn = FnFromMi<FnGetInt>(mi, rva);
    if (!fn) return INT_MIN;
    __try {
        return fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return INT_MIN;
    }
}

struct ApplyJob {
    bool lock = true;  // true=锁帧；false=还原 targetFrameRate
    int target = static_cast<int>(xcat::kFrameLockFpsDefault);
    int readback = -1;
    bool ok = false;
};

void ApplyOnMain(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("frame_lock.Apply");
    auto* job = reinterpret_cast<ApplyJob*>(user);
    if (!job) return;
    job->ok = false;
    job->readback = -1;

    if (!BindMethods()) return;

    if (job->lock) {
        if (!gOrigCaptured) {
            const int cur = CallGetInt(gMiGetFps, kRvaGetTargetFps);
            if (cur != INT_MIN) gOrigTargetFps = cur;
            gOrigCaptured = true;
        }
        // 先关引擎 vSync，再设目标帧率（不改显示器硬件刷新率）。
        if (!CallSetInt(gMiSetVSync, kRvaSetVSync, 0)) return;
        if (!CallSetInt(gMiSetFps, kRvaSetTargetFps, job->target)) return;
    } else {
        // 先还原 targetFrameRate，再恢复 vSync（顺序避免短暂无上限）。
        // job->target 忽略：还原值来自首次开启时捕获的 gOrigTargetFps。
        const int restoreFps = gOrigCaptured ? gOrigTargetFps : -1;
        if (!CallSetInt(gMiSetFps, kRvaSetTargetFps, restoreFps)) return;
        if (!CallSetInt(gMiSetVSync, kRvaSetVSync, kRestoreVSync)) return;
    }

    const int rb = CallGetInt(gMiGetFps, kRvaGetTargetFps);
    job->readback = (rb == INT_MIN) ? -1 : rb;
    job->ok = true;
}

bool InvokeApply(bool lock, int target, int* outReadback) {
    if (outReadback) *outReadback = -1;
    ApplyJob job{};
    job.lock = lock;
    job.target = target;
    if (!x::runtime::main_thread::Ensure()) return false;
    if (!x::runtime::main_thread::InvokeAndWait(&ApplyOnMain, &job, kJobWaitMs)) return false;
    if (outReadback) *outReadback = job.readback;
    return job.ok;
}

void NoteApplyFailure(DWORD now) {
    gFailBackoffUntil = now + kFailBackoffMs;
    gApplyNow.store(true, std::memory_order_relaxed);
}

void TickOnce(DWORD now) {
    if (gFailBackoffUntil != 0 && now < gFailBackoffUntil) return;

    const bool want = gDesired.load(std::memory_order_relaxed);
    const bool was = gWasOn.load(std::memory_order_relaxed);
    const bool force = gApplyNow.exchange(false, std::memory_order_acq_rel);
    const int fps =
        static_cast<int>(xcat::ClampFrameLockFps(gTargetFps.load(std::memory_order_relaxed)));

    if (!want) {
        if (was) {
            int rb = -1;
            if (!InvokeApply(/*lock=*/false, /*unused*/0, &rb)) {
                NoteApplyFailure(now);
                x::runtime::LogWThrottled(82, 3000, "FrameLock", "restore pending");
                return;
            }
            gWasOn.store(false, std::memory_order_relaxed);
            gLastApplied.store(rb, std::memory_order_relaxed);
            gLastApplyTick = now;
            gFailBackoffUntil = 0;
            x::runtime::LogI("FrameLock",
                             "restore targetFrameRate readback=%d vSync=%d", rb,
                             kRestoreVSync);
        }
        return;
    }

    if (!force && gLastApplyTick != 0 && (now - gLastApplyTick) < kReapplyMs) return;

    int rb = -1;
    if (!InvokeApply(/*lock=*/true, fps, &rb)) {
        NoteApplyFailure(now);
        return;
    }
    gWasOn.store(true, std::memory_order_relaxed);
    gLastApplied.store(rb, std::memory_order_relaxed);
    gLastApplyTick = now;
    gFailBackoffUntil = 0;

    if (rb >= 0) {
        const int drift = rb > fps ? (rb - fps) : (fps - rb);
        if (drift > 1) {
            if (gLastMismatchLog == 0 || now - gLastMismatchLog >= 5000) {
                gLastMismatchLog = now;
                x::runtime::LogW("FrameLock",
                                 "readback mismatch want=%d got=%d (driver/OS may override)",
                                 fps, rb);
            }
        }
    }

    if (force || gLastLog == 0 || now - gLastLog >= kLogMs) {
        gLastLog = now;
        x::runtime::LogI("FrameLock", "lock fps=%d readback=%d (engine, not monitor Hz)", fps,
                         rb);
    }
}

DWORD WorkerSleepMs(DWORD now) {
    if (gApplyNow.load(std::memory_order_relaxed)) {
        if (gFailBackoffUntil != 0 && now < gFailBackoffUntil)
            return gFailBackoffUntil - now;
        return 10;
    }
    const bool on = gDesired.load(std::memory_order_relaxed) ||
                    gWasOn.load(std::memory_order_relaxed);
    return on ? kTickMsOn : kTickMsOff;
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("FrameLock", "worker start");
    while (!gStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        TickOnce(now);
        Sleep(WorkerSleepMs(GetTickCount()));
    }
    if (gWasOn.load(std::memory_order_relaxed)) {
        int rb = -1;
        for (int i = 0; i < 3; ++i) {
            // target 参数在 restore 路径忽略（用 gOrigTargetFps）
            if (InvokeApply(/*lock=*/false, /*unused*/0, &rb)) break;
            Sleep(50);
        }
        gWasOn.store(false, std::memory_order_relaxed);
        gLastApplied.store(rb, std::memory_order_relaxed);
        x::runtime::LogI("FrameLock", "worker stop restore readback=%d vSync=%d", rb,
                         kRestoreVSync);
    }
    x::runtime::LogI("FrameLock", "worker exit");
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false);
    gDesired.store(false);
    gWasOn.store(false);
    gTargetFps.store(xcat::kFrameLockFpsDefault);
    gLastApplied.store(-1);
    gApplyNow.store(false);
    gLastApplyTick = 0;
    gFailBackoffUntil = 0;
    gLastBindTry = 0;
    gMiSetFps = nullptr;
    gMiGetFps = nullptr;
    gMiSetVSync = nullptr;
    gOrigCaptured = false;
    gOrigTargetFps = -1;
    gLastLog = 0;
    gLastMismatchLog = 0;
    x::runtime::LogI("FrameLock",
                     "Init (vSync=0 + Application.targetFrameRate; not monitor Hz)");
}

void Shutdown() { StopWorker(); }

void SetEnabled(bool on) {
    gDesired.store(on, std::memory_order_relaxed);
    gApplyNow.store(true, std::memory_order_release);
}

void SetTargetFps(uint32_t fps) {
    gTargetFps.store(xcat::ClampFrameLockFps(fps), std::memory_order_relaxed);
    gApplyNow.store(true, std::memory_order_release);
}

bool IsEnabled() { return gDesired.load(std::memory_order_relaxed); }

uint32_t TargetFps() { return gTargetFps.load(std::memory_order_relaxed); }

int LastAppliedFps() { return gLastApplied.load(std::memory_order_relaxed); }

void Tick(DWORD now) { TickOnce(now); }

void StartWorker() {
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("FrameLock", "CreateThread failed");
        return;
    }
    gWorker.store(th, std::memory_order_release);
}

void StopWorker() {
    gStop.store(true, std::memory_order_release);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

}  // namespace x::features::frame_lock
