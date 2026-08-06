// Classic TWMS — primary attack via UserLocal.OnFuncKey(绑定键 FuncKey)。
// 默认读 A 键（InputSystem.Key.A=15）上绑定的技能/动作；空才回退 BasicActionAttack(5/52)。
// BIN: OnKey(Ctrl) can log ok with pktSum=0; OnFuncKey 才是掉血真源。
// 朝向：VecCtrl.SetInput(±1,0) → OnResolveMoveAction（禁 InjectKeyHold L/R：Up SEH 清 IM）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_input_port.h"

#include "input_port.h"
#include "player_combat_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/mono_clock.h"
#include "../../runtime/anchor_lamps.h"
#include "../../../common/xcat_payload_control.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace x::features::ports::attack {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// 本模块所有「经过了多久」的判断一律走 NowMs()：GetTickCount 的 15.625ms 步进会把
// 出刀门控量化到该步长的整数倍（interval=50 实跑 62.5）。NowMs 与其同轴、分辨率 1ms。
using x::runtime::NowMs;

constexpr DWORD kDefaultIntervalMs = xcat::kSimpleCombatAttackIntervalDefaultMs;
// 智能间隔：在面板 config 附近抖动，不再锁死 480–560。
constexpr DWORD kSmartJitterMs = 40;
// 动作占用（禁 TP）：只挡前摇，不当做出刀地板。
constexpr DWORD kAttackAnimBusyMs = 220;
constexpr float kFaceDeadzone = 8.f;
// 已面向正确半场时，|dx| 未过此值不重 SetInput，减轻怪贴身穿过时的左右抽风。
constexpr float kFaceStickyPx = 28.f;
constexpr DWORD kRateWindowMs = 3000;
constexpr DWORD kFireJobWaitMs = 800;
constexpr DWORD kFaceJobWaitMs = 400;
constexpr DWORD kFkmRebindMs = 3000;

constexpr uint32_t kRvaOnFuncKey = 0x1082250;  // remounted 2026-08-04
constexpr uint32_t kRvaGetKeyByFunc = 0x16504C0;  // remounted 2026-08-04
constexpr uint32_t kRvaGetDataByKeyCode = 0x164F7C0;  // remounted 2026-08-04
constexpr uint32_t kRvaFuncKeyCtor = 0x1647B90;  // remounted 2026-08-04: .ctor(FuncType,int)
// 写 InputX/Y + 内联 OnResolveMoveAction（朝向）；见 docs/features/protocol/MoveElem字段.md
constexpr uint32_t kRvaVecCtrlSetInput = 0x11BC430;  // remounted 2026-08-04

// 方法哈希（dump.cs · remount 2026-08-04）
constexpr char kHashOnFuncKey[] =
    "f8cfa503e0f539e6dbb051a648d375a8b7847d067db4a7043e61ed7d49b423f";
constexpr char kHashGetKeyByFunc[] =
    "ca441497121e760f75345e9aa8575485252c9f2d1e372b00a215d5851ce057f";
constexpr char kHashGetDataByKeyCode[] =
    "a0d3bb0e07878aa585d5c5b47fb2836f7759427a165955082d2e7bf87af7a46";
constexpr char kHashVecCtrlSetInput[] =
    "c3a073db5fb471d6ca353165df8b58dec04bf778804ee0e77505ab5f8d61fb9";
constexpr char kVecCtrlClass[] =
    "ef24024acbe225bcc90ca332f3e00aff5800daa32a769057d2e830eeac776bb";
constexpr char kActorBaseClass[] =
    "ddef6db860cfa2bea6dca39e201bf3065a897797f86009fb4d6104830143d94";
// FKM remounted 2026-08-04 (owns GetKeyByFunc / GetDataByKeyCode).
constexpr char kFkmClass[] =
    "c18b40c5d905e6ddbc8c9e4cfc486aff2d1e47d038a192d5aa77e999ea233d7";
constexpr char kFuncKeyClass[] =
    "c5f306e5860ab75f344a5ad42c89868b10dd30405e7e07ca0ce540ddbb792c8";

// Actor.VecCtrl / VecCtrl.MoveAction / FuncKey.type|value：hash → field_get_offset
constexpr char kHashUserVecCtrl[] =
    "<dc76f5c9e250bc9a327a219b39e16c345cdabf7b01ad5c60b568045069c9120>k__BackingField";
constexpr char kHashVcMoveAction[] =
    "afdef055a699e27cb4575fce73d95752cd4571320e9c13b0c0322e96a023c3a";
constexpr char kHashFkType[] =
    "c457db52bc5102a0fe56359124142c8a914dfc6083b9130be075fada445b22a";
constexpr char kHashFkValue[] =
    "f76a3ef9dbb055eb9d2ad8533e3a498dfff3ba28773d12b37132ca043ba9bfc";

constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kFbVcMoveAction = 0x84;
constexpr size_t kFbFkType = 0x10;
constexpr size_t kFbFkValue = 0x14;
size_t gOffVecCtrl = kFbVecCtrl;
size_t gOffVcMoveAction = kFbVcMoveAction;
size_t gOffFkType = kFbFkType;
size_t gOffFkValue = kFbFkValue;
#define kOffVecCtrl (gOffVecCtrl)
#define kOffVcMoveAction (gOffVcMoveAction)
#define kOffFkType (gOffFkType)
#define kOffFkValue (gOffFkValue)
bool gAttackFieldTried = false;

