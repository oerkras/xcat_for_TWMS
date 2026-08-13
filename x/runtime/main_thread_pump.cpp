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
#include "managed_main.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::runtime::main_thread {
namespace {

constexpr int kQueueCap = 8;
// RVA = 末级兜底；ResolvePumpMi：明文名 → unique void() kind → RVA（禁止 RVA-first）。
constexpr uint32_t kRvaSendWillRenderCanvases = 0x5248FB0;  // remounted 2026-08-06 Canvas.SendWillRenderCanvases
constexpr uint32_t kRvaSceneLoginUpdate = 0xC033D0;         // remounted 2026-08-06
constexpr uint32_t kRvaWorldManagerFixedUpdate = 0xDD5400;  // remounted 2026-08-06
constexpr uint32_t kRvaWorldManagerUpdate = 0xDD9380;       // remounted 2026-08-06

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
std::atomic<JobFn> gAuxFrameTick{nullptr};
std::atomic<void*> gAuxFrameTickUser{nullptr};
std::atomic<JobFn> gBinFrameTick{nullptr};
std::atomic<void*> gBinFrameTickUser{nullptr};
std::atomic<JobFn> gLieFrameTick{nullptr};
std::atomic<void*> gLieFrameTickUser{nullptr};
std::atomic<JobFn> gPrePhysicsFrameTick{nullptr};
std::atomic<void*> gPrePhysicsFrameTickUser{nullptr};
std::atomic<JobFn> gPostPhysicsFrameTick{nullptr};
std::atomic<void*> gPostPhysicsFrameTickUser{nullptr};
std::atomic<JobFn> gInputFrameTick{nullptr};
std::atomic<void*> gInputFrameTickUser{nullptr};
std::atomic<uint32_t> gInputTickRuns{0};
std::atomic<uint8_t> gInputTickHost{0};  // 0=none 1=WM.FixedUpdate 2=SendWill fallback
std::atomic<int> gPumpPhase{static_cast<int>(PumpPhase::Bootstrap)};
std::atomic<DWORD> gLastBudgetLogMs{0};
std::atomic<bool> gWmHostLogged{false};
std::atomic<DWORD> gLastWmTickMs{0};  // any WM FixedUpdate/Update hook fire
std::atomic<DWORD> gLastWmFuMs{0};    // WM.FixedUpdate only (input tick host liveness)
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
// InterStage / leave-map（transit 且已解 login-freeze）：更快拒排队，别占满队等 2s。
constexpr DWORD kTransitIdleFailMs = 400;
// 同窗 InvokeAndWait 等待上限（auto_enter 仍在 freeze=1 时不走此帽）。
constexpr DWORD kTransitInvokeCapMs = 500;
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

// Anti-drift: Unity/明文名 → unique void() kind → RVA 末级。
// 故意不走 FindMethodCached（其 RVA-first 会在入口漂到同形 void() 时钩错函数）。
// outVia：name|plain|kind|rva|MISS — 安装日志与换版诊断用。
MethodInfoHead* ResolvePumpMi(void* klass, uint32_t rva, const char* nameHint, const char* slot,
                              const char** outVia) {
    if (outVia) *outVia = "MISS";
    if (!klass) return nullptr;
    if (nameHint && nameHint[0]) {
        if (MethodInfoHead* byName = FindMiByName(klass, nameHint, kShapeVoid0.arity)) {
            if (outVia) *outVia = "name";
            return byName;
        }
        // 再走 SSOT：hash 无、plain=nameHint（与上面 FindMiByName 等价兜底 + kind 校验）
        const auto mr =
            x::runtime::il2cpp_method::FindMethodResolved(klass, 0, kShapeVoid0, nameHint, nullptr);
        if (mr.method) {
            if (outVia) *outVia = x::runtime::il2cpp_method::PathName(mr.path);
            return reinterpret_cast<MethodInfoHead*>(mr.method);
        }
    }
    // 无可用名，或名未命中：仅当 void() 唯一时用 kind；否则才死钉 RVA。
    if (void* byKind = x::runtime::il2cpp_method::FindMethodByKind(klass, kShapeVoid0)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "ResolvePumpMi kind-fallback slot=%s (name miss)",
                 slot ? slot : "?");
        FailLogThrottled(buf);
        if (outVia) *outVia = "kind";
        return reinterpret_cast<MethodInfoHead*>(byKind);
    }
    if (rva) {
        if (MethodInfoHead* byRva = FindMiByRva(klass, rva)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "ResolvePumpMi RVA-fallback slot=%s rva=0x%X — remount: re-pin if hooks miss",
                     slot ? slot : "?", rva);
            FailLogThrottled(buf);
            if (outVia) *outVia = "rva";
            return byRva;
        }
    }
    return nullptr;
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

