// Classic TWMS — simple_combat explicit state machine (full redesign).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "simple_combat.h"

#include "../auto_supply/auto_supply.h"
#include "../attack_accel/attack_accel.h"
#include "../kick_sniff/kick_sniff.h"
#include "../multi_skill/multi_skill.h"
#include "../pet_feed/pet_feed.h"
#include "../ports/attack_input_port.h"
#include "../ports/foothold_path.h"
#include "../ports/foothold_port.h"
#include "../ports/input_port.h"
#include "../ports/map_bounds_port.h"
#include "../ports/mob_pool_port.h"
#include "../ports/multi_skill_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/skill_port.h"
#include "../ports/teleport_port.h"
#include "../ports/world_port.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/mono_clock.h"
#include "../../../common/xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <timeapi.h>
#include <utility>

#pragma comment(lib, "winmm.lib")

namespace x::features::simple_combat {
namespace {

constexpr DWORD kIdleSleepMs = 8;
// 同层：优先同 zMass（FH 连通域）；图未就绪时退回 |ΔY|≤45。
// SnapStandAt / 落点贴台仍用此 Y 带做「点是否贴在台上」。
constexpr float kSameLayerY = 45.f;
// zMass 相同但垂直多层时仍会 layer=same（BIN：|dy|=150~500 却同层瞬移拉地板）。
constexpr float kSameFloorMaxDy = 100.f;
// 命中带：|dx| ≤ standOff * 此系数。
// P0 硬门后 1.35 使 dx=34~36 贴边不砍（BIN 体感变慢）；1.55→允许 ~39，仍远小于远砍。
constexpr float kHitBandMaxFrac = 1.55f;
// 黏住无脑A：续砍直到该重贴。dx∈[命中带, 100) 仍算 hold——残血纠偏走
// TryConsumeWhiffApproachCorrect，禁止再被本阈值吞掉重贴。
constexpr float kReapproachMinDx = 100.f;  // ≥此值才强制重贴（常规路径）
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
constexpr DWORD kWhiffObserveMs = 100;  // lastHitted 当帧清窗后可收；回退 200
// IDA：_lastHitted@0x208 在 AddDamageInfo（出刀当帧）写；HpPercentage@0x240 在 MobPool 包侧晚写。
// 探针默认只打 combat.log（hit_probe），不改 whiff 判定；确认后再把 Clear 开关拨 true。
constexpr bool kProbeLastHittedLog = true;
constexpr bool kWhiffClearOnLastHitted = true;  // BIN：lastHitted 早于 hpPct；回退改 false
// BIN：UIHpTag 绝对血探针（combat.log · uihp_probe）；确认前不接入 abshp/oneshot。
constexpr bool kProbeUiHpTag = true;
constexpr DWORD kProbeUiHpTagPeriodMs = 400;
constexpr DWORD kProbeUiHpTagMissLogMs = 2000;
// BIN：DamageInfo 列表探针（标定 +0x24 是否为单次伤害）
constexpr bool kProbeDamageInfo = false;  // BIN 标定完关；开关时看 abandon_dealt 行即可
// BIN：残血且 lastHitted≠0 时续刀可能不再 bump/掉血。
// 旧：kWhiffHoldWounded 干等 dead_or_gone → 叠怪心死区可空等数秒（upload d8cd64 ≈7s）。
// 现：观察窗满无掉血 → 侧移纠偏续砍；够死条件仍走 abandon_wounded_stall。
// 加速够死分层（干净优先：须血条确认残血，禁止 hp=100% 早切）。
//   1) dealt：Σ Damage ≥ maxHP 且 hp% 已下降且 ≤ kCleanAbandonMaxHpPct
//   2) oneshot：同上血条门（禁用「射后不管」式 100% 就走）
//   3) abshp：预测够死且 hp% ≤ 门限
//   4) dead_or_gone：最终态（hitlag 干净模式关闭）
constexpr bool kCleanAbandon = true;
constexpr int kCleanAbandonMaxHpPct = 20;  // 高于此继续黏打
constexpr bool kWhiffWoundedReapproach = true;
// 秒杀道默认（面板可改；运行时见 gOneshot*；0 maxHp=关，与 PayloadControl 一致）
constexpr int64_t kOneshotMaxHpDefault = 0;
constexpr int kOneshotMinBumpsDefault = 1;
constexpr int kOneshotMinFiresDefault = 3;
constexpr DWORD kOneshotMinLagMsDefault = 40;
// hitlag 兜底（无 maxHP 表时）：须多刀，防切太早。
constexpr int kHitLagAbandonMinBumps = 3;
constexpr int kHitLagAbandonMinFires = 6;
constexpr DWORD kHitLagAbandonMs = 180;
// 绝对血预测：至少 1 次掉血采样 + 若干出刀。
constexpr int kAbsHpAbandonMinFires = 2;
constexpr int kAbsHpAbandonMinDrops = 1;
// 残血停滞：绝对剩余 ≤ 一刀均伤，或 hp%≤15 且已多刀。
constexpr int kWoundedStallAbandonMaxHp = 15;
constexpr int kWoundedStallAbandonMinFires = 5;
// BIN：过短禁锁 → 同 id 立刻重选 → hop_too_small 空砍；上限也不要 8s 拖死图。
constexpr DWORD kWhiffSoftBanMs = 600;
constexpr DWORD kWhiffSoftBanMaxMs = 3000;
constexpr int kWhiffSoftBanMaxStrikes = 6;
constexpr DWORD kDeadSoftBanMs = 300;
// 早切（dealt/oneshot/abshp）：禁止当尸体软禁。BIN：600ms 禁锁 → 同 id 不再补刀 → 图上「杀不死」。
constexpr DWORD kEarlyAbandonSoftBanMs = 0;
constexpr DWORD kNoLandSoftBanMs = 200;
// 分段贴台失败 / 硬帽：BIN map1020000 200ms 禁 → 两只远怪互抢 ≈100 fail/s。
// 长禁 + Acquire 预检跳过，等同层清完再偶尔重试。
constexpr DWORD kHopChunkFailSoftBanMs = 2000;
// land_miss：arm 停火 / softBan 换怪 / fhBan 毒台 —— 首次从轻，同因短窗累加（勿再 1.5s+8s+120s 三连锤）。
constexpr DWORD kLandMissArmMs = 400;
constexpr DWORD kBadPosArmMs = 500;
constexpr DWORD kLandMissSoftBanMs1 = 800;
constexpr DWORD kLandMissSoftBanMs2 = 2000;
constexpr DWORD kLandMissSoftBanMs3 = 4000;
constexpr DWORD kLandMissEscalateWindowMs = 8000;
// 同点邻台交接误判 skate 时勿用满额 ban（bbda00：fhBan×26 / 120s → 攻速塌陷）。
constexpr DWORD kSkateToxicSoftBanMs = 600;
constexpr DWORD kSkateToxicArmMs = 280;
// 同 z Walk 链 ban：8s 够躲开毒台；120s 曾把整图攻速打穿。
constexpr DWORD kLandFhBanMs = 8000;
constexpr int kLandFhBanCap = 32;
// 封台展开半径（沿 prev/next 的跳数）：只封出事那条缝的左右，别整条链连坐（见 BanLandFhSameZChain）。
constexpr int kLandFhBanSpread = 1;
// hop≈0 但未进命中带：优先同怪翻侧续打；从未交手且纠偏耗尽才短暂让路。
constexpr DWORD kHopStickySoftBanMs = 800;
constexpr int kStickyCorrectMax = 3;           // 未交手最多翻侧次数
constexpr DWORD kStickyCorrectCooldownMs = 180;  // 同怪纠偏最小间隔，防同 tick 空转
// d1a58e：sticky_cd→Aim→aim_reapproach→MoveTo 无节流狂抖数秒发呆；超时让路。
constexpr DWORD kStickySpinAbortMs = 400;
constexpr int kSpecialTplFilter = 9999999;
// fill+Doing 后互斥窗：产品接受空砍 → 时间窗一律 0；但必须等 PosSane 回稳再砍/再跳
//（BIN：Doing 后 ~200ms Ap 变 NaN/近 0 → 黑屏 / Field 软重载 205）。
// settle 按 hop 分档的历史接口仍保留，数值已全 0；节奏只认面板瞬移 CD。
constexpr DWORD kTeleportSettleShortMs = 0;
constexpr DWORD kTeleportSettleMidMs = 0;
constexpr DWORD kTeleportSettleFarMs = 0;
constexpr DWORD kTeleportSettleMegaMs = 0;
constexpr DWORD kPostDoingPosSaneMaxMs = 200;
// Doing 同帧读回 Ap 必 d≈0；至少跨过 1～2 帧再放行，避免 early→Fire 后 Ap 崩成 (0,0)。
constexpr DWORD kPostDoingMinSettleMs = 32;
// 跨层：bbda00 成功路径均卡在 ~200ms minSettle，效率腰斩。毒化另有 skate 闸，不必干等 200。
// BIN 326d34 / P0d：direct_tp 无 hop 切段 → 起伏图单次 fill 1500~3500px → 205 soft。
// false = zMass+同高过滤、maxHop 分段、跨层 fill_gate；允跨层（优先同层），禁硬 SoftBan 清锁。
constexpr bool kDirectTeleportNoLayerHop = false;
constexpr DWORD kPostDoingCrossLayerMinSettleMs = 64;
// 吸物脉冲：短 Settling 也至少开这么久，避免 pet_loot interval 对不齐落地窗。
constexpr DWORD kLootPulseFloorMs = 120;
// MoveTo / TryEnterMoveTo 干等（CD/预算）时续期长度。
constexpr DWORD kLootPulseHoldMs = 80;
constexpr DWORD kPostDoingCrossPosSaneMaxMs = 280;
constexpr DWORD kCrossLayerFillGateDefaultMs = xcat::kCombatCrossLayerFillGateDefaultMs;
constexpr DWORD kCrossLayerForbidSoftBanMs = 4000;
// 位移预算（10s 滚动窗口内 fill 总位移 px）；0=关，面板「位移预算」可调。
// 沿革：曾按「位移速率」建模并加垂直独立上限，但该模型两次被证伪——0.1.75 超速一倍活 71s、
// 0.1.76 恰好卡在预算内却 32s 被踢。踢线真因是 land_miss（写入位置被引擎打回票，见落点修复）。
// a7dc3e 实测垂直上限卡掉四成时间且每次 hold 都由它触发，故垂直上限撤除、总预算默认关。
constexpr DWORD kFillDistWindowMs = 10000;
// 禁贴 x≈0 轴落点（BIN：chunk to=(0,-2145)）；|x|<此值视为有掉出/原点崩风险。
constexpr float kMinLandAbsAxisX = 8.f;
// 原点邻域永远禁止作落点（不使用 (0,0)）。
constexpr float kForbiddenOriginEps = 8.f;
constexpr float kPostDoingLandEpsPx = 96.f;
// 跨层落地容差。改走引擎原生落点后放宽到本值（原 16）：引擎写完 Ap 就交给 CollisionDetect
// 自行挂台，角色顺势落到台面属**合法**位移（我方落点 y 与斜坡插值差可达数十 px），不是违规。
// 旧 16px 是为压住我方补种 RelPos 造成的拽动而设，那个根因已撤除（见 teleport_port 头注）。
constexpr float kPostDoingCrossLandEpsPx = 64.f;
// 引擎自主落点允许的最大位移：超过即认为不是「落到台面」而是真滑走。
constexpr float kNativeSnapTolPx = 64.f;
// Doing 后等 CollisionDetect 挂台的宽限（下落中 curFh=0 属正常，此窗内不判滑走）。
constexpr DWORD kNativeAttachWaitMs = 140;
constexpr float kSettleShortHop = 120.f;
constexpr float kSettleMidHop = 350.f;
constexpr float kSettleFarHop = 600.f;
// 换图 / 重新 PlayReady 后短暂禁止贴怪与出刀；同图热开 F5 不走此宽限。
// 覆盖 FH 缓存重建 + 首跳 RelPos/台稳定窗；崩坐标后也走此宽限。
constexpr DWORD kMapArmGraceMs = 1500;
constexpr int kSoftBanCap = 24;

DWORD SettleMsForHop(float hop, bool hug) {
    if (hug) return kHugSettleMs;
    if (!std::isfinite(hop) || hop < kSettleShortHop) return kTeleportSettleShortMs;
    if (hop < kSettleMidHop) return kTeleportSettleMidMs;
    if (hop < kSettleFarHop) return kTeleportSettleFarMs;
    return kTeleportSettleMegaMs;
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
std::atomic<bool> gExternalPause{false};      // 有效态：硬持有 mask OR 深度
std::atomic<uint32_t> gHardPauseMask{0};      // HardPauseHolder 位或
std::atomic<int> gExternalPauseDepth{0};      // buffs / timed_keys 可重叠
DWORD gMapArmUntilMs = 0;
int gLastMapId = -1;
// BUFF/定时键放闸后：短暂宽限窗（取消过期 MoveTo；CD 不再按 hop 抬高）。
constexpr DWORD kPostExtResumeGraceMs = 280;
DWORD gPostExtResumeUntil = 0;
bool gResumeRelockPending = false;
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gTeleportEnabled{false};  // Phase1 默认关
std::atomic<bool> gLiveStepEnabled{false};  // 锁怪后同层微贴；默认关
std::atomic<bool> gClusterPriority{false};  // 群怪优先；默认关
std::atomic<uint32_t> gTeleportStandOff{xcat::kCombatTeleportStandOffDefault};
std::atomic<uint32_t> gTeleportCooldownMs{xcat::kCombatTeleportCooldownDefaultMs};
std::atomic<uint32_t> gTeleportMinDx{xcat::kCombatTeleportMinDxDefault};
// 单次贴怪 hop 上限（px）；更远分段；调试 TAB / core.simpleCombatTeleportMaxHop。
std::atomic<uint32_t> gTeleportMaxHop{xcat::kCombatTeleportMaxHopDefault};
std::atomic<uint32_t> gCrossLayerFillGateMs{xcat::kCombatCrossLayerFillGateDefaultMs};
// 10s 窗口 fill 位移预算（px）；0=关。面板「位移预算」/ core.simpleCombatFillBudgetPx。
std::atomic<uint32_t> gFillBudgetPx{xcat::kCombatFillBudgetPxDefault};

float MaxApproachHopPx() {
    return static_cast<float>(gTeleportMaxHop.load(std::memory_order_acquire));
}
DWORD CrossLayerFillGateMs() {
    return gCrossLayerFillGateMs.load(std::memory_order_acquire);
}
std::atomic<int64_t> gOneshotMaxHp{kOneshotMaxHpDefault};
std::atomic<int> gOneshotMinBumps{kOneshotMinBumpsDefault};
std::atomic<int> gOneshotMinFires{kOneshotMinFiresDefault};
std::atomic<DWORD> gOneshotMinLagMs{kOneshotMinLagMsDefault};
std::atomic<DWORD> gOneshotFoxFillGapMs{xcat::kCombatOneshotFoxFillGapDefaultMs};
DWORD gFoxFillGateUntil = 0;         // 射后不管：此 tick 前禁 fill
DWORD gCrossLayerFillGateUntil = 0;  // 跨层 fill 后短互斥，防连跳把 Ap 打崩
// 位移预算窗：最近 10s 内每次 fill 的 (时刻, 位移px)。仅 worker 线程读写。
// e2bd95 教训：账本绝不清零——land_miss/bad_pos/换图都不许 Reset（清账 = 白送整份额度）；
// 记录自然 10s 过期。
std::deque<std::pair<DWORD, float>> gFillDistWindow;
float gFillDistWindowSum = 0.f;

float FillBudgetPx() {
    return static_cast<float>(gFillBudgetPx.load(std::memory_order_acquire));
}

void EvictFillDist(DWORD now) {
    while (!gFillDistWindow.empty() &&
           static_cast<int>(now - gFillDistWindow.front().first) >=
               static_cast<int>(kFillDistWindowMs)) {
        gFillDistWindowSum -= gFillDistWindow.front().second;
        gFillDistWindow.pop_front();
    }
    if (gFillDistWindow.empty()) gFillDistWindowSum = 0.f;
}

// 本跳会否击穿预算；会则返回需等待的毫秒数（等旧记录滚出窗口腾额度），0 = 放行（含预算关闭）。
DWORD FillDistBudgetWaitMs(DWORD now, float hopPx, float* outUsed) {
    const float budget = FillBudgetPx();
    if (budget <= 0.f) {
        if (outUsed) *outUsed = 0.f;
        return 0;
    }
    EvictFillDist(now);
    if (outUsed) *outUsed = gFillDistWindowSum;
    if (gFillDistWindowSum + hopPx <= budget) return 0;
    float freed = 0.f;
    for (const auto& e : gFillDistWindow) {
        freed += e.second;
        if (gFillDistWindowSum - freed + hopPx <= budget) {
            const DWORD freeAt = e.first + kFillDistWindowMs;
            return static_cast<int>(freeAt - now) > 0 ? (freeAt - now) : 1;
        }
    }
    // 单跳超总预算（预算被调得比 maxHop 还小）：等整窗清空。
    return kFillDistWindowMs;
}

void NoteFillDist(DWORD now, float hopPx) {
    if (FillBudgetPx() <= 0.f) return;
    gFillDistWindow.emplace_back(now, hopPx);
    gFillDistWindowSum += hopPx;
}

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

DWORD gLastTick = 0;
bool gF5WasDown = false;
bool gF11WasDown = false;
DWORD gLastF11Ms = 0;

State gState = State::Idle;
DWORD gStateEnterMs = 0;
// 挂机吸物脉冲截止（GetTickCount）；0=关。代数供 pet_loot 边沿立刻吸一拍。
std::atomic<DWORD> gLootPulseUntil{0};
std::atomic<uint32_t> gLootPulseGen{0};
DWORD gSettleUntil = 0;
float gSettleX = 0.f;
float gSettleY = 0.f;
uint32_t gSettleFh = 0;
bool gSettleNeedPosSane = false;  // Doing 后：时间窗可为 0，但仍要 PosSane 门
DWORD gSettleEnteredAt = 0;         // Settling 进入时刻（MinSettle 用）
DWORD gSettleMinMs = 48;            // 本跳实际 MinSettle（同层/跨层不同）
bool gSettleWasCross = false;       // 本跳是否跨层（落地 eps 更严）
DWORD gSettleDiagLastMs = 0;        // settle_diag 节流（失粘采证，不改飞行契约）
bool gSettleDidStabilize = false;   // Settling 内对 nan/滑链只自愈一次
bool gSettleSawRpBad = false;       // 本跳曾见 rpV 毒化（latch 后仍 fhDrift 勿 settle_ok）
constexpr DWORD kSettleDiagPeriodMs = 50;
// RelPos.V 异常：nan 或离谱速度 → 禁止 settle_ok 放行出刀（易 soft reload）。
constexpr double kSettleRpVAbsMax = 2500.0;
// BIN 39722a：stabilize 后若 Ap 仍离 land>此值，禁止放行（曾 d=59+重种 RelPos → near_zero）。
constexpr float kPostStabilizeLandEpsPx = 24.f;
// BIN 2f112a：跨层 settle 未 landed 时不自愈 → Walk 链狂奔 d=69→541 → soft reload。
// 毒化且离落点超过此值：立刻 land_miss，勿干等到 posGate。
constexpr float kSettleRunawayLandPx = 120.f;
struct LockState {
    void* ptr = nullptr;
    int32_t id = 0;
    int32_t lastHp = -1;
    int32_t lastHitted = 0;  // Mob+0x208 探针；不参与默认 whiff
    int whiff = 0;
    // 出刀后待确认：臂于首次出刀，窗满未掉血才计入 whiff。
    int32_t armHp = -1;
    int32_t armHitted = -1;
    DWORD armUntil = 0;
    int firesInArm = 0;
    bool hitProbeLogged = false;  // 本观察窗内 hit_probe 只打一次，防刷屏
    float x = 0.f;
    float y = 0.f;
    // 攻击加速·够死预测。
    int32_t templateId = 0;
    int64_t maxHp = 0;   // ResolveAbsHp.max（UI/缓存/表）
    int64_t absHp = 0;   // ResolveAbsHp.cur；pct 估计时 ≈ max×hp%
    ports::mob::AbsHpSrc absSrc = ports::mob::AbsHpSrc::None;
    int32_t lockStartHp = -1;
    int32_t prevHittedSample = -1;
    int hitBumpCount = 0;
    DWORD firstBumpMs = 0;
    int lockFires = 0;
    int32_t predictSampleHp = -1;
    int predictFiresAtHp = 0;
    int predictDropEvents = 0;
    // DamageInfo@+0x24 累计（列表通常 n=1，须本地累加）
    int64_t dealtSum = 0;
    int dealtHits = 0;
    int32_t lastDealt = 0;
    // 同怪纠偏（翻侧重贴）；没打死不换怪。
    int stickyFixes = 0;
    DWORD lastStickyCorrectMs = 0;
    bool needApproachCorrect = false;  // whiff 满但已交手 → 主循环翻侧重贴
};
LockState gLock{};
const char* gLastLockLostWhy = "lost";
// 落点侧粘滞（对照枫星 g_targetSide）：-1=怪左 / +1=怪右；朝向另算，不绑此值。
int gLandSide = 0;
DWORD gStandstillSince = 0;
DWORD gStandstillShuffleLast = 0;
DWORD gStickySpinSince = 0;  // hop_sticky Aim↔MoveTo 空转计时
float gStandstillAnchorX = 0.f;
float gStandstillAnchorY = 0.f;

// sticky：acquire miss 时 DropSoftBanNonSticky 不得清掉（BIN：dead 用 generic → 同 tick 鬼锁重选）。
enum SoftBanKind : uint8_t {
    kBanGeneric = 0,
    kBanWhiff = 1,
    kBanUnreachable = 2,
    kBanDead = 3,
};
struct SoftBan {
    int id = 0;
    DWORD untilMs = 0;
    SoftBanKind kind = kBanGeneric;
    uint8_t strikes = 0;  // whiff 累加；其它 kind 可保留历史 strikes
};
SoftBan gSoftBan[kSoftBanCap]{};
int gSoftBanN = 0;

// 落点 FH 禁飞：换怪也会踩同一毒台；换图清空。
struct LandFhBan {
    uint32_t fh = 0;
    DWORD untilMs = 0;
};
LandFhBan gLandFhBan[kLandFhBanCap]{};
int gLandFhBanN = 0;

// 坏落点表：引擎对少数落点存在**确定性**几何分歧，不是物理抖动。
// 07a30a 实测：86 次 land_miss 只出自 20 个落点，每点每次错同一距离、被引擎摆到同一位置
//（land=(-387,276) 16/16 全 miss，d 恒 27，引擎恒摆到 (-414,273)）。
// 故按「点」避开，而非封整块台面——封台会连带 no_land 弃怪（同 log 22~25 次）。换图清空。
// 一撞先短期回避（个别点是偶发，如某点 68 成 7 败）；再撞即本图长期回避。
constexpr int kBadLandCap = 96;
constexpr float kBadLandAvoidPx = 12.f;  // 候选落点落在此半径内即视为同一坏点
constexpr DWORD kBadLandFirstMs = 20000;
struct BadLandPt {
    float x = 0.f;
    float y = 0.f;
    DWORD untilMs = 0;  // 0 = 本图长期
    uint8_t strikes = 0;
};
BadLandPt gBadLand[kBadLandCap]{};
int gBadLandN = 0;

// 同 FH / 同怪 land_miss 短窗累加（真 miss 与 skate walkedOff 共用）。
uint32_t gLandMissFh = 0;
int gLandMissFhStrikes = 0;
DWORD gLandMissFhWindowUntil = 0;
int gLandMissMobId = 0;
int gLandMissMobStrikes = 0;
DWORD gLandMissMobWindowUntil = 0;

DWORD WhiffBanDurationMs(int strikes) {
    if (strikes < 1) strikes = 1;
    if (strikes > kWhiffSoftBanMaxStrikes) strikes = kWhiffSoftBanMaxStrikes;
    DWORD ms = kWhiffSoftBanMs;
    for (int i = 1; i < strikes; ++i) {
        if (ms >= kWhiffSoftBanMaxMs / 2) return kWhiffSoftBanMaxMs;
        ms *= 2;
    }
    return ms;
}

void OpenLog() {
    // combat.log 走 AppendDbgLog（与 attack_input_port 共享单句柄 + 会话内 512KiB 轮转）。
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    CreateDirectoryA(dir, nullptr);
}

// 热路径（出刀 / Firing↔Recover）只写 combat.log，避免双写刷爆 x.jsonl。
void LogToFile(const char* fmt, ...) {
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
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    (void)x::runtime::AppendDbgLogA(dir, "combat.log", buf, (DWORD)n);
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
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    (void)x::runtime::AppendDbgLogA(dir, "combat.log", buf, (DWORD)n);
    x::runtime::LogI("SimpleCombat", "%s", body);
}

void ClearLootPulse() { gLootPulseUntil.store(0, std::memory_order_release); }

bool LootPulseOpenNow(DWORD now) {
    const DWORD until = gLootPulseUntil.load(std::memory_order_acquire);
    return until != 0 && static_cast<int>(now - until) < 0;
}

// edge=true：递增代数，pet_loot 可无视 interval 立刻吸一拍。
void ArmLootPulse(DWORD now, DWORD durMs, bool edge) {
    if (durMs < 1) durMs = 1;
    const DWORD until = now + durMs;
    const DWORD prev = gLootPulseUntil.load(std::memory_order_relaxed);
    if (until > prev) gLootPulseUntil.store(until, std::memory_order_release);
    if (edge) gLootPulseGen.fetch_add(1u, std::memory_order_acq_rel);
}

// 出刀链外干等时续期；已关闭则带边沿重新打开。
void RenewLootPulseHold(DWORD now) {
    if (gState == State::Aim || gState == State::Firing || gState == State::Recover) return;
    const bool wasOpen = LootPulseOpenNow(now);
    ArmLootPulse(now, kLootPulseHoldMs, !wasOpen);
}

void EnterState(State s, DWORD now, const char* why) {
    if (gState == s) return;
    const bool hotCycle =
        (gState == State::Firing && s == State::Recover) ||
        (gState == State::Recover && s == State::Firing);
    const char* msgWhy = why && why[0] ? why : "";
    const char* sep = why && why[0] ? " " : "";
    if (hotCycle) {
        LogToFile("state %s→%s%s%s", StateName(gState), StateName(s), sep, msgWhy);
    } else {
        LogLine("state %s→%s%s%s", StateName(gState), StateName(s), sep, msgWhy);
    }
    gState = s;
    gStateEnterMs = now;
    // 吸物脉冲：落地 Settling 武装；出刀链 / Idle 立刻关（挂机停后走自由吸路径）。
    if (s == State::Settling) {
        DWORD remain = kLootPulseFloorMs;
        if (gSettleUntil && static_cast<int>(gSettleUntil - now) > 0) {
            remain = gSettleUntil - now;
            if (remain < kLootPulseFloorMs) remain = kLootPulseFloorMs;
        }
        ArmLootPulse(now, remain, /*edge=*/true);
    } else if (s == State::Aim || s == State::Firing || s == State::Recover ||
               s == State::Idle) {
        ClearLootPulse();
    }
}

void ClearWhiffArm() {
    gLock.armHp = -1;
    gLock.armHitted = -1;
    gLock.armUntil = 0;
    gLock.firesInArm = 0;
    gLock.hitProbeLogged = false;
}

// 轻暂停（buffs/timed_keys）会停出刀数百 ms：若不撤观察窗，墙钟到期会误 +whiff → 换怪瞬移（BIN：定时键后 +0.8s whiff×3）。
void SoftResetWhiffForLightPause(const char* why) {
    if (!gLock.id && !gLock.armUntil && !gLock.whiff) return;
    if (gLock.whiff || gLock.armUntil || gLock.firesInArm) {
        LogLine("whiff soft-reset why=%s id=%d streak=%d arm=%d", why ? why : "?", gLock.id,
                gLock.whiff, gLock.firesInArm);
    }
    gLock.whiff = 0;
    ClearWhiffArm();
}

void ClearLock() {
    (void)multi_skill::CancelPendingBurstForRetarget();
    gLock = LockState{};
    gLandSide = 0;
    gStandstillSince = 0;
    gStandstillShuffleLast = 0;
    gStickySpinSince = 0;
}

void ClearSoftBan() {
    gSoftBanN = 0;
    memset(gSoftBan, 0, sizeof(gSoftBan));
}

bool IsBadLandPoint(float x, float y, DWORD now) {
    for (int i = 0; i < gBadLandN; ++i) {
        const BadLandPt& p = gBadLand[i];
        if (p.untilMs && static_cast<int>(now - p.untilMs) >= 0) continue;  // 短期项已过期
        if (std::fabs(x - p.x) <= kBadLandAvoidPx && std::fabs(y - p.y) <= kBadLandAvoidPx)
            return true;
    }
    return false;
}

// 返回累加后 strikes。同点二次即转本图长期回避。
int NoteBadLandPoint(float x, float y, DWORD now) {
    if (!std::isfinite(x) || !std::isfinite(y)) return 0;
    for (int i = 0; i < gBadLandN; ++i) {
        BadLandPt& p = gBadLand[i];
        if (std::fabs(x - p.x) > kBadLandAvoidPx || std::fabs(y - p.y) > kBadLandAvoidPx) continue;
        if (p.strikes < 255) ++p.strikes;
        p.untilMs = 0;  // 长期
        return p.strikes;
    }
    int slot = gBadLandN;
    if (slot >= kBadLandCap) {
        // 满表：优先挤掉短期项，其次覆盖首项（本图坏点远少于 96，正常不触发）。
        slot = 0;
        for (int i = 0; i < gBadLandN; ++i) {
            if (gBadLand[i].untilMs) {
                slot = i;
                break;
            }
        }
    } else {
        ++gBadLandN;
    }
    gBadLand[slot] = BadLandPt{x, y, now + kBadLandFirstMs, 1};
    return 1;
}

void ClearLandFhBan() {
    gLandFhBanN = 0;
    memset(gLandFhBan, 0, sizeof(gLandFhBan));
    gBadLandN = 0;
    memset(gBadLand, 0, sizeof(gBadLand));
    gLandMissFh = 0;
    gLandMissFhStrikes = 0;
    gLandMissFhWindowUntil = 0;
    gLandMissMobId = 0;
    gLandMissMobStrikes = 0;
    gLandMissMobWindowUntil = 0;
}

DWORD LandMissSoftBanMsForStrikes(int strikes) {
    if (strikes <= 1) return kLandMissSoftBanMs1;
    if (strikes == 2) return kLandMissSoftBanMs2;
    return kLandMissSoftBanMs3;
}

int NoteLandMissMob(int id, DWORD now) {
    if (id <= 0) return 0;
    if (id != gLandMissMobId || static_cast<int>(now - gLandMissMobWindowUntil) >= 0) {
        gLandMissMobId = id;
        gLandMissMobStrikes = 1;
    } else {
        ++gLandMissMobStrikes;
    }
    gLandMissMobWindowUntil = now + kLandMissEscalateWindowMs;
    return gLandMissMobStrikes;
}

// 返回累加后 strikes；>=2 才应 fhBan。
int NoteLandMissFh(uint32_t fh, DWORD now) {
    if (!fh) return 0;
    if (fh != gLandMissFh || static_cast<int>(now - gLandMissFhWindowUntil) >= 0) {
        gLandMissFh = fh;
        gLandMissFhStrikes = 1;
    } else {
        ++gLandMissFhStrikes;
    }
    gLandMissFhWindowUntil = now + kLandMissEscalateWindowMs;
    return gLandMissFhStrikes;
}

void PurgeLandFhBan(DWORD now) {
    int w = 0;
    for (int i = 0; i < gLandFhBanN; ++i) {
        if (gLandFhBan[i].fh && gLandFhBan[i].untilMs > now) gLandFhBan[w++] = gLandFhBan[i];
    }
    gLandFhBanN = w;
}

bool IsLandFhBanned(uint32_t fh, DWORD now) {
    if (!fh) return false;
    PurgeLandFhBan(now);
    for (int i = 0; i < gLandFhBanN; ++i) {
        if (gLandFhBan[i].fh == fh) return true;
    }
    return false;
}

void BanLandFhOne(uint32_t fh, DWORD now) {
    if (!fh) return;
    PurgeLandFhBan(now);
    const DWORD until = now + kLandFhBanMs;
    for (int i = 0; i < gLandFhBanN; ++i) {
        if (gLandFhBan[i].fh != fh) continue;
        if (until > gLandFhBan[i].untilMs) gLandFhBan[i].untilMs = until;
        return;
    }
    if (gLandFhBanN >= kLandFhBanCap) {
        int victim = 0;
        for (int i = 1; i < gLandFhBanN; ++i) {
            if (gLandFhBan[i].untilMs < gLandFhBan[victim].untilMs) victim = i;
        }
        gLandFhBan[victim] = LandFhBan{fh, until};
        return;
    }
    gLandFhBan[gLandFhBanN++] = LandFhBan{fh, until};
}

// 沿 prev/next 展开同 zMass 链（异 z 边已在 path 层断开；此处用缓存几何自走）。
// spread = 距种子的最大跳数：miss 只发生在种子那条缝，封满整条链会把半张图打成不可锁。
// a7dc3e：8 次 miss 各封 10~32 台 → 26s 内 153 条禁令灌进 32 格表，反复驱逐 + no_land。
// spread=1（种子 + 左右邻台）刚好盖住「结算横滑跨一条缝」这一类，其余台照常可锁。
int BanLandFhSameZChain(uint32_t seedFh, DWORD now, int spread) {
    if (!seedFh) return 0;
    if (spread < 0) spread = 0;
    struct Node {
        uint32_t id;
        int depth;
    };
    Node stack[kLandFhBanCap];
    uint32_t seen[kLandFhBanCap];
    int sn = 0, seenN = 0;
    auto already = [&](uint32_t id) {
        for (int i = 0; i < seenN; ++i)
            if (seen[i] == id) return true;
        return false;
    };
    stack[sn++] = Node{seedFh, 0};
    int banned = 0;
    while (sn > 0 && banned < kLandFhBanCap) {
        const Node cur = stack[--sn];
        if (already(cur.id)) continue;
        if (seenN < kLandFhBanCap) seen[seenN++] = cur.id;
        BanLandFhOne(cur.id, now);
        ++banned;
        if (cur.depth >= spread) continue;
        ports::foothold::FootholdLite f{};
        if (!ports::foothold::TryGetCachedFh(cur.id, &f) || !f.id) continue;
        const int32_t zm = f.zMass;
        auto pushIfSameZ = [&](uint32_t nid) {
            if (!nid || already(nid) || sn >= kLandFhBanCap) return;
            ports::foothold::FootholdLite n{};
            if (!ports::foothold::TryGetCachedFh(nid, &n) || !n.id) return;
            if (n.zMass != zm) return;
            stack[sn++] = Node{nid, cur.depth + 1};
        };
        pushIfSameZ(f.prev);
        pushIfSameZ(f.next);
    }
    return banned;
}

void PurgeSoftBan(DWORD now) {
    int w = 0;
    for (int i = 0; i < gSoftBanN; ++i) {
        SoftBan b = gSoftBan[i];
        if (b.untilMs > now) {
            gSoftBan[w++] = b;
            continue;
        }
        // 过期 whiff 仍保留 strikes，便于下次空砍累加（否则 1s 到期后永远 strikes=1）。
        if (b.kind == kBanWhiff && b.strikes > 0) {
            b.untilMs = 0;
            gSoftBan[w++] = b;
        }
    }
    gSoftBanN = w;
}

bool IsSoftBanned(int id, DWORD now) {
    PurgeSoftBan(now);
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].id == id && gSoftBan[i].untilMs > now) return true;
    }
    return false;
}

