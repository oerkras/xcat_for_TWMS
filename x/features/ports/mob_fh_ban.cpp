// mob_fh_ban — VecCtrlMob klass 上拦 CollisionDetect / Float；白名单实例才禁挂。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_fh_ban.h"

#include "fly_fh_ban.h"
#include "mob_pool_port.h"
#include "player_combat_port.h"
#include "teleport_port.h"
#include "../../../common/xcat_payload_control.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace x::features::ports::mob_fh_ban {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr size_t kFbUserVecCtrl = 0x50;
constexpr size_t kFbVcCurFh = 0x28;
constexpr size_t kFbVcLastFh = 0x30;
constexpr size_t kFbVcLadderOrRope = 0x40;
constexpr size_t kFbVcAp = 0x98;
constexpr size_t kFbVcApY = 0xA0;
constexpr size_t kKlassOffWuaMethodPtr = 0x1E8;  // Slot 11 WorkUpdateActive
constexpr size_t kKlassOffCdMethodPtr = 0x208;   // Slot 13 CollisionDetectWalk
constexpr size_t kKlassOffCdfMethodPtr = 0x218;  // Slot 14 CollisionDetectFloat
// dump.cs.restored.C · VecCtrlMob（RVA 仅 fallback；安装走 hash）
constexpr uint32_t kRvaMobWua = 0x11DF260;
constexpr uint32_t kRvaBaseWua = 0x11C5510;
constexpr uint32_t kRvaInspect = 0x11DBAB0;
constexpr uint32_t kRvaCtrlStop = 0x11DE6B0;
constexpr uint32_t kRvaCtrlMove = 0x11DFEB0;
constexpr uint32_t kRvaCtrlJump = 0x11E1100;
constexpr uint32_t kRvaCtrlFly = 0x11E8080;
constexpr char kHashWorkUpdateActive[] =
    "cd769ef8216fb484c02c0a08d78ec389a9b88ead83f9323f50d7aa26578497e";
constexpr char kHashInspectUpdateActive[] =
    "d1efee0dea25b6293b6455c5f1256daec2dfe06fd42855f82fabff7246e06b1";
constexpr char kHashCtrlStop[] =
    "c9e385cfbdebe4c7e3fe11062f63ff09014a4da7af4cc6eb5a1df6cb99bc59c";
constexpr char kHashCtrlMove[] =
    "e5220e282d4f0a2223e3e418de7ed57e2ca9293452d00de6b272be674e3e81a";
constexpr char kHashCtrlJump[] =
    "d4cda78c5f174760c6aa00311b7a3e2f46a3c137293f79d849b01aaf4d9b8fa";
constexpr char kHashCtrlFly[] =
    "d89b0988c604ddfec7678c02ed07f661cc08188203c66897ef4ced3b1d6aff7";

constexpr int kMaxBan = 64;
constexpr DWORD kArmTimeoutMs = 8000;
// ≥5X 抄 F5 10X：死拍 err/T + 对站点 err/H=0.15。尸体/池槽复用不当场拉。
constexpr float kKp = 7.f;
constexpr float kKpSettle = 10.f;
constexpr float kDead = 6.f;
constexpr float kSettleDead = 1.f;
constexpr float kSettleErr = 16.f;
constexpr float kSettleMaxVx = 160.f;
constexpr float kSettleMaxVy = 360.f;
constexpr float kBaseCruise = 620.f;
constexpr float kBaseStation = 480.f;
constexpr float kBaseHold = 360.f;
constexpr float kCruiseR = 140.f;
constexpr float kStationR = 28.f;
constexpr float kGravityPerStep = 60.f;
constexpr float kPhysicsStepMs = 30.f;
constexpr float kMaxTrimWindowMs = 50.f;
constexpr float kStickCreepPx = 8.f;
constexpr float kStickStillV = 50.f;
constexpr float kFullFireScale = 5.f;
constexpr float kFullFireEps = 0.05f;
constexpr float kFullFireApproachBrakeSec = 0.15f;
constexpr float kDeadzoneCoastVy = 80.f;
constexpr float kIntentCeil = 4800.f;
constexpr float kMaxCmd = 4800.f;
constexpr float kScaleMin = 0.25f;
constexpr float kScaleMax = 10.f;
// 聚拢点：朝向面前站距 X + 相对人 AbsPos 的 Y（更大 Y = 更高）。悬停不贴台。
// 未下发前与面板默认一致（自定义落点 29 / 9）。
constexpr float kGatherDefaultOffX = static_cast<float>(xcat::kMobGatherStandOffXDefault);
constexpr float kGatherDefaultOffY = static_cast<float>(xcat::kMobGatherStandOffYDefault);