bool IsTransitQuiesce() {
    // 进图后卸图/InterStage：freeze 已散、transit 仍挡。登录大厅 freeze=1 不走此闸（auto_enter）。
    return managed_main::IsMapTransitBlocked() && !managed_main::IsLoginFrozen();
}

// Pop up to maxJobs (higher JobPrio first; same prio → earlier enqueue). Leftovers next tick.
// InterStage quiesce：本拍只抽 High，避免 Low/Normal FindAll 类活拖黑屏。
int DrainQueueBudget(int maxJobs) {
    if (maxJobs <= 0) return 0;
    const bool quiesce = IsTransitQuiesce();
    EnsureCs();
    int ran = 0;
    for (; ran < maxJobs;) {
        QueuedJob job{};
        EnterCriticalSection(&gCs);
        bool found = false;
        int best = -1;
        for (int i = 0; i < kQueueCap; ++i) {
            if (!gQueue[i].slotUsed) continue;
            if (quiesce && gQueue[i].prio < static_cast<int>(JobPrio::High)) continue;
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
            x::runtime::LogW("MainPump", "drain leftover — deferred to next tick (budget=%d quiesce=%d)",
                             gDrainBudget.load(std::memory_order_relaxed), quiesce ? 1 : 0);
        }
    }
    return ran;
}

