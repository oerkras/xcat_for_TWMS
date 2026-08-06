// Shared main-thread pump — Classic TWMS (MethodInfo, no GA .text E9).
// NEVER run managed/GC work on a worker "direct invoke" path — Unity fatals with
// "Fatal error in GC / Collecting from unknown thread".
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "main_thread_pump.h"

#include "il2cpp_bind.h"
#include "il2cpp_method.h"
#include "il2cpp_shape.h"
#include "log.h"

#include <atomic>
#include <cstring>

namespace x::runtime::main_thread {
namespace {

constexpr int kQueueCap = 8;
// RVA = fast path only; resolve order: Unity name → FindMethodCached(RVA+kind) → RVA scan.
constexpr uint32_t kRvaSendWillRenderCanvases = 0x5244FE0;  // remounted 2026-08-04
constexpr uint32_t kRvaSceneLoginUpdate = 0xC02C50;         // remounted 2026-08-04
constexpr uint32_t kRvaWorldManagerFixedUpdate = 0xDD3460;  // remounted 2026-08-04
constexpr uint32_t kRvaWorldManagerUpdate = 0xDD7270;       // remounted 2026-08-04

// All pump hooks are parameterless void (instance or static).
constexpr x::runtime::il2cpp_method::MethodShape kShapeVoid0{
    0, x::runtime::il2cpp_method::TypeKind::Void, true, false, {}};

using FnClassGetMethodFromName = void* (*)(void* klass, const char* name, int argc);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnMethodGetName = const char* (*)(const void* method);
using FnRuntimeClassInit = void (*)(void* klass);
using FnSendWill = void (*)(const void* methodInfo);
using FnUpdate = void (*)(void* self, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

struct QueuedJob {
    JobFn fn = nullptr;
    void* user = nullptr;
    HANDLE done = nullptr;
    bool slotUsed = false;
    int prio = 0;       // JobPrio as int
    uint32_t seq = 0;   // enqueue order; same prio → FIFO
};

HMODULE gGA = nullptr;
uintptr_t gGaBase = 0;
FnClassGetMethodFromName gClassGetMethodFromName = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnMethodGetName gMethodGetName = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;

MethodInfoHead* gMiSendWill = nullptr;
void* gOrigSendWill = nullptr;
MethodInfoHead* gMiSceneUpdate = nullptr;
void* gOrigSceneUpdate = nullptr;
MethodInfoHead* gMiWmFixedUpdate = nullptr;
void* gOrigWmFixedUpdate = nullptr;
MethodInfoHead* gMiWmUpdate = nullptr;
void* gOrigWmUpdate = nullptr;

std::atomic<bool> gPumpInstalled{false};
std::atomic<bool> gInPump{false};
std::atomic<bool> gApisBound{false};
std::atomic<DWORD> gLastFailLogMs{0};
std::atomic<DWORD> gLastHeartbeatMs{0};  // last Drain-host tick only
std::atomic<uint32_t> gRealTickCount{0};  // install grace 不计入
std::atomic<JobFn> gFrameTick{nullptr};
std::atomic<void*> gFrameTickUser{nullptr};
std::atomic<int> gPumpPhase{static_cast<int>(PumpPhase::Bootstrap)};
std::atomic<DWORD> gLastBudgetLogMs{0};
std::atomic<bool> gWmHostLogged{false};
std::atomic<DWORD> gLastWmTickMs{0};  // any WM FixedUpdate/Update hook fire
std::atomic<DWORD> gLastFallbackLogMs{0};
std::atomic<DWORD> gPumpTid{0};  // Unity pump thread; set on each hook entry
std::atomic<int> gQueuedCount{0};  // parked-job depth; +1 enqueue, -1 dequeue/reclaim
std::atomic<uint32_t> gEnqueueSeq{0};  // monotonic; Drain ties broken by earlier seq

void NotePumpThread() {
    gPumpTid.store(GetCurrentThreadId(), std::memory_order_release);
}

// If Canvas.SendWill is not ticking (login/load freeze), refuse to park jobs for
// the full timeout — otherwise N workers fill kQueueCap and log "queue full".
constexpr DWORD kPumpIdleFailMs = 2000;
// Per-tick Drain 上限；面板/ini 可调，默认抽干整队（=kQueueCap）。
std::atomic<int> gDrainBudget{kQueueCap};
// Queue depth at/above which IsCongested() tells producers to back off.
// Runtime-tunable via SetCongestionThreshold (panel/config); 0 disables backpressure.
std::atomic<int> gCongestionThreshold{(kQueueCap * 3) / 4};
// InMap: if WM FixedUpdate/Update has not fired within this window, SendWill
// resumes Drain so InvokeAndWait does not hard-fail at kPumpIdleFailMs.
constexpr DWORD kWmIdleFallbackMs = 1000;

CRITICAL_SECTION gCs{};
bool gCsInit = false;
QueuedJob gQueue[kQueueCap]{};

void EnsureCs() {
    if (gCsInit) return;
    InitializeCriticalSection(&gCs);
    gCsInit = true;
}

void FailLogThrottled(const char* msg) {
    const DWORD now = GetTickCount();
    if (now - gLastFailLogMs.load() < 3000) return;
    gLastFailLogMs.store(now);
    x::runtime::LogW("MainPump", "%s", msg);
}

void* FindClass(const char* ns, const char* name) {
    // 走公共核；Ensure 只 GetProcAddress，不调 managed_main，无泵循环依赖
    return il2cpp::FindClass(ns, name);
}

bool BindApis() {
    if (gApisBound.load()) return true;
    if (!il2cpp::Ensure()) return false;
    const auto& e = il2cpp::Get();
    gGA = e.ga;
    gGaBase = il2cpp::GaBase();
    gClassGetMethodFromName =
        reinterpret_cast<FnClassGetMethodFromName>(e.classGetMethodFromName);
    gClassGetMethods = reinterpret_cast<FnClassGetMethods>(e.classGetMethods);
    gMethodGetName = reinterpret_cast<FnMethodGetName>(e.methodGetName);
    gRuntimeClassInit = reinterpret_cast<FnRuntimeClassInit>(e.runtimeClassInit);
    if (!gGA || !e.domainGet || !e.classFromName) return false;
    gApisBound.store(true);
    return true;
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        if (mi->virtualMethodPointer == orig) mi->virtualMethodPointer = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return ok;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    __try {
        void* cur = mi->methodPointer;
        mi->methodPointer = orig;
        if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

MethodInfoHead* FindMiByName(void* klass, const char* wantName, int argcHint) {
    if (!klass || !wantName) return nullptr;
    if (gClassGetMethodFromName) {
        __try {
            auto* mi = reinterpret_cast<MethodInfoHead*>(
                gClassGetMethodFromName(klass, wantName, argcHint));
            if (mi) return mi;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!gClassGetMethods || !gMethodGetName) return nullptr;
    void* iter = nullptr;
    for (;;) {
        void* miRaw = nullptr;
        __try {
            miRaw = gClassGetMethods(klass, &iter);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!miRaw) break;
        const char* name = nullptr;
        __try {
            name = gMethodGetName(miRaw);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (name && strcmp(name, wantName) == 0) return reinterpret_cast<MethodInfoHead*>(miRaw);
    }
    return nullptr;
}

MethodInfoHead* FindMiByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGaBase || !rva) return nullptr;
    void* target = reinterpret_cast<void*>(gGaBase + rva);
    void* iter = nullptr;
    for (;;) {
        void* miRaw = nullptr;
        __try {
            miRaw = gClassGetMethods(klass, &iter);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!miRaw) break;
        auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
        void* mp = nullptr;
        void* vp = nullptr;
        __try {
            mp = mi->methodPointer;
            vp = mi->virtualMethodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (mp == target || vp == target) return mi;
    }
    return nullptr;
}

// Anti-drift: Unity plaintext name → FindMethodCached(RVA+void()) → raw RVA walk.
MethodInfoHead* ResolvePumpMi(void* klass, uint32_t rva, const char* nameHint) {
    if (nameHint && klass) {
        if (MethodInfoHead* byName = FindMiByName(klass, nameHint, kShapeVoid0.arity)) return byName;
    }
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, kShapeVoid0);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            FailLogThrottled("ResolvePumpMi kind-fallback (RVA drifted)");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMiByRva(klass, rva);
}

void RunOne(QueuedJob& j) {
    if (!j.fn) return;
    __try {
        j.fn(j.user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("MainPump", "job SEH");
    }
    if (j.done) SetEvent(j.done);
}

// Pop up to maxJobs (higher JobPrio first; same prio → earlier enqueue). Leftovers next tick.
int DrainQueueBudget(int maxJobs) {
    if (maxJobs <= 0) return 0;
    EnsureCs();
    int ran = 0;
    for (; ran < maxJobs;) {
        QueuedJob job{};
        EnterCriticalSection(&gCs);
        bool found = false;
        int best = -1;
        for (int i = 0; i < kQueueCap; ++i) {
            if (!gQueue[i].slotUsed) continue;
            if (best < 0) {
                best = i;
                continue;
            }
            const QueuedJob& a = gQueue[i];
            const QueuedJob& b = gQueue[best];
            if (a.prio > b.prio || (a.prio == b.prio && a.seq < b.seq)) best = i;
        }
        if (best >= 0) {
            job = gQueue[best];
            gQueue[best] = {};
            gQueuedCount.fetch_sub(1, std::memory_order_relaxed);
            found = true;
        }
        LeaveCriticalSection(&gCs);
        if (!found) break;
        RunOne(job);
        ++ran;
    }
    // 预算用尽或 Drain 期间又入队 → 下一 tick 再抽。
    bool leftover = false;
    EnterCriticalSection(&gCs);
    for (int i = 0; i < kQueueCap; ++i) {
        if (gQueue[i].slotUsed) {
            leftover = true;
            break;
        }
    }
    LeaveCriticalSection(&gCs);
    if (leftover) {
        const DWORD now = GetTickCount();
        if (now - gLastBudgetLogMs.load() >= 3000) {
            gLastBudgetLogMs.store(now);
            x::runtime::LogW("MainPump", "drain leftover — deferred to next tick (budget=%d)",
                             gDrainBudget.load(std::memory_order_relaxed));
        }
    }
    return ran;
}

void RunFrameTick() {
    JobFn fn = gFrameTick.load(std::memory_order_acquire);
    if (!fn) return;
    void* user = gFrameTickUser.load(std::memory_order_acquire);
    __try {
        fn(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("MainPump", "frame tick SEH");
    }
}

void NoteHeartbeat() {
    gLastHeartbeatMs.store(GetTickCount(), std::memory_order_release);
    gRealTickCount.fetch_add(1, std::memory_order_relaxed);
}

bool WmHostInstalled() { return gMiWmFixedUpdate != nullptr || gMiWmUpdate != nullptr; }

// InMap + WM MI patched + WM recently ticked → Drain only on WM hooks.
// Missing MI, never ticked, or idle > kWmIdleFallbackMs → Bootstrap hooks keep Drain
// (hard constraint: never leave PlayReady without a live drain host).
bool WmHostsDrain() {
    if (gPumpPhase.load(std::memory_order_acquire) != static_cast<int>(PumpPhase::InMap)) {
        return false;
    }
    if (!WmHostInstalled()) return false;
    const DWORD lastWm = gLastWmTickMs.load(std::memory_order_acquire);
    if (!lastWm) return false;
    const DWORD age = GetTickCount() - lastWm;
    if (age > kWmIdleFallbackMs) {
        const DWORD now = GetTickCount();
        if (now - gLastFallbackLogMs.load(std::memory_order_relaxed) >= 3000) {
            gLastFallbackLogMs.store(now, std::memory_order_relaxed);
            x::runtime::LogW("MainPump", "WM host idle %lums — fallback SendWill Drain",
                             static_cast<unsigned long>(age));
        }
        return false;
    }
    return true;
}

void NoteWmHookFire() {
    gLastWmTickMs.store(GetTickCount(), std::memory_order_release);
}

void DrainAfterOrig() {
    if (!gInPump.exchange(true)) {
        (void)DrainQueueBudget(gDrainBudget.load(std::memory_order_relaxed));
        gInPump.store(false);
    }
}

void HookSendWill(const void* methodInfo) {
    NotePumpThread();
    // Prefer canvas work first — do not park heavy Drain ahead of UI render.
    auto orig = reinterpret_cast<FnSendWill>(gOrigSendWill);
    if (orig) orig(methodInfo);
    if (!WmHostsDrain()) {
        NoteHeartbeat();
        DrainAfterOrig();
    }
    // FrameTick stays on SendWill so invuln keeps a per-frame render-path tick in-map.
    RunFrameTick();
}

void HookSceneUpdate(void* self, const void* methodInfo) {
    NotePumpThread();
    auto orig = reinterpret_cast<FnUpdate>(gOrigSceneUpdate);
    if (orig) orig(self, methodInfo);
    if (!WmHostsDrain()) {
        NoteHeartbeat();
        DrainAfterOrig();
    }
}

void HookWmTick(void* self, const void* methodInfo, void* origFn) {
    NotePumpThread();
    auto orig = reinterpret_cast<FnUpdate>(origFn);
    if (orig) orig(self, methodInfo);
    NoteWmHookFire();
    if (WmHostsDrain()) {
        NoteHeartbeat();
        DrainAfterOrig();
    }
}

void HookWmFixedUpdate(void* self, const void* methodInfo) {
    HookWmTick(self, methodInfo, gOrigWmFixedUpdate);
}

void HookWmUpdate(void* self, const void* methodInfo) {
    HookWmTick(self, methodInfo, gOrigWmUpdate);
}

bool TryInstallSendWill(MethodInfoHead* mi) {
    if (!mi || gMiSendWill) return gMiSendWill != nullptr;
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&HookSendWill), &orig)) return false;
    gMiSendWill = mi;
    gOrigSendWill = orig;
    return true;
}

bool TryInstallSceneUpdate(MethodInfoHead* mi) {
    if (!mi || gMiSceneUpdate) return gMiSceneUpdate != nullptr;
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&HookSceneUpdate), &orig)) return false;
    gMiSceneUpdate = mi;
    gOrigSceneUpdate = orig;
    return true;
}

bool TryInstallWmFixedUpdate(MethodInfoHead* mi) {
    if (!mi || gMiWmFixedUpdate) return gMiWmFixedUpdate != nullptr;
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&HookWmFixedUpdate), &orig)) return false;
    gMiWmFixedUpdate = mi;
    gOrigWmFixedUpdate = orig;
    return true;
}

