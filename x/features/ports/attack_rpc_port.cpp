// attack_rpc_port — forge ClientPacket 50 (Melee) via official Encode + SendOutPacket.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_rpc_port.h"

#include "mob_pool_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x::features::ports::attack_rpc {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Session 方法宿主 → ResolveNetworkManagerKlass；单例壳 → ResolveNetworkManagerFacadeKlass
constexpr char kOutPacketClass[] =
    "f07686cc7a01760c9166b2cf7a72f4ac7c084f1ee39bd1c3bdc42c351e884bb";

// P0b / P0c RVAs — remounted 2026-08-04
constexpr uint32_t kRvaOutPacketCreate = 0x1CC22D0;
constexpr uint32_t kRvaEncode1Byte = 0x1CCE9E0;
constexpr uint32_t kRvaEncode1Bool = 0x1CCEAC0;
constexpr uint32_t kRvaEncode2Ushort = 0x1CCEC50;
constexpr uint32_t kRvaEncode2Short = 0x1CCEBD0;
constexpr uint32_t kRvaEncode4Int = 0x1CCECE0;
constexpr uint32_t kRvaEncode4Uint = 0x1CCED60;
constexpr uint32_t kRvaEncodeVector2 = 0x1CCF8B0;
// 官方门：Network_SendOutPacket（this=facade）
//   → HashSet<ushort>.Contains(opcode@pkt+0x20) → Session.SendPacket（[facade+0x10]）
// 2026-08-03 BIN：直调 Session.SendPacket 旁路 HashSet → 第 3 次 forge 后 ~109ms Disconnected
// （与 sellbag「错包+Session.Send → 105ms」同型；KickSniff verdict=lean_local_or_soft）。
constexpr uint32_t kRvaNmSendPacket = 0x1CC3EE0;  // SendOutPacket 内部落点；勿直调
constexpr uint32_t kRvaSendOutPacket = 0x1CC23D0;  // remounted 2026-08-04；旧 0x1CB7CE0
constexpr uint32_t kRvaWorldManagerGetUpdateTime = 0xDC2010;  // remounted 2026-08-04 (int)(_updateTime*1000)
// 延后踢对策：对齐 TryDoingNormalAttack 发包前最小本地态
//   SetAttackAction(lu, action, aux, skill=null, 0) @0xFDAF10
//   CollectAttackPacket(50) @0x3C500A0（Tap 旁路；SendOut 静态无直调）
constexpr uint32_t kRvaSetAttackAction = 0xFDAF10;  // remounted 2026-08-04；旧 0xFD39C0
constexpr uint32_t kRvaCollectAttackPacket = 0x3C500A0;  // remounted 2026-08-04；旧 0x3C44C10
// 探针安全：单次勾选最多成功发包；5 刀后再开第 6 刀仍 ~0.9s 踢 → 先收到 2。
// 每段开启最多 2 刀；进程内累计再硬封顶（BIN：重开 UI 清零后第 6 刀 ~111ms 静默断线）。
constexpr int kAutoStopAfterOk = 2;
constexpr int kSessionForgeCap = 4;

// Create(50)=NormalAttack / MeleeAttack 同 opcode；本探针按 TryDoingNormalAttack（skill=0）。
constexpr int kOpcodeNormalAttack = 50;
// Facade/Session → il2cpp_network SSOT
#define kOffNmSession (x::runtime::il2cpp_network::OffNmSession())
#define kOffNmSessionState (x::runtime::il2cpp_network::OffNmSessionState())
#define kOffNmOpcodeHashSet (x::runtime::il2cpp_network::OffNmOpcodeHashSet())
#define kOffSessionState (x::runtime::il2cpp_network::OffSessionState())
constexpr int kSessionStateConnected = 3;
constexpr size_t kOffMobPos = 0x64;  // FieldActorBase Pos (Vector2)
constexpr size_t kOffUserLocalPad158 = 0x158;  // Encode1 字节源
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

std::atomic<bool> gEnabled{false};
std::atomic<int> gMaxMobs{1};
std::atomic<DWORD> gIntervalMs{500};
std::atomic<int> gDamage{1};
std::atomic<int> gSkillId{0};
std::atomic<bool> gReady{false};
std::atomic<DWORD> gFailBackoffUntilMs{0};
std::atomic<int> gConsecutiveFails{0};
std::atomic<int> gOkSinceEnable{0};   // 本段开启计数（SetEnabled(true) 清零）
std::atomic<int> gOkSession{0};       // 进程内累计成功伪造（不清零）
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

