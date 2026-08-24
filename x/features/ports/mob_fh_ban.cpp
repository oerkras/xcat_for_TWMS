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
constexpr size_t kFbVcInputX = 0x50;
constexpr size_t kFbVcInputY = 0x54;
constexpr size_t kFbVcCurFh = 0x28;
constexpr size_t kFbVcLastFh = 0x30;
constexpr size_t kFbVcLadderOrRope = 0x40;
constexpr size_t kFbVcRelPos = 0x88;  // RelPos.Pos；V=+8
constexpr size_t kFbVcAp = 0x98;
constexpr size_t kFbVcApY = 0xA0;
constexpr size_t kFbVcApl = 0xB8;
constexpr size_t kFbVcMoveAction = 0x84;
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
// 逐帧位移夹速（怪速举报 prevpos 根治）：客户端每帧比对怪移动记录位移 vs 怪速+10，
// 超门就离开正常移动包路径（逆向证据见 docs/features/security/怪速举报type21与被动插值.md §7.2）。
// 把被拽怪每帧位移夹到 ≤ kDispCapPxDefault，就永远走正常路径、发不出举报。
// 单位=px/帧；默认 48 ≈ 1600 px/s@30ms（低于满档 144px/帧、高于巡航 18.6px/帧）。
// 真实怪速阈值需运行时实测——impact 日志会打 disp/cap，据此调 gDispCapPx。
constexpr float kDispCapPxDefault = 48.f;
constexpr bool kDispClampOnDefault = false;
// v145 远怪接力跳（拉距掐线根治）：服务器按「单次连续拉取总距」掐线——实测 ≤~1024px 零断连
// （r=1000 · 9.4min · 0 掐，含 far=22 齐拉），≥~1253px 必掐（179 场尸检 + r=2800 每波必掐）；
// 帧位移夹速(leash ≤39px/帧)与拉取量都救不了。故 >hopPx 的怪不直瞄站点：每跳朝站点推进
// ≤hopPx，到中转点（≤kHopArriveR）驻留 kHopDwellMs 让服务器把这段移动落账，再起下一跳。
// 中转点一旦定下不随玩家移动漂移，避免一段连续移动被越拉越长。
constexpr float kHopArriveR = 32.f;
constexpr DWORD kHopDwellMs = 450;
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
std::atomic<unsigned> gStrategy{xcat::kMobGatherStrategyImpact};

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
// v144 到站落地：本槽怪已水平到站、进入「松手落台」态（CD 放行原生物理，不再摘台/定住）。
std::atomic<uint8_t> gBanLanded[kMaxBan]{};
std::atomic<DWORD> gBanLastMs[kMaxBan]{};
// v145 接力跳：本槽当前中转点（HopOn=1 时 Tx/Ty 写的是它而非站点）与到点驻留起始时刻。
std::atomic<uint32_t> gBanHopX[kMaxBan]{};
std::atomic<uint32_t> gBanHopY[kMaxBan]{};
std::atomic<uint8_t> gBanHopOn[kMaxBan]{};
std::atomic<DWORD> gBanHopDwell[kMaxBan]{};
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
std::atomic<float> gAimJitterPx{static_cast<float>(xcat::kMobGatherAimJitterDefault)};
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
std::atomic<uint8_t> gDispClampOn{kDispClampOnDefault ? 1u : 0u};
std::atomic<float> gDispCapPx{kDispCapPxDefault};
// v144 到站落地（策略 A 子项）：到站后松手让怪自然落台；默认关。
std::atomic<uint8_t> gLandOnArrive{xcat::kMobGatherLandOnArriveDefault ? 1u : 0u};
// v145 接力跳距 px/跳；<1 = 关（直拉旧行为）。
std::atomic<float> gHopPx{static_cast<float>(xcat::kMobGatherHopPxDefault)};

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
    gBanLanded[i].store(0, std::memory_order_release);
    gBanLastMs[i].store(0, std::memory_order_release);
    gBanHopX[i].store(0, std::memory_order_release);
    gBanHopY[i].store(0, std::memory_order_release);
    gBanHopOn[i].store(0, std::memory_order_release);
    gBanHopDwell[i].store(0, std::memory_order_release);
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

