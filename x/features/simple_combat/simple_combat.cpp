// Classic TWMS — simple_combat explicit state machine (full redesign).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "simple_combat.h"

#include "../auto_supply/auto_supply.h"
#include "../kick_sniff/kick_sniff.h"
#include "../multi_skill/multi_skill.h"
#include "../ports/attack_input_port.h"
#include "../ports/foothold_path.h"
#include "../ports/foothold_port.h"
#include "../ports/input_port.h"
#include "../ports/mob_pool_port.h"
#include "../ports/multi_skill_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/teleport_port.h"
#include "../ports/world_port.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../../common/xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x::features::simple_combat {
namespace {

constexpr DWORD kIdleSleepMs = 8;
// 同层：优先同 zMass（FH 连通域）；图未就绪时退回 |ΔY|≤45。
// SnapStandAt / 落点贴台仍用此 Y 带做「点是否贴在台上」。
constexpr float kSameLayerY = 45.f;
// 命中带：|dx| ≤ standOff * 此系数。略放宽减少贴边空砍（仍远小于 ×2 空砍档）。
constexpr float kHitBandMaxFrac = 1.35f;
// 禁塌怪心：落点相对怪台至少离开这么多（枫星对照：halfW+playerHalf / kMinCombatStandoff）。
constexpr float kMinLandAway = 10.f;
// 同层重贴最小 hop：贴近站距下怪走开几十像素也要 TP 回侧，否则空砍。
constexpr float kMinReapproachHop = 24.f;
// LiveStep 微贴：仅短 hop 才算 hug（短 Settling / 低 minHop）。
// BIN：LiveStep∧同层一律 hug=1 → hop=700 仍短收态 → 客服脱同步 + whiff。
constexpr float kMinHugHop = 4.f;
constexpr float kHugMaxHop = 80.f;  // hop≥此值：同层也走远距 tp_ok
// 微贴短 Settling（CD 与远距共用面板「冷却时间」）。压低以缩短首刀前等待。
constexpr DWORD kHugSettleMs = 0;
// 相对 hug 落点的死区：超出才微跳。
constexpr float kHugDeadzoneMin = 12.f;
constexpr float kHugDeadzoneFrac = 0.35f;
// 原地连刀过久翻侧重贴（对照枫星 standstill shuffle；无 LiveStep，改 fill+Doing）。
constexpr DWORD kStandstillShuffleMs = 8000;
constexpr DWORD kStandstillShuffleCooldownMs = 2500;
// BIN：攻击加速 30ms 连刀时，出刀当下 +whiff 会在血条刷新前误换怪。
// 改为：出刀只武装观察窗；窗内掉血清零；窗满仍未掉血才 +1；满 N 次再换。
constexpr int kWhiffFireN = 3;
constexpr DWORD kWhiffObserveMs = 200;  // 覆盖加速连刀 + hpPct 刷新滞后
constexpr DWORD kWhiffSoftBanMs = 120;
constexpr DWORD kDeadSoftBanMs = 400;  // 挡尸体鬼锁；过短会被同 tick ClearSoftBan/再选吃掉
constexpr DWORD kNoLandSoftBanMs = 200;
constexpr int kSpecialTplFilter = 9999999;
// fill+Doing 后互斥窗：近距可近 0；远跳必须更长，否则换图后短 CD 连跳脱同步。
constexpr DWORD kTeleportSettleShortMs = 0;
constexpr DWORD kTeleportSettleMidMs = 8;
constexpr DWORD kTeleportSettleFarMs = 64;
constexpr DWORD kTeleportSettleMegaMs = 120;
constexpr float kSettleShortHop = 120.f;
constexpr float kSettleMidHop = 350.f;
constexpr float kSettleFarHop = 600.f;
// 单次贴怪 hop 上限：BIN 换图后曾 hop=1768 settle=16 → 客服脱同步。
constexpr float kMaxApproachHop = 700.f;
// 大 hop 强制 CD 地板（面板可更长，不可更短）。
constexpr DWORD kHopCdFloorMidMs = 80;
constexpr DWORD kHopCdFloorFarMs = 120;
// 换图 / 重新 PlayReady 后短暂禁止贴怪与出刀；同图热开 F5 不走此宽限。
// 800ms 体感「换图开闸慢」；300ms 挡住卸图瞬间远跳即可。
constexpr DWORD kMapArmGraceMs = 300;
constexpr int kSoftBanCap = 16;

DWORD SettleMsForHop(float hop, bool hug) {
    if (hug) return kHugSettleMs;
    if (!std::isfinite(hop) || hop < kSettleShortHop) return kTeleportSettleShortMs;
    if (hop < kSettleMidHop) return kTeleportSettleMidMs;
    if (hop < kSettleFarHop) return kTeleportSettleFarMs;
    return kTeleportSettleMegaMs;
}

DWORD EffectiveTeleportCdMs(float hop, DWORD panelCd) {
    DWORD floor = panelCd;
    if (std::isfinite(hop)) {
        if (hop >= kSettleFarHop) {
            if (floor < kHopCdFloorFarMs) floor = kHopCdFloorFarMs;
        } else if (hop >= kSettleMidHop) {
            if (floor < kHopCdFloorMidMs) floor = kHopCdFloorMidMs;
        }
    }
    return floor;
}

enum class State : uint8_t {
    Idle = 0,
    Acquire,
    MoveTo,
    Settling,
    Aim,
    Firing,
    Recover,
};

const char* StateName(State s) {
    switch (s) {
        case State::Idle:
            return "Idle";
        case State::Acquire:
            return "Acquire";
        case State::MoveTo:
            return "MoveTo";
        case State::Settling:
            return "Settling";
        case State::Aim:
            return "Aim";
        case State::Firing:
            return "Firing";
        case State::Recover:
            return "Recover";
        default:
            return "?";
    }
}

std::atomic<bool> gEnabled{false};
std::atomic<DWORD> gTickIntervalMs{xcat::kSimpleCombatTickDefaultMs};
std::atomic<bool> gExternalPause{false};      // 有效态：绝对 OR 深度
std::atomic<bool> gExternalPauseAbs{false};   // auto_lie / encounter / channel_hop
std::atomic<int> gExternalPauseDepth{0};      // buffs / timed_keys 可重叠
DWORD gMapArmUntilMs = 0;
int gLastMapId = -1;
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gTeleportEnabled{false};  // Phase1 默认关
std::atomic<bool> gLiveStepEnabled{false};  // 锁怪后同层微贴；默认关
std::atomic<bool> gClusterPriority{false};  // 群怪优先；默认关
std::atomic<uint32_t> gTeleportStandOff{xcat::kCombatTeleportStandOffDefault};
std::atomic<uint32_t> gTeleportCooldownMs{xcat::kCombatTeleportCooldownDefaultMs};
std::atomic<uint32_t> gTeleportMinDx{xcat::kCombatTeleportMinDxDefault};

// 踢号压测：随机贴怪 fill+Doing，CD 由慢→快，断线记 last_ok_cd（combat.log）。
constexpr DWORD kKickStressStartMs = 2000;
constexpr DWORD kKickStressStepMs = 50;
constexpr DWORD kKickStressFloorMs = 50;
// 服端限速多半是「窗口内次数」；每级太少容易扫过阈值却攒不够触发量。
constexpr int kKickStressHopsPerLevel = 8;
// 细扫：50ms 未踢；本档 50→0（步进 5），验证间隔是否还是因素。
constexpr DWORD kKickStressFineStartMs = 50;
constexpr DWORD kKickStressFineStepMs = 5;
constexpr DWORD kKickStressFineFloorMs = 0;
constexpr int kKickStressFineHopsPerLevel = 12;
// 钉地板：上次 5ms 断线；本档只压到 10ms，看能否 floor_reached_no_kick。
constexpr DWORD kKickStressFine10StartMs = 30;
constexpr DWORD kKickStressFine10StepMs = 5;
constexpr DWORD kKickStressFine10FloorMs = 10;
constexpr int kKickStressFine10HopsPerLevel = 12;
// 原地短跳：排除距离——同台 ±120px 来回；CD 扫档对齐 fine0-50，便于对照。
constexpr DWORD kKickStressLocalStartMs = 50;
constexpr DWORD kKickStressLocalStepMs = 5;
constexpr DWORD kKickStressLocalFloorMs = 0;
constexpr int kKickStressLocalHopsPerLevel = 12;
constexpr float kKickLocalHopPx = 120.f;
// 滚动窗口：验证「累计嫌疑」假说（跳数 / hop 米数）。
constexpr DWORD kKickWinShortMs = 15000;
constexpr DWORD kKickWinLongMs = 60000;
constexpr int kKickWinCap = 512;

std::atomic<bool> gKickStressActive{false};
DWORD gKickNextDueMs = 0;
DWORD gKickIntervalMs = kKickStressStartMs;
DWORD gKickLastOkMs = 0;
DWORD gKickStartMs = 0;
uint32_t gKickHopOk = 0;
uint32_t gKickHopFail = 0;
int gKickHopsAtLevel = 0;
bool gKickSawDiscAtStart = false;
DWORD gKickCfgStartMs = kKickStressStartMs;
DWORD gKickCfgStepMs = kKickStressStepMs;
DWORD gKickCfgFloorMs = kKickStressFloorMs;
int gKickCfgHopsPerLevel = kKickStressHopsPerLevel;
char gKickModeTag[24] = "wide";
bool gKickLocalShuttle = false;
float gKickLeftX = 0.f;
float gKickRightX = 0.f;
float gKickY = 0.f;
uint32_t gKickFh = 0;
bool gKickGoRight = true;

struct KickHopSample {
    DWORD t = 0;
    float hop = 0.f;
};
KickHopSample gKickWin[kKickWinCap]{};
int gKickWinNext = 0;
int gKickWinCount = 0;
float gKickMetersTotal = 0.f;
float gKickHopMax = 0.f;
uint32_t gKickHopGe500 = 0;
uint32_t gKickHopGe1000 = 0;

HANDLE gLog = INVALID_HANDLE_VALUE;
DWORD gLastTick = 0;
bool gF5WasDown = false;
bool gF11WasDown = false;
DWORD gLastF11Ms = 0;

State gState = State::Idle;
DWORD gStateEnterMs = 0;
DWORD gSettleUntil = 0;
float gSettleX = 0.f;
float gSettleY = 0.f;
uint32_t gSettleFh = 0;

struct LockState {
    void* ptr = nullptr;
    int32_t id = 0;
    int32_t lastHp = -1;
    int whiff = 0;
    // 出刀后待确认：臂于首次出刀，窗满未掉血才计入 whiff。
    int32_t armHp = -1;
    DWORD armUntil = 0;
    int firesInArm = 0;
    float x = 0.f;
    float y = 0.f;
};
LockState gLock{};
const char* gLastLockLostWhy = "lost";
// 落点侧粘滞（对照枫星 g_targetSide）：-1=怪左 / +1=怪右；朝向另算，不绑此值。
int gLandSide = 0;
DWORD gStandstillSince = 0;
DWORD gStandstillShuffleLast = 0;
float gStandstillAnchorX = 0.f;
float gStandstillAnchorY = 0.f;

struct SoftBan {
    int id = 0;
    DWORD untilMs = 0;
};
SoftBan gSoftBan[kSoftBanCap]{};
int gSoftBanN = 0;

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    CreateDirectoryA(dir, nullptr);
    // Shared with attack_input_port; the helper rotates once per process, so whichever opens
    // second appends to the same generation instead of rotating the other's file away.
    gLog = x::runtime::OpenRotatingDbgLogA(dir, "combat.log");
}

