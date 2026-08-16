// mob_gather_port — 泵上对本地模拟怪：卸台 + CDF VTOL（F5 控制律）吸到人身边。
// 控权申请（选项）：泵上调官方 ApplyControl。默认站立吸怪不占 heli_rotor。
// 「先飞到最密堆再吸」选项开时用人侧 Owner::Gather + BanSource::Gather，不抢 F6/赶路。
// 「软重连后返回原位」与寻簇互斥：回图后飞到 F5/按钮记下的 AbsPos，挂台再吸。
// 禁止硬写 Ap、禁止造 206、禁止钩 calc_priority、禁止写 Mob+0xE8。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_gather_port.h"

#include "foothold_path.h"
#include "mob_fh_ban.h"
#include "mob_pool_port.h"
#include "skill_port.h"
#include "teleport_port.h"
#include "world_port.h"
#include "fly_fh_ban.h"
#include "../auto_lie/auto_lie.h"
#include "../auto_supply/auto_supply.h"
#include "../char_boot/char_boot.h"
#include "../channel_hop/channel_hop.h"
#include "../encounter/encounter.h"
#include "../kick_sniff/kick_sniff.h"
#include "../notify/notify.h"
#include "../sellbag/sellbag.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../simple_combat/heli_rotor.h"
#include "../simple_combat/simple_combat.h"
#include "../travel/travel.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../../common/xcat_payload_control.h"
#include "../../ipc/payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace x::features::ports::mob_gather {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kFbPvcActive = 0xF0;
constexpr size_t kFbMobData = 0x138;
constexpr size_t kFbVcAp = 0x98;
constexpr size_t kFbVcCurFh = 0x28;
constexpr size_t kFbFhId = 0x10;
constexpr size_t kFbMoveAbility = 0x2C;
constexpr size_t kFbVcAct = 0x80;

constexpr int kMaxHold = 64;
constexpr float kGatherRadiusPx = 2800.f;
constexpr DWORD kJobWaitMs = 1500;
constexpr DWORD kDtCapMs = 120;
constexpr DWORD kDtDefaultMs = 40;
constexpr DWORD kOneshotHoldMs = 5000;
constexpr DWORD kApplyPeriodMs = 1000;
// 履历闸：新 oid 先让官方 CD 横移且贴地；脚边 / 进图第一批照吸。不改飞控。
// 掉落用 onFh/vy 挡（AbsPos：Y 增大=更高，掉落 vy 更负）。横移只认平台来回，
// 不要大于巡逻幅度（105060100 实测来回约 70–200，240 会把补刷整波卡死在 14s 窗外）。
// 横移 / 脚边以面板 mobGatherWalkDx / mobGatherFeetExemptPx 为准。
constexpr float kWalkMaxDropVy = -120.f;
// 新 oid 竖直闸：只挡「比人低超过预算」的怪。预算是**服务器容忍度**（179 场断连尸检标定，
// 全图一致，不是某图坑深）：低于人 ≤1156px 的拉取窗口 0/80 被掐；≥1253px 的窗口 78/80
// 被掐（首拉到掐 3.8~14s，中位 8.5s）。速度 maxCmd 与拉取量均与被掐无关。
// AbsPos：Y 更小 = 更低。隔层 |dY|>预算 双向都不新 Arm（人飞到坑底后还拖上层白名单
// 一样是 1300px 层谎，BIN 15:45:10 sample ap=-157 / py=-1509 → 205）。
// 弃用 foothold 图闸：快照是 AbsPos 空间且山谷图斜坡 prev/next 链把坑底连进人所在
// 步行连通域（BIN reach=381/634 含坑），Walk 不限跳会把坑白名单化，闸整体失效。
constexpr float kGatherMaxBelowPlayerPx = 1200.f;  // 厂默；运行时以面板 mobGatherDyLimPx / gDyLimUser 为准
// 最密簇：只在人所在层找（|dY|≤竖层窗，面板「竖层」，默认 200）。禁止跨层俯冲
// （BIN 16:50:40 跨 1078px 0.47s 到了人还在台上方，松旋翼后一边掉一边砍）。
// 选项关（默认）= 站立吸怪，本闸不跑。
constexpr float kClusterNeighborPx = 800.f;
constexpr float kSeekArriveHypotPx = 80.f;
// 贴门同款：Station 圈 140；巡航瞄准抬高（AbsPos 更大 Y=更高）。
// 合距 ≤80 且 |vx| 小就卸推：Station 悬停时 |vy| 会在 ±40~160 抖，等 vy 会永远漂
// （BIN 18:40:00 latch y=-809 人停在 -771，bleed 20s 才偶然下切）。
constexpr float kSeekStationR = 140.f;
constexpr float kSeekAimLiftY = 24.f;
constexpr float kSeekFinalLiftY = 8.f;
constexpr float kSeekHoldEnterVx = 30.f;
constexpr float kSeekHoldEnterVy = 35.f;
constexpr float kSeekHoldEnterDy = 40.f;
constexpr float kSeekHoldBelowMax = 12.f;
constexpr DWORD kSeekBleedMaxMs = 2500;
// 到位后 BAN 会摘台。先 Station 收到 |v| 小，再 SetImpactNext(-v) 反推，再放挂台。
// ma=4/5 不够：BIN 17:38:42 空中 vy=-125 松旋翼后滑出 home 47px 仍 arrived。
constexpr float kSeekArriveStillPx = 30.f;
constexpr DWORD kSeekSettleMaxMs = 2000;
constexpr float kSeekSnapHomeMaxPx = 64.f;
// 高度闸扫描：出生点到坑底 hypot≈2300，日常半径 1900 够不着。扫描时临时放大，不改 TAB。
constexpr float kDyRampRadiusPx = 6000.f;
// 与 mob_fh_ban 武装后 pool_jump 同阈值：一拍位移过大 = 池槽复用，不当走过。
constexpr float kPoolJumpPx = 800.f;
// dump.cs.restored.C 把 ApplyControl 标在 0xF57320，下一项 GetReviveList 只隔 16 字节 = stub。
// OnFixedUpdate 0xF29E50 体积极大，禁止当成申请入口。CanApplyCtrl 0x1518160 可作 RVA fallback。
constexpr uint32_t kRvaDumpApplyStub = 0xF57320;
constexpr uint32_t kRvaDumpOnFixedUpdate = 0xF29E50;
constexpr uint32_t kRvaDumpCanApplyCtrl = 0x1518160;
constexpr uint32_t kRvaDumpCalcPriority = 0xF64360;
constexpr size_t kCallScan = 0x4000;
constexpr size_t kOnFixedScan = 0x8000;

constexpr char kMobClass[] =
    "dd472cd442d83d6a25198278fa3e0a09d3a98a6db2e05d1b12d508d43db4cb5";
constexpr char kHashApplyControl[] =
    "a7982679b4b10ec2c0b41ae7d2ca76e66f7d61e29b31fd00bd18b6a1cf080ed";
constexpr char kHashCanApplyCtrl[] =
    "a96726caa01d4a70412782c3181a5f4cdd6829865a08e859af8df671a384915";
constexpr char kHashCalcPriority[] =
    "c3881ee4b61e05796972a8196ec943d2d660184c9aaca9bf5d958d0fcac9424";
constexpr char kHashOnFixedUpdate[] =
    "f4babc321065db6d87fa1fd3f07a232d8e702738f95f367d429cfd70724e619";

std::atomic<bool> gOn{false};
std::atomic<bool> gEncounterPause{false};
std::atomic<DWORD> gHoldUntil{0};
DWORD gLastHoldTick = 0;
DWORD gLastApplyTick = 0;
std::atomic<int> gMaxHold{kMaxHold};
std::atomic<int> gFarInFlightMax{0};
std::atomic<float> gRadiusPx{kGatherRadiusPx};
std::atomic<float> gLayerYPx{static_cast<float>(xcat::kMobGatherLayerYPxDefault)};
std::atomic<float> gDyLimUser{static_cast<float>(xcat::kMobGatherDyLimPxDefault)};
std::atomic<float> gWalkReadyDx{static_cast<float>(xcat::kMobGatherWalkDxDefault)};
std::atomic<float> gFeetExemptPx{static_cast<float>(xcat::kMobGatherFeetExemptPxDefault)};
std::atomic<unsigned> gHoldTimeoutMs{8000};
std::atomic<unsigned> gRecruitMs{40};
std::atomic<unsigned> gAimMs{17};
std::atomic<uint8_t> gIgnoreQuiet{0};
std::atomic<unsigned> gQuietDelayMs{0};
std::atomic<DWORD> gQuietSinceMs{0};
std::atomic<uint8_t> gApplyCtrl{0};
std::atomic<uint8_t> gSoftRelogin{1};
std::atomic<unsigned> gSoftReloginSec{14};
std::atomic<uint8_t> gClearRelogin{0};
std::atomic<uint8_t> gSeekClusterOn{0};
std::atomic<uint8_t> gHomeReturnOn{0};
std::atomic<uint8_t> gSeeking{0};
std::atomic<uint8_t> gHomePending{0};
std::atomic<int32_t> gHomeX{0};
std::atomic<int32_t> gHomeY{0};
std::atomic<int32_t> gHomeMapId{0};
std::atomic<uint8_t> gHomeValid{0};
std::atomic<uint8_t> gHomeHasMap{0};
uint8_t gFlyKind = 0;  // 0 idle · 1 seek_cluster · 2 home_return
uint8_t gHomeSawPlay = 0;
uint8_t gHomeHaveLastMap = 0;
int32_t gHomeLastMapId = 0;
std::atomic<float> gDyLim{static_cast<float>(xcat::kMobGatherDyLimPxDefault)};
std::atomic<uint8_t> gDyRampOn{0};
DWORD gDyRampLastMs = 0;
uint32_t gDyRampLastSeq = 0;
float gSeekLatchX = 0.f;
float gSeekLatchY = 0.f;
uint8_t gSeekLatchOn = 0;
DWORD gSeekSettleMs = 0;
DWORD gSeekNearMs = 0;
DWORD gSoftArmMs = 0;
DWORD gSoftPauseMs = 0;
DWORD gSoftSkipLogMs = 0;
DWORD gClearWaitReadyMs = 0;
DWORD gClearSkipLogMs = 0;
DWORD gClearEmptyMs = 0;
DWORD gClearLastNewMs = 0;
uint32_t gClearWipeSeen = 0;
int32_t gClearWaveId[kMaxHold]{};
int gClearWaveN = 0;
bool gClearWaveClosed = false;
bool gClearFired = false;

constexpr int kOidTrack = 160;
struct OidTrack {
    int32_t id = 0;
    float homeX = 0.f;
    float homeY = 0.f;
    float lastX = 0.f;
    float lastY = 0.f;
    uint8_t exempt = 0;  // 本图第一次吸之前见到的：允许未横移就远拉
};
OidTrack gOid[kOidTrack]{};
int gOidN = 0;
int gSpawnGateMapId = 0;
bool gSpawnGateOn = false;

void ClearOidTrack() { gOidN = 0; }

void NoteOid(int32_t id, float x, float y) {
    if (id == 0) return;
    for (int i = 0; i < gOidN; ++i) {
        if (gOid[i].id == id) return;
    }
    if (gOidN >= kOidTrack) {
        for (int i = 1; i < gOidN; ++i) gOid[i - 1] = gOid[i];
        --gOidN;
    }
    gOid[gOidN].id = id;
    gOid[gOidN].homeX = x;
    gOid[gOidN].homeY = y;
    gOid[gOidN].lastX = x;
    gOid[gOidN].lastY = y;
    gOid[gOidN].exempt = gSpawnGateOn ? 0 : 1;
    ++gOidN;
}

bool WalkHoldNew(int32_t id, float x, float vy, int onFh, float* outDx) {
    if (outDx) *outDx = 0.f;
    for (int i = 0; i < gOidN; ++i) {
        if (gOid[i].id != id) continue;
        if (gOid[i].exempt) return false;
        float dx = x - gOid[i].homeX;
        if (dx < 0.f) dx = -dx;
        if (outDx) *outDx = dx;
        if (dx < gWalkReadyDx.load(std::memory_order_acquire)) return true;
        if (onFh == 0) return true;
        if (vy < kWalkMaxDropVy) return true;
        return false;
    }
    return gSpawnGateOn;
}

bool OidKnown(int32_t id) {
    for (int i = 0; i < gOidN; ++i) {
        if (gOid[i].id == id) return true;
    }
    return false;
}

bool Dist2AtLeast(float ax, float ay, float bx, float by, float r) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy >= r * r;
}

bool OidPoolJump(int32_t id, float x, float y) {
    for (int i = 0; i < gOidN; ++i) {
        if (gOid[i].id != id) continue;
        const bool jump = Dist2AtLeast(x, y, gOid[i].lastX, gOid[i].lastY, kPoolJumpPx);
        gOid[i].lastX = x;
        gOid[i].lastY = y;
        if (!jump) return false;
        gOid[i].homeX = x;
        gOid[i].homeY = y;
        gOid[i].exempt = 0;
        return true;
    }
    return false;
}

bool PosVsApPool(float mx, float my, float ax, float ay) {
    // Mob+pos 可能是 Ap 同号，也可能是显示翻转 Y。两种都对不上才当池脏。
    if (!Dist2AtLeast(mx, my, ax, ay, kPoolJumpPx)) return false;
    return Dist2AtLeast(mx, my, ax, -ay, kPoolJumpPx);
}