bool TryInstallWmUpdate(MethodInfoHead* mi) {
    if (!mi || gMiWmUpdate) return gMiWmUpdate != nullptr;
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&HookWmUpdate), &orig)) return false;
    gMiWmUpdate = mi;
    gOrigWmUpdate = orig;
    return true;
}

bool InstallPump() {
    if (!BindApis()) {
        FailLogThrottled("BindApis fail (wait GameAssembly)");
        return false;
    }

    // Do NOT call il2cpp_runtime_class_init here — Ensure() runs from worker threads
    // and class_init allocates (GC fatal: Collecting from unknown thread).
    (void)gRuntimeClassInit;

    // 双挂：Canvas 过图可能停跳；SceneLogin.Update 在登录/加载期仍可能心跳。
    // Klass：Canvas 明文；SceneLogin / WM 走 shape（hash→field）。
    // MI：ResolvePumpMi = Unity 名 → FindMethodCached(RVA+void0) → RVA。
    if (!gMiSendWill) {
        void* klassCanvas = FindClass("UnityEngine", "Canvas");
        MethodInfoHead* miSend =
            ResolvePumpMi(klassCanvas, kRvaSendWillRenderCanvases, "SendWillRenderCanvases");
        TryInstallSendWill(miSend);
    }
    if (!gMiSceneUpdate) {
        void* klassSl = x::runtime::il2cpp_shape::ResolveSceneLoginKlass();
        MethodInfoHead* miUp = ResolvePumpMi(klassSl, kRvaSceneLoginUpdate, "Update");
        if (!klassSl) {
            FailLogThrottled("SceneLogin klass miss (hash/shape) — Canvas-only pump");
        } else if (!miUp) {
            FailLogThrottled("SceneLogin.Update MI miss — Canvas-only pump");
        }
        TryInstallSceneUpdate(miUp);
    }

    // WM InMap host — klass may appear after login; retry every Ensure.
    if (!gMiWmFixedUpdate || !gMiWmUpdate) {
        void* klassWm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
        if (klassWm) {
            if (!gMiWmFixedUpdate) {
                MethodInfoHead* miFu =
                    ResolvePumpMi(klassWm, kRvaWorldManagerFixedUpdate, "FixedUpdate");
                if (!TryInstallWmFixedUpdate(miFu) && !gMiWmFixedUpdate) {
                    FailLogThrottled("WM.FixedUpdate MI miss — InMap keeps SendWill Drain");
                }
            }
            if (!gMiWmUpdate) {
                MethodInfoHead* miUp = ResolvePumpMi(klassWm, kRvaWorldManagerUpdate, "Update");
                (void)TryInstallWmUpdate(miUp);
            }
        }
    }

    if (!gMiSendWill && !gMiSceneUpdate) {
        FailLogThrottled("pump MI miss (Canvas+SceneLogin) — wait, no direct invoke");
        return false;
    }
    // 不伪造心跳：真实 tick 前 InvokeAndWait 拒绝排队。
    const bool first = !gPumpInstalled.exchange(true);
    if (first) {
        x::runtime::LogI("MainPump",
                         "installed sendWill=%d sceneUpdate=%d wmFU=%d wmUp=%d drainBudget=%d "
                         "(queueCap=%d, wait real tick)",
                         gMiSendWill ? 1 : 0, gMiSceneUpdate ? 1 : 0, gMiWmFixedUpdate ? 1 : 0,
                         gMiWmUpdate ? 1 : 0, gDrainBudget.load(std::memory_order_relaxed),
                         kQueueCap);
    } else if (WmHostInstalled()) {
        if (!gWmHostLogged.exchange(true)) {
            x::runtime::LogI("MainPump", "WM drain host ready fu=%d up=%d",
                             gMiWmFixedUpdate ? 1 : 0, gMiWmUpdate ? 1 : 0);
        }
    }
    return true;
}

}  // namespace

