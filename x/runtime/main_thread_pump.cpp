// Shared main-thread pump — Classic TWMS (MethodInfo, no GA .text E9).
// NEVER run managed/GC work on a worker "direct invoke" path — Unity fatals with
// "Fatal error in GC / Collecting from unknown thread".
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "main_thread_pump.h"

#include "il2cpp_bind.h"
#include "log.h"

#include <atomic>
#include <cstring>

namespace x::runtime::main_thread {
namespace {

constexpr int kQueueCap = 8;
// Dump Address for Canvas.SendWillRenderCanvases (script.json); used only to match MethodInfo.methodPointer.
constexpr uint32_t kRvaSendWillRenderCanvases = 0x5239AB0;  // remapped 2026-08-03
constexpr char kClassSceneLogin[] =
    "af29cad27898e88172980382e66fca5917dbaaf8edfcad5e70ebb98371f0982";

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
};

HMODULE gGA = nullptr;
uintptr_t gGaBase = 0;
FnClassGetMethodFromName gClassGetMethodFromName = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnMethodGetName gMethodGetName = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;

MethodInfoHead* gMiHooked = nullptr;
void* gOrigHooked = nullptr;
bool gHookIsUpdate = false;  // SceneLogin.Update vs Canvas.SendWill

std::atomic<bool> gPumpInstalled{false};
std::atomic<bool> gInPump{false};
std::atomic<bool> gApisBound{false};
std::atomic<DWORD> gLastFailLogMs{0};
std::atomic<DWORD> gLastHeartbeatMs{0};  // HookSendWill/Update last fire
std::atomic<JobFn> gFrameTick{nullptr};
std::atomic<void*> gFrameTickUser{nullptr};

// If Canvas.SendWill is not ticking (login/load freeze), refuse to park jobs for
// the full timeout — otherwise N workers fill kQueueCap and log "queue full".
constexpr DWORD kPumpIdleFailMs = 2000;

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

void RunOne(QueuedJob& j) {
    if (!j.fn) return;
    __try {
        j.fn(j.user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("MainPump", "job SEH");
    }
    if (j.done) SetEvent(j.done);
}

void DrainQueue() {
    EnsureCs();
    for (;;) {
        QueuedJob job{};
        EnterCriticalSection(&gCs);
        bool found = false;
        for (int i = 0; i < kQueueCap; ++i) {
            if (!gQueue[i].slotUsed) continue;
            job = gQueue[i];
            gQueue[i] = {};
            found = true;
            break;
        }
        LeaveCriticalSection(&gCs);
        if (!found) break;
        RunOne(job);
    }
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

void NoteHeartbeat() { gLastHeartbeatMs.store(GetTickCount(), std::memory_order_release); }

void HookSendWill(const void* methodInfo) {
    NoteHeartbeat();
    if (!gInPump.exchange(true)) {
        DrainQueue();
        gInPump.store(false);
    }
    // Pin both sides of orig — User.Update may run before or after SendWill.
    RunFrameTick();
    auto orig = reinterpret_cast<FnSendWill>(gOrigHooked);
    if (orig) orig(methodInfo);
    RunFrameTick();
}

void HookUpdate(void* self, const void* methodInfo) {
    NoteHeartbeat();
    if (!gInPump.exchange(true)) {
        DrainQueue();
        gInPump.store(false);
    }
    RunFrameTick();
    auto orig = reinterpret_cast<FnUpdate>(gOrigHooked);
    if (orig) orig(self, methodInfo);
    RunFrameTick();
}

bool TryInstallMi(MethodInfoHead* mi, void* hook, bool isUpdate, const char* tag) {
    if (!mi) return false;
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, hook, &orig)) return false;
    gMiHooked = mi;
    gOrigHooked = orig;
    gHookIsUpdate = isUpdate;
    NoteHeartbeat();  // grace until first real tick
    gPumpInstalled.store(true);
    x::runtime::LogI("MainPump", "installed %s MI=%p orig=%p update=%d", tag, (void*)mi, orig,
                     isUpdate ? 1 : 0);
    return true;
}

bool InstallPump() {
    if (gPumpInstalled.load()) return true;
    if (!BindApis()) {
        FailLogThrottled("BindApis fail (wait GameAssembly)");
        return false;
    }

    // Do NOT call il2cpp_runtime_class_init here — Ensure() runs from worker threads
    // and class_init allocates (GC fatal: Collecting from unknown thread).
    (void)gRuntimeClassInit;
    void* klassCanvas = FindClass("UnityEngine", "Canvas");
    MethodInfoHead* miSend = nullptr;
    if (klassCanvas) {
        miSend = FindMiByName(klassCanvas, "SendWillRenderCanvases", 0);
        if (!miSend) miSend = FindMiByRva(klassCanvas, kRvaSendWillRenderCanvases);
    }
    if (TryInstallMi(miSend, reinterpret_cast<void*>(&HookSendWill), false, "Canvas.SendWill"))
        return true;

    void* klassSl = FindClass("", kClassSceneLogin);
    if (!klassSl) klassSl = FindClass("", "SceneLogin");
    MethodInfoHead* miUp = nullptr;
    if (klassSl) {
        miUp = FindMiByName(klassSl, "Update", 0);
        if (!miUp) miUp = FindMiByRva(klassSl, 0xC007E0);
    }
    if (TryInstallMi(miUp, reinterpret_cast<void*>(&HookUpdate), true, "SceneLogin.Update"))
        return true;

    FailLogThrottled("pump MI miss (Canvas+SceneLogin) — wait, no direct invoke");
    return false;
}

}  // namespace

bool Ensure() {
    EnsureCs();
    return InstallPump();
}

bool IsInstalled() { return gPumpInstalled.load(); }

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

bool InvokeAndWait(JobFn fn, void* user, DWORD timeoutMs) {
    if (!fn) return false;
    if (!Ensure()) return false;
    if (!gPumpInstalled.load()) return false;

    const DWORD now = GetTickCount();
    const DWORD lastHb = gLastHeartbeatMs.load(std::memory_order_acquire);
    if (lastHb && now - lastHb > kPumpIdleFailMs) {
        FailLogThrottled("pump idle (no SendWill/Update — load/freeze)");
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
    SetFrameTick(nullptr, nullptr);
    EnsureCs();
    EnterCriticalSection(&gCs);
    for (int i = 0; i < kQueueCap; ++i) {
        if (gQueue[i].slotUsed && gQueue[i].done) SetEvent(gQueue[i].done);
        gQueue[i] = {};
    }
    LeaveCriticalSection(&gCs);

    if (gPumpInstalled.exchange(false)) {
        if (gMiHooked && gOrigHooked) RestoreMethodInfo(gMiHooked, gOrigHooked);
        gMiHooked = nullptr;
        gOrigHooked = nullptr;
        gHookIsUpdate = false;
        x::runtime::LogI("MainPump", "uninstalled");
    }
    gApisBound.store(false);
    gGA = nullptr;
    gGaBase = 0;
}

}  // namespace x::runtime::main_thread
