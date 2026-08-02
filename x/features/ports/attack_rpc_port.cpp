// attack_rpc_port — forge ClientPacket 50 (Melee) via official Encode + SendOutPacket.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_rpc_port.h"

#include "mob_pool_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
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
    "aeb7167893ac51cbc0cf730326f2361e6e8b797eeb940786711185ef0fd658c";

// P0b / P0c RVAs (imagebase 0x7FFB74A20000 era)
constexpr uint32_t kRvaOutPacketCreate = 0x1CB7BB0;
constexpr uint32_t kRvaEncode1Byte = 0x1CC4110;
constexpr uint32_t kRvaEncode1Bool = 0x1CC41F0;
constexpr uint32_t kRvaEncode2Ushort = 0x1CC43F0;
constexpr uint32_t kRvaEncode2Short = 0x1CC4370;
constexpr uint32_t kRvaEncode4Int = 0x1CC4480;
constexpr uint32_t kRvaEncode4Uint = 0x1CC4500;
constexpr uint32_t kRvaEncodeVector2 = 0x1CC5090;
// SendPacket this = Session*（从 facade +0x10 取出），不是壳本身。
constexpr uint32_t kRvaNmSendPacket = 0x1CB98B0;
constexpr uint32_t kRvaSendOutPacket = 0x1CB7CE0;  // 文档锚点；本探针暂不直调

// Create(50)=NormalAttack / MeleeAttack 同 opcode；本探针按 TryDoingNormalAttack（skill=0）。
constexpr int kOpcodeNormalAttack = 50;
// Facade：Session*@0x10、SessionState@0x18。Session：state@0x60。
constexpr size_t kOffNmSession = 0x10;
constexpr size_t kOffNmSessionState = 0x18;
constexpr size_t kOffSessionState = 0x60;
constexpr int kSessionStateConnected = 3;
constexpr size_t kOffMobPos = 0x64;  // FieldActorBase Pos (Vector2)

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
using FnNmSend = bool (*)(void* self, void* packet, const void* method);
using FnFindAll = void* (*)(void* type, void* method);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
void* gFacadeKlass = nullptr;
void* gFacadeType = nullptr;
void* gSessionKlass = nullptr;  // SendPacket MethodInfo 宿主
void* gNm = nullptr;            // Session*（发包 this）
void* gOutPacketKlass = nullptr;

MethodInfoHead* gMiCreate = nullptr;
MethodInfoHead* gMiE1 = nullptr;
MethodInfoHead* gMiE1Bool = nullptr;
MethodInfoHead* gMiE2U = nullptr;
MethodInfoHead* gMiE2S = nullptr;
MethodInfoHead* gMiE4 = nullptr;
MethodInfoHead* gMiE4U = nullptr;
MethodInfoHead* gMiEV2 = nullptr;
MethodInfoHead* gMiSend = nullptr;

std::atomic<bool> gEnabled{false};
std::atomic<int> gMaxMobs{1};
std::atomic<DWORD> gIntervalMs{500};
std::atomic<int> gDamage{1};
std::atomic<int> gSkillId{0};
std::atomic<bool> gReady{false};
std::atomic<DWORD> gFailBackoffUntilMs{0};
std::atomic<int> gConsecutiveFails{0};
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
    if (gSessionKlass) {
        void* k = ReadPtr(cand, 0);
        if (k != gSessionKlass) return false;
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
        __try {
            e.runtimeClassInit(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
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
    if (gSessionKlass && ReadPtr(sess, 0) != gSessionKlass) return nullptr;
    return sess;
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

    if (gNm && !LooksLikeSession(gNm)) gNm = nullptr;
    if (!gNm) {
        void* facade = ResolveFacadeSingleton(gFacadeKlass);
        if (!facade && gFacadeType && gFindAll) {
            void* arr = nullptr;
            __try {
                arr = gFindAll(gFacadeType, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                arr = nullptr;
            }
            const int n = LooksLikeHeapPtr(arr)
                              ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                                    reinterpret_cast<uint8_t*>(arr) + 0x18))
                              : 0;
            for (int i = 0; i < n && i < 8; ++i) {
                void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                                    static_cast<size_t>(i) * sizeof(void*));
                if (!LooksLikeFacade(o)) continue;
                facade = o;
                break;
            }
        }
        gNm = SessionFromFacade(facade);
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
    if (gSessionKlass && !gMiSend) {
        // bool(OutPacket) — paramKlass 钉死 OutPacket，避免 Session 上多个 bool(ptr)。
        x::runtime::il2cpp_method::MethodShape kSend{};
        kSend.arity = 1;
        kSend.ret = x::runtime::il2cpp_method::TypeKind::Bool;
        kSend.unique = true;
        kSend.walkParents = true;
        kSend.param[0] = x::runtime::il2cpp_method::TypeKind::Ptr;
        if (gOutPacketKlass) kSend.paramKlass[0] = gOutPacketKlass;
        const auto mr =
            x::runtime::il2cpp_method::FindMethodCached(gSessionKlass, kRvaNmSendPacket, kSend);
        gMiSend = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("AttackRpc", "Session.SendPacket MethodInfo via kind");
        }
    }

    const bool ok = gGA && gOutPacketKlass && gMiCreate && gMiE1 && gMiE4 && gMiEV2 &&
                    AtRva<void*>(kRvaNmSendPacket);
    gReady.store(ok);
    return ok;
}