bool Ensure() {
    EnsureCs();
    return InstallPump();
}

bool IsInstalled() { return gPumpInstalled.load(); }

void SetPumpPhase(PumpPhase phase) {
    const int next = static_cast<int>(phase);
    const int prev = gPumpPhase.exchange(next, std::memory_order_acq_rel);
    if (prev != next) {
        // Retry WM MI install when entering InMap (klass often ready only then).
        if (phase == PumpPhase::InMap) (void)Ensure();
        x::runtime::LogI("MainPump", "phase %s wmHost=%d (fu=%d up=%d)",
                         phase == PumpPhase::InMap ? "InMap" : "Bootstrap",
                         WmHostInstalled() ? 1 : 0, gMiWmFixedUpdate ? 1 : 0,
                         gMiWmUpdate ? 1 : 0);
    }
}

PumpPhase GetPumpPhase() {
    return static_cast<PumpPhase>(gPumpPhase.load(std::memory_order_acquire));
}

bool IsWmDrainHostActive() { return WmHostsDrain(); }

uint32_t RealTickCount() { return gRealTickCount.load(std::memory_order_acquire); }

DWORD LastRealTickAgeMs() {
    const DWORD last = gLastHeartbeatMs.load(std::memory_order_acquire);
    if (!last) return 0xFFFFFFFFu;
    return GetTickCount() - last;
}

