// Classic TWMS — primary attack via UserLocal.OnFuncKey(绑定键 FuncKey)。
// 默认读 A 键（InputSystem.Key.A=15）上绑定的技能/动作；空才回退 BasicActionAttack(5/52)。
// BIN: OnKey(Ctrl) can log ok with pktSum=0; OnFuncKey 才是掉血真源。
// 朝向：VecCtrl.SetInput(±1,0) → OnResolveMoveAction（同帧立刻清 0）。
// 拟人走路：默认内部输入 —— 往 InputSystem 的 Keyboard 设备灌方向键状态（unity_kbd_port）。
//   走位是每帧轮询设备状态，不是事件驱动；所以 latch / SetInput / PackState bit0 这些下游写值
//   都会被上游门闩清掉（01:38 BIN：hook=1 但 travel=0 / vx=0）。只有喂设备状态才是真源。
//   XCAT_WALK_KBD=0 回落 Win32 SendInput（需前台焦点）；XCAT_WALK_KP=1 走已证伪的 PackBit 试验路。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_input_port.h"

#include "ground_spoof.h"
#include "input_port.h"
#include "key_macro_bin.h"
#include "player_combat_port.h"
#include "unity_kbd_port.h"
#include "../attack_accel/attack_accel.h"
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

constexpr uint32_t kRvaOnFuncKey = 0x10840C0;  // remounted 2026-08-06
constexpr uint32_t kRvaGetKeyByFunc = 0x1653300;  // remounted 2026-08-06
constexpr uint32_t kRvaGetDataByKeyCode = 0x1652600;  // remounted 2026-08-06
constexpr uint32_t kRvaFuncKeyCtor = 0x164A9D0;  // remounted 2026-08-06: .ctor(FuncType,int)
// 写 InputX/Y + 内联 OnResolveMoveAction（朝向）；见 docs/features/protocol/MoveElem字段.md
constexpr uint32_t kRvaVecCtrlSetInput = 0x11BE2A0;  // remounted 2026-08-06
// KeyPad.SetFields / PackState（IDA 2026-08-07；keypad_walk_bin match BASE）。
constexpr uint32_t kRvaKeyPadSetFields = 0x16C9150;
constexpr uint32_t kRvaKeyPadPackState = 0x16C9170;
constexpr size_t kOffKeyPadSlot4 = 0x178;
// Query 用 PackState 返回值 bit0：even→latchX=+1，odd→−1（MoveElem §11.10）。
// 反了设 XCAT_KP_FLIP=1。

// 方法哈希（dump.cs · remount 2026-08-06）
constexpr char kHashOnFuncKey[] =
    "be324137b6b1c45801c55f441c77d215a8bff0130fa1671e92983c4a8cf3c54";
constexpr char kHashGetKeyByFunc[] =
    "fa3b4c14927d326f246c77493643f90c2b894b6f794a6cfbea6a4c6cd9ab618";
constexpr char kHashGetDataByKeyCode[] =
    "f9f41a36e032de163e54e45570ae92982f78e2e068a280be4e696256fe842a4";
constexpr char kHashVecCtrlSetInput[] =
    "a67b5fb0eec1f61f158d6c192259af8cb7085d28ab74439ab2ee05efcfe8622";
constexpr char kVecCtrlClass[] =
    "e0eb55b82f10cb9eeb9424eb3aadf1450a014afa564bc55c3739b2909abfbbc";
constexpr char kActorBaseClass[] =
    "edc85ce203606bdb549e5fb94458b1d2d11ce78034d24d41e39a54c0288d38e";
// FKM remounted 2026-08-06 (owns GetKeyByFunc / GetDataByKeyCode).
constexpr char kFkmClass[] =
    "bccf462f59fa3ac757dd30992984c99e5bda74964a10a02eb5a53a54f02dd61";
constexpr char kFuncKeyClass[] =
    "aee2472baeb766e84b81b7e54686e57dcb9a913f9773d94886c682c410ab778";
// KeyPad 单例（与 keypad_walk_bin 同源；RO hook 已证 Slot4=PackState BASE）。
constexpr char kHashKeyPadClass[] =
    "e800b5ba8a481ffef6d7ed3b23a7332f699ff556d4e7e94d40df035190a7b44";