void LogLine(const char* fmt, ...) {
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1000];
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
    x::runtime::LogI("SimpleCombat", "%s", body);
}

void EnterState(State s, DWORD now, const char* why) {
    if (gState == s) return;
    LogLine("state %s→%s%s%s", StateName(gState), StateName(s), why && why[0] ? " " : "",
            why ? why : "");
    gState = s;
    gStateEnterMs = now;
}

void ClearWhiffArm() {
    gLock.armHp = -1;
    gLock.armUntil = 0;
    gLock.firesInArm = 0;
}

void ClearLock() {
    (void)multi_skill::CancelPendingBurstForRetarget();
    gLock = LockState{};
    gLandSide = 0;
    gStandstillSince = 0;
    gStandstillShuffleLast = 0;
}

void ClearSoftBan() {
    gSoftBanN = 0;
    memset(gSoftBan, 0, sizeof(gSoftBan));
}

void PurgeSoftBan(DWORD now) {
    int w = 0;
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].untilMs > now) gSoftBan[w++] = gSoftBan[i];
    }
    gSoftBanN = w;
}

bool IsSoftBanned(int id, DWORD now) {
    PurgeSoftBan(now);
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].id == id) return true;
    }
    return false;
}

void SoftBanFor(int id, DWORD now, DWORD ms) {
    PurgeSoftBan(now);
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].id == id) {
            gSoftBan[i].untilMs = now + ms;
            return;
        }
    }
    if (gSoftBanN >= kSoftBanCap) return;
    gSoftBan[gSoftBanN++] = SoftBan{id, now + ms};
}

void ArmWhiffObserve(DWORD now, int hpAtFire) {
    if (hpAtFire <= 0) return;
    if (gLock.armUntil == 0) {
        gLock.armHp = hpAtFire;
        gLock.armUntil = now + kWhiffObserveMs;
        gLock.firesInArm = 1;
    } else {
        gLock.firesInArm += 1;
    }
}

// 窗内掉血 → 清 whiff；窗满无掉血 → +1；满 N → 清锁并返回 false。
bool ResolveWhiffArm(DWORD now) {
    if (gLock.armUntil == 0) return true;
    if (gLock.lastHp >= 0 && gLock.armHp >= 0 && gLock.lastHp < gLock.armHp) {
        if (gLock.whiff || gLock.firesInArm) {
            LogLine("whiff clear id=%d hp=%d→%d fires=%d", gLock.id, gLock.armHp, gLock.lastHp,
                    gLock.firesInArm);
        }
        gLock.whiff = 0;
        ClearWhiffArm();
        return true;
    }
    if (now < gLock.armUntil) return true;

    gLock.whiff += 1;
    LogLine("whiff tick id=%d streak=%d/%d hp=%d fires=%d observe=%ums", gLock.id, gLock.whiff,
            kWhiffFireN, gLock.lastHp, gLock.firesInArm, (unsigned)kWhiffObserveMs);
    ClearWhiffArm();
    if (gLock.whiff < kWhiffFireN) return true;

    LogLine("switch reason=whiff id=%d fires=%d hp=%d", gLock.id, gLock.whiff, gLock.lastHp);
    SoftBanFor(gLock.id, now, kWhiffSoftBanMs);
    gLastLockLostWhy = "whiff";
    ClearLock();
    return false;
}

bool SameLayerY(float a, float b) { return std::fabs(a - b) <= kSameLayerY; }

// 玩家优先 CurFh.zMass；怪用点附近站立 FH 的 zMass。解析失败退回 Y 带。
bool TryPlayerZMass(float px, float py, int32_t* outZm) {
    if (!outZm) return false;
    const uint32_t cur = ports::foothold::PeekCurFhId();
    if (cur && ports::foothold_path::ZMassOfFh(cur, outZm)) return true;
    return ports::foothold_path::ZMassAt(px, py, outZm);
}

bool SameLayer(float ax, float ay, float bx, float by) {
    int32_t za = 0, zb = 0;
    if (TryPlayerZMass(ax, ay, &za) && ports::foothold_path::ZMassAt(bx, by, &zb)) {
        return za == zb;
    }
    return SameLayerY(ay, by);
}

bool SameLayerZm(int32_t playerZm, bool playerZmOk, float py, float mx, float my, uint32_t mobFh) {
    int32_t mz = 0;
    bool mobOk = false;
    if (mobFh) mobOk = ports::foothold_path::ZMassOfFh(mobFh, &mz);
    if (!mobOk) mobOk = ports::foothold_path::ZMassAt(mx, my, &mz);
    if (playerZmOk && mobOk) return playerZm == mz;
    return SameLayerY(py, my);
}

float ClampStandOff() {
    float s = static_cast<float>(gTeleportStandOff.load(std::memory_order_acquire));
    if (s < static_cast<float>(xcat::kCombatTeleportStandOffMin))
        s = static_cast<float>(xcat::kCombatTeleportStandOffMin);
    if (s > static_cast<float>(xcat::kCombatTeleportStandOffMax))
        s = static_cast<float>(xcat::kCombatTeleportStandOffMax);
    return s;
}

float HugDeadzone(float standOff) {
    const float d = standOff * kHugDeadzoneFrac;
    return d > kHugDeadzoneMin ? d : kHugDeadzoneMin;
}

bool LiveStepOn() {
    return gLiveStepEnabled.load(std::memory_order_acquire) &&
           gTeleportEnabled.load(std::memory_order_acquire);
}

// LiveStep 微贴门：同层且 hop 足够短。远距同层不得冒充 hug。
bool IsHugMove(bool sameLayer, float hop) {
    return LiveStepOn() && sameLayer && std::isfinite(hop) && hop >= 0.f && hop < kHugMaxHop;
}

float HugErrorX(float playerX, float mobX, float standOff) {
    if (gLandSide == 0) gLandSide = (playerX >= mobX) ? 1 : -1;
    const float hugX = mobX + static_cast<float>(gLandSide) * standOff;
    return std::fabs(playerX - hugX);
}

// CD 未好时禁止进 MoveTo（BIN：Aim↔MoveTo 空转发呆）。
bool CanTeleportNow() { return ports::teleport::NativeCooldownRemainingMs() == 0; }

bool TryEnterMoveTo(DWORD now, const char* why) {
    if (!CanTeleportNow()) {
        static DWORD sCd = 0;
        if (!sCd || now - sCd > 1500) {
            sCd = now;
            LogLine("MoveTo defer why=%s cd_remain=%ums", why ? why : "?",
                    ports::teleport::NativeCooldownRemainingMs());
        }
        return false;
    }
    EnterState(State::MoveTo, now, why);
    return true;
}

bool InHitBand(float playerX, float playerY, float mobX, float mobY, float standOff) {
    if (!SameLayer(playerX, playerY, mobX, mobY)) return false;
    const float dx = std::fabs(mobX - playerX);
    // 站在怪心附近不算可打：必须重贴到侧位（对照枫星禁中心站位，防背打）。
    if (dx < kMinLandAway) return false;
    return dx <= standOff * kHitBandMaxFrac;
}