void SoftBanFor(int id, DWORD now, DWORD ms, SoftBanKind kind = kBanGeneric) {
    if (id <= 0 || ms == 0) return;
    PurgeSoftBan(now);
    const DWORD until = now + ms;
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].id != id) continue;
        // 禁止缩短有效禁锁（BIN：长 whiff 禁被 no_land 200ms 盖掉 → 立刻重锁）。
        if (until > gSoftBan[i].untilMs) gSoftBan[i].untilMs = until;
        // whiff > unreachable > dead > generic：勿降级。
        if (kind == kBanWhiff) {
            gSoftBan[i].kind = kBanWhiff;
        } else if (kind == kBanUnreachable && gSoftBan[i].kind != kBanWhiff) {
            gSoftBan[i].kind = kBanUnreachable;
        } else if (kind == kBanDead && gSoftBan[i].kind == kBanGeneric) {
            gSoftBan[i].kind = kBanDead;
        }
        return;
    }
    if (gSoftBanN >= kSoftBanCap) {
        int victim = -1;
        for (int i = 0; i < gSoftBanN; ++i) {
            if (gSoftBan[i].untilMs == 0) {
                victim = i;
                break;
            }
        }
        if (victim < 0) {
            victim = 0;
            for (int i = 1; i < gSoftBanN; ++i) {
                if (gSoftBan[i].untilMs < gSoftBan[victim].untilMs) victim = i;
            }
        }
        gSoftBan[victim] = SoftBan{id, until, kind, 0};
        return;
    }
    gSoftBan[gSoftBanN++] = SoftBan{id, until, kind, 0};
}

