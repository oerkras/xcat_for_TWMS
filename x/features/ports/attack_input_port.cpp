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

constexpr DWORD kDefaultIntervalMs = xcat::kSimpleCombatAttackIntervalDefaultMs;
// 智能间隔：在面板 config 附近抖动，不再锁死 480–560。
constexpr DWORD kSmartJitterMs = 40;
// 动作占用（禁 TP）：只挡前摇，不当做出刀地板。
constexpr DWORD kAttackAnimBusyMs = 220;
// 松键占用上限；实际 hold = min(此值, 面板间隔)，避免长 hold 锁死短间隔。
constexpr DWORD kAttackHoldCapMs = 100;
constexpr DWORD kAttackHoldFloorMs = 5;
constexpr float kFaceDeadzone = 8.f;
// 已面向正确半场时，|dx| 未过此值不重 SetInput，减轻怪贴身穿过时的左右抽风。
constexpr float kFaceStickyPx = 28.f;
constexpr DWORD kRateWindowMs = 3000;
constexpr DWORD kFireJobWaitMs = 800;
constexpr DWORD kFaceJobWaitMs = 400;
constexpr DWORD kFkmRebindMs = 3000;

constexpr uint32_t kRvaOnFuncKey = 0x107A200;  // remapped 2026-08-03
constexpr uint32_t kRvaGetKeyByFunc = 0x164A610;  // remapped 2026-08-03
constexpr uint32_t kRvaGetDataByKeyCode = 0x1649680;  // remapped 2026-08-03
constexpr uint32_t kRvaFuncKeyCtor = 0x1641AE0;  // remapped 2026-08-03: .ctor(FuncType,int)
// 写 InputX/Y + 内联 OnResolveMoveAction（朝向）；见 docs/features/protocol/MoveElem字段.md
constexpr uint32_t kRvaVecCtrlSetInput = 0x11B30B0;  // remapped 2026-08-03

// 方法哈希（dump.cs · remount 2026-08-03）
constexpr char kHashOnFuncKey[] =
    "eb70dd6a52329f9f7cffa938d48f1c529af67d1705bba4507ade9d5f58eabbe";
constexpr char kHashGetKeyByFunc[] =
    "a5cfdfe2e66f31a23f4629f8a51e540b2e92b98f6448704913554379928bde3";
constexpr char kHashGetDataByKeyCode[] =
    "b9ef9353368915003e2e2a0a7251d3c63e34eb4f444dd7101d9d4302dd77d06";
constexpr char kHashVecCtrlSetInput[] =
    "cd3026f1c8768933331397d44f57e08f944b7877150aa6a984373691848aafc";
constexpr char kVecCtrlClass[] =
    "d5ce57ae29519b9d8ea3e23c7f00e3995b1c02048eb8093dff28802f6cb9598";

constexpr size_t kOffVecCtrl = 0x50;
constexpr size_t kOffVcMoveAction = 0x84;

constexpr int32_t kKeyInputDown = 0;
constexpr int32_t kKeyInputUp = 1;
constexpr int32_t kFuncTypeNone = 0;
constexpr int32_t kFuncTypeBasicAction = 5;
constexpr int32_t kFkmBasicActionAttack = 52;
// 默认攻击槽：Win32 'A' → InputSystem.Key.A = 15（见 input_port VkToUnityKey）。
constexpr WORD kDefaultAttackVk = static_cast<WORD>('A');

// FKM remounted 2026-08-03 (owns GetKeyByFunc@0x164A610 / GetDataByKeyCode@0x1649680).
// Old af052bb0…be266af gone from dump; FuncKey class hash unchanged.
constexpr char kFkmClass[] =
    "b01adf8a23294118cf3e20b9e5ee6cd4e8b28568a920d5713689e7a6be33f97";
constexpr char kFuncKeyClass[] =
    "bd3a79401c7d64bce45aac35ca0daf6e2dc938d1ffc0b396e137fde02b8c4cf";

constexpr size_t kOffFkType = 0x10;
constexpr size_t kOffFkValue = 0x14;

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

HANDLE gLog = INVALID_HANDLE_VALUE;

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
                          const char* plainName = nullptr, const char* hashName = nullptr) {
    if (plainName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plainName, shape.arity)) return mi;
    }
    if (hashName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hashName, shape.arity)) return mi;
    }
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("Attack", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plainName ? plainName : "-");
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

void EnsureMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (!gFkmKlass) gFkmKlass = x::runtime::il2cpp::FindClass("", kFkmClass);
    void* ulKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* fkKlass = x::runtime::il2cpp::FindClass("", kFuncKeyClass);
    void* vcKlass = x::runtime::il2cpp::FindClass("", kVecCtrlClass);

    if (ulKlass && !gMiOnFuncKey) {
        // void(inputType, FuncKey, scan) — UL 上唯一
        constexpr MethodShape kFk{3,
                                  TypeKind::Void,
                                  true,
                                  true,
                                  {TypeKind::I32, TypeKind::Ptr, TypeKind::U32}};
        gMiOnFuncKey =
            ResolveMi(ulKlass, kRvaOnFuncKey, kFk, "OnFuncKey", kHashOnFuncKey);
    }
    if (gFkmKlass) {
        if (!gMiGetKeyByFunc) {
            constexpr MethodShape kGet{2, TypeKind::I32, true, true, {TypeKind::Any, TypeKind::I32}};
            gMiGetKeyByFunc = ResolveMi(gFkmKlass, kRvaGetKeyByFunc, kGet, "GetKeyByFunc",
                                        kHashGetKeyByFunc);
        }
        if (!gMiGetDataByKeyCode) {
            constexpr MethodShape kData{1, TypeKind::Ptr, true, true, {TypeKind::Any}};
            gMiGetDataByKeyCode = ResolveMi(gFkmKlass, kRvaGetDataByKeyCode, kData,
                                            "GetDataByKeyCode", kHashGetDataByKeyCode);
        }
    }
    if (fkKlass && !gMiFuncKeyCtor) {
        constexpr MethodShape kCtor{2, TypeKind::Void, true, false, {TypeKind::Any, TypeKind::I32}};
        gMiFuncKeyCtor = ResolveMi(fkKlass, kRvaFuncKeyCtor, kCtor, ".ctor", nullptr);
    }
    if (vcKlass && !gMiSetInput) {
        // void(int,int) 全局不唯一 → 哈希主
        constexpr MethodShape kIn{2, TypeKind::Void, false, true, {TypeKind::I32, TypeKind::I32}};
        gMiSetInput =
            ResolveMi(vcKlass, kRvaVecCtrlSetInput, kIn, "SetInput", kHashVecCtrlSetInput);
    }
}

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    CreateDirectoryA(dir, nullptr);
    gLog = x::runtime::OpenRotatingDbgLogA(dir, "combat.log");
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
    if (gLog != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(gLog, buf, (DWORD)n, &w, nullptr);
    }
    x::runtime::LogI("Attack", "%s", body);
}