void WriteF64Seh(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint32_t ReadU32Seh(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t ReadI32Seh(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32Seh(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    const float scale = gSpeedScale.load(std::memory_order_acquire);
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

void ApplyAimJitter(int32_t id, float* x, float* y) {
    if (!x || !y) return;
    const float r = gAimJitterPx.load(std::memory_order_acquire);
    const int ir = static_cast<int>(r);
    if (ir <= 0) return;
    uint32_t h = static_cast<uint32_t>(id) * 0x9E3779B9u;
    h ^= 0x85EBCA6Bu;
    const uint32_t span = static_cast<uint32_t>(ir) * 2u + 1u;
    const float dx = static_cast<float>(static_cast<int>(h % span) - ir);
    h = h * 1664525u + 1013904223u;
    const float dy = static_cast<float>(static_cast<int>(h % span) - ir);
    *x += dx;
    *y += dy;
}

// v145 接力跳推进：入参 *tx/*ty 为（已抖动的）真实站点；若怪距站点 > hopPx，改写为本跳中转点。
// 状态机：无跳→起跳（中转点 = 怪位朝站点推进 hopPx）；在途→钉死中转点；到点（≤kHopArriveR）
// →驻留 kHopDwellMs（让服务器移动段落账）→ 起下一跳。距站点 ≤hopPx 时退出接力、直瞄收尾。
// 只在泵上被调（TickPlayerAimImpl / Arm 均在 pump），纯内存读写。
void HopAdvance(int i, void* vc, float* tx, float* ty) {
    const float hopPx = gHopPx.load(std::memory_order_acquire);
    if (hopPx < 1.f) {
        gBanHopOn[i].store(0, std::memory_order_release);
        return;
    }
    const float mx = static_cast<float>(ReadF64Seh(vc, kFbVcAp));
    const float my = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 8));
    if (std::fabs(mx) + std::fabs(my) <= 1.f) return;  // 怪位读不到：本拍保持直瞄
    const float dxA = *tx - mx;
    const float dyA = *ty - my;
    const float distA = std::sqrt(dxA * dxA + dyA * dyA);
    if (distA <= hopPx) {  // 站点已在一跳内：直瞄收尾
        gBanHopOn[i].store(0, std::memory_order_release);
        return;
    }
    const DWORD now = GetTickCount();
    if (gBanHopOn[i].load(std::memory_order_acquire) != 0) {
        const float hx = BitsToF(gBanHopX[i].load(std::memory_order_acquire));
        const float hy = BitsToF(gBanHopY[i].load(std::memory_order_acquire));
        const float dwx = hx - mx;
        const float dwy = hy - my;
        if (std::sqrt(dwx * dwx + dwy * dwy) > kHopArriveR) {  // 在途：目标钉死在中转点
            *tx = hx;
            *ty = hy;
            return;
        }
        const DWORD dwell = gBanHopDwell[i].load(std::memory_order_acquire);
        if (dwell == 0) {
            gBanHopDwell[i].store(now ? now : 1, std::memory_order_release);
            *tx = hx;
            *ty = hy;
            return;
        }
        if (now - dwell < kHopDwellMs) {  // 驻留：等服务器把这段移动落账
            *tx = hx;
            *ty = hy;
            return;
        }
        // 驻留期满 → 落到下面起下一跳
    }
    const float s = hopPx / distA;
    const float hx = mx + dxA * s;
    const float hy = my + dyA * s;
    gBanHopX[i].store(FToBits(hx), std::memory_order_release);
    gBanHopY[i].store(FToBits(hy), std::memory_order_release);
    gBanHopDwell[i].store(0, std::memory_order_release);
    const bool fresh = gBanHopOn[i].exchange(1, std::memory_order_acq_rel) == 0;
    static DWORD sLastHopLog = 0;
    if (now - sLastHopLog >= 200) {
        sLastHopLog = now;
        x::runtime::LogI("MobFhBan",
                         "hop %s id=%d m=(%.0f,%.0f) wp=(%.0f,%.0f) aimD=%.0f hopPx=%.0f",
                         fresh ? "start" : "next", gBanId[i].load(std::memory_order_acquire), mx, my,
                         hx, hy, distA, hopPx);
    }
    *tx = hx;
    *ty = hy;
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
    for (int i = 0; i < kMaxBan; ++i) {
        const uintptr_t vcp = gBanVc[i].load(std::memory_order_acquire);
        if (!vcp) continue;
        if (!gBanAim[i].load(std::memory_order_acquire)) continue;
        float jx = ax;
        float jy = ay;
        ApplyAimJitter(gBanId[i].load(std::memory_order_acquire), &jx, &jy);
        // v145 接力跳：远怪目标改写为中转点。此处是 gBanTx 的每帧真源（CD 钩子每帧经
        // ApplyVtolOnVc 调回来），Arm 里那份只是初值。
        HopAdvance(i, reinterpret_cast<void*>(vcp), &jx, &jy);
        gBanTx[i].store(FToBits(jx), std::memory_order_release);
        gBanTy[i].store(FToBits(jy), std::memory_order_release);
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
    float realizedDisp = -1.f;  // 上一拍→本拍真实位移（=服端/反作弊量的那个量），仅日志用
    if (gBanApOk[i].load(std::memory_order_acquire)) {
        const float lx = BitsToF(gBanLastX[i].load(std::memory_order_acquire));
        const float ly = BitsToF(gBanLastY[i].load(std::memory_order_acquire));
        const float jx = x - lx;
        const float jy = y - ly;
        realizedDisp = std::sqrt(jx * jx + jy * jy);
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
    // 牵引点 leash（怪速举报 prevpos 根治）：实测把速度命令夹到 0.29/0.62，被拽怪逐帧真实位移
    // 都还是 ~120px——游戏按「目标点距离」搬怪，与我们下的速度大小无关（近目标 0.4px、远目标封顶
    // ~120px）。故改夹目标点：每拍只把目标朝真 aim 推进 ≤ cap px，怪就当「近目标」小步挪、逐帧
    // 位移 ≤ cap，永不越客户端「怪速+10」门。cap 面板同项（吸怪 快攻 TAB「防断」）。
    const bool clampOn = gDispClampOn.load(std::memory_order_acquire) != 0;
    const float cap = gDispCapPx.load(std::memory_order_acquire);
    float ltx = tx, lty = ty;
    float leashStep = -1.f;  // 本拍目标点离怪的距离（=期望逐帧位移），仅日志用
    if (clampOn && cap > 0.f) {
        const float ax = tx - x;
        const float ay = ty - y;
        const float adist = std::sqrt(ax * ax + ay * ay);
        if (adist > cap) {
            const float s = cap / adist;
            ltx = x + ax * s;
            lty = y + ay * s;
            leashStep = cap;
        } else {
            leashStep = adist;
        }
    }
    float dvx = 0.f, dvy = 0.f;
    ComputeSetVelocityImpl(x, y, vx, vy, ltx, lty, since, &dvx, &dvy);
    // 速度侧兜底夹（leash 后目标已近、通常不触发）：把预测位移再夹到 ≤ cap，双保险。
    float clampScale = 1.f;
    if (clampOn) {
        float dtSec = static_cast<float>(since) * 0.001f;
        if (dtSec < 0.001f) dtSec = kPhysicsStepMs * 0.001f;
        const float mag = std::sqrt(dvx * dvx + dvy * dvy);
        const float predDisp = mag * dtSec;
        if (cap > 0.f && predDisp > cap && mag > 1e-3f) {
            clampScale = cap / predDisp;
            dvx *= clampScale;
            dvy *= clampScale;
        }
    }
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
    static DWORD sLastBig = 0;
    static unsigned sFireN = 0;
    ++sFireN;
    // 真实逐帧位移越过 cap = 会被客户端「怪速+10」门判异常的那一帧，插队打（20ms 地板防刷屏），
    // 否则 200ms 全局采样会把这些「危险帧」抽掉，标定就永远只看到 disp=0.4/-1.0。
    const bool bigReal = realizedDisp >= 0.f && cap > 0.f && realizedDisp > cap;
    bool doLog = false;
    if (now - sLastOk >= 200) {
        doLog = true;
        sLastOk = now;
    } else if (bigReal && now - sLastBig >= 20) {
        doLog = true;
        sLastBig = now;
    }
    if (doLog) {
        x::runtime::LogI(
            "MobFhBan",
            "impact n=%u cmd=(%.0f,%.0f) vt=(%.0f,%.0f) ap=(%.1f,%.1f) aim=(%.1f,%.1f) "
            "disp=%.1f cap=%.0f clamp=%.2f dt=%ums big=%d leash=%.0f",
            sFireN, cmdVx, cmdVy, dvx, dvy, x, y, tx, ty, realizedDisp, cap, clampScale,
            since, bigReal ? 1 : 0, leashStep);
        sFireN = 0;
    }
}

void WriteApPose(void* vc, float sx, float sy, float fromX) {
    WriteF64Seh(vc, kFbVcAp, static_cast<double>(sx));
    WriteF64Seh(vc, kFbVcAp + 8, static_cast<double>(sy));
    WriteF64Seh(vc, kFbVcAp + 0x10, 0.0);
    WriteF64Seh(vc, kFbVcAp + 0x18, 0.0);
    WriteF64Seh(vc, kFbVcApl, static_cast<double>(sx));
    WriteF64Seh(vc, kFbVcApl + 8, static_cast<double>(sy));
    WriteF64Seh(vc, kFbVcApl + 0x10, 0.0);
    WriteF64Seh(vc, kFbVcApl + 0x18, 0.0);
    WriteI32Seh(vc, kFbVcInputX, 0);
    WriteI32Seh(vc, kFbVcInputY, 0);
    const int32_t oldMa = ReadI32Seh(vc, kFbVcMoveAction);
    int faceLeft = oldMa & 1;
    if (sx < fromX - 1.f) faceLeft = 1;
    else if (sx > fromX + 1.f) faceLeft = 0;
    const bool moving = std::fabs(sx - fromX) > 2.f;
    const int raw = moving ? 1 : 2;  // Walk / Stand
    WriteI32Seh(vc, kFbVcMoveAction, (raw << 1) | faceLeft);
}

void DetachFlyTo(void* vc, float sx, float sy, float fromX) {
    ClearFhOnVc(vc);
    WriteF64Seh(vc, kFbVcRelPos, 0.0);
    WriteF64Seh(vc, kFbVcRelPos + 8, 0.0);
    WriteApPose(vc, sx, sy, fromX);
}

// true = 本拍已写落点。成功后跳过原版 CD/CDF。
// B：卸台 + 一帧把 Ap/Apl 写到聚拢落点（人物站位+off）。不 PlantOnFh、不 leash、不 hop。
bool ApplyFhSnapOnVc(void* vc) {
    const int i = FindBanIndex(vc);
    if (i < 0) return false;
    if (!gBanAim[i].load(std::memory_order_acquire)) return false;
    void* mob = reinterpret_cast<void*>(gBanMob[i].load(std::memory_order_acquire));
    const int32_t id = gBanId[i].load(std::memory_order_acquire);
    const char* keepWhy = "";
    if (!x::features::ports::mob::StillSameLiveMob(mob, id, &keepWhy)) {
        ClearSlot(i, keepWhy && keepWhy[0] ? keepWhy : "dead");
        return false;
    }
    float standX = 0.f;
    float standY = 0.f;
    bool haveStand = false;
    teleport::FlightState st{};
    if (teleport::QueryFlightState(st) && st.ok) {
        TickPlayerAimImpl(st.x, st.y, st.vx, st.vy, st.ma, &standX, &standY);
        haveStand = std::fabs(standX) + std::fabs(standY) > 1.f;
    }
    const float tx = BitsToF(gBanTx[i].load(std::memory_order_acquire));
    const float ty = BitsToF(gBanTy[i].load(std::memory_order_acquire));
    const float destX = haveStand ? standX : tx;
    const float destY = haveStand ? standY : ty;
    const float x = static_cast<float>(ReadF64Seh(vc, kFbVcAp));
    const float y = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 8));
    if (std::fabs(x) + std::fabs(y) <= 1.f) return false;
    gBanTick[i].store(GetTickCount(), std::memory_order_release);
    DetachFlyTo(vc, destX, destY, x);
    gBanLastX[i].store(FToBits(destX), std::memory_order_release);
    gBanLastY[i].store(FToBits(destY), std::memory_order_release);
    gBanApOk[i].store(1, std::memory_order_release);
    static DWORD sLastOk = 0;
    const DWORD now = GetTickCount();
    if (now - sLastOk >= 200) {
        sLastOk = now;
        const float dx = destX - x;
        const float dy = destY - y;
        const float d = std::sqrt(dx * dx + dy * dy);
        x::runtime::LogI("MobFhBan",
                         "fh-snap ap=(%.1f,%.1f) snap=(%.1f,%.1f) aim=(%.1f,%.1f) fh=0 dest=0 "
                         "plant=0 air=0 d=%.0f",
                         x, y, destX, destY, destX, destY, d);
    }
    return true;
}

bool IsBannedVc(void* self) { return FindBanIndex(self) >= 0; }

// 到站落地闩锁（策略 A 子项）。返回 true = 本拍应放行给原生 CD（怪进入「松手落台」态：
// 不摘台、不注速，交给游戏自身重力/踏板碰撞自然落地）。
// 进入：怪水平到站 |aimX-x| ≤ stationR。退出：玩家/怪走远 |aimX-x| > cruiseR（带滞回防抖），
// 退出时重置位移基线，免恢复吸拉首拍被 pool_jump 误清。落地期续 gBanTick，免 arm 超时被扫掉。
bool UpdateLandLatch(int i, void* vc) {
    if (!gLandOnArrive.load(std::memory_order_acquire)) {
        gBanLanded[i].store(0, std::memory_order_release);
        return false;
    }
    if (!gBanAim[i].load(std::memory_order_acquire)) return false;
    // v145 接力跳在途/驻留中：gBanTx 是中转点不是站点，绝不落地（否则怪掉在半路）。
    if (gBanHopOn[i].load(std::memory_order_acquire) != 0) {
        gBanLanded[i].store(0, std::memory_order_release);
        return false;
    }
    const float x = static_cast<float>(ReadF64Seh(vc, kFbVcAp));
    const float y = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 8));
    if (std::fabs(x) + std::fabs(y) <= 1.f) return false;
    const float tx = BitsToF(gBanTx[i].load(std::memory_order_acquire));
    const float ex = std::fabs(tx - x);
    const float stationR = gStationR.load(std::memory_order_acquire);
    const float cruiseR = gCruiseR.load(std::memory_order_acquire);
    const bool landed = gBanLanded[i].load(std::memory_order_acquire) != 0;
    if (landed) {
        if (ex > cruiseR) {
            gBanLanded[i].store(0, std::memory_order_release);
            gBanApOk[i].store(0, std::memory_order_release);
            return false;
        }
        gBanTick[i].store(GetTickCount(), std::memory_order_release);
        return true;
    }
    if (ex <= stationR) {
        gBanLanded[i].store(1, std::memory_order_release);
        gBanTick[i].store(GetTickCount(), std::memory_order_release);
        static DWORD sLastLand = 0;
        const DWORD now = GetTickCount();
        if (now - sLastLand >= 200) {
            sLastLand = now;
            x::runtime::LogI("MobFhBan", "land-on-arrive id=%d ap=(%.1f,%.1f) ex=%.1f stationR=%.0f",
                             gBanId[i].load(std::memory_order_acquire), x, y, ex, stationR);
        }
        return true;
    }
    return false;
}