// arm 之后调用（arm 内 ClearSoftBan）：softBan + 条件 fhBan。
// softBanOverrideMs!=0：固定时长（skate）；否则同怪阶梯 800/2000/4000。
// fhBanOnFirst：毒台连撞（326d34 wantFh=212 两连撞）— Doing 未到位 / 偏台滑走首击即链 ban。
void ApplyLandMissStickyPenalty(int missId, uint32_t missFh, float landX, float landY, DWORD now,
                                bool wantFhBan, const char* why, DWORD softBanOverrideMs = 0,
                                bool fhBanOnFirst = false) {
    // 先按「点」记账：坏点是确定性的，避开这一个点比封整块台面便宜得多（07a30a）。
    {
        const int ptStrikes = NoteBadLandPoint(landX, landY, now);
        if (ptStrikes > 0) {
            LogLine("land_miss badLand pt=(%.0f,%.0f) strikes=%d scope=%s why=%s", landX, landY,
                    ptStrikes, ptStrikes >= 2 ? "map" : "20s", why ? why : "?");
        }
    }
    if (missId > 0) {
        DWORD ms = softBanOverrideMs;
        int strikes = 0;
        if (!ms) {
            strikes = NoteLandMissMob(missId, now);
            ms = LandMissSoftBanMsForStrikes(strikes);
        }
        SoftBanFor(missId, now, ms, kBanUnreachable);
        LogLine("land_miss softBan id=%d %ums strikes=%d why=%s", missId, (unsigned)ms, strikes,
                why ? why : "?");
    }
    if (!wantFhBan || !missFh) return;
    const int fhStrikes = NoteLandMissFh(missFh, now);
    const int need = fhBanOnFirst ? 1 : 2;
    if (fhStrikes < need) {
        LogLine("land_miss fhBan skipped seed=%u strikes=%d need=%d why=%s", missFh, fhStrikes,
                need, why ? why : "?");
        return;
    }
    const int n = BanLandFhSameZChain(missFh, now, kLandFhBanSpread);
    LogLine("land_miss fhBan armed seed=%u chain=%d %ums strikes=%d why=%s", missFh, n,
            (unsigned)kLandFhBanMs, fhStrikes, why ? why : "?");
}

// 空砍满 N：累加 strikes，禁锁阶梯封顶（见 kWhiffSoftBan*）。
void SoftBanWhiff(int id, DWORD now) {
    if (id <= 0) return;
    PurgeSoftBan(now);
    int strikes = 1;
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].id != id) continue;
        strikes = static_cast<int>(gSoftBan[i].strikes) + 1;
        if (strikes > kWhiffSoftBanMaxStrikes) strikes = kWhiffSoftBanMaxStrikes;
        const DWORD ms = WhiffBanDurationMs(strikes);
        const DWORD until = now + ms;
        if (until > gSoftBan[i].untilMs) gSoftBan[i].untilMs = until;
        gSoftBan[i].kind = kBanWhiff;
        gSoftBan[i].strikes = static_cast<uint8_t>(strikes);
        LogLine("softBan whiff id=%d strikes=%d ms=%u", id, strikes, (unsigned)ms);
        return;
    }
    const DWORD ms = WhiffBanDurationMs(1);
    SoftBanFor(id, now, ms, kBanWhiff);
    for (int i = 0; i < gSoftBanN; ++i) {
        if (gSoftBan[i].id != id) continue;
        gSoftBan[i].strikes = 1;
        gSoftBan[i].kind = kBanWhiff;
        break;
    }
    LogLine("softBan whiff id=%d strikes=1 ms=%u", id, (unsigned)ms);
}

// acquire 全 miss：丢掉 generic/no_land 短禁；保留 whiff + 不可达 + dead，避免清禁后立刻鬼锁空转。
void DropSoftBanNonSticky() {
    int w = 0;
    for (int i = 0; i < gSoftBanN; ++i) {
        const SoftBanKind k = gSoftBan[i].kind;
        if (k == kBanWhiff || k == kBanUnreachable || k == kBanDead) gSoftBan[w++] = gSoftBan[i];
    }
    gSoftBanN = w;
}

bool LockHasEngaged() {
    if (gLock.dealtHits > 0) return true;
    if (gLock.lastHitted > 0) return true;
    if (gLock.lastHp >= 0 && gLock.lastHp < 100) return true;
    return false;
}

void FlipLandSideForCorrect(float playerX, float mobX) {
    if (gLandSide == 0) gLandSide = (playerX >= mobX) ? 1 : -1;
    gLandSide = -gLandSide;
}

bool StickyCorrectCooling(DWORD now) {
    return gLock.lastStickyCorrectMs &&
           static_cast<int>(now - gLock.lastStickyCorrectMs) <
               static_cast<int>(kStickyCorrectCooldownMs);
}

void ClearStickySpin() { gStickySpinSince = 0; }

void NoteStickySpin(DWORD now) {
    if (!gStickySpinSince) gStickySpinSince = now;
}

bool StickySpinExpired(DWORD now) {
    return gStickySpinSince &&
           static_cast<int>(now - gStickySpinSince) >= static_cast<int>(kStickySpinAbortMs);
}

// 同怪纠偏：翻侧；返回 false 表示应让路换怪（仅未交手且次数耗尽）。
// 冷却中返回 false——调用方须先 StickyCorrectCooling，禁止当成「成功」去 Aim sticky_cd。
bool TryCorrectSameLock(DWORD now, float playerX, float mobX, const char* why) {
    const bool engaged = LockHasEngaged();
    if (!engaged && gLock.stickyFixes >= kStickyCorrectMax) return false;
    if (StickyCorrectCooling(now)) return false;
    gLock.stickyFixes += 1;
    gLock.lastStickyCorrectMs = now;
    FlipLandSideForCorrect(playerX, mobX);
    LogLine("lock correct why=%s id=%d n=%d/%d side=%d engaged=%d", why ? why : "?", gLock.id,
            gLock.stickyFixes, kStickyCorrectMax, gLandSide, engaged ? 1 : 0);
    return true;
}

// hop 过小且未进带：止血 sticky 空转。true=已接管状态（调用方 continue/break 按返回约定）。
// 返回 true 且 outContinue=true → continue；true 且 outContinue=false → break。
bool HandleTinyHopSticky(DWORD now, float playerX, float playerY, float standOff, bool* outContinue) {
    if (!outContinue) return false;
    (void)standOff;
    *outContinue = false;
    NoteStickySpin(now);
    if (StickySpinExpired(now)) {
        SoftBanFor(gLock.id, now, kHopStickySoftBanMs);
        LogLine("MoveTo sticky_spin abort id=%d age=%ums softBan=%ums", gLock.id,
                (unsigned)(now - gStickySpinSince), (unsigned)kHopStickySoftBanMs);
        ClearStickySpin();
        ClearLock();
        EnterState(State::Acquire, now, "sticky_spin");
        *outContinue = true;
        return true;
    }
    if (StickyCorrectCooling(now)) {
        // 冷却中禁止 Aim↔MoveTo；近距强砍，否则原地等下一 tick。
        if (std::fabs(gLock.x - playerX) < kReapproachMinDx &&
            std::fabs(gLock.y - playerY) <= kSameFloorMaxDy) {
            ClearStickySpin();
            EnterState(State::Firing, now, "sticky_cool_fire");
            *outContinue = true;
            return true;
        }
        static DWORD sCoolLog = 0;
        if (!sCoolLog || now - sCoolLog > 500) {
            sCoolLog = now;
            LogLine("MoveTo sticky_cool wait id=%d remain=%ums dx=%.0f", gLock.id,
                    kStickyCorrectCooldownMs - (now - gLock.lastStickyCorrectMs),
                    gLock.x - playerX);
        }
        return true;  // break：留 MoveTo
    }
    if (!LockHasEngaged()) {
        if (TryCorrectSameLock(now, playerX, gLock.x, "hop_sticky")) {
            *outContinue = true;  // 刚翻侧，同 tick 重估
            return true;
        }
        SoftBanFor(gLock.id, now, kHopStickySoftBanMs);
        LogLine("MoveTo hop_sticky id=%d hop_tiny dx=%.0f — yield softBan %ums", gLock.id,
                gLock.x - playerX, (unsigned)kHopStickySoftBanMs);
        ClearStickySpin();
        ClearLock();
        EnterState(State::Acquire, now, "hop_sticky");
        *outContinue = true;
        return true;
    }
    // 已交手：翻侧再估；失败则强砍或等。
    if (TryCorrectSameLock(now, playerX, gLock.x, "center_hop")) {
        *outContinue = true;
        return true;
    }
    if (std::fabs(gLock.x - playerX) < kReapproachMinDx &&
        std::fabs(gLock.y - playerY) <= kSameFloorMaxDy) {
        ClearStickySpin();
        EnterState(State::Firing, now, "center_hop_fire");
        *outContinue = true;
        return true;
    }
    return true;  // break 等
}

void ArmWhiffObserve(DWORD now, int hpAtFire) {
    gLock.lockFires += 1;
    if (hpAtFire <= 0) return;
    if (gLock.armUntil == 0) {
        gLock.armHp = hpAtFire;
        gLock.armHitted = gLock.lastHitted;
        gLock.armUntil = now + kWhiffObserveMs;
        gLock.firesInArm = 1;
        gLock.hitProbeLogged = false;
    } else {
        gLock.firesInArm += 1;
    }
}

void NoteHittedForHitLag(DWORD now) {
    if (gLock.prevHittedSample < 0) {
        gLock.prevHittedSample = gLock.lastHitted;
        return;
    }
    if (gLock.lastHitted > gLock.prevHittedSample) {
        gLock.hitBumpCount += 1;
        if (!gLock.firstBumpMs) gLock.firstBumpMs = now;
        gLock.prevHittedSample = gLock.lastHitted;

        // 列表多半只留末条：bump 时读 +0x24 累加到 dealtSum（零参数够死）。
        if (gLock.ptr) {
            ports::mob::DamageInfoSnap di{};
            if (ports::mob::TryReadDamageInfoList(gLock.ptr, di) && di.ok && di.count > 0) {
                const ports::mob::DamageInfoLite& last = di.items[di.count - 1];
                if (last.damage > 0) {
                    gLock.dealtSum += last.damage;
                    gLock.dealtHits += 1;
                    gLock.lastDealt = last.damage;
                }
                if (kProbeDamageInfo) {
                    LogLine(
                        "dmg_probe id=%d hit=%d n=%d sum24=%lld last24=%d dealt=%lld/%lld hits=%d "
                        "skill=%d act=%d idx=%d char=%u delay=%.2f raw14=%d raw18=%d raw1C=%d "
                        "raw24=%d raw2C=%d raw34=%d hp%%=%d maxHp=%lld abs=%lld",
                        gLock.id, gLock.lastHitted, di.listSize,
                        static_cast<long long>(di.sumDamage), last.damage,
                        static_cast<long long>(gLock.dealtSum),
                        static_cast<long long>(gLock.maxHp), gLock.dealtHits, last.skillId,
                        last.hitAction, last.attackIdx, last.charId, last.delayed, last.raw[0],
                        last.raw[1], last.raw[2], last.raw[3], last.raw[4], last.raw[5],
                        gLock.lastHp, static_cast<long long>(gLock.maxHp),
                        static_cast<long long>(gLock.absHp));
                }
            } else if (kProbeDamageInfo) {
                LogLine("dmg_probe id=%d hit=%d list_fail_or_empty", gLock.id, gLock.lastHitted);
            }
        }
    }
}

// 干净门：权威 hp% 已相对开锁下降，且残血到门限（表/伤害只负责「可以开始考虑切」）。
bool LockHpCleanToLeave() {
    if (!kCleanAbandon) return true;
    if (gLock.lockStartHp < 0 || gLock.lastHp < 0) return false;
    if (gLock.lastHp >= gLock.lockStartHp) return false;  // 血条未动：黏住
    if (gLock.lastHp > kCleanAbandonMaxHpPct) return false;
    return true;
}

// 预测提前切怪后：gap ms 内禁止下一次贴怪瞬移，防「一刀一 fill」把主线程泵灌爆
// →拉高游戏 GC 频率→触发 Unity「Collecting from unknown thread」致命弹窗。
// gap = 面板「早切禁瞬移」(gOneshotFoxFillGapMs)，0=关，由用户自行评估。
// 覆盖所有 abandon_* 提前切路径；真死(dead_or_gone)/未命中(whiff)不进闸。
void ArmFoxFillGate(DWORD now) {
    const DWORD gap = gOneshotFoxFillGapMs.load(std::memory_order_acquire);
    if (gap == 0) {
        gFoxFillGateUntil = 0;  // 热改 280→0 时清掉未到期闸
        return;
    }
    gFoxFillGateUntil = now + gap;
    LogLine("fox_fill_gate arm %ums until=%u", gap, gFoxFillGateUntil);
}

// 零参数够死 + 干净确认：Σ Damage ≥ maxHP 且血条已残，才切。
bool TryAbandonDealtSum(DWORD now) {
    (void)now;
    if (gLock.maxHp <= 0) return false;
    if (gLock.dealtHits <= 0 || gLock.dealtSum <= 0) return false;
    if (gLock.dealtSum < gLock.maxHp) return false;
    if (!LockHpCleanToLeave()) return false;

    LogLine(
        "switch reason=abandon_dealt id=%d tpl=%d maxHp=%lld dealt=%lld hits=%d last=%d hp=%d%% "
        "src=%s clean=1",
        gLock.id, gLock.templateId, static_cast<long long>(gLock.maxHp),
        static_cast<long long>(gLock.dealtSum), gLock.dealtHits, gLock.lastDealt, gLock.lastHp,
        ports::mob::AbsHpSrcName(gLock.absSrc));
    SoftBanFor(gLock.id, now, kEarlyAbandonSoftBanMs);
    ArmFoxFillGate(now);
    gLastLockLostWhy = "abandon_dealt";
    ClearLock();
    return true;
}

void NoteLockHpSample() {
    if (gLock.lastHp < 0) return;
    if (gLock.predictSampleHp < 0) {
        gLock.predictSampleHp = gLock.lastHp;
        gLock.predictFiresAtHp = gLock.lockFires;
        return;
    }
    if (gLock.lastHp < gLock.predictSampleHp) {
        gLock.predictDropEvents += 1;
        gLock.predictSampleHp = gLock.lastHp;
        gLock.predictFiresAtHp = gLock.lockFires;
    } else if (gLock.lastHp > gLock.predictSampleHp) {
        gLock.predictSampleHp = gLock.lastHp;
        gLock.predictFiresAtHp = gLock.lockFires;
    }
}

static int64_t AbsHpFromPct(int64_t maxHp, int32_t pct) {
    if (maxHp <= 0 || pct <= 0) return 0;
    if (pct >= 100) return maxHp;
    return (maxHp * static_cast<int64_t>(pct) + 50) / 100;  // 四舍五入
}

// 加速秒杀档：脆皮 maxHP → 可早切（不必等 hp%/死亡包）。
// bumps≥1：等 lastHitted 确认（AddDamageInfo 当帧写 +0x208）。
// bumps=0：射后不管——本地出刀达标即切（不等命中回写）。
bool TryAbandonOneshot(DWORD now) {
    if (!attack_accel::IsDesired()) return false;
    const int64_t maxHpGate = gOneshotMaxHp.load(std::memory_order_acquire);
    if (maxHpGate <= 0) return false;  // 面板关秒杀道
    if (gLock.maxHp <= 0 || gLock.maxHp > maxHpGate) return false;
    if (gLock.lockFires < gOneshotMinFires.load(std::memory_order_acquire)) return false;

    const int minBumps = gOneshotMinBumps.load(std::memory_order_acquire);
    if (minBumps > 0) {
        if (gLock.hitBumpCount < minBumps) return false;
        const DWORD minLag = gOneshotMinLagMs.load(std::memory_order_acquire);
        if (!gLock.firstBumpMs || static_cast<int>(now - gLock.firstBumpMs) <
                                      static_cast<int>(minLag)) {
            return false;
        }
    }
    // 干净：禁止 fox/脆皮在 hp=100% 就走。
    if (!LockHpCleanToLeave()) return false;

    LogLine(
        "switch reason=abandon_oneshot id=%d tpl=%d maxHp=%lld hp=%d bumps=%d fires=%d lag=%ums "
        "fox=%d clean=1",
        gLock.id, gLock.templateId, static_cast<long long>(gLock.maxHp), gLock.lastHp,
        gLock.hitBumpCount, gLock.lockFires, gLock.firstBumpMs ? (now - gLock.firstBumpMs) : 0u,
        minBumps <= 0 ? 1 : 0);
    SoftBanFor(gLock.id, now, kEarlyAbandonSoftBanMs);
    ArmFoxFillGate(now);
    gLastLockLostWhy = "abandon_oneshot";
    ClearLock();
    return true;
}