// 落点 = 怪台 ± standOff（左右任一侧，禁止塌怪心）。
// loose=true：选怪饿死时放宽（短站距 / 怪心贴台），避免「池里有怪却 miss 站桩」。
bool EstimateLand(float px, float py, float mx, float my, float standOff, float* outHop,
                  float* outTx, float* outTy, uint32_t* outFh, int* outSide = nullptr,
                  bool loose = false) {
    float standX = mx, standY = my;
    uint32_t standFh = 0;
    if (!ports::foothold_path::SnapStandAt(mx, my, &standX, &standY, &standFh) || !standFh)
        return false;
    if (std::fabs(standY - my) > kSameLayerY) return false;

    const float useOff = loose ? (standOff > 12.f ? standOff * 0.5f : standOff) : standOff;
    const float minAway =
        loose ? kMinLandAway
              : ((useOff * 0.75f > kMinLandAway) ? useOff * 0.75f : kMinLandAway);
    auto trySide = [&](float sign, float* ox, float* oy) -> float {
        float lx = standX + sign * useOff, ly = standY;
        if (!ports::foothold_path::SnapOnFh(standFh, standX + sign * useOff, &lx, &ly))
            return -1.f;
        if (std::fabs(ly - my) > kSameLayerY) return -1.f;
        if (std::fabs(lx - standX) < minAway) return -1.f;
        if (!ports::foothold_path::IsXSafeOnFh(standFh, lx)) return -1.f;
        const float idealX = standX + sign * useOff;
        const float dragTol = loose ? useOff * 1.25f : useOff * 0.75f;
        if (std::fabs(lx - idealX) > dragTol) return -1.f;
        *ox = lx;
        *oy = ly;
        return std::fabs(lx - standX);
    };

    float pref = 0.f;
    if (gLandSide != 0) {
        pref = static_cast<float>(gLandSide);
    } else {
        pref = (px >= standX) ? 1.f : -1.f;
    }
    float txA = 0, tyA = 0, txB = 0, tyB = 0;
    const float gA = trySide(pref, &txA, &tyA);
    const float gB = trySide(-pref, &txB, &tyB);

    float tx = 0, ty = 0;
    int chosenSide = 0;
    if (gA >= minAway) {
        tx = txA;
        ty = tyA;
        chosenSide = (pref > 0.f) ? 1 : -1;
    } else if (gB >= minAway) {
        tx = txB;
        ty = tyB;
        chosenSide = (pref > 0.f) ? -1 : 1;
    } else if (loose) {
        // 最后手段：贴在怪台 snap 点，宁可贴怪心也不站桩 miss。
        tx = standX;
        ty = standY;
        chosenSide = (pref > 0.f) ? 1 : -1;
    } else {
        return false;
    }

    if (outSide) *outSide = chosenSide;

    const float dx = tx - px, dy = ty - py;
    const float hop = std::sqrt(dx * dx + dy * dy);
    if (outHop) *outHop = hop;
    if (outTx) *outTx = tx;
    if (outTy) *outTy = ty;
    if (outFh) *outFh = standFh;
    return std::isfinite(hop);
}

bool EstimateLandPrefer(float px, float py, float mx, float my, float standOff, float* outHop,
                        float* outTx, float* outTy, uint32_t* outFh, int* outSide = nullptr) {
    if (EstimateLand(px, py, mx, my, standOff, outHop, outTx, outTy, outFh, outSide, false))
        return true;
    return EstimateLand(px, py, mx, my, standOff, outHop, outTx, outTy, outFh, outSide, true);
}

bool RefreshLock(const ports::mob::Snapshot& snap) {
    if (!gLock.id || !gLock.ptr) return false;
    const DWORD now = GetTickCount();

    // 热路径：直读锁怪指针，不等 mobscan 缓存（对齐同行：死了立刻切）。
    ports::mob::MobLite live{};
    if (!ports::mob::TryFillLive(gLock.ptr, gLock.id, live)) {
        LogLine("switch reason=dead_or_gone id=%d via=live", gLock.id);
        SoftBanFor(gLock.id, now, kDeadSoftBanMs);
        gLastLockLostWhy = "dead_or_gone";
        ClearLock();
        return false;
    }
    gLock.ptr = live.ptr;
    gLock.x = live.x;
    gLock.y = live.y;
    gLock.lastHp = live.hpPct;
    (void)snap;  // 选怪仍用缓存；锁存续以 live 为准
    return ResolveWhiffArm(now);
}

// 贴怪关：只锁同层近怪。贴怪开：优先同 zMass 可落点；同层没有才跨 zMass（防飞怪物池）。
// 群怪优先开：同层轮次内先比周围活怪密度，再比距离/hop。
constexpr float kClusterRadiusPx = 150.f;

int CountClusterNeighbors(const ports::mob::Snapshot& snap, const ports::mob::MobLite& center) {
    const float r2 = kClusterRadiusPx * kClusterRadiusPx;
    int n = 0;
    for (int i = 0; i < snap.count; ++i) {
        const auto& o = snap.mobs[i];
        if (o.id == center.id) continue;
        if (!o.ready || o.deadType != 0 || o.hpPct <= 0) continue;
        if (o.templateId == kSpecialTplFilter) continue;
        if (!SameLayerY(center.y, o.y)) continue;
        const float dx = o.x - center.x;
        const float dy = o.y - center.y;
        if (dx * dx + dy * dy <= r2) ++n;
    }
    return n;
}

bool PickNearestTarget(const ports::mob::Snapshot& snap, float px, float py, DWORD now,
                       bool allowCrossLayer, bool looseLand) {
    const float standOff = ClampStandOff();
    const bool clusterOn = gClusterPriority.load(std::memory_order_acquire);
    const ports::mob::MobLite* best = nullptr;
    float bestScore = 1e9f;
    float bestHop = 0.f;
    int bestCluster = -1;
    int32_t playerZm = 0;
    const bool playerZmOk = TryPlayerZMass(px, py, &playerZm);

    auto better = [&](int clusterN, float score) -> bool {
        if (clusterOn) {
            if (clusterN > bestCluster) return true;
            if (clusterN < bestCluster) return false;
        }
        return score < bestScore;
    };

    auto consider = [&](const ports::mob::MobLite& m, bool sameLayerPass) {
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) return;
        if (m.templateId == kSpecialTplFilter) return;
        if (IsSoftBanned(m.id, now)) return;

        if (!allowCrossLayer) {
            if (!SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, 0)) return;
            if (!sameLayerPass) return;
            const float dx = m.x - px;
            const float d2 = dx * dx;
            const int cn = clusterOn ? CountClusterNeighbors(snap, m) : 0;
            if (!better(cn, d2)) return;
            bestCluster = cn;
            bestScore = d2;
            best = &m;
            bestHop = std::fabs(dx);
            return;
        }

        float hop = 0, tx = 0, ty = 0;
        uint32_t fh = 0;
        const bool landOk =
            looseLand ? EstimateLandPrefer(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh)
                      : EstimateLand(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh);
        if (!landOk) return;
        if (!std::isfinite(hop) || hop < 0.f) return;
        // 过远不在选怪阶段拒绝——MoveTo 分段贴近（否则 ok 很多却全 miss）。
        const bool same = SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, fh);
        if (sameLayerPass) {
            if (!same) return;
        } else {
            if (same) return;
        }

        const float dx = m.x - px;
        const float dy = m.y - py;
        const float score = dx * dx + dy * dy + hop * hop * 0.35f;
        const int cn = clusterOn ? CountClusterNeighbors(snap, m) : 0;
        if (!better(cn, score)) return;
        bestCluster = cn;
        bestScore = score;
        best = &m;
        bestHop = hop;
    };

    for (int i = 0; i < snap.count; ++i) consider(snap.mobs[i], /*sameLayerPass=*/true);
    if (!best && allowCrossLayer) {
        bestScore = 1e9f;
        bestCluster = -1;
        for (int i = 0; i < snap.count; ++i) consider(snap.mobs[i], /*sameLayerPass=*/false);
    }
    if (!best) return false;

    gLock.ptr = best->ptr;
    gLock.id = best->id;
    gLock.lastHp = best->hpPct;
    gLock.whiff = 0;
    ClearWhiffArm();
    gLock.x = best->x;
    gLock.y = best->y;
    gLandSide = (px >= best->x) ? 1 : -1;
    gStandstillSince = 0;
    gStandstillShuffleLast = 0;
    gStandstillAnchorX = px;
    gStandstillAnchorY = py;
    LogLine("acquire id=%d tpl=%d hp=%d%% pos=(%.0f,%.0f) d=(%.0f,%.0f) layer=%s hop~%.0f side=%d "
            "cluster=%d loose=%d",
            best->id, best->templateId, best->hpPct, best->x, best->y, best->x - px, best->y - py,
            SameLayer(px, py, best->x, best->y) ? "same" : "cross", bestHop, gLandSide,
            clusterOn ? bestCluster : -1, looseLand ? 1 : 0);
    return true;
}