struct FireJob {
    FireResult* out = nullptr;
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
    auto* job = reinterpret_cast<FireJob*>(user);
    if (!job) return;
    job->err = "job";

    if (!gEnabled.load()) {
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
            void* facade = ResolveFacadeSingleton(gFacadeKlass);
            if (!facade && gFacadeType && gFindAll) {
                void* arr = nullptr;
                __try {
                    arr = gFindAll(gFacadeType, nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    arr = nullptr;
                }
                const int n = LooksLikeHeapPtr(arr)
                                  ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                                        reinterpret_cast<uint8_t*>(arr) + 0x18))
                                  : 0;
                for (int i = 0; i < n && i < 8; ++i) {
                    void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                                        static_cast<size_t>(i) * sizeof(void*));
                    if (!LooksLikeFacade(o)) continue;
                    facade = o;
                    break;
                }
            }
            gNm = SessionFromFacade(facade);
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
            if (gFacadeType && gFindAll) {
                void* arr = nullptr;
                __try {
                    arr = gFindAll(gFacadeType, nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    arr = nullptr;
                }
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
    for (int i = 0; i < snap.count && nHit < want && nHit < kMaxMobsHard; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0) continue;
        if (m.hpPct <= 0) continue;
        // Prefer our-ctrl; still allow others for probe if none yet.
        if (m.ctrl <= 0 && nHit > 0) continue;
        hits[nHit].oid = m.id;
        hits[nHit].x = m.x;
        hits[nHit].y = m.y;
        if (LooksLikeHeapPtr(m.ptr)) {
            const float px = ReadF32(m.ptr, kOffMobPos);
            const float py = ReadF32(m.ptr, kOffMobPos + 4);
            if (std::fabs(px) > 0.5f || std::fabs(py) > 0.5f) {
                hits[nHit].x = px;
                hits[nHit].y = py;
            }
        }
        ++nHit;
    }
    if (nHit <= 0) {
        job->err = "no_targets";
        return;
    }

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

    // Header：对齐 send.log.5/7 真包（portal 恒 0x01，不是 0x03）
    // 例 off=55：01 11 | skill0×2 | bool0 | action | 01 05 | tOrKey | 命中环 | 玩家i16
    const uint8_t flags =
        static_cast<uint8_t>((1u & 0xFu) | (static_cast<uint32_t>(nHit) << 4));
    Encode1(pkt, 1);  // portal：真包一律 0x01（上一轮误写成 3 → 服端直接丢）
    Encode1(pkt, flags);
    Encode4(pkt, kSkillId);
    Encode4(pkt, kSkillExtra);
    Encode1Bool(pkt, false);

    bool faceLeft = false;
    if (nHit > 0) faceLeft = (hits[0].x < ctx.x);
    uint16_t action = 5;
    if (faceLeft) action = static_cast<uint16_t>(action | 0x8000u);
    Encode2U(pkt, action);
    Encode1(pkt, 1);  // UserLocal+0x158
    Encode1(pkt, 5);  // 动作/武器字节
    // tOrKey：真包为游戏钟（~1e5 量级）。GetTickCount 在进程刚启动时量级接近，先沿用；
    // 若仍无伤再改为读 UserLocal/Security 同钟。
    Encode4(pkt, static_cast<int32_t>(GetTickCount()));

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

    // 命中环：真包在两 XY 之后是 u16（A5 01 非暴 / 47 01 偏暴）+ dmg i32 + field u32(=0)
    // 再接玩家 XY；无额外 Encode1 尾巴（对照 off=55 hex）。
    int body = 19;
    for (int i = 0; i < nHit; ++i) {
        Encode4(pkt, hits[i].oid);
        Encode1(pkt, 6);  // HitAction
        Encode1(pkt, 0);  // ForeAction：真包多数 0x00（0x80 也有，先走多数）
        Encode1(pkt, 1);  // FrameIdx
        Encode1(pkt, 1);  // facing/state
        encXY(hits[i].x, hits[i].y);
        encXY(hits[i].x + (faceLeft ? -1.f : 1.f), hits[i].y);
        Encode2U(pkt, 0x01A5);
        Encode4(pkt, dmg);
        Encode4U(pkt, 0);
        body += 30;
    }
    encXY(ctx.x, ctx.y);
    body += 4;

    int off = 0;
    const uint8_t* data = nullptr;
    __try {
        off = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pkt) + 0x18);
        data = *reinterpret_cast<const uint8_t**>(reinterpret_cast<uint8_t*>(pkt) + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        off = 0;
        data = nullptr;
    }
    if (off > 0) body = off;

    // 打 BODY hex（前 5 次 + 之后每 5s 一条），对真包；不依赖 KICK_SEND_DUMP 配额
    static int sHexLogs = 0;
    static DWORD sLastHexMs = 0;
    const DWORD nowHex = GetTickCount();
    const bool wantHex =
        data && off >= 6 && off <= 256 &&
        (sHexLogs < 5 || static_cast<DWORD>(nowHex - sLastHexMs) >= 5000);
    if (wantHex) {
        char hex[512];
        int hp = 0;
        const int n = off < 64 ? off : 64;
        for (int i = 0; i < n && hp + 4 < (int)sizeof(hex); ++i) {
            hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X", data[i]);
            if (i + 1 < n) hp += snprintf(hex + hp, sizeof(hex) - hp, " ");
        }
        x::runtime::LogI("AttackRpc", "forge BODY off=%d hex=%s%s", off, hex,
                         off > 64 ? " ..." : "");
        ++sHexLogs;
        sLastHexMs = nowHex;
    }

    auto* send = reinterpret_cast<FnNmSend>(AtRva<void*>(kRvaNmSendPacket));
    if (!send) {
        job->err = "no_send";
        return;
    }
    bool sent = false;
    __try {
        sent = send(gNm, pkt, gMiSend);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->err = "send_seh";
        return;
    }
    if (!sent) {
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
    x::runtime::LogI("AttackRpc", "port init (default OFF) create@0x%X nmSend@0x%X",
                     kRvaOutPacketCreate, kRvaNmSendPacket);
}

