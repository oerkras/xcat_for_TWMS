// TWMS Classic — drop_alert_bypass.
//
// Root cause (BIN): callers use direct `call CanPerformAction` (E8×8), so
// MethodInfo swap never runs. Real drop gate calls thin IsAlertMode
// (UserBase$$IsAlertMode @0x124DE70) which reads +0x118 vs (seed+global).
// 2026-08-13 remount: 字段 0x114(bool 误锚)→0x118(int 警戒戳)；RVA 未漂。
// While enabled, data-plane clear +0x118 — drop opens AND client alert suppressed.
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

// UserBase 父类. 短 IsAlertMode（CanPerformAction 真 callee）
// IDA 2026-08-13: mov eax,imm; add eax,[rip]; cmp [rcx+0x118],eax; setnle — shape 只认 op 形态
constexpr uint32_t kRvaIsAlertMode = 0x124DE70;  // remounted 2026-08-06 (+0x1E70)；RVA 仍准

// Secondary: MethodInfo on DragManager.CanPerformAction (rarely hit; keep for MI callers)
constexpr char kDragManagerClass[] =
    "d2290478242217849e81b25341e5981b7c5dc1bc6897c784b7fd2f5f3db8bee";
constexpr uint32_t kRvaCanPerformAction = 0x4CFED0;  // remounted 2026-08-06（RVA 未漂）
constexpr char kUserAlertClass[] =
    "d5a59751c9ecba4a21314526d7fbe8142abe3ee8b90e8d03a7fc2f80f669add";
constexpr char kHashIsAlertMode[] =
    "a9e101249890ed3f99c5e6a5d37bff8b74311409454a8a63b4f0acf2d18af77";
constexpr char kHashCanPerformAction[] =
    "c9827716092b598b0c395c8455da154d17ed38264ae8c24068596ae83293827";
// UserBase alert stamp（int）：hash → field_get_offset（dump fallback 0x118）
// 勿用 bac75f…@0x114（bool）；IsAlertMode 读的是 a363…@0x118
constexpr char kHashAlertAt[] =
    "a363a66e2ecf97c765a16a7d795ca7cf3416ee02804c5ae5305d1ebbace6e0f";
constexpr size_t kFbAlertAt = 0x118;
size_t gOffAlertAt = kFbAlertAt;
#define kOffAlertAt (gOffAlertAt)
bool gAlertFieldTried = false;

constexpr DWORD kTickMsApply = 32;   // 非 0 / 未攒够零拍：快清
constexpr DWORD kTickMsHold = 1000;  // 连续多拍已是 0：慢校验（减挂机空转）
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kInstallRetryMs = 5000;
constexpr DWORD kLogClearMs = 3000;
// 连续 before==0 达到此拍数才进入 1s hold，避免「刚清→hold→下一刀刷戳→最长 1s 拒丢」
constexpr int kHoldAfterZeroStreak = 8;  // ~8×32ms ≈ 256ms 稳住后再慢扫

// 短 IsAlertMode 机码结构指纹（防漂到 CFF 兄弟 / 字段改偏）
//   B8 xx xx xx xx          mov eax, imm32（种子解混淆后语义常变，不钉死 IMM）
//   03 05 xx xx xx xx       add eax, [rip+disp]
//   39 81 dd dd dd dd       cmp [rcx+alertOff], eax  （alertOff 来自 field hash）
//   0F 9F C0 / C3 …
constexpr size_t kAlertShapeScan = 48;

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