bool IsPumpTicking(DWORD maxAgeMs) {
    const DWORD age = LastRealTickAgeMs();
    return age != 0xFFFFFFFFu && age <= maxAgeMs;
}

bool WaitUntilInstalled(DWORD timeoutMs, DWORD pollMs) {
    if (IsInstalled()) return true;
    if (timeoutMs == 0) return Ensure();
    if (pollMs == 0) pollMs = 250;
    const DWORD start = GetTickCount();
    for (;;) {
        if (Ensure()) return true;
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) return IsInstalled();
        const DWORD remain = timeoutMs - elapsed;
        Sleep(remain < pollMs ? remain : pollMs);
    }
}

void SetFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gFrameTick.store(nullptr, std::memory_order_release);
        gFrameTickUser.store(nullptr, std::memory_order_release);
        return;
    }
    gFrameTickUser.store(user, std::memory_order_release);
    gFrameTick.store(fn, std::memory_order_release);
}

bool IsDirectMode() {
    // Direct mode removed: it caused GC "Collecting from unknown thread".
    return false;
}

bool IsOnPumpThread() {
    const DWORD tid = gPumpTid.load(std::memory_order_acquire);
    return tid != 0 && tid == GetCurrentThreadId();
}

int QueuedJobCount() {
    const int n = gQueuedCount.load(std::memory_order_relaxed);
    return n < 0 ? 0 : n;
}

