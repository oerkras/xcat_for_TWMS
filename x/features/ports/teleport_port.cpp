// Classic TWMS — 唯一产品瞬移：手填 Teleport pending + TryDoingTeleport（fill+Doing）
// 不含 ImpactNext / Attr=4 / SyncRel settle / Register 技能包。
//
// 只填 pending、只让引擎自己落点（逆自 GameAssembly 运行时 dump）：
//   UserLocal+0x3C8 = { byte flag; float x@+4; float y@+8; int lastTime@+0xC }
//   TryDoingTeleport（RVA 0x1026060）在 UserLocal Update 链（RVA 0x1016150）里每帧被调，
//   flag 置位即把 (int)x,(int)y 交给 VecCtrl 落点体（RVA 0x11A8460）后清 flag。
//   落点体全部行为：CurFh/LastFh 置 null；Ap/Apl ← (int)x,y；Ap.V/Apl.V 归零；
//   （vc+0x80 置位时）MovePath_MakeMovePath 上报。不写 RelPos、不挂踏板。
// 结论：踏板重挂是引擎 CollisionDetect 的职责。我方禁止在 Doing 前后补种 CurFh/RelPos
// ——那会让下一帧 AbsPos←f(我方台, 我方弧长) 覆盖引擎结论，即 land_miss 的根因。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "teleport_port.h"

#include "action_gate.h"
#include "attack_input_port.h"
#include "foothold_path.h"
#include "foothold_port.h"
#include "map_bounds_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace x::features::ports::teleport {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaTryDoingTeleport = 0x1026060;  // remounted 2026-08-04
constexpr uint32_t kRvaVecCtrlSetForcedFlush = 0x11A8700;  // remounted 2026-08-04
constexpr uint32_t kRvaMovePathSetForcedFlush = 0x119F320;  // remounted 2026-08-04
constexpr char kHashTryDoingTeleport[] =
    "e877dce51c9e10d99e95d90bc35c5fe1694870f4063b551ff71ca01c0cabf8b";
constexpr char kHashVecCtrlSetForcedFlush[] =
    "c9db3f0b899c94ef3c0e678fbab1708142e77ec86734b4a971a4fbe8ed8b806";
constexpr char kHashMovePathSetForcedFlush[] =
    "a453a7117248fc39bbdfe973b59ccf35203b4ca27f44234c1a3c5f4387d2200";
constexpr char kVecCtrlClass[] =
    "ef24024acbe225bcc90ca332f3e00aff5800daa32a769057d2e830eeac776bb";
constexpr char kMovePathClass[] =
    "e94d0baf8d17ca3e5ccea1f3065f762ec520bdaaf9106e8ef47ce7d3ce68297";
constexpr char kActorBaseClass[] =
    "ddef6db860cfa2bea6dca39e201bf3065a897797f86009fb4d6104830143d94";
constexpr char kFhClass[] =
    "f7493895c6355227ba46ff22f0b3d491fac47e4c4ad2e735773a72878d9f860";
// True TW UserLocal = User subclass with Teleport@0x3C8（resolve: il2cpp_shape）

// hash → field_get_offset（与 foothold / attack / player_combat 同源）
constexpr char kHashUserVecCtrl[] =
    "<dc76f5c9e250bc9a327a219b39e16c345cdabf7b01ad5c60b568045069c9120>k__BackingField";
constexpr char kHashVcCurFh[] =
    "<b7b98b20290b6ec6221cc7a98ad9113018910968cd1f681f60fe20f109ef629>k__BackingField";
constexpr char kHashVcLastFh[] =
    "<c97b850dd190245b9825d1e6892847b5ed3ecdd710e9bd1c08fc8202909be86>k__BackingField";
constexpr char kHashVcMovePath[] =
    "<dfec47f2fe1c0e901374ce4c4aec6f184ddac664e70eddd01f13c87cc697dc2>k__BackingField";
constexpr char kHashVcRelPos[] =
    "b43a7d8d34af59258f63b62c55068f9d50d59aa86f239fd75d9c5f7d8f008e7";  // RelPos; V=+8
constexpr char kHashVcAp[] =
    "a860e652f11e3e8846eaf4dfb600e319058d3e0e9e79b3fd7a3447344d98bb9";  // AbsPos; Y=+8
constexpr char kHashVcApl[] =
    "ddcaef33563d49269da8f9db8391866dfc59ec057b8cca4ffa15a5b38f271b3";  // Apl; Y=+8
constexpr char kHashVcMoveAction[] =
    "afdef055a699e27cb4575fce73d95752cd4571320e9c13b0c0322e96a023c3a";
constexpr char kHashMpForcedFlush[] =
    "e76fd0429b4190c5e8022e4e22b97dfea8c254efd7b8e83d8cf3b13780396b8";
constexpr char kHashMpX[] =
    "b4c1330413ec6a87812b47472c538a303af55e0eb72fa9ba50da034bda2d7a4";
constexpr char kHashMpY[] =
    "ac214c6c09f7977df3ee20b103e35bd87fd5aa82591dace047981e8f136fd2c";
constexpr char kHashFhX1[] =
    "<b4b17e3fe55cee84a1cc309d4c6f7cb8f6ba1132cb7b0e3fe2187515f112799>k__BackingField";
constexpr char kHashFhY1[] =
    "<aea9f00b3be599c5d92303bbcce95fefce71cdecb0030dc3e248807b37221e9>k__BackingField";
constexpr char kHashFhX2[] =
    "<be62b8cab6ec790bc8e45378aea447a5e777169435b71c8cdeaf971d16b69ab>k__BackingField";
constexpr char kHashFhY2[] =
    "<e47137381c7954f12ba8667e57b0df9a37338ba57e2f98cd0a4725f2b6e1cb8>k__BackingField";