DWORD ClampAttackIntervalMs(DWORD ms) {
    if (ms < xcat::kSimpleCombatAttackIntervalMinMs)
        return xcat::kSimpleCombatAttackIntervalMinMs;
    if (ms > xcat::kSimpleCombatAttackIntervalMaxMs)
        return xcat::kSimpleCombatAttackIntervalMaxMs;  // 与 common 常量对齐
    return ms;
}

// 松键时长跟面板间隔走：interval≤5 → hold=5；interval≥100 → hold=100。
DWORD AttackHoldMs() {
    const DWORD interval = ClampAttackIntervalMs(gEffectiveIntervalMs.load(std::memory_order_relaxed));
    if (interval <= kAttackHoldFloorMs) return kAttackHoldFloorMs;
    if (interval < kAttackHoldCapMs) return interval;
    return kAttackHoldCapMs;
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
            e.runtimeClassInit(gFkmKlass);
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
                    e.runtimeClassInit(parent);
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
    const DWORD now = GetTickCount();
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
    const DWORD now = GetTickCount();
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
        __try {
            fk = e.objectNew(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fk = nullptr;
        }
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
    if (!x::runtime::main_thread::InvokeAndWait(&FireJobOnMain, &job, kFireJobWaitMs)) {
        LogLine("OnFuncKey pump timeout up=%d", isUp ? 1 : 0);
        return false;
    }
    return job.ok;
}

// 同一次主线程泵：Down 紧接 Up，消 pending 跨 tick（攻击加速路径）。
struct FirePulseJob {
    bool downOk = false;
    bool upOk = false;
    const char* err = "?";
};

void FirePulseOnMain(void* user) {
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
    if (!x::runtime::main_thread::InvokeAndWait(&FirePulseOnMain, &job, kFireJobWaitMs)) {
        LogLine("OnFuncKey pulse pump timeout");
        return false;
    }
    // Down 成功但 Up 失败：补一次 Up，避免粘键；仍计失败。
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

    gAttackVk.store(kDefaultAttackVk);
    gConfigIntervalMs.store(kDefaultIntervalMs);
    gEffectiveIntervalMs.store(kDefaultIntervalMs);
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
        "hold=min(%ums,interval) animBusy=%ums",
        (unsigned)kAttackHoldCapMs, (unsigned)kAttackAnimBusyMs);
}

void Shutdown() {
    ForceRelease();
    ClearAttackFk();
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
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
        // 只瞬时 Resolve 朝向；禁止随后 SetInput(0,0)（BIN：掐刀/态乱）。
        // 下一帧 KeyPad 锁存为 0 时会自然盖回，站桩出刀足够。
        fn(vc, job->inputX, 0, gMiSetInput);
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
    if (!runtime::main_thread::InvokeAndWait(&FaceSetInputJobFn, &job, kFaceJobWaitMs)) {
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
    const DWORD now = GetTickCount();
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
    if (on && !prev) {
        // 切入立刻松键：清掉未完成的 async Up，避免粘键。
        if (gPendingUp.exchange(false, std::memory_order_acq_rel)) {
            (void)InvokeFire(true);
        }
        static bool sLogged = false;
        if (!sLogged) {
            sLogged = true;
            LogLine("immediate Up on (attack accel: Down+Up same pump, no pending)");
        }
    } else if (!on && prev) {
        LogLine("immediate Up off (restore hold async Up)");
    }
}

bool CanFirePrimary() {
    if (gFireSuppressed.load(std::memory_order_acquire)) return false;
    const DWORD now = GetTickCount();
    FlushPendingUp(now);
    return !SoftBlocked(now);
}

bool TryFirePrimary() {
    const DWORD now = GetTickCount();
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
        gUpDueMs.store(GetTickCount() + hold, std::memory_order_relaxed);
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

void TickReleases() { FlushPendingUp(GetTickCount()); }

void SetFireSuppressed(bool on) {
    const bool was = gFireSuppressed.exchange(on, std::memory_order_acq_rel);
    if (on && !was) ForceRelease();
}

bool IsFireSuppressed() { return gFireSuppressed.load(std::memory_order_acquire); }

bool WaitFireIdle(DWORD timeoutMs, DWORD settleAfterFireMs) {
    ForceRelease();
    const DWORD t0 = GetTickCount();
    for (;;) {
        FlushPendingUp(GetTickCount());
        const DWORD now = GetTickCount();
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
    if (!gPendingUp.exchange(false, std::memory_order_acq_rel)) return;
    (void)InvokeFire(true);
}

}  // namespace x::features::ports::attack
