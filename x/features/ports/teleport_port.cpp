// Classic TWMS — 位移端口：产品统一走 Impact（F5/F6）；fill+Doing 已失效。
// Impact：NockBack / SetImpactNext / ImpactHop / ImpactImpulseToward（Attr=2）。
// TeleportNativeSkillCall / TryDoingTeleport：入口硬拒，禁止再接入。
// Doing 绑桩代码暂留（EnsureBound 仍解析 RVA，供诊断灯）；不得再当产品扳机。
// IDB imagebase 见 Dumps/runtime/GameAssembly.dll。
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
#include "../invuln/invuln.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
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
#include <thread>

namespace x::features::ports::teleport {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// remount 纠偏 2026-08-06：消费 pending 的 Doing = 0x1027ED0（旧邻域 0x1026060），
// 不是 dump 误标名的 0x10AB400。
constexpr uint32_t kRvaTryDoingTeleport = 0x1027ED0;
constexpr uint32_t kRvaVecCtrlSetForcedFlush = 0x11AA570;  // → Mp+0x48 := 1（seed^6）
constexpr uint32_t kRvaMovePathSetForcedFlush = 0x11A1190;  // Mp+0x48 := 1（seed+0x6C）
// remount 2026-08-06：NockBack 内联写 impactNext；SetImpactNext CFA（IL2CPP xmm1/xmm2 ABI）
constexpr uint32_t kRvaVecCtrlNockBack = 0x11BDE90;
constexpr uint32_t kRvaVecCtrlSetImpactNext = 0x11AA5F0;
constexpr char kHashTryDoingTeleport[] =
    "af8e399020feab57b9618e03aa476b5fd41dab01d67a9c0184794a8fa823e74";
constexpr char kHashVecCtrlSetForcedFlush[] =
    "b9b4ff83cc6c06dd35af3749d73ef5a7c65d38e9a5af52530893bae2780d4b0";
constexpr char kHashMovePathSetForcedFlush[] =
    "bd22d20e9f838404e90eca003a298363f3811901d280ac6a687f35aac79f693";
constexpr char kVecCtrlClass[] =
    "e0eb55b82f10cb9eeb9424eb3aadf1450a014afa564bc55c3739b2909abfbbc";
constexpr char kMovePathClass[] =
    "bf8877c4b6040ee2bbcd43dece8fa422d433874b58900fd618aad4f5b0309e7";
constexpr char kActorBaseClass[] =
    "edc85ce203606bdb549e5fb94458b1d2d11ce78034d24d41e39a54c0288d38e";
constexpr char kFhClass[] =
    "efa6625ea3b04a69e7c5b850c9e7f5be45cc60edd0f20d9bd6cd400a4dcd51a";
// True TW UserLocal = User subclass with Teleport@0x3C8（resolve: il2cpp_shape）

// hash → field_get_offset（与 foothold / attack / player_combat 同源）
constexpr char kHashUserVecCtrl[] =
    "<acb8946a384ed398c4ad9268349397cf4f6e65cf136078ebc9aa26a949efd41>k__BackingField";
constexpr char kHashVcCurFh[] =
    "<ee95e4ea9526a8d5a2c0e38ae6b59726f52587018d82072a2020dc6ac5a2398>k__BackingField";
constexpr char kHashVcLastFh[] =
    "<ea429355b0867d9d038619ab4f67f569951204a479856815af334e740450be2>k__BackingField";
constexpr char kHashVcMovePath[] =
    "<f4ed2fef5fe256b1a408b40c57a5c59e4fff75169f423e0ecb1ccd062926bb0>k__BackingField";
constexpr char kHashVcRelPos[] =
    "f9386810e222adacdfbd2e9232322d7cfcd301fa19b7734975ad616d30dfc0c";  // RelPos; V=+8
constexpr char kHashVcAp[] =
    "e558fbd3da65bf13bea9360dfa61506af709ad89f925bc16b67e7e1cdb24107";  // AbsPos; Y=+8
constexpr char kHashVcApl[] =
    "b5eb27f6f80eeaea51f811969e3c5bc8a7b73b19741a8cb481b29a0082c958d";  // Apl; Y=+8
constexpr char kHashVcMoveAction[] =
    "fa93e903eebde8b6fd77060143b0b2f1293e84eeba873dae2034090150daad4";
constexpr char kHashMpForcedFlush[] =
    "daff0d0fb89c860e58ea67292ca7d7353f50a0d671b9433ca1bb57b85fc1488";
constexpr char kHashMpX[] =
    "d99edd7ee4ef38e67d4d2e4868edde9a32596d6d3f968d070462878b120e0cf";
constexpr char kHashMpY[] =
    "d79f3c9cbc0755e62cb2034e7e8876a529873e107c2d6a69ff5e6720db86fa9";
constexpr char kHashFhX1[] =
    "<fb0267bbb0c9e644310fb7a85bb2de177d3efc8e9371567f0302ab754661f78>k__BackingField";
constexpr char kHashFhY1[] =
    "<f116f299fc84de052c922811e44d60e7d144d24e0494716e98c6c8027a340c1>k__BackingField";
constexpr char kHashFhX2[] =
    "<f7e3a1cba4a600a510aa957c3ae5b7bee9fe6ac36d205bb6dc4a3c4b9563fdb>k__BackingField";
constexpr char kHashFhY2[] =
    "<e5815a35031c0f19c85de13d144d3f905064d8edf6cf59601d743c1185aba4b>k__BackingField";
// UserLocal.Teleport valuetype block（嵌套相对：IsValid+0 / ByPortal+1 / Pos+4 / ticks+0xC/+0x10）
constexpr char kHashTeleport[] =
    "d3cfbaeeac1657b366daccedd1678e39163e53d47ef74f20cf2fceba1ca2750";

constexpr size_t kFbVecCtrl = 0x50, kFbVcCurFh = 0x28, kFbVcLastFh = 0x30, kFbVcMovePath = 0x78;
// P0b：LadderOrRope@0x40（无稳定 hash 时钉 fb；点飞预清不用它，飞穿层要卸）
constexpr size_t kFbVcLadderOrRope = 0x40;
constexpr size_t kFbVcRelPos = 0x88, kFbVcAp = 0x98, kFbVcApl = 0xB8, kFbVcMoveAction = 0x84;
constexpr size_t kFbMpForcedFlush = 0x48, kFbMpX = 0x10, kFbMpY = 0x12;
constexpr size_t kFbFhX1 = 0x14, kFbFhY1 = 0x18, kFbFhX2 = 0x1C, kFbFhY2 = 0x20;
constexpr size_t kFbTeleport = 0x3C8;

size_t gOffVecCtrl = kFbVecCtrl, gOffVcCurFh = kFbVcCurFh, gOffVcLastFh = kFbVcLastFh;
size_t gOffVcLadderOrRope = kFbVcLadderOrRope;
size_t gOffVcMovePath = kFbVcMovePath, gOffVcRelPos = kFbVcRelPos, gOffVcAp = kFbVcAp;
size_t gOffVcApl = kFbVcApl, gOffVcMoveAction = kFbVcMoveAction;
size_t gOffMpForcedFlush = kFbMpForcedFlush, gOffMpX = kFbMpX, gOffMpY = kFbMpY;
size_t gOffFhX1 = kFbFhX1, gOffFhY1 = kFbFhY1, gOffFhX2 = kFbFhX2, gOffFhY2 = kFbFhY2;
size_t gOffTeleport = kFbTeleport;

#define kOffVecCtrl (gOffVecCtrl)
#define kOffVcCurFh (gOffVcCurFh)
#define kOffVcLastFh (gOffVcLastFh)
#define kOffVcLadderOrRope (gOffVcLadderOrRope)
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
using FnNockBack = void (*)(void* self, int dir, int vx, int vy, const void* method);
// IL2CPP Windows x64：this@rcx · vx@xmm1 · vy@xmm2 · MethodInfo@r9（非 MSVC 默认 xmm0/xmm1）
using FnSetImpactNextThunk = void (*)(void* target, void* self, uint64_t vxBits, uint64_t vyBits,
                                      void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

FnTryDoingTeleport gDoing = nullptr;
FnSetForcedFlush gSetFlush = nullptr;
FnSetForcedFlush gMpSetFlush = nullptr;
FnNockBack gNockBack = nullptr;
void* gSetImpactNextRaw = nullptr;
FnSetImpactNextThunk gSetImpactNextThunk = nullptr;
void* gMiNockBack = nullptr;
void* gMiSetImpactNext = nullptr;

void* gGA = nullptr;
void* gLocalUserKlass = nullptr;
void* gMiDoing = nullptr;
std::atomic<bool> gBound{false};

FnSetImpactNextThunk EnsureSetImpactNextThunk() {
    if (gSetImpactNextThunk) return gSetImpactNextThunk;
    // rcx=target rdx=self r8=vxBits r9=vyBits [rsp+28h]=method
    // → rcx=self xmm1=vx xmm2=vy r9=method ; jmp target
    static const uint8_t kCode[] = {
        0x48, 0x89, 0xC8,              // mov rax, rcx
        0x48, 0x89, 0xD1,              // mov rcx, rdx
        0x66, 0x49, 0x0F, 0x6E, 0xC8,  // movq xmm1, r8
        0x66, 0x49, 0x0F, 0x6E, 0xD1,  // movq xmm2, r9
        0x4C, 0x8B, 0x4C, 0x24, 0x28,  // mov r9, [rsp+28h]
        0x48, 0x31, 0xD2,              // xor rdx, rdx
        0x4D, 0x31, 0xC0,              // xor r8, r8
        0x48, 0xFF, 0xE0,              // jmp rax
    };
    void* p = VirtualAlloc(nullptr, sizeof(kCode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!p) return nullptr;
    memcpy(p, kCode, sizeof(kCode));
    DWORD old = 0;
    VirtualProtect(p, sizeof(kCode), PAGE_EXECUTE_READ, &old);
    gSetImpactNextThunk = reinterpret_cast<FnSetImpactNextThunk>(p);
    return gSetImpactNextThunk;
}

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

int16_t ReadI16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t ReadU8(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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
        // 不传 plain「TryDoingTeleport」：dump C 误把该名贴到 0x10AB400，plain 会命中错桩。
        auto* mi = resolveMi(gLocalUserKlass, kRvaTryDoingTeleport, kDoing, nullptr,
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

    // 调试冲击：NockBack / SetImpactNext — 今日无可靠方法哈希，RVA 绑定
    {
        MethodInfoHead* miNb = findByRva(vcKlass, kRvaVecCtrlNockBack);
        MethodInfoHead* miSi = findByRva(vcKlass, kRvaVecCtrlSetImpactNext);
        gMiNockBack = miNb;
        gMiSetImpactNext = miSi;
        if (miNb && miNb->methodPointer)
            gNockBack = reinterpret_cast<FnNockBack>(miNb->methodPointer);
        if (miSi && miSi->methodPointer) gSetImpactNextRaw = miSi->methodPointer;
    }
    if (!gNockBack) gNockBack = AtRva<FnNockBack>(kRvaVecCtrlNockBack);
    if (!gSetImpactNextRaw) gSetImpactNextRaw = AtRva<void*>(kRvaVecCtrlSetImpactNext);
    (void)EnsureSetImpactNextThunk();

    EnsureTpFieldOff();
    const bool ok = gDoing != nullptr;
    gBound.store(ok, std::memory_order_release);
    if (ok) {
        const int n = (gMiDoing ? 1 : 0) + (miVc ? 1 : 0) + (miMp ? 1 : 0);
        x::runtime::LogI("Teleport",
                         "methods path=%s hits=%d/3 hash=%d Doing@0x%X Flush@0x%X "
                         "MpFlush@0x%X mi(doing=%d vc=%d mp=%d) NockBack@0x%X SetImpact@0x%X",
                         hashHits == 3 ? "meta" : (hashHits ? "meta-partial" : "rva/kind"), n,
                         hashHits, kRvaTryDoingTeleport, kRvaVecCtrlSetForcedFlush,
                         kRvaMovePathSetForcedFlush, gMiDoing ? 1 : 0, miVc ? 1 : 0, miMp ? 1 : 0,
                         kRvaVecCtrlNockBack, kRvaVecCtrlSetImpactNext);
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

    // 引擎原生瞬移体（VecCtrl RVA 0x11AA2D0，由 Doing@0x1027ED0 消费 pending 后调用）只做：
    //   CurFh(vc+0x28)=null → LastFh(vc+0x30)=null → Ap/Apl ← (int)x,(int)y →
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
        // 点飞悬空（0.1.50～0.1.91）：卸旧图台 CurFh + LastFh（只 detach，不挪人）。
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
    // 产品已统一 Impact（F5/F6）；fill+Doing 失效。
    x::runtime::LogWThrottled(91, 2000, "Teleport",
                              "TeleportNativeSkillCall refused (fill+Doing retired; use Impact)");
    return false;
}

bool TeleportNativeSkillCall(float landX, float landY, uint32_t plantFhId, bool snapStand) {
    (void)landX;
    (void)landY;
    (void)plantFhId;
    (void)snapStand;
    x::runtime::LogWThrottled(92, 2000, "Teleport",
                              "TeleportNativeSkillCall(land) refused (fill+Doing retired; use Impact)");
    return false;
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

namespace {

// MoveElem / MovePath 只读采证（与 movepath_flush_probe 同源偏移）
constexpr size_t kOffMpElemList = 0x30;
constexpr size_t kOffMpElemLast = 0x38;
constexpr size_t kOffElAttr = 0x10;
constexpr size_t kOffElX = 0x12;
constexpr size_t kOffElY = 0x14;
constexpr size_t kOffElVx = 0x16;
constexpr size_t kOffElVy = 0x18;
constexpr size_t kOffElMoveAction = 0x1A;
constexpr size_t kOffElFh = 0x1C;

const char* MovePathAttrName(uint8_t a) {
    switch (a) {
        case 0: return "Normal";
        case 1: return "Jump";
        case 2: return "Impact";
        case 3: return "Immediate";
        case 4: return "Teleport";
        case 5: return "HangOnBack";
        case 6: return "FlashJump";
        case 7: return "Assaulter";
        case 8: return "Assassination";
        case 9: return "Rush";
        case 10: return "StatChange";
        case 11: return "SitDown";
        case 12: return "MobPowerKnockBack";
        case 13: return "BackstepShot";
        case 14: return "StartFalldown";
        case 15: return "FallDown";
        case 16: return "StartWings";
        case 17: return "Wings";
        case 18: return "VerticalJump";
        case 19: return "CustomImpact";
        case 20: return "CombatStep";
        case 21: return "AranAdjust";
        case 22: return "MobToss";
        default: return "?";
    }
}

void FormatOneElem(char* out, size_t outN, void* el) {
    if (!out || outN == 0) return;
    if (!LooksLikeHeapPtr(el)) {
        snprintf(out, outN, "null");
        return;
    }
    const uint8_t a = ReadU8(el, kOffElAttr);
    snprintf(out, outN, "a=%u(%s) xy=(%d,%d) v=(%d,%d) ma=%u fh=%d", (unsigned)a,
             MovePathAttrName(a), (int)ReadI16(el, kOffElX), (int)ReadI16(el, kOffElY),
             (int)ReadI16(el, kOffElVx), (int)ReadI16(el, kOffElVy),
             (unsigned)ReadU8(el, kOffElMoveAction), (int)ReadI16(el, kOffElFh));
}

// 冲击后 Attr 不会立刻写：跟帧采样 List 尾 + _elemLast + Ap 速度。
void LogImpactAttrSnap(const char* tag, void* vc) {
    if (!tag) tag = "?";
    if (!LooksLikeHeapPtr(vc)) {
        x::runtime::LogI("Teleport", "impact_test attr %s no_vc", tag);
        return;
    }
    const double apx = ReadF64(vc, kOffVcApX);
    const double apy = ReadF64(vc, kOffVcApY);
    const double apvx = ReadF64(vc, kOffVcApVx);
    const double apvy = ReadF64(vc, kOffVcApVy);
    void* curFh = ReadPtr(vc, kOffVcCurFh);
    void* mp = ReadPtr(vc, kOffVcMovePath);
    if (!LooksLikeHeapPtr(mp)) {
        x::runtime::LogI("Teleport",
                         "impact_test attr %s ap=(%.1f,%.1f) v=(%.1f,%.1f) fh=%d no_mp", tag, apx,
                         apy, apvx, apvy, LooksLikeHeapPtr(curFh) ? 1 : 0);
        return;
    }
    x::runtime::il2cpp_container::Ensure();
    void* list = ReadPtr(mp, kOffMpElemList);
    void* arr = list ? ReadPtr(list, x::runtime::il2cpp_container::OffListItems()) : nullptr;
    int32_t size = list ? ReadI32(list, x::runtime::il2cpp_container::OffListSize()) : 0;
    if (size < 0) size = 0;
    const size_t dataOff = x::runtime::il2cpp_container::OffArrayData();

    char lastBuf[160];
    FormatOneElem(lastBuf, sizeof(lastBuf), ReadPtr(mp, kOffMpElemLast));

    // 只打尾部最多 4 段，避免刷屏；Attr 非 0 单独标出。
    char tail[512];
    int to = 0;
    const int take = size > 4 ? 4 : size;
    const int start = size - take;
    uint32_t seenMask = 0;
    for (int i = start; i < size && to < static_cast<int>(sizeof(tail)) - 120; ++i) {
        void* el =
            arr ? ReadPtr(arr, dataOff + static_cast<size_t>(i) * sizeof(void*)) : nullptr;
        if (!LooksLikeHeapPtr(el)) continue;
        const uint8_t a = ReadU8(el, kOffElAttr);
        if (a < 32) seenMask |= (1u << a);
        char one[96];
        FormatOneElem(one, sizeof(one), el);
        to += snprintf(tail + to, sizeof(tail) - static_cast<size_t>(to), "%s[%d]%s",
                       to ? " | " : "", i, one);
    }
    if (to == 0) {
        snprintf(tail, sizeof(tail), "(empty)");
    }

    char nonZero[96];
    int nz = 0;
    for (int a = 1; a < 32; ++a) {
        if (seenMask & (1u << a)) {
            nz += snprintf(nonZero + nz, sizeof(nonZero) - static_cast<size_t>(nz), "%s%d(%s)",
                           nz ? "," : "", a, MovePathAttrName(static_cast<uint8_t>(a)));
        }
    }
    if (nz == 0) snprintf(nonZero, sizeof(nonZero), "none");

    x::runtime::LogI(
        "Teleport",
        "impact_test attr %s n=%d ap=(%.1f,%.1f) v=(%.1f,%.1f) fh=%d non0={%s} last={%s} "
        "tail={%s}",
        tag, (int)size, apx, apy, apvx, apvy, LooksLikeHeapPtr(curFh) ? 1 : 0, nonZero, lastBuf,
        tail);
}

struct ImpactJob {
    int mode = 0;  // 0=NockBack 1=SetImpactNext
    int dir = 1;
    int vx = 400;
    int vy = 200;
    double dvx = 400.0;
    double dvy = 200.0;
    bool sampleAttr = true;  // 调试探针跟采；飞控关闭以免刷屏
    bool quietLog = false;
    bool ok = false;
    char why[48]{};
};

struct ImpactAttrSampleJob {
    char tag[24]{};
};

void ImpactAttrSampleFn(void* p) {
    auto* job = static_cast<ImpactAttrSampleJob*>(p);
    if (!job) return;
    EnsureTpFieldOff();
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
        x::runtime::LogI("Teleport", "impact_test attr %s no_local", job->tag);
        return;
    }
    void* vc = ReadPtr(lu, kOffVecCtrl);
    LogImpactAttrSnap(job->tag, vc);
}

bool CallNockBackSeh(void* vc, int dir, int vx, int vy) {
    if (!gNockBack || !LooksLikeHeapPtr(vc)) return false;
    __try {
        gNockBack(vc, dir, vx, vy, gMiNockBack);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetImpactNextSeh(void* vc, double vx, double vy) {
    auto* thunk = EnsureSetImpactNextThunk();
    if (!thunk || !gSetImpactNextRaw || !LooksLikeHeapPtr(vc)) return false;
    uint64_t vxBits = 0, vyBits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t), "double bits");
    memcpy(&vxBits, &vx, sizeof(vxBits));
    memcpy(&vyBits, &vy, sizeof(vyBits));
    __try {
        thunk(gSetImpactNextRaw, vc, vxBits, vyBits, gMiSetImpactNext);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ImpactJobFn(void* p) {
    auto* job = static_cast<ImpactJob*>(p);
    if (!job) return;
    if (!BindFns()) {
        snprintf(job->why, sizeof(job->why), "bind");
        return;
    }
    EnsureTpFieldOff();
    if (!world::IsPlayReady()) {
        snprintf(job->why, sizeof(job->why), "not_play_ready");
        return;
    }
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
        snprintf(job->why, sizeof(job->why), "no_local");
        return;
    }
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) {
        snprintf(job->why, sizeof(job->why), "no_vc");
        return;
    }
    if (job->sampleAttr) LogImpactAttrSnap("T0_pre", vc);
    if (job->mode == 0) {
        int dir = job->dir;
        if (dir != 1 && dir != -1) {
            // 未指定有效 dir：跟当前朝向推（faceLeft → -1）
            int faceLeft = 0;
            __try {
                const int ma =
                    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
                faceLeft = ma & 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                faceLeft = 0;
            }
            dir = faceLeft ? -1 : 1;
        }
        if (!CallNockBackSeh(vc, dir, job->vx, job->vy)) {
            snprintf(job->why, sizeof(job->why), "nockback_seh");
            return;
        }
        if (!job->quietLog) {
            x::runtime::LogI("Teleport", "impact_test A NockBack dir=%d vx=%d vy=%d", dir, job->vx,
                             job->vy);
        }
        if (job->sampleAttr) LogImpactAttrSnap("T0_postA", vc);
        job->ok = true;
        return;
    }
    if (!CallSetImpactNextSeh(vc, job->dvx, job->dvy)) {
        snprintf(job->why, sizeof(job->why), "setimpact_seh");
        return;
    }
    if (!job->quietLog) {
        x::runtime::LogI("Teleport", "impact_test B SetImpactNext vx=%.1f vy=%.1f", job->dvx,
                         job->dvy);
    }
    if (job->sampleAttr) LogImpactAttrSnap("T0_postB", vc);
    job->ok = true;
}

void SampleImpactAttrDelayed(const char* tag) {
    ImpactAttrSampleJob s{};
    snprintf(s.tag, sizeof(s.tag), "%s", tag ? tag : "?");
    if (!runtime::main_thread::InvokeAndWait(&ImpactAttrSampleFn, &s, 800,
                                            runtime::main_thread::JobPrio::Normal)) {
        x::runtime::LogW("Teleport", "impact_test attr %s pump_timeout", s.tag);
    }
}

bool FireImpactJob(ImpactJob& job) {
    // 已在泵线程（跟飞 STW+Impact 合并 job）：直接跑，禁止嵌套 InvokeAndWait 堵死帧。
    if (runtime::main_thread::IsOnPumpThread()) {
        ImpactJobFn(&job);
    } else if (!runtime::main_thread::InvokeAndWait(&ImpactJobFn, &job, kJobWaitMs,
                                                   runtime::main_thread::JobPrio::High)) {
        x::runtime::LogW("Teleport", "impact_test pump timeout mode=%d", job.mode);
        return false;
    }
    if (!job.ok) {
        if (!job.quietLog) {
            x::runtime::LogW("Teleport", "impact_test fail mode=%d why=%s", job.mode,
                             job.why[0] ? job.why : "?");
        }
        return false;
    }
    // Attr 在 MakeMovePath 后续帧才出现：后台 T+50/150/400 跟采（不堵 ApplyControl）。
    if (job.sampleAttr) {
        const int mode = job.mode;
        std::thread([mode]() {
            Sleep(50);
            SampleImpactAttrDelayed("T50");
            Sleep(100);
            SampleImpactAttrDelayed("T150");
            Sleep(250);
            SampleImpactAttrDelayed("T400");
            x::runtime::LogI(
                "Teleport",
                "impact_test attr done mode=%d (expect Normal/Jump/Fall; not Teleport)", mode);
        }).detach();
    }
    return true;
}

}  // namespace

bool FireImpactNockBackTest(int dir, int vx, int vy) {
    ImpactJob job{};
    job.mode = 0;
    job.dir = dir;
    job.vx = vx;
    job.vy = vy;
    return FireImpactJob(job);
}

bool FireImpactSetNextTest(double vx, double vy) {
    ImpactJob job{};
    job.mode = 1;
    job.dvx = vx;
    job.dvy = vy;
    return FireImpactJob(job);
}

namespace {
// P0 标定锚点（平地 BIN）：vx=400,vy=200 → Δx≈100 px / ~400ms → |Δx|→vx ≈ |Δx|*4
constexpr int kImpactHopVyDefault = 200;
constexpr int kImpactHopVxMin = 80;
constexpr int kImpactHopVxMax = 800;
constexpr uint32_t kImpactHopCooldownMs = 400;
constexpr int kImpactHopDxScale = 4;  // vx = |Δx| * scale

std::atomic<DWORD> gLastImpactHopMs{0};

int MapDeltaXToVx(int absDx) {
    if (absDx < 0) absDx = -absDx;
    int vx = absDx * kImpactHopDxScale;
    if (vx < kImpactHopVxMin) vx = kImpactHopVxMin;
    if (vx > kImpactHopVxMax) vx = kImpactHopVxMax;
    return vx;
}
}  // namespace

bool ImpactHopDeltaX(int deltaX, ImpactHopOpts opts) {
    if (deltaX == 0) {
        x::runtime::LogW("Teleport", "impact hop refuse dx=0");
        return false;
    }
    const DWORD now = GetTickCount();
    const DWORD last = gLastImpactHopMs.load(std::memory_order_acquire);
    if (last != 0 && (now - last) < kImpactHopCooldownMs) {
        x::runtime::LogW("Teleport", "impact hop refuse cooldown rem=%u",
                         (unsigned)(kImpactHopCooldownMs - (now - last)));
        return false;
    }
    if (!opts.force && !x::features::invuln::IsEnabled()) {
        x::runtime::LogW("Teleport", "impact hop refuse invuln_off (use force for debug)");
        return false;
    }
    const int dir = deltaX > 0 ? 1 : -1;
    const int absDx = deltaX > 0 ? deltaX : -deltaX;
    const int vx = MapDeltaXToVx(absDx);
    const int vy = opts.vy > 0 ? opts.vy : kImpactHopVyDefault;
    x::runtime::LogI("Teleport", "impact hop fire dx=%d vx=%d vy=%d force=%d invuln=%d", deltaX, vx,
                     vy, opts.force ? 1 : 0, x::features::invuln::IsEnabled() ? 1 : 0);
    if (!FireImpactNockBackTest(dir, vx, vy)) return false;
    gLastImpactHopMs.store(now, std::memory_order_release);
    return true;
}

namespace {

bool ApplyImpactImpulseQuiet(ImpactRoute route, double vx, double vy) {
    ImpactJob job{};
    job.sampleAttr = false;
    job.quietLog = true;
    if (route == ImpactRoute::NockBack) {
        job.mode = 0;
        job.dir = vx >= 0.0 ? 1 : -1;
        job.vx = static_cast<int>(std::lround(std::fabs(vx)));
        job.vy = static_cast<int>(std::lround(vy));
        if (job.vx < 1) job.vx = 1;
    } else {
        job.mode = 1;
        job.dvx = vx;
        job.dvy = vy;
    }
    return FireImpactJob(job);
}

double ClampImpactSpeed(double v, double vmax) {
    const double lo = static_cast<double>(kImpactHopVxMin);
    const double hi = vmax > lo ? vmax : static_cast<double>(kImpactHopVxMax);
    const double a = std::fabs(v);
    if (a < lo) {
        return (v < 0.0 ? -1.0 : 1.0) * lo;
    }
    if (a > hi) {
        return (v < 0.0 ? -1.0 : 1.0) * hi;
    }
    return v;
}

double Smoothstep01(double edge0, double edge1, double x) {
    if (edge1 <= edge0) return x >= edge1 ? 1.0 : 0.0;
    double t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

bool ImpactImpulseToward(float worldX, float worldY, ImpactRoute route, ImpactTowardOpts opts) {
    if (!std::isfinite(worldX) || !std::isfinite(worldY)) return false;
    if (!opts.force && !x::features::invuln::IsEnabled()) {
        if (!opts.quietLog) {
            x::runtime::LogW("Teleport", "impact toward refuse invuln_off");
        }
        return false;
    }
    if (!BindFns()) return false;
    EnsureTpFieldOff();
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    const double apX = ReadF64(vc, kOffVcApX);
    const double apY = ReadF64(vc, kOffVcApY);
    if (!std::isfinite(apX) || !std::isfinite(apY)) return false;

    const double rawX = static_cast<double>(worldX);
    const double rawY = static_cast<double>(worldY);
    double aimX = rawX;
    double aimY = rawY;
    double errRaw = std::sqrt((rawX - apX) * (rawX - apX) + (rawY - apY) * (rawY - apY));
    double drive = 1.0;

    // 动态超前（飞路径当前关闭；opts.adaptive+leadSec 才启用）。
    if (opts.adaptive && opts.leadSec > 0.f) {
        static double sPrevRawX = 0.0;
        static double sPrevRawY = 0.0;
        static DWORD sPrevRawMs = 0;
        const DWORD now = GetTickCount();
        if (sPrevRawMs != 0 && now > sPrevRawMs) {
            const double dt = static_cast<double>(now - sPrevRawMs) * 0.001;
            if (dt > 0.001 && dt < 0.25) {
                const double mx = (rawX - sPrevRawX) / dt;
                const double my = (rawY - sPrevRawY) / dt;
                const double tSpeed = std::sqrt(mx * mx + my * my);
                const double leadK =
                    Smoothstep01(70.0, 320.0, errRaw) * Smoothstep01(180.0, 1600.0, tSpeed);
                double leadDist = tSpeed * static_cast<double>(opts.leadSec) * leadK;
                const double leadCap = errRaw * 0.40;
                if (leadDist > leadCap) leadDist = leadCap;
                if (leadDist > 1.0 && tSpeed > 1e-3) {
                    aimX = rawX + mx / tSpeed * leadDist;
                    aimY = rawY + my / tSpeed * leadDist;
                }
            }
        }
        sPrevRawX = rawX;
        sPrevRawY = rawY;
        sPrevRawMs = now;
    }

    double dx = aimX - apX;
    double dy = aimY - apY;
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < static_cast<double>(opts.minSegPx)) return false;

    double maxSeg = opts.maxSegPx > 0.f ? static_cast<double>(opts.maxSegPx) : 160.0;
    double vmax =
        opts.maxSpeed > 0.f ? static_cast<double>(opts.maxSpeed) : static_cast<double>(kImpactHopVxMax);
    double scale = opts.speedScale > 0.f ? static_cast<double>(opts.speedScale)
                                         : static_cast<double>(kImpactHopDxScale);

    if (opts.adaptive) {
        // 远距拉满天花板；近距软刹：少推一段，留余量给引擎滑行，防冲过再折返。
        const double farSeg = maxSeg;
        const double farV = vmax;
        const double farScale = scale;
        maxSeg = 80.0 + (farSeg - 80.0) * Smoothstep01(40.0, 700.0, errRaw);
        vmax = 500.0 + (farV - 500.0) * Smoothstep01(40.0, 900.0, errRaw);
        scale = 3.0 + (farScale - 3.0) * Smoothstep01(40.0, 650.0, errRaw);
        // drive：近 0.32 / 远 0.78——故意不满推残差
        drive = 0.32 + 0.46 * Smoothstep01(40.0, 520.0, errRaw);
        if (maxSeg > farSeg) maxSeg = farSeg;
        if (vmax > farV) vmax = farV;
        if (scale > farScale) scale = farScale;
    }

    if (len > maxSeg && len > 1e-3) {
        const double s = maxSeg / len;
        dx *= s;
        dy *= s;
        len = maxSeg;
    }
    // P0 表：|Δ|→|v|≈|Δ|*scale；adaptive 再乘 drive 欠驱动
    double speed = len * scale * drive;
    if (speed < static_cast<double>(kImpactHopVxMin)) speed = static_cast<double>(kImpactHopVxMin);
    if (speed > vmax) speed = vmax;
    double vx = speed * (dx / len);
    double vy = speed * (dy / len);
    vx = ClampImpactSpeed(vx, vmax);
    // vy 允许 0（纯水平）；非 0 时同样夹紧幅值
    if (std::fabs(vy) > 1e-6) vy = ClampImpactSpeed(vy, vmax);
    // 战斗空中：额外压 |vy|，避免 fh-ban 下竖直过冲甩出地图。
    if (opts.maxAbsVy > 0.f) {
        const double cap = static_cast<double>(opts.maxAbsVy);
        if (vy > cap) vy = cap;
        if (vy < -cap) vy = -cap;
    }

    auto logToward = [&]() {
        x::runtime::LogI(
            "Teleport",
            "impact toward route=%u to=(%.0f,%.0f) ap=(%.0f,%.0f) v=(%.0f,%.0f) err=%.0f drive=%.2f "
            "scale=%.1f",
            static_cast<unsigned>(route), aimX, aimY, apX, apY, vx, vy, errRaw, drive, scale);
    };
    if (!opts.quietLog) {
        logToward();
    } else {
        // 跟飞安静：~5Hz 抽样，避免每发打盘拖泵。
        static DWORD sLastQuietTowardLogMs = 0;
        const DWORD nowLog = GetTickCount();
        if (!sLastQuietTowardLogMs || (nowLog - sLastQuietTowardLogMs) >= 200) {
            sLastQuietTowardLogMs = nowLog;
            logToward();
        }
    }
    if (!ApplyImpactImpulseQuiet(route, vx, vy)) return false;
    return true;
}

bool QueryFlightState(FlightState& out) {
    out = FlightState{};
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    const double x = ReadF64(vc, kOffVcApX);
    const double y = ReadF64(vc, kOffVcApY);
    const double vx = ReadF64(vc, kOffVcApVx);
    const double vy = ReadF64(vc, kOffVcApVy);
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    out.x = static_cast<float>(x);
    out.y = static_cast<float>(y);
    out.vx = std::isfinite(vx) ? static_cast<float>(vx) : 0.f;
    out.vy = std::isfinite(vy) ? static_cast<float>(vy) : 0.f;
    out.onFh = LooksLikeHeapPtr(ReadPtr(vc, kOffVcCurFh));
    out.ok = true;
    return true;
}

bool ImpactSetVelocity(float vx, float vy, ImpactRoute route, ImpactVelOpts opts) {
    if (!std::isfinite(vx) || !std::isfinite(vy)) return false;
    if (!opts.force && !x::features::invuln::IsEnabled()) return false;
    if (!BindFns()) return false;
    EnsureTpFieldOff();
    double cx = static_cast<double>(vx);
    double cy = static_cast<double>(vy);
    const double capX = opts.maxAbsVx > 0.f ? static_cast<double>(opts.maxAbsVx) : 900.0;
    const double capY = opts.maxAbsVy > 0.f ? static_cast<double>(opts.maxAbsVy) : 420.0;
    if (cx > capX) cx = capX;
    if (cx < -capX) cx = -capX;
    if (cy > capY) cy = capY;
    if (cy < -capY) cy = -capY;
    const double minAbs = opts.minAbs > 0.f ? static_cast<double>(opts.minAbs) : 0.0;
    if (std::fabs(cx) < minAbs && std::fabs(cy) < minAbs) return false;
    if (!opts.quietLog) {
        x::runtime::LogI("Teleport", "impact vel route=%u v=(%.0f,%.0f)",
                         static_cast<unsigned>(route), cx, cy);
    }
    return ApplyImpactImpulseQuiet(route, cx, cy);
}

}  // namespace x::features::ports::teleport