// UserLocal.Teleport valuetype block（嵌套相对：IsValid+0 / ByPortal+1 / Pos+4 / ticks+0xC/+0x10）
constexpr char kHashTeleport[] =
    "f068fbf6557e1bea671fa19a5142e0053d1e93ab847738acfdf746c2e15189a";

constexpr size_t kFbVecCtrl = 0x50, kFbVcCurFh = 0x28, kFbVcLastFh = 0x30, kFbVcMovePath = 0x78;
constexpr size_t kFbVcRelPos = 0x88, kFbVcAp = 0x98, kFbVcApl = 0xB8, kFbVcMoveAction = 0x84;
constexpr size_t kFbMpForcedFlush = 0x48, kFbMpX = 0x10, kFbMpY = 0x12;
constexpr size_t kFbFhX1 = 0x14, kFbFhY1 = 0x18, kFbFhX2 = 0x1C, kFbFhY2 = 0x20;
constexpr size_t kFbTeleport = 0x3C8;

size_t gOffVecCtrl = kFbVecCtrl, gOffVcCurFh = kFbVcCurFh, gOffVcLastFh = kFbVcLastFh;
size_t gOffVcMovePath = kFbVcMovePath, gOffVcRelPos = kFbVcRelPos, gOffVcAp = kFbVcAp;
size_t gOffVcApl = kFbVcApl, gOffVcMoveAction = kFbVcMoveAction;
size_t gOffMpForcedFlush = kFbMpForcedFlush, gOffMpX = kFbMpX, gOffMpY = kFbMpY;
size_t gOffFhX1 = kFbFhX1, gOffFhY1 = kFbFhY1, gOffFhX2 = kFbFhX2, gOffFhY2 = kFbFhY2;
size_t gOffTeleport = kFbTeleport;

#define kOffVecCtrl (gOffVecCtrl)
#define kOffVcCurFh (gOffVcCurFh)
#define kOffVcLastFh (gOffVcLastFh)
#define kOffVcMovePath (gOffVcMovePath)
#define kOffVcRpPos (gOffVcRelPos)
#define kOffVcRpV (gOffVcRelPos + 8)
#define kOffVcApX (gOffVcAp)
#define kOffVcApY (gOffVcAp + 8)
#define kOffVcApVx (gOffVcAp + 0x10)
#define kOffVcApVy (gOffVcAp + 0x18)
#define kOffVcAplX (gOffVcApl)
#define kOffVcAplY (gOffVcApl + 8)
#define kOffVcMoveAction (gOffVcMoveAction)
#define kOffMpForcedFlush (gOffMpForcedFlush)
#define kOffMpX (gOffMpX)
#define kOffMpY (gOffMpY)
#define kOffFhX1 (gOffFhX1)
#define kOffFhY1 (gOffFhY1)
#define kOffFhX2 (gOffFhX2)
#define kOffFhY2 (gOffFhY2)
#define kOffTeleportIsValid (gOffTeleport)
#define kOffTeleportByPortal (gOffTeleport + 1)
#define kOffTeleportPosX (gOffTeleport + 4)
#define kOffTeleportPosY (gOffTeleport + 8)
#define kOffTeleportStartTick (gOffTeleport + 0xC)
#define kOffTeleportCoolTimeEnd (gOffTeleport + 0x10)
bool gTpFieldTried = false;

