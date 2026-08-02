// TWMS Classic — drop_alert_bypass.
//
// Root cause (BIN): callers use direct `call CanPerformAction` (E8×8), so
// MethodInfo swap never runs. Real drop gate calls thin IsAlertMode
// (fb641b6e$$f74b6320 @0x12405C0) which reads LocalUser+0x114 vs (0x14859CC3+global).
// 2026-08-03 remount mis-pinned the sibling CFF method @0x1242770 (+0xC8) — fixed.
// While enabled, data-plane clear +0x114 — drop opens AND client alert suppressed.
// No GA .text patch; no HWBP slot.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "drop_alert_bypass.h"

#include "../ports/player_combat_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/anchor_lamps.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::drop_alert_bypass {
namespace {

using x::runtime::il2cpp::AtRva;

// User 父类 fb641b6e…. 短 IsAlertMode（CanPerformAction 真 callee）
// IDA: cmp [rcx+114h], eax / setnle — 常量 0x14859CC3（与旧版同形）
constexpr uint32_t kRvaIsAlertMode = 0x12405C0;  // fixed 2026-08-03: was wrongly 0x1242770
constexpr size_t kOffAlertAt = 0x114;            // fixed 2026-08-03: was wrongly 0xC8

// Secondary: MethodInfo on DragManager.CanPerformAction (rarely hit; keep for MI callers)
constexpr char kDragManagerClass[] =
    "cd4b127b985202de5876211cf31d5940c9e3d6e6ce21fadc2005a687df9cd52";
constexpr uint32_t kRvaCanPerformAction = 0x4C2C40;
constexpr char kUserAlertClass[] =
    "fb641b6ed2c6220bf18c3f3c2f8a20b4f3e53702c3307c50c75c85dd2a2ef06";
constexpr char kHashIsAlertMode[] =
    "f74b63202db3140d4b5918a8fd68733266491ef9287abfb0125260b485a10f0";
constexpr char kHashCanPerformAction[] =
    "abdf2916ad4d47fa693d3e2f68f914bdd544ffaeb0501f4f35d9215abb6a8af";

constexpr DWORD kTickMsOn = 32;   // dense like 枫星 gate4 maintain
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kInstallRetryMs = 5000;
constexpr DWORD kLogClearMs = 3000;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

using FnCanPerformAction = uint8_t (*)(void* self, uint8_t bCheckAlert, void* methodInfo);
using FnIsAlertMode = uint8_t (*)(void* self, const void* methodInfo);

std::atomic<bool> gDesired{true};
std::atomic<bool> gInstalled{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gMiHits{0};
std::atomic<uint32_t> gClearHits{0};

MethodInfoHead* gMi = nullptr;
MethodInfoHead* gMiIsAlertMode = nullptr;
FnCanPerformAction gOrig = nullptr;
void* gKlass = nullptr;
DWORD gLastInstallTry = 0;
DWORD gLastClearLog = 0;

uint8_t Hook_CanPerformAction(void* self, uint8_t bCheckAlert, void* methodInfo) {
    gMiHits.fetch_add(1, std::memory_order_relaxed);
    FnCanPerformAction orig = gOrig;
    if (!orig) return 0;
    if (gDesired.load(std::memory_order_relaxed)) bCheckAlert = 0;
    __try {
        return orig(self, bCheckAlert, methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods || !e.ga) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(e.ga) + rva;
    const uintptr_t hook = reinterpret_cast<uintptr_t>(&Hook_CanPerformAction);
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = e.classGetMethods(klass, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            void* mp = nullptr;
            __try {
                mp = mi->methodPointer;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            const uintptr_t p = reinterpret_cast<uintptr_t>(mp);
            if (p == want || p == hook) return mi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
    }
    if (mi && mi->methodPointer) return mi;
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* raw = e.classGetMethods(cur, &iter);
                if (!raw) break;
                const char* nm = e.methodGetName(raw);
                if (nm && strcmp(nm, name) == 0) {
                    mi = reinterpret_cast<MethodInfoHead*>(raw);
                    if (mi && mi->methodPointer) return mi;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (plain) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plain, shape.arity)) return mi;
    }
    if (hash) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hash, shape.arity)) return mi;
    }
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("DropAlert", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plain ? plain : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig) return false;
    if (orig == hook) {
        *outOrig = gOrig ? reinterpret_cast<void*>(gOrig) : nullptr;
        return gOrig != nullptr;
    }
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