// Actor.VecCtrl / VecCtrl.MoveAction / FuncKey.type|value：hash → field_get_offset
constexpr char kHashUserVecCtrl[] =
    "<acb8946a384ed398c4ad9268349397cf4f6e65cf136078ebc9aa26a949efd41>k__BackingField";
constexpr char kHashVcMoveAction[] =
    "fa93e903eebde8b6fd77060143b0b2f1293e84eeba873dae2034090150daad4";
constexpr char kHashFkType[] =
    "b3635d661b985fc4a0eb47a782da14d314fe7933f628cfcbe826b3dd1213349";
constexpr char kHashFkValue[] =
    "ca24f1d7aec9f2b9ad4058e8356b09bfe8801a61e2fcd5cf728ab2e96b68105";

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
// 逐刀取证用：上次 ApplyFaceNow 读到的引擎 moveAction 与本次的落地/跳过原因。
std::atomic<int> gFaceLastMa{-1};
std::atomic<int> gFaceLastWhy{-1};
std::atomic<int> gWalkHeld{0};      // 拟人走路锁存：-1/0/+1
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

// 出刀结果探针（详见 attack_input_port.h 的 FireOutcomeDebug）。只在主线程泵上写，
// 面板/日志线程读，所以用 atomic。-2 = 没读到，别当业务语义。
constexpr int kBusyUnread = -2;
std::atomic<int> gFireBusy0{kBusyUnread};
std::atomic<int> gFireBusy1{kBusyUnread};