void PruneOidTrack(const int32_t* liveIds, int nLive) {
    int w = 0;
    for (int i = 0; i < gOidN; ++i) {
        bool keep = false;
        for (int j = 0; j < nLive; ++j) {
            if (liveIds[j] == gOid[i].id) {
                keep = true;
                break;
            }
        }
        if (!keep) continue;
        if (w != i) gOid[w] = gOid[i];
        ++w;
    }
    gOidN = w;
}

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

using FnApplyControl = void (*)(void* self, int tCur, const void* methodInfo);

void* gMiApply = nullptr;
uint32_t gApplyRva = 0;
const char* gApplyHow = "";
DWORD gLastApplyMissLog = 0;

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

bool ReadAp(void* vc, float* x, float* y, float* vx, float* vy) {
    if (!LooksLikeHeapPtr(vc) || !x || !y) return false;
    *x = static_cast<float>(ReadF64(vc, kFbVcAp));
    *y = static_cast<float>(ReadF64(vc, kFbVcAp + 8));
    if (vx) *vx = static_cast<float>(ReadF64(vc, kFbVcAp + 0x10));
    if (vy) *vy = static_cast<float>(ReadF64(vc, kFbVcAp + 0x18));
    return true;
}

int ReadOnFh(void* vc) {
    if (!LooksLikeHeapPtr(vc)) return 0;
    return LooksLikeHeapPtr(ReadPtr(vc, kFbVcCurFh)) ? 1 : 0;
}

bool HeightHoldNew(float my, float py) {
    const float lim = gDyLim.load(std::memory_order_acquire);
    float dy = my - py;
    if (dy < 0.f) dy = -dy;
    return dy > lim;
}

int ReadMoveAbility(void* mob) {
    void* data = ReadPtr(mob, kFbMobData);
    if (!LooksLikeHeapPtr(data)) return -1;
    return ReadI32(data, kFbMoveAbility);
}

bool IsFixedOrImmovable(void* mob) {
    return ReadMoveAbility(mob) == 0;
}

bool ApLooksLive(float x, float y) {
    return std::fabs(x) + std::fabs(y) > 1.f;
}

bool VcLooksLive(void* vc, float* x, float* y, float* vx, float* vy, int* act, int* onFh) {
    if (!LooksLikeHeapPtr(vc)) return false;
    float ax = 0.f, ay = 0.f, avx = 0.f, avy = 0.f;
    (void)ReadAp(vc, &ax, &ay, &avx, &avy);
    const int a = static_cast<int>(ReadU8(vc, kFbVcAct));
    const int fh = ReadOnFh(vc);
    if (x) *x = ax;
    if (y) *y = ay;
    if (vx) *vx = avx;
    if (vy) *vy = avy;
    if (act) *act = a;
    if (onFh) *onFh = fh;
    return a != 0 && ApLooksLive(ax, ay);
}

void* PickLiveVc(void* mob, float* x, float* y, float* vx, float* vy, int* act, int* onFh,
                 int* split = nullptr) {
    if (split) *split = 0;
    if (!LooksLikeHeapPtr(mob)) return nullptr;
    void* pvc = ReadPtr(mob, kFbPvcActive);
    void* vc50 = ReadPtr(mob, kFbVecCtrl);
    float px = 0.f, py = 0.f, pvx = 0.f, pvy = 0.f;
    int pa = 0, pf = 0;
    float sx = 0.f, sy = 0.f, svx = 0.f, svy = 0.f;
    int sa = 0, sf = 0;
    const bool pOk = VcLooksLive(pvc, &px, &py, &pvx, &pvy, &pa, &pf);
    const bool sOk = (pvc != vc50) && VcLooksLive(vc50, &sx, &sy, &svx, &svy, &sa, &sf);
    if (pOk && sOk && Dist2AtLeast(px, py, sx, sy, kPoolJumpPx)) {
        if (split) *split = 1;
        return nullptr;
    }
    if (pOk) {
        if (x) *x = px;
        if (y) *y = py;
        if (vx) *vx = pvx;
        if (vy) *vy = pvy;
        if (act) *act = pa;
        if (onFh) *onFh = pf;
        return pvc;
    }
    if (sOk) {
        if (x) *x = sx;
        if (y) *y = sy;
        if (vx) *vx = svx;
        if (vy) *vy = svy;
        if (act) *act = sa;
        if (onFh) *onFh = sf;
        return vc50;
    }
    return nullptr;
}

struct DenseCluster {
    bool ok = false;
    float cx = 0.f;
    float cy = 0.f;
    int n = 0;
    int live = 0;
};

bool FindDenseCluster(const mob::Snapshot& snap, float playerY, DenseCluster* out) {
    if (!out) return false;
    *out = DenseCluster{};
    const float layerWin = gLayerYPx.load(std::memory_order_acquire);
    float xs[mob::kMaxLiteMobs];
    float ys[mob::kMaxLiteMobs];
    int n = 0;
    for (int i = 0; i < snap.count && n < mob::kMaxLiteMobs; ++i) {
        const mob::MobLite& m = snap.mobs[i];
        if (!LooksLikeHeapPtr(m.ptr) || !m.ready || m.deadType != 0) continue;
        if (IsFixedOrImmovable(m.ptr)) continue;
        float x = 0.f;
        float y = 0.f;
        int split = 0;
        void* vc = PickLiveVc(m.ptr, &x, &y, nullptr, nullptr, nullptr, nullptr, &split);
        if (split || !vc) continue;
        float layerDy = y - playerY;
        if (layerDy < 0.f) layerDy = -layerDy;
        if (layerDy > layerWin) continue;
        xs[n] = x;
        ys[n] = y;
        ++n;
    }
    out->live = n;
    if (n <= 0) return false;

    int bestN = 0;
    int bestI = 0;
    const float r2 = kClusterNeighborPx * kClusterNeighborPx;
    for (int i = 0; i < n; ++i) {
        int cnt = 0;
        for (int j = 0; j < n; ++j) {
            float dy = ys[j] - ys[i];
            if (dy < 0.f) dy = -dy;
            if (dy > layerWin) continue;
            const float dx = xs[j] - xs[i];
            if (dx * dx + dy * dy > r2) continue;
            ++cnt;
        }
        if (cnt > bestN) {
            bestN = cnt;
            bestI = i;
        }
    }

    float sx = 0.f;
    float sy = 0.f;
    int pack = 0;
    for (int j = 0; j < n; ++j) {
        float dy = ys[j] - ys[bestI];
        if (dy < 0.f) dy = -dy;
        if (dy > layerWin) continue;
        const float dx = xs[j] - xs[bestI];
        if (dx * dx + dy * dy > r2) continue;
        sx += xs[j];
        sy += ys[j];
        ++pack;
    }
    if (pack <= 0) return false;
    out->ok = true;
    out->cx = sx / static_cast<float>(pack);
    out->cy = sy / static_cast<float>(pack);
    out->n = pack;
    return true;
}

bool PersonFlyBusy() {
    return gSeeking.load(std::memory_order_acquire) != 0 ||
           gHomePending.load(std::memory_order_acquire) != 0;
}

void ReleaseSeekRotor() {
    using x::features::simple_combat::heli::Owner;
    if (x::features::simple_combat::heli::CurrentOwner() == Owner::Gather) {
        x::features::simple_combat::heli::Disarm(Owner::Gather);
        x::features::simple_combat::heli::Release(Owner::Gather);
    }
    x::features::ports::fly_fh_ban::SetSourceArmed(
        x::features::ports::fly_fh_ban::BanSource::Gather, false);
}

void StopSeekFly(const char* why, bool keepLatch = false);

bool SeekPlayerStood(const teleport::FlightState& st) {
    if (!st.ok || !st.onFh) return false;
    float avx = st.vx;
    if (avx < 0.f) avx = -avx;
    float avy = st.vy;
    if (avy < 0.f) avy = -avy;
    if (avx > kSeekArriveStillPx || avy > kSeekArriveStillPx) return false;
    if (st.ma == 4 || st.ma == 5) return true;
    if (st.ma >= 0) return false;
    return true;
}

bool SeekSpeedBled(const teleport::FlightState& st) {
    float avx = st.vx;
    if (avx < 0.f) avx = -avx;
    float avy = st.vy;
    if (avy < 0.f) avy = -avy;
    return avx <= kSeekHoldEnterVx && avy <= kSeekHoldEnterVy;
}

bool SnapSeekLand(float x, float y, float layerY, float maxSnapPx, float* ox, float* oy) {
    if (!ox || !oy) return false;
    float sx = x;
    float sy = y;
    uint32_t fh = 0;
    if (!foothold_path::SnapStandAt(x, y, &sx, &sy, &fh, true, false) || !fh) return false;
    float layerDy = sy - layerY;
    if (layerDy < 0.f) layerDy = -layerDy;
    if (layerDy > gLayerYPx.load(std::memory_order_acquire)) return false;
    if (maxSnapPx > 0.f) {
        const float dx = sx - x;
        const float dy = sy - y;
        if (dx * dx + dy * dy > maxSnapPx * maxSnapPx) return false;
    }
    x::features::simple_combat::heli::ClampToCombatMoveBounds(&sx, &sy);
    *ox = sx;
    *oy = sy;
    return true;
}

void KillSeekResidual(const teleport::FlightState& st) {
    float avx = st.vx;
    if (avx < 0.f) avx = -avx;
    float avy = st.vy;
    if (avy < 0.f) avy = -avy;
    if (avx < 6.f && avy < 6.f) {
        x::runtime::LogI("MobGather", "land kill skip v=(%.0f,%.0f) (already slow)", st.vx,
                         st.vy);
        return;
    }
    teleport::ImpactVelOpts opts{};
    opts.minAbs = 6.f;
    opts.maxAbsVx = 8000.f;
    opts.maxAbsVy = 8000.f;
    opts.quietLog = true;
    opts.force = true;
    const float kvx = -st.vx;
    const float kvy = -st.vy;
    const bool ok = teleport::ImpactSetVelocity(
        kvx, kvy, teleport::ImpactRoute::SetImpactNext, opts);
    x::runtime::LogI("MobGather", "land kill v=(%.0f,%.0f) cmd=(%.0f,%.0f) ok=%d (SetImpactNext -v)",
                     st.vx, st.vy, kvx, kvy, ok ? 1 : 0);
}

void BeginSeekLand(const teleport::FlightState& st, const char* tag, float dist) {
    using x::features::simple_combat::heli::Owner;
    if (x::features::simple_combat::heli::CurrentOwner() == Owner::Gather) {
        x::features::simple_combat::heli::Disarm(Owner::Gather);
    }
    KillSeekResidual(st);
    ReleaseSeekRotor();
    const DWORD now = GetTickCount();
    gSeekSettleMs = now ? now : 1;
    gSeekNearMs = 0;
    gSeeking.store(1, std::memory_order_release);
    x::runtime::LogI("MobGather",
                     "%s settle enter d=%.0f onFh=%d ma=%d v=(%.0f,%.0f) kill=1",
                     tag ? tag : "seek", dist, st.onFh ? 1 : 0, st.ma, st.vx, st.vy);
}

enum class SeekLandPoll { Arrived, Waiting, RetryFly };

SeekLandPoll PollSeekLand(const teleport::FlightState& st, DWORD now, const char* tag) {
    gSeeking.store(1, std::memory_order_release);
    if (SeekPlayerStood(st)) {
        StopSeekFly("arrived", true);
        gHomePending.store(0, std::memory_order_release);
        return SeekLandPoll::Arrived;
    }
    const float dx = st.x - gSeekLatchX;
    const float dy = st.y - gSeekLatchY;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > kSeekArriveHypotPx + 80.f) {
        gSeekSettleMs = 0;
        x::runtime::LogI("MobGather",
                         "%s settle abort slide d=%.0f ap=(%.0f,%.0f) v=(%.0f,%.0f)",
                         tag ? tag : "seek", dist, st.x, st.y, st.vx, st.vy);
        return SeekLandPoll::RetryFly;
    }
    if (now - gSeekSettleMs > kSeekSettleMaxMs) {
        gSeekSettleMs = 0;
        x::runtime::LogI("MobGather",
                         "%s settle retry onFh=%d ma=%d v=(%.0f,%.0f)", tag ? tag : "seek",
                         st.onFh ? 1 : 0, st.ma, st.vx, st.vy);
        return SeekLandPoll::RetryFly;
    }
    static DWORD sSettleLog = 0;
    if (!sSettleLog || now - sSettleLog > 400) {
        sSettleLog = now;
        x::runtime::LogI("MobGather", "%s settle onFh=%d ma=%d v=(%.0f,%.0f)",
                         tag ? tag : "seek", st.onFh ? 1 : 0, st.ma, st.vx, st.vy);
    }
    return SeekLandPoll::Waiting;
}