// 加速 + 有 maxHP 表：用绝对剩余 vs 均刀×在途刀预测够死（不等死亡包）。
bool TryAbandonAbsHp(DWORD now) {
    if (!attack_accel::IsDesired()) return false;
    if (gLock.maxHp <= 0) return false;
    if (gLock.lockFires < kAbsHpAbandonMinFires) return false;
    if (gLock.predictDropEvents < kAbsHpAbandonMinDrops) return false;
    if (gLock.lockStartHp < 0 || gLock.predictSampleHp < 0) return false;
    if (gLock.predictFiresAtHp <= 0) return false;

    const int droppedPct = gLock.lockStartHp - gLock.predictSampleHp;
    if (droppedPct <= 0) return false;

    const int64_t dealt =
        (gLock.maxHp * static_cast<int64_t>(droppedPct) + 50) / 100;
    const double avgPerFire =
        static_cast<double>(dealt) / static_cast<double>(gLock.predictFiresAtHp);
    if (!(avgPerFire > 0.0)) return false;

    const int pending = gLock.lockFires - gLock.predictFiresAtHp;
    const int pend = pending < 0 ? 0 : pending;
    const bool absLive = gLock.absSrc == ports::mob::AbsHpSrc::UiHpTag ||
                         gLock.absSrc == ports::mob::AbsHpSrc::UiHpTagCache;
    const int64_t remain =
        (absLive && gLock.absHp >= 0) ? gLock.absHp : AbsHpFromPct(gLock.maxHp, gLock.lastHp);
    const double predicted = static_cast<double>(remain) - avgPerFire * static_cast<double>(pend);
    const bool oneShotLeft = remain > 0 && remain <= static_cast<int64_t>(avgPerFire + 0.5) && pend >= 1;

    if (!(predicted <= 0.0 || oneShotLeft)) return false;
    if (!LockHpCleanToLeave()) return false;

    LogLine("switch reason=abandon_abshp id=%d tpl=%d maxHp=%lld hp=%d%% remain=%lld src=%s "
            "dealt=%lld fires=%d@hp=%d pend=%d avg=%.1f pred=%.1f oneshot=%d clean=1",
            gLock.id, gLock.templateId, static_cast<long long>(gLock.maxHp), gLock.lastHp,
            static_cast<long long>(remain), ports::mob::AbsHpSrcName(gLock.absSrc),
            static_cast<long long>(dealt), gLock.lockFires, gLock.predictFiresAtHp, pend,
            avgPerFire, predicted, oneShotLeft ? 1 : 0);
    SoftBanFor(gLock.id, now, kEarlyAbandonSoftBanMs);
    ArmFoxFillGate(now);
    gLastLockLostWhy = "abandon_abshp";
    ClearLock();
    return true;
}

// 表缺失时的 hitlag 兜底：lastHitted 已命中但 hp% 长时间未动。
bool TryAbandonHitLag(DWORD now) {
    if (kCleanAbandon) return false;  // 干净模式：hp 未动绝不因 hitlag 换怪
    if (!attack_accel::IsDesired()) return false;
    if (gLock.maxHp > 0) return false;  // 有表走绝对血闸
    if (gLock.hitBumpCount < kHitLagAbandonMinBumps) return false;
    if (gLock.lockFires < kHitLagAbandonMinFires) return false;
    if (!gLock.firstBumpMs || static_cast<int>(now - gLock.firstBumpMs) <
                                  static_cast<int>(kHitLagAbandonMs)) {
        return false;
    }
    if (gLock.lockStartHp < 0 || gLock.lastHp < gLock.lockStartHp) return false;

    LogLine("switch reason=abandon_hitlag id=%d hp=%d startHp=%d bumps=%d fires=%d lag=%ums",
            gLock.id, gLock.lastHp, gLock.lockStartHp, gLock.hitBumpCount, gLock.lockFires,
            now - gLock.firstBumpMs);
    SoftBanFor(gLock.id, now, kEarlyAbandonSoftBanMs);
    ArmFoxFillGate(now);
    gLastLockLostWhy = "abandon_hitlag";
    ClearLock();
    return true;
}

// 窗内掉血 → 清 whiff；窗满无掉血 → +1；满 N → 清锁并返回 false。
// lastHitted 旁路：默认只探针；kWhiffClearOnLastHitted=true 时才参与清窗。
bool ResolveWhiffArm(DWORD now) {
    if (gLock.armUntil == 0) return true;

    const bool hpDrop =
        gLock.lastHp >= 0 && gLock.armHp >= 0 && gLock.lastHp < gLock.armHp;
    const bool hitBump =
        gLock.armHitted >= 0 && gLock.lastHitted > gLock.armHitted;

    if (kProbeLastHittedLog && hitBump && !gLock.hitProbeLogged) {
        gLock.hitProbeLogged = true;
        LogLine("hit_probe id=%d lastHitted=%d→%d hp=%d→%d fires=%d clear=%d", gLock.id,
                gLock.armHitted, gLock.lastHitted, gLock.armHp, gLock.lastHp, gLock.firesInArm,
                kWhiffClearOnLastHitted ? 1 : 0);
    }

    if (hpDrop || (kWhiffClearOnLastHitted && hitBump)) {
        if (gLock.whiff || gLock.firesInArm || hitBump) {
            LogLine("whiff clear id=%d why=%s hp=%d→%d hit=%d→%d fires=%d", gLock.id,
                    hpDrop ? "hp" : "lastHitted", gLock.armHp, gLock.lastHp, gLock.armHitted,
                    gLock.lastHitted, gLock.firesInArm);
        }
        gLock.whiff = 0;
        ClearWhiffArm();
        return true;
    }
    if (now < gLock.armUntil) return true;

    // 已打残且本锁曾命中：窗满无掉血/bump。
    if (gLock.lastHitted > 0 && gLock.lastHp > 0 && gLock.lastHp < 100) {
        bool stallLeave = false;
        if (attack_accel::IsDesired() && gLock.lockFires >= kWoundedStallAbandonMinFires) {
            if (gLock.maxHp > 0 && gLock.predictFiresAtHp > 0 && gLock.predictDropEvents > 0) {
                const int droppedPct = gLock.lockStartHp - gLock.predictSampleHp;
                if (droppedPct > 0) {
                    const int64_t dealt =
                        (gLock.maxHp * static_cast<int64_t>(droppedPct) + 50) / 100;
                    const double avg =
                        static_cast<double>(dealt) / static_cast<double>(gLock.predictFiresAtHp);
                    const int64_t remain = AbsHpFromPct(gLock.maxHp, gLock.lastHp);
                    stallLeave = remain > 0 && remain <= static_cast<int64_t>(avg + 0.5);
                }
            }
            if (!stallLeave && gLock.lastHp <= kWoundedStallAbandonMaxHp) stallLeave = true;
        }
        if (stallLeave) {
            LogLine("switch reason=abandon_wounded_stall id=%d hp=%d hit=%d fires=%d maxHp=%lld",
                    gLock.id, gLock.lastHp, gLock.lastHitted, gLock.lockFires,
                    static_cast<long long>(gLock.maxHp));
            SoftBanFor(gLock.id, now, kEarlyAbandonSoftBanMs);
            ArmFoxFillGate(now);
            gLastLockLostWhy = "abandon_wounded_stall";
            ClearLock();
            return false;
        }
        if (kWhiffWoundedReapproach) {
            // 残血观察窗满仍无掉血：侧移纠偏续砍，禁止干等（叠怪心死区会空等数秒）。
            LogLine("whiff wounded id=%d hp=%d hit=%d fires=%d → reapproach", gLock.id,
                    gLock.lastHp, gLock.lastHitted, gLock.firesInArm);
            ClearWhiffArm();
            gLock.needApproachCorrect = true;
            gLock.whiff = 0;
            return true;
        }
    }

    gLock.whiff += 1;
    LogLine("whiff tick id=%d streak=%d/%d hp=%d hit=%d fires=%d observe=%ums", gLock.id,
            gLock.whiff, kWhiffFireN, gLock.lastHp, gLock.lastHitted, gLock.firesInArm,
            (unsigned)kWhiffObserveMs);
    ClearWhiffArm();
    if (gLock.whiff < kWhiffFireN) return true;

    // 没打死 / 已交手：清 streak、翻侧重贴，禁止换怪。
    if (LockHasEngaged()) {
        LogLine("whiff correct why=engaged id=%d fires=%d hp=%d hit=%d dealt=%d", gLock.id,
                gLock.whiff, gLock.lastHp, gLock.lastHitted, gLock.dealtHits);
        gLock.whiff = 0;
        ClearWhiffArm();
        gLock.needApproachCorrect = true;
        return true;
    }

    LogLine("switch reason=whiff id=%d fires=%d hp=%d hit=%d", gLock.id, gLock.whiff, gLock.lastHp,
            gLock.lastHitted);
    SoftBanWhiff(gLock.id, now);
    gLastLockLostWhy = "whiff";
    ClearLock();
    return false;
}

bool SameLayerY(float a, float b) { return std::fabs(a - b) <= kSameLayerY; }

bool SameFloorY(float a, float b) { return std::fabs(a - b) <= kSameFloorMaxDy; }

// 玩家优先 CurFh.zMass；怪用点附近站立 FH 的 zMass。解析失败退回 Y 带。
bool TryPlayerZMass(float px, float py, int32_t* outZm) {
    if (!outZm) return false;
    const uint32_t cur = ports::foothold::PeekCurFhId();
    if (cur && ports::foothold_path::ZMassOfFh(cur, outZm)) return true;
    return ports::foothold_path::ZMassAt(px, py, outZm);
}

bool SameLayer(float ax, float ay, float bx, float by) {
    // 直贴模式：只用 |ΔY|，跳过 CurFh/zMass 查询。
    if (kDirectTeleportNoLayerHop) return SameLayerY(ay, by);
    int32_t za = 0, zb = 0;
    if (TryPlayerZMass(ax, ay, &za) && ports::foothold_path::ZMassAt(bx, by, &zb)) {
        // zMass 相等仍须近似同高，否则多层图会被拉去别的地板。
        return za == zb && SameFloorY(ay, by);
    }
    return SameLayerY(ay, by);
}

bool SameLayerZm(int32_t playerZm, bool playerZmOk, float py, float mx, float my, uint32_t mobFh) {
    if (kDirectTeleportNoLayerHop) return SameLayerY(py, my);
    int32_t mz = 0;
    bool mobOk = false;
    if (mobFh) mobOk = ports::foothold_path::ZMassOfFh(mobFh, &mz);
    if (!mobOk) mobOk = ports::foothold_path::ZMassAt(mx, my, &mz);
    if (playerZmOk && mobOk) return playerZm == mz && SameFloorY(py, my);
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

bool IsForbiddenOriginLand(float x, float y) {
    return std::fabs(x) < kForbiddenOriginEps && std::fabs(y) < kForbiddenOriginEps;
}

// fill 落点安全：非有限 / 原点邻域 / 贴 x≈0 轴 / 出本图 FH 外包 → 拒。
// fhId≠0（已 Snap 种台）：margin=0，与 teleport_port 一致（24px 内缩会误杀外沿台）。
bool LandSafeForFill(float x, float y, uint32_t fhId = 0) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (IsForbiddenOriginLand(x, y)) return false;
    if (std::fabs(x) < kMinLandAbsAxisX) return false;
    if (IsBadLandPoint(x, y, GetTickCount())) return false;
    if (fhId && IsLandFhBanned(fhId, GetTickCount())) return false;
    const int margin =
        fhId != 0 ? 0 : ports::map_bounds::kLandMarginPx;
    if (!ports::map_bounds::PointInPlayBounds(x, y, /*mapId=*/0, margin)) return false;
    return true;
}