using FnDetect = uint8_t(__fastcall*)(void* self, void* a2, void* a3, uint8_t flag);
using FnWua = uint8_t(__fastcall*)(void* self, int tElapse, const void* method);
using FnVoid0 = void(__fastcall*)(void* self, const void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<bool> gInstalled{false};
void* gKlass = nullptr;
void** gSlotCd = nullptr;
void** gSlotCdf = nullptr;
void** gSlotWua = nullptr;
FnDetect gOrigCd = nullptr;
FnDetect gOrigCdf = nullptr;
FnWua gOrigMobWua = nullptr;
FnWua gOrigBaseWua = nullptr;
void* gMiBaseWua = nullptr;
void* gMiMobWua = nullptr;

struct AiMiHook {
    const char* tag;
    const char* hash;
    const char* plain;
    uint32_t rva;
    void* hook;
    void* orig;
    MethodInfoHead* mi;
};

void __fastcall HookInspect(void* self, const void* method);
void __fastcall HookCtrlStop(void* self, const void* method);
void __fastcall HookCtrlMove(void* self, const void* method);
void __fastcall HookCtrlJump(void* self, const void* method);
void __fastcall HookCtrlFly(void* self, const void* method);

AiMiHook gAi[] = {
    {"Inspect", kHashInspectUpdateActive, "InspectUpdateActive", kRvaInspect,
     reinterpret_cast<void*>(&HookInspect), nullptr, nullptr},
    {"Stop", kHashCtrlStop, "CtrlUpdateActiveStop", kRvaCtrlStop,
     reinterpret_cast<void*>(&HookCtrlStop), nullptr, nullptr},
    {"Move", kHashCtrlMove, "CtrlUpdateActiveMove", kRvaCtrlMove,
     reinterpret_cast<void*>(&HookCtrlMove), nullptr, nullptr},
    {"Jump", kHashCtrlJump, "CtrlUpdateActiveJump", kRvaCtrlJump,
     reinterpret_cast<void*>(&HookCtrlJump), nullptr, nullptr},
    {"Fly", kHashCtrlFly, "CtrlUpdateActiveFly", kRvaCtrlFly,
     reinterpret_cast<void*>(&HookCtrlFly), nullptr, nullptr},
};
constexpr int kAiN = static_cast<int>(sizeof(gAi) / sizeof(gAi[0]));

constexpr float kPoolJumpPx = 800.f;

std::atomic<uintptr_t> gBanVc[kMaxBan]{};
std::atomic<uintptr_t> gBanMob[kMaxBan]{};
std::atomic<int32_t> gBanId[kMaxBan]{};
std::atomic<DWORD> gBanTick[kMaxBan]{};
std::atomic<uint32_t> gBanTx[kMaxBan]{};
std::atomic<uint32_t> gBanTy[kMaxBan]{};
std::atomic<uint32_t> gBanLastX[kMaxBan]{};
std::atomic<uint32_t> gBanLastY[kMaxBan]{};
std::atomic<uint8_t> gBanApOk[kMaxBan]{};
std::atomic<uint8_t> gBanAim[kMaxBan]{};
std::atomic<DWORD> gBanLastMs[kMaxBan]{};
std::atomic<DWORD> gLastAimMs{0};
std::atomic<unsigned> gLastAimDt{0};
float gStickX = 0.f;
float gStickY = 0.f;
bool gStickOk = false;
std::atomic<float> gSpeedScale{2.f};
std::atomic<uint8_t> gAntiJitter{1};
std::atomic<uint32_t> gLeadVxBits{0};
std::atomic<uint32_t> gLeadVyBits{0};
std::atomic<float> gOffX{kGatherDefaultOffX};
std::atomic<float> gOffY{kGatherDefaultOffY};
std::atomic<uint32_t> gOffGen{0};
std::atomic<int> gMaxArmed{kMaxBan};
std::atomic<uint32_t> gWipeGen{0};
std::atomic<DWORD> gArmTimeoutMs{kArmTimeoutMs};
std::atomic<float> gKp{kKp};
std::atomic<float> gDead{kDead};
std::atomic<float> gCruiseR{kCruiseR};
std::atomic<float> gStationR{kStationR};
std::atomic<float> gMaxCmdLive{kMaxCmd};
std::atomic<float> gGravity{kGravityPerStep};
std::atomic<float> gStickCreep{kStickCreepPx};
std::atomic<float> gStickStillV{kStickStillV};
std::atomic<float> gCruiseV{kBaseCruise};
std::atomic<float> gStationV{kBaseStation};
std::atomic<float> gHoldV{kBaseHold};
std::atomic<float> gSettleErr{kSettleErr};
std::atomic<float> gKpSettle{kKpSettle};
std::atomic<float> gBrakeSec{kFullFireApproachBrakeSec};
std::atomic<float> gCoastVy{kDeadzoneCoastVy};

uint32_t FToBits(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    return u;
}

float BitsToF(uint32_t u) {
    float f = 0.f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

void ClearSlot(int i, const char* why) {
    if (i < 0 || i >= kMaxBan) return;
    const int32_t id = gBanId[i].load(std::memory_order_acquire);
    gBanVc[i].store(0, std::memory_order_release);
    gBanMob[i].store(0, std::memory_order_release);
    gBanId[i].store(0, std::memory_order_release);
    gBanTick[i].store(0, std::memory_order_release);
    gBanTx[i].store(0, std::memory_order_release);
    gBanTy[i].store(0, std::memory_order_release);
    gBanLastX[i].store(0, std::memory_order_release);
    gBanLastY[i].store(0, std::memory_order_release);
    gBanApOk[i].store(0, std::memory_order_release);
    gBanAim[i].store(0, std::memory_order_release);
    gBanLastMs[i].store(0, std::memory_order_release);
    if (why && why[0] && id != 0) {
        x::runtime::LogI("MobFhBan", "disarm id=%d why=%s", id, why);
    }
}

float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void WritePtrSeh(void* base, size_t off, void* val) {
    if (!base) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off) = val;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* ReadPtrSeh(void* base, size_t off) {
    if (!base) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

double ReadF64Seh(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

void ClearFhOnVc(void* vc) {
    if (!LooksLikeHeapPtr(vc)) return;
    WritePtrSeh(vc, kFbVcCurFh, nullptr);
    WritePtrSeh(vc, kFbVcLastFh, nullptr);
    WritePtrSeh(vc, kFbVcLadderOrRope, nullptr);
}

bool PatchSlot(void** slot, void* hook, FnDetect* outOrig) {
    if (!slot || !hook || !outOrig) return false;
    void* cur = nullptr;
    __try {
        cur = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!cur) return false;
    if (*outOrig && reinterpret_cast<void*>(*outOrig) != cur && cur != hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        if (!*outOrig) *outOrig = reinterpret_cast<FnDetect>(cur);
        *slot = hook;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return ok;
}

void RestoreSlot(void** slot, FnDetect orig) {
    if (!slot || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    __try {
        if (*slot != reinterpret_cast<void*>(orig)) *slot = reinterpret_cast<void*>(orig);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

bool PatchPtr(void** slot, void* hook, void** outOrig) {
    if (!slot || !hook || !outOrig) return false;
    void* cur = nullptr;
    __try {
        cur = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!cur) return false;
    if (*outOrig && *outOrig != cur && cur != hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        if (!*outOrig) *outOrig = cur;
        *slot = hook;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return ok;
}

void RestorePtr(void** slot, void* orig) {
    if (!slot || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    __try {
        if (*slot != orig) *slot = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    *outOrig = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
    mi->methodPointer = hook;
    if (mi->virtualMethodPointer == *outOrig || mi->virtualMethodPointer == nullptr)
        mi->virtualMethodPointer = hook;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return true;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    void* cur = mi->methodPointer;
    mi->methodPointer = orig;
    if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

void* ReadVidMethod(void* klass, size_t methodPtrOff) {
    return ReadPtrSeh(klass, methodPtrOff + sizeof(void*));
}

void* ParentKlass(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classParent) return nullptr;
    void* parent = nullptr;
    __try {
        parent = e.classParent(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.parent");
        parent = nullptr;
    }
    return parent;
}

int FindBanIndex(void* self) {
    if (!self) return -1;
    const uintptr_t p = reinterpret_cast<uintptr_t>(self);
    for (int i = 0; i < kMaxBan; ++i) {
        if (gBanVc[i].load(std::memory_order_acquire) == p) return i;
    }
    return -1;
}

void RunAiOrig(int idx, void* self, const void* method) {
    if (idx < 0 || idx >= kAiN) return;
    auto orig = reinterpret_cast<FnVoid0>(gAi[idx].orig);
    if (!orig) return;
    orig(self, method);
}

void __fastcall HookInspect(void* self, const void* method) {
    __try {
        if (FindBanIndex(self) >= 0) return;
        RunAiOrig(0, self, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.Inspect");
    }
}

void __fastcall HookCtrlStop(void* self, const void* method) {
    __try {
        if (FindBanIndex(self) >= 0) return;
        RunAiOrig(1, self, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.Stop");
    }
}

void __fastcall HookCtrlMove(void* self, const void* method) {
    __try {
        if (FindBanIndex(self) >= 0) return;
        RunAiOrig(2, self, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.Move");
    }
}

void __fastcall HookCtrlJump(void* self, const void* method) {
    __try {
        if (FindBanIndex(self) >= 0) return;
        RunAiOrig(3, self, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.Jump");
    }
}

void __fastcall HookCtrlFly(void* self, const void* method) {
    __try {
        if (FindBanIndex(self) >= 0) return;
        RunAiOrig(4, self, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.Fly");
    }
}

uint8_t __fastcall HookWorkUpdateActive(void* self, int tElapse, const void* method) {
    __try {
        if (FindBanIndex(self) >= 0 && gOrigBaseWua) {
            return gOrigBaseWua(self, tElapse, gMiBaseWua ? gMiBaseWua : method);
        }
        if (gOrigMobWua) return gOrigMobWua(self, tElapse, method ? method : gMiMobWua);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobFhBan.WUA");
    }
    return 0;
}

float ImpactDeltaFor(float vt, float v) {
    if (vt * v > 0.f && std::fabs(vt) > std::fabs(v)) return vt;
    return vt - v;
}

float GravityLoss(unsigned sinceMs) {
    float ms = static_cast<float>(sinceMs);
    if (ms < 1.f) ms = 1.f;
    if (ms > kMaxTrimWindowMs) ms = kMaxTrimWindowMs;
    return gGravity.load(std::memory_order_acquire) * ms / kPhysicsStepMs;
}

float ReachableV(float vt, float v, float bias, float cmax) {
    const float av = std::fabs(v);
    const float reach = av > cmax ? av : cmax;
    float lo = v - bias - cmax;
    float hi = v - bias + cmax;
    if (lo < -reach) lo = -reach;
    if (hi > reach) hi = reach;
    if (lo > hi) return vt;
    return vt < lo ? lo : (vt > hi ? hi : vt);
}

void BrakeToRoom(float err, float dead, float H, float* desired) {
    if (!desired || std::fabs(err) <= dead || H < 1e-4f) return;
    const float room = err / H;
    if (err > 0.f) {
        if (*desired > room) *desired = room;
    } else if (*desired < room) {
        *desired = room;
    }
}

void ComputeSetVelocityImpl(float x, float y, float vx, float vy, float aimX, float aimY,
                            unsigned sinceMs, float* setVx, float* setVy) {
    const bool jitter = gAntiJitter.load(std::memory_order_acquire) != 0;
    const float scale = ClampF(gSpeedScale.load(std::memory_order_acquire), kScaleMin, kScaleMax);
    const float leadVx = BitsToF(gLeadVxBits.load(std::memory_order_acquire));
    const float leadVy = BitsToF(gLeadVyBits.load(std::memory_order_acquire));
    const float errX = aimX - x;
    const float errY = aimY - y;
    const float ad = std::sqrt(errX * errX + errY * errY);
    const float settleErr = gSettleErr.load(std::memory_order_acquire);
    const bool settleHover =
        jitter && std::fabs(errX) <= settleErr && std::fabs(errY) <= settleErr;

    float desiredVx = leadVx;
    float desiredVy = leadVy;
    const float cruiseR = gCruiseR.load(std::memory_order_acquire);
    const float stationR = gStationR.load(std::memory_order_acquire);
    const bool fullFire = scale + kFullFireEps >= kFullFireScale;
    if (settleHover) {
        const float kpSettle = gKpSettle.load(std::memory_order_acquire);
        if (std::fabs(errX) > kSettleDead) {
            desiredVx = ClampF(leadVx + kpSettle * errX, -kSettleMaxVx, kSettleMaxVx);
        }
        if (std::fabs(errY) > kSettleDead) {
            desiredVy = ClampF(leadVy + kpSettle * errY, -kSettleMaxVy, kSettleMaxVy);
        }
    } else {
        const float kp = gKp.load(std::memory_order_acquire);
        const float dead = gDead.load(std::memory_order_acquire);
        if (std::fabs(errX) > dead) desiredVx += kp * errX;
        if (std::fabs(errY) > dead) desiredVy += kp * errY;
        if (fullFire) {
            float Tsec = static_cast<float>(sinceMs) * 0.001f;
            if (Tsec < 0.020f) Tsec = 0.020f;
            if (std::fabs(errX) > dead) desiredVx = leadVx + errX / Tsec;
            if (std::fabs(errY) > dead) desiredVy = leadVy + errY / Tsec;
            const float brake = gBrakeSec.load(std::memory_order_acquire);
            BrakeToRoom(errX, dead, brake, &desiredVx);
            BrakeToRoom(errY, dead, brake, &desiredVy);
        }
        if (std::fabs(errY) <= dead &&
            std::fabs(vy) > gCoastVy.load(std::memory_order_acquire))
            desiredVy = 0.f;
    }

    float cap = gHoldV.load(std::memory_order_acquire) * scale;
    if (ad > cruiseR) {
        cap = gCruiseV.load(std::memory_order_acquire) * scale;
    } else if (ad > stationR) {
        cap = gStationV.load(std::memory_order_acquire) * scale;
    }
    if (fullFire) cap = kIntentCeil;
    if (cap > kIntentCeil) cap = kIntentCeil;
    const float mag = std::sqrt(desiredVx * desiredVx + desiredVy * desiredVy);
    if (mag > cap && mag > 1e-3f) {
        desiredVx *= cap / mag;
        desiredVy *= cap / mag;
    }

    const float trim = GravityLoss(sinceMs);
    const float grav = gGravity.load(std::memory_order_acquire);
    const float cmd = gMaxCmdLive.load(std::memory_order_acquire);
    const float ff = trim * 0.5f + grav * 0.5f;
    desiredVx = ReachableV(desiredVx, vx, 0.f, cmd);
    desiredVy = ReachableV(desiredVy, vy, ff, cmd);
    if (setVx) *setVx = desiredVx;
    if (setVy) *setVy = desiredVy + ff;
}

void ResolveGatherStand(float px, float py, int ma, float* outX, float* outY) {
    const int faceLeft = (ma >= 0) ? (ma & 1) : 0;
    const float offX = gOffX.load(std::memory_order_acquire);
    const float offY = gOffY.load(std::memory_order_acquire);
    // 悬停落点：朝向相对站距，不 SnapStandAt（贴台会把点拽回脚下 / 窜到下层台）。
    // 正 offX = 面前，负 offX = 背后。AbsPos：更大 Y = 更高。
    *outX = px + (faceLeft ? -offX : offX);
    *outY = py + offY;  // AbsPos：更大 Y = 更高
}

void ResolveGatherStandCached(float px, float py, int ma, float* outX, float* outY) {
    static float sPx = 0.f, sPy = 0.f, sGx = 0.f, sGy = 0.f;
    static int sMa = -999;
    static uint32_t sGen = 0;
    static bool sOk = false;
    const uint32_t gen = gOffGen.load(std::memory_order_acquire);
    if (sOk && ma == sMa && gen == sGen && std::fabs(px - sPx) < 0.25f &&
        std::fabs(py - sPy) < 0.25f) {
        *outX = sGx;
        *outY = sGy;
        return;
    }
    ResolveGatherStand(px, py, ma, &sGx, &sGy);
    sPx = px;
    sPy = py;
    sMa = ma;
    sGen = gen;
    sOk = true;
    *outX = sGx;
    *outY = sGy;
}

void TickPlayerAimImpl(float x, float y, float vx, float vy, int ma, float* outX, float* outY) {
    const bool jitter = gAntiJitter.load(std::memory_order_acquire) != 0;
    const float playerSp = std::sqrt(vx * vx + vy * vy);
    const bool flying =
        fly_fh_ban::IsBanActive() || playerSp >= gStickStillV.load(std::memory_order_acquire);
    float px = x;
    float py = y;
    const float creep = gStickCreep.load(std::memory_order_acquire);
    if (jitter && !flying && gStickOk && std::fabs(x - gStickX) <= creep &&
        std::fabs(y - gStickY) <= creep) {
        px = gStickX;
        py = gStickY;
    } else {
        gStickX = x;
        gStickY = y;
        gStickOk = true;
    }
    if (!jitter || flying) {
        px = x;
        py = y;
    }
    float ax = px;
    float ay = py;
    ResolveGatherStandCached(px, py, ma, &ax, &ay);
    gLeadVxBits.store(FToBits(vx), std::memory_order_release);
    gLeadVyBits.store(FToBits(vy), std::memory_order_release);
    const uint32_t bx = FToBits(ax);
    const uint32_t by = FToBits(ay);
    for (int i = 0; i < kMaxBan; ++i) {
        if (!gBanVc[i].load(std::memory_order_acquire)) continue;
        if (!gBanAim[i].load(std::memory_order_acquire)) continue;
        gBanTx[i].store(bx, std::memory_order_release);
        gBanTy[i].store(by, std::memory_order_release);
    }
    const DWORD now = GetTickCount();
    const DWORD prev = gLastAimMs.exchange(now, std::memory_order_acq_rel);
    if (prev && now != prev) {
        gLastAimDt.store(now - prev, std::memory_order_release);
    }
    if (outX) *outX = ax;
    if (outY) *outY = ay;
}

void ApplyVtolOnVc(void* vc) {
    const int i = FindBanIndex(vc);
    if (i < 0) return;
    ClearFhOnVc(vc);
    if (!gBanAim[i].load(std::memory_order_acquire)) return;
    void* mob = reinterpret_cast<void*>(gBanMob[i].load(std::memory_order_acquire));
    const int32_t id = gBanId[i].load(std::memory_order_acquire);
    const char* keepWhy = "";
    if (!x::features::ports::mob::StillSameLiveMob(mob, id, &keepWhy)) {
        ClearSlot(i, keepWhy && keepWhy[0] ? keepWhy : "dead");
        return;
    }
    teleport::FlightState st{};
    if (teleport::QueryFlightState(st) && st.ok) {
        TickPlayerAimImpl(st.x, st.y, st.vx, st.vy, st.ma, nullptr, nullptr);
    }
    const float tx = BitsToF(gBanTx[i].load(std::memory_order_acquire));
    const float ty = BitsToF(gBanTy[i].load(std::memory_order_acquire));
    const float x = static_cast<float>(ReadF64Seh(vc, kFbVcAp));
    const float y = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 8));
    const float vx = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 0x10));
    const float vy = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 0x18));
    if (std::fabs(x) + std::fabs(y) <= 1.f) return;
    if (gBanApOk[i].load(std::memory_order_acquire)) {
        const float lx = BitsToF(gBanLastX[i].load(std::memory_order_acquire));
        const float ly = BitsToF(gBanLastY[i].load(std::memory_order_acquire));
        const float jx = x - lx;
        const float jy = y - ly;
        if (jx * jx + jy * jy >= kPoolJumpPx * kPoolJumpPx) {
            ClearSlot(i, "pool_jump");
            return;
        }
    }
    gBanTick[i].store(GetTickCount(), std::memory_order_release);
    const DWORD now = GetTickCount();
    const DWORD last = gBanLastMs[i].load(std::memory_order_acquire);
    unsigned since =
        last ? static_cast<unsigned>(now - last) : static_cast<unsigned>(kPhysicsStepMs);
    if (since > static_cast<unsigned>(kMaxTrimWindowMs)) since = static_cast<unsigned>(kMaxTrimWindowMs);
    gBanLastMs[i].store(now, std::memory_order_release);
    float dvx = 0.f, dvy = 0.f;
    ComputeSetVelocityImpl(x, y, vx, vy, tx, ty, since, &dvx, &dvy);
    const float cmdVx = ImpactDeltaFor(dvx, vx);
    const float cmdVy = ImpactDeltaFor(dvy, vy);
    if (!teleport::SetImpactNextOnVc(vc, static_cast<double>(cmdVx), static_cast<double>(cmdVy))) {
        static DWORD sLastFail = 0;
        if (now - sLastFail >= 1000) {
            sLastFail = now;
            x::runtime::LogW("MobFhBan", "SetImpactNext fail ap=(%.1f,%.1f) vt=(%.0f,%.0f)", x, y,
                             dvx, dvy);
        }
        return;
    }
    gBanLastX[i].store(FToBits(x), std::memory_order_release);
    gBanLastY[i].store(FToBits(y), std::memory_order_release);
    gBanApOk[i].store(1, std::memory_order_release);
    static DWORD sLastOk = 0;
    static unsigned sFireN = 0;
    ++sFireN;
    if (now - sLastOk >= 200) {
        sLastOk = now;
        x::runtime::LogI("MobFhBan",
                         "impact n=%u cmd=(%.0f,%.0f) vt=(%.0f,%.0f) ap=(%.1f,%.1f) aim=(%.1f,%.1f)",
                         sFireN, cmdVx, cmdVy, dvx, dvy, x, y, tx, ty);
        sFireN = 0;
    }
}

bool IsBannedVc(void* self) { return FindBanIndex(self) >= 0; }

uint8_t __fastcall HookCollisionDetect(void* self, void* a2, void* a3, uint8_t flag) {
    if (FindBanIndex(self) >= 0) {
        ApplyVtolOnVc(self);
        return 0;
    }
    if (!gOrigCd) return 0;
    return gOrigCd(self, a2, a3, flag);
}

uint8_t __fastcall HookCollisionDetectFloat(void* self, void* a2, void* a3, uint8_t flag) {
    if (FindBanIndex(self) >= 0) {
        ApplyVtolOnVc(self);
        return 0;
    }
    if (!gOrigCdf) return 0;
    return gOrigCdf(self, a2, a3, flag);
}

}  // namespace

void Init() {
    gStickOk = false;
    gLastAimMs.store(0, std::memory_order_release);
    gLastAimDt.store(0, std::memory_order_release);
}

void Shutdown() {
    ClearAll();
    if (!gInstalled.load(std::memory_order_acquire)) return;
    for (int i = 0; i < kAiN; ++i) {
        if (gAi[i].mi && gAi[i].orig) RestoreMethodInfo(gAi[i].mi, gAi[i].orig);
        gAi[i].mi = nullptr;
        gAi[i].orig = nullptr;
    }
    if (gSlotWua && gOrigMobWua) RestorePtr(gSlotWua, reinterpret_cast<void*>(gOrigMobWua));
    if (gSlotCd) RestoreSlot(gSlotCd, gOrigCd);
    if (gSlotCdf) RestoreSlot(gSlotCdf, gOrigCdf);
    gSlotWua = nullptr;
    gSlotCd = nullptr;
    gSlotCdf = nullptr;
    gOrigMobWua = nullptr;
    gOrigBaseWua = nullptr;
    gMiBaseWua = nullptr;
    gMiMobWua = nullptr;
    gOrigCd = nullptr;
    gOrigCdf = nullptr;
    gKlass = nullptr;
    gInstalled.store(false, std::memory_order_release);
    x::runtime::LogI("MobFhBan", "shutdown restored slots+ai");
}

bool EnsureInstalledOnPump(void* sampleVc) {
    if (gInstalled.load(std::memory_order_acquire)) return true;
    if (!LooksLikeHeapPtr(sampleVc)) return false;

    void* mobKlass = ReadPtrSeh(sampleVc, 0);
    if (!mobKlass) {
        x::runtime::LogW("MobFhBan", "install fail why=no_klass");
        return false;
    }

    void* lu = nullptr;
    if (player_combat::QueryLocalUser(&lu) && LooksLikeHeapPtr(lu)) {
        void* luVc = ReadPtrSeh(lu, kFbUserVecCtrl);
        void* luKlass = LooksLikeHeapPtr(luVc) ? ReadPtrSeh(luVc, 0) : nullptr;
        if (luKlass && luKlass == mobKlass) {
            x::runtime::LogW("MobFhBan", "install refuse: sample klass is LocalUser");
            return false;
        }
    }

    void** slotCd =
        reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mobKlass) + kKlassOffCdMethodPtr);
    void** slotCdf =
        reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mobKlass) + kKlassOffCdfMethodPtr);
    void* pCd = nullptr;
    void* pCdf = nullptr;
    __try {
        pCd = *slotCd;
        pCdf = *slotCdf;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("MobFhBan", "install fail why=slot_seh");
        return false;
    }
    if (!pCd || !pCdf) {
        x::runtime::LogW("MobFhBan", "install fail why=null_mp");
        return false;
    }
    if (!PatchSlot(slotCd, reinterpret_cast<void*>(&HookCollisionDetect), &gOrigCd)) {
        x::runtime::LogW("MobFhBan", "install fail why=patch_cd");
        return false;
    }
    if (!PatchSlot(slotCdf, reinterpret_cast<void*>(&HookCollisionDetectFloat), &gOrigCdf)) {
        RestoreSlot(slotCd, gOrigCd);
        gOrigCd = nullptr;
        x::runtime::LogW("MobFhBan", "install fail why=patch_cdf");
        return false;
    }

    void** slotWua =
        reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mobKlass) + kKlassOffWuaMethodPtr);
    (void)x::runtime::il2cpp::Ensure();
    void* parent = ParentKlass(mobKlass);
    void* pBaseWua = parent ? ReadPtrSeh(parent, kKlassOffWuaMethodPtr) : nullptr;
    void* pMobWua = ReadPtrSeh(mobKlass, kKlassOffWuaMethodPtr);
    gMiMobWua = ReadVidMethod(mobKlass, kKlassOffWuaMethodPtr);
    gMiBaseWua = parent ? ReadVidMethod(parent, kKlassOffWuaMethodPtr) : nullptr;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kWua{1, TypeKind::Bool, false, false, {TypeKind::I32}};
    if (!gMiMobWua) {
        gMiMobWua = x::runtime::il2cpp_method::FindMethodResolved(
                        mobKlass, kRvaMobWua, kWua, "WorkUpdateActive", kHashWorkUpdateActive)
                        .method;
    }
    if (!gMiBaseWua && parent) {
        constexpr MethodShape kWuaParent{1, TypeKind::Bool, false, true, {TypeKind::I32}};
        gMiBaseWua = x::runtime::il2cpp_method::FindMethodResolved(
                         parent, kRvaBaseWua, kWuaParent, "WorkUpdateActive", kHashWorkUpdateActive)
                         .method;
    }
    if (pBaseWua) gOrigBaseWua = reinterpret_cast<FnWua>(pBaseWua);
    int wuaOk = 0;
    if (pMobWua && pBaseWua && pMobWua != pBaseWua) {
        void* origWua = nullptr;
        if (PatchPtr(slotWua, reinterpret_cast<void*>(&HookWorkUpdateActive), &origWua)) {
            gOrigMobWua = reinterpret_cast<FnWua>(origWua);
            gSlotWua = slotWua;
            wuaOk = 1;
        }
    }
    if (!wuaOk) {
        x::runtime::LogW("MobFhBan", "ai WUA slot skip mob=%p base=%p (E8 CtrlUpdate still live)",
                         pMobWua, pBaseWua);
    }

    constexpr MethodShape kVoid0{0, TypeKind::Void, false, false, {}};
    int aiN = 0;
    for (int i = 0; i < kAiN; ++i) {
        AiMiHook& h = gAi[i];
        const auto mr = x::runtime::il2cpp_method::FindMethodResolved(mobKlass, h.rva, kVoid0,
                                                                     h.plain, h.hash);
        auto* mi = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (!mi || !h.hook) continue;
        if (!PatchMethodInfo(mi, h.hook, &h.orig) || !h.orig) continue;
        h.mi = mi;
        ++aiN;
    }

    gKlass = mobKlass;
    gSlotCd = slotCd;
    gSlotCdf = slotCdf;
    gInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("MobFhBan",
                     "installed klass=%p cd=%p cdf=%p wua=%d ai_mi=%d/%d (vtol=SetImpactNext)",
                     mobKlass, pCd, pCdf, wuaOk, aiN, kAiN);
    return true;
}

bool Arm(void* vc, void* mob, int32_t id, float tx, float ty) {
    if (!LooksLikeHeapPtr(vc) || !LooksLikeHeapPtr(mob) || id == 0) return false;
    const uintptr_t p = reinterpret_cast<uintptr_t>(vc);
    const uintptr_t mp = reinterpret_cast<uintptr_t>(mob);
    const DWORD now = GetTickCount();
    const uint32_t bx = FToBits(tx);
    const uint32_t by = FToBits(ty);
    int empty = -1;
    for (int i = 0; i < kMaxBan; ++i) {
        const uintptr_t cur = gBanVc[i].load(std::memory_order_acquire);
        if (cur == p) {
            const int32_t oldId = gBanId[i].load(std::memory_order_acquire);
            gBanMob[i].store(mp, std::memory_order_release);
            gBanTick[i].store(now, std::memory_order_release);
            gBanId[i].store(id, std::memory_order_release);
            gBanTx[i].store(bx, std::memory_order_release);
            gBanTy[i].store(by, std::memory_order_release);
            gBanAim[i].store(1, std::memory_order_release);
            if (oldId != id) gBanApOk[i].store(0, std::memory_order_release);
            return true;
        }
        if (cur == 0 && empty < 0) empty = i;
    }
    if (empty < 0) return false;
    const int cap = gMaxArmed.load(std::memory_order_acquire);
    int live = 0;
    for (int i = 0; i < kMaxBan; ++i) {
        if (gBanVc[i].load(std::memory_order_acquire) != 0) ++live;
    }
    if (live >= cap) return false;
    uintptr_t expected = 0;
    if (!gBanVc[empty].compare_exchange_strong(expected, p, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return Arm(vc, mob, id, tx, ty);
    }
    gBanMob[empty].store(mp, std::memory_order_release);
    gBanId[empty].store(id, std::memory_order_release);
    gBanTick[empty].store(now, std::memory_order_release);
    gBanTx[empty].store(bx, std::memory_order_release);
    gBanTy[empty].store(by, std::memory_order_release);
    gBanAim[empty].store(1, std::memory_order_release);
    gBanLastMs[empty].store(0, std::memory_order_release);
    gBanApOk[empty].store(0, std::memory_order_release);
    return true;
}

void Disarm(void* vc) {
    if (!vc) return;
    const uintptr_t p = reinterpret_cast<uintptr_t>(vc);
    for (int i = 0; i < kMaxBan; ++i) {
        uintptr_t cur = p;
        if (gBanVc[i].compare_exchange_strong(cur, 0, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            ClearSlot(i, nullptr);
        }
    }
}

void ClearFh(void* vc) { ClearFhOnVc(vc); }

void ClearAll() {
    for (int i = 0; i < kMaxBan; ++i) ClearSlot(i, nullptr);
    gWipeGen.fetch_add(1, std::memory_order_acq_rel);
}

bool IsArmed(void* vc) { return FindBanIndex(vc) >= 0; }

int ArmedCount() {
    int n = 0;
    for (int i = 0; i < kMaxBan; ++i) {
        if (gBanVc[i].load(std::memory_order_acquire) != 0) ++n;
    }
    return n;
}

int CopyArmedIds(int32_t* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < kMaxBan && n < cap; ++i) {
        if (gBanVc[i].load(std::memory_order_acquire) == 0) continue;
        const int32_t id = gBanId[i].load(std::memory_order_acquire);
        if (id == 0) continue;
        out[n++] = id;
    }
    return n;
}

uint32_t WipeGeneration() { return gWipeGen.load(std::memory_order_acquire); }

void SweepStale(float playerY) { SweepStale(playerY, nullptr, -1); }

void SweepStale(float playerY, const int32_t* liveIds, int nLive) {
    (void)playerY;
    const DWORD now = GetTickCount();
    const bool dropMissing = liveIds != nullptr && nLive >= 0;
    for (int i = 0; i < kMaxBan; ++i) {
        const uintptr_t p = gBanVc[i].load(std::memory_order_acquire);
        if (!p) continue;
        void* vc = reinterpret_cast<void*>(p);
        const int32_t id = gBanId[i].load(std::memory_order_acquire);
        const DWORD armAt = gBanTick[i].load(std::memory_order_acquire);
        bool drop = false;
        const char* why = "";
        if (dropMissing) {
            bool live = false;
            for (int j = 0; j < nLive; ++j) {
                if (liveIds[j] == id) {
                    live = true;
                    break;
                }
            }
            if (!live) {
                drop = true;
                why = "gone";
            }
        }
        if (!drop && armAt != 0 && now - armAt >= gArmTimeoutMs.load(std::memory_order_acquire)) {
            drop = true;
            why = "timeout";
        } else if (!drop && !LooksLikeHeapPtr(vc)) {
            drop = true;
            why = "dead";
        } else if (!drop) {
            const float mx = static_cast<float>(ReadF64Seh(vc, kFbVcApY - 8));
            const float my = static_cast<float>(ReadF64Seh(vc, kFbVcApY));
            if (std::fabs(mx) + std::fabs(my) <= 1.f) {
                drop = true;
                why = "ap0";
            } else {
                void* mob = reinterpret_cast<void*>(gBanMob[i].load(std::memory_order_acquire));
                const char* keepWhy = "";
                if (!x::features::ports::mob::StillSameLiveMob(mob, id, &keepWhy)) {
                    drop = true;
                    why = keepWhy && keepWhy[0] ? keepWhy : "dead";
                }
            }
        }
        if (!drop) continue;
        ClearSlot(i, why);
    }
}

void SweepLand(float playerY) { SweepStale(playerY); }

void SetSpeedScale(float scale) {
    gSpeedScale.store(ClampF(scale, kScaleMin, kScaleMax), std::memory_order_release);
}

float SpeedScale() { return gSpeedScale.load(std::memory_order_acquire); }

void SetAntiJitter(bool on) {
    gAntiJitter.store(on ? 1 : 0, std::memory_order_release);
}

bool AntiJitterEnabled() { return gAntiJitter.load(std::memory_order_acquire) != 0; }

void SetPlayerLead(float vx, float vy) {
    gLeadVxBits.store(FToBits(vx), std::memory_order_release);
    gLeadVyBits.store(FToBits(vy), std::memory_order_release);
}

void SetMaxArmed(int n) {
    if (n < 1) n = 1;
    if (n > kMaxBan) n = kMaxBan;
    gMaxArmed.store(n, std::memory_order_release);
    int live = 0;
    for (int i = 0; i < kMaxBan; ++i) {
        if (gBanVc[i].load(std::memory_order_acquire) == 0) continue;
        ++live;
        if (live <= n) continue;
        ClearSlot(i, nullptr);
    }
}

void SetArmTimeoutMs(unsigned ms) {
    gArmTimeoutMs.store(xcat::ClampMobGatherHoldMs(ms ? ms : xcat::kMobGatherHoldMsDefault),
                        std::memory_order_release);
}

void SetActuatorParams(float kp, float dead, float cruiseR, float stationR, float maxCmd,
                       float gravity, float stickCreep, float stickStillV) {
    gKp.store(ClampF(kp, 1.f, 20.f), std::memory_order_release);
    gDead.store(ClampF(dead, 1.f, 40.f), std::memory_order_release);
    gCruiseR.store(ClampF(cruiseR, 40.f, 800.f), std::memory_order_release);
    gStationR.store(ClampF(stationR, 8.f, 200.f), std::memory_order_release);
    gMaxCmdLive.store(ClampF(maxCmd, 620.f, 8000.f), std::memory_order_release);
    gGravity.store(ClampF(gravity, 0.f, 200.f), std::memory_order_release);
    gStickCreep.store(ClampF(stickCreep, 1.f, 40.f), std::memory_order_release);
    gStickStillV.store(ClampF(stickStillV, 0.f, 400.f), std::memory_order_release);
}

float CruiseRadius() { return gCruiseR.load(std::memory_order_acquire); }
float StationRadius() { return gStationR.load(std::memory_order_acquire); }

void SetMotionTiers(float cruiseV, float stationV, float holdV) {
    gCruiseV.store(static_cast<float>(xcat::ClampMobGatherCruiseV(
                       cruiseV > 0.f ? static_cast<uint32_t>(cruiseV)
                                     : xcat::kMobGatherCruiseVDefault)),
                   std::memory_order_release);
    gStationV.store(static_cast<float>(xcat::ClampMobGatherStationV(
                        stationV > 0.f ? static_cast<uint32_t>(stationV)
                                       : xcat::kMobGatherStationVDefault)),
                    std::memory_order_release);
    gHoldV.store(static_cast<float>(xcat::ClampMobGatherHoldV(
                     holdV > 0.f ? static_cast<uint32_t>(holdV) : xcat::kMobGatherHoldVDefault)),
                 std::memory_order_release);
}

void SetSettleTune(float settleErr, float kpSettle, float brakeMs, float coastVy) {
    gSettleErr.store(static_cast<float>(xcat::ClampMobGatherSettleErr(
                         settleErr > 0.f ? static_cast<uint32_t>(settleErr)
                                         : xcat::kMobGatherSettleErrDefault)),
                     std::memory_order_release);
    gKpSettle.store(static_cast<float>(xcat::ClampMobGatherKpSettle(
                        kpSettle > 0.f ? static_cast<uint32_t>(kpSettle)
                                       : xcat::kMobGatherKpSettleDefault)),
                    std::memory_order_release);
    const unsigned bms = xcat::ClampMobGatherBrakeMs(
        brakeMs > 0.f ? static_cast<uint32_t>(brakeMs) : xcat::kMobGatherBrakeMsDefault);
    gBrakeSec.store(static_cast<float>(bms) * 0.001f, std::memory_order_release);
    gCoastVy.store(static_cast<float>(xcat::ClampMobGatherCoastVy(
                       static_cast<uint32_t>(coastVy > 0.f ? coastVy : 0.f))),
                   std::memory_order_release);
}

void SetGatherStandOff(bool custom, int32_t x, int32_t y) {
    const float ox = custom ? static_cast<float>(xcat::ClampMobGatherStandOffX(x))
                            : kGatherDefaultOffX;
    const float oy = custom ? static_cast<float>(xcat::ClampMobGatherStandOffY(y))
                            : kGatherDefaultOffY;
    const float prevX = gOffX.exchange(ox, std::memory_order_acq_rel);
    const float prevY = gOffY.exchange(oy, std::memory_order_acq_rel);
    if (prevX == ox && prevY == oy) return;
    gOffGen.fetch_add(1, std::memory_order_acq_rel);
}

void QueryGatherStandOff(float* outX, float* outY) {
    if (outX) *outX = gOffX.load(std::memory_order_acquire);
    if (outY) *outY = gOffY.load(std::memory_order_acquire);
}

void TickPlayerAim(float x, float y, float vx, float vy, int ma, float* outX, float* outY) {
    TickPlayerAimImpl(x, y, vx, vy, ma, outX, outY);
}

unsigned LastAimDtMs() { return gLastAimDt.load(std::memory_order_acquire); }

void ComputeSetVelocity(float x, float y, float vx, float vy, float aimX, float aimY,
                        unsigned sinceMs, float* setVx, float* setVy) {
    ComputeSetVelocityImpl(x, y, vx, vy, aimX, aimY, sinceMs, setVx, setVy);
}

}  // namespace x::features::ports::mob_fh_ban