void DriveSeekFly(const teleport::FlightState& st, DWORD now, const char* tag) {
    using x::features::simple_combat::heli::Mode;
    using x::features::simple_combat::heli::Owner;
    using x::features::simple_combat::heli::Setpoint;
    namespace heli = x::features::simple_combat::heli;

    const float landX = gSeekLatchX;
    const float landY = gSeekLatchY;
    const float dx = st.x - landX;
    const float dy = st.y - landY;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const bool inArrive = dist <= kSeekArriveHypotPx;
    const bool aboveDeck = st.y >= (landY - kSeekHoldBelowMax);
    const bool underDeck = st.y < (landY - kSeekHoldEnterDy);
    float avx = st.vx;
    if (avx < 0.f) avx = -avx;
    const bool bledX = avx <= kSeekHoldEnterVx;
    const bool bled = SeekSpeedBled(st);
    if (inArrive) {
        if (!gSeekNearMs) gSeekNearMs = now ? now : 1;
    } else {
        gSeekNearMs = 0;
    }
    const bool nearLong =
        gSeekNearMs != 0 && (now - gSeekNearMs) >= kSeekBleedMaxMs;
    // 不要等 |vy|：Station 托在台上方时 vy 收不干净。|vx| 小即可卸，落地用 -v 反推。
    // 也不要 y≤landY+24：实机悬停比抬高瞄准还高一截（BIN +38px），nearDeck 永远假。
    if (inArrive && aboveDeck && !underDeck && (bledX || bled || nearLong)) {
        BeginSeekLand(st, tag, dist);
        return;
    }
    if (inArrive && !bledX) {
        static DWORD sBleedLog = 0;
        if (!sBleedLog || now - sBleedLog > 400) {
            sBleedLog = now;
            x::runtime::LogI("MobGather",
                             "%s bleed wait d=%.0f v=(%.0f,%.0f) need |vx|<=%.0f "
                             "above=%d under=%d nearLong=%d",
                             tag ? tag : "seek", dist, st.vx, st.vy, kSeekHoldEnterVx,
                             aboveDeck ? 1 : 0, underDeck ? 1 : 0, nearLong ? 1 : 0);
        }
    }

    gSeeking.store(1, std::memory_order_release);
    if (heli::CurrentOwner() != Owner::Gather) {
        (void)heli::Acquire(Owner::Gather);
        heli::SetSpeedScale(Owner::Gather, heli::SpeedScale(Owner::Combat));
        x::features::ports::fly_fh_ban::SetSourceArmed(
            x::features::ports::fly_fh_ban::BanSource::Gather, true);
    } else {
        heli::SetSpeedScale(Owner::Gather, heli::SpeedScale(Owner::Combat));
    }

    float aimY = landY + kSeekAimLiftY;
    if (inArrive && aboveDeck) aimY = landY + kSeekFinalLiftY;
    Setpoint sp{};
    sp.x = landX;
    sp.y = aimY;
    heli::ClampToCombatMoveBounds(&sp.x, &sp.y);
    sp.mode = (dist > kSeekStationR) ? Mode::Cruise : Mode::Station;
    heli::SetSetpoint(Owner::Gather, sp);
    (void)heli::Tick(Owner::Gather, now, nullptr);

    static DWORD sFlyLog = 0;
    if (!sFlyLog || now - sFlyLog > 800) {
        sFlyLog = now;
        x::runtime::LogI("MobGather",
                         "%s fly latch=(%.0f,%.0f) ap=(%.0f,%.0f) d=%.0f v=(%.0f,%.0f) "
                         "mode=%s owner=%s",
                         tag ? tag : "seek", landX, landY, st.x, st.y, dist, st.vx, st.vy,
                         heli::ModeName(sp.mode), heli::OwnerName(heli::CurrentOwner()));
    }
}

void StopSeekFly(const char* why, bool keepLatch) {
    using x::features::simple_combat::heli::Owner;
    gSeekSettleMs = 0;
    gSeekNearMs = 0;
    const uint8_t wasSeek = gSeeking.exchange(0, std::memory_order_acq_rel);
    const uint8_t kind = gFlyKind;
    if (!keepLatch) gSeekLatchOn = 0;
    gFlyKind = 0;
    const bool owned =
        x::features::simple_combat::heli::CurrentOwner() == Owner::Gather;
    ReleaseSeekRotor();
    if (wasSeek || owned) {
        x::runtime::LogI("MobGather", "%s stop why=%s owned=%d keepLatch=%d",
                         kind == 2 ? "home_return" : "seek_cluster", why ? why : "?",
                         owned ? 1 : 0, keepLatch ? 1 : 0);
    }
}

void* MiPtr(void* mi) {
    auto* h = reinterpret_cast<MethodInfoHead*>(mi);
    if (!h) return nullptr;
    return h->methodPointer ? h->methodPointer : h->virtualMethodPointer;
}

uint32_t MiRva(void* mi) {
    void* p = MiPtr(mi);
    if (!p) return 0;
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (!base) return 0;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < base) return 0;
    const uint64_t d = static_cast<uint64_t>(a - base);
    return d > 0x7FFFFFFFull ? 0u : static_cast<uint32_t>(d);
}

bool InGa(const uint8_t* p) {
    if (!p) return false;
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (!base) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < base) return false;
    return (a - base) < 0x20000000ull;
}