bool TpFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t lo, size_t hi) {
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

void EnsureTpFieldOff() {
    if (gTpFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gTpFieldTried = true;
    void* actor = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    if (!actor) actor = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* vc = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* mp = x::runtime::il2cpp::FindClass("", kMovePathClass);
    void* fh = x::runtime::il2cpp::FindClass("", kFhClass);
    int hits = 0;
    auto hit = [&](bool ok) {
        if (ok) ++hits;
    };
    hit(TpFieldOffHit(actor, kHashUserVecCtrl, kFbVecCtrl, &gOffVecCtrl, 0x40, 0x100));
    hit(TpFieldOffHit(vc, kHashVcCurFh, kFbVcCurFh, &gOffVcCurFh, 0x10, 0x80));
    hit(TpFieldOffHit(vc, kHashVcLastFh, kFbVcLastFh, &gOffVcLastFh, 0x10, 0x80));
    hit(TpFieldOffHit(vc, kHashVcMovePath, kFbVcMovePath, &gOffVcMovePath, 0x40, 0x100));
    hit(TpFieldOffHit(vc, kHashVcRelPos, kFbVcRelPos, &gOffVcRelPos, 0x60, 0x100));
    hit(TpFieldOffHit(vc, kHashVcAp, kFbVcAp, &gOffVcAp, 0x80, 0x100));
    hit(TpFieldOffHit(vc, kHashVcApl, kFbVcApl, &gOffVcApl, 0x80, 0x100));
    hit(TpFieldOffHit(vc, kHashVcMoveAction, kFbVcMoveAction, &gOffVcMoveAction, 0x40, 0x100));
    hit(TpFieldOffHit(mp, kHashMpForcedFlush, kFbMpForcedFlush, &gOffMpForcedFlush, 0x20, 0x80));
    hit(TpFieldOffHit(mp, kHashMpX, kFbMpX, &gOffMpX, 0x08, 0x40));
    hit(TpFieldOffHit(mp, kHashMpY, kFbMpY, &gOffMpY, 0x08, 0x40));
    hit(TpFieldOffHit(fh, kHashFhX1, kFbFhX1, &gOffFhX1, 0x10, 0x80));
    hit(TpFieldOffHit(fh, kHashFhY1, kFbFhY1, &gOffFhY1, 0x10, 0x80));
    hit(TpFieldOffHit(fh, kHashFhX2, kFbFhX2, &gOffFhX2, 0x10, 0x80));
    hit(TpFieldOffHit(fh, kHashFhY2, kFbFhY2, &gOffFhY2, 0x10, 0x80));
    hit(TpFieldOffHit(ul, kHashTeleport, kFbTeleport, &gOffTeleport, 0x300, 0x500));
    constexpr int kExpect = 16;
    x::runtime::LogI("Teleport",
                     "tp slots path=%s hits=%d/%d vc=0x%zX ap=0x%zX mp=0x%zX fhX1=0x%zX tp=0x%zX",
                     hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
                     gOffVecCtrl, gOffVcAp, gOffVcMovePath, gOffFhX1, gOffTeleport);
}

constexpr DWORD kNativeSelfCdMinMs = 5;
constexpr DWORD kNativeSelfCdMaxMs = 8000;
constexpr float kNativeShortHopPx = 140.f;
constexpr DWORD kJobWaitMs = 2000;

std::atomic<uint32_t> gNativeCdMs{kNativeSelfCdMinMs};
DWORD gLastNativeMs = 0;
std::atomic<DWORD> gLastNativeOkMs{0};

void MarkNativeOk(DWORD now) {
    gLastNativeMs = now;
    gLastNativeOkMs.store(now, std::memory_order_release);
}

using FnTryDoingTeleport = void (*)(void* self, const void* method);
using FnSetForcedFlush = void (*)(void* self, const void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

FnTryDoingTeleport gDoing = nullptr;
FnSetForcedFlush gSetFlush = nullptr;
FnSetForcedFlush gMpSetFlush = nullptr;

void* gGA = nullptr;
void* gLocalUserKlass = nullptr;
void* gMiDoing = nullptr;
std::atomic<bool> gBound{false};

template <typename T>
T AtRva(uint32_t rva) {
    return x::runtime::il2cpp::AtRva<T>(rva);
}

void WriteF64(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

void WriteF32(void* obj, size_t off, float v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteBool(void* obj, size_t off, bool v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReadBool(void* obj, size_t off) {
    if (!obj) return false;
    __try {
        return *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WriteI16(void* obj, size_t off, int16_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteI32(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WritePtr(void* obj, size_t off, void* v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool BindFns() {
    if (gBound.load(std::memory_order_acquire) && gDoing) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    gGA = x::runtime::il2cpp::GameAssembly();
    if (!gGA) return false;
    gLocalUserKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();

    auto findByRva = [&](void* klass, uint32_t rva) -> MethodInfoHead* {
        if (!klass || !rva || !gGA) return nullptr;
        const auto& ex = x::runtime::il2cpp::Get();
        if (!ex.classGetMethods) return nullptr;
        void* target = AtRva<void*>(rva);
        void* cur = klass;
        for (int depth = 0; cur && depth < 8; ++depth) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* miRaw = ex.classGetMethods(cur, &iter);
                    if (!miRaw) break;
                    auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
                    void* mp = nullptr;
                    void* vp = nullptr;
                    __try {
                        mp = mi->methodPointer;
                        vp = mi->virtualMethodPointer;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        continue;
                    }
                    if (mp == target || vp == target) return mi;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
            if (!ex.classParent) break;
            void* parent = nullptr;
            __try {
                parent = ex.classParent(cur);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                parent = nullptr;
            }
            if (!parent || parent == cur) break;
            cur = parent;
        }
        return nullptr;
    };

    auto findByName = [](void* klass, const char* name, int argc) -> MethodInfoHead* {
        if (!klass || !name) return nullptr;
        const auto& e = x::runtime::il2cpp::Get();
        MethodInfoHead* mi = nullptr;
        if (e.classGetMethodFromName) {
            const int tryArgc[] = {argc, -1};
            for (int ac : tryArgc) {
                __try {
                    mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, ac));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    mi = nullptr;
                }
                if (mi && mi->methodPointer) return mi;
            }
        }
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
    };

    auto resolveMi = [&](void* klass, uint32_t rva,
                         const x::runtime::il2cpp_method::MethodShape& shape, const char* plain,
                         const char* hash,
                         x::runtime::il2cpp_method::ResolvePath* outPath) -> MethodInfoHead* {
        if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
        if (!klass) return nullptr;
        const auto mr =
            x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
        if (outPath) *outPath = mr.path;
        return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
    };

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    int hashHits = 0;
    ResolvePath pathDoing = ResolvePath::Miss, pathVc = ResolvePath::Miss,
                pathMp = ResolvePath::Miss;
    // void() 在 UL 上极多 → unique=false，靠哈希/RVA。
    if (gLocalUserKlass) {
        constexpr MethodShape kDoing{0, TypeKind::Void, false, true, {}};
        auto* mi = resolveMi(gLocalUserKlass, kRvaTryDoingTeleport, kDoing, "TryDoingTeleport",
                             kHashTryDoingTeleport, &pathDoing);
        gMiDoing = mi;
        if (mi && mi->methodPointer)
            gDoing = reinterpret_cast<FnTryDoingTeleport>(mi->methodPointer);
        if (mi && pathDoing == ResolvePath::Hash) ++hashHits;
    }
    if (!gDoing) gDoing = AtRva<FnTryDoingTeleport>(kRvaTryDoingTeleport);

    // SetForcedFlush：void() 不唯一 → 方法哈希。
    void* vcKlass = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* mpKlass = x::runtime::il2cpp::FindClass("", kMovePathClass);
    constexpr MethodShape kFlush{0, TypeKind::Void, false, false, {}};
    MethodInfoHead* miVc =
        resolveMi(vcKlass, kRvaVecCtrlSetForcedFlush, kFlush, "SetForcedFlush",
                  kHashVecCtrlSetForcedFlush, &pathVc);
    MethodInfoHead* miMp =
        resolveMi(mpKlass, kRvaMovePathSetForcedFlush, kFlush, "SetForcedFlush",
                  kHashMovePathSetForcedFlush, &pathMp);
    if (miVc && pathVc == ResolvePath::Hash) ++hashHits;
    if (miMp && pathMp == ResolvePath::Hash) ++hashHits;
    if (miVc && miVc->methodPointer)
        gSetFlush = reinterpret_cast<FnSetForcedFlush>(miVc->methodPointer);
    if (miMp && miMp->methodPointer)
        gMpSetFlush = reinterpret_cast<FnSetForcedFlush>(miMp->methodPointer);
    if (!gSetFlush) gSetFlush = AtRva<FnSetForcedFlush>(kRvaVecCtrlSetForcedFlush);
    if (!gMpSetFlush) gMpSetFlush = AtRva<FnSetForcedFlush>(kRvaMovePathSetForcedFlush);

    EnsureTpFieldOff();
    const bool ok = gDoing != nullptr;
    gBound.store(ok, std::memory_order_release);
    if (ok) {
        const int n = (gMiDoing ? 1 : 0) + (miVc ? 1 : 0) + (miMp ? 1 : 0);
        x::runtime::LogI("Teleport",
                         "methods path=%s hits=%d/3 hash=%d Doing@0x%X Flush@0x%X MpFlush@0x%X "
                         "mi(doing=%d vc=%d mp=%d)",
                         hashHits == 3 ? "meta" : (hashHits ? "meta-partial" : "rva/kind"), n,
                         hashHits, kRvaTryDoingTeleport, kRvaVecCtrlSetForcedFlush,
                         kRvaMovePathSetForcedFlush, gMiDoing ? 1 : 0, miVc ? 1 : 0, miMp ? 1 : 0);
        char detail[48]{};
        snprintf(detail, sizeof(detail), "mi %d/3", n);
        x::runtime::anchor_lamps::Set(
            "Teleport",
            n == 3   ? x::runtime::anchor_lamps::AnchorLampCode::Ok
            : n > 0  ? x::runtime::anchor_lamps::AnchorLampCode::Degraded
                     : x::runtime::anchor_lamps::AnchorLampCode::Degraded,
            detail);
    } else {
        x::runtime::LogW("Teleport", "bind fail Doing=%p", gDoing);
        x::runtime::anchor_lamps::Set("Teleport", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "MISS");
    }
    return ok;
}

bool CallDoingOnlySeh(void* lu) {
    if (!gDoing || !LooksLikeHeapPtr(lu)) return false;
    __try {
        gDoing(lu, gMiDoing);
        // P0d §6.1：产品路径不再 ForcedFlush。
        // BIN 2f112a：Doing 后沿 Walk 链 rpV=nan 狂奔 → soft reload；Flush 会立刻推送脏 Move，放大窗口。
        // 若回归「瞬移后皮长时间不同步」，再按 P0d 差分表考虑有条件恢复 Flush。
        (void)gSetFlush;
        (void)gMpSetFlush;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PickNativeShortLand(void* /*lu*/, void* vc, float* outX, float* outY, uint32_t* outFh,
                         int* outFaceLeft) {
    if (!outX || !outY) return false;
    const double ox = ReadF64(vc, kOffVcApX);
    const double oy = ReadF64(vc, kOffVcApY);
    int faceLeft = 0;
    __try {
        const int ma = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
        faceLeft = ma & 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faceLeft = 0;
    }
    const bool keyLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    const bool keyRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    if (keyLeft && !keyRight) faceLeft = 1;
    else if (keyRight && !keyLeft) faceLeft = 0;

    const float prefer = faceLeft ? -kNativeShortHopPx : kNativeShortHopPx;
    const float alt = -prefer;
    float tx = 0.f, ty = 0.f;
    uint32_t fh = 0;
    if (foothold_path::SnapStandAt(static_cast<float>(ox) + prefer, static_cast<float>(oy), &tx,
                                   &ty, &fh)) {
        *outX = tx;
        *outY = ty;
        if (outFh) *outFh = fh;
        if (outFaceLeft) *outFaceLeft = faceLeft;
        return true;
    }
    if (foothold_path::SnapStandAt(static_cast<float>(ox) + alt, static_cast<float>(oy), &tx, &ty,
                                   &fh)) {
        *outX = tx;
        *outY = ty;
        if (outFh) *outFh = fh;
        if (outFaceLeft) *outFaceLeft = faceLeft ? 0 : 1;
        return true;
    }
    *outX = static_cast<float>(ox) + prefer;
    *outY = static_cast<float>(oy);
    if (outFh) *outFh = 0;
    if (outFaceLeft) *outFaceLeft = faceLeft;
    return true;
}

bool FillTeleportPending(void* lu, float tx, float ty) {
    if (!LooksLikeHeapPtr(lu)) return false;
    WriteF32(lu, kOffTeleportPosX, tx);
    WriteF32(lu, kOffTeleportPosY, ty);
    WriteBool(lu, kOffTeleportByPortal, false);
    WriteI32(lu, kOffTeleportStartTick, 0);
    WriteI32(lu, kOffTeleportCoolTimeEnd, 0);
    WriteBool(lu, kOffTeleportIsValid, true);
    return true;
}

// 落点在踏板线段上的弧长 → RelPos.Pos；V=0。有台时下一帧 AbsPos←RelPos 用这个，防假到位回拉。
bool WriteRelPosFromLand(void* vc, void* fhObj, float tx, float ty) {
    if (!LooksLikeHeapPtr(vc) || !LooksLikeHeapPtr(fhObj)) return false;
    const int x1 = ReadI32(fhObj, kOffFhX1);
    const int y1 = ReadI32(fhObj, kOffFhY1);
    const int x2 = ReadI32(fhObj, kOffFhX2);
    const int y2 = ReadI32(fhObj, kOffFhY2);
    const double dx = static_cast<double>(x2 - x1);
    const double dy = static_cast<double>(y2 - y1);
    const double len2 = dx * dx + dy * dy;
    double pos = 0.0;
    if (len2 > 1.0) {
        double t = ((static_cast<double>(tx) - x1) * dx + (static_cast<double>(ty) - y1) * dy) / len2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        pos = t * std::sqrt(len2);
    }
    WriteF64(vc, kOffVcRpPos, pos);
    WriteF64(vc, kOffVcRpV, 0.0);
    return true;
}

// requireFh=true：贴地瞬移（F5/短跳）要本图 FH 缓存+图就绪。
// requireFh=false：F6 点飞悬空——只验 PlayReady + LocalUser/VecCtrl，不改变起飞手感。
bool CheckPhysicsReadyUnlocked(bool requireFh, char* fail, size_t failN) {
    auto setFail = [&](const char* why) {
        if (fail && failN) strncpy_s(fail, failN, why ? why : "phys", _TRUNCATE);
    };
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        setFail("not_play_ready");
        return false;
    }
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
        setFail("no_user");
        return false;
    }
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) {
        setFail("no_vc");
        return false;
    }
    if (!requireFh) {
        if (fail && failN) fail[0] = 0;
        return true;
    }

    const int mapId = world::GetMapId();
    if (mapId <= 0) {
        setFail("no_map");
        return false;
    }
    foothold::SnapshotMeta meta{};
    if (!foothold::IsCacheReadyForMap(mapId)) {
        if (!foothold::CollectToCache(&meta) || !meta.ok || meta.mapId != mapId ||
            meta.footholdN <= 0) {
            setFail("fh_cache");
            return false;
        }
    }
    if (!foothold_path::EnsureGraph()) {
        setFail("fh_graph");
        return false;
    }
    foothold_path::GraphMeta gm{};
    if (!foothold_path::GetGraphMeta(&gm) || !gm.ok || gm.mapId != mapId) {
        setFail("fh_graph_map");
        return false;
    }
    // 若仍挂着可解析的 CurFh，须在本图缓存里——旧图台几何是假到位主因。
    // 解析失败视为残留脏指针：不卡死门禁，交由 ApplyFillDoing 重种/清空。
    const uint32_t curFh = foothold::PeekCurFhId();
    if (curFh != 0) {
        if (LooksLikeHeapPtr(foothold::ResolveFhObject(curFh))) {
            foothold::FootholdLite lite{};
            if (!foothold::TryGetCachedFh(curFh, &lite)) {
                setFail("curfh_cache");
                return false;
            }
        }
    }
    if (fail && failN) fail[0] = 0;
    return true;
}

// 手填 Teleport + Mp +（可选 CurFh）+ Doing。不 SyncRel 泵、不 Register、不 ImpactNext。
// Doing 前：挂目标台 + RelPos（绝对贴地路径）。
// Doing 后：
//   - 若 CurFh 仍空：仅补种 CurFh/LastFh + RelPos（BIN e27c33：Doing 清空台，岛台永不重挂 → 失粘）
//   - Apl←Ap（皮对齐；Ap 近原点则跳过）
// 禁止：硬写 Ap / HealVisual / 无条件后置抢收态（悬崖图软重载，见 P0d）。
bool ApplyFillDoing(void* lu, void* vc, float tx, float ty, uint32_t fhId, const char* tag) {
    // 硬拒：永不手填到原点邻域。
    if (!std::isfinite(tx) || !std::isfinite(ty) ||
        (std::fabs(tx) < 8.f && std::fabs(ty) < 8.f)) {
        x::runtime::LogW("Teleport", "fill+Doing reject origin land=(%.0f,%.0f) fh=%u", tx, ty,
                         (unsigned)fhId);
        return false;
    }
    if (!FillTeleportPending(lu, tx, ty)) return false;
    void* mp = ReadPtr(vc, kOffVcMovePath);
    if (LooksLikeHeapPtr(mp)) {
        WriteI16(mp, kOffMpX, static_cast<int16_t>(static_cast<int>(std::lround(tx))));
        WriteI16(mp, kOffMpY, static_cast<int16_t>(static_cast<int>(std::lround(ty))));
    }

    // 引擎原生瞬移体（VecCtrl RVA 0x11A8460，由 TryDoingTeleport 消费 pending 后调用）只做：
    //   CurFh(vc+0x28)=null → LastFh(vc+0x40)=null → Ap/Apl ← (int)x,(int)y →
    //   Ap.V/Apl.V 归零 →（vc+0x80 置位时）MovePath_MakeMovePath 上报。
    // 它**不写 RelPos、不挂踏板**；踏板由下一物理帧 CollisionDetect 自行重挂。
    // 故这里不再预挂目标台：Doing 若被内部门禁提前挡回，预挂会留下「远台 + 人在原位」的脏
    // 组合，下一帧 AbsPos←RelPos 直接把人拽走。fh 只做可解析性校验。
    if (fhId != 0) {
        if (!LooksLikeHeapPtr(foothold::ResolveFhObject(fhId))) {
            x::runtime::LogW("Teleport", "fill plant resolve fail fh=%u", (unsigned)fhId);
            return false;
        }
    } else {
        // 点飞悬空：卸掉可能残留的旧图台（只 detach，不会挪动角色），
        // 否则下一帧 AbsPos←RelPos 闪回旧图坐标。
        WritePtr(vc, kOffVcCurFh, nullptr);
        WritePtr(vc, kOffVcLastFh, nullptr);
    }

    if (!CallDoingOnlySeh(lu)) return false;

    // 此处曾有的「收态」已全部撤除，它们正是 land_miss 的根因：
    // 引擎每次都把 CurFh 清空，旧代码于是每次都补种 CurFh + WriteRelPosFromLand；下一帧引擎
    // 按 AbsPos←f(我们给的台, 我们算的弧长) 重算位置，我们算错就被拽走，服端读到位置违规。
    // 07a30a 实测：86 次 land_miss 只出自 20 个落点，每点恒定偏 19~39px、引擎每次摆到同一处
    // ——确定性错误，正是我们自己算的。Apl←Ap 与 Ap.V 归零引擎已自己做完，重复写无意义。
    // 岛台不重挂（BIN e27c33 失粘）改由 Settling 侧确认「久未挂台」后再 Stabilize 救援。

    // RelPos.V 已是 nan 时消毒：只清毒、不写位置，避免 nan 顺着 CalcWalk 扩散。
    {
        const double rpV = ReadF64(vc, kOffVcRpV);
        if (!std::isfinite(rpV)) WriteF64(vc, kOffVcRpV, 0.0);
    }
    // BIN a9b624：Doing 后 InputX 若仍锁存，CalcWalk 会沿同 z Walk 链滑走；只清输入锁存。
    attack::ClearWalkLatchMainThread();

    {
        char camNote[120]{};
        std::snprintf(camNote, sizeof(camNote), "fill_native tag=%s land=(%.0f,%.0f) fh=%u",
                      tag ? tag : "?", tx, ty, (unsigned)fhId);
        player_combat::LogFillDoingCamProbe(camNote);
    }
    x::runtime::LogI("Teleport",
                     "fill+Doing %s land=(%.0f,%.0f) fh=%u ap=(%.0f,%.0f) rp=%.1f native=1 "
                     "flush=0",
                     tag ? tag : "?", tx, ty, (unsigned)fhId, ReadF64(vc, kOffVcApX),
                     ReadF64(vc, kOffVcApY), ReadF64(vc, kOffVcRpPos));
    return true;
}

struct NativeJob {
    bool ok = false;
    bool overrideLand = false;
    bool snapStand = true;
    float landX = 0.f;
    float landY = 0.f;
    uint32_t plantFhId = 0;
    char fail[48]{};
};

void SetNativeFail(NativeJob* job, const char* why) {
    if (!job) return;
    strncpy_s(job->fail, why ? why : "fail", _TRUNCATE);
}

void NativeTeleportJobFn(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("teleport.Native");
    auto* job = reinterpret_cast<NativeJob*>(user);
    if (!job) return;
    job->ok = false;
    job->fail[0] = 0;
    // 主线程二次闸：worker 入口过闸后、Invoke 前可能已 BeginAct（BUFF 锁移动 TOCTOU）。
    // 清 CD 挪到二次闸之后（见下方），此处只早拒。
    if (action_gate::IsTeleportForbidden()) {
        SetNativeFail(job, "skill_cast_busy");
        return;
    }
    if (!BindFns() || !gDoing) {
        SetNativeFail(job, "bind");
        return;
    }
    if (!CheckPhysicsReadyUnlocked(/*requireFh=*/job->snapStand || !job->overrideLand, job->fail,
                                   sizeof(job->fail))) {
        if (!job->fail[0]) SetNativeFail(job, "phys");
        return;
    }

    player_combat::CombatCtx ctx{};
    void* lu = nullptr;
    // 绝对落点（fly / 贴怪）：不要求 Ap 已 PosSane——换图落地瞬间 QueryCombatCtx 会误报 no_user。
    // 短距自选落点仍走 QueryCombatCtx（需要朝向/坐标）。物理就绪已在上面闸住 FH/图。
    if (job->overrideLand) {
        if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
            SetNativeFail(job, "no_user");
            return;
        }
    } else {
        if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok || !LooksLikeHeapPtr(ctx.localUser)) {
            SetNativeFail(job, "no_user");
            return;
        }
        lu = ctx.localUser;
    }
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) {
        SetNativeFail(job, "no_vc");
        return;
    }

    float tx = job->landX;
    float ty = job->landY;
    uint32_t fh = job->plantFhId;
    const char* tag = "long";

    if (!job->overrideLand) {
        tag = "short";
        int faceLeft = 0;
        if (!PickNativeShortLand(lu, vc, &tx, &ty, &fh, &faceLeft)) {
            SetNativeFail(job, "no_land");
            return;
        }
        x::runtime::LogI("Teleport", "short pick land=(%.0f,%.0f) faceL=%d fh=%u", tx, ty, faceLeft,
                         (unsigned)fh);
    } else {
        if (!std::isfinite(tx) || !std::isfinite(ty)) {
            SetNativeFail(job, "bad_land");
            return;
        }
        if (job->snapStand) {
            float sx = tx, sy = ty;
            uint32_t sfh = 0;
            if (foothold_path::SnapStandAt(tx, ty, &sx, &sy, &sfh)) {
                tx = sx;
                ty = sy;
                if (sfh != 0) fh = sfh;
            }
        }
    }

    // 硬门禁：fh==0 禁止 fill+Doing。
    // 例外：点飞 overrideLand && !snapStand（fly）允许悬空落点。
    if (fh == 0) {
        if (!(job->overrideLand && !job->snapStand)) {
            SetNativeFail(job, "fh0_forbid");
            x::runtime::LogW("Teleport", "forbid fill+Doing fh=0 land=(%.0f,%.0f) snap=%d", tx, ty,
                             job->snapStand ? 1 : 0);
            return;
        }
    } else if (!LooksLikeHeapPtr(foothold::ResolveFhObject(fh))) {
        SetNativeFail(job, "fh_resolve");
        x::runtime::LogW("Teleport", "fh resolve fail id=%u land=(%.0f,%.0f)", (unsigned)fh, tx, ty);
        return;
    }

    // 落点计算后、fill 前再查。CD 只由 FillTeleportPending 写入（禁再 ClearTeleportClientCooldown 双清）。
    if (action_gate::IsTeleportForbidden()) {
        SetNativeFail(job, "skill_cast_busy");
        return;
    }

    if (!ApplyFillDoing(lu, vc, tx, ty, fh, tag)) {
        SetNativeFail(job, "seh_doing");
        return;
    }
    job->ok = true;
    job->landX = tx;
    job->landY = ty;
    job->plantFhId = fh;
    SetNativeFail(job, job->overrideLand ? "ok_fill_long" : "ok_fill_doing");
}

}  // namespace

bool EnsureBound() { return BindFns(); }

bool IsPhysicsReadyForNative() {
    char why[48]{};
    return CheckPhysicsReadyUnlocked(/*requireFh=*/true, why, sizeof(why));
}

void SetNativeCooldownMs(uint32_t ms) {
    if (ms < kNativeSelfCdMinMs) ms = kNativeSelfCdMinMs;
    if (ms > kNativeSelfCdMaxMs) ms = kNativeSelfCdMaxMs;
    gNativeCdMs.store(ms, std::memory_order_release);
}

void ForceNativeCooldownMs(uint32_t ms) {
    SetNativeCooldownMs(ms);
    gLastNativeMs = GetTickCount();
    x::runtime::LogI("Teleport", "force self-cd %ums (trip arm)",
                     gNativeCdMs.load(std::memory_order_acquire));
}

void ClearNativeSelfCd() { gLastNativeMs = 0; }

uint32_t NativeCooldownRemainingMs() {
    const DWORD now = GetTickCount();
    const DWORD cd = gNativeCdMs.load(std::memory_order_acquire);
    if (!gLastNativeMs || !cd) return 0;
    const DWORD elapsed = now - gLastNativeMs;
    if (elapsed >= cd) return 0;
    return static_cast<uint32_t>(cd - elapsed);
}

bool TeleportNativeSkillCall() {
    if (action_gate::IsTeleportForbidden()) {
        x::runtime::LogWThrottled(77, 500, "Teleport", "native short reject skill_prepare_or_busy");
        return false;
    }
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        x::runtime::LogW("Teleport", "native short reject not_play_ready scene=%d",
                         static_cast<int>(world::GetSceneState()));
        return false;
    }
    {
        char why[48]{};
        if (!CheckPhysicsReadyUnlocked(/*requireFh=*/true, why, sizeof(why))) {
            x::runtime::LogW("Teleport", "native short reject phys=%s", why[0] ? why : "?");
            return false;
        }
    }
    if (!BindFns() || !gDoing) {
        x::runtime::LogW("Teleport", "native bind fail Doing=%p", gDoing);
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD cd = gNativeCdMs.load(std::memory_order_acquire);
    if (gLastNativeMs && now - gLastNativeMs < cd) {
        x::runtime::LogW("Teleport", "native self-cd remain=%ums", cd - (now - gLastNativeMs));
        return false;
    }

    if (!runtime::main_thread::Ensure()) {
        x::runtime::LogW("Teleport", "native main pump missing");
        return false;
    }

    NativeJob job{};
    job.overrideLand = false;
    if (!runtime::main_thread::InvokeAndWait(&NativeTeleportJobFn, &job, kJobWaitMs,
                                            runtime::main_thread::JobPrio::High)) {
        x::runtime::LogW("Teleport", "native main-thread timeout");
        return false;
    }
    if (!job.ok) {
        x::runtime::LogW("Teleport", "native fail=%s", job.fail[0] ? job.fail : "?");
        return false;
    }
    MarkNativeOk(now);
    x::runtime::LogI("Teleport", "native ok tag=%s land=(%.0f,%.0f) fh=%u", job.fail, job.landX,
                     job.landY, (unsigned)job.plantFhId);
    return true;
}

bool TeleportNativeSkillCall(float landX, float landY, uint32_t plantFhId, bool snapStand) {
    if (!std::isfinite(landX) || !std::isfinite(landY)) {
        x::runtime::LogW("Teleport", "native land reject non-finite");
        return false;
    }
    // 永远不使用原点邻域（0,0）作落点。
    if (std::fabs(landX) < 8.f && std::fabs(landY) < 8.f) {
        x::runtime::LogW("Teleport", "native land reject origin land=(%.0f,%.0f) fh=%u", landX,
                         landY, (unsigned)plantFhId);
        return false;
    }
    // 贴地路径禁 |x|<8（combat 分段曾落到 to=(0,y)，Ap 随后归零 / 画面掉出图外）。
    // 点飞 snapStand=false 不卡，保留 F6 手感。
    if (snapStand && std::fabs(landX) < 8.f) {
        x::runtime::LogW("Teleport", "native land reject axis_x land=(%.0f,%.0f) fh=%u", landX,
                         landY, (unsigned)plantFhId);
        return false;
    }
    // 边界闸：贴地 + 已种 fh 用 margin=0（BIN 14:12：门在 FH AABB 底边 land.y==B，
    // kLandMarginPx=24 内缩把合法 Snap 判成 out_of_bounds → 赶路 TELEPORT_FAIL 空转）。
    // 无 fh 的贴地仍内缩，挡 combat 贴可视边飞出。
    if (snapStand) {
        const int margin =
            plantFhId != 0 ? 0 : map_bounds::kLandMarginPx;
        if (!map_bounds::PointInPlayBounds(landX, landY, /*mapId=*/0, margin)) {
            x::runtime::LogW("Teleport",
                             "native land reject out_of_bounds land=(%.0f,%.0f) fh=%u margin=%d",
                             landX, landY, (unsigned)plantFhId, margin);
            return false;
        }
    }
    // BUFF Hold / 引擎 Prepare 警戒态：禁止 fill+Doing（视觉层与镜头散开）。
    if (action_gate::IsTeleportForbidden()) {
        x::runtime::LogWThrottled(77, 500, "Teleport", "native reject skill_prepare_or_busy");
        return false;
    }
    // 硬门禁：换图 scene!=play 禁止瞬移
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        x::runtime::LogW("Teleport", "native reject not_play_ready scene=%d",
                         static_cast<int>(world::GetSceneState()));
        return false;
    }
    {
        char why[48]{};
        // F6 点飞 snapStand=false：不卡 FH 图，保持起飞手感。
        if (!CheckPhysicsReadyUnlocked(/*requireFh=*/snapStand, why, sizeof(why))) {
            x::runtime::LogW("Teleport", "native reject phys=%s", why[0] ? why : "?");
            return false;
        }
    }
    if (!BindFns() || !gDoing) {
        x::runtime::LogW("Teleport", "native bind fail Doing=%p", gDoing);
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD cd = gNativeCdMs.load(std::memory_order_acquire);
    if (gLastNativeMs && now - gLastNativeMs < cd) {
        x::runtime::LogW("Teleport", "native self-cd remain=%ums", cd - (now - gLastNativeMs));
        return false;
    }

    if (!runtime::main_thread::Ensure()) {
        x::runtime::LogW("Teleport", "native main pump missing");
        return false;
    }

    NativeJob job{};
    job.overrideLand = true;
    job.snapStand = snapStand;
    job.landX = landX;
    job.landY = landY;
    job.plantFhId = plantFhId;
    if (!runtime::main_thread::InvokeAndWait(&NativeTeleportJobFn, &job, kJobWaitMs,
                                            runtime::main_thread::JobPrio::High)) {
        x::runtime::LogW("Teleport", "native main-thread timeout");
        return false;
    }
    if (!job.ok) {
        x::runtime::LogW("Teleport", "native fail=%s land=(%.0f,%.0f) fh=%u snap=%d",
                         job.fail[0] ? job.fail : "?", landX, landY, (unsigned)plantFhId,
                         snapStand ? 1 : 0);
        return false;
    }
    MarkNativeOk(now);
    x::runtime::LogI("Teleport", "native ok tag=%s land=(%.0f,%.0f) fh=%u snap=%d", job.fail,
                     job.landX, job.landY, (unsigned)job.plantFhId, snapStand ? 1 : 0);
    return true;
}

bool IsPostTeleportQuiet(uint32_t quietMs) {
    if (quietMs == 0) return false;
    const DWORD last = gLastNativeOkMs.load(std::memory_order_acquire);
    if (!last) return false;
    const DWORD now = GetTickCount();
    return (now - last) < quietMs;
}

bool StabilizeFootholdMainThread(float landX, float landY, uint32_t fhId, bool replant) {
    (void)x::runtime::main_thread::AssertOnPumpThread("teleport.StabilizeFh");
    if (!std::isfinite(landX) || !std::isfinite(landY)) return false;
    if (!BindFns()) return false;
    EnsureTpFieldOff();
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;

    if (replant) {
        if (!fhId || (std::fabs(landX) < 8.f && std::fabs(landY) < 8.f)) return false;
        void* plantFh = foothold::ResolveFhObject(fhId);
        if (!LooksLikeHeapPtr(plantFh)) return false;
        WritePtr(vc, kOffVcCurFh, plantFh);
        WritePtr(vc, kOffVcLastFh, plantFh);
        if (!WriteRelPosFromLand(vc, plantFh, landX, landY)) return false;
    } else {
        // BIN 4ab7b0：同点 fhDrift 时 replant=1 与 CollisionDetect 抢交接 → soft。
        // 拆掉 CurFh 打断 Walk/CalcWalk；只清 V + InputX。不拧 RelPos、不硬写 Ap。
        WritePtr(vc, kOffVcCurFh, nullptr);
        WritePtr(vc, kOffVcLastFh, nullptr);
        WriteF64(vc, kOffVcRpV, 0.0);
    }
    WriteF64(vc, kOffVcApVx, 0.0);
    WriteF64(vc, kOffVcApVy, 0.0);
    attack::ClearWalkLatchMainThread();

    {
        const double nx = ReadF64(vc, kOffVcApX);
        const double ny = ReadF64(vc, kOffVcApY);
        if (std::isfinite(nx) && std::isfinite(ny) &&
            !(std::fabs(nx) < 8.0 && std::fabs(ny) < 8.0)) {
            WriteF64(vc, kOffVcAplX, nx);
            WriteF64(vc, kOffVcAplY, ny);
        }
    }
    x::runtime::LogI("Teleport", "stabilize fh=%u land=(%.0f,%.0f) rp=%.1f replant=%d detach=%d",
                     (unsigned)fhId, landX, landY, ReadF64(vc, kOffVcRpPos), replant ? 1 : 0,
                     replant ? 0 : 1);
    return true;
}

bool ClearMotionLatchMainThread() {
    (void)x::runtime::main_thread::AssertOnPumpThread("teleport.ClearMotionLatch");
    if (!BindFns()) return false;
    EnsureTpFieldOff();
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    WriteF64(vc, kOffVcApVx, 0.0);
    WriteF64(vc, kOffVcApVy, 0.0);
    const double rpV = ReadF64(vc, kOffVcRpV);
    if (!std::isfinite(rpV) || std::fabs(rpV) > 2500.0) WriteF64(vc, kOffVcRpV, 0.0);
    attack::ClearWalkLatchMainThread();
    return true;
}

}  // namespace x::features::ports::teleport