uint8_t __fastcall HookCollisionDetect(void* self, void* a2, void* a3, uint8_t flag) {
    const int i = FindBanIndex(self);
    if (i >= 0) {
        if (gStrategy.load(std::memory_order_acquire) == xcat::kMobGatherStrategyFhSnap) {
            if (ApplyFhSnapOnVc(self)) return 0;
            if (gOrigCd) return gOrigCd(self, a2, a3, flag);
            return 0;
        }
        if (UpdateLandLatch(i, self)) {
            if (gOrigCd) return gOrigCd(self, a2, a3, flag);
            return 0;
        }
        ApplyVtolOnVc(self);
        return 0;
    }
    if (!gOrigCd) return 0;
    return gOrigCd(self, a2, a3, flag);
}

uint8_t __fastcall HookCollisionDetectFloat(void* self, void* a2, void* a3, uint8_t flag) {
    const int i = FindBanIndex(self);
    if (i >= 0) {
        if (gStrategy.load(std::memory_order_acquire) == xcat::kMobGatherStrategyFhSnap) {
            if (ApplyFhSnapOnVc(self)) return 0;
            if (gOrigCdf) return gOrigCdf(self, a2, a3, flag);
            return 0;
        }
        if (UpdateLandLatch(i, self)) {
            if (gOrigCdf) return gOrigCdf(self, a2, a3, flag);
            return 0;
        }
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
                     "installed klass=%p cd=%p cdf=%p wua=%d ai_mi=%d/%d "
                     "(A=SetImpactNext B=write-ap-dest)",
                     mobKlass, pCd, pCdf, wuaOk, aiN, kAiN);
    return true;
}

