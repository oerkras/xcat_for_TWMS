// TWMS Classic — drop_alert_bypass.
//
// Root cause (BIN): callers use direct `call CanPerformAction` (E8×N), so
// MethodInfo swap never runs. Real drop gate calls thin IsAlertMode which is:
//   mov eax,imm ; add eax,[global] ; cmp [rcx+0x118],eax ; setnle
// File-time imm+global==0 ⇒ stamp>0. That global has a single xref.
//
// v3 primary: rewrite that dword so imm+global==INT_MAX → IsAlertMode always
// false (出刀怎么刷 +0x118 都无空窗). Restore on disable. No GA .text / no HWBP.
// Secondary: CanPerformAction MethodInfo (rarely hit).
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

// UserBase 父类. 短 IsAlertMode（CanPerformAction 真 callee）
// IDA: mov eax,imm; add eax,[rip]; cmp [rcx+0x118],eax; setnle
constexpr uint32_t kRvaIsAlertMode = 0x1250420;

// Secondary: MethodInfo on DragManager.CanPerformAction (rarely hit; keep for MI callers)
constexpr char kDragManagerClass[] =
    "be43bc57d8f0db676e7f400c91d58e92886efbdea7d940cc00a32a63f9525f7";
constexpr uint32_t kRvaCanPerformAction = 0x4CEEC0;
constexpr char kUserAlertClass[] =
    "dcf2cb53a937aadfeaa3b732e940a10e7e64793f9213ffeed54c59562e772c6";
constexpr char kHashIsAlertMode[] =
    "f1edd34b222841a0ea9bbc9660f256beca1e6734d87d39cc5ad3aa1127c11e0";
constexpr char kHashCanPerformAction[] =
    "ee4e496953bc13056b1ff37de711bc9a39476d3a9c7b71fb633bc85025a6eb3";
// UserBase alert stamp（int）：仅 shape 校验 cmp 偏移；主路径不再清字段
constexpr char kHashAlertAt[] =
    "a1d1a0fe73ff1af12abfb2ca85e2212a9441a832050ab0a45c7dd104a96a8ed";
constexpr size_t kFbAlertAt = 0x118;
size_t gOffAlertAt = kFbAlertAt;
#define kOffAlertAt (gOffAlertAt)
bool gAlertFieldTried = false;

constexpr DWORD kTickMsArmed = 1000;  // threshold 已装：慢校验是否被回写
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kInstallRetryMs = 5000;
constexpr DWORD kLogMs = 15000;
constexpr uint32_t kThreshTarget = 0x7FFFFFFFu;  // signed INT_MAX；stamp > INT_MAX 永不成立

// 短 IsAlertMode 机码结构指纹
//   B8 xx xx xx xx          mov eax, imm32
//   03 05 xx xx xx xx       add eax, [rip+disp]
//   39 81 dd dd dd dd       cmp [rcx+alertOff], eax
//   0F 9F C0 / C3 …
constexpr size_t kAlertShapeScan = 48;

enum class AlertShape : uint8_t { Unknown = 0, Ok = 1, BadConst = 2, BadOff = 3, Unreadable = 4 };

struct AlertShapeInfo {
    AlertShape shape = AlertShape::Unknown;
    uint32_t imm = 0;
    uint32_t* global = nullptr;
};

std::atomic<AlertShape> gAlertShape{AlertShape::Unknown};
std::atomic<bool> gShapeLogged{false};

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

using FnCanPerformAction = uint8_t (*)(void* self, uint8_t bCheckAlert, void* methodInfo);