uint8_t* FollowJmp(uint8_t* p) {
    if (!p) return nullptr;
    __try {
        for (int n = 0; n < 4; ++n) {
            if (!InGa(p)) return p;
            if (p[0] == 0xE9) {
                const int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
                p = p + 5 + rel;
                continue;
            }
            if (p[0] == 0xFF && p[1] == 0x25) {
                const int32_t disp = *reinterpret_cast<int32_t*>(p + 2);
                auto** slot = reinterpret_cast<uint8_t**>(p + 6 + disp);
                if (!slot) break;
                p = *slot;
                continue;
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return p;
    }
    return p;
}

bool RelCallHits(uint8_t* at, uint8_t* want) {
    if (!at || !want) return false;
    uint8_t* dest = nullptr;
    __try {
        if (at[0] != 0xE8) return false;
        const int32_t rel = *reinterpret_cast<int32_t*>(at + 1);
        dest = at + 5 + rel;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    want = FollowJmp(want);
    dest = FollowJmp(dest);
    return dest && want && dest == want;
}

size_t FirstCallSite(void* fn, void* target, size_t scan) {
    if (!fn || !target) return ~size_t{0};
    auto* p = reinterpret_cast<uint8_t*>(fn);
    auto* want = reinterpret_cast<uint8_t*>(target);
    size_t at = ~size_t{0};
    __try {
        for (size_t i = 0; i + 5 < scan; ++i) {
            if (p[i] != 0xE8) continue;
            if (RelCallHits(p + i, want)) {
                at = i;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ~size_t{0};
    }
    return at;
}

bool CodeCalls(void* fn, void* target, size_t scan) {
    return FirstCallSite(fn, target, scan) != ~size_t{0};
}

int CountE8(void* fn, size_t scan) {
    if (!fn) return 0;
    auto* p = reinterpret_cast<uint8_t*>(fn);
    int n = 0;
    __try {
        for (size_t i = 0; i + 5 < scan; ++i) {
            if (p[i] == 0xE8) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

bool LooksLikeTinyStub(void* fn) {
    if (!fn) return true;
    auto* p = reinterpret_cast<uint8_t*>(fn);
    __try {
        bool sawCall = false;
        for (size_t i = 0; i < 24; ++i) {
            if (p[i] == 0xE8 || p[i] == 0xE9) sawCall = true;
            if (p[i] == 0xC3 && i <= 8 && !sawCall) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}

int MethodArity(void* mi) {
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.methodGetParamCount || !mi) return -1;
    uint32_t n = 0;
    __try {
        n = e.methodGetParamCount(mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/arity");
        return -1;
    }
    return static_cast<int>(n);
}

bool NameHas(void* mi, const char* needle) {
    if (!mi || !needle || !needle[0]) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.methodGetName) return false;
    const char* nm = nullptr;
    __try {
        nm = e.methodGetName(mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/name");
        return false;
    }
    return nm && std::strstr(nm, needle);
}

bool ApplyShapeOk(void* mi) {
    if (!mi) return false;
    void* fn = MiPtr(mi);
    if (!fn) return false;
    const uint32_t rva = MiRva(mi);
    if (rva == kRvaDumpApplyStub || rva == kRvaDumpOnFixedUpdate) return false;
    if (LooksLikeTinyStub(fn)) return false;
    if (MethodArity(mi) != 1) return false;
    if (NameHas(mi, "OnFixedUpdate")) return false;
    return true;
}

void* ScanCallerOf(void* mobKlass, void* callee) {
    if (!mobKlass || !callee) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods) return nullptr;
    void* best = nullptr;
    int bestE8 = 9999;
    size_t bestAt = ~size_t{0};
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(mobKlass, &iter);
            if (!raw) break;
            if (!ApplyShapeOk(raw)) continue;
            void* fn = MiPtr(raw);
            const size_t at = FirstCallSite(fn, callee, kCallScan);
            if (at == ~size_t{0}) continue;
            const int e8s = CountE8(fn, kCallScan);
            if (e8s < bestE8 || (e8s == bestE8 && at < bestAt)) {
                bestE8 = e8s;
                bestAt = at;
                best = raw;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/scan");
        return nullptr;
    }
    return best;
}

void* FindMiByFn(void* klass, uint8_t* fn) {
    if (!klass || !fn) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods) return nullptr;
    uint8_t* want = FollowJmp(fn);
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            void* p = MiPtr(raw);
            if (!p) continue;
            if (FollowJmp(reinterpret_cast<uint8_t*>(p)) == want) return raw;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/byFn");
        return nullptr;
    }
    return nullptr;
}

bool CallsPriOrCan(void* mi, void* priFn, void* canFn) {
    void* fn = MiPtr(mi);
    if (!fn) return false;
    if (priFn && CodeCalls(fn, priFn, kCallScan)) return true;
    if (canFn && CodeCalls(fn, canFn, kCallScan)) return true;
    return false;
}

void* ScanOnFixedCallees(void* mobKlass, void* priFn, void* canFn) {
    using x::runtime::il2cpp_method::FindMethodByName;
    using x::runtime::il2cpp_method::FindMethodByRva;
    if (!mobKlass || (!priFn && !canFn)) return nullptr;
    void* ofuMi = FindMethodByName(mobKlass, kHashOnFixedUpdate, 0, true);
    if (!ofuMi) ofuMi = FindMethodByName(mobKlass, "OnFixedUpdate", 0, true);
    if (!ofuMi) ofuMi = FindMethodByRva(mobKlass, kRvaDumpOnFixedUpdate, true);
    void* ofu = MiPtr(ofuMi);
    if (!ofu) return nullptr;
    auto* p = reinterpret_cast<uint8_t*>(ofu);
    void* best = nullptr;
    int bestScore = -1;
    __try {
        for (size_t i = 0; i + 5 < kOnFixedScan; ++i) {
            if (p[i] != 0xE8) continue;
            const int32_t rel = *reinterpret_cast<int32_t*>(p + i + 1);
            uint8_t* dest = FollowJmp(p + i + 5 + rel);
            void* mi = FindMiByFn(mobKlass, dest);
            if (!ApplyShapeOk(mi) || mi == best) continue;
            if (!CallsPriOrCan(mi, priFn, canFn)) continue;
            const int score =
                (priFn && CodeCalls(MiPtr(mi), priFn, kCallScan) ? 2 : 0) +
                (canFn && CodeCalls(MiPtr(mi), canFn, kCallScan) ? 1 : 0);
            if (score > bestScore) {
                bestScore = score;
                best = mi;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/ofu");
        return nullptr;
    }
    return best;
}

void* ResolveCanApplyFn(void* sampleMob) {
    void* data = ReadPtr(sampleMob, kFbMobData);
    if (!LooksLikeHeapPtr(data)) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    void* dataKlass = nullptr;
    if (e.objectGetClass) {
        __try {
            dataKlass = e.objectGetClass(data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/dataKlass");
            dataKlass = nullptr;
        }
    }
    if (!dataKlass) return nullptr;
    // 不走 Kind unique：MobData 上 bool() 很多。walkParents：派生类 objectGetClass 也能命中。
    using x::runtime::il2cpp_method::FindMethodByName;
    using x::runtime::il2cpp_method::FindMethodByRva;
    if (void* mi = FindMethodByName(dataKlass, kHashCanApplyCtrl, 0, true)) return MiPtr(mi);
    if (void* mi = FindMethodByName(dataKlass, "CanApplyCtrl", 0, true)) return MiPtr(mi);
    if (void* mi = FindMethodByRva(dataKlass, kRvaDumpCanApplyCtrl, true)) return MiPtr(mi);
    return nullptr;
}

void* ResolvePriFn(void* mobKlass) {
    using x::runtime::il2cpp_method::FindMethodResolved;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    MethodShape sh{};
    sh.arity = 4;
    sh.ret = TypeKind::I32;
    sh.param[0] = TypeKind::I32;
    sh.param[1] = TypeKind::I32;
    sh.param[2] = TypeKind::I32;
    sh.param[3] = TypeKind::I32;
    sh.unique = true;
    sh.walkParents = false;
    const auto mr = FindMethodResolved(mobKlass, kRvaDumpCalcPriority, sh, "calc_priority",
                                       kHashCalcPriority);
    void* fn = MiPtr(mr.method);
    if (fn && !LooksLikeTinyStub(fn)) return fn;

    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods) return nullptr;
    void* best = nullptr;
    int hits = 0;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(mobKlass, &iter);
            if (!raw) break;
            if (MethodArity(raw) != 4) continue;
            void* cand = MiPtr(raw);
            if (!cand || LooksLikeTinyStub(cand)) continue;
            if (!ScanCallerOf(mobKlass, cand)) continue;
            ++hits;
            best = cand;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/pri");
        return nullptr;
    }
    return hits == 1 ? best : nullptr;
}

void* ResolveMobKlass(void* sampleMob) {
    void* mobKlass = x::runtime::il2cpp::FindClass("", kMobClass);
    if (mobKlass) return mobKlass;
    if (!sampleMob) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.objectGetClass) return nullptr;
    __try {
        return e.objectGetClass(sampleMob);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("ApplyCtrl/mobKlass");
        return nullptr;
    }
}

void LogApplyMiss(const char* why, const char* via, uint32_t rva, int can, int pri) {
    const DWORD now = GetTickCount();
    if (gLastApplyMissLog != 0 && now - gLastApplyMissLog < 10000) return;
    gLastApplyMissLog = now;
    x::runtime::LogW("MobGather", "ApplyControl resolve miss why=%s via=%s rva=0x%X can=%d pri=%d",
                     why, via ? via : "", rva, can, pri);
}

bool ResolveApplyMi(void* sampleMob) {
    if (gMiApply && MiPtr(gMiApply)) return true;
    using x::runtime::il2cpp_method::FindMethodByName;
    void* mobKlass = ResolveMobKlass(sampleMob);
    if (!mobKlass) {
        gApplyHow = "no_klass";
        LogApplyMiss("no_klass", "", 0, 0, 0);
        return false;
    }
    void* canFn = ResolveCanApplyFn(sampleMob);
    void* priFn = ResolvePriFn(mobKlass);

    void* mi = nullptr;
    const char* how = "";
    if (void* named = FindMethodByName(mobKlass, kHashApplyControl, 1, false)) {
        if (ApplyShapeOk(named)) {
            mi = named;
            how = "hash";
        }
    }
    if (!mi) {
        if (void* named = FindMethodByName(mobKlass, "ApplyControl", 1, false)) {
            if (ApplyShapeOk(named)) {
                mi = named;
                how = "plain";
            }
        }
    }
    if (!mi && priFn) {
        mi = ScanCallerOf(mobKlass, priFn);
        if (mi) how = "priority_xref";
    }
    if (!mi && canFn) {
        mi = ScanCallerOf(mobKlass, canFn);
        if (mi) how = "canapply_xref";
    }
    if (!mi) {
        mi = ScanOnFixedCallees(mobKlass, priFn, canFn);
        if (mi) how = "onfixed_xref";
    }
    if (!mi || !ApplyShapeOk(mi) || !CallsPriOrCan(mi, priFn, canFn)) {
        gApplyHow = "stub_or_miss";
        LogApplyMiss(mi ? "no_pri_can_e8" : "stub_or_miss", how, mi ? MiRva(mi) : 0,
                     canFn ? 1 : 0, priFn ? 1 : 0);
        gMiApply = nullptr;
        return false;
    }
    gMiApply = mi;
    gApplyRva = MiRva(mi);
    gApplyHow = how;
    x::runtime::LogI("MobGather",
                     "ApplyControl resolved via=%s rva=0x%X can=%d pri=%d can_e8=%d pri_e8=%d", how,
                     gApplyRva, canFn ? 1 : 0, priFn ? 1 : 0,
                     canFn && CodeCalls(MiPtr(mi), canFn, kCallScan) ? 1 : 0,
                     priFn && CodeCalls(MiPtr(mi), priFn, kCallScan) ? 1 : 0);
    return true;
}

void ApplyCtrlWave(void** mobs, const int32_t* ids, int n, int* applied, int* seh, int* tCurOut) {
    if (applied) *applied = 0;
    if (seh) *seh = 0;
    if (tCurOut) *tCurOut = 0;
    if (n <= 0) return;
    const DWORD now = GetTickCount();
    if (gLastApplyTick != 0 && now - gLastApplyTick < kApplyPeriodMs) return;
    if (!ResolveApplyMi(mobs[0])) return;
    const int tCur = x::features::ports::skill::GetGameUpdateTimeMs();
    if (tCurOut) *tCurOut = tCur;
    if (tCur <= 0) return;
    auto* fn = reinterpret_cast<FnApplyControl>(MiPtr(gMiApply));
    if (!fn) return;
    gLastApplyTick = now;
    int ok = 0;
    int fail = 0;
    for (int i = 0; i < n; ++i) {
        void* mob = mobs[i];
        if (!LooksLikeHeapPtr(mob)) continue;
        __try {
            fn(mob, tCur, gMiApply);
            ++ok;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("Mob.ApplyControl");
            ++fail;
        }
    }
    if (applied) *applied = ok;
    if (seh) *seh = fail;
    x::runtime::LogI("MobGather", "apply n=%d ok=%d seh=%d tCur=%d rva=0x%X via=%s sample=%d", n, ok,
                     fail, tCur, gApplyRva, gApplyHow ? gApplyHow : "", ids && n > 0 ? ids[0] : 0);
}

float GravityLoss(DWORD dtMs) {
    if (dtMs < 1) dtMs = kDtDefaultMs;
    if (dtMs > kDtCapMs) dtMs = kDtCapMs;
    return 60.f * (static_cast<float>(dtMs) / 30.f);
}

struct HoldItem {
    void* mob = nullptr;
    int32_t id = 0;
    int32_t ctrl = 0;
    float ap0x = 0.f;
    float ap0y = 0.f;
    float ap0vy = 0.f;
    float cmdVx = 0.f;
    float cmdVy = 0.f;
    int onFh0 = 0;
    int vcAct0 = 0;
    bool pushed = false;
    bool detach = false;
    bool live = false;
    const char* why = "skip";
};

struct HoldJob {
    HoldItem items[kMaxHold]{};
    void* applyMob[kMaxHold]{};
    int32_t applyId[kMaxHold]{};
    int n = 0;
    int nApply = 0;
    int applied = 0;
    int applySeh = 0;
    int tCur = 0;
    bool applyOn = false;
    float playerX = 0.f;
    float playerY = 0.f;
    float gLoss = 80.f;
    unsigned sinceMs = 30;
    const char* why = "";
};

void HoldJobFn(void* p) {
    auto* job = static_cast<HoldJob*>(p);
    if (!job) return;
    if (!world::IsPlayReady()) {
        job->why = "not_play_ready";
        return;
    }
    if (job->applyOn && job->nApply > 0) {
        ApplyCtrlWave(job->applyMob, job->applyId, job->nApply, &job->applied, &job->applySeh,
                      &job->tCur);
    }
    for (int i = 0; i < job->n; ++i) {
        HoldItem& it = job->items[i];
        float x = 0.f, y = 0.f, vx = 0.f, vy = 0.f;
        int act = 0, onFh = 0;
        void* vc = PickLiveVc(it.mob, &x, &y, &vx, &vy, &act, &onFh);
        it.ap0x = x;
        it.ap0y = y;
        it.ap0vy = vy;
        it.onFh0 = onFh;
        it.vcAct0 = act;
        if (!vc) {
            it.why = "not_live";
            continue;
        }
        it.live = true;
        if (!x::features::ports::mob::StillSameLiveMob(it.mob, it.id, nullptr)) {
            it.why = "dead";
            continue;
        }
        if (!x::features::ports::mob_fh_ban::EnsureInstalledOnPump(vc) ||
            !x::features::ports::mob_fh_ban::Arm(vc, it.mob, it.id, job->playerX, job->playerY)) {
            it.why = "arm";
            continue;
        }
        it.detach = true;
        x::features::ports::mob_fh_ban::ClearFh(vc);
        x::features::ports::mob_fh_ban::ComputeSetVelocity(
            x, y, vx, vy, job->playerX, job->playerY, job->sinceMs, &it.cmdVx, &it.cmdVy);
        it.pushed = true;
        it.why = "ok";
    }
    job->why = "ok";
}

}  // namespace

void Init() {
    gOn.store(false, std::memory_order_release);
    gEncounterPause.store(false, std::memory_order_release);
    gHoldUntil.store(0, std::memory_order_release);
    gLastHoldTick = 0;
    gLastApplyTick = 0;
    gMiApply = nullptr;
    gApplyRva = 0;
    gApplyHow = "";
    gLastApplyMissLog = 0;
    gQuietSinceMs.store(0, std::memory_order_release);
    ClearOidTrack();
    gSpawnGateMapId = 0;
    gSpawnGateOn = false;
    gSeekLatchOn = 0;
    gSeekSettleMs = 0;
    gSeekNearMs = 0;
    gFlyKind = 0;
    gHomePending.store(0, std::memory_order_release);
    gHomeSawPlay = 0;
    gHomeHaveLastMap = 0;
    gHomeLastMapId = 0;
    gSeeking.store(0, std::memory_order_release);
    x::features::ports::mob_fh_ban::Init();
    x::runtime::LogI("MobGather", "port init (OFF; live-sim hold; apply-ctrl opt-in)");
}

void Shutdown() {
    gOn.store(false, std::memory_order_release);
    gEncounterPause.store(false, std::memory_order_release);
    gHoldUntil.store(0, std::memory_order_release);
    StopSeekFly("shutdown");
    x::features::ports::mob_fh_ban::ClearAll();
    x::features::ports::mob_fh_ban::Shutdown();
}

void SetEnabled(bool on) {
    if (!on && gDyRampOn.load(std::memory_order_acquire) != 0) return;
    const bool prev = gOn.exchange(on, std::memory_order_acq_rel);
    if (!on) {
        gHoldUntil.store(0, std::memory_order_release);
        StopSeekFly("disabled");
        x::features::ports::mob_fh_ban::ClearAll();
        gLastHoldTick = 0;
        gQuietSinceMs.store(0, std::memory_order_release);
    } else if (!prev) {
        x::features::encounter::InvalidateOccupancy();
    }
    if (prev != on) {
        x::runtime::LogI("MobGather", "enabled=%d (vtol cdf)", on ? 1 : 0);
    }
}

bool IsEnabled() {
    return gOn.load(std::memory_order_acquire);
}

bool IsHoldActive() {
    if (gEncounterPause.load(std::memory_order_acquire)) return false;
    if (gOn.load(std::memory_order_acquire)) return true;
    const DWORD until = gHoldUntil.load(std::memory_order_acquire);
    return until != 0 && GetTickCount() < until;
}

void SetEncounterPause(bool on) {
    const bool prev = gEncounterPause.exchange(on, std::memory_order_acq_rel);
    if (on == prev) return;
    if (on) {
        if (gFlyKind == 2) gHomePending.store(1, std::memory_order_release);
        StopSeekFly("encounter_pause");
        x::features::ports::mob_fh_ban::ClearAll();
        gLastHoldTick = 0;
        x::runtime::LogI("MobGather", "encounter pause on (disarm; keep home pending)");
    } else {
        x::runtime::LogI("MobGather", "encounter pause off");
    }
}

bool IsEncounterPaused() {
    return gEncounterPause.load(std::memory_order_acquire);
}

float GatherRadiusPx() { return gRadiusPx.load(std::memory_order_acquire); }

void SetSpeedPct(unsigned pct) {
    float scale = static_cast<float>(pct) / 100.f;
    x::features::ports::mob_fh_ban::SetSpeedScale(scale);
}

void SetAntiJitter(bool on) { x::features::ports::mob_fh_ban::SetAntiJitter(on); }

void SetMaxHold(unsigned n) {
    const int cap = static_cast<int>(xcat::ClampMobGatherMax(n ? n : xcat::kMobGatherMaxDefault));
    gMaxHold.store(cap, std::memory_order_release);
    x::features::ports::mob_fh_ban::SetMaxArmed(cap);
}

void SetFarInFlight(unsigned n) {
    gFarInFlightMax.store(static_cast<int>(xcat::ClampMobGatherFarInFlight(n)),
                          std::memory_order_release);
}

void SetRadiusPx(unsigned px) {
    const float r = static_cast<float>(
        xcat::ClampMobGatherRadiusPx(px ? px : xcat::kMobGatherRadiusDefaultPx));
    gRadiusPx.store(r, std::memory_order_release);
}

void SetLayerYPx(unsigned px) {
    gLayerYPx.store(static_cast<float>(xcat::ClampMobGatherLayerYPx(px)),
                    std::memory_order_release);
}

void SetDyLimPx(unsigned px) {
    const float y = static_cast<float>(xcat::ClampMobGatherDyLimPx(px));
    gDyLimUser.store(y, std::memory_order_release);
    if (gDyRampOn.load(std::memory_order_acquire) == 0)
        gDyLim.store(y, std::memory_order_release);
}

void SetWalkDx(unsigned px) {
    gWalkReadyDx.store(static_cast<float>(xcat::ClampMobGatherWalkDx(px)),
                       std::memory_order_release);
}

void SetFeetExemptPx(unsigned px) {
    const float v = static_cast<float>(xcat::ClampMobGatherFeetExemptPx(px));
    gFeetExemptPx.store(v, std::memory_order_release);
}

void SetHoldTimeoutMs(unsigned ms) {
    const unsigned c = xcat::ClampMobGatherHoldMs(ms ? ms : xcat::kMobGatherHoldMsDefault);
    gHoldTimeoutMs.store(c, std::memory_order_release);
    x::features::ports::mob_fh_ban::SetArmTimeoutMs(c);
}

void SetRecruitIntervalMs(unsigned ms) {
    const unsigned c =
        xcat::ClampMobGatherIntervalMs(ms ? ms : xcat::kMobGatherIntervalDefaultMs);
    gRecruitMs.store(c, std::memory_order_release);
}

unsigned RecruitIntervalMs() { return gRecruitMs.load(std::memory_order_acquire); }

void SetAimIntervalMs(unsigned ms) {
    const unsigned c = xcat::ClampMobGatherAimMs(ms ? ms : xcat::kMobGatherAimMsDefault);
    gAimMs.store(c, std::memory_order_release);
}

unsigned AimIntervalMs() { return gAimMs.load(std::memory_order_acquire); }

void SetIgnoreQuiet(bool on) { gIgnoreQuiet.store(on ? 1 : 0, std::memory_order_release); }

void SetQuietDelayMs(unsigned ms) {
    gQuietDelayMs.store(xcat::ClampMobGatherQuietDelayMs(ms), std::memory_order_release);
}

void SetApplyCtrl(bool on) {
    const uint8_t v = on ? 1 : 0;
    const uint8_t prev = gApplyCtrl.exchange(v, std::memory_order_acq_rel);
    if (prev != v) {
        gLastApplyTick = 0;
        gMiApply = nullptr;
        gApplyRva = 0;
        gApplyHow = "";
        gLastApplyMissLog = 0;
        x::runtime::LogI("MobGather", "applyCtrl=%d (official ApplyControl, no pri hook)", on ? 1 : 0);
    }
}

void SetSoftRelogin(bool on, unsigned sec) {
    gSoftRelogin.store(on ? 1 : 0, std::memory_order_release);
    gSoftReloginSec.store(xcat::ClampMobGatherSoftReloginSec(
                              sec ? sec : xcat::kMobGatherSoftReloginSecDefault),
                          std::memory_order_release);
}

void TickSoftRelogin() {
    using x::features::ports::world::GetSceneState;
    using x::features::ports::world::IsInMapScene;
    using x::features::ports::world::IsPlayReady;
    using x::features::ports::world::SceneState;
    using x::features::soft_login_probe::IsArmed;
    using x::features::soft_login_probe::IsReconnectInFlight;
    using x::features::soft_login_probe::RequestProactiveReconnect;

    // 计时：勾了 + 吸怪开着 + 在图里才起表。关着吸怪不起钟，避免一点开就拆会话。
    if (gSoftRelogin.load(std::memory_order_acquire) == 0 || !IsEnabled() ||
        IsEncounterPaused()) {
        gSoftArmMs = 0;
        gSoftPauseMs = 0;
        return;
    }

    const DWORD now = GetTickCount();
    if (!IsInMapScene()) {
        gSoftArmMs = 0;
        gSoftPauseMs = 0;
        return;
    }
    if (!gSoftArmMs) {
        gSoftArmMs = now ? now : 1;
        x::runtime::LogI("MobGather", "soft relogin clock start inMap");
    }

    // hold 期间冻结已走的秒数，避免 14s 间隔叠上重连墙钟、一落地立刻再拆。
    // 寻簇飞行 / 高度闸扫描同套：这段不吃 14s。
    if (IsReconnectInFlight() || PersonFlyBusy() ||
        gDyRampOn.load(std::memory_order_acquire) != 0) {
        if (!gSoftPauseMs) gSoftPauseMs = now ? now : 1;
        return;
    }
    if (gSoftPauseMs) {
        if (gSoftArmMs) gSoftArmMs += (now - gSoftPauseMs);
        gSoftPauseMs = 0;
    }

    const unsigned needMs =
        xcat::ClampMobGatherSoftReloginSec(gSoftReloginSec.load(std::memory_order_acquire)) *
        1000u;
    if (now - gSoftArmMs < needMs) return;

    if (!IsEnabled() || IsEncounterPaused()) return;
    if (GetSceneState() != SceneState::Field || !IsPlayReady()) return;
    if (x::features::channel_hop::GetState() != x::features::channel_hop::State::Idle ||
        x::features::channel_hop::HasPending())
        return;
    if (x::features::auto_lie::IsBusy() || x::features::auto_supply::IsBusy() ||
        x::features::char_boot::IsBusy() || x::features::sellbag::IsBusy())
        return;

    if (!IsArmed()) {
        if (!gSoftSkipLogMs || now - gSoftSkipLogMs > 10000) {
            gSoftSkipLogMs = now;
            x::runtime::LogW("MobGather",
                             "soft relogin skip: homepage 软重连试连 is off (no CloseSession)");
        }
        gSoftArmMs = now;
        return;
    }

    x::runtime::LogI("MobGather", "soft relogin fire after %us", needMs / 1000u);
    if (RequestProactiveReconnect("mob_gather_timer")) {
        gSoftArmMs = 0;
        gSoftPauseMs = 0;
        x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
            x::features::notify::NotificationKind::Info, "mob-gather-soft", "吸怪定时软重连",
            "已主动拆会话，走软重连回图", 5000});
    } else {
        gSoftArmMs = now - needMs + 5000;
    }
}

void QuerySoftReloginClock(unsigned* on, unsigned* paused, unsigned* remainMs, unsigned* needMs) {
    using x::features::soft_login_probe::IsReconnectInFlight;
    const unsigned need =
        xcat::ClampMobGatherSoftReloginSec(gSoftReloginSec.load(std::memory_order_acquire)) *
        1000u;
    const unsigned enabled = gSoftRelogin.load(std::memory_order_acquire) != 0 ? 1u : 0u;
    if (on) *on = enabled;
    if (needMs) *needMs = need;
    if (!enabled || !gSoftArmMs) {
        if (paused) *paused = 0;
        if (remainMs) *remainMs = 0xFFFFFFFFu;
        return;
    }
    const DWORD now = GetTickCount();
    const bool freeze = gSoftPauseMs != 0 || IsReconnectInFlight() || PersonFlyBusy();
    const DWORD mark = gSoftPauseMs ? gSoftPauseMs : now;
    const DWORD elapsed = freeze ? (mark - gSoftArmMs) : (now - gSoftArmMs);
    unsigned remain = 0;
    if (elapsed < need) remain = need - elapsed;
    if (paused) *paused = freeze ? 1u : 0u;
    if (remainMs) *remainMs = remain;
}

namespace {

void ResetClearWave() {
    gClearWaveN = 0;
    gClearWaveClosed = false;
    gClearEmptyMs = 0;
    gClearLastNewMs = 0;
    gClearWipeSeen = x::features::ports::mob_fh_ban::WipeGeneration();
}

bool ClearWaveHas(int32_t id) {
    if (id == 0) return false;
    for (int i = 0; i < gClearWaveN; ++i) {
        if (gClearWaveId[i] == id) return true;
    }
    return false;
}

bool ClearWaveAdd(int32_t id) {
    if (id == 0 || ClearWaveHas(id)) return false;
    if (gClearWaveN >= kMaxHold) return false;
    gClearWaveId[gClearWaveN++] = id;
    return true;
}

void MaybeCloseClearWave(int nArmed, DWORD now) {
    if (gClearWaveClosed || gClearWaveN <= 0) return;
    const int cap = gMaxHold.load(std::memory_order_acquire);
    const bool full = cap > 0 && (gClearWaveN >= cap || nArmed >= cap);
    const bool quiet =
        gClearLastNewMs != 0 && now - gClearLastNewMs >= 800;
    if (!full && !quiet) return;
    gClearWaveClosed = true;
    gClearEmptyMs = 0;
    x::runtime::LogI("MobGather", "clear relogin wave close n=%d armed=%d why=%s", gClearWaveN,
                     nArmed, full ? "cap" : "quiet");
}

bool WaveIdStillLive(int32_t id, const mob::Snapshot& snap) {
    if (id == 0) return false;
    for (int i = 0; i < snap.count; ++i) {
        const mob::MobLite& m = snap.mobs[i];
        if (m.id != id) continue;
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) return false;
        return true;
    }
    return false;
}

bool ClearReloginHoldWaveOnly() {
    // 已冻结、或已开火等落地：空位不再补新怪（同时=1 时死一只会立刻再吸下一只）。
    return gClearRelogin.load(std::memory_order_acquire) != 0 &&
           (gClearWaveClosed || gClearFired);
}

}  // namespace

void SetClearRelogin(bool on) {
    gClearRelogin.store(on ? 1 : 0, std::memory_order_release);
}

void SetSeekCluster(bool on) {
    if (on) {
        const uint8_t prevHome = gHomeReturnOn.exchange(0, std::memory_order_acq_rel);
        if (prevHome != 0) {
            gHomePending.store(0, std::memory_order_release);
            if (gFlyKind == 2) StopSeekFly("seek_mutex");
            x::runtime::LogI("MobGather", "homeReturn=0 (mutex seek_cluster)");
        }
    }
    if (on && gDyRampOn.load(std::memory_order_acquire) != 0) {
        const uint8_t prev = gSeekClusterOn.exchange(0, std::memory_order_acq_rel);
        if (prev != 0) StopSeekFly("dylim_ramp");
        return;
    }
    const uint8_t v = on ? 1 : 0;
    const uint8_t prev = gSeekClusterOn.exchange(v, std::memory_order_acq_rel);
    if (prev != 0 && v == 0) StopSeekFly("opt_off");
    if (prev != v) {
        x::runtime::LogI("MobGather", "seekCluster=%d (fly to densest pack before arm)",
                         on ? 1 : 0);
    }
}

void TickSeekCluster() {
    using x::features::simple_combat::heli::Owner;
    namespace heli = x::features::simple_combat::heli;

    if (gHomeReturnOn.load(std::memory_order_acquire) != 0 || gFlyKind == 2) return;
    if (gDyRampOn.load(std::memory_order_acquire) != 0) {
        StopSeekFly("dylim_ramp");
        return;
    }
    if (gSeekClusterOn.load(std::memory_order_acquire) == 0 || !IsEnabled() ||
        IsEncounterPaused()) {
        StopSeekFly("off");
        return;
    }
    if (x::features::simple_combat::IsSafeLandActive()) {
        StopSeekFly("safe_land");
        return;
    }

    teleport::FlightState st{};
    const bool haveSt = teleport::QueryFlightState(st) && st.ok;
    const bool owned = heli::CurrentOwner() == Owner::Gather;
    const DWORD now = GetTickCount();
    const int nm = x::features::kick_sniff::LastSessionState();
    if (nm == 0 || nm == 1) {
        StopSeekFly("nm_down");
        return;
    }

    const bool hold = x::features::soft_login_probe::IsHoldActive();
    const bool landQ = x::features::soft_login_probe::IsLandQuiet();
    const bool play = x::features::ports::world::IsPlayReady();
    const bool yieldPeer = x::features::travel::IsActive() ||
                           heli::CurrentOwner() == Owner::Fly ||
                           heli::CurrentOwner() == Owner::Travel;
    if (yieldPeer) {
        gSeekSettleMs = 0;
        ReleaseSeekRotor();
        gSeeking.store(1, std::memory_order_release);
        static DWORD sYieldLog = 0;
        if (!sYieldLog || now - sYieldLog > 800) {
            sYieldLog = now;
            x::runtime::LogI("MobGather", "seek_cluster yield owner=%s",
                             heli::OwnerName(heli::CurrentOwner()));
        }
        return;
    }
    if (!haveSt) {
        StopSeekFly("no_st");
        return;
    }

    if (gSeekSettleMs) {
        const SeekLandPoll land = PollSeekLand(st, now, "seek_cluster");
        if (land != SeekLandPoll::RetryFly) return;
    }

    if (hold || landQ || !play) {
        if (owned && !st.onFh) {
            gSeeking.store(1, std::memory_order_release);
            (void)heli::Tick(Owner::Gather, now, nullptr);
            return;
        }
        StopSeekFly(hold ? "hold" : (landQ ? "land_quiet" : "not_play"));
        return;
    }

    if (x::features::ports::mob_fh_ban::ArmedCount() > 0) {
        StopSeekFly("holding");
        return;
    }

    if (!gSeekLatchOn) {
        mob::Snapshot snap{};
        DenseCluster pack{};
        if (!mob::GetCached(snap) || !snap.ok || !FindDenseCluster(snap, st.y, &pack)) {
            StopSeekFly("no_pack");
            return;
        }
        float lx = pack.cx;
        float ly = pack.cy;
        if (!SnapSeekLand(pack.cx, pack.cy, st.y, 0.f, &lx, &ly) &&
            !SnapSeekLand(pack.cx, st.y, st.y, 0.f, &lx, &ly)) {
            StopSeekFly("no_fh");
            return;
        }
        const float ldx = st.x - lx;
        const float ldy = st.y - ly;
        const float ldist = std::sqrt(ldx * ldx + ldy * ldy);
        if (ldist <= kSeekArriveHypotPx) {
            StopSeekFly("same_layer");
            return;
        }
        gSeekLatchOn = 1;
        gFlyKind = 1;
        gSeekLatchX = lx;
        gSeekLatchY = ly;
        x::runtime::LogI("MobGather",
                         "seek_cluster latch c=(%.0f,%.0f) snap=(%.0f,%.0f) n=%d "
                         "ap=(%.0f,%.0f) dY=%.0f layerY=%.0f",
                         pack.cx, pack.cy, lx, ly, pack.n, st.x, st.y, st.y - ly,
                         gLayerYPx.load(std::memory_order_relaxed));
    }

    DriveSeekFly(st, now, "seek_cluster");
}

bool IsSeekingCluster() { return PersonFlyBusy(); }

void SetHomeReturn(bool on) {
    if (on) {
        const uint8_t prevSeek = gSeekClusterOn.exchange(0, std::memory_order_acq_rel);
        if (prevSeek != 0) {
            if (gFlyKind == 1) StopSeekFly("home_mutex");
            x::runtime::LogI("MobGather", "seekCluster=0 (mutex home_return)");
        }
    }
    const uint8_t v = on ? 1 : 0;
    const uint8_t prev = gHomeReturnOn.exchange(v, std::memory_order_acq_rel);
    if (prev != 0 && v == 0) {
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("opt_off");
    }
    if (prev != v) {
        x::runtime::LogI("MobGather",
                         "homeReturn=%d (fly to recorded AbsPos after soft reenter)",
                         on ? 1 : 0);
    }
}

void SetHomePos(int32_t x, int32_t y, int32_t mapId, bool valid, bool hasMap) {
    gHomeX.store(xcat::ClampMobGatherStandOffX(x), std::memory_order_release);
    gHomeY.store(xcat::ClampMobGatherStandOffY(y), std::memory_order_release);
    gHomeMapId.store(mapId, std::memory_order_release);
    gHomeValid.store(valid ? 1 : 0, std::memory_order_release);
    gHomeHasMap.store(hasMap ? 1 : 0, std::memory_order_release);
}

bool RecordHomeNow() {
    teleport::FlightState st{};
    if (!teleport::QueryFlightState(st) || !st.ok) {
        x::runtime::LogW("MobGather", "home_return record miss (no AbsPos)");
        return false;
    }
    if (!st.onFh) {
        x::runtime::LogW("MobGather", "home_return record refuse (not onFh ma=%d)", st.ma);
        return false;
    }
    if (st.ma == 6 || st.ma == 7) {
        x::runtime::LogW("MobGather", "home_return record refuse (air ma=%d)", st.ma);
        return false;
    }
    if (!x::features::ports::world::HasMapData()) {
        x::runtime::LogW("MobGather", "home_return record refuse (no map)");
        return false;
    }
    const uint8_t had = gHomeValid.load(std::memory_order_acquire) != 0 &&
                        gHomeHasMap.load(std::memory_order_acquire) != 0 ? 1 : 0;
    const int mapId = x::features::ports::world::GetMapId();
    const int32_t hx = xcat::ClampMobGatherStandOffX(static_cast<int32_t>(st.x));
    const int32_t hy = xcat::ClampMobGatherStandOffY(static_cast<int32_t>(st.y));
    SetHomePos(hx, hy, mapId, true, true);
    x::ipc::PayloadControl_PublishHomePos(hx, hy, mapId, true);
    x::runtime::LogI("MobGather", "home_return record ap=(%d,%d) map=%d refresh=%d", hx, hy, mapId,
                     had);
    return true;
}

void TickHomeRecord() {
    const uint32_t seq = xcat::ReadMobGatherHomeRecordSeq(x::runtime::GetBinDir());
    static uint8_t sBoot = 0;
    static uint32_t sLast = 0;
    if (!sBoot) {
        sBoot = 1;
        sLast = seq;
        return;
    }
    if (seq == 0 || seq == sLast) return;
    sLast = seq;
    (void)RecordHomeNow();
}

void RecordHomeOnF5() {
    if (gHomeReturnOn.load(std::memory_order_acquire) == 0) return;
    (void)RecordHomeNow();
}

void TickHomeReturn() {
    using x::features::simple_combat::heli::Owner;
    namespace heli = x::features::simple_combat::heli;

    if (gDyRampOn.load(std::memory_order_acquire) != 0) {
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("dylim_ramp");
        return;
    }
    if (gHomeReturnOn.load(std::memory_order_acquire) == 0) {
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("off");
        return;
    }
    if (!IsEnabled()) {
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("off");
        return;
    }
    if (IsEncounterPaused()) {
        if (gFlyKind == 2) {
            gHomePending.store(1, std::memory_order_release);
            StopSeekFly("encounter_pause");
        }
        return;
    }

    const bool hold = x::features::soft_login_probe::IsHoldActive();
    const bool inflight = x::features::soft_login_probe::IsReconnectInFlight();
    const bool homeReady = gHomeValid.load(std::memory_order_acquire) != 0 &&
                           gHomeHasMap.load(std::memory_order_acquire) != 0;
    if (hold || inflight) {
        if (homeReady && gHomePending.load(std::memory_order_acquire) == 0) {
            gHomePending.store(1, std::memory_order_release);
            x::runtime::LogI("MobGather", "home_return pending (soft reenter)");
        }
        if (gFlyKind == 2) StopSeekFly("hold");
        return;
    }

    if (x::features::simple_combat::IsSafeLandActive()) {
        if (gFlyKind == 2) {
            gHomePending.store(1, std::memory_order_release);
            StopSeekFly("safe_land");
        }
        return;
    }

    teleport::FlightState st{};
    const bool haveSt = teleport::QueryFlightState(st) && st.ok;
    const bool owned = heli::CurrentOwner() == Owner::Gather;
    const DWORD now = GetTickCount();
    const int nm = x::features::kick_sniff::LastSessionState();
    if (nm == 0 || nm == 1) {
        if (gFlyKind == 2) StopSeekFly("nm_down");
        return;
    }

    const bool landQ = x::features::soft_login_probe::IsLandQuiet();
    const bool play = x::features::ports::world::IsPlayReady();
    const bool yieldPeer = x::features::travel::IsActive() ||
                           heli::CurrentOwner() == Owner::Fly ||
                           heli::CurrentOwner() == Owner::Travel ||
                           x::features::channel_hop::GetState() !=
                               x::features::channel_hop::State::Idle ||
                           x::features::channel_hop::HasPending() ||
                           x::features::auto_lie::IsBusy() ||
                           x::features::auto_supply::IsBusy() ||
                           x::features::char_boot::IsBusy() ||
                           x::features::sellbag::IsBusy();
    if (yieldPeer) {
        if (gFlyKind == 2 || gHomePending.load(std::memory_order_acquire) != 0) {
            gSeekSettleMs = 0;
            ReleaseSeekRotor();
            gSeeking.store(1, std::memory_order_release);
            static DWORD sYieldLog = 0;
            if (!sYieldLog || now - sYieldLog > 800) {
                sYieldLog = now;
                x::runtime::LogI("MobGather", "home_return yield owner=%s",
                                 heli::OwnerName(heli::CurrentOwner()));
            }
        }
        return;
    }

    if (landQ || !play) {
        gHomeSawPlay = 0;
        if (!haveSt) {
            if (gFlyKind == 2) StopSeekFly("no_st");
            return;
        }
        if (owned && !st.onFh && gFlyKind == 2) {
            gSeeking.store(1, std::memory_order_release);
            (void)heli::Tick(Owner::Gather, now, nullptr);
            return;
        }
        if (gFlyKind == 2) StopSeekFly(landQ ? "land_quiet" : "not_play");
        return;
    }

    const bool already =
        gHomePending.load(std::memory_order_acquire) != 0 || gFlyKind == 2;
    if (!already) {
        if (!x::features::ports::world::HasMapData() || !haveSt || !st.onFh) return;
    } else if (!haveSt) {
        if (gFlyKind == 2) StopSeekFly("no_st");
        return;
    }

    const bool playEdge = gHomeSawPlay == 0;
    gHomeSawPlay = 1;

    if (gHomeValid.load(std::memory_order_acquire) == 0) {
        if (playEdge) {
            x::runtime::LogW("MobGather",
                             "home_return skip (no recorded AbsPos; F5 开打怪或点「记录人物坐标」)");
        }
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("no_pos");
        return;
    }

    if (!x::features::ports::world::HasMapData()) return;
    const int mapId = x::features::ports::world::GetMapId();
    const bool mapEdge = gHomeHaveLastMap == 0 || mapId != gHomeLastMapId;
    gHomeHaveLastMap = 1;
    gHomeLastMapId = mapId;

    if (gHomeHasMap.load(std::memory_order_acquire) == 0) {
        if (playEdge) {
            x::runtime::LogW("MobGather",
                             "home_return skip (no map id; F5 开打怪或点「记录人物坐标」)");
        }
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("no_map");
        return;
    }
    {
        const int32_t homeMap = gHomeMapId.load(std::memory_order_acquire);
        if (mapId != homeMap) {
            gHomePending.store(0, std::memory_order_release);
            if (gFlyKind == 2) StopSeekFly("map");
            return;
        }
    }

    const float homeX = static_cast<float>(gHomeX.load(std::memory_order_acquire));
    const float homeY = static_cast<float>(gHomeY.load(std::memory_order_acquire));
    const float adx = st.x - homeX;
    const float ady = st.y - homeY;
    const float away = std::sqrt(adx * adx + ady * ady);

    if (gHomePending.load(std::memory_order_acquire) == 0 && gFlyKind != 2) {
        if ((playEdge || mapEdge) && away > kSeekArriveHypotPx) {
            gHomePending.store(1, std::memory_order_release);
            x::runtime::LogI("MobGather",
                             "home_return pending (enter map) ap=(%.0f,%.0f) home=(%.0f,%.0f) "
                             "d=%.0f playEdge=%d mapEdge=%d map=%d",
                             st.x, st.y, homeX, homeY, away, playEdge ? 1 : 0, mapEdge ? 1 : 0,
                             mapId);
        } else {
            return;
        }
    }

    if (gSeekSettleMs) {
        const SeekLandPoll land = PollSeekLand(st, now, "home_return");
        if (land != SeekLandPoll::RetryFly) return;
    }

    if (gFlyKind != 2) {
        if (x::features::ports::mob_fh_ban::ArmedCount() > 0)
            x::features::ports::mob_fh_ban::ClearAll();
        gFlyKind = 2;
        gSeekLatchOn = 1;
        float lx = static_cast<float>(gHomeX.load(std::memory_order_acquire));
        float ly = static_cast<float>(gHomeY.load(std::memory_order_acquire));
        float sx = lx;
        float sy = ly;
        if (SnapSeekLand(lx, ly, ly, kSeekSnapHomeMaxPx, &sx, &sy)) {
            lx = sx;
            ly = sy;
        }
        gSeekLatchX = lx;
        gSeekLatchY = ly;
        x::runtime::LogI("MobGather", "home_return latch ap=(%.0f,%.0f) home=(%.0f,%.0f)", st.x,
                         st.y, gSeekLatchX, gSeekLatchY);
    }

    DriveSeekFly(st, now, "home_return");
}

void TickDyLimRamp() {
    const uint32_t seq = xcat::ReadMobGatherDyRampSeq(x::runtime::GetBinDir());
    const DWORD now = GetTickCount();
    if (seq == 0) {
        if (gDyRampOn.exchange(0, std::memory_order_acq_rel) != 0) {
            const float y = gDyLimUser.load(std::memory_order_acquire);
            gDyLim.store(y, std::memory_order_release);
            x::runtime::LogI("MobGather", "dylim_ramp stop restore=%.0f", y);
        }
        gDyRampLastSeq = 0;
        return;
    }
    if (seq != gDyRampLastSeq) {
        gDyRampLastSeq = seq;
        gDyRampOn.store(1, std::memory_order_release);
        gDyRampLastMs = now ? now : 1;
        gDyLim.store(1.f, std::memory_order_release);
        SetEnabled(true);
        SetSeekCluster(false);
        gHomePending.store(0, std::memory_order_release);
        if (gFlyKind == 2) StopSeekFly("dylim_ramp");
        x::features::ports::mob_fh_ban::ClearAll();
        ClearOidTrack();
        gSpawnGateOn = false;
        x::runtime::LogI("MobGather",
                         "dylim_ramp start seq=%u dyLim=1 r=%.0f (auto-on, no-seek, "
                         "+100/s to 2000; soft-relogin/clear paused)",
                         seq, kDyRampRadiusPx);
        return;
    }
    const int nm = x::features::kick_sniff::LastSessionState();
    if (nm == 0 || nm == 1) {
        if (gDyRampOn.load(std::memory_order_acquire) != 0) {
            x::runtime::LogW("MobGather", "dylim_ramp KICK nm=%d dyLim=%.0f", nm,
                             gDyLim.load(std::memory_order_acquire));
            gDyRampOn.store(0, std::memory_order_release);
            const float y = gDyLimUser.load(std::memory_order_acquire);
            gDyLim.store(y, std::memory_order_release);
        }
        return;
    }
    if (gDyRampOn.load(std::memory_order_acquire) == 0) return;
    if (x::features::soft_login_probe::IsHoldActive() ||
        x::features::soft_login_probe::IsLandQuiet() ||
        !x::features::ports::world::IsPlayReady()) {
        return;
    }
    if (now - gDyRampLastMs < 1000) return;
    gDyRampLastMs = now;
    float cur = gDyLim.load(std::memory_order_acquire);
    cur += 100.f;
    if (cur > 2000.f) {
        gDyRampOn.store(0, std::memory_order_release);
        const float y = gDyLimUser.load(std::memory_order_acquire);
        gDyLim.store(y, std::memory_order_release);
        x::runtime::LogI("MobGather", "dylim_ramp cap 2000 no-kick restore=%.0f", y);
        return;
    }
    gDyLim.store(cur, std::memory_order_release);
    x::runtime::LogI("MobGather", "dylim_ramp step dyLim=%.0f", cur);
}

void TickClearRelogin() {
    using x::features::ports::world::GetSceneState;
    using x::features::ports::world::IsPlayReady;
    using x::features::ports::world::SceneState;
    using x::features::soft_login_probe::IsArmed;
    using x::features::soft_login_probe::IsReconnectInFlight;
    using x::features::soft_login_probe::RequestProactiveReconnect;

    constexpr DWORD kWaveEmptySettleMs = 400;

    const bool want = IsEnabled() && !IsEncounterPaused() &&
                      gDyRampOn.load(std::memory_order_acquire) == 0 &&
                      gClearRelogin.load(std::memory_order_acquire) != 0;
    if (!want) {
        ResetClearWave();
        gClearWaitReadyMs = 0;
        gClearFired = false;
        return;
    }

    const DWORD now = GetTickCount();
    if (IsReconnectInFlight()) {
        ResetClearWave();
        gClearWaitReadyMs = 0;
        gClearFired = true;
        return;
    }

    if (gClearFired) {
        if (GetSceneState() == SceneState::Field && IsPlayReady()) {
            if (gClearWaitReadyMs == 0) gClearWaitReadyMs = now;
            else if (now - gClearWaitReadyMs >= 2000) {
                gClearFired = false;
                gClearWaitReadyMs = 0;
                ResetClearWave();
                x::runtime::LogI("MobGather", "clear relogin round restart after land");
            }
        } else {
            gClearWaitReadyMs = 0;
        }
        return;
    }

    if (GetSceneState() != SceneState::Field || !IsPlayReady()) {
        ResetClearWave();
        return;
    }
    if (x::features::channel_hop::GetState() != x::features::channel_hop::State::Idle ||
        x::features::channel_hop::HasPending())
        return;
    if (x::features::auto_lie::IsBusy() || x::features::auto_supply::IsBusy() ||
        x::features::char_boot::IsBusy() || x::features::sellbag::IsBusy())
        return;

    const uint32_t wipe = x::features::ports::mob_fh_ban::WipeGeneration();
    if (gClearWipeSeen == 0) gClearWipeSeen = wipe;
    if (wipe != gClearWipeSeen) {
        if (gClearWaveN > 0) {
            x::runtime::LogI("MobGather", "clear relogin abort wipe n=%d closed=%d", gClearWaveN,
                             gClearWaveClosed ? 1 : 0);
        }
        ResetClearWave();
        return;
    }

    int32_t armedIds[kMaxHold]{};
    const int nArmed = x::features::ports::mob_fh_ban::CopyArmedIds(armedIds, kMaxHold);
    if (!gClearWaveClosed) {
        bool added = false;
        for (int i = 0; i < nArmed; ++i) {
            if (ClearWaveAdd(armedIds[i])) added = true;
        }
        if (added) gClearLastNewMs = now;
        if (gClearWaveN > 0 && gClearLastNewMs == 0) gClearLastNewMs = now;
        MaybeCloseClearWave(nArmed, now);
        return;
    }

    mob::Snapshot snap{};
    const bool snapOk = mob::GetCached(snap) && snap.ok;
    int live = 0;
    int escaped = 0;
    if (snapOk) {
        for (int i = 0; i < gClearWaveN; ++i) {
            const int32_t id = gClearWaveId[i];
            if (!WaveIdStillLive(id, snap)) continue;
            bool stillArmed = false;
            for (int j = 0; j < nArmed; ++j) {
                if (armedIds[j] == id) {
                    stillArmed = true;
                    break;
                }
            }
            if (stillArmed) ++live;
            else ++escaped;
        }
    } else {
        live = gClearWaveN;
    }

    if (live > 0) {
        gClearEmptyMs = 0;
        return;
    }
    if (escaped > 0) {
        x::runtime::LogI("MobGather", "clear relogin abort escaped n=%d escaped=%d", gClearWaveN,
                         escaped);
        ResetClearWave();
        return;
    }

    if (gClearEmptyMs == 0) gClearEmptyMs = now;
    if (now - gClearEmptyMs < kWaveEmptySettleMs) return;

    if (!IsArmed()) {
        if (!gClearSkipLogMs || now - gClearSkipLogMs > 10000) {
            gClearSkipLogMs = now;
            x::runtime::LogW("MobGather",
                             "clear relogin skip: homepage 软重连试连 is off (no CloseSession)");
        }
        gClearEmptyMs = now;
        return;
    }

    x::runtime::LogI("MobGather", "clear relogin fire n=%d", gClearWaveN);
    if (RequestProactiveReconnect("mob_gather_clear")) {
        gClearFired = true;
        char body[96]{};
        snprintf(body, sizeof(body), "本轮 %d 只已清干净，主动拆会话回图", gClearWaveN);
        ResetClearWave();
        x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
            x::features::notify::NotificationKind::Info, "mob-gather-clear", "清怪重连", body,
            5000});
    } else {
        gClearEmptyMs = now;
        x::runtime::LogW("MobGather", "clear relogin fire but CloseSession skipped n=%d",
                         gClearWaveN);
    }
}

bool PeriodicLogOk() {
    static DWORD sLast = 0;
    const DWORD now = GetTickCount();
    if (sLast != 0 && now - sLast < 1000) return false;
    sLast = now;
    return true;
}

bool TryHoldBatch(OneshotResult* out, bool verbose) {
    OneshotResult local{};
    if (!out) out = &local;
    *out = OneshotResult{};
    out->why = "empty";
    const char* kind = verbose ? "oneshot" : "tick";
    float offX = 0.f;
    float offY = 0.f;
    x::features::ports::mob_fh_ban::QueryGatherStandOff(&offX, &offY);

    auto skipLog = [&](const char* why, unsigned leftMs = 0) {
        if (verbose || PeriodicLogOk()) {
            x::runtime::LogI("MobGather",
                             "%s skip why=%s left=%u qdelay=%u max=%d dyLim=%.0f layerY=%.0f "
                             "walkDx=%.0f feet=%.0f off=%.0f,%.0f",
                             kind, why, leftMs,
                             gQuietDelayMs.load(std::memory_order_relaxed),
                             gMaxHold.load(std::memory_order_relaxed),
                             gDyLim.load(std::memory_order_relaxed),
                             gLayerYPx.load(std::memory_order_relaxed),
                             gWalkReadyDx.load(std::memory_order_relaxed),
                             gFeetExemptPx.load(std::memory_order_relaxed), offX, offY);
        }
    };

    // F6 / F5 禁挂台只拦 LocalUser klass，与怪侧 CD/CDF 白名单不抢虚表。
    // 叠怪必须在人飞时继续跟 AbsPos + Ap.V 前馈（随机飞要靠这条）。

    if (gEncounterPause.load(std::memory_order_acquire)) {
        out->why = "encounter";
        skipLog("encounter");
        return false;
    }

    // 未采到人数 / 同图有人：不开吸。避免别人看见角色在吸怪。
    const int others = x::features::encounter::LastOtherCount();
    if (others != 0) {
        x::features::ports::mob_fh_ban::ClearAll();
        out->why = (others < 0) ? "wait_scan" : "others";
        skipLog(out->why);
        return false;
    }

    teleport::FlightState st{};
    if (!teleport::QueryFlightState(st) || !st.ok) {
        gQuietSinceMs.store(0, std::memory_order_release);
        out->why = "no_flight";
        skipLog("no_flight");
        return false;
    }

    const int nm = x::features::kick_sniff::LastSessionState();
    if (nm == 0 || nm == 1) {
        gQuietSinceMs.store(0, std::memory_order_release);
        x::features::ports::mob_fh_ban::ClearAll();
        out->why = "nm_down";
        skipLog("nm_down");
        return false;
    }
    const bool hold = x::features::soft_login_probe::IsHoldActive();
    const bool landQ = x::features::soft_login_probe::IsLandQuiet();
    const bool postAir = x::features::soft_login_probe::IsPostSoftAirCombatBlocked();
    const bool ignoreQuiet = gIgnoreQuiet.load(std::memory_order_acquire) != 0;
    const unsigned quietDelay = gQuietDelayMs.load(std::memory_order_acquire);
    // 软重连 hold = 还没进图站稳。hold / 离图 / 掉线清延时钟。落地也吸只管落地静默。
    if (hold && !landQ) {
        gQuietSinceMs.store(0, std::memory_order_release);
        x::features::ports::mob_fh_ban::ClearAll();
        out->why = "land_quiet";
        skipLog("hold");
        return false;
    }
    const bool landGate = landQ || postAir;
    const DWORD now = GetTickCount();
    DWORD since = gQuietSinceMs.load(std::memory_order_acquire);
    // 整模块延时：进图站稳后起表。关吸怪 / hold / 离图清钟。不绑落地也吸。
    if (since == 0) {
        since = now ? now : 1;
        gQuietSinceMs.store(since, std::memory_order_release);
        ClearOidTrack();
    }
    if (landGate && !ignoreQuiet) {
        x::features::ports::mob_fh_ban::ClearAll();
        out->why = "land_quiet";
        skipLog("land_quiet");
        return false;
    }
    if (quietDelay > 0 && now - since < quietDelay) {
        x::features::ports::mob_fh_ban::ClearAll();
        out->why = "quiet_delay";
        skipLog("quiet_delay", quietDelay - (now - since));
        return false;
    }

    mob::Snapshot snap{};
    if (!mob::GetCached(snap) || !snap.ok) {
        out->why = "no_snap";
        skipLog("no_snap");
        return false;
    }
    int32_t liveIds[mob::kMaxLiteMobs]{};
    int nLiveIds = 0;
    for (int i = 0; i < snap.count && nLiveIds < mob::kMaxLiteMobs; ++i) {
        const int32_t id = snap.mobs[i].id;
        if (id != 0) liveIds[nLiveIds++] = id;
    }
    x::features::ports::mob_fh_ban::SweepStale(st.y, liveIds, nLiveIds);
    if (snap.mapId > 0 && snap.mapId != gSpawnGateMapId) {
        gSpawnGateMapId = snap.mapId;
        ClearOidTrack();
        gSpawnGateOn = false;
    }
    PruneOidTrack(liveIds, nLiveIds);
    if (snap.count <= 0) {
        out->why = "no_snap";
        skipLog("no_snap");
        return false;
    }

    DenseCluster pack{};
    if (PersonFlyBusy()) {
        x::features::ports::mob_fh_ban::ClearAll();
        out->why = (gFlyKind == 2 || gHomePending.load(std::memory_order_acquire) != 0)
                       ? "home_return"
                       : "seek_cluster";
        if (verbose || PeriodicLogOk()) {
            x::runtime::LogI("MobGather", "%s skip why=%s latch=(%.0f,%.0f) py=%.0f", kind,
                             out->why, gSeekLatchX, gSeekLatchY, st.y);
        }
        return false;
    }

    struct Cand {
        const mob::MobLite* m = nullptr;
        void* vc = nullptr;
        float ad = 0.f;
        int32_t ctrl = 0;
        bool armed = false;
    };
    Cand cands[mob::kMaxLiteMobs]{};
    int nCand = 0;
    int skippedFixed = 0;
    int skippedRemote = 0;
    int skippedSpawn = 0;
    int skippedAir = 0;
    int skippedDy = 0;
    int skippedPool = 0;
    int32_t skipWalkId = 0;
    float skipWalkDx = 0.f;
    float skipWalkDxMax = 0.f;
    float skipWalkX = 0.f;
    float skipWalkY = 0.f;
    float skipWalkVy = 0.f;
    int skipWalkFh = 0;
    int nPassive = 0;
    int nOurs = 0;
    int nLive = 0;
    const bool applyOn = gApplyCtrl.load(std::memory_order_acquire) != 0;
    float radius = gRadiusPx.load(std::memory_order_acquire);
    if (gDyRampOn.load(std::memory_order_acquire) != 0 && radius < kDyRampRadiusPx) {
        radius = kDyRampRadiusPx;
    }
    for (int i = 0; i < snap.count; ++i) {
        const mob::MobLite& m = snap.mobs[i];
        if (!LooksLikeHeapPtr(m.ptr) || !m.ready || m.deadType != 0) continue;
        if (m.ctrl > 0) ++nOurs;
        else ++nPassive;
        if (IsFixedOrImmovable(m.ptr)) {
            ++skippedFixed;
            continue;
        }
        float x = 0.f, y = 0.f, vy = 0.f;
        int onFh = 0;
        int split = 0;
        void* liveVc = PickLiveVc(m.ptr, &x, &y, nullptr, &vy, nullptr, &onFh, &split);
        float adLive = 1.e9f;
        if (split) {
            ++skippedPool;
            continue;
        }
        if (liveVc) {
            ++nLive;
            const float dx = x - st.x;
            const float dy = y - st.y;
            adLive = std::sqrt(dx * dx + dy * dy);
        } else {
            ++skippedRemote;
        }
        if (!liveVc) continue;
        if (PosVsApPool(m.x, m.y, x, y)) {
            ++skippedPool;
            continue;
        }
        const bool known = OidKnown(m.id);
        NoteOid(m.id, x, y);
        if (known && OidPoolJump(m.id, x, y)) {
            ++skippedPool;
            continue;
        }
        const bool keepArmed = x::features::ports::mob_fh_ban::IsArmed(liveVc) &&
                               x::features::ports::mob::StillSameLiveMob(m.ptr, m.id, nullptr);
        if (adLive > radius && !keepArmed) continue;
        // 清怪重连本轮已冻结：只维持这一批，禁止 Arm 新怪。
        if (ClearReloginHoldWaveOnly() && (!keepArmed || !ClearWaveHas(m.id))) continue;
        // 新 oid：横移不够、未贴地/掉落、或竖跨出步行网 → 不 Arm。脚边照吸。
        const float feet = gFeetExemptPx.load(std::memory_order_acquire);
        if (!keepArmed && adLive > feet) {
            float dHome = 0.f;
            if (gDyRampOn.load(std::memory_order_acquire) == 0 &&
                WalkHoldNew(m.id, x, vy, onFh, &dHome)) {
                ++skippedSpawn;
                if (onFh == 0 || vy < kWalkMaxDropVy) ++skippedAir;
                if (dHome > skipWalkDxMax) skipWalkDxMax = dHome;
                if (skipWalkId == 0 || dHome < skipWalkDx) {
                    skipWalkId = m.id;
                    skipWalkDx = dHome;
                    skipWalkX = x;
                    skipWalkY = y;
                    skipWalkVy = vy;
                    skipWalkFh = onFh;
                }
                continue;
            }
            if (HeightHoldNew(y, st.y)) {
                ++skippedDy;
                continue;
            }
        }
        if (nCand >= mob::kMaxLiteMobs) break;
        cands[nCand].m = &m;
        cands[nCand].vc = liveVc;
        cands[nCand].ad = adLive;
        cands[nCand].ctrl = m.ctrl;
        cands[nCand].armed = keepArmed;
        ++nCand;
    }
    for (int a = 0; a < nCand; ++a) {
        for (int b = a + 1; b < nCand; ++b) {
            const bool aArmed = cands[a].armed;
            const bool bArmed = cands[b].armed;
            const bool aOurs = cands[a].ctrl > 0;
            const bool bOurs = cands[b].ctrl > 0;
            const bool swap = (bArmed && !aArmed) ||
                              (bArmed == aArmed && bOurs && !aOurs) ||
                              (bArmed == aArmed && bOurs == aOurs && cands[b].ad < cands[a].ad);
            if (!swap) continue;
            const Cand t = cands[a];
            cands[a] = cands[b];
            cands[b] = t;
        }
    }

    DWORD dt = (gLastHoldTick == 0) ? kDtDefaultMs : (now - gLastHoldTick);
    gLastHoldTick = now;

    float aimX = st.x;
    float aimY = st.y;
    x::features::ports::mob_fh_ban::TickPlayerAim(st.x, st.y, st.vx, st.vy, st.ma, &aimX, &aimY);

    HoldJob job{};
    job.playerX = aimX;
    job.playerY = aimY;
    job.gLoss = GravityLoss(dt);
    job.sinceMs = 30;
    job.applyOn = applyOn;
    int cap = gMaxHold.load(std::memory_order_acquire);
    if (cap < 1) cap = 1;
    if (cap > kMaxHold) cap = kMaxHold;
    const float cruiseR = x::features::ports::mob_fh_ban::CruiseRadius();
    const float stationR = x::features::ports::mob_fh_ban::StationRadius();
    // 圈外新收上限：TAB「在途」。0=不限。脚边（巡航圈内）仍全收。
    const float farPx = cruiseR > 1.f ? cruiseR : 140.f;
    const int farCap = gFarInFlightMax.load(std::memory_order_acquire);
    int farInFlight = 0;
    if (farCap > 0) {
        for (int i = 0; i < nCand; ++i) {
            if (cands[i].armed && cands[i].ad > farPx) ++farInFlight;
        }
    }
    int nNew = 0;
    int nFar = 0;
    int nSta = 0;
    int nSkipFar = 0;
    int nFarAdmit = 0;
    for (int i = 0; i < nCand && job.n < cap; ++i) {
        if (ClearReloginHoldWaveOnly() && !cands[i].armed) continue;
        const bool isFar = cands[i].ad > farPx;
        if (farCap > 0 && !cands[i].armed && isFar) {
            if (farInFlight >= farCap) {
                ++nSkipFar;
                continue;
            }
            ++farInFlight;
            ++nFarAdmit;
        }
        if (!cands[i].armed) ++nNew;
        if (cands[i].ad <= stationR) ++nSta;
        else if (cands[i].ad > cruiseR) ++nFar;
        HoldItem& it = job.items[job.n++];
        it.mob = cands[i].m->ptr;
        it.id = cands[i].m->id;
        it.ctrl = cands[i].ctrl;
        ++out->considered;
        if (applyOn && it.ctrl <= 0 && job.nApply < kMaxHold) {
            job.applyMob[job.nApply] = it.mob;
            job.applyId[job.nApply] = it.id;
            ++job.nApply;
        }
    }

    if (job.n <= 0 && job.nApply <= 0) {
        out->why = (nLive <= 0) ? "no_live"
                                : ((skippedSpawn > 0 || skippedDy > 0 || skippedPool > 0) ? "walk"
                                                                                          : "empty");
        if (verbose || PeriodicLogOk()) {
            x::runtime::LogI("MobGather",
                             "%s considered=0 pushed=0 why=%s n=%d live=%d ours=%d passive=%d "
                             "fixed=%d remote=%d skipSpawn=%d skipAir=%d skipDy=%d skipPool=%d "
                             "holdId=%d dHome=%.0f dHomeMax=%.0f ap=%.0f,%.0f "
                             "onFh=%d vy=%.0f spawnN=%d gate=%d applyOn=%d qdelay=%u dyLim=%.0f "
                             "packN=%d packY=%.0f layerY=%.0f walkDx=%.0f feet=%.0f "
                             "off=%.0f,%.0f",
                             kind, out->why, snap.count, nLive, nOurs, nPassive, skippedFixed,
                             skippedRemote, skippedSpawn, skippedAir, skippedDy, skippedPool,
                             skipWalkId, skipWalkDx, skipWalkDxMax, skipWalkX, skipWalkY, skipWalkFh,
                             skipWalkVy, snap.spawnPointN, gSpawnGateOn ? 1 : 0, applyOn ? 1 : 0,
                             gQuietDelayMs.load(std::memory_order_relaxed),
                             gDyLim.load(std::memory_order_relaxed), pack.n, pack.cy,
                             gLayerYPx.load(std::memory_order_relaxed),
                             gWalkReadyDx.load(std::memory_order_relaxed),
                             gFeetExemptPx.load(std::memory_order_relaxed),
                             offX, offY);
        }
        x::features::ports::mob_fh_ban::SweepStale(st.y);
        return false;
    }

    if (!x::runtime::main_thread::InvokeAndWait(&HoldJobFn, &job, kJobWaitMs,
                                                x::runtime::main_thread::JobPrio::Normal)) {
        out->why = "pump";
        x::runtime::LogW("MobGather", "%s pump timeout n=%d", kind, job.n);
        return false;
    }
    if (job.why && job.why[0] && std::strcmp(job.why, "ok") != 0) {
        out->why = job.why;
        x::runtime::LogW("MobGather", "%s job why=%s n=%d", kind, job.why, job.n);
        return false;
    }

    int nDetach = 0;
    for (int i = 0; i < job.n; ++i) {
        const HoldItem& it = job.items[i];
        if (it.pushed) ++out->pushed;
        if (it.detach) ++nDetach;
        if (!verbose) continue;
        float ap1x = 0.f, ap1y = 0.f;
        int onFh1 = 0, vcAct1 = -1;
        (void)PickLiveVc(it.mob, &ap1x, &ap1y, nullptr, nullptr, &vcAct1, &onFh1);
        x::runtime::LogI("MobGather",
                         "id=%d ctrl=%d live=%d detach=%d cmd=(%.0f,%.1f) onFh0=%d onFh1=%d "
                         "vcAct0=%d ap0=%.1f,%.1f ap1=%.1f,%.1f why=%s",
                         it.id, it.ctrl, it.live ? 1 : 0, it.detach ? 1 : 0, it.cmdVx, it.cmdVy,
                         it.onFh0, onFh1, it.vcAct0, it.ap0x, it.ap0y, ap1x, ap1y,
                         it.why ? it.why : "");
    }
    if (gClearRelogin.load(std::memory_order_acquire) != 0 && !gClearWaveClosed && !gClearFired) {
        bool added = false;
        for (int i = 0; i < job.n; ++i) {
            if (!job.items[i].pushed || job.items[i].id == 0) continue;
            if (ClearWaveAdd(job.items[i].id)) added = true;
        }
        const DWORD t = GetTickCount();
        if (added) gClearLastNewMs = t;
        if (gClearWaveN > 0 && gClearLastNewMs == 0) gClearLastNewMs = t;
        MaybeCloseClearWave(x::features::ports::mob_fh_ban::ArmedCount(), t);
    }
    if (out->pushed > 0) gSpawnGateOn = true;
    out->why = "ok";
    if (verbose || PeriodicLogOk()) {
        const HoldItem* s = nullptr;
        float maxAd = 0.f;
        float maxCmd = 0.f;
        for (int i = 0; i < job.n; ++i) {
            const HoldItem& it = job.items[i];
            const float dx = it.ap0x - aimX;
            const float dy = it.ap0y - aimY;
            const float ad = std::sqrt(dx * dx + dy * dy);
            const float cm = std::sqrt(it.cmdVx * it.cmdVx + it.cmdVy * it.cmdVy);
            if (!s || ad > maxAd) {
                s = &it;
                maxAd = ad;
            }
            if (cm > maxCmd) maxCmd = cm;
        }
        x::runtime::LogI("MobGather",
                         "%s considered=%d new=%d sta=%d far=%d skipFar=%d farAdm=%d skipSpawn=%d "
                         "skipAir=%d skipDy=%d skipPool=%d holdId=%d dHome=%.0f dHomeMax=%.0f "
                         "onFh=%d vy=%.0f "
                         "spawnN=%d gate=%d pushed=%d "
                         "detach=%d why=ok "
                         "live=%d ours=%d passive=%d remote=%d aimDt=%u gLoss=%.0f py=%.1f "
                         "aim=%.1f,%.1f faceL=%d scale=%.2f jitter=%d max=%d r=%.0f hold=%u "
                         "iv=%u quiet=%d qdelay=%u apply=%d/%d seh=%d tCur=%d "
                         "waveHold=%d waveN=%d dyLim=%.0f packN=%d packY=%.0f layerY=%.0f "
                         "walkDx=%.0f feet=%.0f off=%.0f,%.0f "
                         "sample id=%d ctrl=%d(%s) cmd=(%.0f,%.0f) ap=%.1f,%.1f vy=%.0f "
                         "maxAd=%.0f maxCmd=%.0f",
                         kind, out->considered, nNew, nSta, nFar, nSkipFar, nFarAdmit, skippedSpawn,
                         skippedAir, skippedDy, skippedPool, skipWalkId, skipWalkDx, skipWalkDxMax,
                         skipWalkFh, skipWalkVy, snap.spawnPointN, gSpawnGateOn ? 1 : 0, out->pushed,
                         nDetach, nLive,
                         nOurs, nPassive, skippedRemote,
                         x::features::ports::mob_fh_ban::LastAimDtMs(), job.gLoss, st.y, aimX,
                         aimY, st.ma >= 0 ? (st.ma & 1) : -1,
                         x::features::ports::mob_fh_ban::SpeedScale(),
                         x::features::ports::mob_fh_ban::AntiJitterEnabled() ? 1 : 0,
                         gMaxHold.load(std::memory_order_relaxed),
                         radius,
                         gHoldTimeoutMs.load(std::memory_order_relaxed),
                         gRecruitMs.load(std::memory_order_relaxed),
                         gIgnoreQuiet.load(std::memory_order_relaxed) ? 1 : 0,
                         gQuietDelayMs.load(std::memory_order_relaxed),
                         job.applied, job.nApply, job.applySeh, job.tCur,
                         ClearReloginHoldWaveOnly() ? 1 : 0, gClearWaveN,
                         gDyLim.load(std::memory_order_relaxed),
                         pack.n, pack.cy,
                         gLayerYPx.load(std::memory_order_relaxed),
                         gWalkReadyDx.load(std::memory_order_relaxed),
                         gFeetExemptPx.load(std::memory_order_relaxed),
                         offX, offY,
                         s ? s->id : 0,
                         s ? s->ctrl : 0, mob::CtrlName(s ? s->ctrl : 0), s ? s->cmdVx : 0.f,
                         s ? s->cmdVy : 0.f, s ? s->ap0x : 0.f, s ? s->ap0y : 0.f,
                         s ? s->ap0vy : 0.f, maxAd, maxCmd);
    }
    x::features::ports::mob_fh_ban::SweepStale(st.y);
    return out->pushed > 0;
}

bool TryPushOneshot(OneshotResult* out) {
    const DWORD until = GetTickCount() + kOneshotHoldMs;
    gHoldUntil.store(until, std::memory_order_release);
    x::runtime::LogI("MobGather", "oneshot hold window %ums", kOneshotHoldMs);
    return TryHoldBatch(out, true);
}

bool TryPushPeriodic(OneshotResult* out) { return TryHoldBatch(out, false); }

void TickHoldWatch() {
    if (gOn.load(std::memory_order_acquire)) return;
    const DWORD until = gHoldUntil.load(std::memory_order_acquire);
    if (until == 0) return;
    if (GetTickCount() < until) return;
    gHoldUntil.store(0, std::memory_order_release);
    x::features::ports::mob_fh_ban::ClearAll();
    gLastHoldTick = 0;
    x::runtime::LogI("MobGather", "oneshot hold end, remount");
}

void TickLandSweep() {
    teleport::FlightState st{};
    if (!teleport::QueryFlightState(st) || !st.ok) return;
    x::features::ports::mob_fh_ban::SweepStale(st.y);
}

}  // namespace x::features::ports::mob_gather