// 抓怪首帧不再瞎：以 arm 时怪的真实 AbsPos 播种 last 基准，让 realizedDisp（=服端/反作弊量的
// 逐帧位移）从被拽后第 1 拍即有效，而不是首帧恒打 disp=-1.0。仅供标定 cap 用。
static void SeedArmBaseline(int i, void* vc) {
    const float ax = static_cast<float>(ReadF64Seh(vc, kFbVcAp));
    const float ay = static_cast<float>(ReadF64Seh(vc, kFbVcAp + 8));
    if (std::fabs(ax) + std::fabs(ay) <= 1.f) {
        gBanApOk[i].store(0, std::memory_order_release);
        return;
    }
    gBanLastX[i].store(FToBits(ax), std::memory_order_release);
    gBanLastY[i].store(FToBits(ay), std::memory_order_release);
    gBanApOk[i].store(1, std::memory_order_release);
}

bool Arm(void* vc, void* mob, int32_t id, float tx, float ty) {
    if (!LooksLikeHeapPtr(vc) || !LooksLikeHeapPtr(mob) || id == 0) return false;
    const uintptr_t p = reinterpret_cast<uintptr_t>(vc);
    const uintptr_t mp = reinterpret_cast<uintptr_t>(mob);
    const DWORD now = GetTickCount();
    ApplyAimJitter(id, &tx, &ty);
    int empty = -1;
    for (int i = 0; i < kMaxBan; ++i) {
        const uintptr_t cur = gBanVc[i].load(std::memory_order_acquire);
        if (cur == p) {
            const int32_t oldId = gBanId[i].load(std::memory_order_acquire);
            gBanMob[i].store(mp, std::memory_order_release);
            gBanTick[i].store(now, std::memory_order_release);
            gBanId[i].store(id, std::memory_order_release);
            if (oldId != id) {
                gBanLanded[i].store(0, std::memory_order_release);
                gBanHopOn[i].store(0, std::memory_order_release);
                gBanHopDwell[i].store(0, std::memory_order_release);
                SeedArmBaseline(i, vc);
            }
            float ex = tx;
            float ey = ty;
            HopAdvance(i, vc, &ex, &ey);
            gBanTx[i].store(FToBits(ex), std::memory_order_release);
            gBanTy[i].store(FToBits(ey), std::memory_order_release);
            gBanAim[i].store(1, std::memory_order_release);
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
    gBanLanded[empty].store(0, std::memory_order_release);
    gBanLastMs[empty].store(0, std::memory_order_release);
    gBanHopOn[empty].store(0, std::memory_order_release);
    gBanHopDwell[empty].store(0, std::memory_order_release);
    {
        float ex = tx;
        float ey = ty;
        HopAdvance(empty, vc, &ex, &ey);
        gBanTx[empty].store(FToBits(ex), std::memory_order_release);
        gBanTy[empty].store(FToBits(ey), std::memory_order_release);
    }
    gBanAim[empty].store(1, std::memory_order_release);
    SeedArmBaseline(empty, vc);
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
    gSpeedScale.store(scale, std::memory_order_release);
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
    gKp.store(kp, std::memory_order_release);
    gDead.store(dead, std::memory_order_release);
    gCruiseR.store(cruiseR, std::memory_order_release);
    gStationR.store(stationR, std::memory_order_release);
    gMaxCmdLive.store(maxCmd, std::memory_order_release);
    gGravity.store(gravity, std::memory_order_release);
    gStickCreep.store(stickCreep, std::memory_order_release);
    gStickStillV.store(stickStillV, std::memory_order_release);
}

float CruiseRadius() { return gCruiseR.load(std::memory_order_acquire); }
float StationRadius() { return gStationR.load(std::memory_order_acquire); }

void SetDispClamp(bool on, float capPx) {
    const uint8_t prevOn = gDispClampOn.exchange(on ? 1u : 0u, std::memory_order_acq_rel);
    float prevCap = gDispCapPx.load(std::memory_order_acquire);
    if (capPx > 0.f) gDispCapPx.store(capPx, std::memory_order_release);
    // 只在值变化时打，避免每次 poll 刷屏：让 x.jsonl 直接看到运行时真值，
    // 不必再靠 UI 是否勾选来猜「牵引点到底开没开」。
    const uint8_t nowOn = on ? 1u : 0u;
    const float nowCap = (capPx > 0.f) ? capPx : prevCap;
    if (nowOn != prevOn || nowCap != prevCap) {
        x::runtime::LogI("MobFhBan", "dispclamp apply on=%d cap=%.0f", nowOn ? 1 : 0, nowCap);
    }
}

bool DispClampEnabled() { return gDispClampOn.load(std::memory_order_acquire) != 0; }

float DispClampCapPx() { return gDispCapPx.load(std::memory_order_acquire); }

void SetStrategy(unsigned strategy) {
    const unsigned v = (strategy == xcat::kMobGatherStrategyFhSnap)
                           ? xcat::kMobGatherStrategyFhSnap
                           : xcat::kMobGatherStrategyImpact;
    const unsigned prev = gStrategy.exchange(v, std::memory_order_acq_rel);
    if (prev == v) return;
    ClearAll();
    x::runtime::LogI("MobFhBan", "strategy %u -> %u (0=A impact 1=B write-ap-dest)", prev, v);
}

unsigned Strategy() { return gStrategy.load(std::memory_order_acquire); }

void SetLandOnArrive(bool on) {
    const uint8_t nowOn = on ? 1u : 0u;
    const uint8_t prev = gLandOnArrive.exchange(nowOn, std::memory_order_acq_rel);
    if (prev == nowOn) return;
    // 关掉时清所有落地闩，让在途怪立刻回到悬停/吸拉态。
    if (!nowOn) {
        for (int i = 0; i < kMaxBan; ++i) gBanLanded[i].store(0, std::memory_order_release);
    }
    x::runtime::LogI("MobFhBan", "land-on-arrive apply on=%d", nowOn ? 1 : 0);
}

bool LandOnArriveEnabled() { return gLandOnArrive.load(std::memory_order_acquire) != 0; }

void SetHopPx(float px) {
    if (px < 1.f) px = 0.f;
    const float prev = gHopPx.exchange(px, std::memory_order_acq_rel);
    if (prev == px) return;
    // 关掉时清所有在途中转点，让持怪立刻回到直瞄。
    if (px < 1.f) {
        for (int i = 0; i < kMaxBan; ++i) {
            gBanHopOn[i].store(0, std::memory_order_release);
            gBanHopDwell[i].store(0, std::memory_order_release);
        }
    }
    x::runtime::LogI("MobFhBan", "hop apply px=%.0f (0=off direct-pull)", px);
}

float HopPx() { return gHopPx.load(std::memory_order_acquire); }

bool EffectiveAim(void* vc, float* tx, float* ty) {
    const int i = FindBanIndex(vc);
    if (i < 0 || !gBanAim[i].load(std::memory_order_acquire)) return false;
    if (tx) *tx = BitsToF(gBanTx[i].load(std::memory_order_acquire));
    if (ty) *ty = BitsToF(gBanTy[i].load(std::memory_order_acquire));
    return true;
}

bool IsLanded(void* vc) {
    const int i = FindBanIndex(vc);
    if (i < 0) return false;
    return gBanLanded[i].load(std::memory_order_acquire) != 0;
}

void SetMotionTiers(float cruiseV, float stationV, float holdV) {
    gCruiseV.store(cruiseV, std::memory_order_release);
    gStationV.store(stationV, std::memory_order_release);
    gHoldV.store(holdV, std::memory_order_release);
}

void SetSettleTune(float settleErr, float kpSettle, float brakeMs, float coastVy) {
    gSettleErr.store(settleErr, std::memory_order_release);
    gKpSettle.store(kpSettle, std::memory_order_release);
    gBrakeSec.store(brakeMs * 0.001f, std::memory_order_release);
    gCoastVy.store(coastVy, std::memory_order_release);
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

void SetAimJitterPx(float px) {
    if (px < 0.f) px = 0.f;
    gAimJitterPx.store(px, std::memory_order_release);
}

float AimJitterPx() { return gAimJitterPx.load(std::memory_order_acquire); }

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