std::atomic<bool> gDesired{false};
std::atomic<bool> gInstalled{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gMiHits{0};
std::atomic<uint32_t> gThreshHits{0};
std::atomic<bool> gThreshArmed{false};

// Worker/SetEnabled：threshold global 现场
uint32_t* gThreshGlobal = nullptr;
uint32_t gThreshOrig = 0;
uint32_t gThreshImm = 0;
bool gThreshSaved = false;

MethodInfoHead* gMi = nullptr;
MethodInfoHead* gMiIsAlertMode = nullptr;
FnCanPerformAction gOrig = nullptr;
void* gKlass = nullptr;
DWORD gLastInstallTry = 0;
DWORD gLastLog = 0;

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

void EnsureAlertFieldOff();

AlertShapeInfo ProbeAlertShape(const void* fn) {
    AlertShapeInfo out{};
    if (!fn) {
        out.shape = AlertShape::Unreadable;
        return out;
    }
    uint8_t buf[kAlertShapeScan]{};
    __try {
        memcpy(buf, fn, sizeof(buf));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.shape = AlertShape::Unreadable;
        return out;
    }

    size_t movAt = static_cast<size_t>(-1);
    for (size_t i = 0; i + 11 <= sizeof(buf); ++i) {
        if (buf[i] == 0xB8 && buf[i + 5] == 0x03 && buf[i + 6] == 0x05) {
            movAt = i;
            break;
        }
    }
    if (movAt == static_cast<size_t>(-1)) {
        out.shape = AlertShape::BadConst;
        return out;
    }

    uint32_t imm = 0;
    int32_t disp = 0;
    memcpy(&imm, buf + movAt + 1, 4);
    memcpy(&disp, buf + movAt + 7, 4);
    const auto rip = reinterpret_cast<uintptr_t>(fn) + movAt + 11;
    out.imm = imm;
    out.global = reinterpret_cast<uint32_t*>(rip + static_cast<intptr_t>(disp));

    const uint32_t wantOff = static_cast<uint32_t>(gOffAlertAt ? gOffAlertAt : kFbAlertAt);
    uint8_t wantCmp[6] = {0x39, 0x81, 0, 0, 0, 0};
    memcpy(wantCmp + 2, &wantOff, 4);
    bool foundOff = false;
    for (size_t i = 0; i + sizeof(wantCmp) <= sizeof(buf); ++i) {
        if (memcmp(buf + i, wantCmp, sizeof(wantCmp)) == 0) {
            foundOff = true;
            break;
        }
    }
    out.shape = foundOff ? AlertShape::Ok : AlertShape::BadOff;
    return out;
}

AlertShapeInfo RefreshAlertShape(bool forceLog) {
    EnsureAlertFieldOff();
    const AlertShapeInfo info = ProbeAlertShape(AlertModeFnPtr());
    const AlertShape prev = gAlertShape.exchange(info.shape);
    if (forceLog || info.shape != prev ||
        (!gShapeLogged.load() && info.shape != AlertShape::Unknown)) {
        gShapeLogged.store(true);
        if (info.shape == AlertShape::Ok) {
            x::runtime::LogI("DropAlert",
                             "shape OK IsAlertMode cmp[rcx+0x%zX] imm=0x%X global=%p (rva=0x%X)",
                             gOffAlertAt, info.imm, reinterpret_cast<void*>(info.global),
                             kRvaIsAlertMode);
        } else {
            x::runtime::LogW("DropAlert",
                             "shape FAIL code=%u — refuse threshold (want mov+add eax,[rip] + "
                             "cmp [rcx+0x%zX]) rva=0x%X",
                             static_cast<unsigned>(info.shape), gOffAlertAt, kRvaIsAlertMode);
        }
    }
    return info;
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

bool WriteU32(uint32_t* p, uint32_t v) {
    if (!p) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(uint32_t), PAGE_READWRITE, &old)) {
        __try {
            *p = v;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    bool ok = false;
    __try {
        *p = v;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(p, sizeof(uint32_t), old, &old);
    return ok;
}

bool ReadU32(const uint32_t* p, uint32_t* out) {
    if (!p || !out) return false;
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RestoreThreshold() {
    if (!gThreshSaved || !gThreshGlobal) {
        gThreshArmed.store(false);
        return;
    }
    uint32_t cur = 0;
    if (ReadU32(gThreshGlobal, &cur) && cur == gThreshOrig) {
        gThreshArmed.store(false);
        return;
    }
    if (WriteU32(gThreshGlobal, gThreshOrig)) {
        x::runtime::LogI("DropAlert", "threshold restore global=%p val=0x%X",
                         reinterpret_cast<void*>(gThreshGlobal), gThreshOrig);
    } else {
        x::runtime::LogW("DropAlert", "threshold restore FAIL global=%p",
                         reinterpret_cast<void*>(gThreshGlobal));
    }
    gThreshArmed.store(false);
}

bool ArmThreshold(const AlertShapeInfo& info, DWORD now) {
    if (info.shape != AlertShape::Ok || !info.global) return false;

    const uint32_t want = static_cast<uint32_t>((static_cast<uint64_t>(kThreshTarget) -
                                                 static_cast<uint64_t>(info.imm)) &
                                                0xffffffffu);

    if (!gThreshSaved || gThreshGlobal != info.global || gThreshImm != info.imm) {
        uint32_t cur = 0;
        if (!ReadU32(info.global, &cur)) return false;
        // 若已是我们写过的值，保留先前 orig；否则以当前为 orig
        if (!(gThreshSaved && gThreshGlobal == info.global && cur == want)) {
            gThreshOrig = cur;
        }
        gThreshGlobal = info.global;
        gThreshImm = info.imm;
        gThreshSaved = true;
    }

    uint32_t cur = 0;
    if (!ReadU32(gThreshGlobal, &cur)) return false;
    if (cur == want) {
        gThreshArmed.store(true);
        return true;
    }

    if (!WriteU32(gThreshGlobal, want)) {
        gThreshArmed.store(false);
        return false;
    }
    gThreshHits.fetch_add(1, std::memory_order_relaxed);
    gThreshArmed.store(true);
    if (!gLastLog || now - gLastLog >= kLogMs) {
        gLastLog = now;
        x::runtime::LogI("DropAlert",
                         "threshold arm global=%p imm=0x%X orig=0x%X want=0x%X "
                         "(imm+want=0x%X) hits=%u",
                         reinterpret_cast<void*>(gThreshGlobal), gThreshImm, gThreshOrig, want,
                         static_cast<uint32_t>(gThreshImm + want), gThreshHits.load());
    }
    return true;
}

void ReportDropAlertLamp() {
    (void)EnsureIsAlertModeMi();
    const AlertShapeInfo info = RefreshAlertShape(false);
    if (info.shape == AlertShape::BadConst || info.shape == AlertShape::BadOff ||
        info.shape == AlertShape::Unreadable) {
        const char* d = info.shape == AlertShape::BadOff     ? "MISS off"
                        : info.shape == AlertShape::BadConst ? "MISS const"
                                                             : "MISS unreadable";
        x::runtime::anchor_lamps::Set("DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     d);
        return;
    }
    if (info.shape == AlertShape::Ok && gThreshArmed.load()) {
        x::runtime::anchor_lamps::Set(
            "DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Ok,
            gInstalled.load() ? "thresh+secMI" : "thresh");
        return;
    }
    if (info.shape == AlertShape::Ok) {
        x::runtime::anchor_lamps::Set("DropAlert",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                     "shape ok, thresh late");
        return;
    }
    x::runtime::anchor_lamps::Set("DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                 "pending");
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
    (void)x::runtime::il2cpp::RuntimeClassInit(gKlass);
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
    x::runtime::LogI("DropAlert", "MI secondary installed (direct-call paths use threshold)");
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

// Primary: arm IsAlertMode threshold global (imm+global → INT_MAX).
bool MaintainThreshold(DWORD now) {
    (void)EnsureIsAlertModeMi();
    const AlertShapeInfo info = RefreshAlertShape(false);
    if (info.shape != AlertShape::Ok) {
        gThreshArmed.store(false);
        if (!gLastLog || now - gLastLog >= kLogMs) {
            gLastLog = now;
            ReportDropAlertLamp();
        }
        return false;
    }
    const bool ok = ArmThreshold(info, now);
    if (!gLastLog || now - gLastLog >= kLogMs) {
        gLastLog = now;
        uint32_t cur = 0;
        (void)ReadU32(info.global, &cur);
        x::runtime::LogI("DropAlert",
                         "threshold %s global=%p cur=0x%X armed=%d miHits=%u threshHits=%u",
                         ok ? "ok" : "FAIL", reinterpret_cast<void*>(info.global), cur,
                         gThreshArmed.load() ? 1 : 0, gMiHits.load(), gThreshHits.load());
        ReportDropAlertLamp();
    }
    return ok;
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("DropAlert",
                     "worker start — threshold global (IsAlertMode); MI secondary");
    for (int i = 0; i < 400 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll"); ++i)
        Sleep(50);
    Tick(GetTickCount());
    while (!gStop.load()) {
        const bool on = gDesired.load();
        Tick(GetTickCount());
        Sleep(on ? kTickMsArmed : kTickMsOff);
    }
    RestoreThreshold();
    UninstallMi();
    x::runtime::LogI("DropAlert", "worker stop miHits=%u threshHits=%u", gMiHits.load(),
                     gThreshHits.load());
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(false);
    EnsureAlertFieldOff();
    x::runtime::LogI("DropAlert",
                     "init — primary: IsAlertMode threshold global; field+0x%zX shape-only; "
                     "IsAlertMode rva=0x%X; MI rva=0x%X secondary (default off)",
                     gOffAlertAt, kRvaIsAlertMode, kRvaCanPerformAction);
}

void Shutdown() {
    gDesired.store(false);
    StopWorker();
    RestoreThreshold();
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
    if (!on) RestoreThreshold();
}

bool IsEnabled() { return gDesired.load(); }

bool IsInstalled() {
    return gThreshArmed.load() || (gInstalled.load() && gOrig != nullptr);
}

void Tick(DWORD now) {
    if (!gDesired.load()) {
        RestoreThreshold();
        return;
    }

    (void)MaintainThreshold(now);

    if (!(gInstalled.load() && HookStillOurs(gMi) && gOrig)) {
        if (!gLastInstallTry || now - gLastInstallTry >= kInstallRetryMs) {
            gLastInstallTry = now;
            (void)TryInstallMi();
        }
    }
}

}  // namespace x::features::drop_alert_bypass