int ReadActionBusy(void* localUser) {
    int v = 0;
    return attack_accel::QueryActionBusy(localUser, v) ? v : kBusyUnread;
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
    // 站立伪装：种台/摘台必须夹在这一次同步派发的两侧（OnFuncKey→OnAttack→TryDoing*
    // 全在本 job 内跑完），跨出去物理就会看见这块台。__try 函数里不能放带析构的对象
    // （C2712），所以只能显式配对，且 __except 路径也必须摘。
    // 只夹 Down：技能派发在 Down 那一拍，Up 种台既没用又白开一次窗口；而且异步 Up
    // 会把 Down 记下的忙位快照冲掉，抢在 combat.log 读之前 → 打点串味。
    const bool spoof = !job->isUp;
    if (spoof) {
        gFireBusy0.store(ReadActionBusy(ctx.localUser), std::memory_order_relaxed);
        (void)ground_spoof::PlantForFire(ctx.localUser);
    }
    __try {
        fn(ctx.localUser, inputType, gAttackFk, 0u, gMiOnFuncKey);
        job->ok = true;
        job->err = "ok";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
        job->err = "SEH";
        ClearAttackFk();
    }
    if (spoof) {
        // 派发是同步的，返回时引擎已经决定接不接这一刀，忙位就是判决书。
        gFireBusy1.store(ReadActionBusy(ctx.localUser), std::memory_order_relaxed);
        ground_spoof::UnplantAfterFire();
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

    // 站立伪装：Down/Up 同泵，整段夹在种台窗口内（理由同 FireJobOnMain）。
    gFireBusy0.store(ReadActionBusy(ctx.localUser), std::memory_order_relaxed);
    (void)ground_spoof::PlantForFire(ctx.localUser);
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
    gFireBusy1.store(ReadActionBusy(ctx.localUser), std::memory_order_relaxed);
    ground_spoof::UnplantAfterFire();
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
        "walk=Win32 (XCAT_WALK_KP=1=PackBit trial) hold=min(%ums,interval) "
        "animBusy=%ums clock=NowMs(1ms)",
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

bool StopWalk();  // ApplyFaceNow 出刀前清拟人锁存

constexpr size_t kOffVcInputX = 0x50;

std::atomic<bool> gWalkTickArmed{false};
std::atomic<WORD> gOsWalkVk{0};  // 默认 Win32 键；0=无
std::atomic<int> gWalkDriveMode{0};  // 0=未定 1=PackBit 2=OS 3=Kbd(InputSystem 设备状态)
void* gKeyPadKlass = nullptr;
void* gKeyPadSing = nullptr;
DWORD gKeyPadSingMs = 0;
using FnPackState = uint32_t(__fastcall*)(void* self, const void* methodInfo);
std::atomic<FnPackState> gPackOrig{nullptr};
void** gPackSlot = nullptr;

struct FaceJob {
    int inputX = 0;
    bool ok = false;
    bool kpOk = false;
    bool kbdOk = false;
    bool wantKbd = false;
    int maBefore = -1;
    int maAfter = -1;
    char fail[48]{};
};

bool EnvFlagOn(const char* name) {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

bool EnvFlagOff(const char* name) {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return false;
    return buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' || buf[0] == 'f' || buf[0] == 'F';
}

// PackBit 已证伪（01:38）；仅显式 XCAT_WALK_KP=1 才开。
bool WantKeyPadWalk() { return EnvFlagOn("XCAT_WALK_KP"); }
bool WantKpFlip() { return EnvFlagOn("XCAT_KP_FLIP"); }
// 内部输入（Keyboard 设备状态）：默认开，XCAT_WALK_KBD=0 才回落 Win32。
bool WantKbdWalk() { return !EnvFlagOff("XCAT_WALK_KBD"); }

HWND FindUnityGameHwnd() {
    struct Ctx {
        DWORD pid = 0;
        HWND unity = nullptr;
        HWND fallback = nullptr;
    } ctx{GetCurrentProcessId(), nullptr, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != c->pid || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd)) return TRUE;
            char cls[64]{};
            GetClassNameA(hwnd, cls, sizeof(cls));
            if (_stricmp(cls, "UnityWndClass") == 0) {
                c->unity = hwnd;
                return FALSE;
            }
            if (!c->fallback) {
                RECT r{};
                GetWindowRect(hwnd, &r);
                if ((r.right - r.left) > 200 && (r.bottom - r.top) > 200) c->fallback = hwnd;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.unity ? ctx.unity : ctx.fallback;
}

// 默认 Win32 走路；XCAT_WALK_KP=1 失败时也回落这里。
bool SendVkKeyEvent(WORD vk, bool down) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(scan);
    in.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (down ? 0u : KEYEVENTF_KEYUP);
    in.ki.dwExtraInfo = 0;
    const UINT n = SendInput(1, &in, sizeof(INPUT));
    x::features::ports::key_macro_bin::NoteOsSendInput(vk, down, n);
    return n == 1;
}

void PostVkToGameWindow(WORD vk, bool down) {
    HWND hwnd = FindUnityGameHwnd();
    if (!hwnd || !IsWindow(hwnd)) return;
    const UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16) | (down ? 0 : (1 << 30) | (1 << 31)) |
                      (1 << 24);  // extended
    PostMessageW(hwnd, msg, vk, lp);
}

void OsWalkKeyUp(WORD vk) {
    if (!vk) return;
    (void)SendVkKeyEvent(vk, false);
    PostVkToGameWindow(vk, false);
}

void OsWalkKeyDown(WORD vk) {
    if (!vk) return;
    (void)SendVkKeyEvent(vk, true);
    PostVkToGameWindow(vk, true);
}

bool EnsureOsWalkDir(int inputX) {
    const WORD want = (inputX < 0) ? VK_LEFT : VK_RIGHT;
    const WORD other = (inputX < 0) ? VK_RIGHT : VK_LEFT;
    const WORD cur = gOsWalkVk.load(std::memory_order_acquire);
    if (cur == other) {
        OsWalkKeyUp(other);
        gOsWalkVk.store(0, std::memory_order_release);
    }
    if (gOsWalkVk.load(std::memory_order_acquire) != want) {
        OsWalkKeyDown(want);
        gOsWalkVk.store(want, std::memory_order_release);
    } else {
        // 锁存已按但 OS 键态丢了（捡物会吞 DN）→ 补按。仅前台窗口可信：
        // 失焦时 GetAsyncKeyState 常为 0，每 tick 补 DN 会回到 ~55/s 宏味（847b21 fg=0）。
        HWND game = FindUnityGameHwnd();
        if (game && GetForegroundWindow() == game &&
            (GetAsyncKeyState(want) & 0x8000) == 0) {
            OsWalkKeyDown(want);
        }
    }
    return true;
}

void ClearOsWalkKeys() {
    const WORD cur = gOsWalkVk.exchange(0, std::memory_order_acq_rel);
    if (cur) OsWalkKeyUp(cur);
    // 双清，避免方向切换残留
    OsWalkKeyUp(VK_LEFT);
    OsWalkKeyUp(VK_RIGHT);
}

FnSetInput ResolveSetInputFn() {
    return FnFromMi<FnSetInput>(gMiSetInput, kRvaVecCtrlSetInput);
}

// KeyPad 单例：klass → static_fields[0]（与 keypad_walk_bin / MoveElem §11.9 一致）。
void* EnsureKeyPadSingleton() {
    const DWORD now = GetTickCount();
    if (gKeyPadSing && LooksLikeHeapPtr(gKeyPadSing) && (now - gKeyPadSingMs) < 2000) {
        void* k = nullptr;
        __try {
            k = *reinterpret_cast<void**>(gKeyPadSing);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            k = nullptr;
        }
        if (LooksLikeHeapPtr(k)) return gKeyPadSing;
        gKeyPadSing = nullptr;
    }
    if (!x::runtime::il2cpp::Ensure()) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!gKeyPadKlass) gKeyPadKlass = x::runtime::il2cpp::FindClass("", kHashKeyPadClass);
    if (!gKeyPadKlass || !e.classStaticData) return nullptr;
    void* sf = nullptr;
    __try {
        sf = e.classStaticData(gKeyPadKlass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sf = nullptr;
    }
    if (!LooksLikeHeapPtr(sf)) return nullptr;
    void* sing = nullptr;
    __try {
        sing = *reinterpret_cast<void**>(sf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sing = nullptr;
    }
    if (!LooksLikeHeapPtr(sing)) return nullptr;
    gKeyPadSing = sing;
    gKeyPadSingMs = now;
    return sing;
}

bool PatchVtableMethodPtr(void** slot, void* hook, void** outOrig) {
    if (!slot || !hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    void* prev = nullptr;
    __try {
        prev = *slot;
        *slot = hook;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(slot, sizeof(void*), old, &old);
        return false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    if (outOrig) *outOrig = prev;
    return true;
}

void RestoreVtableMethodPtr(void** slot, void* orig) {
    if (!slot || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    __try {
        *slot = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// Query/GetState 虚调 Slot4=PackState：改返回值 bit0 → 锁存 X（比 SetFields 更靠近消费点）。
uint32_t __fastcall PackStateDriveHook(void* self, const void* methodInfo) {
    FnPackState prev = gPackOrig.load(std::memory_order_acquire);
    uint32_t r = 0;
    if (prev) {
        __try {
            r = prev(self, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            r = 0;
        }
    } else {
        auto real = AtRva<FnPackState>(kRvaKeyPadPackState);
        if (real) {
            __try {
                r = real(self, methodInfo);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                r = 0;
            }
        }
    }
    const int dir = gWalkHeld.load(std::memory_order_acquire);
    if (dir == 1 || dir == -1) {
        bool wantOdd = (dir < 0);  // 左 odd / 右 even
        if (WantKpFlip()) wantOdd = !wantOdd;
        if (wantOdd) r |= 1u;
        else r &= ~1u;
    }
    return r;
}

bool InstallPackDriveHook() {
    // 已拆除：kOffKeyPadSlot4 所指的类**不是** KeyPad 而是 Rand32，slot4 = `Rand32.Random()`
    // （运行期 origRva=0x16C9170 与 dump 对上；反编译为 xorshift：三状态字、移位 13/19·4/25·8/11；
    //  Random() 正是 Rand32 首个自有虚方法，恰落 slot4）。
    // 于是 PackStateDriveHook 那句 `r |= 1u / r &= ~1u` 不是「锁存方向」，而是把游戏伪随机数的
    // 最低位在走路期间钉成定值 —— 污染 RNG 流，且当初「PackBit 无效」根本没测到真的 PackState。
    // 走位真源已确认是 InputSystem 事件（见 unity_kbd_port.h），此路彻底作废，默认禁止再装。
    // 留一道显式逃生门只为将来做对照实验，名字里写明代价，不许当普通开关用。
    if (!EnvFlagOn("XCAT_WALK_KP_CORRUPT_RNG_OK")) {
        LogLine("walkW PackState hook REFUSED — slot4=Rand32.Random()，非 PackState（会污染 RNG）");
        return false;
    }

    if (gPackSlot) return true;
    void* sing = EnsureKeyPadSingleton();
    if (!sing) return false;
    void* iklass = nullptr;
    __try {
        iklass = *reinterpret_cast<void**>(sing);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!LooksLikeHeapPtr(iklass)) return false;
    void** slot =
        reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(iklass) + kOffKeyPadSlot4);
    void* orig = nullptr;
    if (!PatchVtableMethodPtr(slot, reinterpret_cast<void*>(&PackStateDriveHook), &orig)) {
        return false;
    }
    gPackOrig.store(reinterpret_cast<FnPackState>(orig), std::memory_order_release);
    gPackSlot = slot;
    LogLine("walkW PackState Slot4 hook ON rva=0x%X (bit0→latchX)", kRvaKeyPadPackState);
    return true;
}

void UninstallPackDriveHook() {
    FnPackState orig = gPackOrig.exchange(nullptr, std::memory_order_acq_rel);
    if (gPackSlot && orig) {
        RestoreVtableMethodPtr(gPackSlot, reinterpret_cast<void*>(orig));
        LogLine("walkW PackState Slot4 hook OFF");
    }
    gPackSlot = nullptr;
}

void ApplyWalkSetInput(int inputX, FaceJob* job) {
    if (job) {
        job->ok = false;
        job->fail[0] = '\0';
        job->inputX = inputX;
    }

    player_combat::CombatCtx ctx{};
    if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok || !LooksLikeHeapPtr(ctx.localUser)) {
        if (job) snprintf(job->fail, sizeof(job->fail), "no_lu");
        return;
    }
    void* vc = nullptr;
    __try {
        vc = ReadPtr(ctx.localUser, kOffVecCtrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        vc = nullptr;
    }
    if (!LooksLikeHeapPtr(vc)) {
        if (job) snprintf(job->fail, sizeof(job->fail), "no_vc");
        return;
    }

    EnsureMethodInfos();
    auto fn = ResolveSetInputFn();
    if (!fn) {
        if (job) snprintf(job->fail, sizeof(job->fail), "no_setinput");
        return;
    }

    int maBefore = -1;
    __try {
        maBefore = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (job) job->maBefore = maBefore;

    __try {
        fn(vc, inputX, 0, gMiSetInput);
        if (job) job->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (job) snprintf(job->fail, sizeof(job->fail), "seh");
        return;
    }

    int maAfter = -1;
    __try {
        maAfter = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        maAfter = -1;
    }
    if (job) job->maAfter = maAfter;
}

void WalkLatchJobFn(void* p) {
    auto* job = static_cast<FaceJob*>(p);
    if (!job) return;
    // 设备状态必须在 SetInput 之前灌：这一帧的 KeyPad 轮询才读得到「按住」。
    if (job->wantKbd) job->kbdOk = unity_kbd::SetWalkDirOnMain(job->inputX);
    ApplyWalkSetInput(job->inputX, job);
}

// 每帧补写已迁至 unity_kbd 自管（gMask 非空挂 InputFrameTick）；走路只灌方向位。
void ArmWalkTick() {
    if (gWalkTickArmed.exchange(true, std::memory_order_acq_rel)) return;
    if (runtime::main_thread::Ensure()) {
        runtime::main_thread::SetPrePhysicsFrameTick(nullptr, nullptr);
        runtime::main_thread::SetPostPhysicsFrameTick(nullptr, nullptr);
    }
    if (WantKbdWalk() && !WantKeyPadWalk()) {
        UninstallPackDriveHook();
        ClearOsWalkKeys();  // 纯内部输入，不碰 OS 键，失焦也能走
        gWalkDriveMode.store(3, std::memory_order_relaxed);
        (void)unity_kbd::EnsureBound();
        LogLine("walkW armed (InputSystem Keyboard state; OS off; kbd-owned repush)");
    } else if (WantKeyPadWalk()) {
        const bool hooked = InstallPackDriveHook();
        gWalkDriveMode.store(hooked ? 1 : 2, std::memory_order_relaxed);
        if (hooked) {
            ClearOsWalkKeys();  // 纯 PackBit，便于失焦 BIN
            LogLine("walkW armed (PackState bit0 drive; OS off; flip=%d)", WantKpFlip() ? 1 : 0);
        } else {
            LogLine("walkW PackState hook FAIL → OS fallback");
            LogLine("walkW armed (Win32 SendInput+PostMessage → UnityWnd)");
        }
    } else {
        UninstallPackDriveHook();
        gWalkDriveMode.store(2, std::memory_order_relaxed);
        LogLine("walkW armed (Win32 SendInput+PostMessage → UnityWnd)");
    }
}

void DisarmWalkTick() {
    if (!gWalkTickArmed.exchange(false, std::memory_order_acq_rel)) return;
    ClearOsWalkKeys();
    UninstallPackDriveHook();
    if (runtime::main_thread::Ensure()) {
        (void)runtime::main_thread::InvokeAndWait(
            [](void*) { (void)unity_kbd::ReleaseAllOnMain(); }, nullptr, kFaceJobWaitMs,
            runtime::main_thread::JobPrio::High);
    }
    if (runtime::main_thread::Ensure()) {
        runtime::main_thread::SetPrePhysicsFrameTick(nullptr, nullptr);
        runtime::main_thread::SetPostPhysicsFrameTick(nullptr, nullptr);
        // InputFrameTick 由 unity_kbd 按掩码自管；走路松左右后若仍有 ↑ 等脉冲，tick 保留。
    }
    gWalkDriveMode.store(0, std::memory_order_relaxed);
    LogLine("walkW disarmed");
}

bool HoldWalk(int inputX) {
    if (inputX != -1 && inputX != 1) return StopWalk();

    ArmWalkTick();
    const int mode = gWalkDriveMode.load(std::memory_order_relaxed);
    const bool packMode = (mode == 1);
    const bool kbdMode = (mode == 3);

    // 同向已锁存：靠 unity_kbd 自管 InputFrameTick Repush 续按，勿每 combat tick 再 InvokeAndWait
    // 抢主线程（拟人进带抖动时 Hold+Stop 齐喷 → ImGui/泵卡死，upload 48610f）。
    const int held = gWalkHeld.load(std::memory_order_acquire);
    if (held == inputX && kbdMode && gWalkTickArmed.load(std::memory_order_acquire)) {
        return true;
    }

    gWalkHeld.store(inputX, std::memory_order_release);

    bool osOk = false;  // 仅在真的按了 OS 键时才置位，否则 Hold 日志里的 os= 会骗人
    if (packMode || kbdMode) {
        ClearOsWalkKeys();
    } else {
        osOk = EnsureOsWalkDir(inputX);
    }

    bool setOk = false;
    bool kbdOk = false;
    if (runtime::main_thread::Ensure()) {
        FaceJob job{};
        job.inputX = inputX;
        job.wantKbd = kbdMode;
        (void)runtime::main_thread::InvokeAndWait(&WalkLatchJobFn, &job, kFaceJobWaitMs,
                                                  runtime::main_thread::JobPrio::High);
        setOk = job.ok;
        kbdOk = job.kbdOk;
        if (job.ok) gLastFaceSign.store(inputX, std::memory_order_relaxed);
    }

    // 内部输入绑不上就别装死：回落 Win32，至少前台还能走。
    if (kbdMode && !kbdOk) {
        gWalkDriveMode.store(2, std::memory_order_relaxed);
        LogLine("walkW Kbd inject FAIL (%s) → OS fallback", unity_kbd::LastFail());
        osOk = EnsureOsWalkDir(inputX);
    }

    static DWORD sHoldLog = 0;
    const DWORD now = NowMs();
    if (!sHoldLog || now - sHoldLog > 800) {
        const DWORD dtMs = sHoldLog ? (now - sHoldLog) : 0;
        sHoldLog = now;
        HWND hwnd = FindUnityGameHwnd();
        // 补写自证：push/s 为 0 说明帧槽根本没跑（只注册成功不算数）；
        // clob/s 是被外来真实键盘事件覆盖的频率，前台应显著高于失焦。
        static uint32_t sPush = 0, sClob = 0, sRuns = 0, sDw = 0, sGuard = 0, sHc = 0;
        uint32_t push = 0, clob = 0, dw = 0, guard = 0, hc = 0;
        unity_kbd::Stats(&push, &clob, &dw, &guard, &hc);
        const uint32_t runs = runtime::main_thread::InputFrameTickRuns();
        const double sec = dtMs ? dtMs / 1000.0 : 0.0;
        LogLine("walkW Hold mode=%s dir=%d set=%d kbd=%d os=%d vk=0x%02X hwnd=%p fg=%d hook=%d "
                "grd=%d tick=h%u@%.0f/s dw=%.0f/s push=%.0f/s clob=%.0f/s guard=%.0f/s hc=%.0f/s",
                kbdMode ? "Kbd" : (packMode ? "PackBit" : "OS"), inputX, setOk ? 1 : 0,
                kbdOk ? 1 : 0, osOk ? 1 : 0,
                (unsigned)gOsWalkVk.load(std::memory_order_relaxed), (void*)hwnd,
                (GetForegroundWindow() == hwnd) ? 1 : 0, gPackSlot ? 1 : 0,
                unity_kbd::GuardActive() ? 1 : 0,
                (unsigned)runtime::main_thread::InputFrameTickHost(),
                sec > 0 ? (runs - sRuns) / sec : 0.0, sec > 0 ? (dw - sDw) / sec : 0.0,
                sec > 0 ? (push - sPush) / sec : 0.0, sec > 0 ? (clob - sClob) / sec : 0.0,
                sec > 0 ? (guard - sGuard) / sec : 0.0, sec > 0 ? (hc - sHc) / sec : 0.0);
        sPush = push;
        sClob = clob;
        sRuns = runs;
        sDw = dw;
        sGuard = guard;
        sHc = hc;
    }
    return gWalkTickArmed.load(std::memory_order_acquire) && (packMode || kbdOk || osOk);
}

bool StopWalk() {
    const int prev = gWalkHeld.exchange(0, std::memory_order_acq_rel);
    ClearOsWalkKeys();

    // 没在走：禁止空转 InvokeAndWait（Impact 空路径 / 重复 Stop 曾冻死 ImGui，见模块设计）。
    if (prev == 0) return true;

    if (runtime::main_thread::Ensure()) {
        FaceJob job{};
        job.inputX = 0;
        // 停步先松设备状态键，再清 SetInput；勿 SetFields(A=0)（even 会逼右走）。
        (void)runtime::main_thread::InvokeAndWait(
            [](void* p) {
                auto* job = static_cast<FaceJob*>(p);
                job->kbdOk = unity_kbd::ReleaseAllOnMain();
                ApplyWalkSetInput(job->inputX, job);
            },
            &job, kFaceJobWaitMs, runtime::main_thread::JobPrio::High);
    }
    static uint32_t sStop = 0;
    if (sStop < 32) {
        ++sStop;
        LogLine("walkW Stop prev=%d mode=%d keep_armed=%d", prev,
                gWalkDriveMode.load(std::memory_order_relaxed),
                gWalkTickArmed.load(std::memory_order_relaxed) ? 1 : 0);
    }
    return true;
}

bool IsWalkHeld() { return gWalkHeld.load(std::memory_order_acquire) != 0; }

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
        // 拟人 HoldWalk 另走 WalkLatchJobFn（只写不立刻清）。
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
    // 出刀朝向脉冲会清 InputX；若拟人正持走，先停，避免脉冲后粘死/方向乱。
    if (gWalkHeld.load(std::memory_order_acquire) != 0) {
        (void)StopWalk();
    }
    const float dx = gFaceDx.load(std::memory_order_relaxed);
    if (!std::isfinite(dx) || std::fabs(dx) < kFaceDeadzone) {
        gFaceLastWhy.store(1, std::memory_order_relaxed);
        return true;
    }

    const int want = (dx < 0.f) ? -1 : 1;
    const int last = gLastFaceSign.load(std::memory_order_relaxed);
    // 怪贴身穿过：dx 在 ±20 间抖会每刀翻面；同号且未走出 sticky 则跳过。
    if (last == want && std::fabs(dx) < kFaceStickyPx) {
        gFaceLastWhy.store(2, std::memory_order_relaxed);
        return true;
    }

    if (!runtime::main_thread::Ensure()) {
        gFaceLastWhy.store(3, std::memory_order_relaxed);
        x::runtime::LogWThrottled(51, 3000, "Attack", "face SetInput pump missing");
        return false;
    }

    FaceJob job{};
    job.inputX = want;
    if (!runtime::main_thread::InvokeAndWait(&FaceSetInputJobFn, &job, kFaceJobWaitMs,
                                            runtime::main_thread::JobPrio::High)) {
        gFaceLastWhy.store(4, std::memory_order_relaxed);
        x::runtime::LogWThrottled(51, 3000, "Attack", "face SetInput timeout dx=%.0f", dx);
        return false;
    }
    if (!job.ok) {
        gFaceLastWhy.store(5, std::memory_order_relaxed);
        x::runtime::LogWThrottled(51, 3000, "Attack", "face SetInput fail=%s dx=%.0f",
                                  job.fail[0] ? job.fail : "?", dx);
        return false;
    }
    gLastFaceSign.store(want, std::memory_order_relaxed);
    gFaceLastMa.store(job.maAfter, std::memory_order_relaxed);
    gFaceLastWhy.store(0, std::memory_order_relaxed);

    static uint32_t sFace = 0;
    if (sFace < 32) {
        ++sFace;
        LogLine("face SetInput x=%d dx=%.0f ma=%d→%d faceBit=%d ok=1", job.inputX, dx, job.maBefore,
                job.maAfter, job.maAfter >= 0 ? (job.maAfter & 1) : -1);
    }
    return true;
}

void FaceDebug(int* maOut, int* whyOut) {
    if (maOut) *maOut = gFaceLastMa.load(std::memory_order_relaxed);
    if (whyOut) *whyOut = gFaceLastWhy.load(std::memory_order_relaxed);
}

void FireOutcomeDebug(int* busy0, int* busy1) {
    if (busy0) *busy0 = gFireBusy0.load(std::memory_order_relaxed);
    if (busy1) *busy1 = gFireBusy1.load(std::memory_order_relaxed);
}

bool FaceNeedsFlip(float dx) {
    if (!std::isfinite(dx) || std::fabs(dx) < kFaceDeadzone) return false;
    const int last = gLastFaceSign.load(std::memory_order_relaxed);
    const int want = (dx < 0.f) ? -1 : 1;
    // 冷启动 / 关 F5 后 ForceRelease 会把 last 清 0。旧逻辑 `last==0 → 不 settle`
    // 会让首刀同拍 SetInput 翻面 + OnFuncKey，必空（BIN 21:42:48 F5 开：ma 6→7 后 52ms 出刀，无 face_settle）。
    // 未知朝向一律当需要 settle：先 ApplyFaceNow，下一拍再挥。
    if (last == 0) return true;
    return last != want;
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

bool CanFirePrimaryEx(bool ignoreCombatInterval) {
    if (gFireSuppressed.load(std::memory_order_acquire)) return false;
    if (x::runtime::main_thread::IsCongested()) return false;
    const DWORD now = NowMs();
    FlushPendingUp(now);
    if (ignoreCombatInterval) {
        // 与 TryFirePrimaryEx(ignore) 对齐：只挡还没松完的键，不挡面板间隔。
        return !gPendingUp.load(std::memory_order_acquire);
    }
    return !SoftBlocked(now);
}

bool CanFirePrimary() { return CanFirePrimaryEx(/*ignoreCombatInterval=*/false); }

bool TryFirePrimaryEx(bool ignoreCombatInterval) {
    const DWORD now = NowMs();
    MaybeLogRate(now);
    FlushPendingUp(now);

    // ExternalPause / buffs：硬闸，避免同 tick 已过 combat 顶层检查仍 OnFuncKey。
    if (gFireSuppressed.load(std::memory_order_acquire)) {
        gFireSoft.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 出刀门 = 面板有效间隔；多发普攻改走固定间隔，此处可跳过。
    // 软拒绝不计 fail——加速 5ms 时 Recover→Firing 同 tick 空点会刷出假 fail≈两成。
    if (!ignoreCombatInterval && SoftBlocked(now)) {
        gFireSoft.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 多发仍须等松键，否则粘 Down。
    if (ignoreCombatInterval && gPendingUp.load(std::memory_order_acquire)) {
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

bool TryFirePrimary() { return TryFirePrimaryEx(/*ignoreCombatInterval=*/false); }

bool TryFirePrimaryForMultiSkill() { return TryFirePrimaryEx(/*ignoreCombatInterval=*/true); }

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
    gWalkHeld.store(0, std::memory_order_release);
    DisarmWalkTick();
    // 关 F5 / ExternalPause：强制清 VecCtrl 走路锁存（InputX 粘住 → 走不动）。
    if (runtime::main_thread::Ensure()) {
        FaceJob job{};
        job.inputX = 0;
        if (runtime::main_thread::InvokeAndWait(&WalkLatchJobFn, &job, kFaceJobWaitMs,
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
    gWalkHeld.store(0, std::memory_order_release);
    DisarmWalkTick();
    FaceJob job{};
    job.inputX = 0;
    WalkLatchJobFn(&job);
    if (job.ok) {
        gLastFaceSign.store(0, std::memory_order_relaxed);
        gFaceDx.store(0.f, std::memory_order_relaxed);
    }
}

}  // namespace x::features::ports::attack