void RunOneFrameTick(JobFn fn, void* user, const char* tag) {
    if (!fn) return;
    __try {
        fn(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("MainPump", "%s frame tick SEH", tag ? tag : "frame");
    }
}

void RunFrameTick() {
    RunOneFrameTick(gFrameTick.load(std::memory_order_acquire),
                    gFrameTickUser.load(std::memory_order_acquire), "primary");
    RunOneFrameTick(gAuxFrameTick.load(std::memory_order_acquire),
                    gAuxFrameTickUser.load(std::memory_order_acquire), "aux");
    RunOneFrameTick(gBinFrameTick.load(std::memory_order_acquire),
                    gBinFrameTickUser.load(std::memory_order_acquire), "bin");
    RunOneFrameTick(gLieFrameTick.load(std::memory_order_acquire),
                    gLieFrameTickUser.load(std::memory_order_acquire), "lie");
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

// 输入补写槽的首选宿主是否还活着。注意不能复用 WmHostsDrain()：那个只要 WM.Update
// 挂上就为真，而补写只在 WM.FixedUpdate 钩里跑 —— 那样会两头落空，补写彻底静默。
bool FixedUpdateTickLive() {
    if (!gMiWmFixedUpdate) return false;
    const DWORD last = gLastWmFuMs.load(std::memory_order_acquire);
    if (!last) return false;
    return (GetTickCount() - last) <= kWmIdleFallbackMs;
}

// host: 1=WM.FixedUpdate（物理前，赶在 CalcWalk 之前）2=SendWill 渲染帧（保底）。
void RunInputFrameTick(uint8_t host) {
    JobFn fn = gInputFrameTick.load(std::memory_order_acquire);
    if (!fn) return;
    gInputTickHost.store(host, std::memory_order_relaxed);
    gInputTickRuns.fetch_add(1, std::memory_order_relaxed);
    RunOneFrameTick(fn, gInputFrameTickUser.load(std::memory_order_acquire), "input");
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
    // 保底宿主：WM.FixedUpdate 没挂上 / 已 idle 时，输入补写改由渲染帧驱动。
    if (!FixedUpdateTickLive()) RunInputFrameTick(2);
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
    NotePumpThread();
    gLastWmFuMs.store(GetTickCount(), std::memory_order_release);
    // 输入补写排在最前：设备状态要先就位，本帧后续读输入的逻辑才看得到。
    RunInputFrameTick(1);
    // 拟人走路：在 WM.FixedUpdate orig 之前粘住 SetInput，避免 SendWill 后写赶不上 CalcWalk。
    RunOneFrameTick(gPrePhysicsFrameTick.load(std::memory_order_acquire),
                    gPrePhysicsFrameTickUser.load(std::memory_order_acquire), "prephys");
    auto orig = reinterpret_cast<FnUpdate>(gOrigWmFixedUpdate);
    if (orig) orig(self, methodInfo);
    RunOneFrameTick(gPostPhysicsFrameTick.load(std::memory_order_acquire),
                    gPostPhysicsFrameTickUser.load(std::memory_order_acquire), "postphys");
    NoteWmHookFire();
    if (WmHostsDrain()) {
        NoteHeartbeat();
        DrainAfterOrig();
    }
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
    // 下面的 FindClass / ResolveXxxKlass / ResolvePumpMi 全是 class_from_name、
    // class_get_methods 一类**查询** API，内部会隐式分配；而 Ensure() 跑在 worker 上，
    // 装泵前也没有主线程入口可用（鸡生蛋）。未登记线程上一旦触发回收就是
    // "Collecting from unknown thread" 直接 Abort（2026-08-12 23:47:21 实测命中）。
    // 四根针都到位后不再有查询，此时 no-op，不白付一次托管分配。
    const bool needLookup =
        !gMiSendWill || !gMiSceneUpdate || !gMiWmFixedUpdate || !gMiWmUpdate;
    x::runtime::il2cpp::GcThreadScope gcScope(needLookup);

    if (!BindApis()) {
        FailLogThrottled("BindApis fail (wait GameAssembly)");
        return false;
    }

    // Do NOT call il2cpp_runtime_class_init here — Ensure() runs from worker threads
    // and class_init allocates (GC fatal: Collecting from unknown thread).
    (void)gRuntimeClassInit;

    // 双挂：Canvas 过图可能停跳；SceneLogin.Update 在登录/加载期仍可能心跳。
    // Klass：Canvas 明文；SceneLogin / WM 走 shape（hash→field）。
    // MI：ResolvePumpMi = 名 → unique void() kind → RVA 末级（禁止 RVA-first）。
    const char* viaSend = "MISS";
    const char* viaScene = "MISS";
    const char* viaWmFu = "MISS";
    const char* viaWmUp = "MISS";
    if (!gMiSendWill) {
        void* klassCanvas = FindClass("UnityEngine", "Canvas");
        MethodInfoHead* miSend = ResolvePumpMi(klassCanvas, kRvaSendWillRenderCanvases,
                                               "SendWillRenderCanvases", "Canvas.SendWill", &viaSend);
        TryInstallSendWill(miSend);
    } else {
        viaSend = "kept";
    }
    if (!gMiSceneUpdate) {
        void* klassSl = x::runtime::il2cpp_shape::ResolveSceneLoginKlass();
        MethodInfoHead* miUp =
            ResolvePumpMi(klassSl, kRvaSceneLoginUpdate, "Update", "SceneLogin.Update", &viaScene);
        if (!klassSl) {
            FailLogThrottled("SceneLogin klass miss (hash/shape) — Canvas-only pump");
        } else if (!miUp) {
            FailLogThrottled("SceneLogin.Update MI miss — Canvas-only pump");
        }
        TryInstallSceneUpdate(miUp);
    } else {
        viaScene = "kept";
    }

    // WM InMap host — klass may appear after login; retry every Ensure.
    if (!gMiWmFixedUpdate || !gMiWmUpdate) {
        void* klassWm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
        if (klassWm) {
            if (!gMiWmFixedUpdate) {
                MethodInfoHead* miFu = ResolvePumpMi(klassWm, kRvaWorldManagerFixedUpdate,
                                                     "FixedUpdate", "WM.FixedUpdate", &viaWmFu);
                if (!TryInstallWmFixedUpdate(miFu) && !gMiWmFixedUpdate) {
                    FailLogThrottled("WM.FixedUpdate MI miss — InMap keeps SendWill Drain");
                }
            } else {
                viaWmFu = "kept";
            }
            if (!gMiWmUpdate) {
                MethodInfoHead* miUp =
                    ResolvePumpMi(klassWm, kRvaWorldManagerUpdate, "Update", "WM.Update", &viaWmUp);
                (void)TryInstallWmUpdate(miUp);
            } else {
                viaWmUp = "kept";
            }
        }
    } else {
        viaWmFu = "kept";
        viaWmUp = "kept";
    }

    if (!gMiSendWill && !gMiSceneUpdate) {
        FailLogThrottled("pump MI miss (Canvas+SceneLogin) — wait, no direct invoke");
        return false;
    }
    // 不伪造心跳：真实 tick 前 InvokeAndWait 拒绝排队。
    const bool first = !gPumpInstalled.exchange(true);
    if (first) {
        x::runtime::LogI("MainPump",
                         "installed sendWill=%d(%s) sceneUpdate=%d(%s) wmFU=%d(%s) wmUp=%d(%s) "
                         "drainBudget=%d (queueCap=%d, wait real tick)",
                         gMiSendWill ? 1 : 0, viaSend, gMiSceneUpdate ? 1 : 0, viaScene,
                         gMiWmFixedUpdate ? 1 : 0, viaWmFu, gMiWmUpdate ? 1 : 0, viaWmUp,
                         gDrainBudget.load(std::memory_order_relaxed), kQueueCap);
    } else if (WmHostInstalled()) {
        if (!gWmHostLogged.exchange(true)) {
            x::runtime::LogI("MainPump", "WM drain host ready fu=%d(%s) up=%d(%s)",
                             gMiWmFixedUpdate ? 1 : 0, viaWmFu, gMiWmUpdate ? 1 : 0, viaWmUp);
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

void SetAuxFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gAuxFrameTick.store(nullptr, std::memory_order_release);
        gAuxFrameTickUser.store(nullptr, std::memory_order_release);
        return;
    }
    gAuxFrameTickUser.store(user, std::memory_order_release);
    gAuxFrameTick.store(fn, std::memory_order_release);
}

void SetBinFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gBinFrameTick.store(nullptr, std::memory_order_release);
        gBinFrameTickUser.store(nullptr, std::memory_order_release);
        return;
    }
    gBinFrameTickUser.store(user, std::memory_order_release);
    gBinFrameTick.store(fn, std::memory_order_release);
}

void SetLieFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gLieFrameTick.store(nullptr, std::memory_order_release);
        gLieFrameTickUser.store(nullptr, std::memory_order_release);
        return;
    }
    gLieFrameTickUser.store(user, std::memory_order_release);
    gLieFrameTick.store(fn, std::memory_order_release);
}

void SetPrePhysicsFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gPrePhysicsFrameTick.store(nullptr, std::memory_order_release);
        gPrePhysicsFrameTickUser.store(nullptr, std::memory_order_release);
        return;
    }
    gPrePhysicsFrameTickUser.store(user, std::memory_order_release);
    gPrePhysicsFrameTick.store(fn, std::memory_order_release);
}

void SetInputFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gInputFrameTick.store(nullptr, std::memory_order_release);
        gInputFrameTickUser.store(nullptr, std::memory_order_release);
        gInputTickHost.store(0, std::memory_order_relaxed);
        return;
    }
    gInputFrameTickUser.store(user, std::memory_order_release);
    gInputFrameTick.store(fn, std::memory_order_release);
}

uint32_t InputFrameTickRuns() { return gInputTickRuns.load(std::memory_order_relaxed); }

uint8_t InputFrameTickHost() { return gInputTickHost.load(std::memory_order_relaxed); }

void SetPostPhysicsFrameTick(JobFn fn, void* user) {
    if (!fn) {
        gPostPhysicsFrameTick.store(nullptr, std::memory_order_release);
        gPostPhysicsFrameTickUser.store(nullptr, std::memory_order_release);
        return;
    }
    gPostPhysicsFrameTickUser.store(user, std::memory_order_release);
    gPostPhysicsFrameTick.store(fn, std::memory_order_release);
}

