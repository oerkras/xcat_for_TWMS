// attack_rpc_port — forge ClientPacket 50 (Melee) via official Encode + SendOutPacket.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_rpc_port.h"

#include "attack_input_port.h"
#include "mob_pool_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../final_attack_force/final_attack_force.h"
#include "../../ui/player_vitals.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../../common/xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x::features::ports::attack_rpc {
namespace {

using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Session 方法宿主 → ResolveNetworkManagerKlass；单例壳 → ResolveNetworkManagerFacadeKlass
constexpr char kOutPacketClass[] =
    "e0c844c6ebe831431dd6925430869fed0b7b35b9fad5484c3f4d18ecb8f65c5";
constexpr char kHashUserClass[] =
    "ead9ab2e851cf06879704044ce549197d7fb5017ecb635104f7a6e366f9ac7a";
constexpr char kHashSecurityClient[] =
    "d856fdab82ec7edbec7bafdff562821eb524fe938c019a176509b60ced547a9";
// SkillInfo（与 skill_port 同 hash）。MeleeAttackAction = Dictionary<int, List<ActionType>> @ static 0x118
constexpr char kSkillInfoClass[] =
    "d7ac2f1648fb2ae293bf23a4770ed92a0e0da64dd17c612770f6e713fcad72f";
constexpr char kHashMeleeAttackAction[] =
    "b3cf40917063e9f71eb5e5ddb56577f094998a0fa55e297bebdf2afdde085f2";
constexpr size_t kOffMeleeAttackActionFb = 0x118;

// P0b / P0c RVAs — remounted 2026-08-04；解析优先 hash/plain（见下方 kHash*）
constexpr uint32_t kRvaOutPacketCreate = 0x1CEC3F0;
constexpr uint32_t kRvaEncode1Byte = 0x1CF8B00;
constexpr uint32_t kRvaEncode1Bool = 0x1CF8BE0;
constexpr uint32_t kRvaEncode2Ushort = 0x1CF8D70;
constexpr uint32_t kRvaEncode2Short = 0x1CF8CF0;
constexpr uint32_t kRvaEncode4Int = 0x1CF8E00;
constexpr uint32_t kRvaEncode4Uint = 0x1CF8E80;
constexpr uint32_t kRvaEncodeVector2 = 0x1CF99D0;
// 官方门：Network_SendOutPacket（this=facade）
//   → HashSet<ushort>.Contains(opcode@pkt+0x20) → Session.SendPacket（[facade+0x10]）
// 2026-08-03 BIN：直调 Session.SendPacket 旁路 HashSet → 第 3 次 forge 后 ~109ms Disconnected
// （与 sellbag「错包+Session.Send → 105ms」同型；KickSniff verdict=lean_local_or_soft）。
constexpr uint32_t kRvaNmSendPacket = 0x1CEE000;  // remounted 2026-08-06 · Session.SendPacket
constexpr uint32_t kRvaSendOutPacket = 0x1CEC4F0;  // remounted 2026-08-06
constexpr uint32_t kRvaWorldManagerGetUpdateTime = 0xDDD070;  // remounted 2026-08-06 get_GetUpdateTime
// 延后踢对策：对齐 TryDoingNormalAttack 发包前最小本地态
//   SetAttackAction(lu, action, aux, skill=null, 0) @0xFFA2C0
//   CollectAttackPacket(50) @0x3C8C450（Tap 旁路；SendOut 静态无直调）
constexpr uint32_t kRvaSetAttackAction = 0xFFA2C0;  // remounted 2026-08-04；旧 0xFD39C0
constexpr uint32_t kRvaCollectAttackPacket = 0x3C8C450;  // remounted 2026-08-04；旧 0x3C44C10

constexpr char kHashOutCreate[] =
    "cbfb75afdbb647454b889d8dfa7ea8f054cffaa7192a29dd45ae66baccd03d8";
constexpr char kHashEncode1Byte[] =
    "fc9e10918826f64a862d940d4b763e72cd40171bc4fd0d2ef75f4e989c8ef41";
constexpr char kHashEncode1Bool[] =
    "e53d87c72ee309ae317c82079e7c680e2fb7d23f86b6ab01b432c32a05e4533";
constexpr char kHashEncode2Ushort[] =
    "a0b6f30974d785d0a33004c1b2be559725d9db304f8fd25704214ded69ca43a";
constexpr char kHashEncode2Short[] =
    "aaa2efc6f86ef71ea46889c9d32ec632860bfb8821b7b41fedaf4372c756277";
constexpr char kHashEncode4Int[] =
    "da51eb6bd7463f17bd29c126db804118b7ed718d57c56fb0530915fba7332eb";
constexpr char kHashEncode4Uint[] =
    "c15d23424acf324644a2754913d389f303a2d538029a0a6bf5422e285fae95a";
constexpr char kHashEncodeVector2[] =
    "b2fb4e7795f710469e0a92a6c786d6cc8d43ab3354e59667990b2c1e70fe2a7";
constexpr char kHashSendOutPacket[] =
    "eec75536b8836c264fcdb149641ece23e4efc4a15ef1f321194366e99cc79cd";
constexpr char kHashGetUpdateTime[] =
    "afcecd0392691e1f508cfded7d716d0292eca403dd3a33d10f71d388d0bf4fc";  // get_GetUpdateTime
constexpr char kHashSetAttackAction[] =
    "a457309ea83620947841118cabab21c998f0c6f7f60881e83ace82659ec10ce";
constexpr char kHashCollectAttackPacket[] =
    "f2e83098f8e88eb48b1e830ec506a9637405a7f82b82855ea96dbf64e9b553c";
// 探针安全：单次勾选最多 2 刀（auto_stop）。进程累计封顶可面板清零，不必重注。
// 清零要闲置 >= kResetIdleMs，避免勾选连点把 15ms 洪水送出去。
// 旧 BIN：Session.Send 旁路第 3 踢；SendOut 对齐后稀疏 4 刀已打出真杀。
// 2026-08-15：dist=17 打死、48 像死、90+ 仍 normal ok 但 hp 不动。180px 过松。
constexpr int kAutoStopAfterOk = 2;
constexpr int kSessionForgeCap = 20;
constexpr DWORD kResetIdleMs = 2500;
constexpr float kMeleeMaxDist = 50.f;  // 探针 TAB hypot；更大 Y=更高。钉锁不走这把尺。
// 命中环 XY：oid 仍是怪，点写角色脚下。EncodeVector2 会把 Unity Y 翻成 Maple（IDA：
// add edx,80000000h = IEEE 符号位翻转）；我们 Encode2S 直写，必须自己做同样的 -Y。
// 禁止把「AbsPos 瞄准不反号」套到 wire 上——那会把掉落抛到人物头顶。
constexpr float kDropFootReachX = 0.f;

// Create opcode 跟装备，不手选。射击 Create 种子已实算：word_7FFD6711C344=0xBABC
// + 0x4577 → 0x10033 → u16 51（TryDoingShootAttack @ RVA 0x1070390）。
constexpr int kOpcodeMeleeAttack = 50;
constexpr int kOpcodeShootAttack = 51;
constexpr int kOpcodeMagicAttack = 52;
constexpr int kWtOneHandSword = 30;
constexpr int kWtOneHandAxe = 31;
constexpr int kWtOneHandMace = 32;
constexpr int kWtDagger = 33;
constexpr int kWtWand = 37;
constexpr int kWtStaff = 38;
constexpr int kWtBareHand = 39;
constexpr int kWtTwoHandSword = 40;
constexpr int kWtTwoHandAxe = 41;
constexpr int kWtTwoHandMace = 42;
constexpr int kWtSpear = 43;
constexpr int kWtPoleArm = 44;
constexpr int kWtBow = 45;
constexpr int kWtCrossbow = 46;
constexpr int kWtThrowingGlove = 47;
constexpr int kWtKnuckle = 48;
constexpr int kWtGun = 49;
constexpr int32_t kFuncTypeSkill = 1;
constexpr int32_t kFuncTypeItem = 2;
constexpr int32_t kFuncTypeEmotion = 3;
constexpr int32_t kFuncTypeMenu = 4;
constexpr int32_t kFuncTypeBasicAction = 5;
constexpr int32_t kFuncTypeMacroSkill = 8;
constexpr int32_t kFkmBasicActionAttack = 52;
// Facade/Session → il2cpp_network SSOT
#define kOffNmSession (x::runtime::il2cpp_network::OffNmSession())
#define kOffNmSessionState (x::runtime::il2cpp_network::OffNmSessionState())
#define kOffNmOpcodeHashSet (x::runtime::il2cpp_network::OffNmOpcodeHashSet())
#define kOffSessionState (x::runtime::il2cpp_network::OffSessionState())
constexpr int kSessionStateConnected = 3;
constexpr size_t kOffMobPos = 0x64;  // FieldActorBase Pos (Vector2)
constexpr size_t kOffMobData = 0x138;
constexpr size_t kOffMobDataMoveAbility = 0x2C;  // TW MobData；CMS 在 0x34
// MobMoveAbility：Stop=0 Walk=1 Jump=2 Fly=3 FlyRandom=4
constexpr int kMoveAbilityFly = 3;
constexpr int kMoveAbilityFlyRandom = 4;
// TW MsAvatar（对照 CMS AttackType@0x140 / WeaponAttackSpeed@0x144，本 dump +0x18）
// 0x158 = AttackType（int）；0x15C = WeaponAttackSpeed（int）。
// BIN 02:45：误把 0x158 当攻速编码 → 短刀 01 能伤、双手剑 05 空枪。
constexpr size_t kOffAvatarAttackType = 0x158;
constexpr size_t kOffAvatarWeaponAttackSpeed = 0x15C;
// OutPacket：buffer@+0x10、offset@+0x18；Il2CppArray 元素 → OffArrayData；BODY 跳过 4B 长 + 2B opcode。
constexpr size_t kOffPacketBuffer = 0x10;
constexpr size_t kOffPacketOffset = 0x18;
#define kIl2cppArrayData (x::runtime::il2cpp_container::OffArrayData())
constexpr int kPacketDataPos = 6;

constexpr DWORD kRebindMs = 2000;
constexpr DWORD kJobWaitMs = 2500;
constexpr int kMaxMobsHard = 15;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

struct Vec2 {
    float x;
    float y;
};

using FnOutCreate = void* (*)(int packetId, const void* method);
using FnEncode1 = void (*)(void* self, uint8_t v, const void* method);
using FnEncode1Bool = void (*)(void* self, bool v, const void* method);
using FnEncode2U = void (*)(void* self, uint16_t v, const void* method);
using FnEncode2S = void (*)(void* self, int16_t v, const void* method);
using FnEncode4 = void (*)(void* self, int32_t v, const void* method);
using FnEncode4U = void (*)(void* self, uint32_t v, const void* method);
using FnEncodeV2 = void (*)(void* self, Vec2 v, const void* method);
using FnSendOut = bool (*)(void* nmFacade, void* packet, const void* method);
// bool(User*, int action, int aux, SkillEntry*, int flag, MethodInfo*)
using FnSetAttackAction = bool (*)(void* user, int32_t action, int32_t aux, void* skill,
                                   int32_t flag, const void* method);
// void(ushort packetType, MethodInfo*) — Tap 内 xor edx; call
using FnCollectAttackPacket = void (*)(uint16_t packetType, const void* method);
using FnGetUpdateTime = int (*)(const void* methodInfo);
using FnFindAll = void* (*)(void* type, void* method);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
void* gFacadeKlass = nullptr;
void* gFacadeType = nullptr;
void* gSessionKlass = nullptr;  // Session klass（连通性 / 兜底）
void* gNmFacade = nullptr;      // NetworkManager facade（SendOutPacket this）
void* gNm = nullptr;            // Session*（facade+0x10；仅校验 Connected）
void* gOutPacketKlass = nullptr;
void* gWorldManagerKlass = nullptr;
void* gSkillInfoKlass = nullptr;
size_t gOffMeleeAttackAction = 0;
int gCachedMeleeWt = -1;
int gMeleeActs[4]{};
int gMeleeActN = 0;
int gMeleeSeq = 0;
DWORD gCachedMeleeMs = 0;
const char* gCachedMeleeVia = "none";
FnGetUpdateTime gGetUpdateTime = nullptr;
MethodInfoHead* gMiGetUpdateTime = nullptr;

MethodInfoHead* gMiCreate = nullptr;
MethodInfoHead* gMiE1 = nullptr;
MethodInfoHead* gMiE1Bool = nullptr;
MethodInfoHead* gMiE2U = nullptr;
MethodInfoHead* gMiE2S = nullptr;
MethodInfoHead* gMiE4 = nullptr;
MethodInfoHead* gMiE4U = nullptr;
MethodInfoHead* gMiEV2 = nullptr;
MethodInfoHead* gMiSendOut = nullptr;
MethodInfoHead* gMiSetAttackAction = nullptr;
MethodInfoHead* gMiCollectAttackPacket = nullptr;
FnSetAttackAction gSetAttackAction = nullptr;
FnCollectAttackPacket gCollectAttackPacket = nullptr;

std::atomic<bool> gEnabled{false};
std::atomic<bool> gNeedOffAck{false};   // auto_stop 后必须见到 SetEnabled(false) 才允许再开
std::atomic<bool> gPendingReset{false}; // 清零距上一刀太近：排队到闲置够了再清
std::atomic<uint32_t> gStopGen{0};      // 面板用来勾灭；每次 auto_stop / session_stop +1
std::atomic<DWORD> gLastHoldLogMs{0};
std::atomic<int> gMaxMobs{1};
std::atomic<DWORD> gIntervalMs{500};
std::atomic<int> gDamage{1};
std::atomic<int> gSkillId{0};
std::atomic<bool> gReady{false};
std::atomic<DWORD> gFailBackoffUntilMs{0};
std::atomic<int> gConsecutiveFails{0};
std::atomic<int> gOkSinceEnable{0};   // 本段开启计数（SetEnabled(true) 清零）
std::atomic<int> gOkSession{0};       // 进程内累计成功伪造；ResetSessionCap 可清
// 钉锁过远：与站桩输出面前盒同一把尺（HiraishinFrontOk）。0=该轴不限。
std::atomic<uint32_t> gLockFrontDx{xcat::kHiraishinFrontDxDefault};
std::atomic<uint32_t> gLockFrontDy{xcat::kHiraishinFrontDyDefault};
DWORD gLastRebindMs = 0;
DWORD gLastFireMs = 0;
DWORD gLastNmFindAllMs = 0;
DWORD gLastFailLogMs = 0;

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float ReadF32(void* obj, size_t off) {
    if (!obj) return 0.f;
    __try {
        return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

// 飞行怪：短刀脚下环对不上判定盒（BIN 蝙蝠 38 包 hp=100；不勾造包走官方怪坐标有伤）。
bool MobIsFlyFamily(void* mob) {
    if (!LooksLikeHeapPtr(mob)) return false;
    void* data = ReadPtr(mob, kOffMobData);
    if (!LooksLikeHeapPtr(data)) return false;
    const int ma = ReadI32(data, kOffMobDataMoveAbility);
    return ma == kMoveAbilityFly || ma == kMoveAbilityFlyRandom;
}

void* FindClass(const char* name) { return x::runtime::il2cpp::FindClass("", name); }

void* ClassTypeObject(void* klass) {
    // 必须直调：FireJob 已在 main pump 内，套 managed_main::TypeGetObject
    // 会嵌套 InvokeAndWait → 泵死锁/超时卡顿（shop_port 同约定）。
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetType || !e.typeGetObject) return nullptr;
    __try {
        void* ty = e.classGetType(klass);
        if (!ty) return nullptr;
        return e.typeGetObject(ty);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool LooksLikeFacade(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gFacadeKlass) {
        void* k = ReadPtr(cand, 0);
        if (k != gFacadeKlass) return false;
    } else {
        return false;
    }
    const int st = ReadI32(cand, kOffNmSessionState);
    if (st < 0 || st > 3) return false;
    void* sess = ReadPtr(cand, kOffNmSession);
    if (sess && !LooksLikeHeapPtr(sess)) return false;
    if (LooksLikeHeapPtr(sess)) return true;
    return st == 2 || st == kSessionStateConnected;
}

bool LooksLikeSession(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    // shop 同款：klass 已解析时优先匹配；未解析或漂移时仍用 state 兜底（避免 no_nm）。
    if (gSessionKlass) {
        void* k = ReadPtr(cand, 0);
        if (k && k != gSessionKlass) {
            const int st = ReadI32(cand, kOffSessionState);
            return st >= 0 && st <= 3;
        }
    }
    const int st = ReadI32(cand, kOffSessionState);
    return st >= 0 && st <= 3;
}

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* ResolveFacadeSingleton(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    auto classInit = [&](void* k) {
        if (!k || !e.runtimeClassInit) return;
        x::runtime::il2cpp::RuntimeClassInit(k);
    };
    auto staticsOf = [&](void* k) -> void* {
        if (!k || !e.classStaticData) return nullptr;
        __try {
            void* sd = e.classStaticData(k);
            return LooksLikeHeapPtr(sd) ? sd : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    };
    auto pickFromStatics = [&](void* sd) -> void* {
        if (!sd) return nullptr;
        void* best = nullptr;
        for (size_t s = 0; s < 8; ++s) {
            void* lazy = ReadPtr(sd, s * sizeof(void*));
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (!LooksLikeFacade(cand)) continue;
            void* sess = ReadPtr(cand, kOffNmSession);
            const int st = ReadI32(cand, kOffNmSessionState);
            if (LooksLikeHeapPtr(sess) && st == kSessionStateConnected) return cand;
            if (st == kSessionStateConnected) return cand;
            if (!best) best = cand;
        }
        return best;
    };

    classInit(klass);
    void* parent = nullptr;
    if (e.classParent) {
        __try {
            parent = e.classParent(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
    }
    if (parent) classInit(parent);
    if (void* inst = pickFromStatics(staticsOf(parent))) return inst;
    if (void* inst = pickFromStatics(staticsOf(klass))) return inst;
    return nullptr;
}

void* SessionFromFacade(void* facade) {
    if (!LooksLikeFacade(facade)) return nullptr;
    void* sess = ReadPtr(facade, kOffNmSession);
    if (!LooksLikeHeapPtr(sess)) return nullptr;
    // 对齐 shop：不因 Session klass 漂移拒掉指针；发包 this 以 facade+0x10 为准。
    if (!gSessionKlass) {
        void* k = ReadPtr(sess, 0);
        if (LooksLikeHeapPtr(k)) gSessionKlass = k;
    }
    return sess;
}

uint8_t ReadU8(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva);  // 前向声明：GetGameUpdateTimeMs 用

MethodInfoHead* ResolveMi(void* klass, uint32_t rva, const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (mr.method) return reinterpret_cast<MethodInfoHead*>(mr.method);
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

int GetGameUpdateTimeMs() {
    // 与 skill_port 同钟：WorldManager.GetUpdateTime / static _updateTime*1000。
    if (!gWorldManagerKlass) {
        gWorldManagerKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    }
    if (!gMiGetUpdateTime && gWorldManagerKlass) {
        using namespace x::runtime::il2cpp_method;
        constexpr MethodShape kUt{0, TypeKind::I32, true, true, {}};
        gMiGetUpdateTime = ResolveMi(gWorldManagerKlass, kRvaWorldManagerGetUpdateTime, kUt,
                                     "GetUpdateTime", kHashGetUpdateTime);
        if (!gMiGetUpdateTime)
            gMiGetUpdateTime =
                ResolveMi(gWorldManagerKlass, kRvaWorldManagerGetUpdateTime, kUt,
                          "get_GetUpdateTime", kHashGetUpdateTime);
    }
    if (!gGetUpdateTime)
        gGetUpdateTime = FnFromMi<FnGetUpdateTime>(gMiGetUpdateTime, kRvaWorldManagerGetUpdateTime);
    if (gGetUpdateTime) {
        int t = 0;
        __try {
            t = gGetUpdateTime(gMiGetUpdateTime);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            t = 0;
        }
        if (t > 0) return t;
    }
    if (!gWorldManagerKlass) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classStaticData) return 0;
    void* sf = nullptr;
    __try {
        sf = e.classStaticData(gWorldManagerKlass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sf = nullptr;
    }
    if (!LooksLikeHeapPtr(sf)) return 0;
    float sec = 0.f;
    __try {
        sec = *reinterpret_cast<float*>(sf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!(sec > 0.f) || !(sec < 2.0e6f)) return 0;
    const double ms = static_cast<double>(sec) * 1000.0;
    if (ms <= 0.0 || ms >= 2.0e9) return 0;
    return static_cast<int>(ms);
}

FnSendOut ResolveSendOutFn() {
    if (gMiSendOut) {
        if (gMiSendOut->methodPointer)
            return reinterpret_cast<FnSendOut>(gMiSendOut->methodPointer);
        if (gMiSendOut->virtualMethodPointer)
            return reinterpret_cast<FnSendOut>(gMiSendOut->virtualMethodPointer);
    }
    return AtRva<FnSendOut>(kRvaSendOutPacket);
}

int OpcodeFromWeaponType(int wt) {
    if (wt == kWtBow || wt == kWtCrossbow || wt == kWtThrowingGlove || wt == kWtGun)
        return kOpcodeShootAttack;
    if (wt == kWtWand || wt == kWtStaff) return kOpcodeMagicAttack;
    return kOpcodeMeleeAttack;
}

// CMS ActionType：SwingO1=5 单手/短刀；SwingT1=9 双手；SwingP1=13 枪矛。
bool ActionLooksMelee(int a) { return (a >= 5 && a <= 21) || a == 32; }

int FallbackMeleeAction(int wt) {
    if (wt >= kWtTwoHandSword && wt <= kWtTwoHandMace) return 9;
    if (wt == kWtSpear || wt == kWtPoleArm) return 13;
    return 5;
}

// 双手剑 / 枪矛官方三连挥，但人手可以点一下等前摇再点——每次都是第一招。
// BIN：轮 9/10/11 仍只前几刀有伤。不再轮砍。
bool WeaponNeedsActionCycle(int wt) {
    (void)wt;
    return false;
}

int FillFallbackFamily(int wt, int* out, int cap) {
    if (!out || cap <= 0) return 0;
    const int base = FallbackMeleeAction(wt);
    if (!WeaponNeedsActionCycle(wt)) {
        out[0] = base;
        return 1;
    }
    const int n = cap < 3 ? cap : 3;
    for (int i = 0; i < n; ++i) out[i] = base + i;
    return n;
}

size_t FieldOffsetByHash(void* klass, const char* nameHash) {
    if (!klass || !nameHash || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classGetFieldFromName && e.fieldGetOffset) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(klass, nameHash);
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
            if (off) return off;
        }
    }
    return 0;
}

int ClampAttackSpeed(int spd) {
    if (spd >= 1 && spd <= 15) return spd;
    return 1;
}

int ClampDegree(int deg) {
    if (deg < 2) return 2;
    if (deg > 10) return 10;
    return deg;
}

// 双手剑/斧/钝 + 枪矛：真包环是怪坐标、Delay 450。枪矛动作 13，AttackType 回退 8/9 不是 5。
bool WeaponIsTwoHandFamily(int wt) { return wt >= kWtTwoHandSword && wt <= kWtPoleArm; }

bool WeaponIsDagger(int wt) { return wt == kWtDagger; }

// 单手剑/斧/钝、徒手、指节：SwingO。短刀「WAS + 恒 05」在单手会写成 `05 05` 空挥。
// 头必须 AttackType + 真 degree。P0c `01 05` 是 WAS=5 的剑；新手剑 1302000 WAS=4 → `01 04`。
bool WeaponIsOneHandSwing(int wt) {
    return (wt >= kWtOneHandSword && wt <= kWtOneHandMace) || wt == kWtBareHand ||
           wt == kWtKnuckle;
}

// 装备后 +0x158 读不到时按武器类型回退（CMS WeaponType → AttackType）。
// TW 离线表：wt=30/33 afterImage 都是 swordOL/swordOS，短刀 BIN atkType=1（不是 CMS 常见的 4）。
int FallbackAttackType(int wt) {
    if (wt == kWtOneHandSword || wt == kWtBareHand || wt == kWtDagger) return 1;
    if (wt == kWtOneHandAxe) return 2;
    if (wt == kWtOneHandMace) return 3;
    if (wt == kWtTwoHandSword) return 5;
    if (wt == kWtTwoHandAxe) return 6;
    if (wt == kWtTwoHandMace) return 7;
    if (wt == kWtSpear) return 8;
    if (wt == kWtPoleArm) return 9;
    if (wt == kWtKnuckle) return 13;
    return 1;
}

// 地面近战线。飞行怪环先搁置：不再用 fly 改 XY。
// 短兵器（短刀/单手/徒手/指节）：脚下 + ForeAction 0x81。
// 长兵器（双手剑/斧/钝 + 枪矛）：怪坐标 + ForeAction 0 + Delay 450（双手剑 send.log）。
bool WeaponRingAtMob(int wt) { return WeaponIsTwoHandFamily(wt); }

uint8_t WeaponForeAction(int wt) { return WeaponIsTwoHandFamily(wt) ? 0 : 0x81; }

// 短刀 BIN 钉死 A5（WAS=3 也写 421，不跟公式）。双手剑钉死 C2。
// 单手：P0b (tier+10)*450>>4。WZJS 1302000 WAS=4 → 393；WAS=5 → 421=A5（P0c）。
uint16_t WeaponHitDelay(int wt, int degree) {
    if (WeaponIsDagger(wt)) return 0x01A5;
    if (WeaponIsTwoHandFamily(wt)) return 0x01C2;
    const int t = ClampDegree(degree);
    return static_cast<uint16_t>(((t + 10) * 450) >> 4);
}

// 线上第一字节。WAS 误进 +0x158 时单手会变成 05 05 空挥，改回族回退。
int ResolveWireAttackType(int wt, int at, int was) {
    const int fb = FallbackAttackType(wt);
    if (at < 1 || at > 20) return fb;
    if (at == was && fb != at) return fb;
    return at;
}

const char* MeleeFamilyName(int wt) {
    if (wt == kWtDagger) return "dagger";
    if (wt >= kWtOneHandSword && wt <= kWtOneHandMace) return "1h";
    if (wt == kWtBareHand) return "bare";
    if (wt == kWtKnuckle) return "knuckle";
    if (wt >= kWtTwoHandSword && wt <= kWtTwoHandMace) return "2h";
    if (wt == kWtSpear || wt == kWtPoleArm) return "pole";
    return "other";
}

// 官方 Encode 的是 GetAttackSpeedDegree = clamp(WAS + nBooster + party, 2, 10)，
// 不是裸 0x15C。双手剑 WAS=6 配 booster 后常见 2–4；仍写 6 会跟 133ms 间隔对不上。
constexpr size_t kOffSsBooster = 0xBC;

int ReadAvatarAttackSpeed(void* localUser, int* outWas, int* outBoost) {
    int was = 0;
    int boost = 0;
    if (LooksLikeHeapPtr(localUser)) {
        was = ReadI32(localUser, kOffAvatarWeaponAttackSpeed);
        if (was < 1 || was > 15) was = ReadI32(localUser, kOffAvatarAttackType);
    }
    void* ss = x::ui::player::LocalSecondaryStat();
    if (LooksLikeHeapPtr(ss)) boost = ReadI32(ss, kOffSsBooster);
    if (outWas) *outWas = was;
    if (outBoost) *outBoost = boost;
    const int deg = ClampDegree((was >= 1 && was <= 15 ? was : 6) + boost);
    return deg;
}

int ReadAvatarAttackType(void* localUser) {
    if (!LooksLikeHeapPtr(localUser)) return 0;
    return ReadI32(localUser, kOffAvatarAttackType);
}

int FillActionsFromList(void* list, int* out, int cap) {
    if (!LooksLikeHeapPtr(list) || !out || cap <= 0) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    const int n = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    if (!LooksLikeHeapPtr(items) || n <= 0) return 0;
    const size_t data = x::runtime::il2cpp_container::OffArrayData();
    int wrote = 0;
    const int lim = n < cap ? n : cap;
    for (int i = 0; i < lim; ++i) {
        int v = 0;
        __try {
            v = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(items) + data +
                                            static_cast<size_t>(i) * 4u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (!ActionLooksMelee(v)) continue;
        out[wrote++] = v;
    }
    return wrote;
}

void* DictGetIntPtr(void* dict, int key) {
    if (!LooksLikeHeapPtr(dict)) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, x::runtime::il2cpp_container::OffDictEntries());
    const uintptr_t n = ArrayLen(entries);
    if (!LooksLikeHeapPtr(entries) || n == 0 || n > 4096) return nullptr;
    const size_t stride = x::runtime::il2cpp_container::DictEntryStrideIntPtr();
    const size_t offHash = x::runtime::il2cpp_container::OffDictEntryHash();
    const size_t offKey = x::runtime::il2cpp_container::OffDictEntryKey();
    const size_t offVal = x::runtime::il2cpp_container::OffDictEntryValuePtr();
    for (uintptr_t i = 0; i < n; ++i) {
        uint8_t* e =
            x::runtime::il2cpp_container::DictEntryAt(entries, static_cast<int>(i), stride);
        if (!e) continue;
        if (ReadI32(e, offHash) < 0) continue;
        if (ReadI32(e, offKey) != key) continue;
        void* v = ReadPtr(e, offVal);
        return LooksLikeHeapPtr(v) ? v : nullptr;
    }
    return nullptr;
}

// 官方 GetRandomMeleeAttackAction 的底表：SkillInfo.MeleeAttackAction[weaponType]。
// 只读静态 Dictionary / List，不调 5 参托管方法。必须在 MainPump 上调用。
int TryDictMeleeActions(int wt, int* out, int cap) {
    if (wt <= 0 || !out || cap <= 0 || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    if (!gSkillInfoKlass) gSkillInfoKlass = FindClass(kSkillInfoClass);
    if (!gSkillInfoKlass) return 0;
    if (!gOffMeleeAttackAction) {
        gOffMeleeAttackAction = FieldOffsetByHash(gSkillInfoKlass, kHashMeleeAttackAction);
        if (gOffMeleeAttackAction & 0x80000000u)
            gOffMeleeAttackAction &= 0x7FFFFFFFu;
        if (!gOffMeleeAttackAction) gOffMeleeAttackAction = kOffMeleeAttackActionFb;
    }
    auto staticsOf = [&]() -> void* {
        if (!e.classStaticData) return nullptr;
        void* sf = nullptr;
        __try {
            sf = e.classStaticData(gSkillInfoKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sf = nullptr;
        }
        return LooksLikeHeapPtr(sf) ? sf : nullptr;
    };
    void* sf = staticsOf();
    void* dict = sf ? ReadPtr(sf, gOffMeleeAttackAction) : nullptr;
    if (!LooksLikeHeapPtr(dict)) {
        x::runtime::il2cpp::RuntimeClassInit(gSkillInfoKlass);
        sf = staticsOf();
        dict = sf ? ReadPtr(sf, gOffMeleeAttackAction) : nullptr;
    }
    if (!LooksLikeHeapPtr(dict)) return 0;
    return FillActionsFromList(DictGetIntPtr(dict, wt), out, cap);
}

int PickMeleeAttackAction(int wt) {
    const DWORD now = GetTickCount();
    if (wt != gCachedMeleeWt || gMeleeActN <= 0 || now - gCachedMeleeMs >= 8000) {
        const bool cycle = WeaponNeedsActionCycle(wt);
        int n = TryDictMeleeActions(wt, gMeleeActs, 4);
        const char* via = "dict";
        if (cycle) {
            if (n < 2) {
                n = FillFallbackFamily(wt, gMeleeActs, 4);
                via = "fb";
            }
        } else {
            if (n <= 0) {
                n = FillFallbackFamily(wt, gMeleeActs, 4);
                via = "fb";
            } else {
                n = 1;  // 短刀/单手：只用表首项，不轮 O2/O3
            }
        }
        gCachedMeleeWt = wt;
        gMeleeActN = n;
        gMeleeSeq = 0;
        gCachedMeleeMs = now;
        gCachedMeleeVia = via;
        x::runtime::LogI("AttackRpc", "melee action wt=%d n=%d a0=%d a1=%d a2=%d via=%s cycle=%d",
                         wt, n, n > 0 ? gMeleeActs[0] : 0, n > 1 ? gMeleeActs[1] : 0,
                         n > 2 ? gMeleeActs[2] : 0, via, cycle ? 1 : 0);
    }
    if (gMeleeActN <= 0) return FallbackMeleeAction(wt);
    const int act = gMeleeActs[gMeleeSeq % gMeleeActN];
    if (gMeleeActN > 1) ++gMeleeSeq;
    return act;
}

int ReadWeaponTypeOnPump() {
    int wt = 0;
    __try {
        wt = x::features::final_attack_force::QueryEquippedWeaponType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wt = 0;
    }
    return wt;
}

// 组包只覆盖普攻。A 槽是技能 / 宏 / 非出刀绑定 → err，调用方改走 OnFuncKey。
// 空绑由 attack_input 合成 5/52，这里看成普攻。
bool ResolveASlotAttackOnPump(int* outSkill, int* outOp, int* outWt, int* outFkT, int* outFkV,
                              const char** outSrc, const char** outErr) {
    const int wt = ReadWeaponTypeOnPump();
    const int opcode = OpcodeFromWeaponType(wt);
    int skillId = 0;
    int fkT = -1;
    int fkV = -1;
    const char* src = "a_na.equipWt";
    const char* err = nullptr;
    if (x::features::ports::attack::PeekAttackBinding(&fkT, &fkV)) {
        if (fkT == kFuncTypeSkill && fkV > 0) {
            skillId = fkV;
            src = "a_skill";
            err = "a_slot_skill";
        } else if (fkT == kFuncTypeMacroSkill) {
            src = "a_macro";
            err = "a_slot_skill";
        } else if (fkT == kFuncTypeBasicAction && fkV == kFkmBasicActionAttack) {
            src = "a_na.equipWt";
        } else if (fkT == kFuncTypeItem || fkT == kFuncTypeEmotion || fkT == kFuncTypeMenu) {
            src = "a_other";
            err = "use_onfunckey";
        }
    }
    // 现网 send.log 无 op=51 BODY；51/52 头字段补 0 已 BIN 踢号（盗贼飞镖 wt=47）。
    if (!err && (opcode == kOpcodeShootAttack || opcode == kOpcodeMagicAttack)) {
        err = "op_not_melee";
        src = opcode == kOpcodeShootAttack ? "a_na.shoot" : "a_na.magic";
    }
    if (outSkill) *outSkill = skillId;
    if (outOp) *outOp = opcode;
    if (outWt) *outWt = wt;
    if (outFkT) *outFkT = fkT;
    if (outFkV) *outFkV = fkV;
    if (outSrc) *outSrc = src;
    if (outErr) *outErr = err;
    return err == nullptr;
}

// TryDoingNormalAttack 在 Create 前：SetAttackAction(lu, action, nAttackSpeed, null, 0)；
// Collect 在出站 Tap（SendOut 不直调）——探针显式补一刀，避免本地窗与出站脱节。
// 失败只打日志，不阻断 forge（避免 SetAttackAction 参数漂导致探针哑火）。
void TryLocalAttackPrereq(void* localUser, int32_t actionId, int32_t attackSpeed, int opcode) {
    if (!gGA) return;
    if (!gSetAttackAction) {
        void* userKlass = x::runtime::il2cpp::FindClass("", kHashUserClass);
        using namespace x::runtime::il2cpp_method;
        constexpr MethodShape kSa{4, TypeKind::Bool, true, true,
                                  {TypeKind::I32, TypeKind::I32, TypeKind::Ptr, TypeKind::I32}};
        gMiSetAttackAction =
            ResolveMi(userKlass, kRvaSetAttackAction, kSa, "SetAttackAction", kHashSetAttackAction);
        gSetAttackAction = FnFromMi<FnSetAttackAction>(gMiSetAttackAction, kRvaSetAttackAction);
    }
    if (!gCollectAttackPacket) {
        void* sc = x::runtime::il2cpp::FindClass("", kHashSecurityClient);
        using namespace x::runtime::il2cpp_method;
        constexpr MethodShape kCol{1, TypeKind::Void, true, false, {TypeKind::Any}};
        gMiCollectAttackPacket = ResolveMi(sc, kRvaCollectAttackPacket, kCol, "CollectAttackPacket",
                                           kHashCollectAttackPacket);
        gCollectAttackPacket =
            FnFromMi<FnCollectAttackPacket>(gMiCollectAttackPacket, kRvaCollectAttackPacket);
    }
    if (LooksLikeHeapPtr(localUser) && gSetAttackAction) {
        bool ok = false;
        __try {
            // CMS：SetAttackAction(nAttackAction, nAttackSpeed, skill=null, nSLV=0)
            ok = gSetAttackAction(localUser, actionId, attackSpeed, nullptr, 0, gMiSetAttackAction);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        x::runtime::LogI("AttackRpc", "SetAttackAction action=%d spd=%d ok=%d", actionId,
                         attackSpeed, ok ? 1 : 0);
    }
    if (gCollectAttackPacket) {
        __try {
            gCollectAttackPacket(static_cast<uint16_t>(opcode), gMiCollectAttackPacket);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gGA || !rva) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods) return nullptr;
    void* iter = nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(gGA) + rva;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(raw);
            if (reinterpret_cast<uintptr_t>(mi->methodPointer) == want ||
                reinterpret_cast<uintptr_t>(mi->virtualMethodPointer) == want) {
                return mi;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool ResolveApi() {
    if (gGA && gFindAll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    return gGA != nullptr;
}

// Worker 调 Rebind 时必须走泵；已在主线程 job 内则直调（禁嵌套 InvokeAndWait）。
void* SafeFindAll(void* typeObj) {
    if (!gFindAll || !typeObj) return nullptr;
    // 裸 gFindAll 不经 managed_main 包装 —— 自检仓级闸。
    if (x::runtime::managed_main::IsLoginFrozen() ||
        x::runtime::managed_main::IsMapTransitBlocked() || !world::IsPlayReady())
        return nullptr;
    if (x::runtime::main_thread::IsOnPumpThread()) {
        void* arr = nullptr;
        __try {
            arr = gFindAll(typeObj, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr = nullptr;
        }
        return arr;
    }
    return x::runtime::managed_main::FindAll(gFindAll, typeObj, 2000, false);
}

bool Rebind(DWORD now, bool force = false) {
    if (!force && now - gLastRebindMs < kRebindMs && gReady.load()) return true;
    gLastRebindMs = now;
    if (!ResolveApi()) {
        gReady.store(false);
        return false;
    }

    if (!gFacadeKlass) gFacadeKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    if (!gSessionKlass) gSessionKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    if (!gFacadeType && gFacadeKlass) gFacadeType = ClassTypeObject(gFacadeKlass);
    if (!gOutPacketKlass) gOutPacketKlass = FindClass(kOutPacketClass);
    if (!gOutPacketKlass) gOutPacketKlass = FindClass("OutPacket");
    if (!gWorldManagerKlass) gWorldManagerKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!gMiGetUpdateTime && gWorldManagerKlass) {
        using namespace x::runtime::il2cpp_method;
        constexpr MethodShape kUt{0, TypeKind::I32, true, true, {}};
        gMiGetUpdateTime = ResolveMi(gWorldManagerKlass, kRvaWorldManagerGetUpdateTime, kUt,
                                     "GetUpdateTime", kHashGetUpdateTime);
    }
    if (!gGetUpdateTime)
        gGetUpdateTime = FnFromMi<FnGetUpdateTime>(gMiGetUpdateTime, kRvaWorldManagerGetUpdateTime);

    if (gNm && !LooksLikeSession(gNm)) gNm = nullptr;
    if (gNmFacade && !LooksLikeFacade(gNmFacade)) gNmFacade = nullptr;
    if (!gNmFacade) gNmFacade = ResolveFacadeSingleton(gFacadeKlass);
    if (!gNmFacade && gFacadeType) {
        void* arr = SafeFindAll(gFacadeType);
        const int n = LooksLikeHeapPtr(arr)
                          ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                                reinterpret_cast<uint8_t*>(arr) + 0x18))
                          : 0;
        for (int i = 0; i < n && i < 8; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                                static_cast<size_t>(i) * sizeof(void*));
            if (!LooksLikeFacade(o)) continue;
            gNmFacade = o;
            break;
        }
        // FindAll 样本 klass 不匹配时：仍尝试 +0x10 取 Session（shop 同兜底）。
        if (!gNmFacade && n > 0) {
            void* sample = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
            if (LooksLikeHeapPtr(sample)) {
                void* sess = ReadPtr(sample, kOffNmSession);
                if (LooksLikeHeapPtr(sess)) {
                    if (LooksLikeFacade(sample)) {
                        gNmFacade = sample;
                    } else {
                        gNm = sess;
                        if (!gSessionKlass) {
                            void* sk = ReadPtr(sess, 0);
                            if (LooksLikeHeapPtr(sk)) gSessionKlass = sk;
                        }
                    }
                }
            }
        }
    }
    if (!gNm && gNmFacade) gNm = SessionFromFacade(gNmFacade);
    // 再兜底：facade 校验失败但 +0x10 有 Session
    if (!gNm && gNmFacade) {
        void* sess = ReadPtr(gNmFacade, kOffNmSession);
        if (LooksLikeHeapPtr(sess)) gNm = sess;
    }

    if (gOutPacketKlass) {
        using namespace x::runtime::il2cpp_method;
        constexpr MethodShape kCreate{1, TypeKind::Ptr, true, false, {TypeKind::Any}};
        constexpr MethodShape kEnc{1, TypeKind::Void, true, false, {TypeKind::Any}};
        constexpr MethodShape kEnc4{1, TypeKind::Void, true, false, {TypeKind::I32}};
        constexpr MethodShape kEncV2{1, TypeKind::Void, true, false, {TypeKind::Any}};
        if (!gMiCreate)
            gMiCreate =
                ResolveMi(gOutPacketKlass, kRvaOutPacketCreate, kCreate, "Create", kHashOutCreate);
        if (!gMiE1)
            gMiE1 =
                ResolveMi(gOutPacketKlass, kRvaEncode1Byte, kEnc, "Encode1", kHashEncode1Byte);
        if (!gMiE1Bool)
            gMiE1Bool =
                ResolveMi(gOutPacketKlass, kRvaEncode1Bool, kEnc, nullptr, kHashEncode1Bool);
        if (!gMiE2U)
            gMiE2U =
                ResolveMi(gOutPacketKlass, kRvaEncode2Ushort, kEnc, nullptr, kHashEncode2Ushort);
        if (!gMiE2S)
            gMiE2S =
                ResolveMi(gOutPacketKlass, kRvaEncode2Short, kEnc, nullptr, kHashEncode2Short);
        if (!gMiE4)
            gMiE4 = ResolveMi(gOutPacketKlass, kRvaEncode4Int, kEnc4, "Encode4", kHashEncode4Int);
        if (!gMiE4U)
            gMiE4U =
                ResolveMi(gOutPacketKlass, kRvaEncode4Uint, kEnc, nullptr, kHashEncode4Uint);
        if (!gMiEV2)
            gMiEV2 = ResolveMi(gOutPacketKlass, kRvaEncodeVector2, kEncV2, "Encode",
                               kHashEncodeVector2);
    }
    if (gFacadeKlass && !gMiSendOut) {
        // bool(OutPacket) on NetworkManager facade — HashSet 门后再 Session.SendPacket。
        x::runtime::il2cpp_method::MethodShape kSend{};
        kSend.arity = 1;
        kSend.ret = x::runtime::il2cpp_method::TypeKind::Bool;
        kSend.unique = true;
        kSend.walkParents = true;
        kSend.param[0] = x::runtime::il2cpp_method::TypeKind::Ptr;
        if (gOutPacketKlass) kSend.paramKlass[0] = gOutPacketKlass;
        gMiSendOut =
            ResolveMi(gFacadeKlass, kRvaSendOutPacket, kSend, "SendOutPacket", kHashSendOutPacket);
        if (gMiSendOut) {
            x::runtime::LogI("AttackRpc", "Network.SendOutPacket MethodInfo ok");
        } else {
            x::runtime::LogW("AttackRpc",
                             "SendOutPacket MethodInfo missing; call with null MI (official sites do)");
        }
    }

    const bool ok = gGA && gOutPacketKlass && gMiCreate && gMiE1 && gMiE4 && gMiEV2 &&
                    AtRva<void*>(kRvaSendOutPacket);
    gReady.store(ok);
    return ok;
}

struct FireJob {
    FireResult* out = nullptr;
    bool allowDirect = false;  // 多发直发 / 打怪自组包：跳过 gEnabled
    bool oneshot = false;      // 面板按钮：命中环最多 1
    int32_t lockOid = 0;       // >0：命中环只填此 oid（须在 snap）
    bool ok = false;
    const char* err = "init";
    int mobs = 0;
    int bodyHint = 0;
    int opcode = 0;
    int weaponType = 0;
    int skillId = 0;
    int fkType = -1;
    int fkValue = -1;
};

void Encode1(void* pkt, uint8_t v) {
    auto* fn = reinterpret_cast<FnEncode1>(gMiE1 && gMiE1->methodPointer ? gMiE1->methodPointer
                                                                          : AtRva<void*>(kRvaEncode1Byte));
    if (fn) fn(pkt, v, gMiE1);
}

void Encode1Bool(void* pkt, bool v) {
    auto* fn = reinterpret_cast<FnEncode1Bool>(
        gMiE1Bool && gMiE1Bool->methodPointer ? gMiE1Bool->methodPointer
                                              : AtRva<void*>(kRvaEncode1Bool));
    if (fn) fn(pkt, v, gMiE1Bool);
}

void Encode2U(void* pkt, uint16_t v) {
    auto* fn = reinterpret_cast<FnEncode2U>(
        gMiE2U && gMiE2U->methodPointer ? gMiE2U->methodPointer : AtRva<void*>(kRvaEncode2Ushort));
    if (fn) fn(pkt, v, gMiE2U);
}

void Encode2S(void* pkt, int16_t v) {
    auto* fn = reinterpret_cast<FnEncode2S>(
        gMiE2S && gMiE2S->methodPointer ? gMiE2S->methodPointer : AtRva<void*>(kRvaEncode2Short));
    if (fn) fn(pkt, v, gMiE2S);
}

void Encode4(void* pkt, int32_t v) {
    auto* fn = reinterpret_cast<FnEncode4>(gMiE4 && gMiE4->methodPointer ? gMiE4->methodPointer
                                                                          : AtRva<void*>(kRvaEncode4Int));
    if (fn) fn(pkt, v, gMiE4);
}

void Encode4U(void* pkt, uint32_t v) {
    auto* fn = reinterpret_cast<FnEncode4U>(
        gMiE4U && gMiE4U->methodPointer ? gMiE4U->methodPointer : AtRva<void*>(kRvaEncode4Uint));
    if (fn) fn(pkt, v, gMiE4U);
}

void EncodeV2(void* pkt, float x, float y) {
    auto* fn = reinterpret_cast<FnEncodeV2>(
        gMiEV2 && gMiEV2->methodPointer ? gMiEV2->methodPointer : AtRva<void*>(kRvaEncodeVector2));
    if (!fn) return;
    Vec2 v{x, y};
    fn(pkt, v, gMiEV2);
}

void FireJobOnMain(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("attack_rpc.Fire");
    auto* job = reinterpret_cast<FireJob*>(user);
    if (!job) return;
    job->err = "job";

    if (!job->allowDirect && !gEnabled.load()) {
        job->err = "disabled";
        return;
    }
    if (!world::IsPlayReady()) {
        job->err = "not_in_map";
        return;
    }
    if (!Rebind(GetTickCount())) {
        job->err = "bind_fail";
        return;
    }
    if (!gNm || !LooksLikeSession(gNm)) {
        gNm = nullptr;
        const DWORD nowMs = GetTickCount();
        if (!gLastNmFindAllMs || static_cast<DWORD>(nowMs - gLastNmFindAllMs) >= 3000) {
            gLastNmFindAllMs = nowMs;
            if (gNmFacade && !LooksLikeFacade(gNmFacade)) gNmFacade = nullptr;
            if (!gNmFacade) gNmFacade = ResolveFacadeSingleton(gFacadeKlass);
            if (!gNmFacade && gFacadeType) {
                void* arr = SafeFindAll(gFacadeType);
                const int n = LooksLikeHeapPtr(arr)
                                  ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                                        reinterpret_cast<uint8_t*>(arr) + 0x18))
                                  : 0;
                for (int i = 0; i < n && i < 8; ++i) {
                    void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                                        static_cast<size_t>(i) * sizeof(void*));
                    if (!LooksLikeFacade(o)) continue;
                    gNmFacade = o;
                    break;
                }
                if (!gNmFacade && n > 0) {
                    void* sample = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
                    void* sess = LooksLikeHeapPtr(sample) ? ReadPtr(sample, kOffNmSession) : nullptr;
                    if (LooksLikeHeapPtr(sess)) gNm = sess;
                }
            }
            if (!gNm && gNmFacade) {
                gNm = SessionFromFacade(gNmFacade);
                if (!gNm) {
                    void* sess = ReadPtr(gNmFacade, kOffNmSession);
                    if (LooksLikeHeapPtr(sess)) gNm = sess;
                }
            }
        }
    }
    if (!gNm) {
        job->err = "no_nm";
        static DWORD sLastNoNmLog = 0;
        const DWORD t = GetTickCount();
        if (!sLastNoNmLog || static_cast<DWORD>(t - sLastNoNmLog) >= 3000) {
            sLastNoNmLog = t;
            void* sample = nullptr;
            int nFind = -1;
            int stFac = -1, stSess = -1;
            void* sess = nullptr;
            void* sampleKlass = nullptr;
            if (gFacadeType) {
                void* arr = SafeFindAll(gFacadeType);
                nFind = LooksLikeHeapPtr(arr)
                            ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                                  reinterpret_cast<uint8_t*>(arr) + 0x18))
                            : 0;
                if (nFind > 0) {
                    sample = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
                    if (LooksLikeHeapPtr(sample)) {
                        sampleKlass = ReadPtr(sample, 0);
                        sess = ReadPtr(sample, kOffNmSession);
                        stFac = ReadI32(sample, kOffNmSessionState);
                        if (LooksLikeHeapPtr(sess)) stSess = ReadI32(sess, kOffSessionState);
                    }
                }
            }
            x::runtime::LogW("AttackRpc",
                             "no_nm facade=%p sessionKlass=%p type=%p findN=%d sample=%p sk=%p "
                             "stFac=%d stSess=%d sess=%p",
                             gFacadeKlass, gSessionKlass, gFacadeType, nFind, sample, sampleKlass,
                             stFac, stSess, sess);
        }
        return;
    }

    player_combat::CombatCtx ctx{};
    if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok) {
        job->err = "no_user";
        return;
    }

    mob::Snapshot snap{};
    if (!mob::Collect(snap) || !snap.ok) {
        job->err = "no_mobs_snap";
        return;
    }

    const int dmg = gDamage.load();
    constexpr int kSkillExtra = 0;
    struct Hit {
        int32_t oid = 0;
        float x = 0.f;
        float y = 0.f;
        int32_t ctrl = 0;
        bool fly = false;
    };
    Hit hits[kMaxMobsHard]{};
    int nHit = 0;
    const int32_t lockOid = job->lockOid;
    if (lockOid > 0) {
        for (int i = 0; i < snap.count; ++i) {
            const auto& m = snap.mobs[i];
            if (m.id != lockOid) continue;
            if (!m.ready || m.deadType != 0) continue;
            if (m.hpPct <= 0) continue;
            float mx = m.x;
            float my = m.y;
            if (LooksLikeHeapPtr(m.ptr)) {
                const float px = ReadF32(m.ptr, kOffMobPos);
                const float py = ReadF32(m.ptr, kOffMobPos + 4);
                if (std::fabs(px) > 0.5f || std::fabs(py) > 0.5f) {
                    mx = px;
                    my = py;
                }
            }
            hits[0].oid = m.id;
            hits[0].x = mx;
            hits[0].y = my;
            hits[0].ctrl = m.ctrl;
            hits[0].fly = MobIsFlyFamily(m.ptr);
            nHit = 1;
            break;
        }
        if (nHit <= 0) {
            job->err = "lock_missing";
            x::runtime::LogW("AttackRpc", "forge_lock miss oid=%d snap=%d", lockOid, snap.count);
            return;
        }
        const float ldx = hits[0].x - ctx.x;
        const float ldy = hits[0].y - ctx.y;
        const float adx = std::fabs(ldx);
        const float ady = std::fabs(ldy);
        const float maxDx = static_cast<float>(gLockFrontDx.load(std::memory_order_acquire));
        const float maxDy = static_cast<float>(gLockFrontDy.load(std::memory_order_acquire));
        // 与 HiraishinFrontOk 同一把尺：轴对齐盒，不是 hypot。0=该轴不限。
        if ((maxDx > 0.f && adx > maxDx) || (maxDy > 0.f && ady > maxDy)) {
            job->err = "too_far";
            static DWORD sFar = 0;
            const DWORD t = GetTickCount();
            if (!sFar || static_cast<DWORD>(t - sFar) >= 2000) {
                sFar = t;
                x::runtime::LogW("AttackRpc",
                                 "forge_lock too_far oid=%d dx=%.0f dy=%.0f "
                                 "box=(%.0f,%.0f) player=(%.0f,%.0f) mob=(%.0f,%.0f)",
                                 lockOid, adx, ady, maxDx, maxDy, ctx.x, ctx.y, hits[0].x,
                                 hits[0].y);
            }
            return;
        }
    } else {
    // 候选：贴脸优先。我控只是排序加分，不挡 Passive（2026-08-15 蜗牛 Passive dist=17 已打死）。
    const int want = job->oneshot ? 1 : gMaxMobs.load();
    struct Cand {
        int32_t oid = 0;
        float x = 0.f;
        float y = 0.f;
        float dist = 0.f;
        int32_t ctrl = 0;
        bool fly = false;
    };
    Cand cands[kMaxMobsHard]{};
    int nCand = 0;
    int nNear = 0;
    int nNearOurs = 0;
    float nearestDist = -1.f;
    int32_t nearestOid = 0;
    int32_t nearestCtrl = 0;
    for (int i = 0; i < snap.count && nCand < kMaxMobsHard; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0) continue;
        if (m.hpPct <= 0) continue;
        float mx = m.x;
        float my = m.y;
        if (LooksLikeHeapPtr(m.ptr)) {
            const float px = ReadF32(m.ptr, kOffMobPos);
            const float py = ReadF32(m.ptr, kOffMobPos + 4);
            if (std::fabs(px) > 0.5f || std::fabs(py) > 0.5f) {
                mx = px;
                my = py;
            }
        }
        const float dx = mx - ctx.x;
        const float dy = my - ctx.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (nearestDist < 0.f || dist < nearestDist) {
            nearestDist = dist;
            nearestOid = m.id;
            nearestCtrl = m.ctrl;
        }
        if (dist > kMeleeMaxDist) continue;
        ++nNear;
        if (m.ctrl > 0) ++nNearOurs;
        cands[nCand].oid = m.id;
        cands[nCand].x = mx;
        cands[nCand].y = my;
        cands[nCand].dist = dist;
        cands[nCand].ctrl = m.ctrl;
        cands[nCand].fly = MobIsFlyFamily(m.ptr);
        ++nCand;
    }
    // 排序：我控优先，同档按距升序
    for (int a = 0; a < nCand; ++a) {
        for (int b = a + 1; b < nCand; ++b) {
            const bool swap =
                (cands[b].ctrl > 0 && cands[a].ctrl <= 0) ||
                ((cands[b].ctrl > 0) == (cands[a].ctrl > 0) && cands[b].dist < cands[a].dist);
            if (swap) {
                Cand t = cands[a];
                cands[a] = cands[b];
                cands[b] = t;
            }
        }
    }
    for (int i = 0; i < nCand && nHit < want && nHit < kMaxMobsHard; ++i) {
        hits[nHit].oid = cands[i].oid;
        hits[nHit].x = cands[i].x;
        hits[nHit].y = cands[i].y;
        hits[nHit].ctrl = cands[i].ctrl;
        hits[nHit].fly = cands[i].fly;
        ++nHit;
    }
    if (nHit <= 0) {
        job->err = "no_near_targets";  // 旁无贴脸怪（>50px 不打；180 会 normal ok 但服端空枪）
        static DWORD sLastFarLog = 0;
        const DWORD t = GetTickCount();
        if (!sLastFarLog || static_cast<DWORD>(t - sLastFarLog) >= 2000) {
            sLastFarLog = t;
            x::runtime::LogW("AttackRpc",
                             "no near mob (need dist<=%.0f); nearest oid=%d dist=%.0f ctrl=%d "
                             "near=%d ours=%d player=(%.0f,%.0f) snap=%d",
                             kMeleeMaxDist, nearestOid, nearestDist, nearestCtrl, nNear, nNearOurs,
                             ctx.x, ctx.y, snap.count);
        }
        return;
    }
    }

    bool faceLeft = false;
    if (nHit > 0) faceLeft = (hits[0].x < ctx.x);

    int skillId = 0;
    int opcode = kOpcodeMeleeAttack;
    int fkT = -1;
    int fkV = -1;
    const char* opSrc = "weapon";
    const char* slotErr = nullptr;
    int wt = 0;
    if (!ResolveASlotAttackOnPump(&skillId, &opcode, &wt, &fkT, &fkV, &opSrc, &slotErr)) {
        job->weaponType = wt;
        job->opcode = opcode;
        job->skillId = skillId;
        job->fkType = fkT;
        job->fkValue = fkV;
        job->err = slotErr ? slotErr : "a_slot_not_attack";
        x::runtime::LogI("AttackRpc", "skip forge err=%s op=%d wt=%d fkT=%d fkV=%d src=%s skill=%d",
                         job->err, opcode, wt, fkT, fkV, opSrc, skillId);
        return;
    }
    const int weaponType = wt;
    job->weaponType = weaponType;
    job->opcode = opcode;
    job->skillId = skillId;
    job->fkType = fkT;
    job->fkValue = fkV;
    if (opcode != kOpcodeMeleeAttack) {
        job->err = "op_not_melee";
        x::runtime::LogI("AttackRpc", "skip forge err=op_not_melee op=%d wt=%d src=%s", opcode,
                         weaponType, opSrc);
        return;
    }
    if (weaponType == 0) {
        x::runtime::LogW("AttackRpc", "weaponType=0 fallback opcode=%d src=%s fkT=%d fkV=%d skill=%d",
                         opcode, opSrc, fkT, fkV, skillId);
    } else {
        static int sOpLogs = 0;
        static int sLastOp = -1;
        static int sLastSkill = -1;
        if (sOpLogs < 8 || opcode != sLastOp || skillId != sLastSkill) {
            x::runtime::LogI("AttackRpc", "opcode src=%s op=%d wt=%d skill=%d fkT=%d fkV=%d", opSrc,
                             opcode, weaponType, skillId, fkT, fkV);
            ++sOpLogs;
            sLastOp = opcode;
            sLastSkill = skillId;
        }
    }

    int was = 0;
    int boost = 0;
    const int attackSpeed = ReadAvatarAttackSpeed(ctx.localUser, &was, &boost);
    const bool twoHand = WeaponIsTwoHandFamily(weaponType);
    const bool oneHand = WeaponIsOneHandSwing(weaponType);
    const bool dagger = WeaponIsDagger(weaponType);
    const bool typeThenSpeed = twoHand || oneHand;
    const bool ringAtMob = WeaponRingAtMob(weaponType);
    const uint8_t foreAction = WeaponForeAction(weaponType);
    const uint16_t hitDelay = WeaponHitDelay(weaponType, attackSpeed);
    const char* fam = MeleeFamilyName(weaponType);
    // 本地挥砍动画用 3，看起来快。线上速度字节必须写真 degree：
    // 双手剑 BIN 把第二字节改成 3 → 100→99 磨血。
    constexpr int kForgeFastDegree = 3;
    const int animSpd = typeThenSpeed ? kForgeFastDegree : attackSpeed;

    uint16_t action = static_cast<uint16_t>(PickMeleeAttackAction(weaponType) & 0x7FFF);
    if (faceLeft) action = static_cast<uint16_t>(action | 0x8000u);
    const int attackTypeRaw = ReadAvatarAttackType(ctx.localUser);
    const int attackType = ResolveWireAttackType(weaponType, attackTypeRaw, was);
    uint8_t hdr0 = 0;
    uint8_t hdr1 = 0;
    static int sSpdLogs = 0;
    if (sSpdLogs < 16) {
        x::runtime::LogI("AttackRpc",
                         "melee encode fam=%s wt=%d action=%d spd=%d was=%d boost=%d "
                         "atkType=%d rawAt=%d seq=%d n=%d ring=%s delay=%u fa=0x%02X",
                         fam, weaponType, action & 0x7FFF, animSpd, was, boost, attackType,
                         attackTypeRaw, gMeleeSeq, gMeleeActN, ringAtMob ? "mob" : "feet",
                         static_cast<unsigned>(hitDelay), foreAction);
        ++sSpdLogs;
    }

    // 对齐正路：Create 前先 SetAttackAction(action, nAttackSpeed) + Collect
    TryLocalAttackPrereq(ctx.localUser, static_cast<int32_t>(action & 0x7FFF), animSpd, opcode);

    auto* create = reinterpret_cast<FnOutCreate>(
        gMiCreate && gMiCreate->methodPointer ? gMiCreate->methodPointer
                                              : AtRva<void*>(kRvaOutPacketCreate));
    if (!create) {
        job->err = "no_create";
        return;
    }

    void* pkt = nullptr;
    __try {
        pkt = create(opcode, gMiCreate);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pkt = nullptr;
    }
    if (!LooksLikeHeapPtr(pkt)) {
        job->err = "create_fail";
        return;
    }

    // Header：对齐 send.log 真包（portal 恒 0x01；flags 高 nibble = mobCount）
    // off=55：01 11 | skill0×2 | bool0 | action | 01 05 | tOrKey | 命中环 | 玩家i16
    const uint8_t flags =
        static_cast<uint8_t>((1u & 0xFu) | (static_cast<uint32_t>(nHit) << 4));
    Encode1(pkt, 1);  // portal：真包一律 0x01
    Encode1(pkt, flags);
    Encode4(pkt, skillId);
    Encode4(pkt, kSkillExtra);
    // P0c：Shoot 头比 Melee 多 Encode4；Magic 多 Encode4。占位 0，等 51/52 wire BIN。
    if (opcode == kOpcodeShootAttack || opcode == kOpcodeMagicAttack) Encode4(pkt, 0);
    Encode1Bool(pkt, false);

    Encode2U(pkt, action);
    // 短刀 BIN：WAS + 恒 05（`03 05`）。其余近战：AttackType + 真 degree。
    // 单手剑：1302000 WAS=4 → `01 04`；WAS=5 剑才是 P0c `01 05`。双手剑真包 `05 06`。
    if (dagger) {
        hdr0 = static_cast<uint8_t>(ClampAttackSpeed(attackSpeed));
        hdr1 = 5;
    } else {
        hdr0 = static_cast<uint8_t>(attackType);
        hdr1 = static_cast<uint8_t>(ClampAttackSpeed(attackSpeed));
    }
    Encode1(pkt, hdr0);
    Encode1(pkt, hdr1);
    // tOrKey：必须用游戏钟 GetUpdateTime（~1e4–1e5）；GetTickCount 量级不对服端会吞伤
    int tOrKey = GetGameUpdateTimeMs();
    if (tOrKey <= 0) tOrKey = static_cast<int>(GetTickCount() & 0x7FFFFFFF);
    Encode4(pkt, tOrKey);
    // P0c：Shoot 头在 tOrKey 后再两枚 Encode2。
    if (opcode == kOpcodeShootAttack) {
        Encode2S(pkt, 0);
        Encode2S(pkt, 0);
    }

    auto encXY = [&](float fx, float fy) {
        // fx/fy = Unity AbsPos（更大 Y=更高）。官方 EncodeVector2（VA 0x7FFD745199D0）
        // 对 Y 做 add edx,80000000h 再 cvttsd2si，线上 = i16(x), i16(-y) = Maple Y-down。
        // send.log op=50：蜗牛/近战 Y 为 C6 00=+198，与 Ap.y≈-198 互为取反。
        int x = static_cast<int>(fx);
        int y = static_cast<int>(-fy);
        if (x < -32768) x = -32768;
        if (x > 32767) x = 32767;
        if (y < -32768) y = -32768;
        if (y > 32767) y = 32767;
        Encode2S(pkt, static_cast<int16_t>(x));
        Encode2S(pkt, static_cast<int16_t>(y));
    };

    // 命中环：两 XY 后是 Delay u16（send.log）+ dmg i32 + field u32(=0) + 玩家 XY。
    const float dropAimX = ctx.x + (faceLeft ? -kDropFootReachX : kDropFootReachX);
    const float dropAimY = ctx.y;
    int body = 19;
    if (opcode == kOpcodeShootAttack) body += 8;  // extra Encode4 + Encode2×2
    if (opcode == kOpcodeMagicAttack) body += 4;
    for (int i = 0; i < nHit; ++i) {
        Encode4(pkt, hits[i].oid);
        Encode1(pkt, 6);  // HitAction：双手剑/短刀真包都是 06
        // 短刀 P0c：ForeAction 0x81。双手剑 send.log：00/06，从不是 0x81。
        Encode1(pkt, foreAction);
        Encode1(pkt, 0);
        Encode1(pkt, 1);
        // 短兵器写脚下（落物）。长兵器写怪坐标。飞行怪先不改环。
        const float ringX = ringAtMob ? hits[i].x : dropAimX;
        const float ringY = ringAtMob ? hits[i].y : dropAimY;
        encXY(ringX, ringY);
        encXY(ringX, ringY);
        // AttackInfo.Delay 上线成 u16。短刀钉 A5；双手剑钉 C2；单手按 degree 算。
        Encode2U(pkt, hitDelay);
        Encode4(pkt, dmg);
        Encode4U(pkt, 0);  // fieldId 真包 = 0（勿塞 mapId）
        body += 30;
    }
    encXY(ctx.x, ctx.y);
    body += 4;

    int off = 0;
    const uint8_t* bufObj = nullptr;
    __try {
        off = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pkt) + kOffPacketOffset);
        bufObj = *reinterpret_cast<const uint8_t**>(reinterpret_cast<uint8_t*>(pkt) + kOffPacketBuffer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        off = 0;
        bufObj = nullptr;
    }
    if (off > 0) body = off;

    // BODY hex：Il2CppArray 元素@+0x20，再跳 DataPos=6（与 kick_sniff 同约定）
    static int sHexLogs = 0;
    static DWORD sLastHexMs = 0;
    const DWORD nowHex = GetTickCount();
    const bool wantHex =
        bufObj && off > kPacketDataPos && off <= 256 &&
        (sHexLogs < 8 || static_cast<DWORD>(nowHex - sLastHexMs) >= 5000);
    if (wantHex) {
        char hex[768];
        int hp = 0;
        const uint8_t* bodyPtr =
            reinterpret_cast<const uint8_t*>(bufObj) + kIl2cppArrayData + kPacketDataPos;
        const int nBody = off - kPacketDataPos;
        const int n = nBody < 96 ? nBody : 96;
        for (int i = 0; i < n && hp + 4 < (int)sizeof(hex); ++i) {
            hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X", bodyPtr[i]);
            if (i + 1 < n) hp += snprintf(hex + hp, sizeof(hex) - hp, " ");
        }
        x::runtime::LogI("AttackRpc",
                         "forge BODY off=%d op=%d fam=%s wt=%d skill=%d fkT=%d fkV=%d src=%s mobs=%d "
                         "tOrKey=%d action=0x%04X hdr=%02X %02X oid=%d ctrl=%d(%s) dist=%.0f "
                         "player=(%.0f,%.0f) mob=(%.0f,%.0f) dropAimAp=(%.0f,%.0f) "
                         "wireY=%d fly=%d ring=%s hex=%s%s",
                         off, opcode, fam, weaponType, skillId, fkT, fkV, opSrc, nHit, tOrKey,
                         static_cast<unsigned>(action), hdr0, hdr1, hits[0].oid, hits[0].ctrl,
                         mob::CtrlName(hits[0].ctrl),
                         std::sqrt((hits[0].x - ctx.x) * (hits[0].x - ctx.x) +
                                   (hits[0].y - ctx.y) * (hits[0].y - ctx.y)),
                         ctx.x, ctx.y, hits[0].x, hits[0].y, dropAimX, dropAimY,
                         static_cast<int>(-dropAimY), hits[0].fly ? 1 : 0,
                         ringAtMob ? "mob" : "feet", hex,
                         nBody > 96 ? " ..." : "");
        ++sHexLogs;
        sLastHexMs = nowHex;
    }

    if (!LooksLikeHeapPtr(gNmFacade)) {
        job->err = "no_facade";
        return;
    }
    auto* sendOut = ResolveSendOutFn();
    if (!sendOut) {
        job->err = "no_send";
        return;
    }
    bool sent = false;
    __try {
        // this = NetworkManager facade（非 Session）；官方 caller 常传 MethodInfo=null。
        sent = sendOut(gNmFacade, pkt, gMiSendOut);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->err = "send_seh";
        return;
    }
    if (!sent) {
        // HashSet 未含 opcode / Session 空 / NM+0x18 门未过 → 官方门静默 false（未出站）
        int stFac = -1;
        void* hs = nullptr;
        __try {
            stFac = ReadI32(gNmFacade, kOffNmSessionState);
            hs = ReadPtr(gNmFacade, kOffNmOpcodeHashSet);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        x::runtime::LogW("AttackRpc", "send_false facade=%p stNm=%d hashset=%p (SendOut gate)",
                         gNmFacade, stFac, hs);
        // SendOut 静默 false（换图窗 / 僵尸 Session 常见）。调用方停这一刀，不退 OnFuncKey。
        job->err = "send_false";
        return;
    }

    job->ok = true;
    job->err = "ok";
    job->mobs = nHit;
    job->bodyHint = body;
    gSkillId.store(skillId);
    if (lockOid > 0) {
        x::runtime::LogI("AttackRpc",
                         "forge_lock sent oid=%d nHit=%d op=%d wt=%d skill=%d fkT=%d fkV=%d "
                         "src=%s body~%d dist=%.0f player=(%.0f,%.0f) mob=(%.0f,%.0f)",
                         lockOid, nHit, opcode, weaponType, skillId, fkT, fkV, opSrc, body,
                         std::sqrt((hits[0].x - ctx.x) * (hits[0].x - ctx.x) +
                                   (hits[0].y - ctx.y) * (hits[0].y - ctx.y)),
                         ctx.x, ctx.y, hits[0].x, hits[0].y);
    }
}

void CopyJobToResult(const FireJob& job, FireResult* r) {
    if (!r) return;
    r->ok = job.ok;
    r->mobs = job.mobs;
    r->bodyHint = job.bodyHint;
    r->opcode = job.opcode;
    r->weaponType = job.weaponType;
    r->skillId = job.skillId;
    r->fkType = job.fkType;
    r->fkValue = job.fkValue;
    r->err = job.err;
}

}  // namespace

void SetLockFrontBox(uint32_t dx, uint32_t dy) {
    dx = xcat::ClampHiraishinFrontDx(dx);
    dy = xcat::ClampHiraishinFrontDy(dy);
    gLockFrontDx.store(dx, std::memory_order_release);
    gLockFrontDy.store(dy, std::memory_order_release);
}

void InvalidateAfterMapChange() {
    gLastRebindMs = 0;
    gReady.store(false);
    gNm = nullptr;
    x::runtime::LogI("AttackRpc", "invalidate after map change");
}

void Init() {
    gEnabled.store(false);
    gNeedOffAck.store(false);
    gPendingReset.store(false);
    gReady.store(false);
    gLastFireMs = 0;
    x::runtime::LogI("AttackRpc",
                     "port init (default OFF) create@0x%X SendOut@0x%X SetAtk@0x%X Collect@0x%X "
                     "autoStop=%d sessionCap=%d",
                     kRvaOutPacketCreate, kRvaSendOutPacket, kRvaSetAttackAction,
                     kRvaCollectAttackPacket, kAutoStopAfterOk, kSessionForgeCap);
}

void Shutdown() {
    gEnabled.store(false);
    gReady.store(false);
}

bool EnsureBound() { return Rebind(GetTickCount()); }

bool Ready() { return gReady.load() || EnsureBound(); }

bool ResetSessionCap(const char* why) {
    const DWORD now = GetTickCount();
    if (gLastFireMs && static_cast<DWORD>(now - gLastFireMs) < kResetIdleMs) {
        gPendingReset.store(true);
        if (!why || std::strcmp(why, "queued") != 0) {
            x::runtime::LogW("AttackRpc",
                             "reset deferred: idle %lums < %lums why=%s session=%d/%d "
                             "(queued until idle)",
                             static_cast<unsigned long>(now - gLastFireMs),
                             static_cast<unsigned long>(kResetIdleMs), why ? why : "",
                             gOkSession.load(), kSessionForgeCap);
        }
        return false;
    }
    gPendingReset.store(false);
    const int prev = gOkSession.exchange(0);
    gOkSinceEnable.store(0);
    gConsecutiveFails.store(0);
    gFailBackoffUntilMs.store(0);
    x::runtime::LogI("AttackRpc", "session cap reset prev=%d/%d why=%s", prev, kSessionForgeCap,
                     why ? why : "");
    return true;
}

void SetEnabled(bool on) {
    if (on && gNeedOffAck.load()) {
        gEnabled.store(false);
        const DWORD t = GetTickCount();
        const DWORD last = gLastHoldLogMs.load();
        if (!last || static_cast<DWORD>(t - last) >= 2000) {
            gLastHoldLogMs.store(t);
            x::runtime::LogW("AttackRpc",
                             "enable refused: auto_stop hold-off (uncheck then recheck) "
                             "session=%d/%d",
                             gOkSession.load(), kSessionForgeCap);
        }
        return;
    }
    if (!on) gNeedOffAck.store(false);
    if (on) {
        const int sess = gOkSession.load();
        if (sess >= kSessionForgeCap) {
            if (!ResetSessionCap("reenable")) {
                gEnabled.store(false);
                x::runtime::LogW("AttackRpc",
                                 "enable refused: session forge cap %d/%d "
                                 "(wait >=2.5s then 清零计数, or uncheck and recheck)",
                                 sess, kSessionForgeCap);
                return;
            }
        }
    }
    const bool prev = gEnabled.exchange(on);
    if (prev == on) return;
    if (on) {
        gOkSinceEnable.store(0);
        gConsecutiveFails.store(0);
        gFailBackoffUntilMs.store(0);
    } else {
        gConsecutiveFails.store(0);
        gFailBackoffUntilMs.store(0);
    }
    x::runtime::LogI("AttackRpc",
                     "enabled=%d session_ok=%d/%d (wire dmg placeholder; server recalcs)",
                     on ? 1 : 0, gOkSession.load(), kSessionForgeCap);
}

bool IsEnabled() { return gEnabled.load(); }

void SetMaxMobs(int n) {
    if (n < 1) n = 1;
    if (n > kMaxMobsHard) n = kMaxMobsHard;
    gMaxMobs.store(n);
}

int GetMaxMobs() { return gMaxMobs.load(); }

void SetIntervalMs(DWORD ms) {
    if (ms < 800) ms = 800;  // 延后踢 BIN：低于 ~1s 连打更容易攒异常
    gIntervalMs.store(ms);
}

DWORD GetIntervalMs() { return gIntervalMs.load(); }

void SetDamage(int dmg) {
    if (dmg < 1) dmg = 1;
    gDamage.store(dmg);
}

int GetDamage() { return gDamage.load(); }

void SetSkillId(int /*skillId*/) {
    // skillId 由 A 槽决定；保留 API 以免旧调用方断链。
}

int GetSkillId() { return gSkillId.load(); }

void NoteFireResult(bool ok, const char* err, int mobs, int bodyHint, bool fromDirectMulti) {
    const DWORD now = GetTickCount();
    if (ok) {
        gConsecutiveFails.store(0);
        gFailBackoffUntilMs.store(0);
        const int nOk = gOkSinceEnable.fetch_add(1) + 1;
        const int nSess = gOkSession.fetch_add(1) + 1;
        x::runtime::LogI("AttackRpc",
                         "normal ok mobs=%d body~%d dmg=%d skill=%d ok#%d session#%d direct=%d",
                         mobs, bodyHint, gDamage.load(), gSkillId.load(), nOk, nSess,
                         fromDirectMulti ? 1 : 0);
        // 实验 TAB 探针才 auto_stop；多发直发不关 gEnabled。
        if (!fromDirectMulti && gEnabled.load() &&
            (nOk >= kAutoStopAfterOk || nSess >= kSessionForgeCap)) {
            gEnabled.store(false);
            gNeedOffAck.store(true);
            gStopGen.fetch_add(1);
            if (nSess >= kSessionForgeCap) {
                x::runtime::LogW("AttackRpc",
                                 "session_stop after %d ok (cap=%d; 清零计数 or wait 2.5s recheck)",
                                 nSess, kSessionForgeCap);
            } else {
                x::runtime::LogW("AttackRpc",
                                 "auto_stop after %d ok (burst; session %d/%d)", nOk, nSess,
                                 kSessionForgeCap);
            }
        }
        return;
    }
    const int n = gConsecutiveFails.fetch_add(1) + 1;
    // 1s → 2s → 4s → 8s → 10s；避免 no_nm 时主线程连打卡死
    unsigned shift = static_cast<unsigned>(n - 1);
    if (shift > 3) shift = 3;
    DWORD backoff = 1000u << shift;
    if (backoff > 10000u) backoff = 10000u;
    gFailBackoffUntilMs.store(now + backoff);
    if (!gLastFailLogMs || static_cast<DWORD>(now - gLastFailLogMs) >= 2000) {
        gLastFailLogMs = now;
        x::runtime::LogW("AttackRpc", "melee fail err=%s backoff=%lums fails=%d",
                         err ? err : "?", static_cast<unsigned long>(backoff), n);
    }
}

bool TryFireNormal(FireResult* out) {
    FireResult local{};
    FireResult* r = out ? out : &local;
    r->ok = false;
    r->mobs = 0;
    r->bodyHint = 0;
    r->err = "pending";

    if (!gEnabled.load()) {
        r->err = "disabled";
        return false;
    }

    FireJob job{};
    job.out = r;
    if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, kJobWaitMs)) {
        r->err = "main_timeout";
        NoteFireResult(false, r->err, 0, 0, false);
        return false;
    }
    CopyJobToResult(job, r);
    NoteFireResult(job.ok, job.err, job.mobs, job.bodyHint, false);
    return job.ok;
}