void Shutdown() {
    gEnabled.store(false);
    gReady.store(false);
}

bool EnsureBound() { return Rebind(GetTickCount()); }

bool Ready() { return gReady.load() || EnsureBound(); }

void SetEnabled(bool on) {
    const bool prev = gEnabled.exchange(on);
    if (prev == on) return;
    if (!on) {
        gConsecutiveFails.store(0);
        gFailBackoffUntilMs.store(0);
    }
    x::runtime::LogI("AttackRpc", "enabled=%d (wire dmg is placeholder; server recalcs)",
                     on ? 1 : 0);
}

bool IsEnabled() { return gEnabled.load(); }

void SetMaxMobs(int n) {
    if (n < 1) n = 1;
    if (n > kMaxMobsHard) n = kMaxMobsHard;
    gMaxMobs.store(n);
}

int GetMaxMobs() { return gMaxMobs.load(); }

void SetIntervalMs(DWORD ms) {
    if (ms < 50) ms = 50;
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

void NoteFireResult(bool ok, const char* err, int mobs, int bodyHint) {
    const DWORD now = GetTickCount();
    if (ok) {
        gConsecutiveFails.store(0);
        gFailBackoffUntilMs.store(0);
        x::runtime::LogI("AttackRpc", "normal ok mobs=%d body~%d dmg=%d skill=0", mobs, bodyHint,
                         gDamage.load());
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
        NoteFireResult(false, r->err, 0, 0);
        return false;
    }
    r->ok = job.ok;
    r->mobs = job.mobs;
    r->bodyHint = job.bodyHint;
    r->err = job.err;
    NoteFireResult(job.ok, job.err, job.mobs, job.bodyHint);
    return job.ok;
}

bool TryFireMelee(FireResult* out) {
    // 兼容旧名：现网探针 = NormalAttack（Create 50 + skill 0）。
    return TryFireNormal(out);
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