bool TryEnterMoveTo(DWORD now, const char* why) {
    if (gFoxFillGateUntil && static_cast<int>(now - gFoxFillGateUntil) < 0) {
        static DWORD sFox = 0;
        if (!sFox || now - sFox > 1500) {
            sFox = now;
            LogLine("MoveTo defer why=%s fox_fill_gate remain=%ums", why ? why : "?",
                    gFoxFillGateUntil - now);
        }
        RenewLootPulseHold(now);
        return false;
    }
    if (!kDirectTeleportNoLayerHop && gCrossLayerFillGateUntil &&
        static_cast<int>(now - gCrossLayerFillGateUntil) < 0) {
        static DWORD sCross = 0;
        if (!sCross || now - sCross > 1500) {
            sCross = now;
            LogLine("MoveTo defer why=%s cross_layer_fill_gate remain=%ums", why ? why : "?",
                    gCrossLayerFillGateUntil - now);
        }
        RenewLootPulseHold(now);
        return false;
    }
    if (x::runtime::main_thread::IsCongested()) {
        static DWORD sCong = 0;
        if (!sCong || now - sCong > 1500) {
            sCong = now;
            LogLine("MoveTo defer why=%s pump_congested q=%d", why ? why : "?",
                    x::runtime::main_thread::QueuedJobCount());
        }
        RenewLootPulseHold(now);
        return false;
    }
    if (!CanTeleportNow()) {
        static DWORD sCd = 0;
        if (!sCd || now - sCd > 1500) {
            sCd = now;
            LogLine("MoveTo defer why=%s cd_remain=%ums", why ? why : "?",
                    ports::teleport::NativeCooldownRemainingMs());
        }
        RenewLootPulseHold(now);
        return false;
    }
    // 额度不够连最短一跳：留原地等回额，别进 MoveTo（1d3308：Acquire↔MoveTo 每秒 22 次空转）。
    {
        const DWORD wait = FillDistBudgetWaitMs(now, kMinReapproachHop, nullptr);
        if (wait > 0) {
            static DWORD sBudget = 0;
            if (!sBudget || now - sBudget > 1500) {
                sBudget = now;
                LogLine("MoveTo defer why=%s dist_budget remain=%ums", why ? why : "?",
                        (unsigned)wait);
            }
            RenewLootPulseHold(now);
            return false;
        }
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

// 无脑A 续砍带：同层且未到重贴距离 → 继续打（与 NeedsReapproach 无缝衔接）。
// 含 dx≈0 怪心重叠：旧版 dx<1 拒刀 + Aim 不重贴 → melee_wait_tp 空等（upload d8cd64 ≈7s）。
// BIN 7b792b：起伏双台 |dy|~50 但 dx 已近 → 仍可续砍，勿因 SameLayerY(45) 卡死。
bool NearMeleeFloor(float playerX, float playerY, float mobX, float mobY) {
    const float dx = std::fabs(mobX - playerX);
    const float dy = std::fabs(mobY - playerY);
    return dx < kReapproachMinDx && dy <= kSameFloorMaxDy;
}

bool InMeleeHoldBand(float playerX, float playerY, float mobX, float mobY, float standOff) {
    (void)standOff;
    if (NearMeleeFloor(playerX, playerY, mobX, mobY)) return true;
    if (!SameLayer(playerX, playerY, mobX, mobY)) return false;
    const float dx = std::fabs(mobX - playerX);
    return dx < kReapproachMinDx;
}

bool NeedsReapproach(float playerX, float playerY, float mobX, float mobY) {
    // 近距 + 纵漂≤同高容差：禁止 reapproach_cross 对同一错台反复 fill。
    if (NearMeleeFloor(playerX, playerY, mobX, mobY)) return false;
    if (!SameLayer(playerX, playerY, mobX, mobY)) return true;
    return std::fabs(mobX - playerX) >= kReapproachMinDx;
}

// 残血/空砍纠偏：离开真命中带即强制重贴。
// 旧逻辑用 NeedsReapproach(dx≥100)，与 InMeleeHoldBand(dx<100) 互斥——树妖击退到
// 50~90 时刷 whiff wounded→reapproach 却走 still_valid 空砍假死
// （upload E226_27e7a113 21:54 樹妖王 dx=54~88，265 次无 MoveTo）。
// 返回 true：本 tick 已进 MoveTo（*outMoved）或应让路、禁止 still_valid 空砍。
bool TryConsumeWhiffApproachCorrect(DWORD now, float playerX, float playerY, float standOff,
                                    bool tpOn, bool* outMoved) {
    if (outMoved) *outMoved = false;
    if (!gLock.needApproachCorrect) return false;
    if (InHitBand(playerX, playerY, gLock.x, gLock.y, standOff)) {
        gLock.needApproachCorrect = false;
        return false;
    }
    if (!tpOn) {
        gLock.needApproachCorrect = false;
        return false;
    }
    if (StickyCorrectCooling(now)) return true;
    const int prevSide = gLandSide;
    FlipLandSideForCorrect(playerX, gLock.x);
    if (TryEnterMoveTo(now, "whiff_reapproach")) {
        gLock.needApproachCorrect = false;
        gLock.lastStickyCorrectMs = now;
        if (outMoved) *outMoved = true;
        return true;
    }
    gLandSide = prevSide;
    return true;  // CD/gate：保留 flag，禁止空砍
}

// P0：真远距离禁止空砍；已锁近距无脑A放行。
bool FireGateOk(float playerX, float playerY, float mobX, float mobY, float standOff, DWORD now,
                const char* where) {
    if (InHitBand(playerX, playerY, mobX, mobY, standOff)) return true;
    if (gLock.id && InMeleeHoldBand(playerX, playerY, mobX, mobY, standOff)) return true;
    static DWORD sLog = 0;
    if (!sLog || now - sLog > 400) {
        sLog = now;
        LogLine("fire gate where=%s id=%d dx=%.0f — await_band", where ? where : "?", gLock.id,
                mobX - playerX);
    }
    return false;
}

// noLand 归因：日志只报总数时无法区分「怪台贴不到」和「落点被判不安全」，沼澤地 100% noLand
// 查了两轮仍在猜（BIN 577349/9fd287）。仅 ExplainAcquireMiss 传非空，其余路径零开销。
enum class LandFail : uint8_t {
    kNone,
    kSnap,    // SnapStandAt 贴不到台 / 台 id 为 0
    kLayer,   // 怪台 Y 与怪心差 > kSameLayerY（怪不在可站面附近：浮空 / 水里 / 台被跳过）
    kFhBan,   // 怪台在封台表内
    kSides,   // 两侧 standOff 偏移都不可站（仅严格档；loose 档贴怪台兜底）
    kUnsafe,  // LandSafeForFill 拒（原点邻域 / x≈0 / 坏点 / 出 FH 外包）
    kFar,     // 贴到的台不在怪脚下（X 隔太远）：落过去也打不到
};

// 怪台与怪心的最大 X 距离。Snap 找不到覆盖怪的台时会退回「同高的远台」，若不拦，
// 落点会算到几百 px 外却谎报 hop≈0，于是对空开枪 + MoveTo sticky_spin 空转
//（BIN 0ea69f：怪 dx=-650 仍报 hop~0，86 次 sticky_spin abort）。
// 取续砍带：更远的落点即便落下去也要立刻重贴，等于没接近。
constexpr float kMaxStandAwayX = kReapproachMinDx;

// 落点 = 怪台 ± standOff（左右任一侧，禁止塌怪心）。
// 偏移后必须重新 SnapStandAt（可换邻段 FH），禁止死钉原 standFh+SnapOnFh：
// BIN c0e40b 起伏地：同 fh 插值 Y 与引擎真台差 ~50px → settle 后 reapproach_cross 死循环。
// loose=true：选怪饿死时放宽（短站距 / 怪心贴台），避免「池里有怪却 miss 站桩」。
bool EstimateLand(float px, float py, float mx, float my, float standOff, float* outHop,
                  float* outTx, float* outTy, uint32_t* outFh, int* outSide = nullptr,
                  bool loose = false, LandFail* outFail = nullptr) {
    auto fail = [&](LandFail r) -> bool {
        if (outFail) *outFail = r;
        return false;
    };
    // preferFlat：斜坡落点结算后横滑 16~27px 跨缝/窜层（1d2b0b doing_miss 主源）。
    // 「平台不覆盖时自动回退斜坡」曾经不成立——runPick 的 bestAny 兜底让平台趟永不失败，
    // 落点被甩到隔层的平台上（BIN 577349 沼澤地 noLand=100%）。现已按命中档判定，见 SnapStandAt。
    float standX = mx, standY = my;
    uint32_t standFh = 0;
    if (!ports::foothold_path::SnapStandAt(mx, my, &standX, &standY, &standFh,
                                           /*preferFlat=*/true) ||
        !standFh)
        return fail(LandFail::kSnap);
    if (std::fabs(standY - my) > kSameLayerY) return fail(LandFail::kLayer);
    if (std::fabs(standX - mx) > kMaxStandAwayX) return fail(LandFail::kFar);
    if (standFh && IsLandFhBanned(standFh, GetTickCount())) return fail(LandFail::kFhBan);

    const float useOff = loose ? (standOff > 12.f ? standOff * 0.5f : standOff) : standOff;
    const float minAway =
        loose ? kMinLandAway
              : ((useOff * 0.75f > kMinLandAway) ? useOff * 0.75f : kMinLandAway);
    auto trySide = [&](float sign, float* ox, float* oy, uint32_t* ofh) -> float {
        const float idealX = standX + sign * useOff;
        float lx = idealX, ly = standY;
        uint32_t lfh = 0;
        // 按目标高度重贴：邻段/斜坡由 Snap 选正确 FH，勿 SnapOnFh(原 fh)。
        if (!ports::foothold_path::SnapStandAt(idealX, standY, &lx, &ly, &lfh,
                                               /*preferFlat=*/true) ||
            !lfh)
            return -1.f;
        // Y 漂过大 = 窜到另一层/错台，退回让对侧或 loose 怪台兜底。
        if (std::fabs(ly - standY) > kSameLayerY) return -1.f;
        if (std::fabs(ly - my) > kSameLayerY) return -1.f;
        if (std::fabs(lx - standX) < minAway) return -1.f;
        // 坏点在此就拒，让对侧偏移接手；只靠末尾 LandSafeForFill 会整只怪判 no_land。
        if (IsBadLandPoint(lx, ly, GetTickCount())) return -1.f;
        if (!ports::foothold_path::IsXSafeOnFh(lfh, lx)) return -1.f;
        const float dragTol = loose ? useOff * 1.25f : useOff * 0.75f;
        if (std::fabs(lx - idealX) > dragTol) return -1.f;
        *ox = lx;
        *oy = ly;
        *ofh = lfh;
        return std::fabs(lx - standX);
    };

    float pref = 0.f;
    if (gLandSide != 0) {
        pref = static_cast<float>(gLandSide);
    } else {
        pref = (px >= standX) ? 1.f : -1.f;
    }
    float txA = 0, tyA = 0, txB = 0, tyB = 0;
    uint32_t fhA = 0, fhB = 0;
    const float gA = trySide(pref, &txA, &tyA, &fhA);
    const float gB = trySide(-pref, &txB, &tyB, &fhB);

    float tx = 0, ty = 0;
    uint32_t landFh = standFh;
    int chosenSide = 0;
    if (gA >= minAway) {
        tx = txA;
        ty = tyA;
        landFh = fhA;
        chosenSide = (pref > 0.f) ? 1 : -1;
    } else if (gB >= minAway) {
        tx = txB;
        ty = tyB;
        landFh = fhB;
        chosenSide = (pref > 0.f) ? -1 : 1;
    } else if (loose) {
        // 两侧偏移都不可站（起伏碎台）：贴怪台 snap，宁可贴怪心也不错层循环。
        tx = standX;
        ty = standY;
        landFh = standFh;
        chosenSide = (pref > 0.f) ? 1 : -1;
    } else {
        return fail(LandFail::kSides);
    }

    if (!LandSafeForFill(tx, ty, landFh)) return fail(LandFail::kUnsafe);

    if (outSide) *outSide = chosenSide;

    const float dx = tx - px, dy = ty - py;
    const float hop = std::sqrt(dx * dx + dy * dy);
    if (outHop) *outHop = hop;
    if (outTx) *outTx = tx;
    if (outTy) *outTy = ty;
    if (outFh) *outFh = landFh;
    return std::isfinite(hop);
}

bool EstimateLandPrefer(float px, float py, float mx, float my, float standOff, float* outHop,
                        float* outTx, float* outTy, uint32_t* outFh, int* outSide = nullptr,
                        LandFail* outFail = nullptr) {
    if (EstimateLand(px, py, mx, my, standOff, outHop, outTx, outTy, outFh, outSide, false))
        return true;
    // 归因只取 loose 档：严格档的 kSides 会被 loose 的贴台兜底救回，报它会误导。
    return EstimateLand(px, py, mx, my, standOff, outHop, outTx, outTy, outFh, outSide, true,
                        outFail);
}

// 远距分段：线性插值点经 SnapStandAt 后常被拉到远台（BIN：maxHop=400 → step=911）。
// 只接受 chop∈[min,maxHop] 且更靠近最终落点的候选；同台 SnapOnFh 作兜底。
bool TryChunkApproachLand(float px, float py, float goalX, float goalY, float maxHop, float* outHop,
                          float* outTx, float* outTy, uint32_t* outFh) {
    if (!outHop || !outTx || !outTy || !outFh) return false;
    *outHop = 0.f;
    *outTx = px;
    *outTy = py;
    *outFh = 0;
    if (!std::isfinite(maxHop) || maxHop < kMinReapproachHop) return false;

    const float gdx = goalX - px;
    const float gdy = goalY - py;
    const float goalHop = std::sqrt(gdx * gdx + gdy * gdy);
    if (!std::isfinite(goalHop) || !(goalHop > maxHop)) return false;

    float bestHop = 0.f, bestX = 0.f, bestY = 0.f;
    uint32_t bestFh = 0;
    float bestScore = -1.f;

    auto consider = [&](float sx, float sy, uint32_t sfh) {
        if (!sfh || !std::isfinite(sx) || !std::isfinite(sy)) return;
        if (!LandSafeForFill(sx, sy, sfh)) return;
        const float cdx = sx - px;
        const float cdy = sy - py;
        const float chop = std::sqrt(cdx * cdx + cdy * cdy);
        if (!std::isfinite(chop) || chop < kMinReapproachHop || chop > maxHop) return;
        const float remain = std::hypotf(goalX - sx, goalY - sy);
        // 必须更靠近最终落点，拒绝 Snap 拉到反方向/更远台。
        if (!(remain < goalHop - 1.f)) return;
        if (chop > bestScore) {
            bestScore = chop;
            bestHop = chop;
            bestX = sx;
            bestY = sy;
            bestFh = sfh;
        }
    };

    static constexpr float kFracs[] = {1.f, 0.85f, 0.7f, 0.55f, 0.4f, 0.3f, 0.2f};
    for (float frac : kFracs) {
        const float dist = maxHop * frac;
        const float t = dist / goalHop;
        const float ix = px + gdx * t;
        const float iy = py + gdy * t;
        float sx = 0.f, sy = 0.f;
        uint32_t sfh = 0;
        // 中继点自由选台：平台优先，避免斜坡结算横滑制造 doing_miss（1d2b0b）。
        if (!ports::foothold_path::SnapStandAt(ix, iy, &sx, &sy, &sfh, /*preferFlat=*/true) || !sfh)
            continue;
        consider(sx, sy, sfh);
    }

    uint32_t curFh = ports::foothold::PeekCurFhId();
    if (!curFh) {
        float cx = px, cy = py;
        (void)ports::foothold_path::SnapStandAt(px, py, &cx, &cy, &curFh);
    }
    if (curFh) {
        const float sign = (gdx >= 0.f) ? 1.f : -1.f;
        for (float frac : kFracs) {
            float sx = 0.f, sy = 0.f;
            if (!ports::foothold_path::SnapOnFh(curFh, px + sign * maxHop * frac, &sx, &sy))
                continue;
            consider(sx, sy, curFh);
        }
    }

    if (!bestFh || !(bestHop >= kMinReapproachHop)) return false;
    *outHop = bestHop;
    *outTx = bestX;
    *outTy = bestY;
    *outFh = bestFh;
    return true;
}

// hop≤maxHop 可直达；更远则须能分出合法中间落点，否则视为当前不可达。
bool CanApproachWithinMaxHop(float px, float py, float goalX, float goalY, float maxHop) {
    const float gdx = goalX - px;
    const float gdy = goalY - py;
    const float hop = std::sqrt(gdx * gdx + gdy * gdy);
    if (!std::isfinite(hop) || hop < 0.f) return false;
    if (hop <= maxHop) return true;
    float step = 0.f, sx = 0.f, sy = 0.f;
    uint32_t sfh = 0;
    return TryChunkApproachLand(px, py, goalX, goalY, maxHop, &step, &sx, &sy, &sfh);
}

// BIN：对账 ResolveAbsHp（UIHpTag FindAll → 缓存 → %×表）。不做 ShowMobHpTag hook。
void MaybeProbeUiHpTag(DWORD now) {
    if (!kProbeUiHpTag) return;
    static DWORD sLastScan = 0;
    static DWORD sLastMissLog = 0;
    static int32_t sPrevId = 0;
    static int64_t sPrevCur = -1;
    static int64_t sPrevMax = -1;
    static ports::mob::AbsHpSrc sPrevSrc = ports::mob::AbsHpSrc::None;
    if (sLastScan && now - sLastScan < kProbeUiHpTagPeriodMs) return;
    sLastScan = now;

    ports::mob::AbsHp abs{};
    if (!ports::mob::ResolveAbsHp(gLock.id, gLock.templateId, gLock.lastHp, abs,
                                  /*refreshUi=*/true) ||
        !abs.ok) {
        if (!sLastMissLog || now - sLastMissLog >= kProbeUiHpTagMissLogMs) {
            sLastMissLog = now;
            LogLine("uihp_probe hit=0 lockId=%d hp%%=%d", gLock.id, gLock.lastHp);
        }
        return;
    }

    gLock.maxHp = abs.max;
    gLock.absHp = abs.cur;
    gLock.absSrc = abs.src;

    const bool absLive = abs.src == ports::mob::AbsHpSrc::UiHpTag ||
                         abs.src == ports::mob::AbsHpSrc::UiHpTagCache;
    if (!absLive) {
        if (!sLastMissLog || now - sLastMissLog >= kProbeUiHpTagMissLogMs) {
            sLastMissLog = now;
            LogLine("uihp_probe hit=0 src=pct lockId=%d est=%lld/%lld hp%%=%d", gLock.id,
                    static_cast<long long>(abs.cur), static_cast<long long>(abs.max),
                    gLock.lastHp);
        }
        return;
    }

    const bool changed = abs.src != sPrevSrc || gLock.id != sPrevId || abs.cur != sPrevCur ||
                         abs.max != sPrevMax;
    if (!changed) return;
    sPrevSrc = abs.src;
    sPrevId = gLock.id;
    sPrevCur = abs.cur;
    sPrevMax = abs.max;

    const int64_t tplMax = ports::mob::LookupTemplateMaxHp(gLock.templateId);
    const int64_t est = AbsHpFromPct(tplMax, gLock.lastHp);
    int tagPct = 0;
    if (abs.max > 0) tagPct = static_cast<int>((abs.cur * 100 + abs.max / 2) / abs.max);
    LogLine("uihp_probe hit=1 src=%s id=%d cur=%lld max=%lld hp%%=%d tagPct=%d tplMax=%lld "
            "est=%lld maxMatch=%d pctMatch=%d",
            ports::mob::AbsHpSrcName(abs.src), gLock.id, static_cast<long long>(abs.cur),
            static_cast<long long>(abs.max), gLock.lastHp, tagPct,
            static_cast<long long>(tplMax), static_cast<long long>(est),
            (tplMax > 0 && abs.max == tplMax) ? 1 : 0, (tagPct == gLock.lastHp) ? 1 : 0);
}

bool RefreshLock(const ports::mob::Snapshot& snap) {
    if (!gLock.id || !gLock.ptr) return false;
    const DWORD now = GetTickCount();

    // 热路径：直读锁怪指针，不等 mobscan 缓存（对齐同行：死了立刻切）。
    ports::mob::MobLite live{};
    if (!ports::mob::TryFillLive(gLock.ptr, gLock.id, live)) {
        LogLine("switch reason=dead_or_gone id=%d via=live", gLock.id);
        ports::mob::InvalidateAbsHpCache(gLock.id);
        SoftBanFor(gLock.id, now, kDeadSoftBanMs, kBanDead);
        gLastLockLostWhy = "dead_or_gone";
        ClearLock();
        return false;
    }
    gLock.ptr = live.ptr;
    gLock.x = live.x;
    gLock.y = live.y;
    gLock.lastHp = live.hpPct;
    gLock.lastHitted = live.lastHitted;
    if (live.absSrc != ports::mob::AbsHpSrc::None) {
        // 热路径不 FindAll：缓存或 % 估计；UI 刷新交给 MaybeProbeUiHpTag。
        if (live.absSrc != ports::mob::AbsHpSrc::PctEstimate ||
            gLock.absSrc == ports::mob::AbsHpSrc::None ||
            gLock.absSrc == ports::mob::AbsHpSrc::PctEstimate) {
            gLock.maxHp = live.absMaxHp;
            gLock.absHp = live.absHp;
            gLock.absSrc = live.absSrc;
        }
    }
    (void)snap;  // 选怪仍用缓存；锁存续以 live 为准

    NoteLockHpSample();
    NoteHittedForHitLag(now);
    MaybeProbeUiHpTag(now);
    if (TryAbandonDealtSum(now)) return false;
    if (TryAbandonOneshot(now)) return false;
    if (TryAbandonAbsHp(now)) return false;
    if (TryAbandonHitLag(now)) return false;

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
    const bool playerZmOk =
        kDirectTeleportNoLayerHop ? false : TryPlayerZMass(px, py, &playerZm);

    auto better = [&](int clusterN, float score) -> bool {
        if (clusterOn) {
            if (clusterN > bestCluster) return true;
            if (clusterN < bestCluster) return false;
        }
        return score < bestScore;
    };

    // 直贴：不分层、不跑 hop 分段可达性；有落点即可，按距离选。
    if (kDirectTeleportNoLayerHop) {
        for (int i = 0; i < snap.count; ++i) {
            const auto& m = snap.mobs[i];
            if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
            if (m.templateId == kSpecialTplFilter) continue;
            if (IsSoftBanned(m.id, now)) continue;
            float hop = 0, tx = 0, ty = 0;
            uint32_t fh = 0;
            const bool landOk =
                looseLand ? EstimateLandPrefer(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh)
                          : EstimateLand(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh);
            if (!landOk || !std::isfinite(hop) || hop < 0.f) continue;
            const float dx = m.x - px;
            const float dy = m.y - py;
            const float woundBias = (m.hpPct > 0 && m.hpPct < 100) ? -80000.f : 0.f;
            const float score = dx * dx + dy * dy + woundBias;
            const int cn = clusterOn ? CountClusterNeighbors(snap, m) : 0;
            if (!better(cn, score)) continue;
            bestCluster = cn;
            bestScore = score;
            best = &m;
            bestHop = hop;
        }
        if (!best) return false;
    } else {
        auto consider = [&](const ports::mob::MobLite& m, bool sameLayerPass) {
            if (!m.ready || m.deadType != 0 || m.hpPct <= 0) return;
            if (m.templateId == kSpecialTplFilter) return;
            if (IsSoftBanned(m.id, now)) return;

            if (!allowCrossLayer) {
                if (!SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, 0)) return;
                if (!sameLayerPass) return;
                const float dx = m.x - px;
                const float d2 = dx * dx + ((m.hpPct > 0 && m.hpPct < 100) ? -80000.f : 0.f);
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
            if (hop > MaxApproachHopPx() &&
                !CanApproachWithinMaxHop(px, py, tx, ty, MaxApproachHopPx())) {
                return;
            }
            const bool same = SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, fh);
            if (sameLayerPass) {
                if (!same) return;
            } else {
                if (same) return;
            }

            const float dx = m.x - px;
            const float dy = m.y - py;
            const float woundBias = (m.hpPct > 0 && m.hpPct < 100) ? -80000.f : 0.f;
            const float score = dx * dx + dy * dy + hop * hop * 0.35f + woundBias;
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
    }

    gLock.ptr = best->ptr;
    gLock.id = best->id;
    gLock.lastHp = best->hpPct;
    gLock.lastHitted = best->lastHitted;
    gLock.whiff = 0;
    ClearWhiffArm();
    gLock.x = best->x;
    gLock.y = best->y;
    gLock.templateId = best->templateId;
    {
        ports::mob::AbsHp abs{};
        if (ports::mob::ResolveAbsHp(best->id, best->templateId, best->hpPct, abs,
                                     /*refreshUi=*/false) &&
            abs.ok) {
            gLock.maxHp = abs.max;
            gLock.absHp = abs.cur;
            gLock.absSrc = abs.src;
        } else {
            gLock.maxHp = ports::mob::LookupTemplateMaxHp(best->templateId);
            gLock.absHp = AbsHpFromPct(gLock.maxHp, best->hpPct);
            gLock.absSrc = gLock.maxHp > 0 ? ports::mob::AbsHpSrc::PctEstimate
                                          : ports::mob::AbsHpSrc::None;
        }
    }
    gLock.lockStartHp = best->hpPct;
    gLock.prevHittedSample = best->lastHitted;
    gLock.hitBumpCount = 0;
    gLock.firstBumpMs = 0;
    gLock.lockFires = 0;
    gLock.predictSampleHp = best->hpPct;
    gLock.predictFiresAtHp = 0;
    gLock.predictDropEvents = 0;
    gLock.dealtSum = 0;
    gLock.dealtHits = 0;
    gLock.lastDealt = 0;
    gLandSide = (px >= best->x) ? 1 : -1;
    gStandstillSince = 0;
    gStandstillShuffleLast = 0;
    gStandstillAnchorX = px;
    gStandstillAnchorY = py;
    LogLine("acquire id=%d tpl=%d hp=%d%% maxHp=%lld abs=%lld src=%s pos=(%.0f,%.0f) "
            "d=(%.0f,%.0f) layer=%s hop~%.0f side=%d cluster=%d loose=%d",
            best->id, best->templateId, best->hpPct, static_cast<long long>(gLock.maxHp),
            static_cast<long long>(gLock.absHp), ports::mob::AbsHpSrcName(gLock.absSrc), best->x,
            best->y, best->x - px, best->y - py,
            SameLayer(px, py, best->x, best->y) ? "same" : "cross", bestHop, gLandSide,
            clusterOn ? bestCluster : -1, looseLand ? 1 : 0);
    return true;
}

void ExplainAcquireMiss(const ports::mob::Snapshot& snap, float px, float py, DWORD now,
                        bool allowCrossLayer) {
    const float standOff = ClampStandOff();
    int nBan = 0, nSpecial = 0, nDead = 0, nNoLand = 0, nHopFar = 0, nSame = 0, nCross = 0,
        nOk = 0;
    int nSnap = 0, nLayer = 0, nFhBan = 0, nSides = 0, nUnsafe = 0, nFar = 0;
    int nSample = 0;
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
        float hop = 0, tx = 0, ty = 0;
        uint32_t fh = 0;
        // 与真实选怪一致：PickNearestTarget 首轮失败后会用 looseLand 再选一次（见 Acquire）。
        LandFail fail = LandFail::kNone;
        if (!EstimateLandPrefer(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh, nullptr, &fail)) {
            ++nNoLand;
            switch (fail) {
                case LandFail::kSnap: ++nSnap; break;
                case LandFail::kLayer: ++nLayer; break;
                case LandFail::kFhBan: ++nFhBan; break;
                case LandFail::kSides: ++nSides; break;
                case LandFail::kUnsafe: ++nUnsafe; break;
                case LandFail::kFar: ++nFar; break;
                default: break;
            }
            // 前两只出样：报怪心与贴到的台，才能判「怪离台多远 / 贴到哪去了」。
            if (nSample < 2) {
                ++nSample;
                float sx = m.x, sy = m.y;
                uint32_t sfh = 0;
                const bool snapOk = ports::foothold_path::SnapStandAt(m.x, m.y, &sx, &sy, &sfh,
                                                                      /*preferFlat=*/true);
                LogLine("noLand sample tpl=%d mob=(%.0f,%.0f) snap=%d stand=(%.0f,%.0f) fh=%u "
                        "dY=%.0f dX=%.0f why=%d",
                        m.templateId, m.x, m.y, snapOk ? 1 : 0, sx, sy, (unsigned)sfh, sy - m.y,
                        sx - m.x, (int)fail);
                // 贴到远台时（dY 大）报一遍怪脚下的台普查，定位是哪道过滤把近台吃掉了。
                ports::foothold_path::StandCensus cen{};
                if (ports::foothold_path::CensusStandAt(m.x, m.y, &cen)) {
                    LogLine("noLand census mob=(%.0f,%.0f) nodes=%d inBand=%d usable=%d wall=%d "
                            "narrow=%d bestSpan=%d chain=[%d,%d]",
                            m.x, m.y, cen.nodes, cen.inBand, cen.usable, cen.wall, cen.narrow,
                            cen.bestSpan, cen.bestChainLo, cen.bestChainHi);
                }
            }
            continue;
        }
        if (!kDirectTeleportNoLayerHop && std::isfinite(hop) && hop > MaxApproachHopPx() &&
            !CanApproachWithinMaxHop(px, py, tx, ty, MaxApproachHopPx())) {
            ++nHopFar;
            continue;
        }
        const bool same = SameLayer(px, py, m.x, m.y);
        if (same) {
            ++nSame;
        } else {
            ++nCross;
            if (!allowCrossLayer && !kDirectTeleportNoLayerHop) continue;
        }
        ++nOk;
    }
    LogLine(
        "acquire miss count=%d py=%.0f mode=%s ban=%d noLand=%d(snap=%d layer=%d far=%d fhban=%d "
        "sides=%d unsafe=%d) hopFar=%d same=%d cross=%d dead=%d special=%d ok=%d softN=%d",
        snap.count, py,
        kDirectTeleportNoLayerHop ? "direct"
                                  : (allowCrossLayer ? "any-landable" : "same-layer"),
        nBan, nNoLand, nSnap, nLayer, nFar, nFhBan, nSides, nUnsafe, nHopFar, nSame, nCross, nDead,
        nSpecial, nOk, gSoftBanN);
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
    gSettleNeedPosSane = false;
    gSettleEnteredAt = 0;
    EnterState(State::Idle, now, why);
}

void LogSettleDiag(const char* phase, DWORD now, DWORD sinceEnter, float dLand) {
    ports::player_combat::VisualSnap s{};
    const bool ok = ports::player_combat::QueryVisualSnap(s) && s.ok;
    if (!ok) {
        LogLine("settle_diag phase=%s since=%ums d=%.0f snap=fail wantFh=%u cross=%d",
                phase ? phase : "?", sinceEnter, dLand, gSettleFh, gSettleWasCross ? 1 : 0);
        return;
    }
    LogLine("settle_diag phase=%s since=%ums d=%.0f land=(%.0f,%.0f) wantFh=%u "
            "ap=(%.0f,%.0f) apl=(%.0f,%.0f) pos=(%.0f,%.0f) curFh=%u rp=%.1f rpV=%.1f "
            "dAA=%.1f dApPos=%.0f cross=%d",
            phase ? phase : "?", sinceEnter, dLand, gSettleX, gSettleY, gSettleFh, s.apX, s.apY,
            s.aplX, s.aplY, s.posX, s.posY, (unsigned)s.curFh, s.rpPos, s.rpV, s.dApApl, s.dApPos,
            gSettleWasCross ? 1 : 0);
    (void)now;
}

// 落点失手时把「该 x 这一列的台」摊开报一遍。d5197e 里 6 次失手全是 x 不变、Y 被抬 32~73px
// （engineY 更小 = 屏幕更高）且 curFh=0、dAA≈10（人还在上升），怀疑同一 x 上方另有一条台被
// 引擎选中，而怪站在下面那条。engineY 能否对上列表里的某条，就是这个猜测的判据。
void LogLandColumn(const char* why, float landX, float landY, float engineY) {
    ports::foothold_path::ColumnHit hits[8]{};
    int total = 0;
    const int n = ports::foothold_path::ProbeColumn(landX, landY, 200, hits, 8, &total);
    if (n < 0) return;
    char col[288];
    col[0] = '\0';
    int off = 0;
    for (int i = 0; i < n && off < static_cast<int>(sizeof(col)) - 56; ++i) {
        const int w = snprintf(col + off, sizeof(col) - off, "%s%u@%d(sp%d sl%d%s%s)",
                               i ? " " : "", (unsigned)hits[i].fh, hits[i].y, hits[i].span,
                               hits[i].slope, hits[i].wall ? " wall" : "",
                               hits[i].narrow ? " narrow" : "");
        if (w <= 0) break;
        off += w;
    }
    LogLine("land_column why=%s land=(%.0f,%.0f) engineY=%.0f dY=%.0f n=%d/%d col=[%s]",
            why ? why : "?", landX, landY, engineY, engineY - landY, n, total, col);
}

void MaybeSettleDiagTick(DWORD now, DWORD sinceEnter, float dLand) {
    // 跨层必采；同层仅在已明显漂离落点时采（控日志量）。
    const bool need = gSettleWasCross || dLand > 24.f;
    if (!need) return;
    if (gSettleDiagLastMs && now - gSettleDiagLastMs < kSettleDiagPeriodMs) return;
    gSettleDiagLastMs = now;
    LogSettleDiag("tick", now, sinceEnter, dLand);
}

void BeginMapArmGraceMs(DWORD now, const char* why, DWORD graceMs) {
    if (graceMs < 50) graceMs = 50;
    if (graceMs > kMapArmGraceMs) graceMs = kMapArmGraceMs;
    const bool already =
        gMapArmUntilMs && static_cast<int>(now - gMapArmUntilMs) < 0;
    ClearLock();
    ClearSoftBan();
    // land_miss 毒台 ban 跨过 arm 窗口；仅真换图才清（FH id 按图编号）。
    if (why && (!std::strcmp(why, "map_change") || !std::strcmp(why, "ResetForMapChange"))) {
        ClearLandFhBan();
    }
    ports::mob::ClearAbsHpCache();
    ports::attack::ForceRelease();
    gSettleUntil = 0;
    gSettleNeedPosSane = false;
    gSettleEnteredAt = 0;
    gSettleWasCross = false;
    gCrossLayerFillGateUntil = 0;
    // 位移预算账本不清（e2bd95：land_miss 走此处清账 = 预算失效）；记录自然过期。
    ports::map_bounds::InvalidateFhAabbCache();
    gState = State::Idle;
    gMapArmUntilMs = now + graceMs;
    if (already) {
        static DWORD sRef = 0;
        if (!sRef || now - sRef > 1000) {
            sRef = now;
            LogLine("map arm grace refresh %ums why=%s", (unsigned)graceMs, why ? why : "?");
        }
        return;
    }
    LogLine("map arm grace %ums why=%s", (unsigned)graceMs, why ? why : "?");
}

void BeginMapArmGrace(DWORD now, const char* why) {
    BeginMapArmGraceMs(now, why, kMapArmGraceMs);
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
        // 硬闸（channel_hop / encounter / auto_lie / supply）仍走完整 Idle。
        if (gHardPauseMask.load(std::memory_order_acquire) != 0) {
            if (gState != State::Idle) GoIdle(now, "pause");
        } else {
            // 停刀期间仍清观察窗，避免墙钟到期在 return 前未 Resolve、恢复首拍误 whiff。
            SoftResetWhiffForLightPause("pause_hold");
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
    // 引擎 Prepare/警戒态（自动 BUFF 或手搓）：禁新贴怪瞬移与出刀，避免拆视觉层。
    // 不 GoIdle，保留 lock；已在 Settling 的收态窗让官方做完。
    {
        int prepSkill = 0;
        if (ports::skill::IsPreparingSkill(&prepSkill)) {
            SoftResetWhiffForLightPause("skill_prepare");
            static DWORD sPrepLog = 0;
            if (!sPrepLog || now - sPrepLog > 800) {
                sPrepLog = now;
                LogLine("skill_prepare hold skill=%d state=%s (block tp/fire)", prepSkill,
                        StateName(gState));
            }
            if (gState == State::MoveTo) {
                EnterState(State::Aim, now, "skill_prepare_yield");
            } else if (gState == State::Firing) {
                EnterState(State::Recover, now, "skill_prepare_yield");
            }
            return;
        }
    }
    // 外部 Hold 刚放：取消半截 MoveTo/Settling，回 Acquire 重读坐标，避免 Hold 期间过期落点远跳。
    if (gResumeRelockPending) {
        gResumeRelockPending = false;
        if (gState == State::MoveTo || gState == State::Settling) {
            gSettleUntil = 0;
            EnterState(State::Acquire, now, "ext_pause_resume");
        }
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

    // 自动召唤开着且场上无宠：先让路召唤（警戒态会拒 ActivatePet →「现在无法进行」）。
    if (x::features::pet_feed::ShouldHoldCombatForSummon()) {
        static DWORD sPetHold = 0;
        if (!sPetHold || now - sPetHold > 1000) {
            sPetHold = now;
            LogLine("combat hold: wait pet summon (no tp/fire)");
        }
        if (gState != State::Idle) GoIdle(now, "wait_pet");
        ports::attack::ForceRelease();
        return;
    }

    ports::player_combat::CombatCtx player{};
    if (!ports::player_combat::QueryCombatCtx(player) || !player.ok) {
        // Doing 后 PosSane 门：坐标坏时仍要跑 Settling 超时/武装，不能直接 return 丢状态。
        if (gState == State::Settling && gSettleNeedPosSane) {
            ports::player_combat::PosDiag diag{};
            ports::player_combat::PeekLocalPosDiag(diag);
            if (static_cast<int>(now - gSettleUntil) >= 0) {
                LogLine("Settling abort still_bad why=%s raw=(%.0f,%.0f) land=(%.0f,%.0f) — arm",
                        diag.why ? diag.why : "?", diag.x, diag.y, gSettleX, gSettleY);
                gSettleUntil = 0;
                gSettleNeedPosSane = false;
                gSettleEnteredAt = 0;
                BeginMapArmGraceMs(now, "post_doing_bad_pos", kBadPosArmMs);
            } else {
                static DWORD sWaitBad = 0;
                if (!sWaitBad || now - sWaitBad > 500) {
                    sWaitBad = now;
                    LogLine("Settling wait(bad) remain=%ums why=%s raw=(%.0f,%.0f) land=(%.0f,%.0f)",
                            gSettleUntil - now, diag.why ? diag.why : "?", diag.x, diag.y,
                            gSettleX, gSettleY);
                }
            }
            return;
        }
        {
            ports::player_combat::PosDiag diag{};
            ports::player_combat::PeekLocalPosDiag(diag);
            static DWORD sBad = 0;
            if (!sBad || now - sBad > 2000) {
                sBad = now;
                // 画面「人没了」时这里必现：坐标 NaN/≈0/超界，精灵被摔出图外。
                LogLine("tick skip reason=bad_player_pos why=%s lu=%d alive=%d raw=(%.0f,%.0f) "
                        "state=%s",
                        diag.why ? diag.why : "?", diag.hasLocalUser ? 1 : 0, diag.luAlive ? 1 : 0,
                        diag.x, diag.y, StateName(gState));
            }
            // near_zero/NaN 往往是 Field 软重载前兆（本 BIN：Ap→(0,0) 后 ~1s MyUser drift）。
            // 必须立刻完整武装停刀停跳，禁止只软延 +800 却不停 ForceRelease。
            BeginMapArmGraceMs(now, "bad_player_pos", kBadPosArmMs);
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
            // 贴怪开：可跨层（Pick 优先同层 + hop 可达）；关：仅同层站桩。
            const bool allowCross = tpOn;
            if (!RefreshLock(snap)) {
                static DWORD sMiss = 0;
                bool got = PickNearestTarget(snap, player.x, player.y, now, allowCross,
                                            /*looseLand=*/false);
                if (!got) {
                    // 先松落点再选；禁止立刻 ClearSoftBan——会把刚 dead 的鬼锁清掉同 tick 空转。
                    got = PickNearestTarget(snap, player.x, player.y, now, allowCross,
                                           /*looseLand=*/true);
                }
                if (!got && gSoftBanN > 0) {
                    // 只清短禁；保留 whiff + 不可达，避免 BIN 清光后立刻重锁空转。
                    const int before = gSoftBanN;
                    DropSoftBanNonSticky();
                    if (before != gSoftBanN) {
                        LogLine("softBan keep_sticky dropped=%d keep=%d", before - gSoftBanN,
                                gSoftBanN);
                    }
                    got = PickNearestTarget(snap, player.x, player.y, now, allowCross,
                                           /*looseLand=*/true);
                }
                if (!got) {
                    if (!sMiss || now - sMiss > 1000) {
                        sMiss = now;
                        ExplainAcquireMiss(snap, player.x, player.y, now, allowCross);
                    }
                    break;
                }
            }
            // 贴怪关时不追跨层（站桩）；贴怪开时由 MoveTo 分段 + cross_layer_fill_gate 控节奏。
            if (!tpOn && !SameLayer(player.x, player.y, gLock.x, gLock.y)) {
                SoftBanFor(gLock.id, now, kCrossLayerForbidSoftBanMs);
                LogLine("acquire forbid cross_layer id=%d ban=%ums (tp_off)", gLock.id,
                        (unsigned)kCrossLayerForbidSoftBanMs);
                ClearLock();
                break;
            }
            if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                EnterState(State::Aim, now, "in_band");
                break;
            }
            if (tpOn) {
                // 贴怪开：出命中带即贴；CD 未好不进 MoveTo。
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
            // 预算预检（切段前）：按「切段后这一跳」估算额度，不够就**留在 MoveTo 等回额**。
            // 1d3308：旧版在此弃锁+软禁换怪 6122 次 → 半路折返、两点间徘徊、只有 0.5 刀/跳。
            // 预检放在切段前，省掉每 tick 白算白打一条 chunk 日志。
            {
                const float capHop = MaxApproachHopPx();
                const float estHop = (hop > capHop) ? capHop : hop;
                float used = 0.f;
                const DWORD wait = FillDistBudgetWaitMs(now, estHop, &used);
                if (wait > 0) {
                    static DWORD sBudget = 0;
                    if (!sBudget || now - sBudget > 1000) {
                        sBudget = now;
                        LogLine("MoveTo hold id=%d dist_budget used=%.0f+%.0f>%.0f remain=%ums "
                                "(keep lock)",
                                gLock.id, used, estHop, FillBudgetPx(), (unsigned)wait);
                    }
                    RenewLootPulseHold(now);
                    break;  // 保持锁定与朝向，等额度回来接着追同一只
                }
            }
            // 326d34：无论是否 direct_tp，单次 fill 都必须受 maxHop 切段（BIN hop=3517→205）。
            bool didChunk = false;
            {
                const float maxHop = MaxApproachHopPx();
                if (hop > maxHop) {
                    const float fullHop = hop;
                    float step = 0.f, sx = 0.f, sy = 0.f;
                    uint32_t sfh = 0;
                    if (!TryChunkApproachLand(player.x, player.y, tx, ty, maxHop, &step, &sx, &sy,
                                              &sfh)) {
                        SoftBanFor(gLock.id, now, kHopChunkFailSoftBanMs, kBanUnreachable);
                        LogLine("MoveTo chunk fail id=%d wantHop=%.0f maxHop=%.0f softBan=%ums "
                                "(snap overshoot/no progress)",
                                gLock.id, fullHop, maxHop, (unsigned)kHopChunkFailSoftBanMs);
                        ClearLock();
                        EnterState(State::Acquire, now, "hop_chunk_fail");
                        continue;
                    }
                    LogLine(
                        "MoveTo chunk id=%d fullHop=%.0f → step=%.0f to=(%.0f,%.0f) fh=%u maxHop=%.0f",
                        gLock.id, fullHop, step, sx, sy, sfh, maxHop);
                    hop = step;
                    tx = sx;
                    ty = sy;
                    fh = sfh;
                    didChunk = true;
                }
                if (hop > maxHop) {
                    SoftBanFor(gLock.id, now, kHopChunkFailSoftBanMs, kBanUnreachable);
                    LogLine("MoveTo reject id=%d hop=%.0f > maxHop=%.0f softBan=%ums (hard cap)",
                            gLock.id, hop, maxHop, (unsigned)kHopChunkFailSoftBanMs);
                    ClearLock();
                    EnterState(State::Acquire, now, "hop_hard_cap");
                    continue;
                }
            }
            if (landSide != 0) gLandSide = landSide;

            // 切段后精确复核（估算偏乐观时兜底）：同样只等额度，不弃锁。
            {
                const DWORD wait = FillDistBudgetWaitMs(now, hop, nullptr);
                if (wait > 0) {
                    RenewLootPulseHold(now);
                    break;
                }
            }
            const bool crossHop = !SameLayer(player.x, player.y, tx, ty);

            const bool same = SameLayer(player.x, player.y, gLock.x, gLock.y);
            const bool hug = IsHugMove(same, hop);
            const float minHop = hug ? kMinHugHop : kMinReapproachHop;
            // 黏住无脑A：hop 小且人还近 → 直接砍；禁止翻侧 Aim↔MoveTo 空转。
            if (hop < minHop) {
                if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff) ||
                    InMeleeHoldBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                    LogLine("MoveTo melee_hold id=%d hop=%.1f dx=%.0f → fire", gLock.id, hop,
                            gLock.x - player.x);
                    ClearStickySpin();
                    EnterState(State::Firing, now, "melee_hold");
                    continue;
                }
                bool cont = false;
                if (HandleTinyHopSticky(now, player.x, player.y, standOff, &cont)) {
                    if (cont) continue;
                    RenewLootPulseHold(now);
                    break;
                }
            }
            // 瞬移远近同价：Native 自冷只认面板 CD，不再按 hop 抬地板。
            const DWORD panelCd = gTeleportCooldownMs.load(std::memory_order_acquire);
            const DWORD cd = panelCd;
            int prepSkill = 0;
            if (ports::skill::IsPreparingSkill(&prepSkill)) {
                static DWORD sPrepTp = 0;
                if (!sPrepTp || now - sPrepTp > 800) {
                    sPrepTp = now;
                    LogLine("MoveTo defer why=skill_prepare skill=%d hop=%.0f", prepSkill, hop);
                }
                RenewLootPulseHold(now);
                break;
            }
            if (!LandSafeForFill(tx, ty, fh)) {
                SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                LogLine("MoveTo reject id=%d land_unsafe to=(%.0f,%.0f) fh=%u — softBan", gLock.id,
                        tx, ty, fh);
                ClearLock();
                EnterState(State::Acquire, now, "land_unsafe");
                continue;
            }
            ports::teleport::SetNativeCooldownMs(cd);
            if (!ports::teleport::TeleportNativeSkillCall(tx, ty, fh)) {
                static DWORD sFail = 0;
                if (!sFail || now - sFail > 1500) {
                    sFail = now;
                    LogLine("MoveTo teleport fail id=%d want=(%.0f,%.0f) hop=%.0f cd=%ums hug=%d",
                            gLock.id, tx, ty, hop, cd, hug ? 1 : 0);
                }
                RenewLootPulseHold(now);
                break;
            }
            ClearStickySpin();
            NoteFillDist(now, hop);  // 每次 fill 都记账（切段中间跳就是爬层本身）
            const DWORD minSettle = same ? kPostDoingMinSettleMs : kPostDoingCrossLayerMinSettleMs;
            const DWORD posGate = same ? kPostDoingPosSaneMaxMs : kPostDoingCrossPosSaneMaxMs;
            LogLine(
                "MoveTo fill+Doing id=%d to=(%.0f,%.0f) from=(%.0f,%.0f) hop=%.0f fh=%u cd=%ums "
                "panelCd=%ums side=%d hug=%d settle=%ums posGate=%ums minSettle=%ums cross=%d",
                gLock.id, tx, ty, player.x, player.y, hop, fh, cd, panelCd, gLandSide, hug ? 1 : 0,
                SettleMsForHop(hop, hug), (unsigned)posGate, (unsigned)minSettle, same ? 0 : 1);
            gSettleX = tx;
            gSettleY = ty;
            gSettleFh = fh;
            gStandstillSince = now;
            gStandstillAnchorX = tx;
            gStandstillAnchorY = ty;
            // settle 时间窗可为 0；仍强制 MinSettle + PosSane（跨层加长观察窗）。
            const DWORD settle = SettleMsForHop(hop, hug);
            DWORD gate = settle;
            if (gate < minSettle) gate = minSettle;
            if (gate < posGate) gate = posGate;
            gSettleNeedPosSane = true;
            gSettleEnteredAt = now;
            gSettleMinMs = minSettle;
            gSettleWasCross = !same;
            gSettleDiagLastMs = 0;
            gSettleDidStabilize = false;
            gSettleSawRpBad = false;
            gSettleUntil = now + gate;
            // 跨层短门：防连跳打崩 Ap。切段中间跳不武装（否则追一只怪每段干等，ebc679）；
            // 持续爬塔由 cross_budget 滚动预算兜底。面板「跨层门控」可调；0=关。
            if (!kDirectTeleportNoLayerHop && !didChunk && crossHop) {
                const DWORD gateMs = CrossLayerFillGateMs();
                if (gateMs > 0) {
                    gCrossLayerFillGateUntil = now + gateMs;
                    LogLine("cross_layer_fill_gate arm %ums until=%u", (unsigned)gateMs,
                            gCrossLayerFillGateUntil);
                }
            }
            // 入 Settling 立刻采一帧（Doing 刚结束的基准）。
            LogSettleDiag("enter", now, 0, 0.f);
            EnterState(State::Settling, now, hug ? "tp_ok_hug" : "tp_ok");
            break;  // 禁止同 tick Settling→Aim→Fire（Doing 刚写完的 Ap 不可信）
        }

        case State::Settling: {
            ports::player_combat::CombatCtx landCtx{};
            const bool posOk = ports::player_combat::QueryCombatCtx(landCtx) && landCtx.ok;
            float dLand = 1e9f;
            if (posOk) {
                const float dx = landCtx.x - gSettleX;
                const float dy = landCtx.y - gSettleY;
                dLand = std::sqrt(dx * dx + dy * dy);
            }
            const DWORD sinceEnter =
                gSettleEnteredAt ? (now - gSettleEnteredAt) : gSettleMinMs;
            const bool minWaitOk = sinceEnter >= gSettleMinMs;
            const float landEps =
                gSettleWasCross ? kPostDoingCrossLandEpsPx : kPostDoingLandEpsPx;

            ports::player_combat::VisualSnap vs{};
            const bool vsOk = ports::player_combat::QueryVisualSnap(vs) && vs.ok;
            const bool rpBad =
                vsOk && (!std::isfinite(vs.rpV) || std::fabs(vs.rpV) > kSettleRpVAbsMax);
            if (rpBad) gSettleSawRpBad = true;
            // 走引擎原生落点后 curFh 由 CollisionDetect 自选：挂到邻台是**合法结果**，
            // 不再入毒（旧判据属「我方补种 RelPos」时代，那个根因已撤）。仅留作日志。
            const bool fhDrift =
                gSettleFh != 0 && vsOk && vs.curFh != 0 && vs.curFh != gSettleFh;
            // 仍贴近落点时的同点滑台（旧判定，用于 landed 路径日志）。
            const bool fhSkate = fhDrift && dLand <= landEps;
            // 尚未挂台：Doing 清空 CurFh 属正常（下落中也如此），久等不挂才是岛台失粘。
            const bool fhUnattached = vsOk && vs.curFh == 0;

            // 悬空（curFh=0）不算落地。原生路径下挂台是 CollisionDetect 的职责，久等不挂就说明
            // 这个落点站不住：f668e1 实测 45 次/10s 重复 fill 到同一点，每次 d≈4 却始终 curFh=0，
            // 而 SameLayer 在 curFh=0 时退回按悬空坐标取 zMass → Aim 又判 cross_layer → 自激
            // 振荡两分钟后静默踢线。放行前必须确认引擎已接手；读不到视觉快照时不卡（维持旧behavior）。
            const bool attachOk = gSettleFh == 0 || !fhUnattached;
            bool landed =
                posOk && minWaitOk && attachOk && (dLand <= landEps || !gSettleNeedPosSane);
            const bool timedOut = static_cast<int>(now - gSettleUntil) >= 0;

            MaybeSettleDiagTick(now, sinceEnter, posOk ? dLand : 1e9f);

            // 326d34：Doing「成功」但 enter 时 Ap 已在错台（d=67 wantFh=212 curFh=834）。
            // 5acf32：跨层 landEps=16，d=21∧邻台落在「>24 才 miss」死区 → 干等满 posGate→205。
            // 阈值用本跳 landEps：同层仍宽（96），跨层邻台 enter 即早退。
            // 原生落点下 curFh==0 是 Doing 后的**正常**状态（引擎显式清台），不再入判据；
            // 只在「引擎压根没把 Ap 写到我们给的点」时才算 Doing 失手。
            const bool doingMiss =
                posOk && gSettleFh != 0 && sinceEnter <= 48 && dLand > landEps;
            if (doingMiss) {
                const int missId = gLock.id;
                const uint32_t missFh = gSettleFh;
                LogSettleDiag("land_miss", now, sinceEnter, dLand);
                LogLine("Settling abort land_miss why=doing_miss d=%.0f land=(%.0f,%.0f) "
                        "pos=(%.0f,%.0f) wantFh=%u curFh=%u since=%ums — arm",
                        dLand, gSettleX, gSettleY, landCtx.x, landCtx.y, gSettleFh,
                        vsOk ? (unsigned)vs.curFh : 0u, sinceEnter);
                LogLandColumn("doing_miss", gSettleX, gSettleY, landCtx.y);
                gSettleUntil = 0;
                gSettleNeedPosSane = false;
                gSettleEnteredAt = 0;
                BeginMapArmGraceMs(now, "post_doing_land_miss", kSkateToxicArmMs);
                // 点级回避接手「换怪再撞同一落点」，故封台回到满 2 撞才动（少造 no_land）。
                ApplyLandMissStickyPenalty(missId, missFh, gSettleX, gSettleY, now,
                                          /*wantFhBan=*/true, "doing_miss", kSkateToxicSoftBanMs,
                                          /*fhBanOnFirst=*/false);
                break;
            }

            // 曾想在这里做「岛台失粘救援」（久未挂台就补种 CurFh+RelPos），已否决：
            // 那个补种正是 land_miss 的老根因，等于把拽人动作请回来。改用引擎当裁判——
            // 引擎挂不上台就说明这个落点站不住，走下方 timeout_no_attach 记坏点、换点重贴。
            // e27c33 那类岛台失粘在本策略下退化为「放弃该落点」，不再悬空硬撑。

            // bbda00：同点邻台交接（fhDrift∧d=0∧rpV 有限）是正常 Walk。
            // d8d80e：同点 rpV=nan 也常见——先清锁存观察，勿立刻 detach+miss（拖换怪）。
            // 真毒：已偏出台阶可落地圈（d>landEps）且仍毒/drift——勿用固定 24，跨层会死区。
            // d1a58e：latch 清 nan 后仍 fhDrift 就 settle_ok → Aim 期沿 Walk 链滑出图。
            // 本跳曾见 rpBad：同点仍 drift 则继续等；超时按 skate_toxic 拆台+miss。
            const bool atLand = dLand <= kPostStabilizeLandEpsPx;
            const bool junctionPoison = rpBad && atLand;
            const bool postNanJunctionHang =
                gSettleSawRpBad && fhDrift && atLand && gSettleDidStabilize;
            // 位移超出容差才算滑走。未挂台且仍在宽限内=正在下落，属合法过程，不判。
            const bool drifted =
                dLand > landEps && (!fhUnattached || sinceEnter >= kNativeAttachWaitMs);
            const bool skateToxic =
                (rpBad && !atLand) || drifted || (postNanJunctionHang && timedOut);

            if (posOk && junctionPoison && !skateToxic) {
                if (!gSettleDidStabilize) {
                    gSettleDidStabilize = true;
                    struct LatchJob {
                        bool ok = false;
                    } job{};
                    auto latchFn = [](void* p) {
                        auto* j = static_cast<LatchJob*>(p);
                        if (!j) return;
                        j->ok = ports::teleport::ClearMotionLatchMainThread();
                    };
                    const bool pumped =
                        x::runtime::main_thread::Ensure() &&
                        x::runtime::main_thread::InvokeAndWait(
                            latchFn, &job, 80, x::runtime::main_thread::JobPrio::High);
                    LogSettleDiag("latch", now, sinceEnter, dLand);
                    LogLine("Settling latch_only ok=%d pumped=%d wantFh=%u curFh=%u rpV=%.1f "
                            "d=%.0f",
                            job.ok ? 1 : 0, pumped ? 1 : 0, gSettleFh,
                            vsOk ? (unsigned)vs.curFh : 0u, vsOk ? vs.rpV : 0.0, dLand);
                }
                // 仍毒则本拍不放行，等下一 tick；干净后走正常 landed。
                if (rpBad) {
                    if (!timedOut && !minWaitOk) break;
                    if (!timedOut) break;
                    // 超时仍 nan：落入下方 skate/land_miss 前，按短 miss 处理
                } else {
                    // fall through to landed checks
                }
            }

            // 曾 nan 且同点仍挂邻台：未超时则继续等 wantFh 重挂，禁止 settle_ok。
            if (posOk && postNanJunctionHang && !timedOut) {
                static DWORD sHang = 0;
                if (!sHang || now - sHang > 2000) {
                    sHang = now;
                    LogLine("Settling wait_post_nan wantFh=%u curFh=%u d=%.0f since=%ums",
                            gSettleFh, vsOk ? (unsigned)vs.curFh : 0u, dLand, sinceEnter);
                }
                break;
            }

            // 毒化滑走：拆 CurFh + 短 arm；仅真滑走才 fhBan。
            if (posOk && (skateToxic || (junctionPoison && timedOut && rpBad))) {
                if (!gSettleDidStabilize) {
                    gSettleDidStabilize = true;
                    struct StabJob {
                        float x = 0.f;
                        float y = 0.f;
                        uint32_t fh = 0;
                        bool ok = false;
                    } job{gSettleX, gSettleY, gSettleFh, false};
                    auto stabFn = [](void* p) {
                        auto* j = static_cast<StabJob*>(p);
                        if (!j) return;
                        j->ok = ports::teleport::StabilizeFootholdMainThread(j->x, j->y, j->fh,
                                                                            /*replant=*/false);
                    };
                    const bool pumped =
                        x::runtime::main_thread::Ensure() &&
                        x::runtime::main_thread::InvokeAndWait(
                            stabFn, &job, 80, x::runtime::main_thread::JobPrio::High);
                    LogSettleDiag("stabilize", now, sinceEnter, dLand);
                    LogLine("Settling stabilize ok=%d pumped=%d replant=0 detach=1 wantFh=%u "
                            "curFh=%u rpV=%.1f skate=%d drift=%d d=%.0f",
                            job.ok ? 1 : 0, pumped ? 1 : 0, gSettleFh,
                            vsOk ? (unsigned)vs.curFh : 0u, vsOk ? vs.rpV : 0.0, fhSkate ? 1 : 0,
                            fhDrift ? 1 : 0, dLand);
                }
                const int missId = gLock.id;
                const uint32_t missFh = gSettleFh;
                const bool walkedOff = dLand > landEps;
                LogSettleDiag("land_miss", now, sinceEnter, dLand);
                LogLine("Settling abort land_miss why=skate_toxic d=%.0f land=(%.0f,%.0f) "
                        "pos=(%.0f,%.0f) since=%ums — arm",
                        dLand, gSettleX, gSettleY, landCtx.x, landCtx.y, sinceEnter);
                LogLandColumn("skate_toxic", gSettleX, gSettleY, landCtx.y);
                gSettleUntil = 0;
                gSettleNeedPosSane = false;
                gSettleEnteredAt = 0;
                BeginMapArmGraceMs(now, "post_doing_land_miss", kSkateToxicArmMs);
                // 偏台滑走：点级回避已堵住「换 id 再撞同一落点」，封台留给同台二次撞。
                ApplyLandMissStickyPenalty(missId, missFh, gSettleX, gSettleY, now, walkedOff,
                                          "skate_toxic", kSkateToxicSoftBanMs,
                                          /*fhBanOnFirst=*/false);
                break;
            }

            if (!landed && !timedOut) {
                static DWORD sWait = 0;
                if (!sWait || now - sWait > 2000) {
                    sWait = now;
                    ports::player_combat::PosDiag diag{};
                    ports::player_combat::PeekLocalPosDiag(diag);
                    LogLine("Settling wait remain=%ums land=(%.0f,%.0f) posOk=%d d=%.0f "
                            "minWait=%d since=%ums why=%s raw=(%.0f,%.0f)",
                            gSettleUntil - now, gSettleX, gSettleY, posOk ? 1 : 0, dLand,
                            minWaitOk ? 1 : 0, sinceEnter, diag.why ? diag.why : "?", diag.x,
                            diag.y);
                }
                break;  // 禁止 Fire / 再 TP
            }

            if (!posOk) {
                ports::player_combat::PosDiag diag{};
                ports::player_combat::PeekLocalPosDiag(diag);
                LogSettleDiag("bad_pos", now, sinceEnter, dLand);
                LogLine("Settling abort still_bad why=%s raw=(%.0f,%.0f) land=(%.0f,%.0f) — arm",
                        diag.why ? diag.why : "?", diag.x, diag.y, gSettleX, gSettleY);
                gSettleUntil = 0;
                gSettleNeedPosSane = false;
                gSettleEnteredAt = 0;
                BeginMapArmGraceMs(now, "post_doing_bad_pos", kBadPosArmMs);
                break;
            }

            // timedOut 但未满 MinSettle：仍等（防时钟回绕/同 tick）。
            if (!minWaitOk) {
                break;
            }

            // BIN 0.1.47：超时仍 d=805 却 Settling done→Aim，40ms 后 Ap→(0,0)。
            // 未贴近落点一律武装，禁止放行。
            // 5acf32：邻台超时（fhDrift∧d>landEps）首击 fhBan + 短 arm，勿再干等满窗后轻罚。
            if (!landed) {
                const int missId = gLock.id;
                const uint32_t missFh = gSettleFh;
                const bool neighborTimeout = fhDrift && dLand > landEps;
                // 救援后仍无台：这个落点引擎不认，记入坏点表让下次换点（真信号，非我方算错）。
                const bool noAttach = fhUnattached;
                const char* whyTimeout = noAttach            ? "timeout_no_attach"
                                         : neighborTimeout   ? "timeout_neighbor"
                                                             : "timeout";
                LogSettleDiag("land_miss", now, sinceEnter, dLand);
                LogLine("Settling abort land_miss why=%s d=%.0f land=(%.0f,%.0f) "
                        "pos=(%.0f,%.0f) wantFh=%u curFh=%u since=%ums — arm",
                        whyTimeout, dLand, gSettleX, gSettleY, landCtx.x, landCtx.y, gSettleFh,
                        vsOk ? (unsigned)vs.curFh : 0u, sinceEnter);
                LogLandColumn(whyTimeout, gSettleX, gSettleY, landCtx.y);
                gSettleUntil = 0;
                gSettleNeedPosSane = false;
                gSettleEnteredAt = 0;
                BeginMapArmGraceMs(now, "post_doing_land_miss",
                                   neighborTimeout ? kSkateToxicArmMs : kLandMissArmMs);
                ApplyLandMissStickyPenalty(
                    missId, missFh, gSettleX, gSettleY, now,
                    // 引擎压根不认这个落点时封台无意义（台是好的、这个 x 站不住），只封点。
                    /*wantFhBan=*/!noAttach, whyTimeout,
                    // no_attach 必须走升级阶梯（800→2000→4000ms）而非固定短禁：f668e1 里
                    // 该点属浮空怪 loose 落点，怪一飘坐标就变、点级回避追不上，只有按「这只怪
                    // 落不下去」逐次加重才收敛。传 override 会跳过 strikes 计数，故此处传 0。
                    neighborTimeout ? kSkateToxicSoftBanMs : 0,
                    // 挂到邻台已不算我方过错（引擎自选），故不再首击封台；留给点级回避。
                    /*fhBanOnFirst=*/false);
                break;
            }

            LogSettleDiag("done", now, sinceEnter, dLand);
            LogLine("Settling done land=(%.0f,%.0f) fh=%u d=%.0f early=%d since=%ums", gSettleX,
                    gSettleY, gSettleFh, dLand, timedOut ? 0 : 1, sinceEnter);
            gSettleUntil = 0;
            gSettleNeedPosSane = false;
            gSettleEnteredAt = 0;
            // 收态全部交给引擎（Ap/Apl/速度/挂台都由原生落点体与 CollisionDetect 完成）。
            // 禁止再 SyncRel/Impact，也禁止在此补写位置。
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, "lost_after_settle");
                break;
            }
            EnterState(State::Aim, now, "settle_ok");
            break;  // 下一 worker tick 再 Aim/Fire，给主线程一帧消化 Doing
        }

        case State::Aim: {
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                break;
            }
            // 无脑A：近距续砍；只有真远/跨层才重贴。禁止 aim_sticky 翻侧空转。
            // 残血纠偏：离开命中带即强制重贴（勿被 kReapproachMinDx 吞掉）。
            {
                bool moved = false;
                if (TryConsumeWhiffApproachCorrect(now, player.x, player.y, standOff, tpOn,
                                                   &moved)) {
                    if (moved) continue;
                    break;
                }
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
                // 近距纵漂：当 melee 续砍，勿 cross 狂贴。
                if (NearMeleeFloor(player.x, player.y, gLock.x, gLock.y)) {
                    // fall through to fire gate below
                } else if (tpOn) {
                    // 跨层重贴走 MoveTo（maxHop 切段 + fill_gate）；勿 SoftBan 清锁。
                    if (TryEnterMoveTo(now, "cross_layer")) break;
                    break;  // CD / gate：留 Aim 等贴
                } else {
                    SoftBanFor(gLock.id, now, kCrossLayerForbidSoftBanMs);
                    LogLine("Aim forbid cross_layer id=%d ban=%ums (tp_off)", gLock.id,
                            (unsigned)kCrossLayerForbidSoftBanMs);
                    ClearLock();
                    EnterState(State::Acquire, now, "cross_layer_forbid");
                    continue;
                }
            }
            if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff) ||
                InMeleeHoldBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                const float faceDx = gLock.x - player.x;
                (void)ports::attack::FaceToward(faceDx);
                EnterState(State::Firing, now, "ready");
                continue;
            }
            if (tpOn && NeedsReapproach(player.x, player.y, gLock.x, gLock.y)) {
                // d1a58e：sticky 冷却中禁止 aim_reapproach 回灌 MoveTo。
                if (StickyCorrectCooling(now)) {
                    break;
                }
                if (TryEnterMoveTo(now, "aim_reapproach")) continue;
                break;
            }
            // 中距：等 CD / 等怪走近，不翻侧。
            break;
        }

        case State::Firing: {
            if (ports::attack::IsFireSuppressed()) break;  // buffs Hold：同拍已进 Firing 也停
            if (ports::skill::IsPreparingSkill()) break;   // 手搓/自动 Prepare：不出刀
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;  // 同 tick 选下一只并贴
            }
            {
                bool moved = false;
                if (TryConsumeWhiffApproachCorrect(now, player.x, player.y, standOff, tpOn,
                                                   &moved)) {
                    if (moved) continue;
                    break;
                }
            }
            // P0 出刀硬门：未进带绝不砍（含误入 Firing 的路径）。
            if (!FireGateOk(player.x, player.y, gLock.x, gLock.y, standOff, now, "Firing")) {
                if (tpOn && NeedsReapproach(player.x, player.y, gLock.x, gLock.y) &&
                    !StickyCorrectCooling(now)) {
                    if (TryEnterMoveTo(now, "fire_gate")) continue;
                }
                EnterState(State::Aim, now, "fire_gate");
                break;
            }
            // 间隔/松键由 TryFirePrimary 门控；贴怪瞬移已不再等 MotionBusy。

            // 泵拥堵：本 tick 不出刀（多刀/主刀同源背压），回 Recover 等排水，走
            // pace_wait 软路径而非硬失败（与 CanFirePrimary 背压同义）。
            if (x::runtime::main_thread::IsCongested()) {
                static DWORD sPumpFire = 0;
                if (!sPumpFire || now - sPumpFire > 2000) {
                    sPumpFire = now;
                    LogLine("fire defer id=%d pump_congested q=%d", gLock.id,
                            x::runtime::main_thread::QueuedJobCount());
                }
                EnterState(State::Recover, now, "pace_wait_pump");
                break;
            }

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
                    LogToFile("multi fire id=%d dx=%.0f hp=%d whiff=%d arm=%d", gLock.id, faceDx,
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
                    LogToFile("fire id=%d dx=%.0f hp=%d whiff=%d arm=%d", gLock.id, faceDx, hpBefore,
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
            // 射后不管：出刀当帧即可早切（不等 Recover 下一拍读 lastHitted）。
            if (TryAbandonOneshot(now)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;
            }
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
            {
                bool moved = false;
                if (TryConsumeWhiffApproachCorrect(now, player.x, player.y, standOff, tpOn,
                                                   &moved)) {
                    if (moved) continue;
                    break;
                }
            }
            // 黏住无脑A：近距继续砍；跨层/真远才重贴。
            const float dx = std::fabs(gLock.x - player.x);
            const bool same = SameLayer(player.x, player.y, gLock.x, gLock.y);
            // needApproachCorrect 未清时禁止 still_valid/near_band 空砍。
            if (same && !gLock.needApproachCorrect &&
                InMeleeHoldBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                if (!ports::attack::CanFirePrimary()) break;
                EnterState(State::Firing, now, "still_valid");
                continue;
            }
            if (tpOn && same && dx < 1.f) {
                if (!TryEnterMoveTo(now, LiveStepOn() ? "hug_follow" : "recenter_hug")) break;
                continue;
            }
            if (tpOn && NeedsReapproach(player.x, player.y, gLock.x, gLock.y) &&
                !StickyCorrectCooling(now)) {
                const char* why = same ? "reapproach" : "reapproach_cross";
                if (!TryEnterMoveTo(now, why)) break;
                continue;
            }
            if (same && !gLock.needApproachCorrect) {
                // 中距未进 hold：仍尝试砍（FireGate 再判），不 recover_out_band。
                if (!ports::attack::CanFirePrimary()) break;
                EnterState(State::Firing, now, "near_band");
                continue;
            }
            EnterState(State::Acquire, now, "reacquire");
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
    LogLine("hop_cap maxHop=%u always_chunk=1 direct_layer=%d",
            gTeleportMaxHop.load(std::memory_order_acquire), kDirectTeleportNoLayerHop ? 1 : 0);
    if (kDirectTeleportNoLayerHop) {
        LogLine("direct_tp mode=1 (no layer/zMass filter; hop chunk STILL on)");
    } else {
    LogLine("layer_hop mode=1 (zMass+floor, maxHop chunk, cross_layer_fill_gate, "
            "dist_budget=%.0fpx/%us no_reset, fhBanSpread=%d; native_land snapTol=%.0f "
            "attachWait=%ums)",
            FillBudgetPx(), (unsigned)(kFillDistWindowMs / 1000), kLandFhBanSpread,
            kNativeSnapTolPx, (unsigned)kNativeAttachWaitMs);
    }
    ports::input::Init();
    ports::attack::Init();
    (void)ports::teleport::EnsureBound();

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        // 必须用 NowMs：GetTickCount 只按 15.625ms 步进，timeBeginPeriod(1) 也改不动它，
        // 于是 tick 门再怎么调小，TickImpl 实际仍是每 15.625ms 才跑一次 —— 这是出刀节奏的
        // 第二个量化器（第一个在 attack_input_port 的 SoftBlocked）。两处都换掉才有效。
        const DWORD now = x::runtime::NowMs();
        const DWORD tickMs = gTickIntervalMs.load(std::memory_order_acquire);
        if (!gLastTick || now - gLastTick >= tickMs) {
            gLastTick = now;
            TickImpl(now);
        }
        // 睡眠粒度必须细于 tick，否则 tick 被 Sleep 步长二次量化：kIdleSleepMs=8 时
        // tick=10 只能在 8/16ms 的检查点上命中，实跑 16ms。取半个 tick 封顶 kIdleSleepMs。
        const DWORD nap = tickMs <= kIdleSleepMs
                              ? 1
                              : (tickMs / 2 < kIdleSleepMs ? tickMs / 2 : kIdleSleepMs);
        Sleep(nap);
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
    gHardPauseMask.store(0, std::memory_order_release);
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

bool IsFarmingActive() {
    return gEnabled.load(std::memory_order_acquire) &&
           gHardPauseMask.load(std::memory_order_acquire) == 0;
}

bool IsLootPulseActive() {
    // 未真正挂机：吸物自由跑（与改前一致）。
    if (!IsFarmingActive()) return true;
    return LootPulseOpenNow(GetTickCount());
}

uint32_t LootPulseGeneration() {
    return gLootPulseGen.load(std::memory_order_acquire);
}

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

void SetTeleportParams(uint32_t minDx, uint32_t standOff, uint32_t cooldownMs, uint32_t maxHop,
                       uint32_t crossLayerFillGateMs, uint32_t fillBudgetPx) {
    if (minDx < 160) minDx = 160;
    if (minDx > 2000) minDx = 2000;
    standOff = xcat::ClampCombatTeleportStandOff(standOff);
    cooldownMs = xcat::ClampCombatTeleportCooldownMs(cooldownMs);
    // 旧默认 400 会导致中→顶 hop_chunk_fail；与 payload 落盘迁移一致。
    if (!maxHop || maxHop == xcat::kCombatTeleportMaxHopLegacyDefault)
        maxHop = xcat::kCombatTeleportMaxHopDefault;
    maxHop = xcat::ClampCombatTeleportMaxHop(maxHop);
    crossLayerFillGateMs = xcat::ClampCombatCrossLayerFillGateMs(crossLayerFillGateMs);
    gTeleportMinDx.store(minDx, std::memory_order_release);
    gTeleportStandOff.store(standOff, std::memory_order_release);
    gTeleportCooldownMs.store(cooldownMs, std::memory_order_release);
    gTeleportMaxHop.store(maxHop, std::memory_order_release);
    gCrossLayerFillGateMs.store(crossLayerFillGateMs, std::memory_order_release);
    if (crossLayerFillGateMs == 0) gCrossLayerFillGateUntil = 0;
    gFillBudgetPx.store(xcat::ClampCombatFillBudgetPx(fillBudgetPx), std::memory_order_release);
    ports::teleport::SetNativeCooldownMs(cooldownMs);
}

void SetOneshotParams(uint32_t maxHp, uint32_t minBumps, uint32_t minFires, uint32_t minLagMs,
                      uint32_t foxFillGapMs) {
    maxHp = xcat::ClampCombatOneshotMaxHp(maxHp);
    minBumps = xcat::ClampCombatOneshotMinBumps(minBumps);
    minFires = xcat::ClampCombatOneshotMinFires(minFires);
    minLagMs = xcat::ClampCombatOneshotMinLagMs(minLagMs);
    foxFillGapMs = xcat::ClampCombatOneshotFoxFillGapMs(foxFillGapMs);
    const int64_t prevMax = gOneshotMaxHp.exchange(static_cast<int64_t>(maxHp),
                                                    std::memory_order_acq_rel);
    const int prevBumps = gOneshotMinBumps.exchange(static_cast<int>(minBumps),
                                                    std::memory_order_acq_rel);
    const int prevFires = gOneshotMinFires.exchange(static_cast<int>(minFires),
                                                     std::memory_order_acq_rel);
    const DWORD prevLag = gOneshotMinLagMs.exchange(minLagMs, std::memory_order_acq_rel);
    const DWORD prevGap = gOneshotFoxFillGapMs.exchange(foxFillGapMs, std::memory_order_acq_rel);
    if (foxFillGapMs == 0) gFoxFillGateUntil = 0;
    if (prevMax == static_cast<int64_t>(maxHp) && prevBumps == static_cast<int>(minBumps) &&
        prevFires == static_cast<int>(minFires) && prevLag == minLagMs && prevGap == foxFillGapMs) {
        return;
    }
    LogLine("SetOneshotParams maxHp=%u bumps=%u fires=%u lag=%ums foxGap=%ums", maxHp, minBumps,
            minFires, minLagMs, foxFillGapMs);
}

void RefreshExternalPauseEffective() {
    const bool on = gHardPauseMask.load(std::memory_order_acquire) != 0 ||
                    gExternalPauseDepth.load(std::memory_order_acquire) > 0;
    gExternalPause.store(on, std::memory_order_release);
    // 与 Tick 顶层闸同步：已进 Firing 的同拍 / 主线程排队出刀也要被 TryFirePrimary 挡掉。
    ports::attack::SetFireSuppressed(on);
}

void SetHardPause(HardPauseHolder holder, bool on) {
    const uint32_t bit = static_cast<uint32_t>(holder);
    if (!bit) return;
    uint32_t prev = gHardPauseMask.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = on ? (prev | bit) : (prev & ~bit);
        if (gHardPauseMask.compare_exchange_weak(prev, next, std::memory_order_acq_rel)) break;
    }
    RefreshExternalPauseEffective();
}

void SetExternalPause(bool on) {
    // 遗留入口：勿用于新代码。会直接写 AutoLie 位，绕过 follower 的 quiz|following|ui 聚合。
    // 新代码请用 SetHardPause(holder, …)；测谎请走 auto_lie / SetQuizWorldPaused。
    SetHardPause(HardPauseHolder::AutoLie, on);
}

void AcquireExternalPause() {
    const int prev = gExternalPauseDepth.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0) SoftResetWhiffForLightPause("pause_on");
    RefreshExternalPauseEffective();
}