bool IsCongested() {
    const int th = gCongestionThreshold.load(std::memory_order_relaxed);
    if (th <= 0) return false;  // 0 = backpressure off
    return gQueuedCount.load(std::memory_order_relaxed) >= th;
}

void SetCongestionThreshold(int depth) {
    if (depth < 0) depth = 0;
    if (depth > kQueueCap) depth = kQueueCap;
    const int prev = gCongestionThreshold.exchange(depth, std::memory_order_relaxed);
    if (prev != depth)
        x::runtime::LogI("MainPump", "congestion threshold %d -> %d%s", prev, depth,
                         depth == 0 ? " (backpressure off)" : "");
}

void SetDrainBudget(int maxJobs) {
    if (maxJobs < 1) maxJobs = 1;
    if (maxJobs > kQueueCap) maxJobs = kQueueCap;
    const int prev = gDrainBudget.exchange(maxJobs, std::memory_order_relaxed);
    if (prev != maxJobs)
        x::runtime::LogI("MainPump", "drain budget %d -> %d%s", prev, maxJobs,
                         maxJobs >= kQueueCap ? " (full queue)" : "");
}

int DrainBudget() { return gDrainBudget.load(std::memory_order_relaxed); }

bool AssertOnPumpThread(const char* tag) {
    if (IsOnPumpThread()) return true;
    const unsigned long tid = static_cast<unsigned long>(GetCurrentThreadId());
#if defined(_DEBUG)
    x::runtime::LogW("MainPump", "OFF-PUMP managed access '%s' tid=%lu — GC unknown-thread risk",
                     tag ? tag : "?", tid);
    __debugbreak();
#else
    x::runtime::LogWThrottled(60, 3000, "MainPump",
                              "OFF-PUMP managed access '%s' tid=%lu — GC risk", tag ? tag : "?",
                              tid);
#endif
    return false;
}