void ExplainAcquireMiss(const ports::mob::Snapshot& snap, float px, float py, DWORD now,
                        bool allowCrossLayer) {
    const float standOff = ClampStandOff();
    int nBan = 0, nSpecial = 0, nDead = 0, nNoLand = 0, nHopFar = 0, nOk = 0;
    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) {
            ++nDead;
            continue;
        }
        if (m.templateId == kSpecialTplFilter) {
            ++nSpecial;
            continue;
        }
        if (IsSoftBanned(m.id, now)) {
            ++nBan;
            continue;
        }
        if (allowCrossLayer) {
            float hop = 0, tx = 0, ty = 0;
            uint32_t fh = 0;
            if (!EstimateLand(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh)) {
                ++nNoLand;
                continue;
            }
            if (std::isfinite(hop) && hop > kMaxApproachHop) {
                ++nHopFar;  // 仍可选中，MoveTo 会分段；此处仅诊断
            }
        }
        ++nOk;
    }
    LogLine(
        "acquire miss count=%d py=%.0f mode=%s ban=%d noLand=%d hopFar=%d dead=%d special=%d "
        "ok=%d softN=%d",
        snap.count, py, allowCrossLayer ? "any-landable" : "same-layer", nBan, nNoLand, nHopFar,
        nDead, nSpecial, nOk, gSoftBanN);
}

bool UseMulti() {
    return multi_skill::IsEnabled() && ports::multi_skill::HasSelection();
}