bool TryFireMelee(FireResult* out) {
    // 兼容旧名：现网探针 = NormalAttack（Create 50 + skill 0）。
    return TryFireNormal(out);
}

bool TryFireNormalDirect(FireResult* out) {
    // 仅实验/直调入口；多发已切断。跳过 fail backoff 以免探针连点被卡住。
    FireResult local{};
    FireResult* r = out ? out : &local;
    r->ok = false;
    r->mobs = 0;
    r->bodyHint = 0;
    r->err = "pending";

    gFailBackoffUntilMs.store(0);

    FireJob job{};
    job.out = r;
    job.allowDirect = true;
    if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, kJobWaitMs)) {
        r->err = "main_timeout";
        NoteFireResult(false, r->err, 0, 0, true);
        return false;
    }
    CopyJobToResult(job, r);
    NoteFireResult(job.ok, job.err, job.mobs, job.bodyHint, true);
    return job.ok;
}

bool TryFireOneshot(FireResult* out) {
    FireResult local{};
    FireResult* r = out ? out : &local;
    r->ok = false;
    r->mobs = 0;
    r->bodyHint = 0;
    r->err = "pending";

    gFailBackoffUntilMs.store(0);

    FireJob job{};
    job.out = r;
    job.allowDirect = true;
    job.oneshot = true;
    if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, kJobWaitMs)) {
        r->err = "main_timeout";
        NoteFireResult(false, r->err, 0, 0, true);
        return false;
    }
    CopyJobToResult(job, r);
    NoteFireResult(job.ok, job.err, job.mobs, job.bodyHint, true);
    return job.ok;
}