bool HookStillOurs(MethodInfoHead* mi) {
    if (!mi) return false;
    __try {
        return mi->methodPointer == reinterpret_cast<void*>(&Hook_CanPerformAction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryInstallMi() {
    if (gInstalled.load() && HookStillOurs(gMi) && gOrig) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    if (!gKlass) {
        gKlass = x::runtime::il2cpp::FindClass("", kDragManagerClass);
        if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("", "DragManager");
    }
    if (!gKlass) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.runtimeClassInit) {
        __try {
            e.runtimeClassInit(gKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    MethodInfoHead* mi = nullptr;
    {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kCan{1, TypeKind::Bool, true, true, {TypeKind::Bool}};
        mi = ResolveMi(gKlass, kRvaCanPerformAction, kCan, "CanPerformAction",
                       kHashCanPerformAction);
    }
    if (!mi) return false;
    if (HookStillOurs(mi)) {
        if (!gOrig) return false;
        gMi = mi;
        gInstalled.store(true);
        x::runtime::anchor_lamps::Set(
            "DropAlert",
            gMiIsAlertMode ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                           : x::runtime::anchor_lamps::AnchorLampCode::Degraded,
            gMiIsAlertMode ? "field+MI" : "field only");
        return true;
    }
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&Hook_CanPerformAction), &orig) || !orig)
        return false;
    gMi = mi;
    gOrig = reinterpret_cast<FnCanPerformAction>(orig);
    gInstalled.store(true);
    x::runtime::LogI("DropAlert", "MI secondary installed (direct-call paths use data-plane)");
    x::runtime::anchor_lamps::Set(
        "DropAlert",
        gMiIsAlertMode ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                       : x::runtime::anchor_lamps::AnchorLampCode::Degraded,
        gMiIsAlertMode ? "field+MI" : "field only");
    return true;
}

void UninstallMi() {
    if (!gInstalled.load() && !gMi) return;
    MethodInfoHead* mi = gMi;
    FnCanPerformAction orig = gOrig;
    if (mi && orig) RestoreMethodInfo(mi, reinterpret_cast<void*>(orig));
    gInstalled.store(false);
    gMi = nullptr;
    gOrig = nullptr;
}

// Primary path: expire LocalUser+0x114 so thin IsAlertMode returns false.
bool MaintainAlertField(DWORD now) {
    ports::player_combat::CombatCtx ctx{};
    if (!ports::player_combat::QueryCombatCtx(ctx) || !ctx.localUser) return false;

    int before = 0;
    __try {
        before = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx.localUser) + kOffAlertAt);
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx.localUser) + kOffAlertAt) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    bool still = false;
    if (!gMiIsAlertMode) {
        void* userAlert = x::runtime::il2cpp::FindClass("", kUserAlertClass);
        if (!userAlert) userAlert = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
        if (userAlert) {
            using x::runtime::il2cpp_method::MethodShape;
            using x::runtime::il2cpp_method::TypeKind;
            constexpr MethodShape kAl{0, TypeKind::Bool, false, true, {}};
            gMiIsAlertMode =
                ResolveMi(userAlert, kRvaIsAlertMode, kAl, "IsAlertMode", kHashIsAlertMode);
        }
    }
    auto fn = FnFromMi<FnIsAlertMode>(gMiIsAlertMode, kRvaIsAlertMode);
    if (fn) {
        __try {
            still = fn(ctx.localUser, gMiIsAlertMode) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            still = false;
        }
    }

    gClearHits.fetch_add(1, std::memory_order_relaxed);
    if (!gLastClearLog || now - gLastClearLog >= kLogClearMs) {
        gLastClearLog = now;
        x::runtime::LogI("DropAlert",
                         "field clear +0x114 was=%d stillAlert=%d miHits=%u clears=%u", before,
                         still ? 1 : 0, gMiHits.load(), gClearHits.load());
    }
    return !still || before != 0;
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("DropAlert",
                     "worker start — data-plane LocalUser+0x114 (IsAlertMode); MI secondary");
    for (int i = 0; i < 400 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll"); ++i)
        Sleep(50);
    Tick(GetTickCount());
    while (!gStop.load()) {
        const bool on = gDesired.load();
        Tick(GetTickCount());
        Sleep(on ? kTickMsOn : kTickMsOff);
    }
    UninstallMi();
    x::runtime::LogI("DropAlert", "worker stop miHits=%u clears=%u", gMiHits.load(),
                     gClearHits.load());
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(true);
    x::runtime::LogI("DropAlert",
                     "init — primary: LocalUser+0x114 clear; IsAlertMode rva=0x%X; MI rva=0x%X "
                     "secondary",
                     kRvaIsAlertMode, kRvaCanPerformAction);
}

void Shutdown() {
    StopWorker();
    UninstallMi();
}

void StartWorker() {
    if (gWorker.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) gWorker.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gWorker.exchange(nullptr);
    if (th) CloseHandle(th);
}

void SetEnabled(bool on) {
    const bool prev = gDesired.exchange(on);
    if (prev != on) {
        x::runtime::LogI("DropAlert", "SetEnabled %d", on ? 1 : 0);
    }
}

bool IsEnabled() { return gDesired.load(); }

bool IsInstalled() {
    // "Installed" for UI/diag = data-plane path ready (GA + LocalUser resolvable) or MI on.
    return gClearHits.load() > 0 || (gInstalled.load() && gOrig != nullptr);
}

void Tick(DWORD now) {
    if (!gDesired.load()) return;

    (void)MaintainAlertField(now);

    // Best-effort secondary MI (most drop paths never hit it).
    if (!(gInstalled.load() && HookStillOurs(gMi) && gOrig)) {
        if (!gLastInstallTry || now - gLastInstallTry >= kInstallRetryMs) {
            gLastInstallTry = now;
            (void)TryInstallMi();
        }
    }
}

}  // namespace x::features::drop_alert_bypass