bool GameWindowLikelyFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void PollF5() {
    if (!GameWindowLikelyFocused()) {
        gF5WasDown = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (down && !gF5WasDown) {
        const bool next = !gEnabled.load();
        SetEnabled(next);
        x::ipc::PayloadControl_PublishSimpleCombat(next);
        const DWORD now = GetTickCount();
        if (next && gMapArmUntilMs && static_cast<int>(now - gMapArmUntilMs) < 0) {
            LogLine("F5 toggle enabled=1 (map arm wait remain=%ums)", gMapArmUntilMs - now);
        } else {
            LogLine("F5 toggle enabled=%d", next ? 1 : 0);
        }
    }
    gF5WasDown = down;
}

// 裸 F11：随机活怪 → fill+Doing 长距贴怪。避开 Ctrl/Shift+F11（留给其它热键）。
void PollF11NativeMob() {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (!GameWindowLikelyFocused() || ctrl || shift) {
        gF11WasDown = f11;
        return;
    }
    if (f11 && !gF11WasDown) {
        const DWORD now = GetTickCount();
        if (!gLastF11Ms || now - gLastF11Ms >= 50) {
            gLastF11Ms = now;
            LogLine("F11 fill+Doing→random mob");
            RequestNativeTeleportToRandomMob();
        }
    }
    gF11WasDown = f11;
}

void GoIdle(DWORD now, const char* why) {
    ClearLock();
    gSettleUntil = 0;
    EnterState(State::Idle, now, why);
}

void BeginMapArmGrace(DWORD now, const char* why) {
    const bool already =
        gMapArmUntilMs && static_cast<int>(now - gMapArmUntilMs) < 0;
    ClearLock();
    ClearSoftBan();
    ports::attack::ForceRelease();
    gSettleUntil = 0;
    gState = State::Idle;
    gMapArmUntilMs = now + kMapArmGraceMs;
    if (already) {
        static DWORD sRef = 0;
        if (!sRef || now - sRef > 1000) {
            sRef = now;
            LogLine("map arm grace refresh %ums why=%s", (unsigned)kMapArmGraceMs,
                    why ? why : "?");
        }
        return;
    }
    LogLine("map arm grace %ums why=%s", (unsigned)kMapArmGraceMs, why ? why : "?");
}

void KickWinReset() {
    gKickWinNext = 0;
    gKickWinCount = 0;
    gKickMetersTotal = 0.f;
    gKickHopMax = 0.f;
    gKickHopGe500 = 0;
    gKickHopGe1000 = 0;
    gKickHopFail = 0;
}

void KickWinPush(DWORD now, float hop) {
    if (!(hop >= 0.f) || !std::isfinite(hop)) hop = 0.f;
    gKickWin[gKickWinNext] = KickHopSample{now, hop};
    gKickWinNext = (gKickWinNext + 1) % kKickWinCap;
    if (gKickWinCount < kKickWinCap) ++gKickWinCount;
    gKickMetersTotal += hop;
    if (hop > gKickHopMax) gKickHopMax = hop;
    if (hop >= 500.f) ++gKickHopGe500;
    if (hop >= 1000.f) ++gKickHopGe1000;
}

void KickWinStats(DWORD now, DWORD winMs, int* outN, float* outMeters, float* outMax) {
    int n = 0;
    float meters = 0.f;
    float mx = 0.f;
    for (int i = 0; i < gKickWinCount; ++i) {
        const int idx = (gKickWinNext - gKickWinCount + i + kKickWinCap) % kKickWinCap;
        const KickHopSample& s = gKickWin[idx];
        if (now < s.t || now - s.t > winMs) continue;
        ++n;
        meters += s.hop;
        if (s.hop > mx) mx = s.hop;
    }
    if (outN) *outN = n;
    if (outMeters) *outMeters = meters;
    if (outMax) *outMax = mx;
}

void LogKickWinSnapshot(DWORD now, const char* tag) {
    int n15 = 0, n60 = 0;
    float m15 = 0.f, m60 = 0.f, x15 = 0.f, x60 = 0.f;
    KickWinStats(now, kKickWinShortMs, &n15, &m15, &x15);
    KickWinStats(now, kKickWinLongMs, &n60, &m60, &x60);
    const float rate15 = n15 > 0 ? (15000.f / static_cast<float>(n15)) : 0.f;
    LogLine(
        "kick_stress win %s | 15s n=%d m=%.0f max=%.0f ~cd=%.0fms | 60s n=%d m=%.0f max=%.0f | "
        "run hops=%u fail=%u m=%.0f max=%.0f ge500=%u ge1000=%u",
        tag ? tag : "?", n15, m15, x15, rate15, n60, m60, x60, gKickHopOk, gKickHopFail,
        gKickMetersTotal, gKickHopMax, gKickHopGe500, gKickHopGe1000);
}

void FinishKickStress(const char* reason, DWORD now) {
    if (!gKickStressActive.load(std::memory_order_acquire)) return;
    gKickStressActive.store(false, std::memory_order_release);
    const int err = kick_sniff::LastPendingErrorCode();
    const int st = kick_sniff::LastSessionState();
    const DWORD at = now ? now : GetTickCount();
    LogLine(
        "kick_stress DONE mode=%s reason=%s last_ok_cd=%ums fail_cd=%ums hops=%u elapsed=%ums "
        "pendingError=%d sessionState=%d",
        gKickModeTag, reason ? reason : "?", gKickLastOkMs, gKickIntervalMs, gKickHopOk,
        gKickStartMs ? (at - gKickStartMs) : 0u, err, st);
    LogKickWinSnapshot(at, reason ? reason : "done");
}

void TickKickStress(DWORD now) {
    if (!gKickStressActive.load(std::memory_order_acquire)) return;

    // 断线 / 离开 Play：压测终点（阈值 = last_ok_cd）。
    const bool disc = kick_sniff::SawDisconnect() && !gKickSawDiscAtStart;
    if (disc || !ports::world::IsPlayReady()) {
        FinishKickStress(disc ? "disconnect" : "not_play_ready", now);
        return;
    }

    if (now < gKickNextDueMs) return;

    const DWORD cd = gKickIntervalMs;

    ports::player_combat::CombatCtx player{};
    if (!ports::player_combat::QueryCombatCtx(player) || !player.ok) {
        LogLine("kick_stress hop skip reason=no_player cd=%ums", cd);
        gKickNextDueMs = now + (cd > 200 ? cd : 200);
        return;
    }

    float tx = 0.f, ty = 0.f, hop = 0.f;
    uint32_t fh = 0;
    int pickId = 0;
    int poolN = 0, poolAlive = 0;

    if (gKickLocalShuttle) {
        // 同台左右锚点来回；hop ≈ 2×120，排除远距贴怪因素。
        if (!gKickFh) {
            LogLine("kick_stress hop skip reason=no_local_fh cd=%ums", cd);
            gKickNextDueMs = now + (cd > 200 ? cd : 200);
            return;
        }
        const float wantX = gKickGoRight ? gKickRightX : gKickLeftX;
        if (!ports::foothold_path::SnapOnFh(gKickFh, wantX, &tx, &ty)) {
            LogLine("kick_stress hop skip reason=local_snap_fail side=%d cd=%ums",
                    gKickGoRight ? 1 : -1, cd);
            gKickNextDueMs = now + (cd > 200 ? cd : 200);
            return;
        }
        fh = gKickFh;
        const float dx = tx - player.x, dy = ty - player.y;
        hop = std::sqrt(dx * dx + dy * dy);
        pickId = gKickGoRight ? 1 : -1;
        poolN = 2;
        poolAlive = 2;
        gKickGoRight = !gKickGoRight;
    } else {
        ports::mob::Snapshot snap{};
        if (!ports::mob::GetCached(snap) || !snap.ok || snap.count <= 0) {
            LogLine("kick_stress hop skip reason=no_mob_cache cd=%ums", cd);
            gKickNextDueMs = now + (cd > 200 ? cd : 200);
            return;
        }

        const float standOff = ClampStandOff();
        struct Cand {
            int id = 0;
            float hop = 0.f;
            float tx = 0.f;
            float ty = 0.f;
            uint32_t fh = 0;
        };
        Cand cands[ports::mob::kMaxLiteMobs];
        int n = 0;
        for (int i = 0; i < snap.count; ++i) {
            const auto& m = snap.mobs[i];
            if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
            float h = 0, lx = 0, ly = 0;
            uint32_t lf = 0;
            if (!EstimateLand(player.x, player.y, m.x, m.y, standOff, &h, &lx, &ly, &lf)) continue;
            if (n >= ports::mob::kMaxLiteMobs) break;
            cands[n++] = Cand{m.id, h, lx, ly, lf};
        }
        if (n <= 0) {
            LogLine("kick_stress hop skip reason=no_landable_mob alive=%d cd=%ums", snap.count, cd);
            gKickNextDueMs = now + (cd > 200 ? cd : 200);
            return;
        }

        const unsigned rnd =
            GetTickCount() ^ (gKickHopOk * 2654435761u) ^ (static_cast<unsigned>(snap.count) * 97u);
        const Cand& pick = cands[rnd % static_cast<unsigned>(n)];
        pickId = pick.id;
        hop = pick.hop;
        tx = pick.tx;
        ty = pick.ty;
        fh = pick.fh;
        poolN = n;
        poolAlive = snap.count;
    }

    // 压测节奏由 gKickIntervalMs 管；本地 CD 压到最小，避免被 NativeCooldown 挡住。
    ports::teleport::SetNativeCooldownMs(50);
    if (!ports::teleport::TeleportNativeSkillCall(tx, ty, fh)) {
        ++gKickHopFail;
        LogLine("kick_stress hop fail id=%d hop=%.0f cd=%ums hops=%u fails=%u (see Teleport log)",
                pickId, hop, cd, gKickHopOk, gKickHopFail);
        gKickNextDueMs = now + (cd > 200 ? cd : 200);
        return;
    }

    gKickLastOkMs = cd;
    ++gKickHopOk;
    ++gKickHopsAtLevel;
    KickWinPush(now, hop);
    LogLine("kick_stress hop#%u ok id=%d to=(%.0f,%.0f) hop=%.0f fh=%u cd=%ums pool=%d/%d%s",
            gKickHopOk, pickId, tx, ty, hop, fh, cd, poolN, poolAlive,
            gKickLocalShuttle ? " local" : "");

    if (gKickHopsAtLevel >= gKickCfgHopsPerLevel) {
        gKickHopsAtLevel = 0;
        if (gKickIntervalMs <= gKickCfgFloorMs) {
            FinishKickStress("floor_reached_no_kick", now);
            return;
        }
        const DWORD next =
            gKickIntervalMs > gKickCfgStepMs + gKickCfgFloorMs
                ? gKickIntervalMs - gKickCfgStepMs
                : gKickCfgFloorMs;
        gKickIntervalMs = next;
        LogLine("kick_stress level→ cd=%ums mode=%s", gKickIntervalMs, gKickModeTag);
        LogKickWinSnapshot(now, "level");
    }
    gKickNextDueMs = now + gKickIntervalMs;
}

void TickImpl(DWORD now) {
    PollF5();
    PollF11NativeMob();
    ports::attack::TickReleases();
    ports::input::TickReleases(now);

    // 踢号压测优先：跑时挂起普攻状态机，避免抢 CD / 抢皮。
    TickKickStress(now);
    if (gKickStressActive.load(std::memory_order_acquire)) {
        if (gState != State::Idle) GoIdle(now, "kick_stress");
        return;
    }

    if (!gEnabled.load(std::memory_order_acquire)) {
        if (gState != State::Idle) GoIdle(now, "disabled");
        return;
    }
    if (gExternalPause.load(std::memory_order_acquire)) {
        // 对照枫星：buffs/timed_keys 是高频短暂停 → 轻暂停只停出刀，保留 lock/FSM。
        // 旧实现 GoIdle("pause") 会 ClearLock → 补 BUFF 后丢锁重寻/乱跳，客户感知「打怪错乱」。
        // 绝对闸（auto_lie / encounter / channel_hop）仍走完整 Idle。
        if (gExternalPauseAbs.load(std::memory_order_acquire)) {
            if (gState != State::Idle) GoIdle(now, "pause");
        } else {
            static DWORD sPauseHoldLog = 0;
            if (!sPauseHoldLog || now - sPauseHoldLog > 1500) {
                sPauseHoldLog = now;
                LogLine("pause hold light depth=%d state=%s keep_lock",
                        gExternalPauseDepth.load(std::memory_order_acquire),
                        StateName(gState));
            }
        }
        return;
    }
    if (!ports::world::IsPlayReady()) {
        if (gState != State::Idle) GoIdle(now, "not_play");
        if (gLastMapId >= 0) {
            gLastMapId = -1;  // 下一帧进图重新武装
            gMapArmUntilMs = 0;
        }
        return;
    }

    const int mapId = ports::world::GetMapId();
    if (mapId > 0 && mapId != gLastMapId) {
        gLastMapId = mapId;
        BeginMapArmGrace(now, "map_change");
    }
    if (gMapArmUntilMs && static_cast<int>(now - gMapArmUntilMs) < 0) {
        static DWORD sArm = 0;
        if (!sArm || now - sArm > 800) {
            sArm = now;
            LogLine("combat arm wait remain=%ums (no tp/fire after map)",
                    gMapArmUntilMs - now);
        }
        if (gState != State::Idle) GoIdle(now, "arm_grace");
        return;
    }

    ports::player_combat::CombatCtx player{};
    if (!ports::player_combat::QueryCombatCtx(player) || !player.ok) {
        static DWORD sBad = 0;
        if (!sBad || now - sBad > 2000) {
            sBad = now;
            // 画面「人没了」时这里必现：坐标 NaN/≈0/超界，精灵被摔出图外。
            LogLine("tick skip reason=bad_player_pos (coords invalid — char visually gone)");
        }
        // 换图落地瞬间坐标坏：延长武装，禁止紧接着远跳。
        if (!gMapArmUntilMs || static_cast<int>(now - gMapArmUntilMs) >= 0) {
            gMapArmUntilMs = now + 800;
        }
        return;
    }

    ports::mob::Snapshot snap{};
    if (!ports::mob::GetCached(snap) || !snap.ok) {
        // 禁止在 combat worker 上 Collect（Singleton/RuntimeClassInit/FindAll 属托管）
        return;
    }

    const float standOff = ClampStandOff();
    const bool tpOn = gTeleportEnabled.load(std::memory_order_acquire);

    if (gState == State::Idle) EnterState(State::Acquire, now, "enabled");

    // Aim/Recover→Firing 同 tick 连跑；多给几拍让 dead→acquire→MoveTo 同轮完成。
    for (int pass = 0; pass < 5; ++pass) {
    switch (gState) {
        case State::Idle:
            break;

        case State::Acquire: {
            if (!RefreshLock(snap)) {
                static DWORD sMiss = 0;
                bool got = PickNearestTarget(snap, player.x, player.y, now, /*allowCrossLayer=*/tpOn,
                                            /*looseLand=*/false);
                if (!got) {
                    // 先松落点再选；禁止立刻 ClearSoftBan——会把刚 dead 的鬼锁清掉同 tick 空转。
                    got = PickNearestTarget(snap, player.x, player.y, now, /*allowCrossLayer=*/tpOn,
                                           /*looseLand=*/true);
                }
                if (!got && gSoftBanN > 0) {
                    // 整图被 ban 光才清；否则保留尸体软禁。
                    ClearSoftBan();
                    got = PickNearestTarget(snap, player.x, player.y, now, /*allowCrossLayer=*/tpOn,
                                           /*looseLand=*/true);
                }
                if (!got) {
                    if (!sMiss || now - sMiss > 1000) {
                        sMiss = now;
                        ExplainAcquireMiss(snap, player.x, player.y, now, tpOn);
                    }
                    break;
                }
            }
            if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                EnterState(State::Aim, now, "in_band");
                break;
            }
            if (tpOn) {
                // 贴怪开：出命中带即贴（可跨层），不再卡 minDx；CD 未好不进 MoveTo。
                float hop = 0, tx = 0, ty = 0;
                uint32_t fh = 0;
                if (!EstimateLandPrefer(player.x, player.y, gLock.x, gLock.y, standOff, &hop, &tx,
                                        &ty, &fh)) {
                    SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                    LogLine("acquire no_land id=%d — softBan", gLock.id);
                    ClearLock();
                    break;
                }
                if (!TryEnterMoveTo(now, "need_approach")) break;
                continue;  // 同 tick 进 MoveTo 出瞬移（禁再等一轮 worker）
            }
            // 贴怪关：本阶段优先验证出刀——同层有怪就打，不因 dx 挡 Fire
            EnterState(State::Aim, now, "stand_fire");
            break;
        }

        case State::MoveTo: {
            if (!tpOn) {
                EnterState(State::Acquire, now, "tp_disabled");
                break;
            }
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;  // 同 tick 选下一只
            }
            // BIN：MotionBusy(animBusy=220) 会把换怪贴身拖到 ~170ms——同行是砍着就飞。
            // 出刀互斥只靠 interval/pendingUp；贴怪瞬移不再等前摇。

            // 已在命中带则直接打，否则立刻贴（无 minDx）。
            if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                EnterState(State::Firing, now, "in_band");
                continue;
            }

            float hop = 0, tx = 0, ty = 0;
            uint32_t fh = 0;
            int landSide = 0;
            if (!EstimateLandPrefer(player.x, player.y, gLock.x, gLock.y, standOff, &hop, &tx, &ty,
                                    &fh, &landSide)) {
                SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                LogLine("MoveTo reject id=%d no_land", gLock.id);
                ClearLock();
                EnterState(State::Acquire, now, "no_land");
                continue;
            }
            if (hop > kMaxApproachHop) {
                // 分段贴近：朝最终落点方向最多跳 kMaxApproachHop，禁止一次飞 1700。
                const float t = kMaxApproachHop / hop;
                float ix = player.x + (tx - player.x) * t;
                float iy = player.y + (ty - player.y) * t;
                float sx = 0.f, sy = 0.f;
                uint32_t sfh = 0;
                if (!ports::foothold_path::SnapStandAt(ix, iy, &sx, &sy, &sfh) || sfh == 0) {
                    SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                    LogLine("MoveTo chunk fail id=%d wantHop=%.0f mid=(%.0f,%.0f)", gLock.id, hop, ix,
                            iy);
                    ClearLock();
                    EnterState(State::Acquire, now, "hop_chunk_fail");
                    continue;
                }
                const float cdx = sx - player.x;
                const float cdy = sy - player.y;
                const float chop = std::sqrt(cdx * cdx + cdy * cdy);
                if (!(chop >= kMinReapproachHop)) {
                    SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                    LogLine("MoveTo chunk sticky id=%d wantHop=%.0f chop=%.1f", gLock.id, hop, chop);
                    ClearLock();
                    EnterState(State::Acquire, now, "hop_chunk_sticky");
                    continue;
                }
                LogLine("MoveTo chunk id=%d fullHop=%.0f → step=%.0f to=(%.0f,%.0f) fh=%u", gLock.id,
                        hop, chop, sx, sy, sfh);
                hop = chop;
                tx = sx;
                ty = sy;
                fh = sfh;
            }
            if (landSide != 0) gLandSide = landSide;

            const bool same = SameLayer(player.x, player.y, gLock.x, gLock.y);
            const bool hug = IsHugMove(same, hop);
            const float minHop = hug ? kMinHugHop : kMinReapproachHop;
            // 已几乎在侧位落点上：直接打，别空耗 CD。
            // BIN：退回 Aim 后若 InHitBand 仍 false（怪心/贴边）会 Aim↔MoveTo(left_band/hop_too_small) 空转数秒。
            if (hop < minHop) {
                LogLine("MoveTo hop_too_small id=%d hop=%.1f min=%.1f → fire", gLock.id, hop, minHop);
                EnterState(State::Firing, now, "hop_too_small");
                continue;  // 同 tick 出刀
            }
            // LiveStep / 远距贴怪：面板冷却 + 大 hop 地板（防 cd 过短连跳脱同步）。
            const DWORD panelCd = gTeleportCooldownMs.load(std::memory_order_acquire);
            const DWORD cd = EffectiveTeleportCdMs(hop, panelCd);
            ports::teleport::SetNativeCooldownMs(cd);
            if (!ports::teleport::TeleportNativeSkillCall(tx, ty, fh)) {
                static DWORD sFail = 0;
                if (!sFail || now - sFail > 1500) {
                    sFail = now;
                    LogLine("MoveTo teleport fail id=%d want=(%.0f,%.0f) hop=%.0f cd=%ums hug=%d",
                            gLock.id, tx, ty, hop, cd, hug ? 1 : 0);
                }
                break;
            }
            LogLine(
                "MoveTo fill+Doing id=%d to=(%.0f,%.0f) from=(%.0f,%.0f) hop=%.0f fh=%u cd=%ums "
                "panelCd=%ums side=%d hug=%d settle=%ums",
                gLock.id, tx, ty, player.x, player.y, hop, fh, cd, panelCd, gLandSide, hug ? 1 : 0,
                SettleMsForHop(hop, hug));
            gSettleX = tx;
            gSettleY = ty;
            gSettleFh = fh;
            gStandstillSince = now;
            gStandstillAnchorX = tx;
            gStandstillAnchorY = ty;
            const DWORD settle = SettleMsForHop(hop, hug);
            if (settle == 0) {
                // 落地即砍：跳过 Settling/Aim 空态
                EnterState(State::Firing, now, hug ? "tp_fire_hug" : "tp_fire");
                continue;
            }
            gSettleUntil = now + settle;
            EnterState(State::Settling, now, hug ? "tp_ok_hug" : "tp_ok");
            continue;  // 同 tick 若已到期会进 Settling→Fire
        }

        case State::Settling: {
            if (now < gSettleUntil) {
                static DWORD sWait = 0;
                if (!sWait || now - sWait > 2000) {
                    sWait = now;
                    LogLine("Settling wait remain=%ums land=(%.0f,%.0f)", gSettleUntil - now,
                            gSettleX, gSettleY);
                }
                break;  // 禁止 Fire / 再 TP
            }
            LogLine("Settling done land=(%.0f,%.0f) fh=%u", gSettleX, gSettleY, gSettleFh);
            gSettleUntil = 0;
            // 收态只靠 fill+Doing（Apl←Ap 已在 teleport_port 内完成）。禁止再 SyncRel/Impact。
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, "lost_after_settle");
                break;
            }
            EnterState(State::Aim, now, "settle_ok");
            continue;  // 同 tick 进 Aim/Fire（settle=0 时对齐同行落地即砍）
        }

        case State::Aim: {
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                break;
            }
            // 原地连刀过久：翻落点侧并重贴（对照枫星 standstill shuffle；瞬移穿怪）。
            if (tpOn && gLandSide != 0 && gStandstillSince &&
                now - gStandstillSince >= kStandstillShuffleMs &&
                (!gStandstillShuffleLast ||
                 now - gStandstillShuffleLast >= kStandstillShuffleCooldownMs)) {
                const float moved =
                    std::hypotf(player.x - gStandstillAnchorX, player.y - gStandstillAnchorY);
                if (moved < 40.f) {
                    gStandstillShuffleLast = now;
                    gLandSide = -gLandSide;
                    LogLine("standstill shuffle id=%d flip side=%d age=%ums", gLock.id, gLandSide,
                            (unsigned)(now - gStandstillSince));
                    if (TryEnterMoveTo(now, "standstill_flip")) break;
                    // CD 未好：带内继续砍，不空等 MoveTo。
                } else {
                    gStandstillSince = now;
                    gStandstillAnchorX = player.x;
                    gStandstillAnchorY = player.y;
                }
            }
            if (!SameLayer(player.x, player.y, gLock.x, gLock.y)) {
                if (tpOn) {
                    if (TryEnterMoveTo(now, "cross_layer")) break;
                    break;  // CD 未好：留 Aim，等 CD，禁止跨层空转 MoveTo
                }
                SoftBanFor(gLock.id, now, kWhiffSoftBanMs);
                ClearLock();
                EnterState(State::Acquire, now, "layer_change");
                break;
            }
            // 命中带外才重贴；带内禁止 hug_follow（BIN：Aim↔MoveTo(in_band) 发呆）。
            // CD 未好：留 Aim——带内仍可出刀，出带则等 CD。
            // hop 已够小：直接砍，避免 MoveTo→hop_too_small 空转。
            if (!InHitBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                if (tpOn) {
                    float hop = 0, tx = 0, ty = 0;
                    uint32_t fh = 0;
                    if (!EstimateLandPrefer(player.x, player.y, gLock.x, gLock.y, standOff, &hop,
                                            &tx, &ty, &fh)) {
                        SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                        ClearLock();
                        EnterState(State::Acquire, now, "aim_no_land");
                        continue;
                    }
                    const bool hug = IsHugMove(true, hop);
                    const float minHop = hug ? kMinHugHop : kMinReapproachHop;
                    if (hop < minHop) {
                        const float faceDx = gLock.x - player.x;
                        (void)ports::attack::FaceToward(faceDx);
                        EnterState(State::Firing, now, "near_land");
                        continue;
                    }
                    const char* why =
                        (hug && HugErrorX(player.x, gLock.x, standOff) > HugDeadzone(standOff))
                            ? "hug_follow"
                            : "left_band";
                    if (TryEnterMoveTo(now, why)) continue;
                    break;
                }
            }
            const float faceDx = gLock.x - player.x;
            (void)ports::attack::FaceToward(faceDx);
            EnterState(State::Firing, now, "ready");
            continue;  // 同 tick 出刀
        }

        case State::Firing: {
            if (ports::attack::IsFireSuppressed()) break;  // buffs Hold：同拍已进 Firing 也停
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;  // 同 tick 选下一只并贴
            }
            // 间隔/松键由 TryFirePrimary 门控；贴怪瞬移已不再等 MotionBusy。

            const float faceDx = gLock.x - player.x;
            (void)ports::attack::FaceToward(faceDx);
            const int hpBefore = gLock.lastHp;
            bool ok = false;

            if (UseMulti()) {
                if (multi_skill::IsBurstBusy()) break;
                (void)ports::attack::ApplyFaceNow();
                char reason[64]{};
                ok = multi_skill::TryCast(reason, sizeof(reason));
                if (ok) {
                    LogLine("multi fire id=%d dx=%.0f hp=%d whiff=%d arm=%d", gLock.id, faceDx,
                            hpBefore, gLock.whiff, gLock.firesInArm);
                    if (!gStandstillSince) {
                        gStandstillSince = now;
                        gStandstillAnchorX = player.x;
                        gStandstillAnchorY = player.y;
                    }
                } else {
                    static DWORD sM = 0;
                    if (!sM || now - sM > 2000) {
                        sM = now;
                        LogLine("multi fire skip id=%d reason=%s", gLock.id,
                                reason[0] ? reason : "?");
                    }
                    break;
                }
            } else {
                // 未就绪则回 Recover 等下一轮；勿空点（会刷 soft / 假 fail）。
                if (!ports::attack::CanFirePrimary()) {
                    EnterState(State::Recover, now, "pace_wait");
                    break;
                }
                ok = ports::attack::TryFirePrimary();
                if (ok) {
                    LogLine("fire id=%d dx=%.0f hp=%d whiff=%d arm=%d", gLock.id, faceDx, hpBefore,
                            gLock.whiff, gLock.firesInArm);
                    if (!gStandstillSince) {
                        gStandstillSince = now;
                        gStandstillAnchorX = player.x;
                        gStandstillAnchorY = player.y;
                    }
                } else {
                    // 此处多为硬失败（OnFuncKey）；软门已在 CanFire 挡掉。
                    static DWORD sF = 0;
                    if (!sF || now - sF > 2000) {
                        sF = now;
                        LogLine("fire skip id=%d dx=%.0f (hard_fail)", gLock.id, faceDx);
                    }
                    EnterState(State::Recover, now, "fire_fail");
                    break;
                }
            }

            // 出刀只武装观察窗；真正 +whiff / 换怪在 RefreshLock→ResolveWhiffArm。
            ArmWhiffObserve(now, hpBefore);
            EnterState(State::Recover, now, "fired");
            break;
        }

        case State::Recover: {
            // 节奏真源 = TryFirePrimary(interval+pendingUp)。禁止再硬等 100ms——
            // BIN：面板 50ms 时火间隔仍≈190ms（Recover100 + Aim tick）。
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;
            }
            // 滞回：带内只砍（禁 hug）；怪心 / 出带 / 跨层 → TryEnterMoveTo（CD 门控）。
            const float dx = std::fabs(gLock.x - player.x);
            const float bandOut = standOff * (kHitBandMaxFrac + 0.5f);
            const bool same = SameLayer(player.x, player.y, gLock.x, gLock.y);
            if (same && dx >= kMinLandAway && dx <= standOff * kHitBandMaxFrac) {
                // 间隔/松键未到：停本 tick，别同 tick 空点（加速 5ms 时 pass 环会刷 soft）。
                if (!ports::attack::CanFirePrimary()) break;
                EnterState(State::Firing, now, "still_valid");
                continue;
            } else if (tpOn && same && dx < kMinLandAway) {
                // 怪心重贴：允许带刀飞（不再等 MotionBusy）
                if (!TryEnterMoveTo(now, LiveStepOn() ? "hug_follow" : "recenter_hug")) break;
                continue;
            } else if (tpOn && (!same || dx > bandOut)) {
                const char* why =
                    same ? (LiveStepOn() ? "hug_follow" : "reapproach") : "reapproach_cross";
                if (!TryEnterMoveTo(now, why)) break;
                continue;
            } else if (tpOn && same) {
                if (!ports::attack::CanFirePrimary()) break;
                EnterState(State::Firing, now, "near_band");
                continue;
            } else {
                EnterState(State::Acquire, now, "reacquire");
            }
            break;
        }
    }
    break;  // 未 continue 则结束同 tick 连跑
    }       // for pass
}