void ReleaseExternalPause() {
    bool releasedLast = false;
    for (;;) {
        int cur = gExternalPauseDepth.load(std::memory_order_acquire);
        if (cur <= 0) {
            gExternalPauseDepth.store(0, std::memory_order_release);
            break;
        }
        if (gExternalPauseDepth.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) {
            if (cur == 1) {
                SoftResetWhiffForLightPause("pause_off");
                releasedLast = true;
            }
            break;
        }
    }
    RefreshExternalPauseEffective();
    // 深度归零且硬闸也关：开恢复窗，避免 Hold 期间过期落点立刻远跳。
    if (releasedLast && gHardPauseMask.load(std::memory_order_acquire) == 0 &&
        !gExternalPause.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        gPostExtResumeUntil = now + kPostExtResumeGraceMs;
        gResumeRelockPending = true;
    }
}

bool IsTeleportTransit() {
    // 只挡「真·换图/收态」。MoveTo + post-quiet 在贴怪开启时几乎常真（hop CD≈200、
    // quiet=220），会让 timed_keys/buffs 在 BeginAct 前永久 defer，ExternalPause 停不了刀。
    // 外部动作应先 Arm pause 打断 MoveTo，再插键/DoActive。
    // settle=0 时 TP 走 Aim 不进 Settling，本函数运行时几乎只命中 gMapArmUntilMs。
    if (gState == State::Settling) return true;
    const DWORD now = GetTickCount();
    if (gMapArmUntilMs && static_cast<int>(now - gMapArmUntilMs) < 0) return true;
    return false;
}

void ResetForMapChange() {
    gFoxFillGateUntil = 0;
    gCrossLayerFillGateUntil = 0;
    // 位移预算跨图保留：服端漏桶跟会话走，换图重置会放纵「爬完就传门再爬」。
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