bool TryFireLockOid(int32_t oid, FireResult* out) {
    FireResult local{};
    FireResult* r = out ? out : &local;
    r->ok = false;
    r->mobs = 0;
    r->bodyHint = 0;
    r->err = "pending";
    if (oid <= 0) {
        r->err = "bad_oid";
        return false;
    }
    gFailBackoffUntilMs.store(0);
    FireJob job{};
    job.out = r;
    job.allowDirect = true;
    job.lockOid = oid;
    if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, kJobWaitMs)) {
        r->err = "main_timeout";
        NoteFireResult(false, r->err, 0, 0, true);
        return false;
    }
    CopyJobToResult(job, r);
    NoteFireResult(job.ok, job.err, job.mobs, job.bodyHint, true);
    return job.ok;
}

uint32_t PeekStopGen() { return gStopGen.load(); }

void Tick() {
    if (gPendingReset.load()) (void)ResetSessionCap("queued");
    if (!gEnabled.load()) return;
    const DWORD now = GetTickCount();
    const DWORD until = gFailBackoffUntilMs.load();
    if (until && static_cast<int32_t>(now - until) < 0) return;
    const DWORD iv = gIntervalMs.load();
    if (gLastFireMs && static_cast<DWORD>(now - gLastFireMs) < iv) return;
    gLastFireMs = now;
    TryFireNormal(nullptr);
}

}  // namespace x::features::ports::attack_rpc