bool InvokeAndWait(JobFn fn, void* user, DWORD timeoutMs, JobPrio prio) {
    if (!fn) return false;
    if (!Ensure()) return false;
    if (!gPumpInstalled.load()) return false;

    const DWORD now = GetTickCount();
    const DWORD lastHb = gLastHeartbeatMs.load(std::memory_order_acquire);
    // 从未真实 tick（含刚装泵）：禁止排队，避免 5e3768 式 job timeout / 冻死。
    if (!lastHb || now - lastHb > kPumpIdleFailMs) {
        FailLogThrottled(lastHb ? "pump idle (no drain-host tick — load/freeze)"
                                : "pump not ticking yet (wait first drain-host tick)");
        return false;
    }

    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return false;

    EnsureCs();
    EnterCriticalSection(&gCs);
    int slot = -1;
    for (int i = 0; i < kQueueCap; ++i) {
        if (gQueue[i].slotUsed) continue;
        gQueue[i].fn = fn;
        gQueue[i].user = user;
        gQueue[i].done = done;
        gQueue[i].slotUsed = true;
        gQueue[i].prio = static_cast<int>(prio);
        gQueue[i].seq = gEnqueueSeq.fetch_add(1, std::memory_order_relaxed);
        gQueuedCount.fetch_add(1, std::memory_order_relaxed);
        slot = i;
        break;
    }
    LeaveCriticalSection(&gCs);

    if (slot < 0) {
        CloseHandle(done);
        FailLogThrottled("queue full");
        return false;
    }

    const DWORD wr = WaitForSingleObject(done, timeoutMs ? timeoutMs : 1);
    if (wr != WAIT_OBJECT_0) {
        bool stillQueued = false;
        EnterCriticalSection(&gCs);
        if (gQueue[slot].slotUsed && gQueue[slot].done == done) {
            gQueue[slot] = {};
            gQueuedCount.fetch_sub(1, std::memory_order_relaxed);
            stillQueued = true;
        }
        LeaveCriticalSection(&gCs);
        if (!stillQueued) (void)WaitForSingleObject(done, 500);
        CloseHandle(done);
        FailLogThrottled("job timeout");
        return false;
    }
    CloseHandle(done);
    return true;
}