bool IsDirectMode() {
    // Direct mode removed: it caused GC "Collecting from unknown thread".
    return false;
}

bool IsOnPumpThread() {
    const DWORD tid = gPumpTid.load(std::memory_order_acquire);
    return tid != 0 && tid == GetCurrentThreadId();
}

DWORD PumpThreadId() { return gPumpTid.load(std::memory_order_acquire); }

int QueuedJobCount() {
    const int n = gQueuedCount.load(std::memory_order_relaxed);
    return n < 0 ? 0 : n;
}

bool IsCongested() {
    // InterStage / 卸图：让打怪/吸物等认背压直接让路（登录 freeze 期不触发，护 auto_enter）。
    if (IsTransitQuiesce()) return true;
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

    const bool transit = managed_main::IsMapTransitBlocked();
    const bool freeze = managed_main::IsLoginFrozen();
    const bool quiesce = transit && !freeze;  // = IsTransitQuiesce；此处内联免跨层歧义

    // 玩法 Low（吸物等）：冻屏/卸图一律拒排，别占 kQueueCap。
    if (prio == JobPrio::Low && (transit || freeze)) {
        FailLogThrottled("reject Low (transit/freeze)");
        return false;
    }
    // InterStage：Normal 也拒（出刀/重绑）；High 留给换频/系统短探。
    if (quiesce && prio != JobPrio::High) {
        FailLogThrottled("reject non-High (interstage quiesce)");
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD lastHb = gLastHeartbeatMs.load(std::memory_order_acquire);
    const DWORD idleLimit = quiesce ? kTransitIdleFailMs : kPumpIdleFailMs;
    // 从未真实 tick（含刚装泵）：禁止排队，避免 5e3768 式 job timeout / 冻死。
    if (!lastHb || now - lastHb > idleLimit) {
        FailLogThrottled(lastHb ? (quiesce ? "pump idle (interstage/load — fast fail)"
                                           : "pump idle (no drain-host tick — load/freeze)")
                                : "pump not ticking yet (wait first drain-host tick)");
        return false;
    }

    if (quiesce && timeoutMs > kTransitInvokeCapMs) timeoutMs = kTransitInvokeCapMs;

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
        FailLogThrottled(quiesce ? "job timeout (interstage)" : "job timeout");
        return false;
    }
    CloseHandle(done);
    return true;
}

void Shutdown() {
    gFrameTick.store(nullptr, std::memory_order_release);
    gFrameTickUser.store(nullptr, std::memory_order_release);
    gAuxFrameTick.store(nullptr, std::memory_order_release);
    gAuxFrameTickUser.store(nullptr, std::memory_order_release);
    gBinFrameTick.store(nullptr, std::memory_order_release);
    gBinFrameTickUser.store(nullptr, std::memory_order_release);
    gLieFrameTick.store(nullptr, std::memory_order_release);
    gLieFrameTickUser.store(nullptr, std::memory_order_release);
    gPrePhysicsFrameTick.store(nullptr, std::memory_order_release);
    gPrePhysicsFrameTickUser.store(nullptr, std::memory_order_release);
    gPostPhysicsFrameTick.store(nullptr, std::memory_order_release);
    gPostPhysicsFrameTickUser.store(nullptr, std::memory_order_release);
    gInputFrameTick.store(nullptr, std::memory_order_release);
    gInputFrameTickUser.store(nullptr, std::memory_order_release);
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