DWORD WINAPI Worker(LPVOID) {
    timeBeginPeriod(1);
    OpenLog();
    LogLine("simple_combat worker start (state-machine redesign)");
    ports::input::Init();
    ports::attack::Init();
    (void)ports::teleport::EnsureBound();

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        const DWORD tickMs = gTickIntervalMs.load(std::memory_order_acquire);
        if (!gLastTick || now - gLastTick >= tickMs) {
            gLastTick = now;
            TickImpl(now);
        }
        // tick≤8 时若仍 Sleep(8) 会把心跳卡死在 8ms+；短 tick 改睡 1ms。
        Sleep(tickMs <= 8 ? 1 : kIdleSleepMs);
    }

    ports::attack::ForceRelease();
    LogLine("simple_combat worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    LogLine("Init");
    gEnabled.store(false);
    gExternalPauseAbs.store(false);
    gExternalPauseDepth.store(0);
    gExternalPause.store(false);
    gTeleportEnabled.store(false);
    gState = State::Idle;
    ClearLock();
    ClearSoftBan();
}

void Shutdown() {
    StopTeleportKickStress();
    StopWorker();
    SetEnabled(false);
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    gWorkerThread.store(h);
}

void StopWorker() {
    gWorkerStop.store(true);
    HANDLE h = gWorkerThread.exchange(nullptr);
    if (h) {
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
}

void SetEnabled(bool on) {
    const bool prev = gEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    if (!on) {
        // 纯普攻：关 F5 = 松攻击键 + 停状态机。
        ClearLock();
        ports::attack::ForceRelease();
        gState = State::Idle;
        gSettleUntil = 0;
    } else {
        ClearSoftBan();
        ClearLock();
        gState = State::Idle;
        gSettleUntil = 0;
        // 同图 / 首次热开：人已站稳则不要空等 map arm（BIN：F5 后干等 1–2s）。
        if (ports::world::IsPlayReady()) {
            const int mapId = ports::world::GetMapId();
            ports::player_combat::CombatCtx player{};
            if (mapId > 0 && ports::player_combat::QueryCombatCtx(player) && player.ok) {
                if (gLastMapId == mapId || gLastMapId < 0) {
                    gLastMapId = mapId;
                    if (gMapArmUntilMs) {
                        LogLine("warm start clear arm map=%d", mapId);
                    }
                    gMapArmUntilMs = 0;
                }
            }
        }
        x::features::auto_supply::RecordHangupFarmMap("simple_combat_on");
    }
    LogLine("SetEnabled %d teleport=%d liveStep=%d standOff=%u minDx=%u", on ? 1 : 0,
            gTeleportEnabled.load() ? 1 : 0, gLiveStepEnabled.load() ? 1 : 0,
            gTeleportStandOff.load(), gTeleportMinDx.load());
}

bool IsEnabled() { return gEnabled.load(std::memory_order_acquire); }

void SetAttackIntervalMs(uint32_t ms) {
    ms = xcat::ClampSimpleCombatAttackIntervalMs(ms);
    ports::attack::SetIntervalMs(ms);
}

void SetTickIntervalMs(uint32_t ms) {
    ms = xcat::ClampSimpleCombatTickMs(ms);
    const DWORD prev = gTickIntervalMs.exchange(ms, std::memory_order_acq_rel);
    if (prev == ms) return;
    LogLine("SetTickIntervalMs %u (prev=%u)", ms, (unsigned)prev);
}

void SetSmartInterval(bool on) { ports::attack::SetSmartInterval(on); }

void SetClusterPriority(bool on) {
    const bool prev = gClusterPriority.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetClusterPriority %d", on ? 1 : 0);
}

bool IsClusterPriority() { return gClusterPriority.load(std::memory_order_acquire); }

void SetTeleportEnabled(bool on) {
    const bool prev = gTeleportEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetTeleportEnabled %d", on ? 1 : 0);
}

void SetLiveStepEnabled(bool on) {
    const bool prev = gLiveStepEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetLiveStepEnabled %d", on ? 1 : 0);
}

bool IsLiveStepEnabled() { return gLiveStepEnabled.load(std::memory_order_acquire); }

void SetTeleportParams(uint32_t minDx, uint32_t standOff, uint32_t cooldownMs) {
    if (minDx < 160) minDx = 160;
    if (minDx > 2000) minDx = 2000;
    standOff = xcat::ClampCombatTeleportStandOff(standOff);
    cooldownMs = xcat::ClampCombatTeleportCooldownMs(cooldownMs);
    gTeleportMinDx.store(minDx, std::memory_order_release);
    gTeleportStandOff.store(standOff, std::memory_order_release);
    gTeleportCooldownMs.store(cooldownMs, std::memory_order_release);
    ports::teleport::SetNativeCooldownMs(cooldownMs);
}

void RefreshExternalPauseEffective() {
    const bool on = gExternalPauseAbs.load(std::memory_order_acquire) ||
                    gExternalPauseDepth.load(std::memory_order_acquire) > 0;
    gExternalPause.store(on, std::memory_order_release);
    // 与 Tick 顶层闸同步：已进 Firing 的同拍 / 主线程排队出刀也要被 TryFirePrimary 挡掉。
    ports::attack::SetFireSuppressed(on);
}

void SetExternalPause(bool on) {
    gExternalPauseAbs.store(on, std::memory_order_release);
    RefreshExternalPauseEffective();
}

void AcquireExternalPause() {
    gExternalPauseDepth.fetch_add(1, std::memory_order_acq_rel);
    RefreshExternalPauseEffective();
}

void ReleaseExternalPause() {
    for (;;) {
        int cur = gExternalPauseDepth.load(std::memory_order_acquire);
        if (cur <= 0) {
            gExternalPauseDepth.store(0, std::memory_order_release);
            break;
        }
        if (gExternalPauseDepth.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) {
            break;
        }
    }
    RefreshExternalPauseEffective();
}

void ResetForMapChange() {
    BeginMapArmGrace(GetTickCount(), "ResetForMapChange");
}

void RequestTeleportToRandomMob() {
    // 面板「测试贴怪」= F11：fill+Doing 绝对落点。
    RequestNativeTeleportToRandomMob();
}

void RequestNativeTeleportCall() {
    if (!ports::world::IsPlayReady()) {
        LogLine("tp_native skip reason=not_play_ready");
        return;
    }
    // 短距探针：短 CD；与战斗长距 CD 分开意图——先压到最小再跳。
    ports::teleport::SetNativeCooldownMs(50);
    if (!ports::teleport::TeleportNativeSkillCall()) {
        LogLine("tp_native fail (see Teleport log: land/doing)");
        return;
    }
    LogLine("tp_native ok fill+Doing short hop");
}

void StopTeleportKickStress() {
    if (!gKickStressActive.load(std::memory_order_acquire)) return;
    FinishKickStress("manual_stop", GetTickCount());
}

bool IsTeleportKickStressActive() {
    return gKickStressActive.load(std::memory_order_acquire);
}

void StartKickStress(DWORD startMs, DWORD stepMs, DWORD floorMs, int hopsPerLevel,
                     const char* modeTag, bool localShuttle) {
    if (gKickStressActive.load(std::memory_order_acquire)) {
        StopTeleportKickStress();
        return;
    }
    if (!ports::world::IsPlayReady()) {
        LogLine("kick_stress skip reason=not_play_ready mode=%s", modeTag ? modeTag : "?");
        return;
    }

    gKickLocalShuttle = localShuttle;
    gKickFh = 0;
    gKickLeftX = gKickRightX = gKickY = 0.f;
    gKickGoRight = true;

    if (localShuttle) {
        ports::player_combat::CombatCtx player{};
        if (!ports::player_combat::QueryCombatCtx(player) || !player.ok) {
            LogLine("kick_stress skip reason=no_player mode=%s", modeTag ? modeTag : "?");
            return;
        }
        float ax = 0.f, ay = 0.f;
        uint32_t afh = 0;
        if (!ports::foothold_path::SnapStandAt(player.x, player.y, &ax, &ay, &afh) || !afh) {
            LogLine("kick_stress skip reason=no_stand_fh mode=%s pos=(%.0f,%.0f)",
                    modeTag ? modeTag : "?", player.x, player.y);
            return;
        }
        float lx = 0.f, ly = 0.f, rx = 0.f, ry = 0.f;
        bool have = false;
        float bestSpan = 0.f;
        // 由大到小试振幅；短台 SnapOnFh 会夹到台端 → span 变小，但仍是「本地来回」。
        const float amps[] = {kKickLocalHopPx, 80.f, 50.f, 30.f};
        for (float amp : amps) {
            float tl = 0.f, tly = 0.f, tr = 0.f, tryY = 0.f;
            const bool okL = ports::foothold_path::SnapOnFh(afh, ax - amp, &tl, &tly);
            const bool okR = ports::foothold_path::SnapOnFh(afh, ax + amp, &tr, &tryY);
            if (!okL || !okR) continue;
            const float span = std::fabs(tr - tl);
            // 只要左右可分（≥24px），就接受；优先更大 span。
            if (span < 24.f) continue;
            if (!have || span > bestSpan) {
                have = true;
                bestSpan = span;
                lx = tl;
                ly = tly;
                rx = tr;
                ry = tryY;
            }
            // 已经接近目标振幅，不必再缩。
            if (span >= amp * 1.5f) break;
        }
        if (!have) {
            LogLine("kick_stress skip reason=local_anchors_fail mode=%s fh=%u ax=%.0f ay=%.0f",
                    modeTag ? modeTag : "?", afh, ax, ay);
            return;
        }
        if (bestSpan < 80.f) {
            LogLine("kick_stress local short_fh warn span=%.0f fh=%u (still ok; was rejecting <80)",
                    bestSpan, afh);
        }
        gKickFh = afh;
        gKickLeftX = lx;
        gKickRightX = rx;
        gKickY = ay;
        gKickGoRight = true;
    }

    gKickCfgStartMs = startMs;
    gKickCfgStepMs = stepMs > 0 ? stepMs : 1;
    gKickCfgFloorMs = floorMs;
    gKickCfgHopsPerLevel = hopsPerLevel > 0 ? hopsPerLevel : 1;
    gKickIntervalMs = startMs;
    gKickLastOkMs = 0;
    gKickHopOk = 0;
    gKickHopsAtLevel = 0;
    KickWinReset();
    gKickStartMs = GetTickCount();
    gKickNextDueMs = gKickStartMs + 300;
    gKickSawDiscAtStart = kick_sniff::SawDisconnect();
    if (modeTag && modeTag[0]) {
        strncpy_s(gKickModeTag, modeTag, _TRUNCATE);
    } else {
        strncpy_s(gKickModeTag, "?", _TRUNCATE);
    }
    gKickStressActive.store(true, std::memory_order_release);
    if (localShuttle) {
        LogLine(
            "kick_stress START mode=%s start_cd=%ums step=%ums floor=%ums hops/level=%d "
            "local shuttle ±%.0f fh=%u L=(%.0f,%.0f) R=(%.0f,%.0f) span=%.0f "
            "(win15s/60s meters; watch combat.log DONE)",
            gKickModeTag, gKickCfgStartMs, gKickCfgStepMs, gKickCfgFloorMs, gKickCfgHopsPerLevel,
            kKickLocalHopPx, gKickFh, gKickLeftX, gKickY, gKickRightX, gKickY,
            std::fabs(gKickRightX - gKickLeftX));
    } else {
        LogLine(
            "kick_stress START mode=%s start_cd=%ums step=%ums floor=%ums hops/level=%d "
            "(random mob; win15s/60s meters; watch combat.log DONE)",
            gKickModeTag, gKickCfgStartMs, gKickCfgStepMs, gKickCfgFloorMs, gKickCfgHopsPerLevel);
    }
}

void RequestTeleportKickStress() {
    StartKickStress(kKickStressStartMs, kKickStressStepMs, kKickStressFloorMs,
                    kKickStressHopsPerLevel, "wide", false);
}

void RequestTeleportKickStressFine() {
    StartKickStress(kKickStressFineStartMs, kKickStressFineStepMs, kKickStressFineFloorMs,
                    kKickStressFineHopsPerLevel, "fine0-50", false);
}

void RequestTeleportKickStressFine10() {
    StartKickStress(kKickStressFine10StartMs, kKickStressFine10StepMs, kKickStressFine10FloorMs,
                    kKickStressFine10HopsPerLevel, "fine10-30", false);
}

void RequestTeleportKickStressLocal() {
    StartKickStress(kKickStressLocalStartMs, kKickStressLocalStepMs, kKickStressLocalFloorMs,
                    kKickStressLocalHopsPerLevel, "local0-50", true);
}

void RequestNativeTeleportToRandomMob() {
    if (!ports::world::IsPlayReady()) {
        LogLine("tp_native_mob skip reason=not_play_ready");
        return;
    }
    ports::player_combat::CombatCtx player{};
    if (!ports::player_combat::QueryCombatCtx(player) || !player.ok) {
        LogLine("tp_native_mob skip reason=no_player");
        return;
    }

    ports::mob::Snapshot snap{};
    if (!ports::mob::GetCached(snap) || !snap.ok || snap.count <= 0) {
        LogLine("tp_native_mob skip reason=no_mob_cache");
        return;
    }

    const float standOff = ClampStandOff();
    struct Cand {
        int id = 0;
        float hop = 0.f;
        float tx = 0.f;
        float ty = 0.f;
        uint32_t fh = 0;
    };
    Cand cands[ports::mob::kMaxLiteMobs];
    int n = 0;
    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
        float hop = 0, tx = 0, ty = 0;
        uint32_t fh = 0;
        if (!EstimateLand(player.x, player.y, m.x, m.y, standOff, &hop, &tx, &ty, &fh)) continue;
        if (n >= ports::mob::kMaxLiteMobs) break;
        cands[n++] = Cand{m.id, hop, tx, ty, fh};
    }
    if (n <= 0) {
        LogLine("tp_native_mob fail reason=no_landable_mob alive=%d standOff=%.0f", snap.count,
                standOff);
        return;
    }

    const unsigned rnd = GetTickCount() ^ (static_cast<unsigned>(snap.count) * 2654435761u);
    const Cand& pick = cands[rnd % static_cast<unsigned>(n)];

    // 与 F5 贴怪同冷却，防狂按 205。
    const uint32_t cd = gTeleportCooldownMs.load(std::memory_order_acquire);
    ports::teleport::SetNativeCooldownMs(cd);
    if (!ports::teleport::TeleportNativeSkillCall(pick.tx, pick.ty, pick.fh)) {
        LogLine("tp_native_mob fail id=%d to=(%.0f,%.0f) hop=%.0f fh=%u cd=%ums (among %d)",
                pick.id, pick.tx, pick.ty, pick.hop, pick.fh, cd, n);
        return;
    }
    LogLine("tp_native_mob ok id=%d to=(%.0f,%.0f) from=(%.0f,%.0f) hop=%.0f fh=%u cd=%ums pool=%d/%d",
            pick.id, pick.tx, pick.ty, player.x, player.y, pick.hop, pick.fh, cd, n, snap.count);
}

void Tick(DWORD nowMs) { TickImpl(nowMs); }

}  // namespace x::features::simple_combat