int GetGameUpdateTimeMs() {
    // 与 skill_port 同钟：WorldManager.GetUpdateTime / static _updateTime*1000。
    if (!gGetUpdateTime && gGA) {
        gGetUpdateTime = AtRva<FnGetUpdateTime>(kRvaWorldManagerGetUpdateTime);
    }
    if (!gWorldManagerKlass) {
        gWorldManagerKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    }
    if (!gMiGetUpdateTime && gWorldManagerKlass) {
        gMiGetUpdateTime = FindMethodByRva(gWorldManagerKlass, kRvaWorldManagerGetUpdateTime);
    }
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

// TryDoingNormalAttack 在 Create 前：SetAttackAction(lu, action, aux, null, 0)；
// Collect 在出站 Tap（SendOut 不直调）——探针显式补一刀，避免本地窗与出站脱节。
// 失败只打日志，不阻断 forge（避免 SetAttackAction 参数漂导致探针哑火）。
void TryLocalAttackPrereq(void* localUser, int32_t actionId) {
    if (!gGA) return;
    if (LooksLikeHeapPtr(localUser)) {
        auto* setAct = AtRva<FnSetAttackAction>(kRvaSetAttackAction);
        if (setAct) {
            bool ok = false;
            __try {
                // IDA 实锤：NormalAttack 位点 skill=null、第 5 参解混淆=0；
                // action/aux 取自栈槽；探针用 Encode 同款 action 低字节 + aux=0。
                ok = setAct(localUser, actionId, 0, nullptr, 0, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            x::runtime::LogI("AttackRpc", "SetAttackAction action=%d ok=%d", actionId, ok ? 1 : 0);
        }
    }
    auto* collect = AtRva<FnCollectAttackPacket>(kRvaCollectAttackPacket);
    if (collect) {
        __try {
            collect(static_cast<uint16_t>(kOpcodeNormalAttack), nullptr);
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
    if (x::runtime::main_thread::IsOnPumpThread()) {
        void* arr = nullptr;
        __try {
            arr = gFindAll(typeObj, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr = nullptr;
        }
        return arr;
    }
    if (x::runtime::managed_main::IsLoginFrozen()) return nullptr;
    return x::runtime::managed_main::FindAll(gFindAll, typeObj, 2000, false);
}

bool Rebind(DWORD now) {
    if (now - gLastRebindMs < kRebindMs && gReady.load()) return true;
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
    if (!gGetUpdateTime) gGetUpdateTime = AtRva<FnGetUpdateTime>(kRvaWorldManagerGetUpdateTime);
    if (!gMiGetUpdateTime && gWorldManagerKlass) {
        gMiGetUpdateTime = FindMethodByRva(gWorldManagerKlass, kRvaWorldManagerGetUpdateTime);
    }

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
        if (!gMiCreate) gMiCreate = FindMethodByRva(gOutPacketKlass, kRvaOutPacketCreate);
        if (!gMiE1) gMiE1 = FindMethodByRva(gOutPacketKlass, kRvaEncode1Byte);
        if (!gMiE1Bool) gMiE1Bool = FindMethodByRva(gOutPacketKlass, kRvaEncode1Bool);
        if (!gMiE2U) gMiE2U = FindMethodByRva(gOutPacketKlass, kRvaEncode2Ushort);
        if (!gMiE2S) gMiE2S = FindMethodByRva(gOutPacketKlass, kRvaEncode2Short);
        if (!gMiE4) gMiE4 = FindMethodByRva(gOutPacketKlass, kRvaEncode4Int);
        if (!gMiE4U) gMiE4U = FindMethodByRva(gOutPacketKlass, kRvaEncode4Uint);
        if (!gMiEV2) gMiEV2 = FindMethodByRva(gOutPacketKlass, kRvaEncodeVector2);
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
        const auto mr =
            x::runtime::il2cpp_method::FindMethodCached(gFacadeKlass, kRvaSendOutPacket, kSend);
        gMiSendOut = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (!gMiSendOut) gMiSendOut = FindMethodByRva(gFacadeKlass, kRvaSendOutPacket);
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
    bool allowDirect = false;  // 多发直发：跳过 gEnabled
    bool ok = false;
    const char* err = "init";
    int mobs = 0;
    int bodyHint = 0;
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

    const int want = gMaxMobs.load();
    const int dmg = gDamage.load();
    // NormalAttack：P0c 落空/普攻头 skill 两枚均为 0；不读 gSkillId。
    constexpr int kSkillId = 0;
    constexpr int kSkillExtra = 0;
    struct Hit {
        int32_t oid = 0;
        float x = 0.f;
        float y = 0.f;
    };
    Hit hits[kMaxMobsHard]{};
    int nHit = 0;
    // 候选：优先近距 + 我控。上一轮 BIN 打到 ~945px 外怪 → 服端吞伤不掉血。
    constexpr float kMeleeMaxDist = 180.f;
    struct Cand {
        int32_t oid = 0;
        float x = 0.f;
        float y = 0.f;
        float dist = 0.f;
        int32_t ctrl = 0;
    };
    Cand cands[kMaxMobsHard]{};
    int nCand = 0;
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
        if (dist > kMeleeMaxDist) continue;
        cands[nCand].oid = m.id;
        cands[nCand].x = mx;
        cands[nCand].y = my;
        cands[nCand].dist = dist;
        cands[nCand].ctrl = m.ctrl;
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
        ++nHit;
    }
    if (nHit <= 0) {
        job->err = "no_near_targets";  // 旁无近距怪（>180px 不打，避免远距吞伤）
        static DWORD sLastFarLog = 0;
        const DWORD t = GetTickCount();
        if (!sLastFarLog || static_cast<DWORD>(t - sLastFarLog) >= 2000) {
            sLastFarLog = t;
            x::runtime::LogW("AttackRpc",
                             "no near mob (need dist<=%.0f); player=(%.0f,%.0f) snap=%d",
                             kMeleeMaxDist, ctx.x, ctx.y, snap.count);
        }
        return;
    }

    bool faceLeft = false;
    if (nHit > 0) faceLeft = (hits[0].x < ctx.x);
    uint16_t action = 5;  // 真包常见 5..0x10；探针固定普攻动作 5
    if (faceLeft) action = static_cast<uint16_t>(action | 0x8000u);

    // 对齐正路：Create 前先 SetAttackAction + CollectAttackPacket(50)
    TryLocalAttackPrereq(ctx.localUser, static_cast<int32_t>(action & 0x7FFF));

    auto* create = reinterpret_cast<FnOutCreate>(
        gMiCreate && gMiCreate->methodPointer ? gMiCreate->methodPointer
                                              : AtRva<void*>(kRvaOutPacketCreate));
    if (!create) {
        job->err = "no_create";
        return;
    }

    void* pkt = nullptr;
    __try {
        pkt = create(kOpcodeNormalAttack, gMiCreate);
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
    Encode4(pkt, kSkillId);
    Encode4(pkt, kSkillExtra);
    Encode1Bool(pkt, false);

    Encode2U(pkt, action);
    uint8_t pad158 = 1;
    if (LooksLikeHeapPtr(ctx.localUser)) {
        const uint8_t v = ReadU8(ctx.localUser, kOffUserLocalPad158);
        // 真包多数 0x01；读到合理字节则用，否则回退 1
        if (v <= 0x3F) pad158 = v ? v : 1;
    }
    Encode1(pkt, pad158);
    Encode1(pkt, 5);  // 动作/武器字节（真包恒见 05）
    // tOrKey：必须用游戏钟 GetUpdateTime（~1e4–1e5）；GetTickCount 量级不对服端会吞伤
    int tOrKey = GetGameUpdateTimeMs();
    if (tOrKey <= 0) tOrKey = static_cast<int>(GetTickCount() & 0x7FFFFFFF);
    Encode4(pkt, tOrKey);

    auto encXY = [&](float fx, float fy) {
        // EncodeVector2 在线上只 BlockCopy 2B/分量（=i16）；直接 Encode2 等价且省一次调用。
        int x = static_cast<int>(fx);
        int y = static_cast<int>(fy);
        if (x < -32768) x = -32768;
        if (x > 32767) x = 32767;
        if (y < -32768) y = -32768;
        if (y > 32767) y = 32767;
        Encode2S(pkt, static_cast<int16_t>(x));
        Encode2S(pkt, static_cast<int16_t>(y));
    };

    // 命中环：真包两 XY 后是 u16（A5 01 非暴 / 47 01 偏暴）+ dmg i32 + field u32(=0)
    // 再接玩家 XY。P0c「Encode1 AttackCount」在现网 wire 上是 u16 占位，以 send.log 为准。
    int body = 19;
    for (int i = 0; i < nHit; ++i) {
        Encode4(pkt, hits[i].oid);
        Encode1(pkt, 6);     // HitAction（真包常见 06）
        Encode1(pkt, 0x81);  // ForeAction：真包多数 0x81（偶见 0x80）；旧探针 0x80
        Encode1(pkt, 0);     // FrameIdx：真包常见 0（偶见 1）
        Encode1(pkt, 1);     // facing/state：真包多数 01（与 action 朝向位独立）
        encXY(hits[i].x, hits[i].y);
        // 真包两 XY 多数相同（偶有 ±1）；跟多数，不人为偏移
        encXY(hits[i].x, hits[i].y);
        Encode2U(pkt, 0x01A5);
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
                         "forge BODY off=%d mobs=%d tOrKey=%d action=0x%04X "
                         "oid=%d dist=%.0f player=(%.0f,%.0f) mob=(%.0f,%.0f) hex=%s%s",
                         off, nHit, tOrKey, static_cast<unsigned>(action), hits[0].oid,
                         std::sqrt((hits[0].x - ctx.x) * (hits[0].x - ctx.x) +
                                   (hits[0].y - ctx.y) * (hits[0].y - ctx.y)),
                         ctx.x, ctx.y, hits[0].x, hits[0].y, hex,
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
        job->err = "send_false";
        return;
    }

    job->ok = true;
    job->err = "ok";
    job->mobs = nHit;
    job->bodyHint = body;
}

}  // namespace

void Init() {
    gEnabled.store(false);
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

void SetEnabled(bool on) {
    if (on) {
        const int sess = gOkSession.load();
        if (sess >= kSessionForgeCap) {
            gEnabled.store(false);
            x::runtime::LogW("AttackRpc",
                             "enable refused: session forge cap %d/%d (kick-safety; restart DLL)",
                             sess, kSessionForgeCap);
            return;
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
    // NormalAttack 固定 skill=0；保留 API 以免旧调用方断链。
    gSkillId.store(0);
}

int GetSkillId() { return 0; }

void NoteFireResult(bool ok, const char* err, int mobs, int bodyHint, bool fromDirectMulti) {
    const DWORD now = GetTickCount();
    if (ok) {
        gConsecutiveFails.store(0);
        gFailBackoffUntilMs.store(0);
        const int nOk = gOkSinceEnable.fetch_add(1) + 1;
        const int nSess = gOkSession.fetch_add(1) + 1;
        x::runtime::LogI("AttackRpc",
                         "normal ok mobs=%d body~%d dmg=%d skill=0 ok#%d session#%d direct=%d", mobs,
                         bodyHint, gDamage.load(), nOk, nSess, fromDirectMulti ? 1 : 0);
        // 实验 TAB 探针才 auto_stop；多发直发不关 gEnabled。
        if (!fromDirectMulti && gEnabled.load() &&
            (nOk >= kAutoStopAfterOk || nSess >= kSessionForgeCap)) {
            gEnabled.store(false);
            if (nSess >= kSessionForgeCap) {
                x::runtime::LogW("AttackRpc",
                                 "session_stop after %d ok (cap=%d; restart DLL to reset)", nSess,
                                 kSessionForgeCap);
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
    r->ok = job.ok;
    r->mobs = job.mobs;
    r->bodyHint = job.bodyHint;
    r->err = job.err;
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
    r->ok = job.ok;
    r->mobs = job.mobs;
    r->bodyHint = job.bodyHint;
    r->err = job.err;
    NoteFireResult(job.ok, job.err, job.mobs, job.bodyHint, true);
    return job.ok;
}

void Tick() {
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