std::atomic<bool> gDesired{false};
std::atomic<bool> gInstalled{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gMiHits{0};
std::atomic<uint32_t> gClearHits{0};
// Worker-only：连续多拍字段为 0 后才 hold；出刀刷戳立刻退回快拍。
bool gHolding = false;
int gZeroStreak = 0;

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

void EnsureAlertFieldOff();

AlertShape ProbeAlertShape(const void* fn) {
    if (!fn) return AlertShape::Unreadable;
    uint8_t buf[kAlertShapeScan]{};
    __try {
        memcpy(buf, fn, sizeof(buf));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return AlertShape::Unreadable;
    }
    // 结构：mov eax,imm32 ; add eax,[rip+rel32] —— IMM 随种子漂，只认操作码形态
    bool constOk = false;
    for (size_t i = 0; i + 11 <= sizeof(buf); ++i) {
        if (buf[i] == 0xB8 && buf[i + 5] == 0x03 && buf[i + 6] == 0x05) {
            constOk = true;
            break;
        }
    }
    if (!constOk) return AlertShape::BadConst;

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
    return foundOff ? AlertShape::Ok : AlertShape::BadOff;
}

AlertShape RefreshAlertShape(bool forceLog) {
    EnsureAlertFieldOff();
    const AlertShape s = ProbeAlertShape(AlertModeFnPtr());
    const AlertShape prev = gAlertShape.exchange(s);
    if (forceLog || s != prev || (!gShapeLogged.load() && s != AlertShape::Unknown)) {
        gShapeLogged.store(true);
        if (s == AlertShape::Ok) {
            x::runtime::LogI("DropAlert", "shape OK IsAlertMode cmp[rcx+0x%zX] (rva=0x%X)",
                             gOffAlertAt, kRvaIsAlertMode);
        } else {
            x::runtime::LogW("DropAlert",
                             "shape FAIL code=%u — refuse field clear (want mov+add eax,[rip] + "
                             "cmp [rcx+0x%zX]) rva=0x%X",
                             static_cast<unsigned>(s), gOffAlertAt, kRvaIsAlertMode);
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
        const char* d = shape == AlertShape::BadOff     ? "MISS off"
                        : shape == AlertShape::BadConst ? "MISS const"
                                                        : "MISS unreadable";
        x::runtime::anchor_lamps::Set("DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     d);
        return;
    }
    if (shape == AlertShape::Ok && gMiIsAlertMode) {
        x::runtime::anchor_lamps::Set(
            "DropAlert", x::runtime::anchor_lamps::AnchorLampCode::Ok,
            gInstalled.load() ? "shape+118+secMI" : "shape+118");
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
    // 禁 worker RuntimeClassInit（GC unknown thread）。游戏已用过 DragManager 则 cctor 已跑；
    // 未初始化时 RuntimeClassInit 在泵外会被 ManagedAllocSafe 挡掉——MI 安装延后即可。
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

// Primary path: expire LocalUser+0x118 so thin IsAlertMode returns false.
// Shape gate: 机码不像「mov+add eax,[rip] + cmp [rcx+alertOff]」则拒绝写字段（防漂）。
// Hold：连续 kHoldAfterZeroStreak 拍已是 0 才慢扫；非 0 清零并重置 streak（防 1s 窗口拒丢）。
bool MaintainAlertField(DWORD now) {
    ports::player_combat::CombatCtx ctx{};
    if (!ports::player_combat::QueryCombatCtx(ctx) || !ctx.localUser) {
        gHolding = false;
        gZeroStreak = 0;
        return false;
    }

    EnsureAlertFieldOff();
    (void)EnsureIsAlertModeMi();
    const AlertShape shape = RefreshAlertShape(false);
    if (shape != AlertShape::Ok) {
        gHolding = false;
        gZeroStreak = 0;
        if (!gLastClearLog || now - gLastClearLog >= kLogClearMs) {
            gLastClearLog = now;
            ReportDropAlertLamp();
        }
        return false;
    }

    int before = 0;
    __try {
        before = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx.localUser) + kOffAlertAt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gHolding = false;
        gZeroStreak = 0;
        return false;
    }

    if (before == 0) {
        if (gZeroStreak < kHoldAfterZeroStreak) ++gZeroStreak;
        gHolding = (gZeroStreak >= kHoldAfterZeroStreak);
        if (!gLastClearLog || now - gLastClearLog >= kLogClearMs) {
            gLastClearLog = now;
            x::runtime::LogI("DropAlert",
                             "hold-cand +0x%zX=0 streak=%d/%d holding=%d miHits=%u clears=%u "
                             "shape=%u",
                             kOffAlertAt, gZeroStreak, kHoldAfterZeroStreak, gHolding ? 1 : 0,
                             gMiHits.load(), gClearHits.load(), static_cast<unsigned>(shape));
            ReportDropAlertLamp();
        }
        return true;
    }

    gZeroStreak = 0;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx.localUser) + kOffAlertAt) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gHolding = false;
        return false;
    }

    gHolding = false;
    // 禁 worker 直调 IsAlertMode（托管入口 → GC unknown thread）。字段清零即主路径。
    gClearHits.fetch_add(1, std::memory_order_relaxed);
    if (!gLastClearLog || now - gLastClearLog >= kLogClearMs) {
        gLastClearLog = now;
        x::runtime::LogI("DropAlert",
                         "field clear +0x%zX was=%d miHits=%u clears=%u alertMi=%d shape=%u",
                         kOffAlertAt, before, gMiHits.load(), gClearHits.load(),
                         gMiIsAlertMode ? 1 : 0, static_cast<unsigned>(shape));
        ReportDropAlertLamp();
    }
    return true;
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("DropAlert",
                     "worker start — data-plane +0x%zX hold/apply; MI secondary", kOffAlertAt);
    for (int i = 0; i < 400 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll"); ++i)
        Sleep(50);
    Tick(GetTickCount());
    while (!gStop.load()) {
        const bool on = gDesired.load();
        Tick(GetTickCount());
        DWORD sleepMs = kTickMsOff;
        if (on) sleepMs = gHolding ? kTickMsHold : kTickMsApply;
        Sleep(sleepMs);
    }
    UninstallMi();
    gHolding = false;
    gZeroStreak = 0;
    x::runtime::LogI("DropAlert", "worker stop miHits=%u clears=%u", gMiHits.load(),
                     gClearHits.load());
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(false);
    EnsureAlertFieldOff();
    x::runtime::LogI("DropAlert",
                     "init — primary: LocalUser+0x%zX clear; IsAlertMode rva=0x%X; MI rva=0x%X "
                     "secondary (default off)",
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
    if (!gDesired.load()) {
        gHolding = false;
        gZeroStreak = 0;
        return;
    }

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