void Shutdown() {
    gFrameTick.store(nullptr, std::memory_order_release);
    gFrameTickUser.store(nullptr, std::memory_order_release);
    EnsureCs();
    EnterCriticalSection(&gCs);
    for (int i = 0; i < kQueueCap; ++i) {
        if (gQueue[i].slotUsed && gQueue[i].done) SetEvent(gQueue[i].done);
        gQueue[i] = {};
    }
    gQueuedCount.store(0, std::memory_order_relaxed);
    LeaveCriticalSection(&gCs);

    if (gPumpInstalled.exchange(false)) {
        if (gMiSendWill && gOrigSendWill) RestoreMethodInfo(gMiSendWill, gOrigSendWill);
        if (gMiSceneUpdate && gOrigSceneUpdate)
            RestoreMethodInfo(gMiSceneUpdate, gOrigSceneUpdate);
        if (gMiWmFixedUpdate && gOrigWmFixedUpdate)
            RestoreMethodInfo(gMiWmFixedUpdate, gOrigWmFixedUpdate);
        if (gMiWmUpdate && gOrigWmUpdate) RestoreMethodInfo(gMiWmUpdate, gOrigWmUpdate);
        gMiSendWill = nullptr;
        gOrigSendWill = nullptr;
        gMiSceneUpdate = nullptr;
        gOrigSceneUpdate = nullptr;
        gMiWmFixedUpdate = nullptr;
        gOrigWmFixedUpdate = nullptr;
        gMiWmUpdate = nullptr;
        gOrigWmUpdate = nullptr;
        x::runtime::LogI("MainPump", "uninstalled");
    }
    gLastHeartbeatMs.store(0, std::memory_order_release);
    gRealTickCount.store(0, std::memory_order_release);
    gLastWmTickMs.store(0, std::memory_order_release);
    gPumpTid.store(0, std::memory_order_release);
    gPumpPhase.store(static_cast<int>(PumpPhase::Bootstrap), std::memory_order_release);
    gWmHostLogged.store(false, std::memory_order_release);
    gApisBound.store(false);
    gGA = nullptr;
    gGaBase = 0;
}

}  // namespace x::runtime::main_thread
