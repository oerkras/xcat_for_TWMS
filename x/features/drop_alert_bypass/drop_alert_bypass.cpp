// TWMS Classic — drop_alert_bypass.
//
// Root cause (BIN): callers use direct `call CanPerformAction` (E8×8), so
// MethodInfo swap never runs. Real drop gate calls thin IsAlertMode
// (b36db157$$c61ba922 @0x124A3C0) which reads LocalUser+0x114 vs (0x41B001EF+global).
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

// User 父类 b36db157…. 短 IsAlertMode（CanPerformAction 真 callee）
// IDA 2026-08-04: cmp [rcx+114h], eax / setnle — 常量 0x41B001EF（旧 0x14859CC3）
constexpr uint32_t kRvaIsAlertMode = 0x124A3C0;  // remounted 2026-08-04

// Secondary: MethodInfo on DragManager.CanPerformAction (rarely hit; keep for MI callers)
constexpr char kDragManagerClass[] =
    "f3c1e2949898302937dc5a07f04d4ef3b80e35bbc80f60a9510eda368888253";
constexpr uint32_t kRvaCanPerformAction = 0x4C38F0;  // remounted 2026-08-04
constexpr char kUserAlertClass[] =
    "b36db157c954de56d1658f10eb3edcbce83710b40064dc5840287ccad9a80fa";
constexpr char kHashIsAlertMode[] =
    "c61ba92227f1dc80c43bb5d5d044d27c15236883570aa6ba095d7a670b7b54e";
constexpr char kHashCanPerformAction[] =
    "b4c6efa9c0cb728b5eba8f06154405752c6e39320cb2e688be57f39975d8fd5";
// LocalUser alert stamp：hash → field_get_offset（dump fallback 0x114）
constexpr char kHashAlertAt[] =
    "af55bdf7678948c178713fa3706164f9976891127d115b8ba7350a416511a71";
constexpr size_t kFbAlertAt = 0x114;
size_t gOffAlertAt = kFbAlertAt;
#define kOffAlertAt (gOffAlertAt)
bool gAlertFieldTried = false;

constexpr DWORD kTickMsOn = 32;   // dense like 枫星 gate4 maintain
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kInstallRetryMs = 5000;
constexpr DWORD kLogClearMs = 3000;

// 短 IsAlertMode 机码指纹（防版本漂到 CFF 兄弟 / 字段改偏）
//   B8 EF 01 B0 41          mov eax, 0x41B001EF
//   03 05 xx xx xx xx       add eax, [rip+disp]
//   39 81 14 01 00 00       cmp [rcx+0x114], eax
//   0F 9F C0 C3             setnle al / ret
constexpr uint8_t kAlertMovImm[] = {0xB8, 0xEF, 0x01, 0xB0, 0x41};
constexpr uint8_t kAlertCmpOff114[] = {0x39, 0x81, 0x14, 0x01, 0x00, 0x00};
constexpr size_t kAlertShapeScan = 24;

enum class AlertShape : uint8_t { Unknown = 0, Ok = 1, BadConst = 2, BadOff = 3, Unreadable = 4 };

std::atomic<AlertShape> gAlertShape{AlertShape::Unknown};
std::atomic<bool> gShapeLogged{false};

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
                          const char* plain, const char* hash,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
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