bool AttackFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t lo,
                       size_t hi) {
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
            if (off >= lo && off < hi) {
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

void EnsureAttackFieldOff() {
    if (gAttackFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gAttackFieldTried = true;
    void* actor = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    if (!actor) actor = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* vc = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* fk = x::runtime::il2cpp::FindClass("", kFuncKeyClass);
    int hits = 0;
    if (AttackFieldOffHit(actor, kHashUserVecCtrl, kFbVecCtrl, &gOffVecCtrl, 0x40, 0x100)) ++hits;
    if (AttackFieldOffHit(vc, kHashVcMoveAction, kFbVcMoveAction, &gOffVcMoveAction, 0x40, 0x100))
        ++hits;
    if (AttackFieldOffHit(fk, kHashFkType, kFbFkType, &gOffFkType, 0x10, 0x40)) ++hits;
    if (AttackFieldOffHit(fk, kHashFkValue, kFbFkValue, &gOffFkValue, 0x10, 0x40)) ++hits;
    x::runtime::LogI("Attack",
                     "attack slots path=%s hits=%d/4 vc=0x%zX move=0x%zX fkT=0x%zX fkV=0x%zX",
                     hits == 4 ? "meta" : (hits ? "meta-partial" : "fallback"), hits, gOffVecCtrl,
                     gOffVcMoveAction, gOffFkType, gOffFkValue);
}

constexpr int32_t kKeyInputDown = 0;
constexpr int32_t kKeyInputUp = 1;
constexpr int32_t kFuncTypeNone = 0;
constexpr int32_t kFuncTypeBasicAction = 5;
constexpr int32_t kFkmBasicActionAttack = 52;
// 默认攻击槽：Win32 'A' → InputSystem.Key.A = 15（见 input_port VkToUnityKey）。
constexpr WORD kDefaultAttackVk = static_cast<WORD>('A');

using FnOnFuncKey = void (*)(void* self, int32_t inputType, void* funcKey, uint32_t scan,
                             const void* methodInfo);
using FnGetKeyByFunc = int32_t (*)(void* self, int32_t funcType, int32_t fkmType,
                                   const void* methodInfo);
using FnGetDataByKeyCode = void* (*)(void* self, int32_t key, const void* methodInfo);
using FnFuncKeyCtor = void (*)(void* self, int32_t funcType, int32_t value,
                               const void* methodInfo);
using FnSetInput = void (*)(void* self, int inputX, int inputY, const void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

std::atomic<WORD> gAttackVk{kDefaultAttackVk};
std::atomic<DWORD> gConfigIntervalMs{kDefaultIntervalMs};
std::atomic<DWORD> gEffectiveIntervalMs{kDefaultIntervalMs};
std::atomic<DWORD> gAttackHoldMs{xcat::kAttackHoldDefaultMs};
std::atomic<bool> gSmartInterval{false};
std::atomic<DWORD> gLastFireMs{0};
std::atomic<int> gAnimBusyOverrideMs{-1};  // <0 = kAttackAnimBusyMs
std::atomic<bool> gImmediateUp{false};     // 攻击加速：Down+Up 同泵，无 pending
std::atomic<uint32_t> gFireOk{0};
std::atomic<uint32_t> gFireFail{0};   // 仅 OnFuncKey Down 硬失败
std::atomic<uint32_t> gFireSoft{0};   // 间隔/pending 软拒绝（加速短间隔下同 tick 空点）
std::atomic<float> gFaceDx{0.f};
std::atomic<int> gLastFaceSign{0};  // -1 左 / +1 右 / 0 未知
DWORD gRateWindowStart = 0;

void* gFkm = nullptr;
void* gFkmKlass = nullptr;
DWORD gLastFkmRebind = 0;

MethodInfoHead* gMiOnFuncKey = nullptr;
MethodInfoHead* gMiGetKeyByFunc = nullptr;
MethodInfoHead* gMiGetDataByKeyCode = nullptr;
MethodInfoHead* gMiFuncKeyCtor = nullptr;
MethodInfoHead* gMiSetInput = nullptr;

void* gAttackFk = nullptr;  // pinned FuncKey（优先 A 键绑定）
uint32_t gAttackFkGc = 0;
int32_t gResolvedUnityKey = 0;
int32_t gResolvedFkType = -1;
int32_t gResolvedFkValue = -1;
bool gFkSynthetic = false;
DWORD gLastFkResolveMs = 0;

std::atomic<bool> gPendingUp{false};
std::atomic<bool> gFireSuppressed{false};
std::atomic<DWORD> gUpDueMs{0};
// simple_combat / multi_skill 双 worker 都会 Init；第二次不得重置间隔/计数。
std::atomic<bool> gInited{false};


template <typename T>
T AtRva(uint32_t rva) {
    return x::runtime::il2cpp::AtRva<T>(rva);
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    auto* mi = reinterpret_cast<MethodInfoHead*>(
        x::runtime::il2cpp_method::FindMethodByRva(klass, rva, true));
    return (mi && mi->methodPointer) ? mi : nullptr;
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
                          const char* plainName, const char* hashName,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plainName, hashName);
    if (outPath) *outPath = mr.path;
    if (mr.method) return reinterpret_cast<MethodInfoHead*>(mr.method);
    return nullptr;
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

void EnsureMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    EnsureAttackFieldOff();
    if (!gFkmKlass) gFkmKlass = x::runtime::il2cpp::FindClass("", kFkmClass);
    void* ulKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* fkKlass = x::runtime::il2cpp::FindClass("", kFuncKeyClass);
    void* vcKlass = x::runtime::il2cpp::FindClass("", kVecCtrlClass);

    int hashHits = 0;
    auto fill = [&](MethodInfoHead*& slot, void* klass, uint32_t rva, const MethodShape& shape,
                    const char* plain, const char* hash) {
        if (slot || !klass) return;
        ResolvePath path = ResolvePath::Miss;
        slot = ResolveMi(klass, rva, shape, plain, hash, &path);
        if (slot && path == ResolvePath::Hash) ++hashHits;
    };

    if (ulKlass) {
        constexpr MethodShape kFk{3,
                                  TypeKind::Void,
                                  true,
                                  true,
                                  {TypeKind::I32, TypeKind::Ptr, TypeKind::U32}};
        fill(gMiOnFuncKey, ulKlass, kRvaOnFuncKey, kFk, "OnFuncKey", kHashOnFuncKey);
    }
    if (gFkmKlass) {
        constexpr MethodShape kGet{2, TypeKind::I32, true, true, {TypeKind::Any, TypeKind::I32}};
        fill(gMiGetKeyByFunc, gFkmKlass, kRvaGetKeyByFunc, kGet, "GetKeyByFunc",
             kHashGetKeyByFunc);
        constexpr MethodShape kData{1, TypeKind::Ptr, true, true, {TypeKind::Any}};
        fill(gMiGetDataByKeyCode, gFkmKlass, kRvaGetDataByKeyCode, kData, "GetDataByKeyCode",
             kHashGetDataByKeyCode);
    }
    if (fkKlass) {
        constexpr MethodShape kCtor{2, TypeKind::Void, true, false, {TypeKind::Any, TypeKind::I32}};
        fill(gMiFuncKeyCtor, fkKlass, kRvaFuncKeyCtor, kCtor, ".ctor", nullptr);
    }
    if (vcKlass) {
        constexpr MethodShape kIn{2, TypeKind::Void, false, true, {TypeKind::I32, TypeKind::I32}};
        fill(gMiSetInput, vcKlass, kRvaVecCtrlSetInput, kIn, "SetInput", kHashVecCtrlSetInput);
    }

    static bool sLogged = false;
    const int hits = (gMiOnFuncKey ? 1 : 0) + (gMiGetKeyByFunc ? 1 : 0) +
                     (gMiGetDataByKeyCode ? 1 : 0) + (gMiFuncKeyCtor ? 1 : 0) +
                     (gMiSetInput ? 1 : 0);
    if (!sLogged && hits > 0) {
        sLogged = true;
        // ctor 无 dump 哈希名（.ctor），hash 满分按 4/4 计（不含 ctor）
        x::runtime::LogI("Attack",
                         "methods path=%s hits=%d/5 hash=%d onFk=%d getKey=%d getData=%d ctor=%d "
                         "setIn=%d",
                         hashHits >= 4 ? "meta" : (hashHits ? "meta-partial" : "rva/kind"), hits,
                         hashHits, gMiOnFuncKey ? 1 : 0, gMiGetKeyByFunc ? 1 : 0,
                         gMiGetDataByKeyCode ? 1 : 0, gMiFuncKeyCtor ? 1 : 0, gMiSetInput ? 1 : 0);
    }
}

void OpenLog() {
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    CreateDirectoryA(dir, nullptr);
}

void LogLine(const char* fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[640];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    OpenLog();
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    (void)x::runtime::AppendDbgLogA(dir, "combat.log", buf, (DWORD)n);
    x::runtime::LogI("Attack", "%s", body);
}

DWORD ClampAttackIntervalMs(DWORD ms) {
    if (ms < xcat::kSimpleCombatAttackIntervalMinMs)
        return xcat::kSimpleCombatAttackIntervalMinMs;
    if (ms > xcat::kSimpleCombatAttackIntervalMaxMs)
        return xcat::kSimpleCombatAttackIntervalMaxMs;  // 与 common 常量对齐
    return ms;
}

// 松键时长 = 调试 TAB 的独立参数，再按面板间隔封顶：hold ≥ interval 会让 pendingUp
// 一直挡住 SoftBlocked，把下一刀锁死。攻击加速开启时走 pulse 路径（hold=0），此值不参与。
DWORD AttackHoldMs() {
    const DWORD interval = ClampAttackIntervalMs(gEffectiveIntervalMs.load(std::memory_order_relaxed));
    const DWORD hold = xcat::ClampAttackHoldMs(gAttackHoldMs.load(std::memory_order_relaxed));
    return hold < interval ? hold : interval;
}

DWORD RandomSmartIntervalMs(DWORD config) {
    static uint32_t s_state = 0;
    if (!s_state) {
        s_state = static_cast<uint32_t>(GetTickCount());
        if (!s_state) s_state = 0x9E3779B9u;
    }
    s_state ^= s_state << 13;
    s_state ^= s_state >> 17;
    s_state ^= s_state << 5;
    const DWORD base = ClampAttackIntervalMs(config);
    const DWORD span = kSmartJitterMs * 2 + 1;
    const int delta = static_cast<int>(s_state % span) - static_cast<int>(kSmartJitterMs);
    return ClampAttackIntervalMs(static_cast<DWORD>(static_cast<int>(base) + delta));
}

void RefreshEffectiveInterval(bool forceSmartRandom, bool logChange) {
    const bool smart = gSmartInterval.load(std::memory_order_relaxed);
    const DWORD config = ClampAttackIntervalMs(gConfigIntervalMs.load(std::memory_order_relaxed));
    const DWORD current = gEffectiveIntervalMs.load(std::memory_order_relaxed);
    DWORD next = config;
    if (smart) {
        // 每次强制刷新重抽；固定模式严格跟面板，不再抬到 480。
        if (forceSmartRandom || current == 0)
            next = RandomSmartIntervalMs(config);
        else
            next = current;
    }
    if (gEffectiveIntervalMs.exchange(next, std::memory_order_relaxed) == next) return;
    if (logChange) {
        LogLine("%s interval %ums config=%ums path=OnFuncKey(A-slot/fallback)",
                smart ? "smart attack random" : "fixed attack", (unsigned)next, (unsigned)config);
    }
}

void MaybeLogRate(DWORD now) {
    if (!gRateWindowStart) {
        gRateWindowStart = now;
        return;
    }
    if (now - gRateWindowStart < kRateWindowMs) return;
    const uint32_t ok = gFireOk.exchange(0);
    const uint32_t fail = gFireFail.exchange(0);
    const uint32_t soft = gFireSoft.exchange(0);
    LogLine(
        "cast rate/3s ok=%u fail=%u soft=%u interval=%ums smart=%d path=OnFuncKey(A) t=%d v=%d",
        ok, fail, soft, (unsigned)gEffectiveIntervalMs.load(), gSmartInterval.load() ? 1 : 0,
        (int)gResolvedFkType, (int)gResolvedFkValue);
    gRateWindowStart = now;
}

// 软门控：间隔未到或松键未到。调用方须已 FlushPendingUp。
bool SoftBlocked(DWORD now) {
    const DWORD interval = gEffectiveIntervalMs.load(std::memory_order_relaxed);
    const DWORD last = gLastFireMs.load(std::memory_order_relaxed);
    if (last && now - last < interval) return true;
    if (gPendingUp.load(std::memory_order_acquire)) return true;
    return false;
}

void* TryLazyValue(void* lazy) {
    if (!lazy || !LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classStaticData) {
        __try {
            void* p = e.classStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

void* TryResolveFkmSingleton() {
    if (!gFkmKlass) gFkmKlass = x::runtime::il2cpp::FindClass("", kFkmClass);
    if (!gFkmKlass) return nullptr;

    const auto& e = x::runtime::il2cpp::Get();
    if (e.runtimeClassInit) {
        __try {
            x::runtime::il2cpp::RuntimeClassInit(gFkmKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* staticsKlass = gFkmKlass;
    if (e.classParent) {
        void* parent = nullptr;
        __try {
            parent = e.classParent(gFkmKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (e.runtimeClassInit) {
                __try {
                    x::runtime::il2cpp::RuntimeClassInit(parent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            staticsKlass = parent;
        }
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gFkmKlass);
    if (!statics) return nullptr;

    for (size_t s = 0; s < 6; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeHeapPtr(cand)) continue;
        if (ReadPtr(cand, 0) == gFkmKlass) return cand;
        if (!gFkm) return cand;
    }
    return nullptr;
}

bool EnsureFkmOnMain() {
    const DWORD now = NowMs();
    if (gFkm && LooksLikeHeapPtr(gFkm) && ReadPtr(gFkm, 0) && now - gLastFkmRebind < kFkmRebindMs)
        return true;
    gLastFkmRebind = now;
    gFkm = TryResolveFkmSingleton();
    return gFkm != nullptr;
}

void ClearAttackFk() {
    if (gAttackFkGc) {
        const auto& e = x::runtime::il2cpp::Get();
        if (e.gcHandleFree) {
            __try {
                e.gcHandleFree(gAttackFkGc);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        gAttackFkGc = 0;
    }
    gAttackFk = nullptr;
    gResolvedUnityKey = 0;
    gResolvedFkType = -1;
    gResolvedFkValue = -1;
    gFkSynthetic = false;
    gLastFkResolveMs = 0;
}

bool PinAttackFk(void* fk) {
    if (!LooksLikeHeapPtr(fk)) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.gcHandleNew) {
        gAttackFk = fk;
        gAttackFkGc = 0;
        return true;
    }
    uint32_t h = 0;
    __try {
        h = e.gcHandleNew(fk, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    if (!h) {
        gAttackFk = fk;
        gAttackFkGc = 0;
        return true;
    }
    ClearAttackFk();
    gAttackFk = fk;
    gAttackFkGc = h;
    return true;
}

bool ReadFkFields(void* fk, int32_t* outType, int32_t* outValue) {
    if (!LooksLikeHeapPtr(fk)) return false;
    int32_t t = -1;
    int32_t v = -1;
    __try {
        t = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(fk) + kOffFkType);
        v = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(fk) + kOffFkValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (outType) *outType = t;
    if (outValue) *outValue = v;
    return true;
}

bool EnsureAttackFkOnMain() {
    const DWORD now = NowMs();
    // 短窗复用已 pin 的键位；到期重读 A，换绑技能即时生效。
    if (gAttackFk && LooksLikeHeapPtr(gAttackFk) && gLastFkResolveMs &&
        now - gLastFkResolveMs < kFkmRebindMs) {
        int32_t t = -1, v = -1;
        if (ReadFkFields(gAttackFk, &t, &v) && t != kFuncTypeNone) return true;
    }

    ClearAttackFk();
    if (!x::runtime::il2cpp::Ensure()) return false;
    EnsureMethodInfos();
    const bool haveFkm = EnsureFkmOnMain();
    if (!haveFkm) LogLine("FKM singleton miss → try synthetic FuncKey");

    auto getKey = FnFromMi<FnGetKeyByFunc>(gMiGetKeyByFunc, kRvaGetKeyByFunc);
    auto getData = FnFromMi<FnGetDataByKeyCode>(gMiGetDataByKeyCode, kRvaGetDataByKeyCode);
    int32_t unityKey = 0;
    void* fk = nullptr;
    bool syn = false;

    const WORD atkVk = gAttackVk.load(std::memory_order_relaxed);
    const int32_t slotKey = x::features::ports::input::VkToUnityKey(atkVk);

    // ①② 有 FKM 才读键表；否则直接落到合成 5/52（仍可 OnFuncKey 出刀）。
    if (haveFkm) {
        // ① 优先：攻击 VK（默认 A）→ GetDataByKeyCode，用玩家实际绑定。
        if (slotKey > 0 && getData) {
            void* slotFk = nullptr;
            __try {
                    slotFk = getData(gFkm, slotKey, gMiGetDataByKeyCode);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                slotFk = nullptr;
            }
            int32_t t = -1, v = -1;
            if (slotFk && ReadFkFields(slotFk, &t, &v) && t != kFuncTypeNone) {
                fk = slotFk;
                unityKey = slotKey;
                LogLine("keymap A-slot key=%d vk=0x%02X GetData t=%d v=%d", (int)slotKey,
                        (unsigned)atkVk, (int)t, (int)v);
            } else {
                LogLine("keymap A-slot key=%d vk=0x%02X empty/None", (int)slotKey, (unsigned)atkVk);
            }
        }

        // ② 回退：键表里的 BasicActionAttack，或 Ctrl 上若绑了普攻。
        if (!fk && getKey) {
            __try {
                unityKey = getKey(gFkm, kFuncTypeBasicAction, kFkmBasicActionAttack,
                                  gMiGetKeyByFunc);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                unityKey = 0;
            }
            if (unityKey > 0 && getData) {
                __try {
                    fk = getData(gFkm, unityKey, gMiGetDataByKeyCode);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    fk = nullptr;
                }
                int32_t t = -1, v = -1;
                if (fk && ReadFkFields(fk, &t, &v)) {
                    LogLine("keymap fallback GetKeyByFunc(5,52)→%d GetData t=%d v=%d",
                            (int)unityKey, (int)t, (int)v);
                    if (t != kFuncTypeBasicAction || v != kFkmBasicActionAttack) fk = nullptr;
                } else {
                    fk = nullptr;
                }
            }
        }

        if (!fk && getData) {
            void* ctrlFk = nullptr;
            __try {
                ctrlFk = getData(gFkm, 55, gMiGetDataByKeyCode);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ctrlFk = nullptr;
            }
            int32_t t = -1, v = -1;
            if (ctrlFk && ReadFkFields(ctrlFk, &t, &v)) {
                LogLine("keymap Ctrl(55) t=%d v=%d", (int)t, (int)v);
                if (t == kFuncTypeBasicAction && v == kFkmBasicActionAttack) {
                    fk = ctrlFk;
                    unityKey = 55;
                }
            }
        }
    }

    // ③ 最后：合成 5/52（无键位表 / FKM 未绑上时）
    if (!fk) {
        void* klass = x::runtime::il2cpp::FindClass("", kFuncKeyClass);
        const auto& e = x::runtime::il2cpp::Get();
        if (!klass || !e.objectNew) {
            LogLine("FuncKey alloc miss klass=%p objectNew=%p", klass, e.objectNew);
            return false;
        }
        fk = x::runtime::il2cpp::AllocObject(klass);
        if (!fk) return false;
        auto ctor = FnFromMi<FnFuncKeyCtor>(gMiFuncKeyCtor, kRvaFuncKeyCtor);
        if (!ctor) return false;
        __try {
            ctor(fk, kFuncTypeBasicAction, kFkmBasicActionAttack, gMiFuncKeyCtor);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        syn = true;
        if (unityKey <= 0) unityKey = slotKey > 0 ? slotKey : 55;
        LogLine("FuncKey synthetic t=5 v=52 (A-slot+keymap miss)");
    }

    if (!PinAttackFk(fk)) return false;
    gResolvedUnityKey = unityKey;
    ReadFkFields(gAttackFk, &gResolvedFkType, &gResolvedFkValue);
    gFkSynthetic = syn;
    gLastFkResolveMs = now;
    LogLine("attack FuncKey ready unityKey=%d t=%d v=%d syn=%d slot=A", (int)gResolvedUnityKey,
            (int)gResolvedFkType, (int)gResolvedFkValue, syn ? 1 : 0);
    x::runtime::anchor_lamps::Set(
        "AttackFK",
        gMiOnFuncKey ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                     : x::runtime::anchor_lamps::AnchorLampCode::Degraded,
        gMiOnFuncKey ? "OnFuncKey MI" : "RVA fallback");
    return true;
}

struct FireJob {
    bool ok = false;
    bool isUp = false;
    const char* err = "?";
};

void FireJobOnMain(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("attack.FireJob");
    auto* job = reinterpret_cast<FireJob*>(user);
    ports::player_combat::CombatCtx ctx{};
    if (!ports::player_combat::QueryCombatCtx(ctx) || !LooksLikeHeapPtr(ctx.localUser)) {
        job->err = "no LocalUser";
        return;
    }
    if (!EnsureAttackFkOnMain() || !LooksLikeHeapPtr(gAttackFk)) {
        job->err = "no FuncKey";
        return;
    }

    EnsureMethodInfos();
    auto fn = FnFromMi<FnOnFuncKey>(gMiOnFuncKey, kRvaOnFuncKey);
    if (!fn) {
        job->err = "no OnFuncKey";
        return;
    }

    const int32_t inputType = job->isUp ? kKeyInputUp : kKeyInputDown;
    __try {
        fn(ctx.localUser, inputType, gAttackFk, 0u, gMiOnFuncKey);
        job->ok = true;
        job->err = "ok";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
        job->err = "SEH";
        ClearAttackFk();
    }
}

bool InvokeFire(bool isUp) {
    FireJob job{};
    job.isUp = isUp;
    if (!x::runtime::main_thread::InvokeAndWait(&FireJobOnMain, &job, kFireJobWaitMs,
                                               x::runtime::main_thread::JobPrio::High)) {
        LogLine("OnFuncKey pump timeout up=%d", isUp ? 1 : 0);
        return false;
    }
    return job.ok;
}

// 同一次主线程泵：Down 紧接 Up，消 pending 跨 tick（攻击加速路径）。
// 同帧连打探针已关停（实测服端 lastHitted 仍钉 30ms，N>1 不增伤）。
struct FirePulseJob {
    bool downOk = false;
    bool upOk = false;
    const char* err = "?";
};

void FirePulseOnMain(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("attack.FirePulse");
    auto* job = reinterpret_cast<FirePulseJob*>(user);
    ports::player_combat::CombatCtx ctx{};
    if (!ports::player_combat::QueryCombatCtx(ctx) || !LooksLikeHeapPtr(ctx.localUser)) {
        job->err = "no LocalUser";
        return;
    }
    if (!EnsureAttackFkOnMain() || !LooksLikeHeapPtr(gAttackFk)) {
        job->err = "no FuncKey";
        return;
    }

    EnsureMethodInfos();
    auto fn = FnFromMi<FnOnFuncKey>(gMiOnFuncKey, kRvaOnFuncKey);
    if (!fn) {
        job->err = "no OnFuncKey";
        return;
    }

    __try {
        fn(ctx.localUser, kKeyInputDown, gAttackFk, 0u, gMiOnFuncKey);
        job->downOk = true;
        fn(ctx.localUser, kKeyInputUp, gAttackFk, 0u, gMiOnFuncKey);
        job->upOk = true;
        job->err = "ok";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->err = "SEH";
        ClearAttackFk();
    }
}

bool InvokeFirePulse() {
    FirePulseJob job{};
    if (!x::runtime::main_thread::InvokeAndWait(&FirePulseOnMain, &job, kFireJobWaitMs,
                                               x::runtime::main_thread::JobPrio::High)) {
        LogLine("OnFuncKey pulse pump timeout");
        return false;
    }
    if (job.downOk && !job.upOk) {
        (void)InvokeFire(true);
        LogLine("OnFuncKey pulse Up miss → ForceUp err=%s", job.err ? job.err : "?");
        return false;
    }
    return job.downOk && job.upOk;
}

void FlushPendingUp(DWORD now) {
    if (!gPendingUp.load(std::memory_order_acquire)) return;
    const DWORD due = gUpDueMs.load(std::memory_order_relaxed);
    if (static_cast<int>(now - due) < 0) return;
    gPendingUp.store(false, std::memory_order_release);
    if (!InvokeFire(true)) {
        x::runtime::LogWThrottled(51, 3000, "Attack", "OnFuncKey Up fail");
    }
}

}  // namespace

void Init() {
    bool expected = false;
    if (!gInited.compare_exchange_strong(expected, true)) return;

    // 首次锚定要自旋至多一个系统 tick；放在 worker 线程 Init 里，别落到 Unity 主线程泵上。
    x::runtime::WarmUpClock();
    gAttackVk.store(kDefaultAttackVk);
    gConfigIntervalMs.store(kDefaultIntervalMs);
    gEffectiveIntervalMs.store(kDefaultIntervalMs);
    gAttackHoldMs.store(xcat::kAttackHoldDefaultMs);
    gSmartInterval.store(false);
    gLastFireMs.store(0);
    gFireOk.store(0);
    gFireFail.store(0);
    gFireSoft.store(0);
    gRateWindowStart = 0;
    gFaceDx.store(0.f);
    gLastFaceSign.store(0);
    gPendingUp.store(false);
    gFireSuppressed.store(false);
    gFkm = nullptr;
    gLastFkmRebind = 0;
    ClearAttackFk();
    OpenLog();
    RefreshEffectiveInterval(true, true);
    LogLine(
        "attack_input_port ready path=OnFuncKey(A-slot→fallback 5/52) face=SetInput(±1,0) "
        "hold=min(%ums,interval) animBusy=%ums clock=NowMs(1ms)",
        (unsigned)gAttackHoldMs.load(), (unsigned)kAttackAnimBusyMs);
}

void Shutdown() {
    ForceRelease();
    ClearAttackFk();
    gInited.store(false);
}

void SetAttackVk(WORD vk) {
    if (vk) gAttackVk.store(vk);
}

WORD GetAttackVk() { return gAttackVk.load(); }

void SetIntervalMs(DWORD ms) {
    const DWORD clamped = ClampAttackIntervalMs(ms);
    const DWORD prev = gConfigIntervalMs.exchange(clamped, std::memory_order_relaxed);
    // ApplyControl 每帧下发相同间隔：勿 force 重抽 smart，否则改无关开关会打乱刀速。
    const bool changed = prev != clamped;
    RefreshEffectiveInterval(/*forceSmartRandom=*/changed &&
                                 gSmartInterval.load(std::memory_order_relaxed),
                             changed);
}

DWORD GetIntervalMs() { return gEffectiveIntervalMs.load(); }

void SetAttackHoldMs(DWORD ms) {
    const DWORD clamped = xcat::ClampAttackHoldMs(ms);
    const DWORD prev = gAttackHoldMs.exchange(clamped, std::memory_order_relaxed);
    if (prev == clamped) return;
    LogLine("hold set %ums -> %ums (实际取 min(hold,interval)；加速开时走 pulse 不参与)",
            (unsigned)prev, (unsigned)clamped);
}

DWORD GetAttackHoldMs() { return gAttackHoldMs.load(std::memory_order_relaxed); }

void SetSmartInterval(bool on) {
    const bool prev = gSmartInterval.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    RefreshEffectiveInterval(on, true);
}

bool GetSmartInterval() { return gSmartInterval.load(std::memory_order_relaxed); }

bool FaceToward(float dx) {
    gFaceDx.store(dx, std::memory_order_relaxed);
    return true;
}

struct FaceJob {
    int inputX = 0;
    bool ok = false;
    int maBefore = -1;
    int maAfter = -1;
    char fail[48]{};
};

void FaceSetInputJobFn(void* p) {
    auto* job = static_cast<FaceJob*>(p);
    if (!job) return;
    job->ok = false;
    job->fail[0] = '\0';

    player_combat::CombatCtx ctx{};
    if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok || !LooksLikeHeapPtr(ctx.localUser)) {
        snprintf(job->fail, sizeof(job->fail), "no_lu");
        return;
    }
    void* vc = nullptr;
    __try {
        vc = ReadPtr(ctx.localUser, kOffVecCtrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        vc = nullptr;
    }
    if (!LooksLikeHeapPtr(vc)) {
        snprintf(job->fail, sizeof(job->fail), "no_vc");
        return;
    }

    EnsureMethodInfos();
    auto fn = FnFromMi<FnSetInput>(gMiSetInput, kRvaVecCtrlSetInput);
    if (!fn) {
        snprintf(job->fail, sizeof(job->fail), "no_setinput");
        return;
    }

    __try {
        job->maBefore = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->maBefore = -1;
    }

    __try {
        // 瞬时 ±1 只为 OnResolve 改朝向 bit；同帧立刻 SetInput(0,0) 释放走路锁存。
        // 08-04 换包后不能再赌「下一帧 KeyPad 会盖回」——BIN：出完刀 InputX 粘住 → 走不动。
        fn(vc, job->inputX, 0, gMiSetInput);
        fn(vc, 0, 0, gMiSetInput);
        job->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(job->fail, sizeof(job->fail), "seh");
        return;
    }

    __try {
        job->maAfter = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->maAfter = -1;
    }
}

bool ApplyFaceNow() {
    const float dx = gFaceDx.load(std::memory_order_relaxed);
    if (!std::isfinite(dx) || std::fabs(dx) < kFaceDeadzone) return true;

    const int want = (dx < 0.f) ? -1 : 1;
    const int last = gLastFaceSign.load(std::memory_order_relaxed);
    // 怪贴身穿过：dx 在 ±20 间抖会每刀翻面；同号且未走出 sticky 则跳过。
    if (last == want && std::fabs(dx) < kFaceStickyPx) return true;

    if (!runtime::main_thread::Ensure()) {
        x::runtime::LogWThrottled(51, 3000, "Attack", "face SetInput pump missing");
        return false;
    }

    FaceJob job{};
    job.inputX = want;
    if (!runtime::main_thread::InvokeAndWait(&FaceSetInputJobFn, &job, kFaceJobWaitMs,
                                            runtime::main_thread::JobPrio::High)) {
        x::runtime::LogWThrottled(51, 3000, "Attack", "face SetInput timeout dx=%.0f", dx);
        return false;
    }
    if (!job.ok) {
        x::runtime::LogWThrottled(51, 3000, "Attack", "face SetInput fail=%s dx=%.0f",
                                  job.fail[0] ? job.fail : "?", dx);
        return false;
    }
    gLastFaceSign.store(want, std::memory_order_relaxed);

    static uint32_t sFace = 0;
    if (sFace < 32) {
        ++sFace;
        LogLine("face SetInput x=%d dx=%.0f ma=%d→%d faceBit=%d ok=1", job.inputX, dx, job.maBefore,
                job.maAfter, job.maAfter >= 0 ? (job.maAfter & 1) : -1);
    }
    return true;
}

DWORD EffectiveAnimBusyMs() {
    const int ov = gAnimBusyOverrideMs.load(std::memory_order_relaxed);
    if (ov < 0) return kAttackAnimBusyMs;
    return static_cast<DWORD>(ov);
}

bool MotionBusy() {
    const DWORD last = gLastFireMs.load();
    if (!last) return false;
    const DWORD now = NowMs();
    // 仅挡动作前摇，允许面板间隔 < animBusy 时继续出刀。
    if ((now - last) < EffectiveAnimBusyMs()) return true;
    if (gPendingUp.load(std::memory_order_acquire)) return true;
    return false;
}

void SetAnimBusyOverrideMs(int ms) {
    gAnimBusyOverrideMs.store(ms, std::memory_order_relaxed);
}

void SetImmediateUp(bool on) {
    const bool prev = gImmediateUp.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    if (on) {
        // 切到 pulse：清掉可能挂着的异步 Up，避免粘键。
        if (gPendingUp.exchange(false, std::memory_order_acq_rel)) {
            (void)InvokeFire(true);
        }
        LogLine("immediate Up on (attack accel: Down+Up same pump, no pending)");
    } else {
        LogLine("immediate Up off (restore async hold)");
    }
}

bool CanFirePrimary() {
    if (gFireSuppressed.load(std::memory_order_acquire)) return false;
    // 泵拥堵作为软门的一部分：让战斗循环走 pace_wait 软路径，而不是把背压跳刀
    // 误判成 OnFuncKey 硬失败（TryFirePrimary 里同样有此闸做直调兜底）。
    if (x::runtime::main_thread::IsCongested()) return false;
    const DWORD now = NowMs();
    FlushPendingUp(now);
    return !SoftBlocked(now);
}

bool TryFirePrimary() {
    const DWORD now = NowMs();
    MaybeLogRate(now);
    FlushPendingUp(now);

    // ExternalPause / buffs：硬闸，避免同 tick 已过 combat 顶层检查仍 OnFuncKey。
    if (gFireSuppressed.load(std::memory_order_acquire)) {
        gFireSoft.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 出刀门 = 面板有效间隔；不再与 animBusy 取 max。
    // 软拒绝不计 fail——加速 5ms 时 Recover→Firing 同 tick 空点会刷出假 fail≈两成。
    if (SoftBlocked(now)) {
        gFireSoft.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 主线程泵拥堵：软跳过本刀，不建 job、不阻塞——proactive backpressure，断开
    // 「灌爆→job timeout→重试→更灌」的死循环（该死循环也是泵侧 GC 压力的放大器）。
    if (x::runtime::main_thread::IsCongested()) {
        gFireSoft.fetch_add(1, std::memory_order_relaxed);
        x::runtime::LogWThrottled(52, 3000, "Attack", "pump congested — skip fire (backpressure)");
        return false;
    }

    (void)ApplyFaceNow();

    const bool immediate = gImmediateUp.load(std::memory_order_relaxed);
    if (immediate) {
        if (!InvokeFirePulse()) {
            gFireFail.fetch_add(1, std::memory_order_relaxed);
            x::runtime::LogWThrottled(50, 3000, "Attack", "OnFuncKey pulse fail");
            return false;
        }
        // 无 pending：下一刀只受 interval 门控。
        gPendingUp.store(false, std::memory_order_release);
    } else {
        if (!InvokeFire(false)) {
            gFireFail.fetch_add(1, std::memory_order_relaxed);
            x::runtime::LogWThrottled(50, 3000, "Attack", "OnFuncKey Down fail");
            return false;
        }
        const DWORD hold = AttackHoldMs();
        gPendingUp.store(true, std::memory_order_release);
        gUpDueMs.store(NowMs() + hold, std::memory_order_relaxed);
    }

    gLastFireMs.store(now, std::memory_order_relaxed);
    gFireOk.fetch_add(1, std::memory_order_relaxed);

    static uint32_t sFaceLog = 0;
    if (sFaceLog < 12) {
        ++sFaceLog;
        if (immediate) {
            LogLine("OnFuncKey pulse ok unityKey=%d t=%d v=%d syn=%d faceDx=%.0f hold=0",
                    (int)gResolvedUnityKey, (int)gResolvedFkType, (int)gResolvedFkValue,
                    gFkSynthetic ? 1 : 0, gFaceDx.load());
        } else {
            const DWORD hold = AttackHoldMs();
            LogLine("OnFuncKey ok unityKey=%d t=%d v=%d syn=%d faceDx=%.0f hold=%ums asyncUp=1",
                    (int)gResolvedUnityKey, (int)gResolvedFkType, (int)gResolvedFkValue,
                    gFkSynthetic ? 1 : 0, gFaceDx.load(), (unsigned)hold);
        }
    }
    if (gSmartInterval.load(std::memory_order_relaxed)) {
        gEffectiveIntervalMs.store(
            RandomSmartIntervalMs(gConfigIntervalMs.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
    }
    return true;
}

void TickReleases() { FlushPendingUp(NowMs()); }

void SetFireSuppressed(bool on) {
    const bool was = gFireSuppressed.exchange(on, std::memory_order_acq_rel);
    if (on && !was) ForceRelease();
}

bool IsFireSuppressed() { return gFireSuppressed.load(std::memory_order_acquire); }

bool WaitFireIdle(DWORD timeoutMs, DWORD settleAfterFireMs) {
    ForceRelease();
    const DWORD t0 = NowMs();
    for (;;) {
        FlushPendingUp(NowMs());
        const DWORD now = NowMs();
        const DWORD last = gLastFireMs.load(std::memory_order_relaxed);
        const bool pending = gPendingUp.load(std::memory_order_acquire);
        const bool recent = last && settleAfterFireMs && (now - last) < settleAfterFireMs;
        if (!pending && !recent) return true;
        if (timeoutMs == 0 || now - t0 >= timeoutMs) return false;
        Sleep(1);
    }
}

void ForceRelease() {
    // 朝向已不持 L/R；仍清一次，避免旧路径残留键。
    ports::input::ForceReleaseVk(VK_LEFT);
    ports::input::ForceReleaseVk(VK_RIGHT);
    // 关 F5 / ExternalPause：强制清 VecCtrl 走路锁存（InputX 粘住 → 走不动）。
    if (runtime::main_thread::Ensure()) {
        FaceJob job{};
        job.inputX = 0;
        if (runtime::main_thread::InvokeAndWait(&FaceSetInputJobFn, &job, kFaceJobWaitMs,
                                               runtime::main_thread::JobPrio::High) &&
            job.ok) {
            gLastFaceSign.store(0, std::memory_order_relaxed);
            gFaceDx.store(0.f, std::memory_order_relaxed);
        }
    }
    if (!gPendingUp.exchange(false, std::memory_order_acq_rel)) return;
    (void)InvokeFire(true);
}

void ClearWalkLatchMainThread() {
    (void)x::runtime::main_thread::AssertOnPumpThread("attack.ClearWalkLatch");
    FaceJob job{};
    job.inputX = 0;
    FaceSetInputJobFn(&job);
    if (job.ok) {
        gLastFaceSign.store(0, std::memory_order_relaxed);
        gFaceDx.store(0.f, std::memory_order_relaxed);
    }
}

}  // namespace x::features::ports::attack