const void* AlertModeFnPtr() {
    if (gMiIsAlertMode) {
        __try {
            if (gMiIsAlertMode->methodPointer) return gMiIsAlertMode->methodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return AtRva<const void*>(kRvaIsAlertMode);
}

AlertShape ProbeAlertShape(const void* fn) {
    if (!fn) return AlertShape::Unreadable;
    uint8_t buf[kAlertShapeScan]{};
    __try {
        memcpy(buf, fn, sizeof(buf));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return AlertShape::Unreadable;
    }
    if (memcmp(buf, kAlertMovImm, sizeof(kAlertMovImm)) != 0) return AlertShape::BadConst;
    bool foundOff = false;
    for (size_t i = 0; i + sizeof(kAlertCmpOff114) <= sizeof(buf); ++i) {
        if (memcmp(buf + i, kAlertCmpOff114, sizeof(kAlertCmpOff114)) == 0) {
            foundOff = true;
            break;
        }
    }
    return foundOff ? AlertShape::Ok : AlertShape::BadOff;
}

AlertShape RefreshAlertShape(bool forceLog) {
    const AlertShape s = ProbeAlertShape(AlertModeFnPtr());
    const AlertShape prev = gAlertShape.exchange(s);
    if (forceLog || s != prev || (!gShapeLogged.load() && s != AlertShape::Unknown)) {
        gShapeLogged.store(true);
        if (s == AlertShape::Ok) {
            x::runtime::LogI("DropAlert", "shape OK thin IsAlertMode +0x114 (rva=0x%X)",
                             kRvaIsAlertMode);
        } else {
            x::runtime::LogW("DropAlert",
                             "shape FAIL code=%u — refuse field clear (expect mov 14859CC3 + "
                             "cmp [rcx+0x114]) rva=0x%X",
                             static_cast<unsigned>(s), kRvaIsAlertMode);
        }
    }
    return s;
}

bool AlertFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, hash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (off >= 0x10 && off < 0x800) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}

void EnsureAlertFieldOff() {
    if (gAlertFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gAlertFieldTried = true;
    void* klass = x::runtime::il2cpp::FindClass("", kUserAlertClass);
    if (!klass) klass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    const int hits = AlertFieldOffHit(klass, kHashAlertAt, kFbAlertAt, &gOffAlertAt) ? 1 : 0;
    x::runtime::LogI("DropAlert", "alert field path=%s hits=%d/1 off=0x%zX",
                     hits ? "meta" : "fallback", hits, gOffAlertAt);
}

bool EnsureIsAlertModeMi() {
    if (gMiIsAlertMode && gMiIsAlertMode->methodPointer) return true;
    void* userAlert = x::runtime::il2cpp::FindClass("", kUserAlertClass);
    if (!userAlert) userAlert = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (!userAlert) return false;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kAl{0, TypeKind::Bool, false, true, {}};
    ResolvePath path = ResolvePath::Miss;
    gMiIsAlertMode =
        ResolveMi(userAlert, kRvaIsAlertMode, kAl, "IsAlertMode", kHashIsAlertMode, &path);
    static bool sLogged = false;
    if (!sLogged && gMiIsAlertMode) {
        sLogged = true;
        x::runtime::LogI("DropAlert", "IsAlertMode method path=%s",
                         x::runtime::il2cpp_method::PathName(path));
    }
    return gMiIsAlertMode && gMiIsAlertMode->methodPointer;
}

void ReportDropAlertLamp() {
    (void)EnsureIsAlertModeMi();
    const AlertShape shape = RefreshAlertShape(false);
    if (shape == AlertShape::BadConst || shape == AlertShape::BadOff ||
        shape == AlertShape::Unreadable) {
        const char* d = shape == AlertShape::BadOff     ? "MISS off!=114"
                        : shape == AlertShape::BadConst ? "MISS const"
                                                        : "MISS unreadable";
        x::runtime::anchor_lamps::Set("DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     d);
        return;
    }
    if (shape == AlertShape::Ok && gMiIsAlertMode) {
        x::runtime::anchor_lamps::Set(
            "DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Ok,
            gInstalled.load() ? "shape+114+secMI" : "shape+114");
        return;
    }
    if (shape == AlertShape::Ok) {
        x::runtime::anchor_lamps::Set("DropAlert",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                     "shape ok, MI late");
        return;
    }
    if (gClearHits.load() > 0) {
        x::runtime::anchor_lamps::Set("DropAlert",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                     "field only");
    } else {
        x::runtime::anchor_lamps::Set("DropAlert",
                                     x::runtime::anchor_lamps::AnchorLampCode::Unknown, "pending");
    }
}

bool TryInstallMi() {
    if (gInstalled.load() && HookStillOurs(gMi) && gOrig) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    (void)EnsureIsAlertModeMi();
    if (!gKlass) {
        gKlass = x::runtime::il2cpp::FindClass("", kDragManagerClass);
        if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("", "DragManager");
    }
    if (!gKlass) return false;
    x::runtime::il2cpp::RuntimeClassInit(gKlass);
    MethodInfoHead* mi = nullptr;
    x::runtime::il2cpp_method::ResolvePath canPath =
        x::runtime::il2cpp_method::ResolvePath::Miss;
    {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kCan{1, TypeKind::Bool, true, true, {TypeKind::Bool}};
        mi = ResolveMi(gKlass, kRvaCanPerformAction, kCan, "CanPerformAction",
                       kHashCanPerformAction, &canPath);
    }
    if (!mi) return false;
    static bool sCanLogged = false;
    if (!sCanLogged) {
        sCanLogged = true;
        x::runtime::LogI("DropAlert", "CanPerformAction method path=%s",
                         x::runtime::il2cpp_method::PathName(canPath));
    }
    if (HookStillOurs(mi)) {
        if (!gOrig) return false;
        gMi = mi;
        gInstalled.store(true);
        ReportDropAlertLamp();
        return true;
    }
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&Hook_CanPerformAction), &orig) || !orig)
        return false;
    gMi = mi;
    gOrig = reinterpret_cast<FnCanPerformAction>(orig);
    gInstalled.store(true);
    x::runtime::LogI("DropAlert", "MI secondary installed (direct-call paths use data-plane)");
    ReportDropAlertLamp();
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
// Shape gate: 机码不像「mov 14859CC3 + cmp [rcx+0x114]」则拒绝写字段（防漂）。
bool MaintainAlertField(DWORD now) {
    ports::player_combat::CombatCtx ctx{};
    if (!ports::player_combat::QueryCombatCtx(ctx) || !ctx.localUser) return false;

    EnsureAlertFieldOff();
    (void)EnsureIsAlertModeMi();
    const AlertShape shape = RefreshAlertShape(false);
    if (shape != AlertShape::Ok) {
        if (!gLastClearLog || now - gLastClearLog >= kLogClearMs) {
            gLastClearLog = now;
            ReportDropAlertLamp();
        }
        return false;
    }

    int before = 0;
    __try {
        before = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx.localUser) + kOffAlertAt);
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx.localUser) + kOffAlertAt) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    bool still = false;
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
                         "field clear +0x114 was=%d stillAlert=%d miHits=%u clears=%u alertMi=%d "
                         "shape=%u",
                         before, still ? 1 : 0, gMiHits.load(), gClearHits.load(),
                         gMiIsAlertMode ? 1 : 0, static_cast<unsigned>(shape));
        ReportDropAlertLamp();
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
    EnsureAlertFieldOff();
    x::runtime::LogI("DropAlert",
                     "init — primary: LocalUser+0x%zX clear; IsAlertMode rva=0x%X; MI rva=0x%X "
                     "secondary",
                     gOffAlertAt, kRvaIsAlertMode, kRvaCanPerformAction);
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
