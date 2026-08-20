// Classic TWMS — simple_combat explicit state machine (full redesign).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "simple_combat.h"

#include "heli_rotor.h"
#include "reach_cal.h"

#include "../auto_supply/auto_supply.h"
#include "../attack_accel/attack_accel.h"
#include "../fly/fly.h"
#include "../kick_sniff/kick_sniff.h"
#include "../multi_skill/multi_skill.h"
#include "../pet_feed/pet_feed.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../ports/attack_input_port.h"
#include "../ports/attack_rpc_port.h"
#include "../ports/foothold_path.h"
#include "../ports/foothold_port.h"
#include "../ports/ground_spoof.h"
#include "../ports/hit_pin_port.h"
#include "../ports/input_port.h"
#include "../ports/map_bounds_port.h"
#include "../ports/mob_gather_port.h"
#include "../ports/mob_pool_port.h"
#include "../mob_scan/mob_scan.h"
#include "../ports/multi_skill_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/skill_port.h"
#include "../ports/teleport_port.h"
#include "../ports/fly_fh_ban.h"
#include "../ports/world_port.h"
#include "../travel/travel.h"
#include "../invuln/invuln.h"
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
#include <string>
#include <timeapi.h>

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
// 微贴短 Settling。压低以缩短首刀前等待。
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
// 探针默认关（减负）：热路径只靠 hpPct / lastHitted；要对账再临时拨 true。
constexpr bool kProbeLastHittedLog = false;
constexpr bool kWhiffClearOnLastHitted = true;  // BIN：lastHitted 早于 hpPct；回退改 false
// BIN：UIHpTag 绝对血探针（combat.log · uihp_probe）；确认前不接入 abshp/oneshot。
// FindAll(UIHpTag) 约 400ms 一次，出刀不用 → 默认关。
constexpr bool kProbeUiHpTag = false;
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
// 命中盒接上但 ACC 不够 → 飘 MISS / 极低伤：lastHitted 仍 bump，whiff 清窗当「打中」。
// ACC MISS 另打 combat.log `acc_miss`（末条 DamageInfo.Damage=0 且 CharacterId=自己）。
// engaged 纠偏永不换怪。自上次有效掉血起墙钟限时无进展 → softBan 换靶。
// 轻暂停/prepare 冻钟（见 Note/ReleaseKillTimeoutHold），避免补 BUFF 误弃锁。
constexpr DWORD kLockKillTimeoutMs = 3000;
constexpr DWORD kLockKillTimeoutSoftBanMs = 12000;
// 相对上次进度标记至少掉这么多 % 才算「有进展」并重置超时钟（1=任意可见掉血）。
constexpr int kLockKillTimeoutMinDropPct = 1;
constexpr DWORD kDeadSoftBanMs = 300;
// 早切（dealt/oneshot/abshp）：禁止当尸体软禁。BIN：600ms 禁锁 → 同 id 不再补刀 → 图上「杀不死」。
constexpr DWORD kEarlyAbandonSoftBanMs = 0;
// 打中换怪：刚打过的怪禁锁一会儿，避免投射物/回贴又打到同一只；也挡住 Acquire 回退最近。
constexpr DWORD kHitRotateBanMs = 5000;
constexpr int kHitRotateMinLive = 3;
// 不打 MISS 怪：连续 ACC MISS 满 N 后禁锁。sticky（kBanHitRotate）防同 tick 重选。
// 比 hit_rotate 稍长：高 EVA 怪会一直飘字，短禁立刻被最近选回来。
constexpr DWORD kSkipAccMissBanMs = 8000;
constexpr DWORD kTeleportOneHitBanMs = 2500;
// lastHitted 多数当帧写，但 DamageInfo 列表常晚一拍；窗只挡「确认命中」路径。
// 锁怪换刀真源是 hitBumpCount（lastHitted 上升沿），不依赖这扇窗、也不依赖 DI.charId。
constexpr DWORD kHitRotateObserveMs = 220;
// 刚满 N 的怪周围这一盒仍可能进下一刀攻击盒（BIN：叠怪 |dx|≈64 仍 off_lock）。
// 数值对齐出刀带 kHeliFireMaxDx/Dy；盒外再按离玩家最近选，不再飞全图对角。
constexpr float kHitRotateClearDx = 280.f;
constexpr float kHitRotateClearDy = 220.f;
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
// 瞬移远近同价：不再按 hop / 同层·跨层分档。
constexpr DWORD kPostDoingPosSaneMaxMs = 200;
// Doing 同帧读回 Ap 必 d≈0；至少跨过 1～2 帧再放行，避免 early→Fire 后 Ap 崩成 (0,0)。
constexpr DWORD kPostDoingMinSettleMs = 32;
// BIN 326d34 / P0d：direct_tp 无 hop 切段 → 起伏图单次 fill 1500~3500px → 205 soft。
// false = zMass+同高过滤、maxHop 分段；瞬移可跨层且同层/跨层同价。
constexpr bool kDirectTeleportNoLayerHop = false;
// 吸物脉冲：短 Settling 也至少开这么久，避免 pet_loot interval 对不齐落地窗。
constexpr DWORD kLootPulseFloorMs = 120;
// Acquire / 无缓存：快照超过此年龄则 TryRefreshCacheLite（字典只读，不 FindAll）。
constexpr DWORD kMobCacheFreshMs = 16;

// 当前 combat tick 的 mob 快照；ClearLockRetarget 写回，避免同 tick 抱旧表。
// thread_local：仅本线程 Tick 可见，避免以后多线程调 Tick 时串指针。
thread_local ports::mob::Snapshot* gTickMobSnap = nullptr;

struct TickMobSnapScope {
    explicit TickMobSnapScope(ports::mob::Snapshot* p) { gTickMobSnap = p; }
    ~TickMobSnapScope() { gTickMobSnap = nullptr; }
    TickMobSnapScope(const TickMobSnapScope&) = delete;
    TickMobSnapScope& operator=(const TickMobSnapScope&) = delete;
};
// MoveTo / TryEnterMoveTo 干等（CD）时续期长度。
constexpr DWORD kLootPulseHoldMs = 80;
constexpr DWORD kCrossLayerForbidSoftBanMs = 4000;
// 禁贴 x≈0 轴落点（BIN：chunk to=(0,-2145)）；|x|<此值视为有掉出/原点崩风险。
constexpr float kMinLandAbsAxisX = 8.f;
// 原点邻域永远禁止作落点（不使用 (0,0)）。
constexpr float kForbiddenOriginEps = 8.f;
constexpr float kPostDoingLandEpsPx = 96.f;
// 引擎自主落点允许的最大位移：超过即认为不是「落到台面」而是真滑走。
constexpr float kNativeSnapTolPx = 64.f;
// Doing 后等 CollisionDetect 挂台的宽限（下落中 curFh=0 属正常，此窗内不判滑走）。
constexpr DWORD kNativeAttachWaitMs = 140;
// 换图 / 重新 PlayReady 后短暂禁止贴怪与出刀；同图热开 F5 不走此宽限。
// 覆盖 FH 缓存重建 + 首跳 RelPos/台稳定窗；崩坐标后也走此宽限。
constexpr DWORD kMapArmGraceMs = 1500;
// F5 / 面板刚开：先空转一截再找怪出刀。BIN 03:38:25 贴身站桩热开 20ms 内
// SetInput + SetImpactNext + OnFuncKey，随后进程停写。只在 enable 边沿武装一次，
// 换怪不走。同图热开仍清 map arm（那是 1.5s）；本 hold 只挡旋翼/出刀。
constexpr DWORD kCombatEnableHoldMs = 300;
constexpr int kSoftBanCap = 24;

DWORD SettleMsForHop(float hop, bool hug) {
    (void)hop;
    (void)hug;
    return 0;
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
DWORD gEnableHoldUntilMs = 0;  // 0=无；与 TickImpl 同源 NowMs
bool gHiraishinNeedLootHold = false;
DWORD gHiraishinLootHoldUntilMs = 0;  // 0=时钟未起；与 TickImpl 同源 NowMs
bool gHiraishinSawSoftQuiet = false;  // 软重连/断线静默边沿，落地后重新武装静止窗
// F5 开后默认不武装旋翼。BIN 16:17:13：hold 300ms 后仍贴身 v=0 打 SetImpactNext(-160,80) 崩。
// 要飞（MoveTo）或开 F5 时已经离台，才 latch；换怪不重置。
bool gHeliLatchedThisEnable = false;
int gLastMapId = -1;
// PlayReady 闪断只清 gLastMapId；本值跨软重连保留，用来认出「同图回来」而不是真换图。
int gStickyMapId = -1;
// 本图至少挂过一次台（CurFh!=0）。进图/注入瞬间 CurFh 常为 0：此时 BAN+detach
// 会让人自由落体（BIN 15:46：footholds=426 仍 curFh=0，F5 已 BAN ON）。
// 交战期 BAN 摘台后此标志保持，空中砍不受影响。
bool gSawOnFhThisMap = false;
// encounter ResetForMapChange 与 combat mapId 边沿会各调一次 OnCombatMapChange；
// 同图短窗内第二次会 RestartLieSafeLand 拆掉 catch → 旋翼抖振（review 686e3f）。
constexpr DWORD kMapChangeDedupeMs = 800;
int gLastMapChangeHandledId = 0;
DWORD gLastMapChangeHandledMs = 0;
// BUFF/定时键放闸后：短暂宽限窗（取消过期 MoveTo；CD 不再按 hop 抬高）。
constexpr DWORD kPostExtResumeGraceMs = 280;
DWORD gPostExtResumeUntil = 0;
bool gResumeRelockPending = false;
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gTeleportEnabled{false};  // fill+Doing 瞬移找怪；面板单选，默认关
std::atomic<bool> gTeleportOneHit{false};   // 瞬移「每只怪打一下」；默认关
std::atomic<bool> gImpactApproachEnabled{true};  // Impact 贴怪默认开；优先于拟人/瞬移
std::atomic<bool> gHiraishinEnabled{false};      // 站桩输出；与 Impact/拟人互斥，默认关
std::atomic<DWORD> gHiraishinLootHoldMs{xcat::kHiraishinLootHoldDefaultMs};
std::atomic<uint32_t> gHiraishinRangePx{xcat::kHiraishinRangeDefaultPx};
std::atomic<uint32_t> gHiraishinFrontDx{xcat::kHiraishinFrontDxDefault};
std::atomic<uint32_t> gHiraishinFrontDy{xcat::kHiraishinFrontDyDefault};
// 选怪圈由范围滑条管，0=叠怪作动器半径。出刀等怪进近战带。
std::atomic<bool> gAntiJitterEnabled{true};       // 空中贴怪防抖；可面板/ini 一键关
// 防贴脸退避（LiveStep）；默认关。这是整套退避的**唯一总闸**：为 false 时 ComputeDodge
// 立刻清零并返回，读 gDodge 的三处判据全部走原分支，行为与没有这个功能时逐位相同。
std::atomic<bool> gAntiHugEnabled{false};
std::atomic<bool> gHumanWalkEnabled{true};  // 拟人；仅 Impact 关时生效
std::atomic<bool> gLiveStepEnabled{false};  // 锁怪后同层微贴；默认关
std::atomic<bool> gClusterPriority{false};  // 群怪优先；默认关
std::atomic<bool> gHitRotateEnabled{false};  // 打中换怪；默认关
std::atomic<int> gHitRotateN{static_cast<int>(xcat::kCombatHitRotateNDefault)};
std::atomic<bool> gSkipAccMissEnabled{true};  // 不打 MISS 怪；厂默开 / N=1
std::atomic<int> gSkipAccMissN{static_cast<int>(xcat::kCombatSkipAccMissNDefault)};
std::atomic<bool> gForgeHitEnabled{false};  // 实验·出刀自组攻包；默认关
std::atomic<uint32_t> gForgeHitFrontDx{xcat::kForgeHitFrontDxDefault};
std::atomic<uint32_t> gForgeHitFrontDy{xcat::kForgeHitFrontDyDefault};
// 刚因确认命中满 N 弃锁：下一次 Acquire 按「离挨刀那只最远」选，禁止回退最近。
bool gHitRotatePending = false;
int gHitRotateFromId = 0;
float gHitRotateFromX = 0.f;
float gHitRotateFromY = 0.f;
DWORD gHitRotateObserveUntil = 0;  // 我方刚出刀后的确认窗；窗外的 bump 不当自己的命中
struct HitRotateWatch {
    int id = 0;
    int32_t lastHitted = 0;
    int hits = 0;  // 对该 oid 的确认命中；满 N 后 spentUntil 内不清零（防贴身再开一轮）
    DWORD spentUntil = 0;  // 满 N 后的冷却；到期才允许再计 N 刀（补残血）
    bool seeded = false;
    bool seen = false;
};
constexpr int kHitRotateWatchCap = 64;
HitRotateWatch gHitRotateWatch[kHitRotateWatchCap];
int gHitRotateWatchN = 0;

bool TeleportOneHitActive() {
    return gTeleportOneHit.load(std::memory_order_acquire) &&
           gTeleportEnabled.load(std::memory_order_acquire);
}
// 瞬移找怪：打怪不等召宠。召唤仍在 pet_feed 后台跑。
// BIN 05:04:36：soft_hold 放行后 wait pet ~2.9s 才第一发；同图 05:04:04 宠已在则 27ms。
bool TeleportSkipsPetSummonHold() {
    return gTeleportEnabled.load(std::memory_order_acquire);
}
bool HitRotateFarmActive() {
    return gHitRotateEnabled.load(std::memory_order_acquire) && !TeleportOneHitActive();
}

std::atomic<uint32_t> gTeleportStandOff{xcat::kCombatTeleportStandOffDefault};
// 自定义站距：水平 X 与瞬移/拟人 ClampStandOff 共用；Y 只喂直升机。
std::atomic<bool> gStandOffCustom{false};
std::atomic<uint32_t> gStandOffX{xcat::kCombatStandOffXDefault};
std::atomic<int32_t> gStandOffY{xcat::kCombatStandOffYDefault};
std::atomic<uint32_t> gTeleportCooldownMs{xcat::kCombatTeleportCooldownDefaultMs};
std::atomic<uint32_t> gTeleportMinDx{xcat::kCombatTeleportMinDxDefault};
// 单次贴怪 hop 上限（px）；更远分段；调试 TAB / core.simpleCombatTeleportMaxHop。
std::atomic<uint32_t> gTeleportMaxHop{xcat::kCombatTeleportMaxHopDefault};
// 拟人走路追怪超时：卡台边 / 够不着则 softBan 换怪。
constexpr DWORD kHumanWalkTimeoutMs = 8000;
// 拟人单层 MVP（upload 847b21）：timeout 禁 200ms → 两只错台怪 8s 乒乓空走。
// 不可达禁锁对齐跨层量级，让同台怪有机会被选中。
constexpr DWORD kHumanUnreachableSoftBanMs = 5000;
// 走路无进展早退：进度钟武装后，「自己几乎没朝目标挪」且「距离也没缩短」→ 卡边。
// 自己走了但 dx 变大 = 怪跑路，不 stall（勿误禁 5s）。
constexpr DWORD kHumanWalkStallMs = 2500;
constexpr float kHumanWalkMinProgressPx = 40.f;
// BIN 00:43：失焦开步 travel=0 空等满 stall → 进度钟未武装前仅给这么久，超时 human_no_move。
constexpr DWORD kHumanWalkArmGiveUpMs = 1200;
constexpr float kHumanWalkArmTravelPx = 2.f;  // 自身位移达此才武装进度钟
// no_move 多为失焦/慢启动，勿用满额 5s 把旁怪一起晾干。
constexpr DWORD kHumanNoMoveSoftBanMs = 1500;
// BIN 01:01：kHumanWalkMaxChaseDx=400 把同台 ok=4/5 全标 humanFar → 远处有怪空转。
// 远距卡死改由 stall / no_move / walk timeout 管；选怪只靠同高 + Walk 连通 + 近者优先。
// BIN 01:07：MoveTo 用选怪同高 45，走路 |dy| 微漂到 46~48 就 forbid+5s ban → 左右远怪乒乓洗黑名单。
// 已锁追击放宽到同高地板容差；真断崖才 5s，Y 越界只短晾。
constexpr float kHumanWalkChaseY = kSameFloorMaxDy;  // 100
constexpr DWORD kHumanYGapSoftBanMs = 600;
// BIN 01:11：fg=0 时 travel=0 → 1.2s human_no_move 洗 ban；失焦冻结走路计时。
// 途中顺手刀：短冷却 + 交手粘锁，防叠怪 human_passby 乒乓（BIN 00:01:56）。
constexpr DWORD kHumanPassbyCooldownMs = 500;

float MaxApproachHopPx() {
    return static_cast<float>(gTeleportMaxHop.load(std::memory_order_acquire));
}
std::atomic<int64_t> gOneshotMaxHp{kOneshotMaxHpDefault};
std::atomic<int> gOneshotMinBumps{kOneshotMinBumpsDefault};
std::atomic<int> gOneshotMinFires{kOneshotMinFiresDefault};
std::atomic<DWORD> gOneshotMinLagMs{kOneshotMinLagMsDefault};
std::atomic<DWORD> gOneshotFoxFillGapMs{xcat::kCombatOneshotFoxFillGapDefaultMs};
DWORD gFoxFillGateUntil = 0;         // 射后不管：此 tick 前禁 fill
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
// F5 刚开：首刀前强制 ApplyFaceNow + Recover 一拍。
// BIN：热开后已在出刀带 → Aim→火仅 24ms，无换向分拍时朝向未落地就原地空挥。
std::atomic<bool> gNeedEnableFaceSettle{false};

State gState = State::Idle;
DWORD gStateEnterMs = 0;
// 挂机吸物脉冲截止（GetTickCount）；0=关。代数供 pet_loot 边沿立刻吸一拍。
std::atomic<DWORD> gLootPulseUntil{0};
std::atomic<uint32_t> gLootPulseGen{0};
std::atomic<bool> gHvLootUrgent{false};
std::atomic<bool> gHvLootPauseHeld{false};
DWORD gSettleUntil = 0;
float gSettleX = 0.f;
float gSettleY = 0.f;
uint32_t gSettleFh = 0;
bool gSettleNeedPosSane = false;  // Doing 后：时间窗可为 0，但仍要 PosSane 门
DWORD gSettleEnteredAt = 0;         // Settling 进入时刻（MinSettle 用）
DWORD gSettleMinMs = 48;            // 本跳实际 MinSettle（同层/跨层不同）
bool gSettleWasCross = false;       // 仅日志：本跳是否跨层
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
    // 武装时刻。用来量「出刀 → lastHitted 回写」的真实延迟：若延迟中位已逼近
    // kWhiffObserveMs，那么「窗满未掉血」判空里有一部分只是窗太短，不是真空刀。
    // 这是分清「第一刀被引擎吞掉」与「第一刀命中回写晚于窗」的唯一判据，别删。
    DWORD armAtMs = 0;
    // 本窗**首刀**的 |dx|，判决时喂给 reach_cal 估触及包线。取首刀是为了与离线口径
    // 一致（`_faceflip.windows` 也用 w[0]），两边的数才能并表核对。
    float armDx = -1.f;
    int firesInArm = 0;
    bool hitProbeLogged = false;  // 本观察窗内 hit_probe 只打一次，防刷屏
    int32_t accMissWaitHitted = -1;  // lastHitted 上升后等 DamageInfo；-1=无待判
    int accMissCount = 0;            // 本锁确认的 ACC MISS 次数（Damage=0）
    int accMissStreak = 0;           // 连续 ACC MISS；真伤/掉血清零
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
    DWORD firstFireMs = 0;  // 本锁首刀墙钟（诊断）；KillTimeout 认 lastKillProgressMs
    DWORD lastKillProgressMs = 0;  // 上次有效掉血（或首刀）时刻；超时从这里起算
    int32_t killProgressHp = -1;   // 进度标记血量%；掉 ≥ MinDrop 才刷新
    DWORD killTimeoutHoldSince = 0;  // 轻暂停/prepare 冻钟起点；0=未冻
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
    // FindHit 要 inView；false 时只贴飞不砍，避免空挥吃 softban（BIN：下层 iv=0 满血怪）。
    bool inView = true;
};
LockState gLock{};
const char* gLastLockLostWhy = "lost";
// iv=0 持火起点；仅「站稳后」累计，超时仍不可命中则 softban 换怪。
DWORD gInViewHoldSince = 0;
// 站稳后仍 iv=0 超过此时长 → unreachable softban（FindHit 长期不恢复）。
constexpr DWORD kInViewHoldFireMaxMs = 2500;
// 与 heli 贴飞失败同级；过短会只剩 iv=0 时反复重锁空转。
constexpr DWORD kInViewHoldSoftBanMs = 2500;
// 落点侧粘滞（对照枫星 g_targetSide）：-1=怪左 / +1=怪右；朝向另算，不绑此值。
int gLandSide = 0;
DWORD gStandstillSince = 0;
DWORD gStandstillShuffleLast = 0;
DWORD gEngineBusySince = 0;   // 被引擎忙锁连续拦住的起点；0=上一拍不忙
unsigned gEngineBusyTicks = 0;  // 累计被拦拍数（节流播报用，勿逐拍打日志）
DWORD gStickySpinSince = 0;  // hop_sticky Aim↔MoveTo 空转计时
DWORD gLastHumanPassbyMs = 0;
float gHumanWalkStartAbsDx = -1.f;  // 进度钟武装时的 |dx|；<0=未采样
float gHumanWalkStartPx = 0.f;
int gHumanWalkStartDir = 0;  // 武装时朝怪符号 -1/1
DWORD gHumanWalkArmedMs = 0;  // 首次真正开走（travel）时刻；0=未武装
DWORD gHumanWalkFocusTickMs = 0;  // 失焦时推进 enter/armed 锚点，冻 no_move/stall/timeout
float gStandstillAnchorX = 0.f;
float gStandstillAnchorY = 0.f;

// sticky：acquire miss 时 DropSoftBanNonSticky 不得清掉（BIN：dead 用 generic → 同 tick 鬼锁重选）。
enum SoftBanKind : uint8_t {
    kBanGeneric = 0,
    kBanWhiff = 1,
    kBanUnreachable = 2,
    kBanDead = 3,
    kBanHitRotate = 4,  // 打中换怪：确认命中满 N 后禁锁，DropSoftBanNonSticky 不得清
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
    // acquire miss / mob_cache 是选怪空转心跳，combat.log 留全量；x.jsonl 15s 一条。
    if (std::strncmp(body, "acquire miss ", 13) == 0 ||
        std::strncmp(body, "mob_cache refresh ", 18) == 0) {
        x::runtime::LogIThrottled(1, 15000, "SimpleCombat", "%s", body);
        return;
    }
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
    const State prev = gState;
    // 离开走路追怪态时清 InputX，避免粘键拖进出刀/Idle。
    //
    // ⚠️ StopWalk 会 `InvokeAndWait` **阻塞抢占游戏主线程**（High），没在走时也曾空跑整段。
    // BIN 1394b0：FSM 自激 64/s → 合成键+泵抢占冻死 ImGui。Impact 从不走路：直接跳过。
    // 拟人仍需停步，但 StopWalk 已对 prev==0 早退、HoldWalk 同向短路；出带另有迟滞。
    // 切档锁存清理由 `SetImpactApproachEnabled` / `SetHumanWalkEnabled` 兜底。
    if (prev == State::MoveTo && s != State::MoveTo &&
        !gImpactApproachEnabled.load(std::memory_order_acquire)) {
        (void)ports::attack::StopWalk();
    }
    const bool hotCycle =
        (prev == State::Firing && s == State::Recover) ||
        (prev == State::Recover && s == State::Firing);
    const char* msgWhy = why && why[0] ? why : "";
    const char* sep = why && why[0] ? " " : "";
    if (hotCycle) {
        LogToFile("state %s→%s%s%s", StateName(prev), StateName(s), sep, msgWhy);
    } else {
        LogLine("state %s→%s%s%s", StateName(prev), StateName(s), sep, msgWhy);
    }
    gState = s;
    gStateEnterMs = now;
    if (s == State::MoveTo) {
        gHumanWalkStartAbsDx = -1.f;
        gHumanWalkStartDir = 0;
        gHumanWalkArmedMs = 0;
        gHumanWalkFocusTickMs = 0;
    }
    // 吸物：出刀链关脉冲；其余态开门。离开出刀 / 进 Settling 时边沿，便于立刻吸一拍。
    if (s == State::Aim || s == State::Firing || s == State::Recover) {
        ClearLootPulse();
    } else {
        const bool fromFire =
            prev == State::Aim || prev == State::Firing || prev == State::Recover;
        if (s == State::Settling) {
            DWORD remain = kLootPulseFloorMs;
            if (gSettleUntil && static_cast<int>(gSettleUntil - now) > 0) {
                remain = gSettleUntil - now;
                if (remain < kLootPulseFloorMs) remain = kLootPulseFloorMs;
            }
            ArmLootPulse(now, remain, /*edge=*/true);
        } else if (fromFire) {
            ArmLootPulse(now, kLootPulseFloorMs, /*edge=*/true);
        }
    }
}

void ClearWhiffArm() {
    gLock.armHp = -1;
    gLock.armHitted = -1;
    gLock.armUntil = 0;
    gLock.armAtMs = 0;
    gLock.armDx = -1.f;
    gLock.firesInArm = 0;
    gLock.hitProbeLogged = false;
    gLock.accMissWaitHitted = -1;
}

// 轻暂停 / skill_prepare：停刀期间 KillTimeout 不计墙钟（与 SoftResetWhiff 同因）。
void NoteKillTimeoutHold(DWORD now) {
    if (!gLock.firstFireMs && !gLock.lastKillProgressMs) return;
    if (!gLock.killTimeoutHoldSince) gLock.killTimeoutHoldSince = now;
}

void ReleaseKillTimeoutHold(DWORD now) {
    if (!gLock.killTimeoutHoldSince) return;
    const DWORD held = now - gLock.killTimeoutHoldSince;
    gLock.killTimeoutHoldSince = 0;
    if (!held) return;
    // 把锚点整体前推 = 冻住年龄。
    if (gLock.firstFireMs) gLock.firstFireMs += held;
    if (gLock.lastKillProgressMs) gLock.lastKillProgressMs += held;
}

// 轻暂停（buffs/timed_keys）会停出刀数百 ms：若不撤观察窗，墙钟到期会误 +whiff → 换怪瞬移（BIN：定时键后 +0.8s whiff×3）。
void SoftResetWhiffForLightPause(const char* why) {
    NoteKillTimeoutHold(GetTickCount());
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
    gInViewHoldSince = 0;
    // 不清会残留上一只怪的忙窗起点：下次开打首拍就被判「已等超 1200ms」而误放行一刀。
    gEngineBusySince = 0;
}

// 换怪瞬间：叫醒 mob_scan，并尽量用已热身的 MobPool 字典立刻刷新缓存。
// forceLite：弃锁换怪必须扫字典（可能刚死），勿因龄≤16ms 跳过留下尸体行。
void NoteNeedFreshMobs(ports::mob::Snapshot* inoutSnap, bool forceLite = false) {
    mob_scan::RequestImmediateScan();
    const uint32_t age = ports::mob::GetCachedAgeMs();
    // EnsureFresh 热路径：缓存已够新则只回填，避免连调刷 Lite/Publish。
    if (!forceLite && age != 0xFFFFFFFFu && age <= kMobCacheFreshMs) {
        if (inoutSnap) (void)ports::mob::GetCached(*inoutSnap);
        return;
    }
    ports::mob::Snapshot fresh{};
    if (ports::mob::TryRefreshCacheLite(fresh) && fresh.ok) {
        if (inoutSnap) *inoutSnap = fresh;
        return;
    }
    if (inoutSnap) (void)ports::mob::GetCached(*inoutSnap);
}

// 弃锁并立刻换怪：写回本 tick snap（若有）+ 叫醒 mob_scan。
// forceLite：死怪/早切/whiff 等可能脏表时强制 Lite；落点/跨层 softBan 走龄门即可。
void ClearLockRetarget(bool forceLite = true) {
    ClearLock();
    NoteNeedFreshMobs(gTickMobSnap, forceLite);
}

// 选怪前保证快照够新；禁止完整 Collect（战斗线程）。
bool EnsureFreshMobSnap(ports::mob::Snapshot& snap, DWORD maxAgeMs) {
    const uint32_t ageBefore = ports::mob::GetCachedAgeMs();
    if (ports::mob::GetCached(snap) && snap.ok && ageBefore <= maxAgeMs) {
        return true;
    }
    NoteNeedFreshMobs(&snap);
    static DWORD sAgeLog = 0;
    const DWORD now = GetTickCount();
    if (!sAgeLog || now - sAgeLog > 800) {
        sAgeLog = now;
        LogLine("mob_cache refresh age=%ums max=%ums ok=%d n=%d",
                ageBefore == 0xFFFFFFFFu ? 0u : ageBefore, maxAgeMs, snap.ok ? 1 : 0,
                snap.count);
    }
    return snap.ok;
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
        } else if (kind == kBanHitRotate && gSoftBan[i].kind != kBanWhiff &&
                   gSoftBan[i].kind != kBanUnreachable) {
            gSoftBan[i].kind = kBanHitRotate;
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

// acquire 全 miss：丢掉 generic/no_land 短禁；保留 whiff + 不可达 + dead + 打中换怪，
// 避免清禁后立刻鬼锁空转 / 又打回刚出满 N 刀的那只。
void DropSoftBanNonSticky() {
    int w = 0;
    for (int i = 0; i < gSoftBanN; ++i) {
        const SoftBanKind k = gSoftBan[i].kind;
        if (k == kBanWhiff || k == kBanUnreachable || k == kBanDead || k == kBanHitRotate)
            gSoftBan[w++] = gSoftBan[i];
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
        ClearLockRetarget(/*forceLite=*/false);
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
        ClearLockRetarget(/*forceLite=*/false);
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

// 进盒后 ACC 不够：AddDamageInfo 仍写 lastHitted，但 Damage@+0x24=0（飘 MISS）。
// 列表偶发晚一拍 → 上升沿先记下 lastHitted，窗内再读。血条已掉不当 MISS。
void ResolveAccMissAfterBump(DWORD now, const char* when) {
    if (gLock.accMissWaitHitted < 0) return;
    if (gLock.armHp >= 0 && gLock.lastHp >= 0 && gLock.lastHp < gLock.armHp) {
        gLock.accMissWaitHitted = -1;
        gLock.accMissStreak = 0;
        return;
    }
    if (!gLock.ptr) return;
    const uint32_t me = ports::world::GetCharacterId();
    if (!me) return;
    ports::mob::DamageInfoSnap di{};
    if (!ports::mob::TryReadDamageInfoList(gLock.ptr, di) || !di.ok || di.count <= 0) return;
    const ports::mob::DamageInfoLite& last = di.items[di.count - 1];
    if (last.charId == 0) return;
    if (last.charId != me) {
        gLock.accMissWaitHitted = -1;
        return;
    }
    gLock.accMissWaitHitted = -1;
    if (last.damage > 0) {
        gLock.accMissStreak = 0;
        return;
    }
    gLock.accMissStreak += 1;
    gLock.accMissCount += 1;
    LogLine("acc_miss id=%d hit=%d dmg=0 char=%u skill=%d act=%d idx=%d hp=%d lf=%d fires=%d "
            "lat=%ums n=%d streak=%d when=%s",
            gLock.id, gLock.lastHitted, last.charId, last.skillId, last.hitAction, last.attackIdx,
            gLock.lastHp, gLock.lockFires, gLock.firesInArm,
            (unsigned)(gLock.armAtMs ? now - gLock.armAtMs : 0), gLock.accMissCount,
            gLock.accMissStreak, when && when[0] ? when : "?");
}

// CMS CheckPDamageMiss 吃 Rand32，禁止预调。进盒连续 Damage=0 满 N 再禁锁。
bool TryAbandonAccMiss(DWORD now) {
    if (!gSkipAccMissEnabled.load(std::memory_order_acquire)) return false;
    if (!gLock.id) return false;
    const int need = gSkipAccMissN.load(std::memory_order_acquire);
    if (need <= 0 || gLock.accMissStreak < need) return false;
    LogLine("switch reason=skip_acc_miss id=%d streak=%d need=%d n=%d hp=%d hit=%d fires=%d "
            "ban=%ums",
            gLock.id, gLock.accMissStreak, need, gLock.accMissCount, gLock.lastHp, gLock.lastHitted,
            gLock.lockFires, (unsigned)kSkipAccMissBanMs);
    SoftBanFor(gLock.id, now, kSkipAccMissBanMs, kBanHitRotate);
    gLastLockLostWhy = "skip_acc_miss";
    ClearLockRetarget();
    return true;
}

void ArmWhiffObserve(DWORD now, int hpAtFire, float absDx) {
    gLock.lockFires += 1;
    ReleaseKillTimeoutHold(now);
    if (gHitRotateEnabled.load(std::memory_order_acquire)) {
        gHitRotateObserveUntil = now + kHitRotateObserveMs;
    }
    if (!gLock.firstFireMs) {
        gLock.firstFireMs = now;
        gLock.lastKillProgressMs = now;
        if (gLock.killProgressHp < 0) gLock.killProgressHp = gLock.lockStartHp;
    }
    if (hpAtFire <= 0) return;
    if (gLock.armUntil == 0) {
        gLock.armHp = hpAtFire;
        gLock.armHitted = gLock.lastHitted;
        gLock.armUntil = now + kWhiffObserveMs;
        gLock.armAtMs = now;
        gLock.armDx = absDx;
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
    // 出刀前 snap/live 龄差（BIN：acquire lastHitted=0，首帧 live 已经是 135415）
    // 不能记成自己的一刀，否则 hitBumpCount 虚高、后面真命中反而不涨。
    if (gLock.lockFires <= 0) {
        gLock.prevHittedSample = gLock.lastHitted;
        return;
    }
    if (gLock.lastHitted > gLock.prevHittedSample) {
        gLock.hitBumpCount += 1;
        if (!gLock.firstBumpMs) gLock.firstBumpMs = now;
        gLock.prevHittedSample = gLock.lastHitted;
        gLock.accMissWaitHitted = gLock.lastHitted;
        ResolveAccMissAfterBump(now, "bump");

        // 列表多半只留末条：bump 时读 +0x24 累加到 dealtSum（零参数够死）。
        // 脆皮早切关着时不读 DamageInfo 列表。
        if (gLock.ptr && gOneshotMaxHp.load(std::memory_order_acquire) > 0) {
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
// 与脆皮面板同闸：oneshotMaxHp==0 时不跑（用户不用早切则不查表、不早切）。
bool TryAbandonDealtSum(DWORD now) {
    (void)now;
    if (gOneshotMaxHp.load(std::memory_order_acquire) <= 0) return false;
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
    ClearLockRetarget();
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
    ClearLockRetarget();
    return true;
}

// 加速 + 有 maxHP 表：用绝对剩余 vs 均刀×在途刀预测够死（不等死亡包）。
// 与脆皮面板同闸：oneshotMaxHp==0 时不跑。
bool TryAbandonAbsHp(DWORD now) {
    if (gOneshotMaxHp.load(std::memory_order_acquire) <= 0) return false;
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
    ClearLockRetarget();
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
    ClearLockRetarget();
    return true;
}

// 有可见掉血：刷新进度锚点（滑动窗）。MISS/磨皮不会进这里 → 满超时弃锁。
void NoteKillTimeoutHpProgress(DWORD now) {
    if (!gLock.lastKillProgressMs) return;
    if (gLock.lastHp < 0 || gLock.killProgressHp < 0) return;
    const int dropped = gLock.killProgressHp - gLock.lastHp;
    if (dropped < kLockKillTimeoutMinDropPct) return;
    gLock.killProgressHp = gLock.lastHp;
    gLock.lastKillProgressMs = now;
}

// 自上次有效掉血起限时无进展：MISS / 磨皮 / 打残后僵死 / engaged 永纠偏 → 拉黑换怪。
// 轻暂停持火中不计龄；掉血达 MinDrop% 会重置钟，避免「正在杀」被误弃。
bool TryAbandonLockKillTimeout(DWORD now) {
    if (gLock.killTimeoutHoldSince) return false;  // 仍在 pause/prepare
    if (!gLock.lastKillProgressMs) return false;

    NoteKillTimeoutHpProgress(now);

    const DWORD age = now - gLock.lastKillProgressMs;
    if (static_cast<int>(age) < static_cast<int>(kLockKillTimeoutMs)) return false;

    const int droppedTotal =
        (gLock.lockStartHp >= 0 && gLock.lastHp >= 0) ? (gLock.lockStartHp - gLock.lastHp) : 0;
    LogLine("switch reason=kill_timeout id=%d age=%ums fires=%d hp=%d→%d drop=%d hit=%d "
            "dealt=%lld bumps=%d softBan=%ums",
            gLock.id, (unsigned)age, gLock.lockFires, gLock.lockStartHp, gLock.lastHp,
            droppedTotal, gLock.lastHitted, static_cast<long long>(gLock.dealtSum),
            gLock.hitBumpCount, (unsigned)kLockKillTimeoutSoftBanMs);
    SoftBanFor(gLock.id, now, kLockKillTimeoutSoftBanMs, kBanWhiff);
    gLastLockLostWhy = "kill_timeout";
    ClearLockRetarget();
    return true;
}

// 窗内掉血 → 清 whiff；窗满无掉血 → +1；满 N → 清锁并返回 false。
// lastHitted 旁路：默认只探针；kWhiffClearOnLastHitted=true 时才参与清窗。
bool ResolveWhiffArm(DWORD now) {
    ResolveAccMissAfterBump(now, "arm");
    // 站桩输出：面前有怪就连砍。空砍 / 残血停滞一律不换锁、不 softBan。
    if (gHiraishinEnabled.load(std::memory_order_acquire)) {
        ClearWhiffArm();
        gLock.whiff = 0;
        gLock.needApproachCorrect = false;
        return true;
    }
    if (gLock.armUntil == 0) return true;

    const bool hpDrop =
        gLock.lastHp >= 0 && gLock.armHp >= 0 && gLock.lastHp < gLock.armHp;
    const bool hitBump =
        gLock.armHitted >= 0 && gLock.lastHitted > gLock.armHitted;

    if (kProbeLastHittedLog && hitBump && !gLock.hitProbeLogged) {
        gLock.hitProbeLogged = true;
        // lat = 出刀→回写延迟；lf = 本锁第几刀（1 即「换锁后第一刀」，实测其空刀率
        // 63% 且与站位/速度都无关，lat 就是用来判它到底是被吞还是回写晚的）。
        LogLine("hit_probe id=%d lastHitted=%d→%d hp=%d→%d fires=%d lat=%ums lf=%d clear=%d",
                gLock.id, gLock.armHitted, gLock.lastHitted, gLock.armHp, gLock.lastHp,
                gLock.firesInArm, (unsigned)(gLock.armAtMs ? now - gLock.armAtMs : 0),
                gLock.lockFires, kWhiffClearOnLastHitted ? 1 : 0);
    }

    // 触及包线自校准：喂真值用 hpDrop||hitBump（比清窗策略更纯的命中判据，
    // 不受 kWhiffClearOnLastHitted 开关影响）。命中与未命中各只喂一次，见下方窗满处。
    if (hpDrop || hitBump) {
        if (gLock.armDx >= 0.f) {
            reach::Feed(gLock.armDx, true);
            gLock.armDx = -1.f;  // 防同窗重复喂
        }
    }

    if (hpDrop || (kWhiffClearOnLastHitted && hitBump)) {
        if (gLock.whiff || gLock.firesInArm || hitBump) {
            LogLine("whiff clear id=%d why=%s hp=%d→%d hit=%d→%d fires=%d", gLock.id,
                    hpDrop ? "hp" : "lastHitted", gLock.armHp, gLock.lastHp, gLock.armHitted,
                    gLock.lastHitted, gLock.firesInArm);
        }
        if (hpDrop) gLock.accMissStreak = 0;
        gLock.whiff = 0;
        ClearWhiffArm();
        return true;
    }
    if (now < gLock.armUntil) return true;

    // 窗满仍无掉血/bump ⇒ 这一窗判空。所有「未命中」分支都从这里往下走，
    // 故只在此处喂一次即可覆盖全部失败路径（残血 reapproach / whiff++ / 弃怪）。
    if (gLock.armDx >= 0.f) {
        reach::Feed(gLock.armDx, false);
        gLock.armDx = -1.f;
    }

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
            ClearLockRetarget();
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

    // 换锁后第一刀空：不记 streak、不 softBan 换靶。面向/引擎 settle 的锅不该让刚贴上的
    // 怪被赶走；第二刀起恢复正常累计（lf 在 ArmWhiffObserve 里已 +1，故 lf<=1 = 仅首刀）。
    if (gLock.lockFires <= 1) {
        LogLine("whiff forgive why=first_lock_fire id=%d hp=%d hit=%d fires=%d lf=%d", gLock.id,
                gLock.lastHp, gLock.lastHitted, gLock.firesInArm, gLock.lockFires);
        ClearWhiffArm();
        return true;
    }

    gLock.whiff += 1;
    LogLine("whiff tick id=%d streak=%d/%d hp=%d hit=%d fires=%d lf=%d observe=%ums", gLock.id,
            gLock.whiff, kWhiffFireN, gLock.lastHp, gLock.lastHitted, gLock.firesInArm,
            gLock.lockFires, (unsigned)kWhiffObserveMs);
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
    ClearLockRetarget();
    return false;
}

bool SameLayerY(float a, float b) { return std::fabs(a - b) <= kSameLayerY; }

bool SameFloorY(float a, float b) { return std::fabs(a - b) <= kSameFloorMaxDy; }

// 拟人单层 MVP：选怪用紧同高；已锁追击用 chaseY（见 HumanWalkReachable）。
bool HumanWalkSamePlatform(float py, float my, float yTol = kSameLayerY) {
    return std::fabs(py - my) <= yTol;
}

// 解析玩家站姿 FH：CurFh 与 Snap 不一致时信 Snap（过期 CurFh 会假拒旁怪）。
// 这里不给 Snap 加距离闸：36541b 埋点实测（2min17s、46 次远 snap 采样）本处零命中——
// 人再悬空，脚下也总有台落在 kCoverYTol(80) 内，退化档轮不到。加闸只会平白拒掉合法 fh。
uint32_t ResolveHumanPlayerFh(float px, float py, uint32_t hint = 0) {
    float sx = 0.f, sy = 0.f;
    uint32_t snapFh = 0;
    const bool snapOk =
        ports::foothold_path::SnapStandAt(px, py, &sx, &sy, &snapFh, /*preferFlat=*/false) &&
        snapFh != 0;

    uint32_t cur = hint;
    if (!cur) cur = ports::foothold::PeekCurFhId();

    if (cur && snapOk) {
        if (cur == snapFh) return cur;
        if (ports::foothold_path::SameWalkComponent(cur, snapFh)) return cur;
        return snapFh;  // 错台 / 过期 CurFh
    }
    if (snapOk) return snapFh;
    if (cur) {
        float ox = 0.f, oy = 0.f;
        if (ports::foothold_path::SnapOnFh(cur, px, &ox, &oy, /*avoidWalkJunction=*/true) &&
            SameLayerY(py, oy))
            return cur;
    }
    return 0;
}

// 拟人可走达细分类：选怪紧同高；MoveTo 用 chaseY + 区分 Y 隙 / 断崖（BIN 01:07）。
enum class HumanWalkVerdict : uint8_t { Ok = 0, YGap, Cliff };

HumanWalkVerdict ClassifyHumanWalk(float px, float py, float mx, float my, float yTol,
                                   uint32_t playerFhHint = 0) {
    if (!HumanWalkSamePlatform(py, my, yTol)) return HumanWalkVerdict::YGap;
    if (!ports::foothold_path::EnsureGraph()) return HumanWalkVerdict::Ok;

    // 非 0 hint = 调用方已 ResolveHumanPlayerFh；避免每怪再 Snap+BFS。
    const uint32_t pfh = playerFhHint ? playerFhHint : ResolveHumanPlayerFh(px, py);
    if (!pfh) return HumanWalkVerdict::Ok;

    float msx = 0.f, msy = 0.f;
    uint32_t mfh = 0;
    if (!ports::foothold_path::SnapStandAt(mx, my, &msx, &msy, &mfh, /*preferFlat=*/false) ||
        !mfh)
        return HumanWalkVerdict::Ok;  // 贴不准：放行，交给 stall / timeout
    // 贴飞到另一层台：不可信，勿当断崖拒绝（怪心原坐标已过 yTol）。
    if (std::fabs(my - msy) > yTol) return HumanWalkVerdict::Ok;
    return ports::foothold_path::SameWalkComponent(pfh, mfh) ? HumanWalkVerdict::Ok
                                                            : HumanWalkVerdict::Cliff;
}

// BIN 00:52：preferFlat 把怪心贴飞 / Snap 失败曾 return false → acquire miss ok=5 却不锁旁怪。
bool HumanWalkReachable(float px, float py, float mx, float my, uint32_t playerFhHint = 0,
                        float yTol = kSameLayerY) {
    return ClassifyHumanWalk(px, py, mx, my, yTol, playerFhHint) == HumanWalkVerdict::Ok;
}

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
    // 与空中贴怪共用水平 X：勾「自定义站距」听用户；不勾用内置 60。
    // 地面落点再夹进 12–200，避免贴怪心或 hop 过远。
    float s;
    if (gStandOffCustom.load(std::memory_order_acquire)) {
        s = static_cast<float>(
            xcat::ClampCombatStandOffX(gStandOffX.load(std::memory_order_acquire)));
    } else {
        s = static_cast<float>(xcat::kCombatStandOffXDefault);
    }
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

bool HiraishinRangeOk(float px, float py, float mx, float my) {
    if (!gHiraishinEnabled.load(std::memory_order_acquire)) return true;
    const uint32_t r = gHiraishinRangePx.load(std::memory_order_acquire);
    float lim = static_cast<float>(r);
    const float cap = ports::mob_gather::GatherRadiusPx();
    if (lim <= 0.f || lim > cap) lim = cap;
    const float dx = mx - px;
    const float dy = my - py;
    return dx * dx + dy * dy <= lim * lim;
}

// 站桩输出「面前」：攻击盒（AbsPos 半宽/半高，面板横向/竖直滑条；0=该轴不限），含重叠。
// 出刀前 FaceToward；这里不要求朝向已经摆正。
bool HiraishinFrontOk(float px, float py, float mx, float my) {
    const float maxDx =
        static_cast<float>(gHiraishinFrontDx.load(std::memory_order_acquire));
    const float maxDy =
        static_cast<float>(gHiraishinFrontDy.load(std::memory_order_acquire));
    if (maxDx > 0.f && std::fabs(mx - px) > maxDx) return false;
    if (maxDy > 0.f && std::fabs(my - py) > maxDy) return false;
    return true;
}

void LatchCombatHeli(const char* why) {
    if (gHeliLatchedThisEnable) return;
    if (travel::IsActive()) return;
    gHeliLatchedThisEnable = true;
    LogLine("heli latch why=%s (impact armed for this F5 session)", why ? why : "?");
}

bool CombatStandMa(const ports::teleport::FlightState& st) {
    return st.ok && (st.ma == 4 || st.ma == 5);
}

// BAN 会摘 CurFh，人还能是站立动作。进 Firing 必须已经是跳/飞，否则 BAN+ma=4 出刀会崩，
// 只看 !onFh 会在台沿上空转 fire defer stand_fhban（BIN 15:24:51）。
bool CombatHeliAirborne() {
    ports::teleport::FlightState st{};
    if (!(ports::teleport::QueryFlightState(st) && st.ok && !st.onFh)) return false;
    if (CombatStandMa(st)) return false;
    return true;
}

bool HeliHeldByPeer() {
    const heli::Owner o = heli::CurrentOwner();
    return o == heli::Owner::Travel || o == heli::Owner::Fly || o == heli::Owner::Gather;
}

// 关 F5 / 软静默 / 赶路抢主：卸 CombatImpact。交战期不走这里落地砍（build 132：空中砍）。
void UnlatchCombatHeli(const char* why) {
    gHeliLatchedThisEnable = false;
    // 赶路/F6/吸怪寻簇占着旋翼时只清 F5 latch，禁止 Disarm/卸 CombatImpact。
    // BIN 17:36:08：贴门超时后换图 lost_session，unlatch 打进 Travel 窗口。
    if (travel::IsActive() || HeliHeldByPeer()) {
        LogLine("heli unlatch why=%s skip_rotor (peer owns)", why ? why : "?");
        return;
    }
    if ((ports::fly_fh_ban::ActiveMask() &
         static_cast<unsigned>(ports::fly_fh_ban::BanSource::CombatImpact)) == 0) {
        LogLine("heli unlatch why=%s (latch only)", why ? why : "?");
        return;
    }
    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
    heli::Disarm(heli::Owner::Combat);
    LogLine("heli unlatch why=%s (session end; BAN dropped)", why ? why : "?");
}

bool TryEnterMoveTo(DWORD now, const char* why) {
    // 站桩输出原地出刀，不走路、不滑翔；怪由叠怪吸过来。
    if (gHiraishinEnabled.load(std::memory_order_acquire)) return false;
    // Impact / 拟人：不查瞬移 NativeCD（速率在 MoveTo 内自管）。
    if (gImpactApproachEnabled.load(std::memory_order_acquire) ||
        gHumanWalkEnabled.load(std::memory_order_acquire)) {
        if (gImpactApproachEnabled.load(std::memory_order_acquire)) {
            LatchCombatHeli(why ? why : "MoveTo");
        }
        EnterState(State::MoveTo, now, why);
        return true;
    }
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

// 拟人出带迟滞：进带用紧口径，Recover 用宽口径，避免带边 MoveTo↔Aim 自激打 StopWalk。
constexpr float kHumanBandExitSlackPx = 36.f;
bool InHumanHoldBand(float playerX, float playerY, float mobX, float mobY, float standOff) {
    if (!SameLayer(playerX, playerY, mobX, mobY)) return false;
    const float dx = std::fabs(mobX - playerX);
    if (dx < kMinLandAway) return false;
    return dx <= standOff * kHitBandMaxFrac + kHumanBandExitSlackPx;
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
bool HeliStrikeOk(float px, float py, float mx, float my, bool firstLock = false);  // 定义见下
bool TryConsumeWhiffApproachCorrect(DWORD now, float playerX, float playerY, float standOff,
                                    bool canApproach, bool* outMoved) {
    if (outMoved) *outMoved = false;
    if (!gLock.needApproachCorrect) return false;
    // 站桩输出下一刀会闪到怪脚边，禁止把纠偏走成 MoveTo 把出刀卡住。
    if (gHiraishinEnabled.load(std::memory_order_acquire)) {
        gLock.needApproachCorrect = false;
        return false;
    }
    // Impact：已在紧出刀带 → 清 flag，禁止假重贴。
    // BIN：Recover→MoveTo whiff_reapproach→Firing 再等 ~300ms 间隔，体感贴着愣一下。
    if (gImpactApproachEnabled.load(std::memory_order_acquire) &&
        HeliStrikeOk(playerX, playerY, gLock.x, gLock.y, /*firstLock=*/false)) {
        gLock.needApproachCorrect = false;
        return false;
    }
    if (InHitBand(playerX, playerY, gLock.x, gLock.y, standOff)) {
        gLock.needApproachCorrect = false;
        return false;
    }
    if (!canApproach) {
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
// 拟人：只用真命中带（禁 dx<100 续砍门），逼走路贴近 standOff。
bool FireGateOk(float playerX, float playerY, float mobX, float mobY, float standOff, DWORD now,
                const char* where) {
    if (InHitBand(playerX, playerY, mobX, mobY, standOff)) return true;
    // 拟人要贴到真命中带；Impact/瞬移允许 melee hold 续砍。
    const bool humanOnly = !gImpactApproachEnabled.load(std::memory_order_acquire) &&
                           gHumanWalkEnabled.load(std::memory_order_acquire);
    if (!humanOnly && gLock.id && InMeleeHoldBand(playerX, playerY, mobX, mobY, standOff))
        return true;
    static DWORD sLog = 0;
    if (!sLog || now - sLog > 400) {
        sLog = now;
        LogLine("fire gate where=%s id=%d dx=%.0f human=%d — await_band", where ? where : "?",
                gLock.id, mobX - playerX, humanOnly ? 1 : 0);
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

bool TryAbandonHitRotate(DWORD now, const ports::mob::Snapshot& snap);
bool TryAbandonTeleportOneHit(DWORD now);

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
        ClearLockRetarget();
        return false;
    }
    gLock.ptr = live.ptr;
    gLock.x = live.x;
    gLock.y = live.y;
    gLock.lastHp = live.hpPct;
    gLock.lastHitted = live.lastHitted;
    gLock.inView = live.inView;
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
    NoteLockHpSample();
    NoteHittedForHitLag(now);
    MaybeProbeUiHpTag(now);
    // DI 偶发晚一拍：bump 当帧没读到，锁刷新再试。站桩也要跳过 MISS 怪。
    ResolveAccMissAfterBump(now, "lock");
    if (TryAbandonAccMiss(now)) return false;
    if (gHiraishinEnabled.load(std::memory_order_acquire)) return true;
    if (TryAbandonDealtSum(now)) return false;
    if (TryAbandonOneshot(now)) return false;
    if (TryAbandonAbsHp(now)) return false;
    if (TryAbandonHitLag(now)) return false;
    if (TryAbandonTeleportOneHit(now)) return false;
    if (TryAbandonHitRotate(now, snap)) return false;
    if (TryAbandonLockKillTimeout(now)) return false;

    if (!ResolveWhiffArm(now)) return false;
    return !TryAbandonAccMiss(now);
}

// 贴怪关：只锁同层近怪。贴怪开：优先同 zMass 可落点；同层没有才跨 zMass（防飞怪物池）。
// 群怪优先开：500px 内先比密度（geoD2，不含 inView）；半径外仍按距离。
// 空中贴怪允许为了半径内的密堆跨层。MobCtrl：软优先我方控 > 中性 > 他人驱动。
constexpr float kClusterRadiusPx = 250.f;
// 半径内密度优先：脚边独怪不得压过 500px 内的堆。
// 旧 2x 相对距（score 含 inView=2.5e6）让 40px 独怪永远赢 200px 密堆，客户体感「勾了没用」。
constexpr float kClusterPreferPx = 500.f;
constexpr float kClusterPreferR2 = kClusterPreferPx * kClusterPreferPx;
constexpr int kClusterPackMin = 3;

bool MobIsLiveFarm(const ports::mob::MobLite& m) {
    if (!m.ready || m.deadType != 0 || m.hpPct <= 0) return false;
    if (m.templateId == kSpecialTplFilter) return false;
    return true;
}

int CountLiveFarmMobs(const ports::mob::Snapshot& snap) {
    int n = 0;
    for (int i = 0; i < snap.count; ++i) {
        if (MobIsLiveFarm(snap.mobs[i])) ++n;
    }
    return n;
}

void ClearHitRotatePending() {
    gHitRotatePending = false;
    gHitRotateFromId = 0;
}

void ClearHitRotateWatches() {
    gHitRotateWatchN = 0;
    gHitRotateObserveUntil = 0;
    memset(gHitRotateWatch, 0, sizeof(gHitRotateWatch));
}

void ClearHitRotateState() {
    ClearHitRotatePending();
    ClearHitRotateWatches();
}

void SyncHitPinWish() {
    const int32_t oid =
        (HitRotateFarmActive() && gLock.id > 0) ? gLock.id : 0;
    ports::hit_pin::SetWishOid(oid);
}

HitRotateWatch* HitRotateWatchOf(int id) {
    if (id <= 0) return nullptr;
    for (int i = 0; i < gHitRotateWatchN; ++i) {
        if (gHitRotateWatch[i].id == id) return &gHitRotateWatch[i];
    }
    if (gHitRotateWatchN >= kHitRotateWatchCap) {
        int vic = 0;
        for (int i = 1; i < gHitRotateWatchN; ++i) {
            if (gHitRotateWatch[i].hits < gHitRotateWatch[vic].hits) vic = i;
        }
        gHitRotateWatch[vic] = HitRotateWatch{};
        gHitRotateWatch[vic].id = id;
        return &gHitRotateWatch[vic];
    }
    HitRotateWatch& w = gHitRotateWatch[gHitRotateWatchN++];
    w = HitRotateWatch{};
    w.id = id;
    return &w;
}

// lastHitted 上升对应最新一条 AddDamageInfo。只认末条 CharacterId==本角色；
// 列表里残留的旧自己条目不算（否则别人刚打一刀也会被当成我们的）。
// Unknown：列表还没写上 / charId 读不到 —— 不得把 lastHitted 推上去，否则下一拍没 bump 可计。
enum class HitRotateBumpKind : uint8_t { Ours, Foreign, Unknown };

HitRotateBumpKind ClassifyHitRotateBump(void* mob, uint32_t me) {
    if (!me || !mob) return HitRotateBumpKind::Unknown;
    ports::mob::DamageInfoSnap di{};
    if (!ports::mob::TryReadDamageInfoList(mob, di) || !di.ok || di.count <= 0) {
        return HitRotateBumpKind::Unknown;
    }
    return di.items[di.count - 1].charId == me ? HitRotateBumpKind::Ours
                                               : HitRotateBumpKind::Foreign;
}

bool HitRotateQuotaFull() {
    if (!HitRotateFarmActive()) return false;
    const int nNeed = gHitRotateN.load(std::memory_order_acquire);
    if (nNeed <= 0 || !gLock.id) return false;
    // 出手硬顶：lastHitted 卡住时 confirm/bump 都不涨（BIN 3716395 lf=19 hp 钉死），
    // 不按 lockFires 收闸就会在同一只上无限挥。
    if (gLock.lockFires >= nNeed) return true;
    if (gLock.hitBumpCount >= nNeed) return true;
    for (int i = 0; i < gHitRotateWatchN; ++i) {
        if (gHitRotateWatch[i].id == gLock.id && gHitRotateWatch[i].hits >= nNeed) return true;
    }
    return false;
}

bool TriggerHitRotateFrom(int id, float x, float y, int hits, int live, DWORD now,
                          const char* why) {
    if (id <= 0) return false;
    gHitRotateFromX = x;
    gHitRotateFromY = y;
    gHitRotateFromId = id;
    gHitRotatePending = true;
    gHitRotateObserveUntil = 0;
    SoftBanFor(id, now, kHitRotateBanMs, kBanHitRotate);
    if (HitRotateWatch* w = HitRotateWatchOf(id)) {
        // 禁止清零：BIN 3883505 满 4 后贴身 off_lock 又从 1/4 再打一轮。
        if (w->hits < hits) w->hits = hits;
        w->spentUntil = now + kHitRotateBanMs;
    }
    LogLine("switch reason=hit_rotate id=%d hits=%d n=%d live=%d lock=%d pos=(%.0f,%.0f) why=%s",
            id, hits, gHitRotateN.load(std::memory_order_acquire), live, gLock.id, x, y,
            why ? why : "?");
    gLastLockLostWhy = "hit_rotate";
    ClearLockRetarget();
    return true;
}

// 扫全场 lastHitted 上升沿。确认命中仍要观察窗 + 末条 DI.charId==自己。
// 窗外 / DI 未就绪：禁止把 watch.lastHitted 推上去，否则延迟命中被吃掉后永远不计刀。
bool NoteHitRotateBumps(DWORD now, const ports::mob::Snapshot& snap) {
    if (!HitRotateFarmActive()) return false;
    const int nNeed = gHitRotateN.load(std::memory_order_acquire);
    const uint32_t me = ports::world::GetCharacterId();
    const bool inWin =
        gHitRotateObserveUntil != 0 && static_cast<int>(now - gHitRotateObserveUntil) < 0;
    for (int i = 0; i < gHitRotateWatchN; ++i) gHitRotateWatch[i].seen = false;

    int rotateId = 0;
    float rotateX = 0.f, rotateY = 0.f;
    int rotateHits = 0;

    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!MobIsLiveFarm(m) || m.id <= 0) continue;
        HitRotateWatch* w = HitRotateWatchOf(m.id);
        if (!w) continue;
        w->seen = true;
        if (w->spentUntil != 0 && static_cast<int>(now - w->spentUntil) >= 0) {
            w->hits = 0;
            w->spentUntil = 0;
        }
        int32_t hitted = m.lastHitted;
        if (m.id == gLock.id) hitted = gLock.lastHitted;  // 锁怪用 live 直读，不吃 snap 龄
        if (!w->seeded) {
            w->lastHitted = hitted;
            w->seeded = true;
            continue;
        }
        const bool bump = hitted > w->lastHitted;
        if (!bump) continue;
        // 已满 N 禁锁：迟到多段/投射物只吞掉 bump，禁止 spent_rehit 拆掉新锁。
        if (w->spentUntil != 0 && static_cast<int>(now - w->spentUntil) < 0) {
            w->lastHitted = hitted;
            continue;
        }
        if (!inWin) continue;  // 保留旧 lastHitted，等窗开再计
        const HitRotateBumpKind kind = ClassifyHitRotateBump(m.ptr, me);
        if (kind != HitRotateBumpKind::Ours) {
            if (kind == HitRotateBumpKind::Foreign) w->lastHitted = hitted;
            static DWORD sSkip = 0;
            if (!sSkip || now - sSkip > 1000) {
                sSkip = now;
                LogLine("hit_rotate skip_%s id=%d hitted=%d me=%u",
                        kind == HitRotateBumpKind::Foreign ? "foreign" : "di", m.id, hitted, me);
            }
            continue;
        }
        w->lastHitted = hitted;
        w->hits += 1;
        LogLine("hit_rotate confirm id=%d hits=%d/%d lock=%d hitted=%d me=%u", m.id, w->hits, nNeed,
                gLock.id, hitted, me);
        if (w->hits >= nNeed && rotateId == 0) {
            rotateId = m.id;
            rotateX = m.x;
            rotateY = m.y;
            rotateHits = w->hits;
        }
    }

    if (rotateId <= 0) return false;
    const int live = CountLiveFarmMobs(snap);
    return TriggerHitRotateFrom(rotateId, rotateX, rotateY, rotateHits, live, now,
                                rotateHits > nNeed ? "spent_rehit"
                                : (rotateId == gLock.id ? "lock" : "off_lock"));
}

// 瞬移找怪「每只怪打一下」：当前锁成功出一刀（lockFires≥1）即禁锁，下一拍走原选怪。
// 不设活怪<3；不走 PickHitRotateTarget（盒外最近）。空挥也切——用户要的是「打一下就换」。
bool TryAbandonTeleportOneHit(DWORD now) {
    if (!TeleportOneHitActive()) return false;
    if (!gLock.id) return false;
    if (gLock.lockFires < 1) return false;
    SoftBanFor(gLock.id, now, kTeleportOneHitBanMs, kBanHitRotate);
    LogLine("switch reason=tp_one_hit id=%d fires=%d bumps=%d ban=%ums", gLock.id,
            gLock.lockFires, gLock.hitBumpCount, (unsigned)kTeleportOneHitBanMs);
    gLastLockLostWhy = "tp_one_hit";
    ClearLockRetarget();
    return true;
}

// 打中换怪：活怪 < 3 停刀；出手满 N / lastHitted 满 N / 确认命中满 N 则禁锁换盒外。
// 瞬移「每只怪打一下」开启时本路径让路（无活怪<3、不走盒外选怪）。
bool TryAbandonHitRotate(DWORD now, const ports::mob::Snapshot& snap) {
    if (!HitRotateFarmActive()) return false;
    const int nNeed = gHitRotateN.load(std::memory_order_acquire);
    if (snap.ok) {
        const int live = CountLiveFarmMobs(snap);
        if (live < kHitRotateMinLive) {
            LogLine("switch reason=hit_rotate_low_live id=%d live=%d hits_armed=%d need>=%d",
                    gLock.id, live, gLock.hitBumpCount, kHitRotateMinLive);
            ClearHitRotateState();
            gLastLockLostWhy = "hit_rotate_low_live";
            ClearLockRetarget();
            return true;
        }
    }
    if (gLock.id > 0 && nNeed > 0) {
        const int live = snap.ok ? CountLiveFarmMobs(snap) : 0;
        if (gLock.hitBumpCount >= nNeed) {
            return TriggerHitRotateFrom(gLock.id, gLock.x, gLock.y, gLock.hitBumpCount, live, now,
                                        "lock_bumps");
        }
        if (gLock.lockFires >= nNeed) {
            return TriggerHitRotateFrom(gLock.id, gLock.x, gLock.y, gLock.lockFires, live, now,
                                        "lock_fires");
        }
    }
    if (!snap.ok) return false;
    return NoteHitRotateBumps(now, snap);
}

// Count = self + same-layer live mobs within 250px (min 1).
int CountClusterNeighbors(const ports::mob::Snapshot& snap, const ports::mob::MobLite& center) {
    const float r2 = kClusterRadiusPx * kClusterRadiusPx;
    int n = 1;  // self
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

void AssignLockFromMob(const ports::mob::MobLite& m, float px, float py) {
    gLock.ptr = m.ptr;
    gLock.id = m.id;
    gLock.lastHp = m.hpPct;
    gLock.lastHitted = m.lastHitted;
    gLock.whiff = 0;
    ClearWhiffArm();
    gLock.x = m.x;
    gLock.y = m.y;
    gLock.inView = m.inView;
    gInViewHoldSince = 0;
    gLock.templateId = m.templateId;
    // 绝对血 / mob_stats 查表：仅脆皮早切面板开着时锁怪查一次。
    // 日常出刀只靠 hpPct；BIN 曾每锁 src=pct + abshp 早切，用户不用早切则整段跳过。
    gLock.maxHp = 0;
    gLock.absHp = 0;
    gLock.absSrc = ports::mob::AbsHpSrc::None;
    if (gOneshotMaxHp.load(std::memory_order_acquire) > 0) {
        ports::mob::AbsHp abs{};
        if (ports::mob::ResolveAbsHp(m.id, m.templateId, m.hpPct, abs, /*refreshUi=*/false) &&
            abs.ok) {
            gLock.maxHp = abs.max;
            gLock.absHp = abs.cur;
            gLock.absSrc = abs.src;
        } else {
            gLock.maxHp = ports::mob::LookupTemplateMaxHp(m.templateId);
            gLock.absHp = AbsHpFromPct(gLock.maxHp, m.hpPct);
            gLock.absSrc = gLock.maxHp > 0 ? ports::mob::AbsHpSrc::PctEstimate
                                          : ports::mob::AbsHpSrc::None;
        }
    }
    gLock.lockStartHp = m.hpPct;
    gLock.prevHittedSample = m.lastHitted;
    gLock.hitBumpCount = 0;
    gLock.firstBumpMs = 0;
    gLock.lockFires = 0;
    gLock.firstFireMs = 0;
    gLock.lastKillProgressMs = 0;
    gLock.killProgressHp = m.hpPct;
    gLock.killTimeoutHoldSince = 0;
    gLock.predictSampleHp = m.hpPct;
    gLock.predictFiresAtHp = 0;
    gLock.predictDropEvents = 0;
    gLock.dealtSum = 0;
    gLock.dealtHits = 0;
    gLock.lastDealt = 0;
    gLock.stickyFixes = 0;
    gLock.lastStickyCorrectMs = 0;
    gLock.needApproachCorrect = false;
    gLandSide = (px >= m.x) ? 1 : -1;
    gStandstillSince = 0;
    gStandstillShuffleLast = 0;
    gStandstillAnchorX = px;
    gStandstillAnchorY = py;
}

// 拟人 MoveTo：夹在自己与锁怪之间、已进命中带的同层怪 → 换锁顺手砍。
// BIN：交手中 / 近距叠怪禁止再切；取消「任意已进带」回退，避免 182609↔657474 乒乓。
bool TryHumanPassbyRetarget(const ports::mob::Snapshot& snap, float px, float py, float standOff,
                            DWORD now) {
    if (HitRotateFarmActive()) return false;
    if (TeleportOneHitActive()) return false;
    if (!gLock.id) return false;
    if (LockHasEngaged() || gLock.lockFires > 0) return false;
    if (gLastHumanPassbyMs && now - gLastHumanPassbyMs < kHumanPassbyCooldownMs) return false;

    const float chaseDx = gLock.x - px;
    if (!std::isfinite(chaseDx) || std::fabs(chaseDx) < 1.f) return false;
    const int chaseSign = (chaseDx < 0.f) ? -1 : 1;
    const float chaseAbs = std::fabs(chaseDx);
    // 锁怪已进入/贴近命中带：叠怪堆里勿再 passby。
    const float nearLock = standOff * kHitBandMaxFrac + 8.f;
    if (chaseAbs <= nearLock) return false;

    const ports::mob::MobLite* along = nullptr;
    float alongAbs = 1e9f;

    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
        if (m.templateId == kSpecialTplFilter) continue;
        if (m.id == gLock.id) continue;
        if (IsSoftBanned(m.id, now)) continue;
        if (!HumanWalkReachable(px, py, m.x, m.y)) continue;
        if (!InHitBand(px, py, m.x, m.y, standOff)) continue;
        const float dx = m.x - px;
        if (!std::isfinite(dx)) continue;
        const float adx = std::fabs(dx);
        const int sign = (dx < 0.f) ? -1 : 1;
        if (sign != chaseSign) continue;
        if (adx >= chaseAbs) continue;  // 须夹在途中，更近才切
        if (adx < alongAbs) {
            alongAbs = adx;
            along = &m;
        }
    }

    if (!along) return false;
    const int oldId = gLock.id;
    AssignLockFromMob(*along, px, py);
    gLastHumanPassbyMs = now;
    LogLine("MoveTo human_passby id=%d→%d dx=%.0f chaseDx=%.0f along=1", oldId, along->id,
            along->x - px, chaseDx);
    return true;
}

bool PickNearestTarget(const ports::mob::Snapshot& snap, float px, float py, DWORD now,
                       bool allowCrossLayer, bool looseLand) {
    const float standOff = ClampStandOff();
    const bool clusterOn = gClusterPriority.load(std::memory_order_acquire);
    const ports::mob::MobLite* best = nullptr;
    float bestScore = 1e9f;
    float bestGeoD2 = 1e9f;
    float bestHop = 0.f;
    int bestCluster = -1;
    int bestCtrlRank = -1;
    int32_t playerZm = 0;
    const bool playerZmOk =
        kDirectTeleportNoLayerHop ? false : TryPlayerZMass(px, py, &playerZm);
    // 拟人选怪：玩家 FH 只解析一次，避免每只怪重复 Snap。
    const bool humanPick =
        !allowCrossLayer && gHumanWalkEnabled.load(std::memory_order_acquire);
    uint32_t humanPlayerFh = 0;
    if (humanPick) humanPlayerFh = ResolveHumanPlayerFh(px, py);

    // 群怪优先：500px 内密度是第一键（geoD2，不含 inView/残血偏置）。
    // 旧 2x 相对距把 score（含 inView=2.5e6）拿来比，40px 独怪永远压过 200px 堆。
    // 半径外仍按距离，避免再出现 BIN 22:47 追 600px+ 密堆发呆。
    auto better = [&](int ctrlRank, int clusterN, float geoD2, float score) -> bool {
        if (ctrlRank > bestCtrlRank) return true;
        if (ctrlRank < bestCtrlRank) return false;
        if (clusterOn && bestCluster >= 0) {
            const bool candIn = geoD2 <= kClusterPreferR2;
            const bool bestIn = bestGeoD2 <= kClusterPreferR2;
            if (candIn && bestIn) {
                if (clusterN != bestCluster) return clusterN > bestCluster;
                return score < bestScore;
            }
            if (candIn && clusterN >= kClusterPackMin && !bestIn) return true;
            if (bestIn && bestCluster >= kClusterPackMin && !candIn) return false;
        }
        return score < bestScore;
    };

    const bool hirPick = gHiraishinEnabled.load(std::memory_order_acquire);
    if (hirPick) {
        (void)looseLand;
        (void)allowCrossLayer;
        (void)standOff;
        (void)now;
        for (int i = 0; i < snap.count; ++i) {
            const auto& m = snap.mobs[i];
            if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
            if (m.templateId == kSpecialTplFilter) continue;
            if (!HiraishinFrontOk(px, py, m.x, m.y)) continue;
            const float dx = m.x - px;
            const float dy = m.y - py;
            const float geoD2 = dx * dx + dy * dy;
            if (geoD2 >= bestGeoD2) continue;
            bestGeoD2 = geoD2;
            bestScore = geoD2;
            best = &m;
            bestHop = std::hypot(dx, dy);
        }
        if (!best) return false;
    } else if (kDirectTeleportNoLayerHop) {
        for (int i = 0; i < snap.count; ++i) {
            const auto& m = snap.mobs[i];
            if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
            if (m.templateId == kSpecialTplFilter) continue;
            if (IsSoftBanned(m.id, now)) continue;
            if (!HiraishinRangeOk(px, py, m.x, m.y)) continue;
            float hop = 0, tx = 0, ty = 0;
            uint32_t fh = 0;
            const bool landOk =
                looseLand ? EstimateLandPrefer(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh)
                          : EstimateLand(px, py, m.x, m.y, standOff, &hop, &tx, &ty, &fh);
            if (!landOk || !std::isfinite(hop) || hop < 0.f) continue;
            const float dx = m.x - px;
            const float dy = m.y - py;
            const float geoD2 = dx * dx + dy * dy;
            const float woundBias = (m.hpPct > 0 && m.hpPct < 100) ? -80000.f : 0.f;
            // inView=0 仍可选（避免假空图），但有同距可命中怪时优先后者。
            const float inViewBias = m.inView ? 0.f : 2.5e6f;
            const float score = geoD2 + woundBias + inViewBias;
            const int cn = clusterOn ? CountClusterNeighbors(snap, m) : 0;
            const int cr = ports::mob::CtrlPreferRank(m.ctrl);
            if (!better(cr, cn, geoD2, score)) continue;
            bestCtrlRank = cr;
            bestCluster = cn;
            bestScore = score;
            bestGeoD2 = geoD2;
            best = &m;
            bestHop = hop;
        }
        if (!best) return false;
    } else {
        auto consider = [&](const ports::mob::MobLite& m, bool sameLayerPass) {
            if (!m.ready || m.deadType != 0 || m.hpPct <= 0) return;
            if (m.templateId == kSpecialTplFilter) return;
            if (IsSoftBanned(m.id, now)) return;
            if (!HiraishinRangeOk(px, py, m.x, m.y)) return;

            if (!allowCrossLayer) {
                if (!SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, 0)) return;
                // 拟人：同高 + Walk 连通（拒绝同 zMass 错台 / 同高断崖）。
                if (humanPick && !HumanWalkReachable(px, py, m.x, m.y, humanPlayerFh)) return;
                if (!sameLayerPass) return;
                const float dx = m.x - px;
                const float dy = m.y - py;
                const float geoD2 = dx * dx + dy * dy;
                const float inViewBias = m.inView ? 0.f : 2.5e6f;
                const float d2 =
                    geoD2 + ((m.hpPct > 0 && m.hpPct < 100) ? -80000.f : 0.f) + inViewBias;
                const int cn = clusterOn ? CountClusterNeighbors(snap, m) : 0;
                const int cr = ports::mob::CtrlPreferRank(m.ctrl);
                if (!better(cr, cn, geoD2, d2)) return;
                bestCtrlRank = cr;
                bestCluster = cn;
                bestScore = d2;
                bestGeoD2 = geoD2;
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
            if (hop > MaxApproachHopPx()) {
                if (!CanApproachWithinMaxHop(px, py, tx, ty, MaxApproachHopPx())) return;
            }
            const bool same = SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, fh);
            if (sameLayerPass) {
                if (!same) return;
            } else {
                if (same) return;
            }

            const float dx = m.x - px;
            const float dy = m.y - py;
            const float geoD2 = dx * dx + dy * dy;
            const float woundBias = (m.hpPct > 0 && m.hpPct < 100) ? -80000.f : 0.f;
            const float inViewBias = m.inView ? 0.f : 2.5e6f;
            // 落地闸远近同价；选怪仍先清同层近的，避免整图乱跳体感变慢。
            const float score = geoD2 + woundBias + inViewBias;
            const int cn = clusterOn ? CountClusterNeighbors(snap, m) : 0;
            const int cr = ports::mob::CtrlPreferRank(m.ctrl);
            if (!better(cr, cn, geoD2, score)) return;
            bestCtrlRank = cr;
            bestCluster = cn;
            bestScore = score;
            bestGeoD2 = geoD2;
            best = &m;
            bestHop = hop;
        };

        for (int i = 0; i < snap.count; ++i) consider(snap.mobs[i], /*sameLayerPass=*/true);
        if (allowCrossLayer) {
            // 群怪：同层散怪也跟跨层密堆比密度。未开群怪：同层有怪就不跨层。
            if (clusterOn || !best) {
                if (!clusterOn) {
                    bestScore = 1e9f;
                    bestGeoD2 = 1e9f;
                    bestCluster = -1;
                    bestCtrlRank = -1;
                }
                for (int i = 0; i < snap.count; ++i)
                    consider(snap.mobs[i], /*sameLayerPass=*/false);
            }
        }
        if (!best) return false;
    }

    AssignLockFromMob(*best, px, py);
    LogLine("acquire id=%d tpl=%d hp=%d%% maxHp=%lld abs=%lld src=%s ctrl=%d(%s) rank=%d "
            "pos=(%.0f,%.0f) d=(%.0f,%.0f) layer=%s hop~%.0f side=%d cluster=%d loose=%d iv=%d",
            best->id, best->templateId, best->hpPct, static_cast<long long>(gLock.maxHp),
            static_cast<long long>(gLock.absHp), ports::mob::AbsHpSrcName(gLock.absSrc), best->ctrl,
            ports::mob::CtrlName(best->ctrl), bestCtrlRank, best->x, best->y, best->x - px,
            best->y - py, SameLayer(px, py, best->x, best->y) ? "same" : "cross", bestHop,
            gLandSide, clusterOn ? bestCluster : -1, looseLand ? 1 : 0, best->inView ? 1 : 0);
    return true;
}

bool HitRotateOutsideClear(float mx, float my, float fromX, float fromY) {
    return std::fabs(mx - fromX) >= kHitRotateClearDx ||
           std::fabs(my - fromY) >= kHitRotateClearDy;
}

// 打中换怪：禁刚命中的 oid 后，选「攻击盒外、离玩家最近」的活怪。
// 空中贴怪不跑 hop/落点过滤；拟人/站桩仍受同层与走路可达约束。
// 盒内全挤在一起时才退回离 from 最远，避免下一刀还打进刚满 N 的那只。
bool PickHitRotateTarget(const ports::mob::Snapshot& snap, float fromX, float fromY, int excludeId,
                         float px, float py, DWORD now, bool allowCrossLayer) {
    const ports::mob::MobLite* bestClear = nullptr;
    float bestClearD2 = 0.f;
    const ports::mob::MobLite* bestFar = nullptr;
    float bestFarD2 = -1.f;
    int32_t playerZm = 0;
    const bool playerZmOk = TryPlayerZMass(px, py, &playerZm);
    const bool humanPick =
        !allowCrossLayer && gHumanWalkEnabled.load(std::memory_order_acquire);
    uint32_t humanPlayerFh = 0;
    if (humanPick) humanPlayerFh = ResolveHumanPlayerFh(px, py);

    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!MobIsLiveFarm(m)) continue;
        if (m.id == excludeId) continue;
        if (IsSoftBanned(m.id, now)) continue;
        if (!HiraishinRangeOk(px, py, m.x, m.y)) continue;
        if (!allowCrossLayer) {
            if (!SameLayerZm(playerZm, playerZmOk, py, m.x, m.y, 0)) continue;
            if (humanPick && !HumanWalkReachable(px, py, m.x, m.y, humanPlayerFh)) continue;
        }
        const float dxFrom = m.x - fromX;
        const float dyFrom = m.y - fromY;
        const float d2From = dxFrom * dxFrom + dyFrom * dyFrom;
        if (!bestFar || d2From > bestFarD2) {
            bestFar = &m;
            bestFarD2 = d2From;
        }
        if (!HitRotateOutsideClear(m.x, m.y, fromX, fromY)) continue;
        const float dxP = m.x - px;
        const float dyP = m.y - py;
        const float d2P = dxP * dxP + dyP * dyP;
        if (!bestClear || d2P < bestClearD2) {
            bestClear = &m;
            bestClearD2 = d2P;
        }
    }
    const ports::mob::MobLite* best = bestClear ? bestClear : bestFar;
    if (!best) return false;
    const char* via = bestClear ? "clear" : "fallback_far";
    const float dFrom = std::sqrt((best->x - fromX) * (best->x - fromX) +
                                  (best->y - fromY) * (best->y - fromY));

    AssignLockFromMob(*best, px, py);
    LogLine("acquire hit_rotate %s id=%d fromId=%d tpl=%d hp=%d%% pos=(%.0f,%.0f) "
            "from=(%.0f,%.0f) distFrom=%.0f dPlayer=(%.0f,%.0f) live=%d",
            via, best->id, excludeId, best->templateId, best->hpPct, best->x, best->y, fromX,
            fromY, dFrom, best->x - px, best->y - py, CountLiveFarmMobs(snap));
    return true;
}

void ExplainAcquireMiss(const ports::mob::Snapshot& snap, float px, float py, DWORD now,
                        bool allowCrossLayer) {
    if (gHiraishinEnabled.load(std::memory_order_acquire)) {
        (void)now;
        int nSpecial = 0, nDead = 0, nFront = 0, nSame = 0, nCross = 0, nOk = 0;
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
            if (!HiraishinFrontOk(px, py, m.x, m.y)) {
                ++nFront;
                continue;
            }
            if (SameLayer(px, py, m.x, m.y)) {
                ++nSame;
            } else {
                ++nCross;
            }
            ++nOk;
        }
        LogLine("acquire miss count=%d py=%.0f mode=hiraishin-front frontOut=%d "
                "same=%d cross=%d dead=%d special=%d ok=%d",
                snap.count, py, nFront, nSame, nCross, nDead, nSpecial, nOk);
        return;
    }
    const float standOff = ClampStandOff();
    const bool humanOn = gHumanWalkEnabled.load(std::memory_order_acquire);
    int nBan = 0, nSpecial = 0, nDead = 0, nNoLand = 0, nHopFar = 0, nSame = 0, nCross = 0,
        nOk = 0;
    int nSnap = 0, nLayer = 0, nFhBan = 0, nSides = 0, nUnsafe = 0, nFar = 0;
    int nHumanWalk = 0, nHumanOk = 0;
    int nSample = 0;
    uint32_t humanPlayerFh = 0;
    if (humanOn && !allowCrossLayer) humanPlayerFh = ResolveHumanPlayerFh(px, py);
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
                                                                      /*preferFlat=*/false);
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
        if (!kDirectTeleportNoLayerHop && std::isfinite(hop) && hop > MaxApproachHopPx()) {
            if (!CanApproachWithinMaxHop(px, py, tx, ty, MaxApproachHopPx())) {
                ++nHopFar;
                continue;
            }
        }
        const bool same = SameLayer(px, py, m.x, m.y);
        if (same) {
            ++nSame;
        } else {
            ++nCross;
            if (!allowCrossLayer && !kDirectTeleportNoLayerHop) continue;
        }
        ++nOk;
        if (humanOn && !allowCrossLayer) {
            if (!HumanWalkReachable(px, py, m.x, m.y, humanPlayerFh)) {
                ++nHumanWalk;
                if (nHumanWalk <= 2) {
                    LogLine("human reject sample id=%d d=(%.0f,%.0f) pfh=%u", m.id, m.x - px,
                            m.y - py, (unsigned)humanPlayerFh);
                }
                continue;
            }
            ++nHumanOk;
        }
    }
    LogLine(
        "acquire miss count=%d py=%.0f mode=%s ban=%d noLand=%d(snap=%d layer=%d far=%d fhban=%d "
        "sides=%d unsafe=%d) hopFar=%d same=%d cross=%d dead=%d special=%d ok=%d softN=%d "
        "humanWalk=%d humanOk=%d",
        snap.count, py,
        kDirectTeleportNoLayerHop ? "direct"
                                  : (allowCrossLayer ? "any-landable" : "same-layer"),
        nBan, nNoLand, nSnap, nLayer, nFar, nFhBan, nSides, nUnsafe, nHopFar, nSame, nCross, nDead,
        nSpecial, nOk, gSoftBanN, nHumanWalk, nHumanOk);
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
        x::features::ports::mob_gather::RecordHomeOnF5();
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

// 裸 F11：原 fill+Doing 贴怪已禁用（封禁风险）。
void PollF11NativeMob() {
    // 仍采样键态，避免下次若恢复时边沿粘住；不开火。
    const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11 && !gF11WasDown) {
        static DWORD sLastRefuseMs = 0;
        const DWORD now = GetTickCount();
        if (!sLastRefuseMs || now - sLastRefuseMs > 2000) {
            sLastRefuseMs = now;
            LogLine("F11 refused (native fill+Doing disabled)");
        }
    }
    gF11WasDown = f11;
}

// ── 直升机 B 层：飞控指令 ────────────────────────────────────────────
// 悬停点取「怪心 ±standOff、与怪同高」。**+Y 向上（见 heli_rotor.h）**：
// 曾经写成 `my - lift` 把悬停点压到怪下方，造成「悬停在怪物底下空刀」（BIN 79947e）。
//
// 抬升量是 **0**，而且现在有实测证明它已经是最优，别再动。
//
// 历史上给 +14 是为了补重力锯齿下沉，A 层改成「重力前馈 + 周期均速闭环」后锯齿残余只剩
// 约 4px，那 14px 就纯变成上偏，叠加死区残差后稳定悬在怪上方 32px 砍空气（BIN 14a58c）。
//
// ★★ 反向也不行，而且原因**不是对称的**（BIN cf0e5d）。曾按「抵消死区上偏」把它设成
//    −12：稳态残差实测中位 +12（正好一个 kDeadY，因为进近永远从上方降下来，角色停在
//    刚进死区那一点），看似该抵消掉。改完 dy 中位确实从 +11 打到 0 —— 但命中率崩了。
//
//    1984 次出刀分桶（三份日志合并）证明命中带**明显偏正**，不是以 0 为中心：
//        dy 区间   [-25,-15) [-15,-8) [-8,-3) [-3,3) [3,8) [8,15) [15,25)
//        空刀率      40.0%    32.2%   40.3%  19.8% 18.6% 15.1%  25.0%
//    剔除速度混淆（只取 |v|<400）后仍成立：dy<0 空刀 31.8%(n=274) vs dy≥0 19.2%(n=1121)，
//    差 12.6 个百分点、z≈4.1。同档对比：5X 下出刀 297/分 → 135/分，空刀 18.0% → 27.0%。
//
//    换言之 **[8,15) 才是最优带，而 lift=0 的自然落点(+11~12)正好落在它正中**。死区上偏
//    在这里不是缺陷，是免费的正偏置。
//
// ⚠️ 教训：那次判断依据的「最佳命中带 |dy| ≤ 24」取自一份 83% 为正的分布，负侧从未采样，
//    却被当成对称区间外推。要改这个常量，先确认新区间在数据里**真的被采过**。
//
// 用户反馈的「脚边拾取不到」是真问题，但不能用压低悬停点来解 —— 代价是 12.6 个点的空刀率。
// 该走「无目标时才下沉扫货」这类与出刀解耦的路子。
//
// 现与面板「自定义站距」开箱默认对齐：X=60 / Y=-4。
constexpr float kHeliLiftPx = -4.f;

// 水平站距。地面 ClampStandOff() 已与自定义站距 X 共用；此处仍用同一组 X，
// 但不套 12–200 的地面夹（弓/弩空中可以更远）。
//
// 历史分桶（3,265 观察窗）曾推 28；现产品默认改为 60，与自定义站距开箱一致。
// 28 是近战格。飞镖/弓这类远程开打中换怪也走 60——禁止按钉锁把人按进贴脸。
constexpr float kHeliStandOffPx = 60.f;

// 站距真源：勾了「自定义站距」就完全听用户的，否则用上面两个内置值。
// y 带符号，**+Y 向上**（见 heli_rotor.h）⇒ 正数 = 站在怪上方。
struct HeliStand {
    float x = kHeliStandOffPx;  // 人↔怪心的水平目标距离（恒正；左右哪侧由 gLandSide 定）
    float y = kHeliLiftPx;      // 相对怪心的垂直偏移（带符号）
    bool custom = false;
};

HeliStand HeliStandOff() {
    HeliStand s{};
    if (!gStandOffCustom.load(std::memory_order_acquire)) return s;
    s.x = static_cast<float>(
        xcat::ClampCombatStandOffX(gStandOffX.load(std::memory_order_acquire)));
    s.y = static_cast<float>(
        xcat::ClampCombatStandOffY(gStandOffY.load(std::memory_order_acquire)));
    s.custom = true;
    return s;
}
// ⚠️ 别再为「脚边拾取不到」去动悬停高度——那是个**误诊**，实测见下：
//   · 拾取有三条路：petmap(宠吸) / charvac(人物直吸) / foot(原生脚边)。
//     全库战绩：charvac 24737 次吸中 16655、petmap 23954 次吸中 21200，
//     **foot 770 次只吸中 4 次(0.5%)**——用户当时开的正是 foot 这条唯一坏掉的路。
//   · charvac 的盒子是 300x200（半高 100），12px 偏置在里面连零头都不算。
//   · fh-ban 悬空**不挡** charvac：在跑直升机的 4 个场次吸中率 40.5%，
//     反而高于完全没飞的 49 个场次的 29.5%。
// 结论：解法是「开人物直吸」，不是让飞控下沉。曾实现过 SinkHoldToGround（无目标时
// 贴地扫货），除了修错问题之外还是死代码：无锁窗中位仅 1ms、占总时长 1.1%，
// 场上有怪时根本不存在「无目标」的时刻。
// 超此距离走 Cruise 档（放宽速度上限）。
//
// 260 → 140（BIN adc7b2）：Station 的职责是「已经到怪身边了，稳住站位」，260 却把整段
// 中距赶路也划了进去 —— 实测 120~260px 的进近有 248 次、合计 92s，全程被压在 Station 档，
// 而这段路和 300px 的路没有任何性质区别，只是短一点。
//
// 收窄不会带来过冲：Kp·T ≤ 1 已保证单调收敛（见 heli_rotor 的 kKpX 注释），末段速度由
// 增益而非档位决定，抬高档位只影响「远处能跑多快」。140px 约等于 Station 档 3 个发射周期
// 的位移（480px/s × 94ms ≈ 45px），够旋翼把站位稳下来。
constexpr float kHeliCruiseRadius = 140.f;
// 丢目标宽限：宽限内钉住原地悬停等换怪；到点则卸 fh-ban 让它正常落地。
// 不设宽限而直接停旋翼 = fh-ban 下无人托举 → 一路掉出图底，这正是「掉出地图」的主因。
constexpr DWORD kHeliAirborneGraceMs = 1500;
// 进近看门狗：判据是「**还在不在靠近**」，不是给行程掐秒表。
//
// 曾经是固定 2600ms 预算，与地图尺寸直接冲突：换靶时目标常在 1200~1900px 外，而实际
// 巡航均速只有约 400~470px/s（Cruise 上限 560，还要扣加速段和近端 Station 收窄），
// 2600ms 只够飞约 1100px ——**超过这个距离的目标在算术上不可能按时到达**。
// BIN 8bd148 三次超时全是同一形态：距离 1872/1741/1223px 单调下降、全程没停，
// 都在差约 200px 时被砍掉，整趟 2.6 秒白飞，还顺手把那只怪软禁 2.5 秒。
//
// 换成看推进量之后，长途不再误判，真卡住时 1.2s 就放弃，比原来的 2.6s 还果断。
constexpr DWORD kHeliApproachStallMs = 1200;     // 距离无实质改善这么久 = 卡住
constexpr float kHeliApproachProgressPx = 60.f;  // 「实质改善」的门槛
// 兜底上限：防止追一只与我们同速逃跑的怪追到天荒地老。按 450px/s 覆盖约 3600px，
// 远宽于常见地图，正常的跨图长途碰不到它。
constexpr DWORD kHeliApproachHardCapMs = 8000;
constexpr DWORD kHeliApproachSoftBanMs = 2500;

// 进近看门狗的本趟状态。epoch 绑定 gStateEnterMs，用来识别「换了一趟进近」，
// 这样不必往 EnterState 里插重置钩子。
DWORD gHeliApproachEpoch = 0;
DWORD gHeliProgressMs = 0;  // 上次取得实质改善的时刻
float gHeliBestDist = 0.f;  // 本趟见过的最近距离

DWORD gHeliAirborneUntilMs = 0;
bool gHeliHoldValid = false;
float gHeliHoldX = 0.f;
float gHeliHoldY = 0.f;

// 防抖（feature）：到位后只钉住站位 **Y**（吃皮抖/微颤）；**X 每拍跟 ideal**。
// 怪心 Y 一旦蠕动（起跳前几帧）立刻跟 ideal——旧破钉 6px 会让人慢一拍（BIN 02:41）。
// 模式仍 Station。关 gAntiJitterEnabled 即整段旁路。
bool gStationStickValid = false;
int gStationStickLockId = 0;
float gStationStickY = 0.f;
float gStationStickMobY = 0.f;  // 钉住时的怪心 Y
// 稳定才钉：|ΔmobY|≤creep 视为皮抖；>creep 本拍跟 ideal；>jump 整段重锁。
// BIN 15:08：creep=0.75 把插值皮抖当起跳，每拍改钉点 → 与旋翼 Y 死区对拉成上下抖。
constexpr float kStationStickYPx = 8.f;
constexpr float kStationStickMobCreepY = 3.f;
constexpr float kStationStickMobJumpY = 10.f;

// 测谎 / 自动补给硬闸上升沿：停飞前先把人**飞**到最近可站台面，
// 避免图底无台 → 掉穿 → 场景重载（测谎关题 / 卖装换图连带异常）。
// StabilizeFoothold 仅 detach（replant 路径已删；补种=AbsPos 重算=瞬移）。
std::atomic<bool> gLieSafeLand{false};
bool gLieFlyPausedByUs = false;
bool gLieOweFlyBanRestore = false;  // 测谎期临时卸了 Fly ban，答题完再按 armed 接回
bool gLieSafeHaveTarget = false;
bool gLieSafeCatching = false;   // 已靠近：卸 CombatImpact，等引擎自然挂台
bool gLieSafeCatchDrop = false;  // catch 内已软卸旋翼，靠重力贴台（勿一封 catch 就 Disarm）
float gLieSafeX = 0.f;
float gLieSafeY = 0.f;
uint32_t gLieSafeFh = 0;
DWORD gLieSafeStartedMs = 0;
DWORD gLieSafeCatchStartedMs = 0;
int gLieSafeMapId = -1;     // 钉落点时所在图；换图后旧 fh/坐标在新图无意义，必须重钉
int gLieSafeDropFarN = 0;   // 连续 drop_far 重试次数（见 kLieSafeDropFarRetryMax）
// BIN 64b013：回城卷后 mapId 已翻、AbsPos 仍是挂机图 → 假 onFh/原地 Station。
// 换图落台先等出生点落入本图近台，再钉点。
bool gLieSafeAwaitSpawn = false;
constexpr float kLieSafeArrivePx = 36.f;
constexpr float kLieSafeOnFhOkPx = 80.f;  // 已挂台且不太远即可收
// soft_land_quiet：离台 ≤ 此距（或 PeekCurFh≠0）→ 只卸 ban 自然挂台，勿 Station 抖飞（本机 19:06 d=16）。
constexpr float kSoftQuietNaturalLandPx = 100.f;
constexpr DWORD kLieSafeTimeoutMs = 4000;
constexpr DWORD kLieSafeAwaitSpawnMs = 3000;
constexpr DWORD kLieSafeCatchMs = 1200;
// drop_far 每次都把 catch 计时清零，kLieSafeCatchMs 那条兜底因此永远走不到；目标不可达时
// 就成了每 kLieSafeCatchHoldMs 一轮的永动机（BIN 82a4b0 在监狱抖了整段日志）。给它个上限。
constexpr int kLieSafeDropFarRetryMax = 6;
// fh=0（近处无台）时托住原地的保险上限。正常由硬闸释放来收（Encounter/测谎答题都远短于此），
// 这条只防「闸卡住了没人来关」。托着是安全态（人悬停不掉），宁可托久点也别提前撒手。
constexpr DWORD kLieSafeNoLandHoldMs = 60000;
// 同列兜底的纵向搜索窗。只对「X 区间真覆盖脚下」的台放宽纵向距离：旋翼飞几百像素是秒级的事
// （BIN 64b013 闸解后 cruise 0.5s 落位），与 kLieSafeMaxSnapPx 要挡的「水平瞬移到幽灵台」不同。
constexpr int kLieSafeColumnWinPx = 600;
// BIN 10:49: Station 一直托在落点上方 2px → onFh 永不置位 → catch_timeout。
// 先短托消速，再软卸旋翼让 CollisionDetect 挂台。
constexpr DWORD kLieSafeCatchHoldMs = 350;
// 撒手时人必须在台面「上方」：引擎只认自上而下穿过台面才挂台，齐平/下方松手 = 自由落体。
// approach 段 ban 开着能穿台，人常沉到台面下方；旧判据只看直线距离 d≤kLieSafeArrivePx，
// 分不清上下，于是在台面下 35px 处关闸，Station 又从下方渐近收敛到「差 1px」永远上不去。
// BIN 2026-08-13 00:31:00 图 101030102 fh=111（台面 y=410）：每轮都在 y=409 撒手，
// 掉到 y=314（96px）→ drop_far → 重飞，同一死点循环 6 次到 give_up，用户体感「起起落落」。
// 对照同段两次成功：fh=92 在台面上方 15px 撒手、fh=140 上方 10px 撒手，均一次挂住。
// 托举点抬到台面上方 kLieSafeCatchLiftPx，撒手后自然落下这段高度即可挂台（同赶路贴门 liftY）。
constexpr float kLieSafeCatchLiftPx = 32.f;
// 允许进 catch 的纵向窗口（台面上方 0..此值）。要盖得住 lift 后的悬停高度。
constexpr float kLieSafeCatchBandPx = 96.f;

// NM 断开 / 落地静默：停战停旋翼（勿用粘性 SawDisconnect）。
// hold 只留给守护 SHM（kMinHoldMs）。已经 PlayReady 且 NM Connected 时打怪不再认 hold，
// 否则 Finish 里那 2s 最小 hold 会把软链后的刀再卡住（BIN 05:13 soft_hold 清不掉）。
bool SoftOrNetQuiet() {
    const int nm = kick_sniff::LastSessionState();
    if (nm == 0 || nm == 1) return true;
    if (soft_login_probe::IsLandQuiet()) return true;
    if (!soft_login_probe::IsHoldActive()) return false;
    if (ports::world::IsPlayReady()) return false;
    return true;
}

bool InCombatEnableHold(DWORD now) {
    return gEnableHoldUntilMs != 0 && static_cast<int>(now - gEnableHoldUntilMs) < 0;
}

void ClearHiraishinLootHold() {
    gHiraishinNeedLootHold = false;
    gHiraishinLootHoldUntilMs = 0;
}

void ArmHiraishinLootHold(const char* why) {
    if (!gHiraishinEnabled.load(std::memory_order_acquire)) {
        ClearHiraishinLootHold();
        return;
    }
    const DWORD holdMs = gHiraishinLootHoldMs.load(std::memory_order_acquire);
    if (holdMs == 0) {
        ClearHiraishinLootHold();
        return;
    }
    gHiraishinNeedLootHold = true;
    gHiraishinLootHoldUntilMs = 0;
    LogLine("hiraishin loot hold arm %ums why=%s", (unsigned)holdMs,
            why && why[0] ? why : "?");
}

// 能吸物才起钟：等宠 / 硬闸 / 未挂机 不算进静止窗。map arm 期间 Idle 已能吸，计入。
void PumpHiraishinLootHoldClock(DWORD now) {
    if (!gHiraishinNeedLootHold) return;
    if (!gHiraishinEnabled.load(std::memory_order_acquire)) {
        ClearHiraishinLootHold();
        return;
    }
    const DWORD holdMs = gHiraishinLootHoldMs.load(std::memory_order_acquire);
    if (holdMs == 0) {
        ClearHiraishinLootHold();
        return;
    }
    if (!gEnabled.load(std::memory_order_acquire)) return;
    if (gHardPauseMask.load(std::memory_order_acquire) != 0) return;
    if (!TeleportSkipsPetSummonHold() &&
        x::features::pet_feed::ShouldHoldCombatForSummon())
        return;
    // 空中闸未放行前不起钟：别和 post_air_gate 重叠把静止窗吃掉。
    if (soft_login_probe::IsPostSoftAirCombatBlocked()) return;
    if (gHiraishinLootHoldUntilMs == 0) {
        gHiraishinLootHoldUntilMs = now + holdMs;
        gLootPulseGen.fetch_add(1u, std::memory_order_acq_rel);
        LogLine("hiraishin loot hold start %ums", (unsigned)holdMs);
    }
}

bool HiraishinLootHoldBlocksFire(DWORD now) {
    PumpHiraishinLootHoldClock(now);
    if (!gHiraishinNeedLootHold) return false;
    if (!gHiraishinEnabled.load(std::memory_order_acquire)) return false;
    if (gHiraishinLootHoldUntilMs == 0) return true;
    if (static_cast<int>(now - gHiraishinLootHoldUntilMs) < 0) {
        static DWORD sLootHoldLog = 0;
        if (!sLootHoldLog || now - sLootHoldLog > 800) {
            sLootHoldLog = now;
            LogLine("hiraishin loot hold remain=%ums (stand / loot)",
                    gHiraishinLootHoldUntilMs - now);
        }
        return true;
    }
    ClearHiraishinLootHold();
    LogLine("hiraishin loot hold done");
    return false;
}

bool CombatAlreadyAirborne() {
    const DWORD now = x::runtime::NowMs();
    if (gHeliAirborneUntilMs != 0 && static_cast<int>(now - gHeliAirborneUntilMs) < 0) {
        return true;
    }
    if ((ports::fly_fh_ban::ActiveMask() &
         static_cast<unsigned>(ports::fly_fh_ban::BanSource::CombatImpact)) != 0) {
        return true;
    }
    return heli::CurrentOwner() == heli::Owner::Combat ||
           heli::CurrentOwner() == heli::Owner::Gather;
}

bool CombatSpawnAllowsBan() {
    if (!ports::world::IsPlayReady()) return false;
    // 交战已起飞：CurFh=0 是 BAN 摘台，不是进图没台。同图误报 Reset 不得把人按下去。
    if (CombatAlreadyAirborne()) return true;
    const int mapId = ports::world::GetMapId();
    if (mapId <= 0 || !ports::foothold::IsCacheReadyForMap(mapId)) return false;
    if (gSawOnFhThisMap) return true;
    ports::teleport::FlightState st{};
    if (ports::teleport::QueryFlightState(st) && st.ok && st.onFh) {
        gSawOnFhThisMap = true;
        return true;
    }
    return false;
}

bool CombatGlideEnabled() {
    return gImpactApproachEnabled.load(std::memory_order_acquire);
}

bool HeliBaseArmed() {
    // F5+空中贴怪即武装。站桩输出不滑翔，不进这里。
    // Bootstrap / 非地图：禁止武装——payload 可在 worker 起来前 SetEnabled，BAN 会卡住整段换图。
    if (travel::IsActive()) return false;
    if (!ports::world::IsPlayReady()) return false;
    return gEnabled.load(std::memory_order_acquire) && CombatGlideEnabled() &&
           gHardPauseMask.load(std::memory_order_acquire) == 0 &&
           !gKickStressActive.load(std::memory_order_acquire) &&
           !SoftOrNetQuiet() &&
           // ce6797：soft success 后勿立刻 CombatImpact BAN ON 空中扫图。
           !soft_login_probe::IsPostSoftAirCombatBlocked();
}

bool PlayerOutOfPlayBounds(float px, float py);  // 定义见下；Sync 清 bail 要用
bool ClampAimInPlayBounds(float* x, float* y);  // 定义见下；测谎落台 setpoint 要用

void EndLieSafeLand(const char* why);
void OnCombatMapChange(const char* why);
void OnCombatSameMapResume(const char* why);

void BeginLieSafeLand(const char* why) {
    const bool already = gLieSafeLand.exchange(true, std::memory_order_acq_rel);
    if (already) return;
    gLieSafeHaveTarget = false;
    gLieSafeCatching = false;
    gLieSafeCatchDrop = false;
    gLieSafeFh = 0;
    gLieSafeX = 0.f;
    gLieSafeY = 0.f;
    // 与 TickImpl 同源 NowMs：GetTickCount 粗步进可短暂超前 → unsigned 差成「已超时」
    // → 首拍 catch+Disarm（BIN f99271：soft_land_quiet_air d=1800 timedOut=1）。
    gLieSafeStartedMs = x::runtime::NowMs();
    gLieSafeCatchStartedMs = 0;
    gLieSafeMapId = -1;
    gLieSafeDropFarN = 0;
    gLieSafeAwaitSpawn =
        why && (std::strstr(why, "map_change") || std::strstr(why, "ResetForMapChange") ||
                std::strstr(why, "map_arrive") || std::strstr(why, "MapArrive"));
    // F6：停冲量；同时临时卸 Fly 禁挂台源——否则落稳后 CombatImpact 卸掉时 Fly 源仍 OR
    // 着 → 站不住继续掉（补给 PauseSystems 曾踩过同一坑）。
    if (!fly::IsExternallyPaused()) {
        fly::SetExternalPause(true);
        gLieFlyPausedByUs = true;
    }
    if (fly::IsArmed() &&
        (ports::fly_fh_ban::ActiveMask() &
         static_cast<unsigned>(ports::fly_fh_ban::BanSource::Fly)) != 0) {
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Fly, false);
        gLieOweFlyBanRestore = true;
    }
    LogLine("lie_safe_land begin why=%s flyPause=%d oweFlyBan=%d", why ? why : "?",
            gLieFlyPausedByUs ? 1 : 0, gLieOweFlyBanRestore ? 1 : 0);
}

constexpr uint32_t kSafeLandHoldersMask =
    static_cast<uint32_t>(HardPauseHolder::ChannelHop) |
    static_cast<uint32_t>(HardPauseHolder::AutoLie) |
    static_cast<uint32_t>(HardPauseHolder::AutoSupply) |
    static_cast<uint32_t>(HardPauseHolder::Encounter) |
    static_cast<uint32_t>(HardPauseHolder::MapArrive) |
    static_cast<uint32_t>(HardPauseHolder::CharBoot);

const char* SafeLandBeginWhy(HardPauseHolder holder) {
    switch (holder) {
    case HardPauseHolder::ChannelHop:
        return "channel_hop_pause_on";
    case HardPauseHolder::AutoLie:
        return "auto_lie_pause_on";
    case HardPauseHolder::AutoSupply:
        return "auto_supply_pause_on";
    case HardPauseHolder::Encounter:
        return "encounter_pause_on";
    case HardPauseHolder::MapArrive:
        return "map_arrive_pause_on";
    case HardPauseHolder::CharBoot:
        return "char_boot_pause_on";
    default:
        return "hard_pause_on";
    }
}

const char* SafeLandEndWhy(HardPauseHolder holder) {
    switch (holder) {
    case HardPauseHolder::ChannelHop:
        return "channel_hop_pause_off";
    case HardPauseHolder::AutoLie:
        return "auto_lie_pause_off";
    case HardPauseHolder::AutoSupply:
        return "auto_supply_pause_off";
    case HardPauseHolder::Encounter:
        return "encounter_pause_off";
    case HardPauseHolder::MapArrive:
        return "map_arrive_pause_off";
    case HardPauseHolder::CharBoot:
        return "char_boot_pause_off";
    default:
        return "hard_pause_off";
    }
}

void EndLieSafeLand(const char* why) {
    const bool was = gLieSafeLand.exchange(false, std::memory_order_acq_rel);
    gLieSafeHaveTarget = false;
    gLieSafeCatching = false;
    gLieSafeCatchDrop = false;
    gLieSafeCatchStartedMs = 0;
    gLieSafeDropFarN = 0;
    if (was) {
        heli::Disarm(heli::Owner::Combat);
        heli::Release(heli::Owner::Combat);
        gHeliHoldValid = false;
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
        // BIN adfed6：落台 Tick 每拍续 gHeliAirborneUntilMs；若 onFh 结束仍留宽限，
        // SyncImpactFhBan 立刻 BAN ON 撕掉刚挂的台 → Hold 悬空 / combat↔none 抖振。
        gHeliAirborneUntilMs = 0;
    }
    gLieSafeAwaitSpawn = false;
    const bool keepFlyHold =
        (gHardPauseMask.load(std::memory_order_acquire) & kSafeLandHoldersMask) != 0;
    // 测谎答题 / 补给行程仍在：Fly ban / ExternalPause 继续卸着；硬闸全灭才恢复。
    if (!keepFlyHold) {
        if (gLieOweFlyBanRestore) {
            gLieOweFlyBanRestore = false;
            if (fly::IsArmed()) {
                ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Fly, true);
            }
        }
        if (gLieFlyPausedByUs) {
            fly::SetExternalPause(false);
            gLieFlyPausedByUs = false;
        }
    }
    if (was || why) {
        LogLine("lie_safe_land end why=%s was=%d keepFlyHold=%d", why ? why : "?", was ? 1 : 0,
                keepFlyHold ? 1 : 0);
    }
}

void EnsureLieSafeTarget(float px, float py) {
    const int mapId = ports::world::GetMapId();
    const bool playReady = ports::world::IsInMapScene() && ports::world::IsPlayReady() &&
                           ports::world::GetSceneState() == ports::world::SceneState::Field;
    const bool fhReady = mapId > 0 && ports::foothold::IsCacheReadyForMap(mapId) &&
                         ports::foothold_path::EnsureGraph();

    // 已有目标：同图且人仍靠近则保持（含 fh=0 原地钉，BIN 79a8f1 每拍 miss 重钉=乱飘）。
    if (gLieSafeHaveTarget) {
        const bool sameMap = mapId > 0 && mapId == gLieSafeMapId;
        const float tdx = px - gLieSafeX;
        const float tdy = py - gLieSafeY;
        const float td = std::sqrt(tdx * tdx + tdy * tdy);
        constexpr float kKeepNearTargetPx = 120.f;
        if (gLieSafeFh != 0) {
            // BIN 82a4b0：换图边沿用旧图落点钉死 → 半径外空转；换图或出界则作废。
            // BIN f283a3：play bounds 未热误报 inBounds=0 → 勿因抖动重钉远台。
            const bool inBounds = ports::map_bounds::PointInPlayBounds(
                gLieSafeX, gLieSafeY, /*mapId=*/0, ports::map_bounds::kLandMarginPx);
            if (sameMap && (inBounds || td <= kKeepNearTargetPx)) return;
            LogLine("lie_safe_land target stale sameMap=%d inBounds=%d td=%.0f old=(%.0f,%.0f) "
                    "fh=%u map=%d->%d",
                    sameMap ? 1 : 0, inBounds ? 1 : 0, td, gLieSafeX, gLieSafeY,
                    (unsigned)gLieSafeFh, gLieSafeMapId, mapId);
        } else {
            // 原地钉：勿跟飘动 AbsPos 每拍改 hold（否则 Station 追着人飞）。
            if (sameMap && td <= kKeepNearTargetPx) return;
            LogLine("lie_safe_land in-place stale td=%.0f old=(%.0f,%.0f) map=%d->%d", td,
                    gLieSafeX, gLieSafeY, gLieSafeMapId, mapId);
        }
        gLieSafeHaveTarget = false;
        gLieSafeFh = 0;
        gLieSafeCatching = false;
        gLieSafeCatchDrop = false;
        gLieSafeCatchStartedMs = 0;
        gLieSafeDropFarN = 0;
    }

    // BIN 79a8f1：回城卷 mapId 已翻成 104000000、AbsPos 仍是挂机图 (181,325)
    // → Snap 钉到主城幽灵台 fh=102 → 旋翼 chase/drop_far 乱飘数秒。等 Field+FH 再钉。
    if (!playReady || !fhReady) {
        static DWORD sWaitLog = 0;
        const DWORD nowLog = x::runtime::NowMs();
        if (!sWaitLog || nowLog - sWaitLog > 400) {
            sWaitLog = nowLog;
            LogLine("lie_safe_land wait map_ready play=%d fhReady=%d map=%d ap=(%.0f,%.0f)",
                    playReady ? 1 : 0, fhReady ? 1 : 0, mapId, px, py);
        }
        return;
    }

    float sx = px;
    float sy = py;
    uint32_t fh = 0;
    // preferFlat=false：脚下/近处优先；远台退化与 settle hover 同病（d76f13/f283a3）。
    constexpr float kLieSafeMaxSnapPx = 100.f;
    bool nearSnap = false;
    if (ports::foothold_path::SnapStandAt(px, py, &sx, &sy, &fh, /*preferFlat=*/false) && fh) {
        const float sdx = sx - px;
        const float sdy = sy - py;
        const float sd = std::sqrt(sdx * sdx + sdy * sdy);
        if (sd > kLieSafeMaxSnapPx) {
            const float farX = sx;
            const float farY = sy;
            const uint32_t farFh = fh;
            sx = px;
            sy = py;
            fh = 0;
            // SnapStandAt 在「人悬空、到台落差 > kCoverYTol(80)」时会退化：三个覆盖档
            // （stack≤72 / sameY≤45 / loose≤80）全落空后，退化档里 bestYBand（只要同高
            // |dy|≤45，不管 X 隔多远）排在 bestAny（全图最近）前面，于是挑出水平上千像素
            // 外的台。BIN 64b013：人在 (-1533,245)，旋翼自己知道台在 x≈-1355（setpoint 一直
            // 指那儿、闸解后 cruise 0.5s 就落位），Snap 却给了 (267,265)——只因它同高 20px。
            // ProbeColumn 只收 X 区间真覆盖脚下的段，天然排掉那类远台；纵向按
            // kLieSafeColumnWinPx 放宽，不分上下取最近（旋翼上下皆可达）。
            ports::foothold_path::ColumnHit hits[8]{};
            const int nHit = ports::foothold_path::ProbeColumn(px, py, kLieSafeColumnWinPx, hits, 8);
            int pick = -1;
            float pickDy = 1e9f;
            for (int i = 0; i < nHit; ++i) {
                if (hits[i].wall || hits[i].narrow) continue;
                const float dy = std::fabs(static_cast<float>(hits[i].y) - py);
                if (dy < pickDy) {
                    pickDy = dy;
                    pick = i;
                }
            }
            float cx = px;
            float cy = py;
            if (pick >= 0 && ports::foothold_path::SnapOnFh(hits[pick].fh, px, &cx, &cy)) {
                gLieSafeX = cx;
                gLieSafeY = cy;
                gLieSafeFh = hits[pick].fh;
                gLieSafeHaveTarget = true;
                gLieSafeMapId = mapId;
                gLieSafeAwaitSpawn = false;
                LogLine("lie_safe_land column fallback fh=%u stand=(%.0f,%.0f) dy=%.0f "
                        "from=(%.0f,%.0f) hits=%d (snap gave fh=%u d=%.0f)",
                        (unsigned)hits[pick].fh, cx, cy, pickDy, px, py, nHit, (unsigned)farFh, sd);
                return;
            }
            LogLine("lie_safe_land snap reject far d=%.0f fh=%u sp=(%.0f,%.0f) from=(%.0f,%.0f) "
                    "colHits=%d → in-place",
                    sd, (unsigned)farFh, farX, farY, px, py, nHit);
        } else {
            gLieSafeX = sx;
            gLieSafeY = sy;
            gLieSafeFh = fh;
            gLieSafeHaveTarget = true;
            gLieSafeMapId = mapId;
            gLieSafeAwaitSpawn = false;
            LogLine("lie_safe_land target fh=%u stand=(%.0f,%.0f) from=(%.0f,%.0f) map=%d",
                    (unsigned)fh, sx, sy, px, py, mapId);
            return;
        }
    }
    // BIN 64b013：卷轴后 AbsPos 仍是挂机图 (-944,-215)，近处无本图台 → 假 onFh/原地 Station
    // 会立刻放行 Travel，出生点再 settle+横飞。等出生传送落到近台再钉。
    if (gLieSafeAwaitSpawn) {
        const DWORD age = gLieSafeStartedMs
                              ? (x::runtime::NowMs() - gLieSafeStartedMs)
                              : 0;
        if (age < kLieSafeAwaitSpawnMs) {
            static DWORD sSpawnLog = 0;
            const DWORD nowLog = x::runtime::NowMs();
            if (!sSpawnLog || nowLog - sSpawnLog > 400) {
                sSpawnLog = nowLog;
                LogLine("lie_safe_land wait spawn nearFh=0 age=%ums ap=(%.0f,%.0f) map=%d",
                        (unsigned)age, px, py, mapId);
            }
            return;
        }
        gLieSafeAwaitSpawn = false;
        LogLine("lie_safe_land await spawn timeout age=%ums → in-place ap=(%.0f,%.0f)",
                (unsigned)age, px, py);
    }
    gLieSafeX = px;
    gLieSafeY = py;
    gLieSafeFh = 0;
    gLieSafeHaveTarget = true;
    gLieSafeMapId = mapId;
    LogLine("lie_safe_land target miss snap; hold=(%.0f,%.0f) map=%d", px, py, mapId);
}

// 换图后重钉目标：不清旋翼、不卸 ban——补给/测谎硬闸还在，只换本图落点。
void RestartLieSafeLand(const char* why) {
    if (!gLieSafeLand.load(std::memory_order_acquire)) {
        BeginLieSafeLand(why);
        return;
    }
    gLieSafeHaveTarget = false;
    gLieSafeCatching = false;
    gLieSafeCatchDrop = false;
    gLieSafeFh = 0;
    gLieSafeX = 0.f;
    gLieSafeY = 0.f;
    gLieSafeStartedMs = x::runtime::NowMs();
    gLieSafeCatchStartedMs = 0;
    gLieSafeMapId = -1;
    gLieSafeDropFarN = 0;
    gLieSafeAwaitSpawn = true;
    // 不在此 Acquire：等 Ensure 钉到本图落点后再由 Tick 抢旋翼（BIN 79a8f1 幽灵台）。
    if (heli::CurrentOwner() == heli::Owner::Combat) {
        heli::Disarm(heli::Owner::Combat);
        heli::Release(heli::Owner::Combat);
    }
    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
    LogLine("lie_safe_land restart why=%s", why ? why : "?");
}

void ReleaseMapArriveIfHeld() {
    const uint32_t bit = static_cast<uint32_t>(HardPauseHolder::MapArrive);
    if ((gHardPauseMask.load(std::memory_order_acquire) & bit) == 0) return;
    SetHardPause(HardPauseHolder::MapArrive, false);
}

// 只清速度锁存（不碰 CurFh；stabilize 才 detach）。
void ClearLieSafeMotion() {
    struct ClearJob {
        bool ok = false;
    } job{};
    auto fn = [](void* p) {
        auto* j = static_cast<ClearJob*>(p);
        if (!j) return;
        j->ok = ports::teleport::ClearMotionLatchMainThread();
    };
    const bool pumped =
        x::runtime::main_thread::Ensure() &&
        x::runtime::main_thread::InvokeAndWait(fn, &job, 80,
                                              x::runtime::main_thread::JobPrio::High);
    LogLine("lie_safe_land clear_v ok=%d pumped=%d", job.ok ? 1 : 0, pumped ? 1 : 0);
}

// 安全落台：旋翼飞近 → 卸禁挂台 → 短托消速 → 软卸旋翼挂台。禁止硬掰 AbsPos。
// BIN 10:42:57 一进 catch 就 Disarm → 直坠；10:49 Station 永不卸 → onFh 卡死 catch_timeout。
void TickLieSafeLand(DWORD now) {
    if (!gLieSafeLand.load(std::memory_order_acquire)) return;

    ports::teleport::FlightState st{};
    if (!ports::teleport::QueryFlightState(st) || !st.ok) return;
    EnsureLieSafeTarget(st.x, st.y);
    // BIN 79a8f1：等图就绪期间无落点——勿用 (0,0)/陈旧坐标 Station，也勿挂 approach ban。
    if (!gLieSafeHaveTarget) {
        if (heli::CurrentOwner() == heli::Owner::Combat) {
            heli::Disarm(heli::Owner::Combat);
            heli::Release(heli::Owner::Combat);
        }
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
        static DWORD sWaitDriveLog = 0;
        if (!sWaitDriveLog || now - sWaitDriveLog > 400) {
            sWaitDriveLog = now;
            LogLine("lie_safe_land wait target ap=(%.0f,%.0f) map=%d", st.x, st.y,
                    ports::world::GetMapId());
        }
        return;
    }

    const float dx = st.x - gLieSafeX;
    const float dy = st.y - gLieSafeY;
    const float d = std::sqrt(dx * dx + dy * dy);
    const bool closeEnough = d <= kLieSafeArrivePx;
    // AbsPos：更大 Y = 更高。dy>0 即人在台面之上，是唯一能挂住台的撒手姿势。
    const bool overFh = std::fabs(dx) <= kLieSafeArrivePx && dy >= 0.f &&
                        dy <= kLieSafeCatchBandPx;
    // 有符号差：同拍 Begin 用 NowMs() 可能略晚于 Tick 的 now → unsigned 下溢成「已超时」
    // （BIN 706c42：begin 后 17ms 就 timedOut=1 d=1800）。
    const bool timedOut =
        gLieSafeStartedMs != 0 &&
        static_cast<int>(now - gLieSafeStartedMs) >= static_cast<int>(kLieSafeTimeoutMs);

    // 引擎已挂台且距落点不远：只清速度，结束。
    // 勿用「catching 即可」放宽：软重连幽灵坐标 (BIN f99271 ap=-1533) 偶发假 onFh，
    // d≈1800 时若因 catching 收台 → 立刻放闸自由落体。
    if (st.onFh && (closeEnough || d <= kLieSafeOnFhOkPx)) {
        // BIN 64b013：卷轴后 AbsPos 仍挂机图坐标却假 onFh（curFh 残留）→ 放行 Travel 后
        // 出生点再 settle 起飞。收台须脚下确有近台。
        float nx = st.x;
        float ny = st.y;
        uint32_t nfh = 0;
        bool nearFh = false;
        if (ports::foothold_path::SnapStandAt(st.x, st.y, &nx, &ny, &nfh,
                                              /*preferFlat=*/false) &&
            nfh) {
            const float ndx = nx - st.x;
            const float ndy = ny - st.y;
            nearFh = std::sqrt(ndx * ndx + ndy * ndy) <= 100.f;
        }
        if (!nearFh) {
            static DWORD sStaleOnFh = 0;
            if (!sStaleOnFh || now - sStaleOnFh > 400) {
                sStaleOnFh = now;
                LogLine("lie_safe_land onFh reject no-near-fh ap=(%.0f,%.0f) map=%d", st.x, st.y,
                        ports::world::GetMapId());
            }
        } else {
            ClearLieSafeMotion();
            EndLieSafeLand("onFh");
            ReleaseMapArriveIfHeld();
            return;
        }
    }

    // fh=0 = 近处压根没有可挂的台（snap reject far / miss snap 的原地钉）。这时 catch→drop 那套
    // 「松手让引擎自己挂台」是空转：一松手就自由落体，掉出 kLieSafeOnFhOkPx 又被旋翼拉回，
    // 成了 ~780ms 一轮的弹跳——BIN 64b013 在 ap=(-1533,245) 抖了 4.5s，直到撞上 drop_far
    // 上限才停（why=encounter_pause_on 不走 awaitSpawn，那道保护挡不住）。
    // 没台可落就别撒手：Station 托住原地等硬闸自己结束。期间若人自然挂上台，上面那段
    // onFh 判定（d≈0 满足 closeEnough）会正常收台。
    if (gLieSafeFh == 0) {
        if (gLieSafeStartedMs != 0 &&
            static_cast<int>(now - gLieSafeStartedMs) >= static_cast<int>(kLieSafeNoLandHoldMs)) {
            LogLine("lie_safe_land no-landable hold expired age=%dms ap=(%.0f,%.0f) map=%d",
                    static_cast<int>(now - gLieSafeStartedMs), st.x, st.y,
                    ports::world::GetMapId());
            ClearLieSafeMotion();
            EndLieSafeLand("no_landable_timeout");
            ReleaseMapArriveIfHeld();
            return;
        }
        // 托举期必须清掉 catch/drop 状态：否则 SyncImpactFhBan 会按 catchHold 口径摆动 ban。
        gLieSafeCatching = false;
        gLieSafeCatchDrop = false;
        gLieSafeCatchStartedMs = 0;
        gLieSafeDropFarN = 0;
        if (heli::Bailed()) heli::ClearBailed();
        (void)heli::Acquire(heli::Owner::Combat);
        gHeliAirborneUntilMs = now + kHeliAirborneGraceMs;
        heli::Setpoint sp{};
        sp.x = gLieSafeX;
        sp.y = gLieSafeY;
        ClampAimInPlayBounds(&sp.x, &sp.y);
        sp.mode = heli::Mode::Station;
        heli::SetSetpoint(heli::Owner::Combat, sp);
        static DWORD sHoldLog = 0;
        if (!sHoldLog || now - sHoldLog > 800) {
            sHoldLog = now;
            LogLine("lie_safe_land no-landable hold d=%.0f onFh=%d ap=(%.0f,%.0f) sp=(%.0f,%.0f)",
                    d, st.onFh ? 1 : 0, st.x, st.y, sp.x, sp.y);
        }
        return;
    }

    // 到台面上方（或总超时）：卸 ban；先 Station 消速，再软卸旋翼（见下方 drop）。
    // 只按 overFh 进 catch——在台面下方关闸必然接不住，白掉一轮又被拉回来。
    if (!gLieSafeCatching && (overFh || timedOut)) {
        gLieSafeCatching = true;
        gLieSafeCatchDrop = false;
        gLieSafeCatchStartedMs = now;
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
        LogLine("lie_safe_land catch begin d=%.0f dy=%.0f timedOut=%d fh=%u ap=(%.0f,%.0f) "
                "sp=(%.0f,%.0f)",
                d, dy, timedOut ? 1 : 0, (unsigned)gLieSafeFh, st.x, st.y, gLieSafeX, gLieSafeY);
    }

    if (gLieSafeCatching) {
        const DWORD catchAge =
            gLieSafeCatchStartedMs ? (now - gLieSafeCatchStartedMs) : 0;
        // 软卸：消速后放开托举，让引擎挂台；掉远则重飞。
        if (!gLieSafeCatchDrop && catchAge >= kLieSafeCatchHoldMs) {
            gLieSafeCatchDrop = true;
            heli::Disarm(heli::Owner::Combat);
            heli::Release(heli::Owner::Combat);
            LogLine("lie_safe_land catch drop d=%.0f age=%ums ap=(%.0f,%.0f)", d,
                    (unsigned)catchAge, st.x, st.y);
        }
        if (gLieSafeCatchDrop && d > kLieSafeOnFhOkPx) {
            gLieSafeCatching = false;
            gLieSafeCatchDrop = false;
            gLieSafeCatchStartedMs = 0;
            ++gLieSafeDropFarN;
            if (gLieSafeDropFarN >= kLieSafeDropFarRetryMax) {
                LogLine("lie_safe_land drop_far give_up n=%d d=%.0f fh=%u — end safe land",
                        gLieSafeDropFarN, d, (unsigned)gLieSafeFh);
                ClearLieSafeMotion();
                EndLieSafeLand("drop_far_exhausted");
                ReleaseMapArriveIfHeld();
                return;
            }
            LogLine("lie_safe_land catch drop_far retry d=%.0f n=%d", d, gLieSafeDropFarN);
        } else {
            const bool catchTimedOut = catchAge >= kLieSafeCatchMs;
            if (catchTimedOut) {
                if (!timedOut) {
                    gLieSafeCatching = false;
                    gLieSafeCatchDrop = false;
                    gLieSafeCatchStartedMs = 0;
                    LogLine("lie_safe_land catch retry d=%.0f", d);
                } else {
                    ClearLieSafeMotion();
                    EndLieSafeLand("catch_timeout");
                    ReleaseMapArriveIfHeld();
                    return;
                }
            }
        }
        // drop 中：不发 setpoint，等 onFh / 掉远重飞 / 超时。
        if (gLieSafeCatchDrop) {
            static DWORD sDropLog = 0;
            if (!sDropLog || now - sDropLog > 400) {
                sDropLog = now;
                LogLine("lie_safe_land drop wait d=%.0f onFh=%d ap=(%.0f,%.0f)", d,
                        st.onFh ? 1 : 0, st.x, st.y);
            }
            return;
        }
    }

    if (heli::Bailed()) heli::ClearBailed();
    // 落台必须抢过 F6：ExternalPause 后 Fly owner 可能仍占着旋翼。
    (void)heli::Acquire(heli::Owner::Combat);
    gHeliAirborneUntilMs = now + kHeliAirborneGraceMs;

    heli::Setpoint sp{};
    sp.x = gLieSafeX;
    // 托到台面上方再撒手（AbsPos 更大 Y = 更高）。目标点就设在台面上会从下方渐近收敛，
    // 永远差最后 1px 上不去，撒手即掉——见 kLieSafeCatchLiftPx 处的 BIN 记录。
    sp.y = gLieSafeY + kLieSafeCatchLiftPx;
    ClampAimInPlayBounds(&sp.x, &sp.y);
    // catch 消速段 Station；远距 cruise 飞近。
    sp.mode = (!gLieSafeCatching && d > 120.f) ? heli::Mode::Cruise : heli::Mode::Station;
    heli::SetSetpoint(heli::Owner::Combat, sp);

    static DWORD sDriveLog = 0;
    if (!sDriveLog || now - sDriveLog > 400) {
        sDriveLog = now;
        LogLine("lie_safe_land drive d=%.0f onFh=%d catch=%d drop=%d fh=%u sp=(%.0f,%.0f) "
                "ap=(%.0f,%.0f)",
                d, st.onFh ? 1 : 0, gLieSafeCatching ? 1 : 0, gLieSafeCatchDrop ? 1 : 0,
                (unsigned)gLieSafeFh, sp.x, sp.y, st.x, st.y);
    }
}

// F5 Impact 挂机期禁挂台（与 F6 共用 fly_fh_ban 多源 OR）：空中可砍，穿层滑翔。
// 仅在「有目标或刚丢目标」期间挂：长时间无怪就落地站着，别悬在空中赌旋翼。
void SyncImpactFhBan() {
    // 安全落台分两层（BIN 686e3f / 12:04:31）：
    //   · approach（!catching）：要 ban + 旋翼，飞近落点
    //   · catch hold（catching && !drop）：必须卸 ban 才能 onFh，但旋翼仍要 Station 消速
    //   · catch drop：旋翼卸掉，交给引擎挂台
    // 旧逻辑把 lieLand 写成「!catching」→ catch 时 want=false → 每帧 Release，
    // 而 TickLieSafeLand 又 Acquire → owner combat↔none 抖振 = 下落回拉循环。
    const bool safeLand = gLieSafeLand.load(std::memory_order_acquire);
    const bool lieApproach = safeLand && gLieSafeHaveTarget && !gLieSafeCatching;
    const bool lieCatchHold = safeLand && gLieSafeHaveTarget && gLieSafeCatching && !gLieSafeCatchDrop;
    // 落台飞近仍可 BAN；交战 BAN 必须等人挂过台。否则首次进图 CurFh=0 被 detach 直坠。
    bool wantBan = false;
    if (lieApproach) {
        wantBan = ports::world::IsPlayReady();
    } else if (HeliBaseArmed() && CombatSpawnAllowsBan()) {
        wantBan = true;
    } else if (HeliBaseArmed()) {
        static DWORD sSpawnBan = 0;
        const DWORD nowLog = x::runtime::NowMs();
        if (!sSpawnBan || nowLog - sSpawnBan > 800) {
            sSpawnBan = nowLog;
            LogLine("impact BAN defer spawn wait onFh (graph/play/curFh)");
        }
    }
    // 旋翼已判定不可救（状态停更 / 深度出界）：立刻卸禁挂台，让引擎按常规落地或复位。
    // 继续挂着 fh-ban 只会让角色永远接不住地板（bea1c3 就是这么一路掉到断线的）。
    // 测谎落台飞近阶段：不清 wantBan（否则图底 freefall → 重载关题）。
    if (wantBan && heli::Bailed() && !lieApproach) {
        // 无主时 Tick 早退清不了 bail（7848f4）。只在落地，或「进图且未出界的无主」时放行，
        // 避免深度出界 bail→Release→立刻清 bail→再冲的空中抖振。
        ports::teleport::FlightState st{};
        const bool have = ports::teleport::QueryFlightState(st) && st.ok;
        const bool grounded = have && st.onFh;
        const bool orphanInMap = heli::CurrentOwner() == heli::Owner::None &&
                                 ports::world::IsPlayReady() && have &&
                                 !PlayerOutOfPlayBounds(st.x, st.y);
        if (grounded || orphanInMap) {
            heli::ClearBailed();
            static DWORD sBailClearLog = 0;
            const DWORD nowLog = x::runtime::NowMs();
            if (!sBailClearLog || nowLog - sBailClearLog > 2000) {
                sBailClearLog = nowLog;
                x::runtime::LogI("Heli", "clear bail why=%s", grounded ? "onFh" : "orphan_play");
            }
        }
        if (heli::Bailed()) wantBan = false;
    }
    if (wantBan && !lieApproach) {
        const DWORD now = x::runtime::NowMs();
        wantBan = gHeliAirborneUntilMs != 0 &&
               static_cast<int>(now - gHeliAirborneUntilMs) < 0;
    }
    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, wantBan);
    // catch hold：ban 已关，但仍要占着 Combat 发 Station；drop 段才交还。
    const bool wantOwner = wantBan || lieCatchHold || lieApproach;
    if (wantOwner) {
        if (safeLand) {
            (void)heli::Acquire(heli::Owner::Combat);
        } else {
        // 每 tick 都试：F6 抢走时这里失败（不回抢，避免对拽），它一释放又能立刻接回来。
        // 接不回来就是没人发冲量而 fh-ban 还挂着 = 自由落体，所以不能只在武装时调一次。
        (void)heli::TryAcquire(heli::Owner::Combat);
        }
    } else {
        heli::Disarm(heli::Owner::Combat);
        heli::Release(heli::Owner::Combat);
        gHeliHoldValid = false;
    }
}

// 旋翼 setpoint 夹取：F5 Combat 可位移区 = raw FH AABB × kCombatMoveBoundsScale（0.95）。
// 贴边怪照打，人/站位点不得稳出此框（205：贴边出刀越界）。
// F6 手动飞仍用 heli::ClampToAirspace（外扩空域），见 fly 路径直接调用。
bool ClampAimInPlayBounds(float* x, float* y) { return heli::ClampToCombatMoveBounds(x, y); }

// 直升机悬停带：怪旁站距内 + 纵漂可控 → 停 Impact，只出刀。
constexpr float kHeliHoverMaxDy = 80.f;
constexpr float kHeliStationSlack = 22.f;  // 相对站距的水平松弛，内则不纠
// 到位环半宽（自定义站距下沿 / HeliStationOk 共用；须在 InHeliHoverBand 之前可见）。
constexpr float kHeliStationDy = 15.f;
constexpr float kHeliStationDx = 40.f;
// BAN+站立钉在台沿：站位点再抬高（AbsPos 更大 Y=更高），逼出跳/飞 ma 才能出刀。
constexpr float kHeliStandTakeoffLiftY = 48.f;

// ── 防贴脸退避（LiveStep）· 本拍解算结果 ───────────────────────────────────
//
// 由来：弓手贴身「挥弓」不是 bug，是客户端分支 —— 怪压进一个 30 宽的身位框就挥弓，
// 没压进去就射箭，两条路都正常结算伤害（BIN 11:48 实测 mrect=[x y 30 87]）。所以正解
// 不是去改客户端分支（改过，会把伤害整条路打断），而是**别让怪进到那个框里**。
// 静态站距只绕着锁定目标算，群怪冲脸时无效，故补这一层动态退避。
//
// ★ `active` 是总闸。它为 false 时，下面读它的四处（HeliHoverMaxDx / HeliStationOk /
//   BuildHeliStationPoint / Firing 的出刀门）全部退回原分支 —— 既有语句一行未动，
//   新代码只挂在 `if (gDodge.active)` 之后。这是「关掉等于没这功能」的唯一保证。
//
// 只在战斗 worker 的 TickImpl 里写（ComputeDodge，pass 循环之前），同线程内只读，故不加锁。
struct DodgeState {
    bool active = false;    // 本拍处于退避（总闸）
    bool clear = false;     // 退避点上一只怪都够不着 → 可以照常出刀
    float x = 0.f;          // 修正后的站位 X（AbsPos）
    float fireMaxDx = 0.f;  // 退到位后到锁怪的水平距离 + 余量；喂给下面几道闸的下限
    int intruders = 0;      // 压进 Rx/Ry 的怪数（诊断用）
};
DodgeState gDodge;

// ★ 全部四道闸都从 HeliStandOff() 反解，**不许再写死**。原因是自定义站距一开，写死的
//   上界会把功能整个废掉：远程站到 250px，`|dx| <= 120` 这道硬闸每拍一票否决，角色只会
//   在 250px 外挥空气或反复重进 MoveTo。闸门必须跟着站距走。
//   内置档（custom=false）代入 x=28 / y=0 后与改版前**逐位相同**，不是近似。
//
// 退避同理：它把人推到比站距更远的地方，闸门不跟着放宽就会在人退到位之后一票否决 ——
// MoveTo 的 `heli_in_band` 进不了 Firing，人悬在退避点上一刀不出。这是本功能唯一一处
// 必须改「共用判据」的地方，改在这里能让下游五处门禁**同时**跟上，不会漏一处对推。
// 上界由解算器自己夹（见 kDodgeMaxStandMul），不会无限放宽。
float HeliHoverMaxDx() {
    const HeliStand s = HeliStandOff();
    const float base = s.x * kHitBandMaxFrac + kHeliStationSlack;
    if (gDodge.active && gDodge.fireMaxDx > base) return gDodge.fireMaxDx;
    return base;
}

float HeliHoverMaxDy() {
    const HeliStand s = HeliStandOff();
    return std::fabs(s.y) + kHeliHoverMaxDy;
}

bool InHeliHoverBand(float px, float py, float mx, float my) {
    const HeliStand s = HeliStandOff();
    const float dx = std::fabs(mx - px);
    const float dy = std::fabs(my - py);
    if (dy > HeliHoverMaxDy()) return false;
    // 叠怪心：内置口径要求侧移再砍（防背打）。自定义 X 小于这个数是用户明说要贴怪心，
    // 再拦就等于没让他自定义。
    if (!s.custom && dx < kMinLandAway) return false;
    // 自定义是**环带**（与 HeliStationOk 同语义）：|dx| ∈ [X−stationDx, X+stationDx]。
    // · 缺下沿 → |dx|≪X 也算到位 → 贴脸出刀（BIN 18b0df：X=201 却 dx=15）。
    // · 缺上沿 → 还在飞、|dx| 远大于 X 就 heli_in_band 开砍 → 远程空挥（用户要压的
    //   OUTER）。上沿不再用 HoverMax（X×1.55+22）；那是近战「边打边挪」口径。
    // X≤stationDx 时下沿≤0，不额外收紧（用户就是要贴）。
    //
    // 只跟「自定义站距」走，**不**跟防贴脸总闸：空挥是站距问题，群怪退避开关与否
    // 都该进环再砍。退避激活时到位轴换成 gDodge.x（与 HeliStationOk 一致）。
    if (s.custom) {
        if (gDodge.active) {
            if (std::fabs(px - gDodge.x) > kHeliStationDx) return false;
        } else {
            const float minDx = s.x - kHeliStationDx;
            const float maxDx = s.x + kHeliStationDx;
            if (minDx > 0.f && dx < minDx) return false;
            if (dx > maxDx) return false;
        }
    }
    return dx <= HeliHoverMaxDx();
}

// 出刀带与悬停带必须分开：悬停带回答「还要不要再飞」，出刀带回答「这一刀够不够得到」。
// 历史上共用 kHeliHoverMaxDy=80，代价是近一半的刀砍在空气里。
//
// ★ 判据真源已从「客户端猜」换成「客户端自己说」。攻包 op=50 的 `flags` 高 nibble 就是
// 命中判定的输出（命中 0x11 / 落空 0x01），命中包还带着怪物与玩家坐标——等于客户端把
// 每一次判决连同当时的相对位置都标注好了，不必去反编译判定函数。
//
// 据此得到两组数（脚本见 `Dumps/runtime/_hitbox.py` 与 `_hitrate.py`）：
//   · 34,360 个命中样本反解出的命中盒：dx ∈ [-20,+130]、dy ∈ ±45（尾到 ±61）；矩形，非椭圆
//   · 12,063 刀（攻包判决 × combat.log 站位，±120ms 配对）的条件命中率：
//       dy<15 & dx<40 → 96% ｜ dx 40~80 → 92% ｜ dx 80~120 → 60%
//       dy 15~30      → 89% ｜ dy 30~45 → 53%
//
// ⚠️ **不要按近战格收紧远程出刀闸**。曾把闸收到 dy24/dx60，kills/min 119→109。
// 钉锁空包 ≠ 站太远：飞镖盗贼站 60 本就在投掷盒里，再收到 28 是把远程按进贴脸
// （和防贴脸退避对着干）。空包优先查朝向 / 竖直 / inView，不要改站距。
//
// 仍然成立的旧结论：速度不判别空刀。只取 |dy|<12 且 |dx|<40 的刀，合速 0~200/200~400/
// 400~600 的空刀率是 5%/5%/2%；边际上「越快越空」是「飞得快时更常在框外出刀」的伪装。
//
// ⚠️ 出刀闸必须在**全部五处**门禁上同步生效，少一处就会两头对推（BIN 1394b0 实证）：
//   ① `MoveTo` 的 `heli_in_band` 到位判定
//   ② `Aim` 的 `heli_hover`
//   ③ `Aim` 的 `InHitBand / InMeleeHoldBand` 旁路
//   ④ `Recover` 的 `heli_hold` / `heli_near`
//   ⑤ `Firing` 的 P0 硬门（`FireGateOk` 是地面口径，超限须一票否决）
// 出刀闸 = 客户端自己的命中盒，**只否决几何上不可能的刀**。
// 数值取自 34,360 个 op=50 命中包反解出的真实边界（`_hitbox.py`）：命中被接受的
// dx 最远 136（p99 103）、dy 最远 ±61（p99 ±33），且各 dy 带的 dx 触及距离一致 —— 是矩形不是椭圆。
// 两道闸（BIN 01:00）：|dy|≥140 首刀几乎必空，却仍吃面板间隔 → 空挥拖慢首伤。
// · 进态带（宽）：MoveTo→Firing 早切，飞途预转向 / 持火，体感不在 MoveTo 干等。
// · 出刀带（紧）：真 TryFire；命中盒 dy≈±61，|dy|<60 空刀约 10%。
// 效率：acq→首火日志会变慢，acq→首段 lastHitted 应更快（少一张必空首刀）。
constexpr float kHeliFireMaxDy = 220.f;     // 进态宽带（MoveTo 途中预转向等）
constexpr float kHeliStrikeMaxDy = 50.f;    // 同锁续砍（内置档 / AABB 竖直半高地板）
// BIN 13:00 换怪首刀：|dy|<20 空 8%，20–50 空 51%，跨层首刀空 49%。首刀更严（仅内置档）。
constexpr float kHeliFirstLockMaxDy = 20.f;
constexpr float kHeliFireMaxDx = 280.f;
// 内置档首刀横向硬闸（命中盒 p99≈103）。自定义档改走站距 AABB，不再用这数盖射程。
constexpr float kHeliFirstLockMaxDx = 120.f;
// 同锁 |vy| 上限；首刀曾用 200（旧 BIN：|vy|≥200 空 62%）。
// BIN 20:49–20:57（带=70）：人已进出刀几何仍 fire hold vy，体感「冲到跟前顿一下」。
// 21:02 提到 450 后中速顿刀少了；仍常见 |vy| 550–700 被卡 → 再放到 600。
constexpr float kHeliStrikeMaxVy = 600.f;
constexpr float kHeliFirstLockMaxVy = 600.f;

// 自定义站距下这两个数必须放大，否则远程被一票否决（理由见 HeliHoverMaxDx 处）。
// 取 max 保证**只放宽、不收紧**：用户把 X 填成 5，仍按内置命中盒 120 判，
// 免得自定义反而砍掉本来打得中的刀。
//
// 退避的放宽已经在 HeliHoverMaxDx 里做过，这里跟着继承，不必再判一次。
float HeliFireMaxDx() {
    const float want = HeliHoverMaxDx();
    return want > kHeliFireMaxDx ? want : kHeliFireMaxDx;
}

float HeliFireMaxDy() {
    const HeliStand s = HeliStandOff();
    return std::fabs(s.y) + kHeliFireMaxDy;
}

// 自定义站距 → 角色攻击 AABB（真出刀门）。
// · 心：玩家 AbsPos (px, py)
// · 半宽 = 站距 X + 到位环半宽（kHeliStationDx）
//   旋翼「站好了」允许 |dx|∈[X−stationDx, X+stationDx]；若半宽只取 X，
//   人停在环外半带（X < |dx| ≤ X+40）会 Aim 空转：HeliStationOk=真、HeliStrikeOk=假
//   → BIN 14:09「怪面前不出刀」+ fire hold strike needDx=X 且 |d|≈X+1..15。
// · 竖直中心按站距 Y 偏置：期望 my ≈ py − s.y（即 py − my ≈ s.y）
// · 半高 = max(|站距 Y|, 出刀竖直带)：|Y| 很小时（默认 −4）仍要能容飞行末段抖动
// 框内有怪 → 可砍；框外 → 继续飞。命中率归用户调（面板 tooltip 同口径）。
struct HeliAttackAabb {
    float halfX = 0.f;
    float halfY = 0.f;  // 相对 (py − my) − s.y 的容差
};
HeliAttackAabb HeliAttackAabbFromStand(const HeliStand& s, bool firstLock) {
    HeliAttackAabb a{};
    a.halfX = s.x + kHeliStationDx;
    const float floorY = firstLock ? kHeliFirstLockMaxDy : kHeliStrikeMaxDy;
    const float fromStandY = std::fabs(s.y);
    a.halfY = fromStandY > floorY ? fromStandY : floorY;
    return a;
}

// 到位判据 = 「96% 命中」那一格，只用来决定**还要不要继续飞**，不用来拦刀。
// 12,063 刀配对判决（`_hitrate.py`）：dy<15 且 dx<40 命中 96%，dx 放到 80 仍有 92%，
// 越过 dx 80 掉到 60%、越过 dy 30 掉到 53%。故旋翼一路把站位往这格里压。
// （kHeliStationDx/Dy 定义在文件前部，供 InHeliHoverBand 环带下沿共用。）

// 进态 / 够得着（宽 Y）：MoveTo 离开、Firing 不弹回 MoveTo。
bool HeliReachOk(float px, float py, float mx, float my) {
    return std::fabs(my - py) <= HeliFireMaxDy() && std::fabs(mx - px) <= HeliFireMaxDx();
}

// 真出刀。
// · 自定义站距：攻击 AABB（站距 X/Y）框内有怪才砍——不再另加首刀 dx120 硬闸盖射程。
// · 内置档：仍按命中盒经验带 + 首刀更严 dy/dx。
bool HeliStrikeOk(float px, float py, float mx, float my, bool firstLock) {
    const HeliStand s = HeliStandOff();
    if (s.custom) {
        const HeliAttackAabb box = HeliAttackAabbFromStand(s, firstLock);
        if (std::fabs(mx - px) > box.halfX) return false;
        return std::fabs((py - my) - s.y) <= box.halfY;
    }
    const float maxDy = firstLock ? kHeliFirstLockMaxDy : kHeliStrikeMaxDy;
    if (std::fabs((py - my) - s.y) > maxDy) return false;
    const float wideDx = HeliFireMaxDx();
    const float maxDx =
        firstLock ? (wideDx < kHeliFirstLockMaxDx ? wideDx : kHeliFirstLockMaxDx) : wideDx;
    return std::fabs(mx - px) <= maxDx;
}

// 「站位是否已经够好、可以不再挪」——比出刀闸严得多，二者**必须分开**。
//
// 横向阈值改由 reach_cal 在线自校准：客户端判定是「攻击盒 × 怪体盒」相交，攻击盒随**武器**
// 变、怪体盒随**怪种**变，写死一个数对谁都不对。44 台客户机实测里 7 台有真断崖（|dx| 20~80
// 不等），且**同机不同角色结论相反**（95C577CA51C72B0 的两个角色：断崖@50 vs 到 80 不掉），
// 硬件/网络已被控制住。详见 reach_cal.h 与模块设计文档。
//
// kHeliStationDx 退化为**冷启动兜底**：样本不足、或没测到断崖时一律用它（自校准只许放宽、
// 不许收紧，见 reach::StationDx）。且系数取 0.60 使主测机（edge=65）解出 39≈40，
// 对现状是恒等变换 —— 只有射程明显更长/更短的角色才会被挪动。
//
// 竖直不校准：dy 分布本就压在最优格上（中位 +9~10，主峰 [10,15) 命中 91.7%），
// 两侧是薄尾，动它是零和（账见模块设计文档「dy<0 也不值得治」）。
bool HeliStationOk(float px, float py, float mx, float my) {
    const HeliStand s = HeliStandOff();
    // +Y 向上 ⇒ relY 为正表示站在怪上方，与 s.y 同号。内置档 s.y=0，退化成旧的 |dy|<=15。
    const float relY = py - my;
    if (std::fabs(relY - s.y) > kHeliStationDy) return false;
    const float dx = std::fabs(mx - px);
    // 自定义档的「到位」是**环带**（落在设定站距附近），不是「离怪足够近」。
    // 沿用后者的话，远程站在自己射程上时 |dx| 恒大于 40 ⇒ 每拍都判 need_station_keep
    // ⇒ 明明站好了却反复重进 MoveTo，出刀窗被这来回抖没了。
    if (s.custom) {
        // 退避生效时「站好了」的定义换成「到了退避点」。否则退开的那一步会被判成没站好，
        // 旋翼立刻又把人拉回怪身上，两边对推 —— 这正是本文件反复警告的自激形态。
        if (gDodge.active) return std::fabs(px - gDodge.x) <= kHeliStationDx;
        return std::fabs(dx - s.x) <= kHeliStationDx;
    }
    return dx <= reach::StationDx(kHeliStationDx);
}

// 进态带 ≠ 悬停环 ≠ 真出刀。悬停环答「还要不要飞」；InHeliFireBand=宽进态；
// 真挥刀见 HeliStrikeOk。贴脸也放行进态——「贴着就准备砍」。
//
// 曾用 X−40 / minLandAway 挡刀：BIN 换锁已 hop 够砍仍进 MoveTo 空等。
bool InHeliFireBand(float px, float py, float mx, float my) {
    return HeliReachOk(px, py, mx, my);
}

bool NeedsHeliStationKeep(float px, float py, float mx, float my) {
    // 曾经这里跟着出刀带走（够不到才飞），等于「能砍就不再挪」，站位便长期停在 60% 命中那片区。
    // 现在改成盯 96% 格：**边打边挪**——出刀闸宽、到位判据窄，两者不会互锁。
    // 之所以不能反过来（拦刀去换站位），是因为边际账是负的：dx 80~100 那 1,772 刀命中 58%，
    // 拦掉只省 744 次空挥≈91s，而这 91s 按 86.4% 本能换 639 次命中，却要丢掉 1,028 次。净亏 389。
    return !HeliStationOk(px, py, mx, my);
}

// 悬停站位点：怪旁 standOff、略高于怪心；**X** 夹进 Combat 左右框（raw×0.95），Y 不夹。
// 给的是「要待着的地方」，不是「这一拍往哪推」——推多少由旋翼按 P 控 + 重力前馈自己算。
//
// ★ 左右竖边：强制站**地图内侧**朝外打（BIN 弓箭手 205 / 08:37 框沿空挥）。
//   1) 外侧站位 mx∓X 越可位移框；或两侧都越界 → 朝图心；
//   2) **更早（BIN 08:37）**：人已在朝该缘一侧，且人到该缘余量 < X
//      （怪从内侧贴过来时，等「外侧站位 OOB」才翻 → 人先被挤到框沿、|dx|被压半、whiff）。
void BuildHeliStationPoint(float px, float py, float mx, float my, float* outX, float* outY,
                           int* outSide) {
    const HeliStand s = HeliStandOff();
    int side = gLandSide;
    if (side == 0) side = (px >= mx) ? 1 : -1;
    // 已明确在某一侧：锁侧，避免左右拍打像甩头（边怪内侧强制可覆盖）。
    if (std::fabs(mx - px) >= kMinLandAway) side = (px >= mx) ? 1 : -1;

    float l = 0.f, t = 0.f, ri = 0.f, b = 0.f;
    if (heli::QueryCombatMoveBounds(&l, &t, &ri, &b) && s.x > 0.f) {
        const float leftSt = mx - s.x;
        const float rightSt = mx + s.x;
        const bool leftOob = leftSt < l;
        const bool rightOob = rightSt > ri;
        if (leftOob && !rightOob) {
            side = 1;  // 左竖边：站怪右侧（图内）朝左打
        } else if (rightOob && !leftOob) {
            side = -1;  // 右竖边：站怪左侧（图内）朝右打
        } else if (leftOob && rightOob) {
            const float cx = 0.5f * (l + ri);
            side = (mx <= cx) ? 1 : -1;
        } else {
            // 人侧余量：人在怪左且距左缘 < X → 再往左保站距会顶框；对称右缘。
            // 只朝**更挤的那一侧**翻，同拍最多一次——否则宽 < 2X 时左右 if 互打（REVIEW）。
            const float roomL = px - l;
            const float roomR = ri - px;
            if (side < 0 && roomL < s.x && roomL <= roomR) {
                side = 1;
            } else if (side > 0 && roomR < s.x && roomR < roomL) {
                side = -1;
            }
        }
    }

    float tx = mx + static_cast<float>(side) * s.x;
    // 防贴脸退避：站位点整体让给解算器（它已把左右框、禁区、离 ideal 最近三件事一起算过）。
    // 总闸未开时这行不生效，tx 就是上面那个原始站位点。
    if (gDodge.active) tx = gDodge.x;
    // 内置档相对怪心 Y=kHeliLiftPx（现 -4）；+Y 向上，抬高是加不是减。死区上偏会把落点自然带到
    // +11~12，而那正是实测最优命中带 [8,15) 的正中——别去"修正"它。
    float ty = my + s.y;
    ClampAimInPlayBounds(&tx, &ty);
    if (outX) *outX = tx;
    if (outY) *outY = ty;
    if (outSide) *outSide = side;
    (void)py;
    (void)t;
    (void)b;
}

// ── 防贴脸退避（LiveStep）· 解算与三级安全回退 ─────────────────────────────
//
// 触发半径参考面板「自定义站距 X/Y」，但**各自夹进一个区间**（见下方 kDodgeMinRx 等）：
// 那两个数表达的是「我想站多远」，不是「安全距离要多大」，直接照搬会把约束绷死。
// 不另开一组参数，是不想让用户对着两套数猜。
//
// 只退 X。纵向不退：那要改悬停高度，会跟 gStationStickY 钉点防抖和悬停带互打。
constexpr float kDodgeReleaseFrac = 1.15f;   // 迟滞：进用 Rx，出用 1.15×Rx
// 净空半径 = clamp(站距X, 40, 80)，**不等于站距**。
//
// 上限 80 是关键。要躲的只是那个挥弓框：实测近战找怪矩形 30 宽（BIN 11:48 `mrect=[x y 30 87]`），
// 加上怪自身体宽，约 60 就够保证不挥弓，80 再留一档旋翼跟踪误差。
// 直接拿站距当净空（用户填 103 就要求离每只怪 103）会把约束绷得极紧：几只怪一聚，可行区被
// 切成很窄的碎片，解算容易在碎片间跳，角色就被甩来甩去（BIN 13:09 实测一秒跳 600px）。
// 上限还有个好性质：净空 ≤ 站距 ⇒ 原站位点相对**锁怪**永远是可行解，退避只会因为**别的怪**
// 而挪人，正合本功能的本意。
constexpr float kDodgeMinRx = 40.f;   // 下限：挥弓框 30 宽 + 余量
constexpr float kDodgeMaxRx = 80.f;   // 上限：见上
constexpr float kDodgeMinRy = kHeliFireMaxDy;   // 纵向筛选下限 = 实测命中盒半高 45
constexpr float kDodgeMaxRy = 2.f * kHeliFireMaxDy;  // 上限 ≈ 挥弓框全高，别把远上下的怪算进来
constexpr float kDodgeMaxStandMul = 2.0f;    // 退避点离锁怪最远 = 站距×2 + Rx
constexpr float kDodgeFireSlackPx = 24.f;    // 出刀闸放宽余量
constexpr float kDodgeBoundInsetPx = 24.f;   // 离左右可位移框留的余量
constexpr float kDodgeEpsPx = 2.f;           // 触发判据的容差，防在等号上反复点亮
constexpr DWORD kDodgeHoldMaxMs = 1500;      // 第 2 级：压刀上限，超时放弃退避
constexpr DWORD kDodgeStarveMs = 30000;      // 第 3 级：看门狗窗口
constexpr DWORD kDodgeCrossDelayMs = 700;    // 近侧连续无解这么久，才准穿到锁怪另一侧

// 跨拍的回退账本。与 gDodge 分开：gDodge 每拍重算，这一份要活过多拍。
struct DodgeRuntime {
    bool softOff = false;      // 第 3 级：看门狗熄火（本次注入内有效，不改用户勾选）
    bool giveUp = false;       // 第 2 级：本锁放弃退避
    int32_t giveUpLock = 0;
    DWORD holdSince = 0;       // 因退避未就位而压刀的起点
    DWORD activeSince = 0;     // 连续处于退避的起点
    DWORD lastFireMs = 0;      // 上一次真的出刀
    // 落点粘滞：上一拍选中的退避点。只要它还安全、还够得着，就一直待着不重选。
    bool haveLast = false;
    float lastX = 0.f;
    int32_t lastLock = 0;
    DWORD nearFailSince = 0;   // 近侧连续解不出来的起点；跨侧闸靠它开
};
DodgeRuntime gDodgeRt;
// 复位请求。gDodge / gDodgeRt 是战斗 worker 独占的裸结构，换图与面板下发来自别的线程，
// 直接去清就是数据竞争 —— 改成投个原子标记，由 worker 在 ComputeDodge 里自己消费。
std::atomic<bool> gDodgeResetReq{false};

// 任意线程可调；实际清空发生在下一拍（16ms 内）。
void RequestDodgeReset(const char* why) {
    gDodgeResetReq.store(true, std::memory_order_release);
    if (why) LogLine("dodge reset why=%s", why);
}

// 出刀成功回执：看门狗靠它判断「开了退避之后还打不打得出去」。
void NoteDodgeFire(DWORD now) {
    gDodgeRt.lastFireMs = now ? now : 1;
}

// 每 tick 一次，在 pass 循环之前算；pass 内只读。
//
// **任何一条 return 都等于「本拍无退避」**（gDodge 在函数开头已清零）——这就是第 1 级回退：
// 解算不出来就当这功能不存在，站位点回到 mx ± standOff，绝不做补偿动作。
void ComputeDodge(DWORD now, float px, float py, const ports::mob::Snapshot& snap) {
    bool wasActive = gDodge.active;
    gDodge = DodgeState{};
    if (gDodgeResetReq.exchange(false, std::memory_order_acq_rel)) {
        // 换图 / 面板开关：把 latch、放弃闩、熄火状态一起丢掉，重新给一次机会。
        gDodgeRt = DodgeRuntime{};
        wasActive = false;
    }

    if (!gAntiHugEnabled.load(std::memory_order_acquire)) {
        if (gDodgeRt.softOff || gDodgeRt.giveUp || gDodgeRt.activeSince) gDodgeRt = DodgeRuntime{};
        return;
    }
    if (gDodgeRt.softOff) return;  // 第 3 级已熄火：等换图或重开
    if (!gImpactApproachEnabled.load(std::memory_order_acquire)) return;  // 只做空中档
    if (!gLock.id || !snap.ok) return;

    const HeliStand s = HeliStandOff();
    if (!s.custom) {
        // 半径取自自定义站距；没开就没有半径可用。只提示一次，别刷屏。
        static DWORD sWhy = 0;
        if (!sWhy || now - sWhy > 30000) {
            sWhy = now;
            LogLine("dodge idle why=need_custom_standoff");
        }
        return;
    }

    // 换怪即解除上一只怪的放弃闩，否则一次 give_up 会连累后面所有怪。
    if (gDodgeRt.giveUpLock != gLock.id) {
        gDodgeRt.giveUp = false;
        gDodgeRt.giveUpLock = gLock.id;
        gDodgeRt.haveLast = false;  // 换怪即弃旧落点，勿把上一只怪的位置粘过来
        gDodgeRt.nearFailSince = 0;
    }
    if (gDodgeRt.giveUp) return;

    // 站距 X/Y 只当「参考量级」，实际净空/筛选半径夹进上面那两组区间，理由见常量注释。
    const float rx = std::fmin(std::fmax(s.x, kDodgeMinRx), kDodgeMaxRx);
    // Y 只做纵向筛选（这只怪算不算贴脸），不参与退避方向。下限取实测命中盒半高：
    // 用户把 Y 填成 11 时若照搬，几乎没有怪能入选，功能等于白开；上限防把远处上下层的怪算进来。
    const float ry = std::fmin(std::fmax(std::fabs(s.y), kDodgeMinRy), kDodgeMaxRy);

    float bl = 0.f, bt = 0.f, br = 0.f, bb = 0.f;
    if (!heli::QueryCombatMoveBounds(&bl, &bt, &br, &bb)) return;  // 拿不到框就不退，免得飞出图
    (void)bt;
    (void)bb;
    const float loX = bl + kDodgeBoundInsetPx;
    const float hiX = br - kDodgeBoundInsetPx;
    if (hiX <= loX) return;

    // 此刻 gDodge.active 已是 false，所以这次拿到的是**未经退避**的原始站位点。
    float idealX = 0.f, idealY = 0.f;
    int side = 0;
    BuildHeliStationPoint(px, py, gLock.x, gLock.y, &idealX, &idealY, &side);

    // 禁区：纵向落在 ±ry 的活怪，各自在 X 上占 [m.x-rx, m.x+rx]。
    // 纵向基准取 idealY（我们打算待的高度），而不是当前 py —— 问的是「飞到位之后会不会被贴脸」。
    struct Band {
        float lo;
        float hi;
    };
    Band bands[ports::mob::kMaxLiteMobs];
    int nb = 0;
    int n = snap.count;
    if (n > ports::mob::kMaxLiteMobs) n = ports::mob::kMaxLiteMobs;
    for (int i = 0; i < n; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
        if (std::fabs(m.y - idealY) > ry) continue;
        bands[nb].lo = m.x - rx;
        bands[nb].hi = m.x + rx;
        ++nb;
    }
    if (nb == 0) return;

    // 触发判据问的是「照原计划站过去，会不会被贴脸」，所以比的是理想站位点而不是当前位置。
    //
    // ⚠️ 锁定目标一定在 bands 里（解算不会把人退进它怀里），但它**几乎不会**成为这里的
    // intruder：idealX 按定义就是离它 s.x 的地方，恒等于净空半径。这是对的 —— 锁怪本身
    // 的距离归站距管，本功能只解「别的怪冲过来」。故用严格小于 + 2px 容差，免得锁怪永远
    // 卡在等号上把退避一直点亮。
    const float testRx = (wasActive ? rx * kDodgeReleaseFrac : rx) - kDodgeEpsPx;
    int intruders = 0;
    for (int i = 0; i < nb; ++i) {
        const float mx = 0.5f * (bands[i].lo + bands[i].hi);
        if (std::fabs(mx - idealX) < testRx) ++intruders;
    }
    if (intruders == 0) return;  // 理想站位本来就干净，不必退

    auto safeAt = [&](float cx) {
        for (int i = 0; i < nb; ++i) {
            if (cx > bands[i].lo && cx < bands[i].hi) return false;
        }
    return true;
    };
    // 离最近禁区中心还有多远：没有完全干净的点时，用它挑「最不糟」的一个。
    auto clearance = [&](float cx) {
        float best = 1e9f;
        for (int i = 0; i < nb; ++i) {
            const float mx = 0.5f * (bands[i].lo + bands[i].hi);
            const float d = std::fabs(cx - mx);
            if (d < best) best = d;
        }
        return best;
    };

    // 退避点离锁怪太远就没意义了（打不着还一路飞）。这个上界同时兜住 HeliHoverMaxDx 的放宽幅度。
    //
    // 「站距 + 一个净空半径」= 最多让你比设定的站位再退开一只怪的身位。BIN 13:09 实测把它
    // 放到 站距×2+净空（=309）时，角色被甩到离怪 300+ 的地方，`fmax` 一度到 323。
    const float maxStand = s.x + rx;
    auto usable = [&](float cx) {
        if (cx < loX || cx > hiX) return false;
        return std::fabs(cx - gLock.x) <= maxStand;
    };

    // ★ 同侧优先：不许为了找位置从锁怪身上穿过去。
    //
    // 「离 idealX 最近的可行点」不认边，怪另一侧的点常常真的更近，于是解算会让人横穿怪身。
    // 这既是最难看的一类甩动，也恰好是最容易吃挥弓的走位 —— 穿过去的路上必然经过挥弓框。
    // BIN 13:29 实测 81 次退避里 15 次是这种穿越：位移全在 151~261，而同侧解**全部** ≤80
    // （上界就是 maxStand-站距），中间 81~150 一次都没有 —— 两类解在数值上是分离的。
    // 最难看的一对是同一只怪连续两拍 `877->682` 与 `800->984`，一秒摆了 302px。
    //
    // 近侧一时无解也先别急着穿：憋满 kDodgeCrossDelayMs 再放行，多半那只怪自己就走开了。
    // 憋的这段时间由下面的 fallback 分支接管（clear=0 → Firing 压刀，仍有 1500ms 上限）。
    // 优先搜**站位侧**（idealX），不是人当前侧。边怪被 BuildHeliStationPoint 强制内侧后，
    // 人若还贴在外侧/怪心，按 curSide 搜只会落在 OOB 半区 → 近侧无解 → 憋 cross 或直接
    // 放弃退避，贴脸刀继续出（与「防贴脸」目标相反）。
    const int curSide = (px >= gLock.x) ? 1 : -1;
    const int preferSide =
        (std::fabs(idealX - gLock.x) > kDodgeEpsPx) ? ((idealX >= gLock.x) ? 1 : -1) : curSide;
    const bool crossOk =
        gDodgeRt.nearFailSince && (now - gDodgeRt.nearFailSince) >= kDodgeCrossDelayMs;
    auto sideOk = [&](float cx) {
        if (crossOk) return true;
        const float d = cx - gLock.x;
        if (std::fabs(d) <= kDodgeEpsPx) return true;
        return ((d > 0.f) ? 1 : -1) == preferSide;
    };

    // ★ 落点粘滞：上一拍的退避点只要还安全、还够得着，就原样留着，**不重选**。
    //
    // 少了这一条会把角色在图上来回甩。「离 idealX 最近的可行点」对怪的位置**不连续**：
    // 几只怪的禁区连成一片时，可行点只剩这片的左右两个边缘，idealX 一越过中点，答案就
    // 从左边缘瞬间跳到右边缘。BIN 13:09 实测 idealX 只从 1098 挪到 1022，解却从 -330
    // 翻成 +389，站位点一秒内 768→1419，旋翼被指令用 3724 的速度横冲，出刀 dx 在
    // ±300 之间穿来穿去。
    //
    // 上面那个 1.15 迟滞只管「要不要退」，管不了「退到哪」—— 这里才是稳定性的那一环。
    // 不必担心粘太久：usable() 会把它夹在离锁怪 maxStand 之内，怪走远了自然失效重选。
    float bestX = 0.f;
    bool found = false;
    bool sticky = false;
    if (gDodgeRt.haveLast && gDodgeRt.lastLock == gLock.id && usable(gDodgeRt.lastX) &&
        safeAt(gDodgeRt.lastX)) {
        bestX = gDodgeRt.lastX;
        found = true;
        sticky = true;
        gDodgeRt.nearFailSince = 0;
    }

    // 候选 = 各禁区端点 + 理想点本身。端点即「刚好贴着净空边缘」，最优解必在其中。
    float bestScore = 0.f;
    if (!sticky) {
        auto consider = [&](float cx) {
            if (!usable(cx) || !sideOk(cx) || !safeAt(cx)) return;
            const float d = std::fabs(cx - idealX);
            if (!found || d < bestScore) {
                found = true;
                bestX = cx;
                bestScore = d;
            }
        };
        consider(idealX);
        // usable() 的两个边界本身也是候选：最优解被「离锁怪 maxStand」这条夹住时，答案就落在
        // 这里，光试禁区端点会漏掉，然后误判成近侧无解、白白把跨侧闸憋开。
        consider(gLock.x - maxStand);
        consider(gLock.x + maxStand);
        for (int i = 0; i < nb; ++i) {
            consider(bands[i].lo);
            consider(bands[i].hi);
        }
        // 解出来就把跨侧闸重新上锁：下一次要穿，得重新憋满 kDodgeCrossDelayMs。
        if (found) {
            gDodgeRt.nearFailSince = 0;
        } else if (!gDodgeRt.nearFailSince) {
            gDodgeRt.nearFailSince = now ? now : 1;
        }
    }

    if (!found) {
        // 清不出干净位：退向「离怪最远」的可用点，本拍交由 Firing 决定压不压刀（有 1500ms 上限）。
        float fbX = 0.f;
        float fbScore = -1.f;
        bool fb = false;
        auto considerFallback = [&](float cx) {
            if (!usable(cx) || !sideOk(cx)) return;
            const float c = clearance(cx);
            if (!fb || c > fbScore) {
                fb = true;
                fbX = cx;
                fbScore = c;
            }
        };
        considerFallback(idealX);
        considerFallback(loX);
        considerFallback(hiX);
        considerFallback(gLock.x - maxStand);
        considerFallback(gLock.x + maxStand);
        for (int i = 0; i < nb; ++i) {
            considerFallback(bands[i].lo);
            considerFallback(bands[i].hi);
        }
        if (!fb) return;  // 连退的地方都没有 → 第 1 级回退，当无退避
        bestX = fbX;
    }

    // ★ 解出来就是原点 = 没有任何怪真的把我们推开 → **不要点亮总闸**。
    //
    // 少了这一条会长期空转激活：触发判据比的是「怪离 idealX 多远」，而锁定目标按定义恰好
    // 站在 s.x 上；释放半径 1.15×rx 比 s.x 大，于是锁怪永远落在释放带内，latch 一旦合上
    // 就灭不掉。实机 BIN 12:36 实测 140 次激活里 84 次是这种空转（日志 `ideal=324->324`、
    // `fmax` 恒等于 s.x+24），位置没动，却让 HeliStationOk / HeliHoverMaxDx 一直走退避分支
    // —— 等于勾上之后判据被悄悄改了，违背「关掉/没怪贴脸时与原逻辑逐位相同」。
    //
    // 这里不会反过来引发抖动：判据的自变量是 idealX（只由锁怪决定），与我们退不退无关，
    // 不存在「退开→判据变→又退回来」的反馈环。
    if (found && std::fabs(bestX - idealX) < 1.f) {
        gDodgeRt.haveLast = false;
        return;
    }

    gDodge.active = true;
    gDodge.clear = found;
    gDodge.x = bestX;
    gDodge.intruders = intruders;
    gDodge.fireMaxDx = std::fabs(bestX - gLock.x) + kDodgeFireSlackPx;
    gDodgeRt.haveLast = true;
    gDodgeRt.lastX = bestX;
    gDodgeRt.lastLock = gLock.id;

    if (!wasActive) {
        gDodgeRt.activeSince = now ? now : 1;
        gDodgeRt.lastFireMs = now ? now : 1;  // 起算点：别把开退避之前的空窗算到它头上
    }

    // 第 3 级 · 看门狗：连续退避满 30s 且这 30s 一刀没出去 → 说明退避把输出彻底堵死了，
    // 自动软关闭。只在本次注入内生效，**不动用户的勾选**——意图保留，重开或重勾即可再试。
    if (gDodgeRt.activeSince && now - gDodgeRt.activeSince >= kDodgeStarveMs &&
        now - gDodgeRt.lastFireMs >= kDodgeStarveMs) {
        LogLine("dodge watchdog_off why=fire_starved window=%ums intruders=%d rx=%.0f ry=%.0f",
                (unsigned)kDodgeStarveMs, intruders, rx, ry);
        x::runtime::LogW("SimpleCombat",
                         "防贴脸退避已自动关闭：开启后 30s 未能出刀。勾选保留，换图或重新勾选可再试。");
        gDodge = DodgeState{};
        gDodgeRt.softOff = true;
        return;
    }

    static DWORD sLog = 0;
    if (!sLog || now - sLog > 1000) {
        sLog = now;
        // cross=1 才是「从怪身上穿过去了」。它应当罕见；若又变多，先看是不是 nearFailSince
        // 被什么路径反复清零，而不是急着调 kDodgeCrossDelayMs。
        const bool crossed = ((bestX - gLock.x) > 0.f ? 1 : -1) != preferSide &&
                             std::fabs(bestX - gLock.x) > kDodgeEpsPx;
        LogLine(
            "dodge on id=%d n=%d rx=%.0f ry=%.0f ideal=%.0f->%.0f clear=%d stick=%d cross=%d "
            "fmax=%.0f",
            gLock.id, intruders, rx, ry, idealX, bestX, found ? 1 : 0, sticky ? 1 : 0,
            crossed ? 1 : 0, gDodge.fireMaxDx);
    }
}

// Firing 的退避门：退避激活但没退到干净位时压刀。
// 留在 Firing 原地 break（勿 EnterState —— Firing↔Recover 会按 tick 自激，见 engine_busy 处）。
bool DodgeHoldsFire(DWORD now) {
    if (!gDodge.active || gDodge.clear) {
        gDodgeRt.holdSince = 0;
        return false;
    }
    if (!gDodgeRt.holdSince) gDodgeRt.holdSince = now ? now : 1;
    const DWORD held = now - gDodgeRt.holdSince;
    if (held >= kDodgeHoldMaxMs) {
        // 第 2 级回退：无限期「安全优先」会变成站着挨打不还手。宁可挨一下挥弓。
        LogLine("dodge give_up id=%d why=hold_timeout held=%ums n=%d", gLock.id, held,
                gDodge.intruders);
        gDodgeRt.giveUp = true;
        gDodgeRt.giveUpLock = gLock.id;
        gDodgeRt.holdSince = 0;
        gDodgeRt.activeSince = 0;
        gDodge = DodgeState{};
        return false;
    }
    static DWORD sHold = 0;
    if (!sHold || now - sHold > 500) {
        sHold = now;
        LogLine("dodge hold_fire id=%d n=%d held=%ums", gLock.id, gDodge.intruders, held);
    }
    return true;
}

// 需要弃战自救吗？**只判左右竖边**（出 Combat 可位移框 left/right）。
// 上下（top/bottom）不进 RTB——竖直仍由 A 层包线与站位点夹取管。
// 已回左右框内即停 RTB（可 Station）；latch 深入后清除。走门由用户关 F5/F6。
constexpr float kRtbAimInsetPx = 48.f;
constexpr float kRtbExitInsetPx = 48.f;
bool gHeliRtbLatched = false;

// RTB latch 清闩：只看水平是否深入左右框（与 NeedsHeliRtb 同口径）。
bool CombatMoveDeepInsideX(float px, float inset) {
    float l = 0.f, t = 0.f, ri = 0.f, b = 0.f;
    if (!heli::QueryCombatMoveBounds(&l, &t, &ri, &b)) return true;
    (void)t;
    (void)b;
    float ix = inset;
    const float halfW = 0.5f * (ri - l);
    if (ix > halfW) ix = halfW;
    if (ix < 0.f) ix = 0.f;
    return px >= l + ix && px <= ri - ix;
}

void ComputeRtbAim(float px, float py, float* outX, float* outY) {
    float l = 0.f, t = 0.f, ri = 0.f, b = 0.f;
    if (!heli::QueryCombatMoveBounds(&l, &t, &ri, &b)) {
        if (outX) *outX = px;
        if (outY) *outY = py;
        return;
    }
    (void)t;
    (void)b;
    float k = kRtbAimInsetPx;
    const float halfW = 0.5f * (ri - l);
    if (k > halfW) k = halfW;
    if (k < 1.f) k = 1.f;
    // 只内缩 X；Y 保持当前高度，不去拽上下界。
    float tx = px;
    if (tx < l + k) tx = l + k;
    if (tx > ri - k) tx = ri - k;
    if (tx < l) tx = l;
    if (tx > ri) tx = ri;
    if (outX) *outX = tx;
    if (outY) *outY = py;
}

bool NeedsHeliRtb(float px, float py, float* outX, float* outY) {
    float l = 0.f, t = 0.f, ri = 0.f, b = 0.f;
    if (!heli::QueryCombatMoveBounds(&l, &t, &ri, &b)) {
        gHeliRtbLatched = false;
        return false;
    }
    (void)t;
    (void)b;
    const bool outsideX = px < l || px > ri;
    if (outsideX) {
        gHeliRtbLatched = true;
        ComputeRtbAim(px, py, outX, outY);
        return true;
    }
    if (gHeliRtbLatched && CombatMoveDeepInsideX(px, kRtbExitInsetPx)) {
        gHeliRtbLatched = false;
    }
    return false;
}

// 人在 Combat 左右可位移框外吗？Impact 出刀硬闸（只闸 L/R；Y 不闸）。
// 无 bounds 返回 false：宁可放行，也不能因为读不到图信息就哑火。
bool PlayerOutOfPlayBounds(float px, float py) {
    float l = 0.f, t = 0.f, ri = 0.f, b = 0.f;
    if (!heli::QueryCombatMoveBounds(&l, &t, &ri, &b)) return false;
    (void)py;
    (void)t;
    (void)b;
    return px < l || px > ri;
}

// 把「打哪只、站哪儿」翻译成旋翼 setpoint。每 tick 可重复调，幂等。
void PublishHeliSetpoint(DWORD now, float px, float py, bool haveLock) {
    if (!HeliBaseArmed() || !CombatSpawnAllowsBan()) {
        heli::Disarm(heli::Owner::Combat);
        gHeliHoldValid = false;
        gHeliRtbLatched = false;
        return;
    }
    heli::Setpoint rtbSp{};
    const bool needRtb = NeedsHeliRtb(px, py, &rtbSp.x, &rtbSp.y);
    const bool airborneGrace =
        gHeliAirborneUntilMs != 0 && static_cast<int>(now - gHeliAirborneUntilMs) < 0;
    // BIN adfed6：无锁且宽限已尽时仍 TryAcquire→立刻 Disarm → owner combat↔none 抖振。
    if (!haveLock && !needRtb && !airborneGrace) {
        heli::Disarm(heli::Owner::Combat);
        gHeliHoldValid = false;
        gStationStickValid = false;
        gStationStickLockId = 0;
        return;
    }
    // SetSetpoint 要求持有 Combat；断线 Sync 会 Release。仅无主时清 bail 并抢回，
    // 不碰 F6/Travel 持有期间的 bail 闩。
    if (heli::CurrentOwner() == heli::Owner::None) {
        if (heli::Bailed()) heli::ClearBailed();
        (void)heli::TryAcquire(heli::Owner::Combat);
    }
    heli::Setpoint sp{};
    if (needRtb) {
        sp.mode = heli::Mode::Rtb;
        sp.x = rtbSp.x;
        sp.y = rtbSp.y;
        gHeliAirborneUntilMs = now + kHeliAirborneGraceMs;
        gHeliHoldValid = false;  // RTB 期间禁止 Hold 钉危险点（BIN 07:00 抖）
        heli::SetSetpoint(heli::Owner::Combat, sp);
        return;
    }
    if (haveLock && gLock.id) {
        int side = 0;
        float idealX = 0.f, idealY = 0.f;
        BuildHeliStationPoint(px, py, gLock.x, gLock.y, &idealX, &idealY, &side);
        if (side != 0) gLandSide = side;

        ports::teleport::FlightState standSt{};
        const bool standMa =
            ports::teleport::QueryFlightState(standSt) && CombatStandMa(standSt);
        // 只从真挂台上抬飞。ma=4/5 且 CurFh=0 是进图真空站姿，+48 只会把人送进虚空。
        const bool standTakeoff = standMa && standSt.onFh;
        if (standTakeoff) {
            idealY += kHeliStandTakeoffLiftY;
            gStationStickValid = false;
            static DWORD sTakeoff = 0;
            if (!sTakeoff || now - sTakeoff > 400) {
                sTakeoff = now;
                LogLine("heli takeoff lift ma=%d +%.0f ap=(%.0f,%.0f) sp=(%.0f,%.0f)",
                        standSt.ma, kHeliStandTakeoffLiftY, px, py, idealX, idealY);
            }
        }

        if (gAntiJitterEnabled.load(std::memory_order_acquire) && !standTakeoff) {
            if (gStationStickLockId != gLock.id) {
                gStationStickValid = false;
                gStationStickLockId = gLock.id;
            }
            // 出刀带内钉 **Y**（ReachOk）。X 始终跟 ideal。
            // 怪心 Y 蠕动/起跳：立刻跟 ideal（勿等旧 6px 破钉 → BIN 慢一拍）。
            const bool canStick = HeliReachOk(px, py, gLock.x, gLock.y);
            if (gStationStickValid && canStick) {
                const float mobDy = std::fabs(gLock.y - gStationStickMobY);
                if (mobDy > kStationStickMobJumpY) {
                    // 明显跳变：落下重锁 ideal
                } else if (mobDy > kStationStickMobCreepY) {
                    // 起跳/落地蠕动：本拍就跟高，并刷新钉点基准
                    gStationStickY = idealY;
                    gStationStickMobY = gLock.y;
                    sp.mode = heli::Mode::Station;
                    sp.x = idealX;
                    sp.y = idealY;
                    gHeliAirborneUntilMs = now + kHeliAirborneGraceMs;
                    gHeliHoldValid = false;
                    heli::SetSetpoint(heli::Owner::Combat, sp);
                    return;
                } else {
                    const float dmy = std::fabs(idealY - gStationStickY);
                    if (dmy <= kStationStickYPx) {
                        sp.mode = heli::Mode::Station;
                        sp.x = idealX;
                        sp.y = gStationStickY;
                        gHeliAirborneUntilMs = now + kHeliAirborneGraceMs;
                        gHeliHoldValid = false;
                        heli::SetSetpoint(heli::Owner::Combat, sp);
                        return;
                    }
                }
            }
            gStationStickY = idealY;
            gStationStickMobY = gLock.y;
            gStationStickValid = true;
        } else {
            gStationStickValid = false;
            gStationStickLockId = 0;
        }

        sp.x = idealX;
        sp.y = idealY;
        const float dx = sp.x - px;
        const float dy = sp.y - py;
        const float d = std::sqrt(dx * dx + dy * dy);
        sp.mode = d > kHeliCruiseRadius ? heli::Mode::Cruise : heli::Mode::Station;
        gHeliAirborneUntilMs = now + kHeliAirborneGraceMs;
        gHeliHoldValid = false;
        heli::SetSetpoint(heli::Owner::Combat, sp);
        return;
    }
    gStationStickValid = false;
    gStationStickLockId = 0;
    // 无锁：宽限内钉住「进入 Hold 的那一刻」的点悬停（不可每拍跟当前位置，否则等于放任下坠）。
    // 走传送门由用户关 F5/F6，不在浅区无锁时强行 Disarm。
    if (gHeliAirborneUntilMs && static_cast<int>(now - gHeliAirborneUntilMs) < 0) {
        if (!gHeliHoldValid) {
            gHeliHoldValid = true;
            gHeliHoldX = px;
            gHeliHoldY = py;
            ClampAimInPlayBounds(&gHeliHoldX, &gHeliHoldY);
        }
        sp.mode = heli::Mode::Hold;
        sp.x = gHeliHoldX;
        sp.y = gHeliHoldY;
        heli::SetSetpoint(heli::Owner::Combat, sp);
        return;
    }
    heli::Disarm(heli::Owner::Combat);
    gHeliHoldValid = false;
}

// 最近一拍的旋翼速度。出刀日志要用它，而遥测每 500ms 才落一行、对不上单刀。
// 旋翼与战斗 FSM 同 tick，所以这份缓存与出刀瞬间的时差就是一个 tick。
// 读不到飞行状态（未起飞 / Impact 关）时归零：那种情况下是地面出刀，速度本就不参与判读。
float gHeliLastVx = 0.f;
float gHeliLastVy = 0.f;

// A 层入口：必须在 TickImpl 所有提前 return 之前调，否则 skill_prepare / 池未热身 /
// arm_grace 这些 return 会让旋翼停转，fh-ban 下就是自由落体。
void TickHeliRotor(DWORD now) {
    heli::Telemetry tm{};
    const bool fired = heli::Tick(heli::Owner::Combat, now, &tm);
    gHeliLastVx = tm.haveState ? tm.vx : 0.f;
    gHeliLastVy = tm.haveState ? tm.vy : 0.f;
    static DWORD sLog = 0;
    const char* g = tm.guard ? tm.guard : "";
    const bool notable =
        tm.emergency || std::strcmp(g, "impact_fail") == 0 || std::strcmp(g, "invuln_off") == 0;
    if (tm.mode == heli::Mode::Off && !notable) return;
    if (notable || !sLog || now - sLog > 500) {
        sLog = now;
        // des/trim/since 三个字段是竖直律的全部输入：净爬升权限 = cmdVy - trim，
        // 它若长期 ≤ 0 就是净下沉（历史三轮掉图全是这个形态），一眼可判。
        // des 是**意图速度**、cmd 是发出去的**增量**，两者本就不该相等（作动器是叠加语义）。
        // 校验横向作动器只需一条：下一拍的 v.x 应精确等于本拍的 desx。
        LogLine("heli mode=%s pos=(%.0f,%.0f) v=(%.0f,%.0f) sp=(%.0f,%.0f) cmd=(%.0f,%.0f) "
                "desx=%.0f des=%.0f trim=%.0f since=%u fired=%d emg=%d fh=%d guard=%s",
                heli::ModeName(tm.mode), tm.x, tm.y, tm.vx, tm.vy, tm.spX, tm.spY, tm.cmdVx,
                tm.cmdVy, tm.desiredVx, tm.desiredVy, tm.trimVy, tm.sinceMs, fired ? 1 : 0,
                tm.emergency ? 1 : 0, tm.onFh ? 1 : 0, g[0] ? g : "-");
    }
}

void GoIdle(DWORD now, const char* why) {
    ClearLock();
    ClearHitRotateState();
    gSettleUntil = 0;
    gSettleNeedPosSane = false;
    gSettleEnteredAt = 0;
    (void)ports::attack::StopWalk();
    EnterState(State::Idle, now, why);
    // Idle 不一定关 F5（pause/arm）；fh-ban 仍由 SyncImpactFhBan 按总开关收敛。
    SyncImpactFhBan();
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
    ClearHitRotateState();
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

    // fill+Doing 已禁用：若残留压测态，立刻停掉。
    (void)tx; (void)ty; (void)fh; (void)pickId; (void)hop; (void)poolN; (void)poolAlive;
    LogLine("kick_stress hop refused (native fill+Doing disabled) cd=%ums — stop", cd);
    FinishKickStress("native_tp_disabled", now);
    return;

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

    // 进图预装 fh-ban 钩（不抬 BAN）：F5 关着也会跑，避免热开首刀与首次改虚表同拍。
    if (!ports::fly_fh_ban::IsInstalled() && ports::world::IsPlayReady()) {
        static DWORD sFhBanWarmTryMs = 0;
        if (!sFhBanWarmTryMs || now - sFhBanWarmTryMs > 2000) {
            sFhBanWarmTryMs = now;
            (void)ports::fly_fh_ban::WarmInstall();
        }
    }

    // 踢号压测优先：跑时挂起普攻状态机，避免抢 CD / 抢皮。
    TickKickStress(now);
    if (gKickStressActive.load(std::memory_order_acquire)) {
        if (gState != State::Idle) GoIdle(now, "kick_stress");
        SyncImpactFhBan();
        // 测谎落台优先于压测早退：禁挂台挂着时必须继续转旋翼。
        if (gLieSafeLand.load(std::memory_order_acquire)) {
            TickLieSafeLand(now);
            TickHeliRotor(now);
            SyncImpactFhBan();
        }
        return;
    }

    if (!gEnabled.load(std::memory_order_acquire)) {
        ports::hit_pin::SetWishOid(0);
        if (gState != State::Idle) GoIdle(now, "disabled");
        SyncImpactFhBan();
        if (gLieSafeLand.load(std::memory_order_acquire)) {
            TickLieSafeLand(now);
            TickHeliRotor(now);
            SyncImpactFhBan();
        }
        return;
    }
    // 断线边沿 / soft settle：立刻 Idle + 卸刀；旋翼策略分 hold vs land quiet。
    // IsPlayReady 在 NM 断后仍常为真（InMap&&Alive），不能依赖 not_play（752824）。
    if (SoftOrNetQuiet()) {
        ports::hit_pin::SetWishOid(0);
        gHeliAirborneUntilMs = 0;
        gHeliHoldValid = false;
        const bool softHold = soft_login_probe::IsHoldActive();
        const bool landQuiet = soft_login_probe::IsLandQuiet();
        const char* why = softHold ? "soft_hold" : (landQuiet ? "soft_land_quiet" : "nm_down");

        // soft_hold / nm_down：拆落台并清 MapArrive（BIN fada72：abort 后 MapArrive 永挂）。
        // soft_land_quiet：已回图——悬空则开落台抗重力，勿清 MapArrive（BIN 681ebe）。
        if (softHold || !landQuiet) {
            if (gLieSafeLand.load(std::memory_order_acquire)) {
                EndLieSafeLand("soft_or_net_quiet");
            }
            ReleaseMapArriveIfHeld();
            // F5 还开着：软静默停飞。结束后 HeliBaseArmed 会再武装（build 132 先起飞）。
            if (gHeliLatchedThisEnable) UnlatchCombatHeli(why);
            if (gState != State::Idle) GoIdle(now, why);
            else SyncImpactFhBan();
            ports::attack::ForceRelease();
        } else {
            if (gState != State::Idle) GoIdle(now, why);
            ports::attack::ForceRelease();
            ports::teleport::FlightState st{};
            if (ports::teleport::QueryFlightState(st) && st.ok && !st.onFh) {
                // 软重连后 VecCtrl 偶发幽灵点（BIN f99271/681ebe/706c42：x≈-1533）。
                // 界外或战斗 AABB 不可用时先等坐标可信，避免 Snap 到千里外再 fly 穿越。
                float bl = 0, bt = 0, br = 0, bb = 0;
                const bool haveBb = heli::QueryCombatMoveBounds(&bl, &bt, &br, &bb);
                // QueryCombatMoveBounds：top < bottom（与 map_bounds 同向）。
                const bool oob =
                    !haveBb || st.x < bl || st.x > br || st.y < bt || st.y > bb;
                if (oob) {
                    static DWORD sGhostLog = 0;
                    if (!sGhostLog || now - sGhostLog > 500) {
                        sGhostLog = now;
                        LogLine("soft_land_quiet defer ghost/oob pos=(%.0f,%.0f) bb=%d",
                                st.x, st.y, haveBb ? 1 : 0);
                    }
                } else if (!gLieSafeLand.load(std::memory_order_acquire)) {
                    // 近台 / soft 已见 CurFh：卸 CombatImpact，让引擎自然挂台。
                    // 旧逻辑无条件 RequestSafeLand → Station 抖飞再 drop（本机 19:06 d=16）。
                    float sx = 0.f, sy = 0.f;
                    uint32_t snapFh = 0;
                    float snapD = 1.e9f;
                    const uint32_t curFh = ports::foothold::PeekCurFhId();
                    const bool snapOk =
                        ports::foothold_path::SnapStandAt(st.x, st.y, &sx, &sy, &snapFh,
                                                         /*preferFlat=*/false) &&
                        snapFh != 0;
                    if (snapOk) {
                        const float dx = st.x - sx;
                        const float dy = st.y - sy;
                        snapD = std::sqrt(dx * dx + dy * dy);
                    }
                    const bool natural =
                        curFh != 0 || (snapOk && snapD <= kSoftQuietNaturalLandPx);
                    if (natural) {
                        ports::fly_fh_ban::SetSourceArmed(
                            ports::fly_fh_ban::BanSource::CombatImpact, false);
                        static DWORD sNatLog = 0;
                        if (!sNatLog || now - sNatLog > 800) {
                            sNatLog = now;
                            LogLine("soft_land_quiet natural curFh=%u snapFh=%u d=%.0f "
                                    "ap=(%.0f,%.0f) — no SafeLand",
                                    (unsigned)curFh, (unsigned)snapFh, snapD, st.x, st.y);
                        }
                    } else {
                        RequestSafeLand("soft_land_quiet_air");
                    }
                }
            }
            TickLieSafeLand(now);
            TickHeliRotor(now);
    SyncImpactFhBan();
        }
        static DWORD sQuietLog = 0;
        if (!sQuietLog || now - sQuietLog > 2000) {
            sQuietLog = now;
            LogLine("combat quiet why=%s hold=%d landQuiet=%d nm=%d safeLand=%d (no fire)",
                    why, softHold ? 1 : 0, landQuiet ? 1 : 0, kick_sniff::LastSessionState(),
                    gLieSafeLand.load(std::memory_order_acquire) ? 1 : 0);
        }
        gHiraishinSawSoftQuiet = true;
        return;
    }
    // 软重连/断线静默结束：丢掉换图清掉的 NM 缓存，下一刀才能 SendOut。
    if (gHiraishinSawSoftQuiet) {
        gHiraishinSawSoftQuiet = false;
        ports::attack_rpc::InvalidateAfterMapChange();
        ArmHiraishinLootHold("soft_resume");
        if (ports::mob_gather::HangupCombatHold())
            LogLine("soft_hold released — wait auto-sell before combat");
        else
            LogLine("soft_hold released — combat resume (play-ready)");
    }
    // soft quiet 已过、post_air_gate 仍在：HeliBaseArmed=false，清宽免 BAN 拖尾。
    if (soft_login_probe::IsPostSoftAirCombatBlocked()) {
        gHeliAirborneUntilMs = 0;
        gHeliHoldValid = false;
    }
    // map arm / 后面提前 return 也要走钟：那段 Idle 已经在吸物。
    PumpHiraishinLootHoldClock(now);
    SyncImpactFhBan();
    // 测谎落台 setpoint 必须在旋翼 Tick 之前写好（同 PublishHeliSetpoint 约束）。
    TickLieSafeLand(now);
    // 旋翼必须在这里转：下面每一个提前 return（skill_prepare / arm_grace / 池未热身 /
    // bad_pos / wait_pet）都会跳过 FSM，而 fh-ban 仍挂着——停一拍就是掉一段。
    TickHeliRotor(now);
    if (ports::mob_gather::IsSeekingCluster()) {
        if (gState != State::Idle) GoIdle(now, "gather_seek");
        ports::attack::ForceRelease();
        static DWORD sGatherSeekLog = 0;
        if (!sGatherSeekLog || now - sGatherSeekLog > 800) {
            sGatherSeekLog = now;
            LogLine("gather_seek yield (no fire while flying to pack/home)");
        }
        return;
    }
    if (ports::mob_gather::HangupFiresDue()) {
        if (gState != State::Idle) GoIdle(now, "hangup_fires_due");
        ports::attack::ForceRelease();
        static DWORD sHangupFiresDueLog = 0;
        if (!sHangupFiresDueLog || now - sHangupFiresDueLog > 800) {
            sHangupFiresDueLog = now;
            LogLine("hangup fires due (no fire until CloseSession clears flags)");
        }
        return;
    }
    if (ports::mob_gather::HangupCombatHold()) {
        if (gState != State::Idle) GoIdle(now, "hangup_sell_first");
        ports::attack::ForceRelease();
        static DWORD sHangupSellLog = 0;
        if (!sHangupSellLog || now - sHangupSellLog > 800) {
            sHangupSellLog = now;
            LogLine("hangup sell-first hold (no fire until auto-supply decides)");
        }
        return;
    }
    if (gExternalPause.load(std::memory_order_acquire)) {
        // 对照枫星：buffs/timed_keys 是高频短暂停 → 轻暂停只停出刀，保留 lock/FSM。
        // 旧实现 GoIdle("pause") 会 ClearLock → 补 BUFF 后丢锁重寻/乱跳，客户感知「打怪错乱」。
        // 硬闸（channel_hop / encounter / auto_lie / supply）仍走完整 Idle。
        // 安全落台中 GoIdle 可走：SyncImpactFhBan 在 gLieSafeLand 下仍保持 CombatImpact+旋翼。
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
        SyncImpactFhBan();  // 硬暂停：无落台则卸 CombatImpact；落台中保持空中可控
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
    // 走出轻暂停 / prepare：解冻 KillTimeout（SoftReset 里 Note 的持火）。
    ReleaseKillTimeoutHold(now);
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
        gSawOnFhThisMap = false;
        gHeliAirborneUntilMs = 0;
        gHeliHoldValid = false;
        SyncImpactFhBan();
        if (gLastMapId >= 0) {
            gLastMapId = -1;  // 下一帧进图重新武装
            gMapArmUntilMs = 0;
        }
        return;
    }

    const int mapId = ports::world::GetMapId();
    if (mapId > 0 && mapId != gLastMapId) {
        const bool sameMapResume = (gStickyMapId == mapId);
        gLastMapId = mapId;
        gStickyMapId = mapId;
        gHeliRtbLatched = false;
        if (sameMapResume) {
            // 同图软重连：PlayReady 闪断会把 gLastMapId 清成 -1，旧逻辑当成换图再套 1.5s arm。
            OnCombatSameMapResume("same_map_resume");
        } else {
            // 与 encounter::ResetForMapChange 同口径：空中进图要落台，勿只开 arm。
            OnCombatMapChange("map_change");
        }
    }
    if (!CombatSpawnAllowsBan()) {
        static DWORD sWaitFh = 0;
        if (!sWaitFh || now - sWaitFh > 800) {
            sWaitFh = now;
            LogLine("combat wait onFh (no BAN/takeoff until foothold attach)");
        }
        if (gState != State::Idle) GoIdle(now, "wait_onFh");
        return;
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
    if (InCombatEnableHold(now)) {
        static DWORD sEnHold = 0;
        if (!sEnHold || now - sEnHold > 800) {
            sEnHold = now;
            LogLine("enable hold remain=%ums (heli on; first fire delayed)",
                    gEnableHoldUntilMs - now);
        }
        // build 132：hold 只压首刀，不挡起飞。Acquire/MoveTo 照跑。
    }

    // 自动召唤开着且场上无宠：走路/旋翼先让路（警戒态会拒 ActivatePet）。
    // 瞬移找怪跳过：贴怪不靠走路，等宠会把软重连后第一发拖数秒。
    if (TeleportSkipsPetSummonHold()) {
        if (x::features::pet_feed::ShouldHoldCombatForSummon(false)) {
            static DWORD sPetSkip = 0;
            if (!sPetSkip || now - sPetSkip > 5000) {
                sPetSkip = now;
                LogLine("wait pet skip teleport (summon background)");
            }
        }
    } else if (x::features::pet_feed::ShouldHoldCombatForSummon()) {
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
    // Acquire / 无锁：要够新；站桩输出扫掠每拍都要活怪坐标；有锁热路径仍可读略旧缓存。
    const bool needFreshPick =
        gHiraishinEnabled.load(std::memory_order_acquire) || gState == State::Acquire ||
        gState == State::Idle || gLock.id == 0;
    if (needFreshPick) {
        if (!EnsureFreshMobSnap(snap, kMobCacheFreshMs)) {
            // 池未热身时只催 mob_scan；禁止在战斗线程完整 Collect。
            return;
        }
    } else if (!ports::mob::GetCached(snap) || !snap.ok) {
        if (!EnsureFreshMobSnap(snap, kMobCacheFreshMs)) return;
    }
    TickMobSnapScope tickSnap(&snap);

    const float standOff = ClampStandOff();
    // 优先级：站桩输出 > Impact 贴怪 > 拟人走路 > fill+Doing 瞬移找怪。
    const bool hiraishinOn = gHiraishinEnabled.load(std::memory_order_acquire);
    const bool impactOn =
        !hiraishinOn && gImpactApproachEnabled.load(std::memory_order_acquire);
    const bool humanOn =
        !hiraishinOn && !impactOn && gHumanWalkEnabled.load(std::memory_order_acquire);
    const bool tpOn =
        !hiraishinOn && !impactOn && !humanOn && gTeleportEnabled.load(std::memory_order_acquire);
    const bool canApproach = impactOn || tpOn || humanOn;

    if (gState == State::Idle) EnterState(State::Acquire, now, "enabled");

    // 防贴脸退避：每 tick 只解一次，pass 内只读。放在 pass 循环之前是必需的——
    // 循环里五个 pass 都会读它（站位点 + 四处距离判据），中途变值就是自己跟自己对推。
    ComputeDodge(now, player.x, player.y, snap);

    if (hiraishinOn && heli::CurrentOwner() == heli::Owner::Combat) {
        heli::Disarm(heli::Owner::Combat);
        gHeliHoldValid = false;
    }

    // Aim/Recover→Firing 同 tick 连跑；多给几拍让 dead→acquire→MoveTo 同轮完成。
    for (int pass = 0; pass < 5; ++pass) {
        SyncHitPinWish();
        // 站位点每拍刷新：怪会动，出刀期间旋翼也要跟着它悬停（这才是「打完不掉」的关键）。
        if (impactOn) PublishHeliSetpoint(now, player.x, player.y, gLock.id != 0);
    switch (gState) {
        case State::Idle:
            break;

        case State::Acquire: {
            // Impact/瞬移/站桩输出：可跨层；拟人/站桩：仅同层。
            const bool allowCross = impactOn || tpOn || hiraishinOn;
            const bool hitRotateOn = HitRotateFarmActive();
            if (hitRotateOn) {
                const int live = CountLiveFarmMobs(snap);
                if (live < kHitRotateMinLive) {
                    if (gLock.id) ClearLockRetarget(/*forceLite=*/false);
                    ClearHitRotateState();
                    static DWORD sHold = 0;
                    if (!sHold || now - sHold > 2000) {
                        sHold = now;
                        LogLine("hit_rotate hold live=%d need>=%d (boss-safe)", live,
                                kHitRotateMinLive);
                    }
                    break;
                }
            }
            if (!RefreshLock(snap)) {
                // 死怪/早切刚清锁：再刷一帧再选，避免同 tick 抱旧缓存空转。
                (void)EnsureFreshMobSnap(snap, kMobCacheFreshMs);
                static DWORD sMiss = 0;
                bool got = false;
                // 无锁时仍扫确认命中（打偏记在真实 oid；刚死锁后观察窗还开着）。
                if (hitRotateOn) (void)NoteHitRotateBumps(now, snap);
                if (hitRotateOn && gHitRotatePending) {
                    got = PickHitRotateTarget(snap, gHitRotateFromX, gHitRotateFromY,
                                              gHitRotateFromId, player.x, player.y, now,
                                              allowCross);
                    if (got) {
                        ClearHitRotatePending();
                    } else {
                        static DWORD sFarMiss = 0;
                        if (!sFarMiss || now - sFarMiss > 1000) {
                            sFarMiss = now;
                            LogLine("hit_rotate clear miss fromId=%d live=%d", gHitRotateFromId,
                                    CountLiveFarmMobs(snap));
                        }
                        break;
                    }
                } else {
                    got = PickNearestTarget(snap, player.x, player.y, now, allowCross,
                                            /*looseLand=*/false);
                    if (!got) {
                        // 先松落点再选；禁止立刻 ClearSoftBan——会把刚 dead 的鬼锁清掉同 tick 空转。
                        got = PickNearestTarget(snap, player.x, player.y, now, allowCross,
                                               /*looseLand=*/true);
                    }
                    if (!got && gSoftBanN > 0) {
                        // 只清短禁；保留 whiff + 不可达 + 打中换怪禁，避免 BIN 清光后立刻重锁空转。
                        const int before = gSoftBanN;
                        DropSoftBanNonSticky();
                        if (before != gSoftBanN) {
                            LogLine("softBan keep_sticky dropped=%d keep=%d", before - gSoftBanN,
                                    gSoftBanN);
                        }
                        got = PickNearestTarget(snap, player.x, player.y, now, allowCross,
                                               /*looseLand=*/true);
                    }
                }
                if (!got) {
                    if (!sMiss || now - sMiss > 1000) {
                        sMiss = now;
                        ExplainAcquireMiss(snap, player.x, player.y, now, allowCross);
                    }
                    break;
                }
            }
            // 纯拟人：不追跨层；Impact/瞬移由 MoveTo 控节奏。
            if (!allowCross && !SameLayer(player.x, player.y, gLock.x, gLock.y)) {
                SoftBanFor(gLock.id, now, kCrossLayerForbidSoftBanMs);
                LogLine("acquire forbid cross_layer id=%d ban=%ums (%s)", gLock.id,
                        (unsigned)kCrossLayerForbidSoftBanMs,
                        hiraishinOn ? "hiraishin" : (humanOn ? "human" : "tp_off"));
                ClearLockRetarget(/*forceLite=*/false);
                break;
            }
            // 拟人：已锁用 chaseY；断崖 5s，Y 隙短晾（勿用选怪 45 误杀）。
            if (humanOn) {
                const auto hv = ClassifyHumanWalk(player.x, player.y, gLock.x, gLock.y,
                                                 kHumanWalkChaseY);
                if (hv != HumanWalkVerdict::Ok) {
                    const DWORD banMs = (hv == HumanWalkVerdict::Cliff)
                                           ? kHumanUnreachableSoftBanMs
                                           : kHumanYGapSoftBanMs;
                    SoftBanFor(gLock.id, now, banMs, kBanUnreachable);
                    LogLine("acquire forbid human_walk id=%d d=(%.0f,%.0f) why=%s ban=%ums",
                            gLock.id, gLock.x - player.x, gLock.y - player.y,
                            hv == HumanWalkVerdict::Cliff ? "cliff" : "ygap",
                            (unsigned)banMs);
                    ClearLockRetarget(/*forceLite=*/false);
                    break;
                }
            }
            // 站桩输出：面前盒（面板横向/竖直）里有怪就砍，不等叠怪。
            if (hiraishinOn) {
                if (!HiraishinFrontOk(player.x, player.y, gLock.x, gLock.y)) {
                    LogLine("acquire skip front id=%d d=(%.0f,%.0f)", gLock.id,
                            gLock.x - player.x, gLock.y - player.y);
                    ClearLockRetarget(/*forceLite=*/false);
                    break;
                }
                EnterState(State::Aim, now, "hiraishin");
                break;
            }
            // 空中贴怪：build 132 先起飞再砍。已在空中且进出刀带才 Aim；地上一律 MoveTo。
            if (impactOn) {
                LatchCombatHeli("impact_approach");
                if (HeliStrikeOk(player.x, player.y, gLock.x, gLock.y,
                                 /*firstLock=*/gLock.lockFires == 0)) {
                    ports::teleport::FlightState st{};
                    const bool airborne = ports::teleport::QueryFlightState(st) && st.ok &&
                                          !st.onFh;
                    if (airborne) {
                        EnterState(State::Aim, now, "heli_strike");
                        break;
                    }
                }
                if (!TryEnterMoveTo(now, "impact_approach")) break;
                continue;
            }
            if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                EnterState(State::Aim, now, "in_band");
                break;
            }
            if (humanOn) {
                if (!TryEnterMoveTo(now, "human_approach")) break;
                continue;
            }
            if (tpOn) {
                // 贴怪开：出命中带即贴；CD 未好不进 MoveTo。
                float hop = 0, tx = 0, ty = 0;
                uint32_t fh = 0;
                if (!EstimateLandPrefer(player.x, player.y, gLock.x, gLock.y, standOff, &hop, &tx,
                                        &ty, &fh)) {
                    SoftBanFor(gLock.id, now, kNoLandSoftBanMs);
                    LogLine("acquire no_land id=%d — softBan", gLock.id);
                    ClearLockRetarget(/*forceLite=*/false);
                    break;
                }
                if (!TryEnterMoveTo(now, "need_approach")) break;
                continue;  // 同 tick 进 MoveTo 出瞬移（禁再等一轮 worker）
            }
            // 贴怪关且非拟人：本阶段优先验证出刀——同层有怪就打，不因 dx 挡 Fire
            EnterState(State::Aim, now, "stand_fire");
            break;
        }

        case State::MoveTo: {
            if (impactOn) {
                if (!RefreshLock(snap)) {
                    EnterState(State::Acquire, now, gLastLockLostWhy);
                    continue;
                }
                // 直升机 B 层：MoveTo 只负责「等进带」。站位点在 pass 顶部统一发布，
                // 冲量由 A 层按 ~11Hz 自己发 —— 这里不再点射，也不吃瞬移冷却。
                // ⚠️ 到位判定必须与 Firing 的出刀硬门**同口径**。这三条带的纵向容差分别是
                // SameLayer 45 / 同台 100 / 悬停 80，全都宽于出刀带的 35。dy 落在中间时
                // MoveTo 说「到了」→ Firing，Firing 说「够不到」→ MoveTo，两头对推；更糟的是
                // `gStateEnterMs` 被每次迁移重置，进近看门狗永远触发不了，
                // 连换靶自救都做不到，角色就在怪下方原地空转。
                // BIN 1394b0：70s 内翻 4495 次（≈64/s），头顶台上的怪一直打不到；同一路径
                // 在收紧出刀带之前只有约 1.9 次/秒。
                // 紧出刀带才进 Firing（与真 TryFire 同口径）。宽带进火再 hold strike =
                // 人已停在怪旁发呆（BIN 01:11）。未达紧带留 MoveTo 继续飞。
                //
                // 飞途预转向：FaceToward/ApplyFaceNow 原先只在 Aim/Firing。
                // BIN：Acquire→MoveTo 全程无 face 行，MoveTo→Firing 后才 SetInput —— 体感
                // 「飞的时候背对怪，落地才转」。ApplyFaceNow = 同帧 ±1→0，不锁走路。
                {
                    const float faceDx = gLock.x - player.x;
                    (void)ports::attack::FaceToward(faceDx);
                    if (ports::attack::FaceNeedsFlip(faceDx)) {
                        static DWORD sFaceCruise = 0;
                        if (!sFaceCruise || now - sFaceCruise > 32) {
                            sFaceCruise = now;
                            (void)ports::attack::ApplyFaceNow();
                        }
                    }
                }
                if (HeliStrikeOk(player.x, player.y, gLock.x, gLock.y,
                                 /*firstLock=*/gLock.lockFires == 0)) {
                    if (!CombatHeliAirborne()) break;
                    ClearStickySpin();
                    ClearLootPulse();
                    EnterState(State::Firing, now, "heli_strike");
                    continue;
                }
                if (!x::features::invuln::IsEnabled()) {
                    static DWORD sInv = 0;
                    if (!sInv || now - sInv > 1500) {
                        sInv = now;
                        LogLine("MoveTo heli defer id=%d invuln_off (rotor grounded)", gLock.id);
                    }
                    RenewLootPulseHold(now);
                    break;
                }
                // 进近看门狗：只在「不再靠近」时换靶。长途本身不是卡住，别给行程掐秒表
                //（见 kHeliApproachStallMs 上的事故记录）。
                const DWORD age = gStateEnterMs ? (now - gStateEnterMs) : 0u;
                const float ddx = gLock.x - player.x;
                const float ddy = gLock.y - player.y;
                const float dist = std::sqrt(ddx * ddx + ddy * ddy);
                if (gHeliApproachEpoch != gStateEnterMs || gHeliProgressMs == 0) {
                    gHeliApproachEpoch = gStateEnterMs;
                    gHeliBestDist = dist;
                    gHeliProgressMs = now;
                } else if (dist < gHeliBestDist - kHeliApproachProgressPx) {
                    gHeliBestDist = dist;
                    gHeliProgressMs = now;
                }
                const DWORD stalled = now - gHeliProgressMs;
                if (stalled > kHeliApproachStallMs || age > kHeliApproachHardCapMs) {
                    SoftBanFor(gLock.id, now, kHeliApproachSoftBanMs, kBanUnreachable);
                    LogLine(
                        "MoveTo heli timeout id=%d age=%ums stall=%ums d=(%.0f,%.0f) best=%.0f "
                        "— softBan %ums",
                        gLock.id, (unsigned)age, (unsigned)stalled, ddx, ddy, gHeliBestDist,
                        (unsigned)kHeliApproachSoftBanMs);
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "heli_timeout");
                    continue;
                }
                RenewLootPulseHold(now);
                static DWORD sOk = 0;
                if (!sOk || now - sOk > 500) {
                    sOk = now;
                    LogLine("MoveTo heli id=%d d=(%.0f,%.0f) age=%ums (rotor inbound)", gLock.id,
                            gLock.x - player.x, gLock.y - player.y, (unsigned)age);
                }
                break;
            }
            if (humanOn) {
                if (!RefreshLock(snap)) {
                    EnterState(State::Acquire, now, gLastLockLostWhy);
                    continue;
                }
                if (!SameLayer(player.x, player.y, gLock.x, gLock.y)) {
                    SoftBanFor(gLock.id, now, kCrossLayerForbidSoftBanMs);
                    LogLine("MoveTo human forbid cross_layer id=%d ban=%ums", gLock.id,
                            (unsigned)kCrossLayerForbidSoftBanMs);
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "human_cross");
                    continue;
                }
                {
                    const auto hv = ClassifyHumanWalk(player.x, player.y, gLock.x, gLock.y,
                                                     kHumanWalkChaseY);
                    if (hv != HumanWalkVerdict::Ok) {
                        // BIN 01:07：|dy|~46~48 被当成断崖 5s → 乒乓洗黑；Y 隙短晾，Cliff 才 5s。
                        const DWORD banMs = (hv == HumanWalkVerdict::Cliff)
                                               ? kHumanUnreachableSoftBanMs
                                               : kHumanYGapSoftBanMs;
                        SoftBanFor(gLock.id, now, banMs, kBanUnreachable);
                        LogLine("MoveTo human forbid walk id=%d d=(%.0f,%.0f) why=%s ban=%ums",
                                gLock.id, gLock.x - player.x, gLock.y - player.y,
                                hv == HumanWalkVerdict::Cliff ? "cliff" : "ygap",
                                (unsigned)banMs);
                        ClearLockRetarget(/*forceLite=*/false);
                        EnterState(State::Acquire, now, "human_walk");
                        continue;
                    }
                }
                // 拟人进带：只用真命中带。禁 InMeleeHoldBand(dx<100)——会在 ~99px 停走空砍
                // （BIN 21:14 standOff=12 时 hit≈19，却在 dx=99 human_in_band）。
                if (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                    EnterState(State::Aim, now, "human_in_band");
                    continue;
                }
                // 途中已进命中带的同层怪：停走换锁出刀，勿路过浪费。
                if (TryHumanPassbyRetarget(snap, player.x, player.y, standOff, now)) {
                    // EnterState(Aim) 离 MoveTo 会 StopWalk，此处勿再同步抢泵一次。
                    EnterState(State::Aim, now, "human_passby");
                    continue;
                }
                const float dx = gLock.x - player.x;
                if (!std::isfinite(dx)) {
                    SoftBanFor(gLock.id, now, kHumanUnreachableSoftBanMs, kBanUnreachable);
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "human_bad_dx");
                    continue;
                }
                const float absDx = std::fabs(dx);
                // 失焦：人走不动属预期，推进锚点冻住 walkAge/progAge（BIN 01:11 no_move 洗 ban）。
                if (gHumanWalkFocusTickMs && now > gHumanWalkFocusTickMs &&
                    !GameWindowLikelyFocused()) {
                    const DWORD dt = now - gHumanWalkFocusTickMs;
                    if (gStateEnterMs) gStateEnterMs += dt;
                    if (gHumanWalkArmedMs) gHumanWalkArmedMs += dt;
                }
                gHumanWalkFocusTickMs = now;
                const DWORD walkAge = gStateEnterMs ? (now - gStateEnterMs) : 0u;
                const float inBandDx = standOff * kHitBandMaxFrac + 8.f;
                // 采样开步锚点（未武装前也要，用来算 travel 以武装进度钟）。
                if (gHumanWalkStartAbsDx < 0.f) {
                    gHumanWalkStartAbsDx = absDx;
                    gHumanWalkStartPx = player.x;
                    gHumanWalkStartDir = (dx < 0.f) ? -1 : 1;
                }
                float travel =
                    (player.x - gHumanWalkStartPx) * static_cast<float>(gHumanWalkStartDir);
                // 进度钟：首次真正开走再起算 stall（BIN：失焦 travel=0 勿空等 2.5s）。
                if (!gHumanWalkArmedMs) {
                    if (travel >= kHumanWalkArmTravelPx) {
                        gHumanWalkArmedMs = now;
                        gHumanWalkStartAbsDx = absDx;
                        gHumanWalkStartPx = player.x;
                        gHumanWalkStartDir = (dx < 0.f) ? -1 : 1;
                        travel = 0.f;
                    } else if (walkAge >= kHumanWalkArmGiveUpMs) {
                        SoftBanFor(gLock.id, now, kHumanNoMoveSoftBanMs, kBanUnreachable);
                        LogLine("MoveTo human no_move id=%d age=%ums dx=%.0f travel=%.0f — "
                                "softBan %ums",
                                gLock.id, (unsigned)walkAge, dx, travel,
                                (unsigned)kHumanNoMoveSoftBanMs);
                        ClearLockRetarget(/*forceLite=*/false);
                        EnterState(State::Acquire, now, "human_no_move");
                        continue;
                    }
                }
                const DWORD progAge =
                    gHumanWalkArmedMs ? (now - gHumanWalkArmedMs) : 0u;
                const float closing = gHumanWalkStartAbsDx - absDx;
                // 负进展 stall：武装后自己几乎没挪且 |dx| 没缩短 → 卡边。
                if (gHumanWalkArmedMs && progAge >= kHumanWalkStallMs && absDx > inBandDx &&
                    closing < kHumanWalkMinProgressPx && travel < kHumanWalkMinProgressPx) {
                    SoftBanFor(gLock.id, now, kHumanUnreachableSoftBanMs, kBanUnreachable);
                    LogLine("MoveTo human stall id=%d age=%ums prog=%ums dx=%.0f startDx=%.0f "
                            "travel=%.0f closing=%.0f — softBan %ums",
                            gLock.id, (unsigned)walkAge, (unsigned)progAge, dx,
                            gHumanWalkStartAbsDx, travel, closing,
                            (unsigned)kHumanUnreachableSoftBanMs);
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "human_stall");
                    continue;
                }
                if (gStateEnterMs && walkAge >= kHumanWalkTimeoutMs) {
                    SoftBanFor(gLock.id, now, kHumanUnreachableSoftBanMs, kBanUnreachable);
                    LogLine("MoveTo human timeout id=%d age=%ums dx=%.0f — softBan %ums", gLock.id,
                            (unsigned)walkAge, dx, (unsigned)kHumanUnreachableSoftBanMs);
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "human_timeout");
                    continue;
                }
                if (absDx < 1.f) {
                    // 怪心重叠：等怪挪开或超时换靶，勿左右抽风。
                    RenewLootPulseHold(now);
                    break;
                }
                const int dir = (dx < 0.f) ? -1 : 1;
                if (!ports::attack::HoldWalk(dir)) {
                    static DWORD sHoldFail = 0;
                    if (!sHoldFail || now - sHoldFail > 1500) {
                        sHoldFail = now;
                        LogLine("MoveTo human HoldWalk fail id=%d dir=%d", gLock.id, dir);
                    }
                    break;
                }
                static DWORD sWalkLog = 0;
                if (!sWalkLog || now - sWalkLog > 800) {
                    sWalkLog = now;
                    LogLine("MoveTo human walk id=%d dx=%.0f dir=%d age=%ums", gLock.id, dx, dir,
                            gStateEnterMs ? (unsigned)(now - gStateEnterMs) : 0u);
                }
                // 拟人走路：不出刀即可吸（charVac 直调 Send）。续脉冲边沿供 interval 豁免。
                RenewLootPulseHold(now);
                break;
            }
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
                ClearLockRetarget(/*forceLite=*/false);
                EnterState(State::Acquire, now, "no_land");
                continue;
            }
            // 326d34：无论是否 direct_tp，单次 fill 都必须受 maxHop 切段（BIN hop=3517→205）。
            bool didChunk = false;
            {
                const float maxHop = MaxApproachHopPx();
                if (hop > maxHop) {
                    const float fullHop = hop;
                    float step = 0.f, sx = 0.f, sy = 0.f;
                    uint32_t sfh = 0;
                    const bool chunkOk = TryChunkApproachLand(player.x, player.y, tx, ty, maxHop,
                                                              &step, &sx, &sy, &sfh);
                    if (!chunkOk) {
                        SoftBanFor(gLock.id, now, kHopChunkFailSoftBanMs, kBanUnreachable);
                        LogLine("MoveTo chunk fail id=%d wantHop=%.0f maxHop=%.0f softBan=%ums "
                                "(snap overshoot/no progress)",
                                gLock.id, fullHop, maxHop, (unsigned)kHopChunkFailSoftBanMs);
                        ClearLockRetarget(/*forceLite=*/false);
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
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "hop_hard_cap");
                    continue;
                }
            }
            if (landSide != 0) gLandSide = landSide;

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
                ClearLockRetarget(/*forceLite=*/false);
                EnterState(State::Acquire, now, "land_unsafe");
                continue;
            }
            // snapStand=true：NativeJob 用 SnapOnFh 钉死 EstimateLand 的台，不再 SnapStandAt 换邻段。
            // hop 间距不垫 Native 自冷；Settling/PosSane 未完成不会进下一发 MoveTo。
            if (!ports::teleport::TeleportNativeSkillCall(tx, ty, fh, /*snapStand=*/true, &tx, &ty,
                                                          &fh)) {
                static DWORD sFail = 0;
                if (!sFail || now - sFail > 1500) {
                    sFail = now;
                    LogLine("MoveTo teleport fail id=%d want=(%.0f,%.0f) hop=%.0f hug=%d",
                            gLock.id, tx, ty, hop, hug ? 1 : 0);
                }
                RenewLootPulseHold(now);
                break;
            }
            ClearStickySpin();
            const DWORD minSettle = kPostDoingMinSettleMs;
            const DWORD posGate = kPostDoingPosSaneMaxMs;
            LogLine(
                "MoveTo fill+Doing id=%d to=(%.0f,%.0f) from=(%.0f,%.0f) hop=%.0f fh=%u "
                "side=%d hug=%d settle=%ums posGate=%ums minSettle=%ums cross=%d chunk=%d",
                gLock.id, tx, ty, player.x, player.y, hop, fh, gLandSide, hug ? 1 : 0,
                SettleMsForHop(hop, hug), (unsigned)posGate, (unsigned)minSettle, same ? 0 : 1,
                didChunk ? 1 : 0);
            gSettleX = tx;
            gSettleY = ty;
            gSettleFh = fh;
            gStandstillSince = now;
            gStandstillAnchorX = tx;
            gStandstillAnchorY = ty;
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
            (void)crossHop;
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
            const float landEps = kPostDoingLandEpsPx;

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
            const bool landed =
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
                        j->ok = ports::teleport::StabilizeFootholdMainThread(j->x, j->y, j->fh);
                    };
                    const bool pumped =
                        x::runtime::main_thread::Ensure() &&
                        x::runtime::main_thread::InvokeAndWait(
                            stabFn, &job, 80, x::runtime::main_thread::JobPrio::High);
                    LogSettleDiag("stabilize", now, sinceEnter, dLand);
                    LogLine("Settling stabilize ok=%d pumped=%d detach=1 wantFh=%u "
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
            LogLine("Settling done land=(%.0f,%.0f) fh=%u d=%.0f early=%d since=%ums curFh=%u",
                    gSettleX, gSettleY, gSettleFh, dLand, timedOut ? 0 : 1, sinceEnter,
                    vsOk ? (unsigned)vs.curFh : 0u);
            gSettleUntil = 0;
            gSettleNeedPosSane = false;
            gSettleEnteredAt = 0;
            // 收态全部交给引擎（Ap/Apl/速度/挂台都由原生落点体与 CollisionDetect 完成）。
            // 禁止再 SyncRel/Impact，也禁止在此补写位置。
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, "lost_after_settle");
                break;
            }
            // hop 后引擎朝向跟跳跃走，gLastFaceSign 仍是上一只怪。
            // 挂台后再对齐：已对只同步缓存；不一致才 SetInput。仍 break，下一拍才 Aim/Fire。
            {
                const float px = posOk ? landCtx.x : player.x;
                float faceDx = gLock.x - px;
                // |dx|<死区时 AlignFace 不转。贴脸 hop BIN 第一刀大量 fw=1；用落点侧合成朝向。
                // gLandSide：-1=站怪左（应对右）/ +1=站怪右（应对左）。
                if (std::fabs(faceDx) < 8.f && gLandSide != 0)
                    faceDx = static_cast<float>(-gLandSide) * 16.f;
                ports::teleport::FlightState st{};
                const int ma =
                    (ports::teleport::QueryFlightState(st) && st.ok) ? st.ma : -1;
                const int want = (std::isfinite(faceDx) && std::fabs(faceDx) >= 8.f)
                                     ? (faceDx < 0.f ? -1 : 1)
                                     : 0;
                const int eng = (ma >= 0) ? ((ma & 1) ? -1 : 1) : 0;
                const bool pulsed = ports::attack::AlignFaceToEngine(faceDx, ma);
                LogLine("settle face dx=%.0f ma=%d want=%d eng=%d pulsed=%d", faceDx, ma, want,
                        eng, pulsed ? 1 : 0);
            }
            EnterState(State::Aim, now, "settle_ok");
            break;  // 下一 worker tick 再 Aim/Fire，给主线程一帧消化 Doing
        }

        case State::Aim: {
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                break;
            }
            if (hiraishinOn) {
                const float faceDx = gLock.x - player.x;
                (void)ports::attack::FaceToward(faceDx);
                if (HiraishinLootHoldBlocksFire(now)) break;
                if (!HiraishinFrontOk(player.x, player.y, gLock.x, gLock.y)) {
                    EnterState(State::Acquire, now, "hiraishin_not_front");
                    continue;
                }
                EnterState(State::Firing, now, "hiraishin_ready");
                continue;
            }
            // 无脑A：近距续砍；只有真远/跨层才重贴。禁止 aim_sticky 翻侧空转。
            // 残血纠偏：离开命中带即强制重贴（勿被 kReapproachMinDx 吞掉）。
            {
                bool moved = false;
                if (TryConsumeWhiffApproachCorrect(now, player.x, player.y, standOff, canApproach,
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
            // Impact：空中紧带才进 Firing；还在台上先 MoveTo 起飞。
            if (impactOn && HeliStrikeOk(player.x, player.y, gLock.x, gLock.y,
                                         /*firstLock=*/gLock.lockFires == 0)) {
                if (!CombatHeliAirborne()) {
                    if (TryEnterMoveTo(now, "impact_approach")) break;
                    break;
                }
                const float faceDx = gLock.x - player.x;
                (void)ports::attack::FaceToward(faceDx);
                EnterState(State::Firing, now, "heli_strike");
                continue;
            }
            // 已进宽带但未紧带：回 MoveTo 继续收 Y，勿在 Aim 空转。
            if (impactOn && InHeliFireBand(player.x, player.y, gLock.x, gLock.y)) {
                if (TryEnterMoveTo(now, "heli_strike_close")) break;
                break;
            }
            if (!SameLayer(player.x, player.y, gLock.x, gLock.y)) {
                // 近距纵漂：当 melee 续砍，勿 cross 狂贴。
                if (NearMeleeFloor(player.x, player.y, gLock.x, gLock.y) ||
                    (impactOn && InHeliHoverBand(player.x, player.y, gLock.x, gLock.y))) {
                    // fall through to fire gate below
                } else if (impactOn || tpOn) {
                    // Impact 直升机 / 瞬移：跨层走 MoveTo，勿 SoftBan 清锁。
                    if (TryEnterMoveTo(now, impactOn ? "heli_cross" : "cross_layer")) break;
                    break;
                } else {
                    SoftBanFor(gLock.id, now, kCrossLayerForbidSoftBanMs);
                    LogLine("Aim forbid cross_layer id=%d ban=%ums (%s)", gLock.id,
                            (unsigned)kCrossLayerForbidSoftBanMs, humanOn ? "human" : "tp_off");
                    ClearLockRetarget(/*forceLite=*/false);
                    EnterState(State::Acquire, now, "cross_layer_forbid");
                    continue;
                }
            }
            // 非 Impact 或未进出刀带：下面继续地面旁路。
            // Impact 已在上面 InHeliFireBand 分支进 Firing。
            // InHitBand / InMeleeHoldBand 的容差（纵向 SameLayer 45 / 同台 100，横向放到
            // dx<100）是给地面站桩定的，空中够不到那么远。Impact 悬停时必须再叠一道出刀带
            // 的 dy+dx，否则这条支路会绕过上面刚收紧的 heli_hover 门禁继续空砍。
            // Impact 档禁止再走 InHitBand(ClampStandOff)：否则自定义站距下贴脸仍 ready 出刀。
            if (!impactOn &&
                (InHitBand(player.x, player.y, gLock.x, gLock.y, standOff) ||
                 (!humanOn &&
                  InMeleeHoldBand(player.x, player.y, gLock.x, gLock.y, standOff)))) {
                const float faceDx = gLock.x - player.x;
                (void)ports::attack::FaceToward(faceDx);
                EnterState(State::Firing, now, "ready");
                continue;
            }
            if (impactOn) {
                if (NeedsHeliStationKeep(player.x, player.y, gLock.x, gLock.y)) {
                    if (TryEnterMoveTo(now, "heli_reapproach")) continue;
                }
                break;
            }
            if (humanOn) {
                // 出真命中带就走；勿等 dx≥100（NeedsReapproach）才追。
                if (TryEnterMoveTo(now, "aim_human_approach")) continue;
                break;
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
            if (!hiraishinOn && InCombatEnableHold(now)) break;  // 起飞中，压首刀
            if (!RefreshLock(snap)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;  // 同 tick 选下一只并贴
            }
            if (hiraishinOn && HiraishinLootHoldBlocksFire(now)) break;
            if (hiraishinOn && !HiraishinFrontOk(player.x, player.y, gLock.x, gLock.y)) {
                EnterState(State::Acquire, now, "hiraishin_not_front");
                continue;
            }
            // 满 N 后禁止再 OnFuncKey：RefreshLock 里 TryAbandon 偶发没清锁时这里兜住。
            if (HitRotateQuotaFull() && !hiraishinOn) {
                const int live = snap.ok ? CountLiveFarmMobs(snap) : 0;
                (void)TriggerHitRotateFrom(gLock.id, gLock.x, gLock.y, gLock.hitBumpCount, live, now,
                                           "quota");
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;
            }
            {
                bool moved = false;
                if (TryConsumeWhiffApproachCorrect(now, player.x, player.y, standOff, canApproach,
                                                   &moved)) {
                    if (moved) continue;
                    break;
                }
            }
            // 左右可位移框外：旋翼拉回（build 132 交战期一直飞，无地面 skip）。
            if (impactOn && PlayerOutOfPlayBounds(player.x, player.y)) {
                EnterState(State::Aim, now, "oob_hold");
                break;
            }
            // 防贴脸退避未退到干净位：本拍不出刀，原地等旋翼把人挪开。
            // 总闸未开时 DodgeHoldsFire 恒 false，这一行等于不存在。
            // 压刀有 1500ms 上限（见 kDodgeHoldMaxMs），超时放弃退避恢复出刀，绝不站着挨打。
            // 换锁首刀不压：到位后 hold_fire 也是「怪旁边发呆」（BIN 23:01 held=67ms+）。
            if (impactOn && gLock.lockFires > 0 && DodgeHoldsFire(now)) break;
            // P0 出刀硬门：未进带绝不砍（含误入 Firing 的路径）。
            // 站桩输出原地出刀，不走滑翔打击带。
            const bool heliFireOk = impactOn && InHeliFireBand(player.x, player.y, gLock.x, gLock.y);
            const bool heliReachBlocked = impactOn && !HeliReachOk(player.x, player.y, gLock.x, gLock.y);
            if (!hiraishinOn && !heliFireOk &&
                (heliReachBlocked ||
                 !FireGateOk(player.x, player.y, gLock.x, gLock.y, standOff, now, "Firing"))) {
                if (impactOn && NeedsHeliStationKeep(player.x, player.y, gLock.x, gLock.y)) {
                    if (TryEnterMoveTo(now, "heli_fire_gate")) continue;
                } else if (canApproach && NeedsReapproach(player.x, player.y, gLock.x, gLock.y) &&
                           !(tpOn && StickyCorrectCooling(now))) {
                    if (TryEnterMoveTo(now, humanOn ? "fire_gate_human" : "fire_gate")) continue;
                }
                EnterState(State::Aim, now, "fire_gate");
                break;
            }
            // 已进宽带但未进紧出刀带：留 Firing 持火（兜底；进火路径已改紧带）。
            // 换怪首刀用更严 dy（BIN 13:00 跨层首刀空近半）。
            const bool firstOfLockGeo = (gLock.lockFires == 0);
            if (impactOn && !HeliStrikeOk(player.x, player.y, gLock.x, gLock.y, firstOfLockGeo)) {
                static DWORD sStrikeHold = 0;
                if (!sStrikeHold || now - sStrikeHold > 500) {
                    sStrikeHold = now;
                    const HeliStand hs = HeliStandOff();
                    float needDy = firstOfLockGeo ? kHeliFirstLockMaxDy : kHeliStrikeMaxDy;
                    float needDx = HeliFireMaxDx();
                    if (hs.custom) {
                        const HeliAttackAabb box =
                            HeliAttackAabbFromStand(hs, firstOfLockGeo);
                        needDx = box.halfX;
                        needDy = box.halfY;
                    } else if (firstOfLockGeo) {
                        needDx = needDx < kHeliFirstLockMaxDx ? needDx : kHeliFirstLockMaxDx;
                    }
                    LogLine(
                        "fire hold strike id=%d d=(%.0f,%.0f) relY=%.0f needDy=%.0f needDx=%.0f "
                        "first=%d aabb=%d",
                        gLock.id, gLock.x - player.x, gLock.y - player.y, player.y - gLock.y,
                        needDy, needDx, firstOfLockGeo ? 1 : 0, hs.custom ? 1 : 0);
                }
                break;
            }
            // 竖直还太快：首刀更严（BIN：|vy|≥200 空 62%）。
            {
                const float maxVy =
                    firstOfLockGeo ? kHeliFirstLockMaxVy : kHeliStrikeMaxVy;
                if (impactOn && std::fabs(gHeliLastVy) > maxVy) {
                    static DWORD sVyHold = 0;
                    if (!sVyHold || now - sVyHold > 500) {
                        sVyHold = now;
                        LogLine("fire hold vy id=%d vy=%.0f max=%.0f d=(%.0f,%.0f) first=%d",
                                gLock.id, gHeliLastVy, maxVy, gLock.x - player.x,
                                gLock.y - player.y, firstOfLockGeo ? 1 : 0);
                    }
                    break;
                }
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

            // ★ inView=0：默认不 TryFire（FindHit 拒刀 → softban）。
            // BIN 00:16：几何已 InHeliFireBand 仍 NeedsHeliStationKeep → heli_iv0_hold
            // Firing↔MoveTo 自激，体感「贴到怪身上呆住」。
            // · 已进带：禁止再进 MoveTo，原地持火等 inView（旋翼仍 Publish 收站）。
            // · 换锁首刀且已进带：直接放行（first_lock_fire forgive 兜空挥）。
            // · 打中换怪：FindHit 硬拒 inView=0，宽带 forgive 必空，不放行。
            if (!hiraishinOn && !gLock.inView) {
                const bool hitRotateOn = HitRotateFarmActive();
                const bool heliInBand =
                    impactOn && InHeliFireBand(player.x, player.y, gLock.x, gLock.y);
                if (hitRotateOn || !(heliInBand && gLock.lockFires == 0)) {
                    if (impactOn && !heliInBand &&
                        NeedsHeliStationKeep(player.x, player.y, gLock.x, gLock.y)) {
                        gInViewHoldSince = 0;
                        if (TryEnterMoveTo(now, "heli_iv0_hold")) continue;
                        break;
                    }
                    if (!gInViewHoldSince) gInViewHoldSince = now;
                    static DWORD sIvHoldLog = 0;
                    if (!sIvHoldLog || now - sIvHoldLog > 500) {
                        sIvHoldLog = now;
                        LogLine("fire hold iv=0 id=%d d=(%.0f,%.0f) since=%ums", gLock.id,
                                gLock.x - player.x, gLock.y - player.y, now - gInViewHoldSince);
                    }
                    if (now - gInViewHoldSince >= kInViewHoldFireMaxMs) {
                        SoftBanFor(gLock.id, now, kInViewHoldSoftBanMs, kBanUnreachable);
                        LogLine("switch reason=iv0_timeout id=%d hold=%ums softBan=%ums", gLock.id,
                                now - gInViewHoldSince, (unsigned)kInViewHoldSoftBanMs);
                        gLastLockLostWhy = "iv0_timeout";
                        ClearLockRetarget();
                        EnterState(State::Acquire, now, "iv0_timeout");
                        continue;
                    }
                    break;  // 留 Firing：持火等 inView，不弹 MoveTo
                }
                gInViewHoldSince = 0;  // 首刀+已进带：落入下方出刀
            } else {
                gInViewHoldSince = 0;
            }

            const float faceDx = gLock.x - player.x;
            (void)ports::attack::FaceToward(faceDx);
            const bool firstOfLock = (gLock.lockFires == 0);

            // F5 刚开首刀：强制转身一拍再砍（仅一次）。站桩输出 1s 窗不赔这一拍。
            // 热开时常已在出刀带 → Aim→火极快；去掉全局 face_settle 后朝向未落地就原地空挥。
            if (gNeedEnableFaceSettle.exchange(false, std::memory_order_acq_rel)) {
                (void)ports::attack::ApplyFaceNow();
                if (!hiraishinOn) {
                    EnterState(State::Recover, now, "enable_face_settle");
                    break;
                }
            }

            // ── 换向：一律同拍转身+出刀，不再 face_settle Recover ────────────────
            // 分拍实证能抬换向命中，但到位后 ApplyFaceNow 行走输入 + Recover =
            // 体感发呆（BIN 23:09）。首刀已同拍；用户要连砍也不愣 → 后续刀同样不恢复。
            // 换向空刀：首刀仍 first_lock_fire forgive；后续走正常 whiff。
            // （启用瞬间的那一刀改走上面 enable_face_settle。）
            if (ports::attack::FaceNeedsFlip(faceDx)) {
                (void)ports::attack::ApplyFaceNow();
            }

            // ── 引擎动作忙锁：忙时不出刀，也不算空刀 ──────────────────────────
            // 「攻击加速」（写 LocalUser+ActionBusy=-1 清忙锁）停用后，引擎的出刀周期从
            // 82ms 掉到 650~800ms（`_cadence.py` 实测中位：b105 有加速 82 / b106 无加速 801），
            // 而我们仍按 gConfigIntervalMs(123ms) 刷键——每命中一次要刷 5.8 次，4.8 次被吞。
            //
            // 真正的代价不是废键，是**假空刀丢靶**：被吞的刀照样走 ArmWhiffObserve，连续
            // kWhiffFireN 次判空就换怪，于是 b106 出现「丢靶 424 次 > 击杀 214 次」——每杀
            // 1 只，先扔掉 2 只已经贴好位的怪，而那些刀根本没挥出去。所以这里 break 得赶在
            // ArmWhiffObserve 之前：没挥出去的刀不该记账。
            //
            // 放在转身块**之后**是有意的：忙的这 800ms 里转身是免费的，等忙位一开就能立刻
            // 挥，不用再赔一拍 face_settle（见上方换向分拍的收益账）。
            //
            // 只读 ActionBusy：简单战斗**绝不** ClearActionBusy（清忙只归实验 TAB /
            // attack_accel 周期路径）。忙则原地 break，等引擎或面板清忙自己放开。
            //
            // ⚠️ 忙时**原地 break，绝不换态**。第一版弹去 Recover，结果 FSM 按 tick(16ms)
            // 在 Firing↔Recover 之间自激：310 次真实出刀伴随 9038 次弹回（≈66 转换/秒），
            // 正踩在本文件反复警告的「FSM 自激 64/s → StopWalk/InvokeAndWait 合成泵抢占
            // 主线程」那条红线上。命中节奏明明打满了引擎上限（中位 810ms），kills/min 却
            // 从 27.9 掉到 18.1——抢走的主线程时间全从移动和贴怪里出。附带还把 state 行
            // 刷到日志的 91%，把验收数据挤出轮转。
            // 留在 Firing 不损失任何跟踪能力：本 case 开头已做 RefreshLock / 越界 / 纠位，
            // 与 Recover 等价；且 Firing 无滞留看门狗（gStateEnterMs 只被 MoveTo 用）。
            {
                int busy = 0;
                if (attack_accel::QueryActionBusy(player.localUser, busy) && busy >= 0) {
                    // 多发：不在 Fire 入口统一清忙锁。
                    // NA / post-NA SendUse 由 multi Tick 自行 Clear；仅技能 DoActive 必须保留
                    // ActionBusy，否则无表 CD 的蜗牛会被清成亚秒连发。
                    if (UseMulti()) {
                        gEngineBusySince = 0;
                    } else {
                        if (!gEngineBusySince) gEngineBusySince = now;
                        ++gEngineBusyTicks;
                        static DWORD sBusyStat = 0;
                        if (!sBusyStat || now - sBusyStat > 5000) {
                            sBusyStat = now;
                            LogLine("engine_busy wait id=%d busy=%d blocked=%u held=%ums",
                                    gLock.id, busy, gEngineBusyTicks,
                                    (unsigned)(now - gEngineBusySince));
                        }
                        break;  // 等 busy 自行结束；超时也不 Clear
                    }
                } else {
                    gEngineBusySince = 0;
                }
            }

            // build 132：BAN 交战期不卸。BAN+ma=4/5 出刀会崩（edfbf1），只压刀等跳/飞。
            // FaceDebug 是上次 SetInput 的 ma，台沿钉死后不再转身 → 假 4 空转。
            // 用 VecCtrl 实读；站着就回 MoveTo 抬飞，禁止在 Firing 里 break 死等。
            // BAN+ma=4/5 出刀会崩。站桩输出站着砍，不抬飞。
            if (!hiraishinOn &&
                (ports::fly_fh_ban::ActiveMask() &
                 static_cast<unsigned>(ports::fly_fh_ban::BanSource::CombatImpact)) != 0) {
                ports::teleport::FlightState standSt{};
                if (ports::teleport::QueryFlightState(standSt) && CombatStandMa(standSt)) {
                    static DWORD sStandBan = 0;
                    if (!sStandBan || now - sStandBan > 400) {
                        sStandBan = now;
                        LogLine("fire defer stand_fhban ma=%d ap=(%.0f,%.0f) v=(%.0f,%.0f) "
                                "(keep BAN; takeoff)",
                                standSt.ma, player.x, player.y, gHeliLastVx, gHeliLastVy);
                    }
                    if (TryEnterMoveTo(now, "wait_air_ma")) continue;
                    break;
                }
            }

            const int hpBefore = gLock.lastHp;
            bool ok = false;

            if (UseMulti() && !hiraishinOn) {
                if (multi_skill::IsBurstBusy()) break;
                (void)ports::attack::ApplyFaceNow();
                char reason[64]{};
                ok = multi_skill::TryCast(reason, sizeof(reason));
                if (ok) {
                    NoteDodgeFire(now);
                    int faceMa = -1, faceWhy = -1;
                    ports::attack::FaceDebug(&faceMa, &faceWhy);
                    // 这是**排程行**：TryCast 只入队，真正派发在 multi_skill_port::Tick 里。
                    // 所以站立伪装的 sp/b0/b1 不能打在这儿（会记成上一刀的值），
                    // 它们打在 x.jsonl 的 `MultiSkill cast NormalAttack` 行上。
                    //
                    // 空刀观察也禁止在这儿 Arm：BIN 167435 TryCast 后 NA 被 skill_busy
                    // 推迟 / 本波只出蜗牛，100ms 窗仍 +whiff→假 softBan（朝向/iv 都对）。
                    // 改由 NotifyMultiNormalAttackFired（NA OnFuncKey 成功）武装。
                    LogToFile(
                        "multi fire id=%d dx=%.0f dy=%.0f v=(%.0f,%.0f) hp=%d whiff=%d arm=%d "
                        "lf=%d ma=%d fw=%d",
                        gLock.id, faceDx, player.y - gLock.y, gHeliLastVx, gHeliLastVy, hpBefore,
                        gLock.whiff, gLock.firesInArm, gLock.lockFires, faceMa, faceWhy);
                    if (!gStandstillSince) {
                        gStandstillSince = now;
                        gStandstillAnchorX = player.x;
                        gStandstillAnchorY = player.y;
                    }
                    // 自校准节流 + Recover：与下方单刀路径对齐，但跳过 ArmWhiffObserve。
                    {
                        static DWORD sReachLog = 0;
                        if (!sReachLog || now - sReachLog > 15000) {
                            sReachLog = now;
                            const reach::Snap rs = reach::Read();
                            LogLine(
                                "reach_cal n=%d plateau=%.0f%% edge=%.0f cliff=%d "
                                "station=%.0f(gate=%.0f)",
                                rs.samples, rs.plateau, rs.edge, rs.cliff ? 1 : 0, rs.stationDx,
                                reach::StationDx(kHeliStationDx));
                        }
                    }
                    if (TryAbandonOneshot(now)) {
                        EnterState(State::Acquire, now, gLastLockLostWhy);
                        continue;
                    }
                    EnterState(State::Recover, now, "fired");
                    break;
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
                // 仅换锁首刀跳过面板间隔（BIN：换锁后常卡在 ~400ms 残值）。
                // 同锁第 2 刀起恢复面板节奏；pendingUp / 泵拥堵始终挡。
                // 站桩输出不走 ignore：换锁第一刀也等间隔 + 忙锁，禁止 18ms 空刷。
                const bool ignoreInterval = firstOfLock && !hiraishinOn;
                if (!ports::attack::CanFirePrimaryEx(ignoreInterval)) break;
                const bool forgeOn = gForgeHitEnabled.load(std::memory_order_acquire);
                bool forgedOk = false;
                if (forgeOn) {
                    // 开了自组就只发自组。过远 / SendOut false / 非近战：停这一刀，不退 OnFuncKey。
                    ports::attack_rpc::FireResult fr{};
                    if (gLock.id > 0) {
                        ok = ports::attack_rpc::TryFireLockOid(gLock.id, &fr);
                    } else {
                        fr.err = "no_oid";
                    }
                    const char* ferr = fr.err && fr.err[0] ? fr.err : "?";
                    if (ok) {
                        forgedOk = true;
                        NoteDodgeFire(now);
                        ports::attack::NoteLastFire();
                        int faceMa = -1, faceWhy = -1;
                        ports::attack::FaceDebug(&faceMa, &faceWhy);
                        LogToFile(
                            "forge_hit ok id=%d mobs=%d body=%d op=%d wt=%d skill=%d fkT=%d fkV=%d "
                            "dx=%.0f dy=%.0f hp=%d lf=%d ma=%d fw=%d err=%s",
                            gLock.id, fr.mobs, fr.bodyHint, fr.opcode, fr.weaponType, fr.skillId,
                            fr.fkType, fr.fkValue, faceDx,
                            player.y - gLock.y, hpBefore, gLock.lockFires, faceMa, faceWhy,
                            ferr);
                        if (!gStandstillSince) {
                            gStandstillSince = now;
                            gStandstillAnchorX = player.x;
                            gStandstillAnchorY = player.y;
                        }
                    } else {
                        static DWORD sForgeFail = 0;
                        if (!sForgeFail || now - sForgeFail > 2000) {
                            sForgeFail = now;
                            LogLine("forge_hit fail id=%d err=%s (no OnFuncKey)", gLock.id, ferr);
                        }
                        EnterState(State::Recover, now, "forge_fail");
                        break;
                    }
                }
                if (!forgedOk) {
                ports::attack::FireBlink blink{};
                ok = ports::attack::TryFirePrimaryEx(ignoreInterval, blink);
                if (ok) {
                    NoteDodgeFire(now);
                    // dy / v 是为了判「空刀由什么决定」而补的：历史上这行只有 dx，
                    // 结果 780 档空刀翻 7 倍时手上没有能判决的量，白扫了一轮横向前瞻假设。
                    // 补上后一份数据即定案（见 kHeliFireMaxDy 注释）：空刀由站位决定、
                    // 与速度无关。dy 取「角色 − 怪」，与那处阈值同向，可直接并表。
                    // 这三个量是收闸后验收 kills/min 的唯一依据，别再删。
                    //
                    // lf / ma / fw 是第二轮补的，为查「换锁后第一刀空刀 63%」：
                    //   lf=0 即本锁第一刀（ArmWhiffObserve 在本行之后才 +1）。
                    //   实测该刀的空刀率与站位无关（好格 61.2% vs 非好格 63.2%）、
                    //   与速度无关（|v|<150 仍 63.5%）、任何前瞻 Δ 都不提高预测力，
                    //   所以只剩「引擎吞刀」与「朝向没摆对」两条，靠 ma/fw 分。
                    //   ma bit0=1 面朝左；faceDx<0 表示怪在左，两者应一致。
                    //   fw≠0 表示 ApplyFaceNow 跳过了，此时 ma 是旧值不可用。
                    int faceMa = -1, faceWhy = -1;
                    ports::attack::FaceDebug(&faceMa, &faceWhy);
                    // 站立伪装取证：sp = 本刀的种台判决（见 ground_spoof.h），spfh = 种下的台 ID。
                    // b0/b1 = 派发前后的引擎动作忙位（见 attack_input_port::FireOutcomeDebug）：
                    //   b1≥0 引擎接了这一刀，值即攻击动作 id；b1=-1 这一刀被引擎吞了。
                    // 判伪装有没有用，看的就是 sp=1 与 sp=0 两组的 b1=-1 占比差。
                    // 别再用 VecCtrl.moveAction 当 action —— 那是移动姿态，种台不会动它。
                    int spV = 0, b0 = -2, b1 = -2;
                    uint32_t spFh = 0;
                    ports::ground_spoof::FireDebug(&spV, &spFh);
                    ports::attack::FireOutcomeDebug(&b0, &b1);
                    int bapx = 0, bapy = 0, baplx = 0, baply = 0, bap2x = 0, bap2y = 0;
                    if (blink.on) ports::attack::BlinkDebug(&bapx, &bapy, &baplx, &baply, &bap2x, &bap2y);
                    LogToFile(
                        "fire id=%d dx=%.0f dy=%.0f v=(%.0f,%.0f) hp=%d whiff=%d arm=%d lf=%d "
                        "ma=%d fw=%d sp=%d spfh=%u b0=%d b1=%d blink=%d aim=(%.0f,%.0f) "
                        "ap=(%d,%d) apl=(%d,%d) ap2=(%d,%d)",
                        gLock.id, faceDx, player.y - gLock.y, gHeliLastVx, gHeliLastVy, hpBefore,
                        gLock.whiff, gLock.firesInArm, gLock.lockFires, faceMa, faceWhy, spV, spFh,
                        b0, b1, blink.on ? 1 : 0, blink.x, blink.y, bapx, bapy, baplx, baply, bap2x,
                        bap2y);
                    // 与 BIN「fire id=」同一拍：墙钟分钟加总就是那次 2030。不看 b1/命中。
                    x::features::ports::mob_gather::NoteHangupFire();
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
            }

            // 出刀只武装观察窗；真正 +whiff / 换怪在 RefreshLock→ResolveWhiffArm。
            ArmWhiffObserve(now, hpBefore, std::fabs(faceDx));

            // 自校准状态节流播报：离线用它与 `_reach.py` 的逐档表并行核对，
            // 确认「在线估的边界」与「事后统计的边界」是同一个数。
            {
                static DWORD sReachLog = 0;
                if (!sReachLog || now - sReachLog > 15000) {
                    sReachLog = now;
                    const reach::Snap rs = reach::Read();
                    LogLine("reach_cal n=%d plateau=%.0f%% edge=%.0f cliff=%d station=%.0f(gate=%.0f)",
                            rs.samples, rs.plateau, rs.edge, rs.cliff ? 1 : 0, rs.stationDx,
                            reach::StationDx(kHeliStationDx));
                }
            }
            // 瞬移「每只怪打一下」：出刀当帧切下一只（与射后不管同拍，不等 Recover）。
            if (!hiraishinOn && TryAbandonTeleportOneHit(now)) {
                EnterState(State::Acquire, now, gLastLockLostWhy);
                continue;
            }
            // 射后不管：出刀当帧即可早切（不等 Recover 下一拍读 lastHitted）。
            // 站桩输出不因 oneshot / 空刀观察换锁。
            if (!hiraishinOn && TryAbandonOneshot(now)) {
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
                if (TryConsumeWhiffApproachCorrect(now, player.x, player.y, standOff, canApproach,
                                                   &moved)) {
                    if (moved) continue;
                    break;
                }
            }
            // 站桩输出：面前有怪就点按续砍（等忙锁 / 间隔，换锁首刀也不 ignore）。
            if (hiraishinOn) {
                if (HiraishinLootHoldBlocksFire(now)) break;
                if (!HiraishinFrontOk(player.x, player.y, gLock.x, gLock.y)) {
                    EnterState(State::Acquire, now, "hiraishin_not_front");
                    continue;
                }
                if (!ports::attack::CanFirePrimaryEx(/*ignoreCombatInterval=*/false)) break;
                EnterState(State::Firing, now, "hiraishin_next");
                continue;
            }
            // 黏住无脑A：近距继续砍；跨层/真远才重贴。
            const float dx = std::fabs(gLock.x - player.x);
            const bool same = SameLayer(player.x, player.y, gLock.x, gLock.y);
            const bool firstOfLock = (gLock.lockFires == 0);
            // 直升机：悬停续砍；漂出站位才 MoveTo；怪死走 Acquire 换下一只。
            if (impactOn && !gLock.needApproachCorrect) {
                if (!CombatHeliAirborne()) {
                    if (!TryEnterMoveTo(now, "impact_approach")) break;
                    continue;
                }
                // 出刀带续砍；仅换锁首刀放行间隔（与 Firing 一致）。
                if (InHeliFireBand(player.x, player.y, gLock.x, gLock.y)) {
                    if (!ports::attack::CanFirePrimaryEx(firstOfLock)) break;
                    EnterState(State::Firing, now, "heli_hold");
                    continue;
                }
                if (NeedsHeliStationKeep(player.x, player.y, gLock.x, gLock.y)) {
                    if (!TryEnterMoveTo(now, "heli_reapproach")) break;
                    continue;
                }
                // 够不到又未到「要重飞」：旋翼收敛，勿落 heli_near 无条件出刀。
                if (!HeliReachOk(player.x, player.y, gLock.x, gLock.y)) break;
                if (!ports::attack::CanFirePrimaryEx(firstOfLock)) break;
                EnterState(State::Firing, now, "heli_near");
                continue;
            }
            // needApproachCorrect 未清时禁止 still_valid/near_band 空砍。
            if (same && !gLock.needApproachCorrect) {
                if (humanOn) {
                    // 出带迟滞：仍在宽松带内就续砍，勿 reapproach→立刻 human_in_band 抖 StopWalk。
                    if (InHumanHoldBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                        if (!ports::attack::CanFirePrimaryEx(firstOfLock)) break;
                        EnterState(State::Firing, now, "still_valid");
                        continue;
                    }
                    if (!TryEnterMoveTo(now, "reapproach_human")) break;
                    continue;
                }
                if (InMeleeHoldBand(player.x, player.y, gLock.x, gLock.y, standOff)) {
                    if (!ports::attack::CanFirePrimaryEx(firstOfLock)) break;
                    EnterState(State::Firing, now, "still_valid");
                    continue;
                }
            }
            if (tpOn && same && dx < 1.f) {
                if (!TryEnterMoveTo(now, LiveStepOn() ? "hug_follow" : "recenter_hug")) break;
                continue;
            }
            if (humanOn && same) {
                if (!TryEnterMoveTo(now, "reapproach_human")) break;
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
                if (!ports::attack::CanFirePrimaryEx(firstOfLock)) break;
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
        LogLine("layer_hop mode=1 (zMass+floor, maxHop chunk, fhBanSpread=%d; native_land "
                "snapTol=%.0f attachWait=%ums)",
                kLandFhBanSpread, kNativeSnapTolPx, (unsigned)kNativeAttachWaitMs);
    }
    ports::input::Init();
    ports::attack::Init();
    // teleport::EnsureBound 不在此急切：BindFns 会扫 FindClass/方法表，进场齐开会堵泵。
    // StabilizeFoothold / ClearMotionLatch / 其它入口内部已懒 BindFns。

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

// 换图统一入口（encounter::ResetForMapChange 与 Tick 地图边沿共用）。
// 旧行为：无条件 EndLieSafeLand → 回城卷进主城后拆掉落台，人悬空 freefall。
// 新行为：
//   · Travel 赶路中：不抢旋翼（贴门 settle 由 Travel 自己托）；只清残留速度。
//   · 硬闸持有者仍在且无 Travel：本图重钉落点继续飞。
//   · 空中换图（手动回城/F5 禁挂台）且无 Travel：MapArrive + 同测谎落台，站稳自清。
void OnCombatMapChange(const char* why) {
    // 换图 InterStage 会把 NM opcode HashSet 清空；丢掉 2s Rebind 缓存，下一刀才能看见空表。
    ports::attack_rpc::InvalidateAfterMapChange();
    const int mapId = ports::world::GetMapId();
    const DWORD now = GetTickCount();
    // 双入口去重：同 mapId 在短窗内只处理一次（仍记日志便于 BIN）。
    if (mapId > 0 && mapId == gLastMapChangeHandledId && gLastMapChangeHandledMs != 0 &&
        (now - gLastMapChangeHandledMs) < kMapChangeDedupeMs) {
        LogLine("OnCombatMapChange dedupe map=%d why=%s age=%ums", mapId, why ? why : "?",
                (unsigned)(now - gLastMapChangeHandledMs));
        return;
    }
    // encounter 首次 0→图号会 ResetForMapChange；F5 已在本图起飞时不得清 saw / 强制落台
    // （BIN 16:22:39：空中 ma=6 被 wait_onFh + lie_safe 按到 ma=5 站立）。
    if (why && std::strcmp(why, "ResetForMapChange") == 0 && mapId > 0 && mapId == gLastMapId) {
        LogLine("OnCombatMapChange skip same-map reset map=%d (keep fly)", mapId);
        return;
    }
    if (mapId > 0) {
        gLastMapChangeHandledId = mapId;
        gLastMapChangeHandledMs = now;
    }
    gSawOnFhThisMap = false;
    gFoxFillGateUntil = 0;
    SetHighValueLootUrgent(false);
    // 上一图的退避 latch / 放弃闩 / 熄火状态一律不带过图：新图重新给它一次机会。
    RequestDodgeReset("map_change");
    // 残留 Impact 速度带进新城会横甩/直坠；落台与否都先清。
    ClearLieSafeMotion();

    // BIN 9d504e：卖装无卷改 stick 赶路，换图若 RestartLieSafeLand 会抢 Travel 旋翼。
    if (travel::IsActive()) {
        BeginMapArmGrace(now, why ? why : "map_change");
        ArmHiraishinLootHold("map_change_travel");
        return;
    }

    const uint32_t holders =
        gHardPauseMask.load(std::memory_order_acquire) & kSafeLandHoldersMask;
    constexpr uint32_t kMapArriveBit =
        static_cast<uint32_t>(HardPauseHolder::MapArrive);
    const uint32_t sticky = holders & ~kMapArriveBit;  // AutoLie/Supply/Encounter
    const bool airborneGrace =
        gHeliAirborneUntilMs != 0 && static_cast<int>(now - gHeliAirborneUntilMs) < 0;
    const bool wasFlying = ports::fly_fh_ban::IsBanActive() ||
                           heli::CurrentOwner() == heli::Owner::Combat || airborneGrace;
    const bool landActive = gLieSafeLand.load(std::memory_order_acquire);
    const bool leftover = gLieFlyPausedByUs || gLieOweFlyBanRestore || landActive;

    if (sticky != 0 || (holders & kMapArriveBit) != 0 || landActive) {
        // 行程硬闸跨图 / 已在落台中：重钉本图台面，绝不拆掉。
        if ((holders & kMapArriveBit) == 0 && sticky == 0 && landActive) {
            // 落台中却无持有者（异常残留）：补 MapArrive，避免 FSM 抢旋翼。
            SetHardPause(HardPauseHolder::MapArrive, true);
        }
        RestartLieSafeLand(why && why[0] ? why : "map_change_resnap");
    } else if (wasFlying || leftover) {
        // 手动回城 / F5 空中过图：升沿开落台；站稳后 ReleaseMapArriveIfHeld 自清。
        if (leftover) EndLieSafeLand("map_change_cleanup");
        SetHardPause(HardPauseHolder::MapArrive, true);
        RestartLieSafeLand(why && why[0] ? why : "map_change_airborne");
    } else {
        // BIN 8fa033：赶路到站后开 F5，map 边沿触发但 wasFlying=0；人已悬空仍须钉台。
        ports::teleport::FlightState st{};
        if (ports::teleport::QueryFlightState(st) && st.ok && !st.onFh) {
            SetHardPause(HardPauseHolder::MapArrive, true);
            RestartLieSafeLand(why && why[0] ? why : "map_change_airborne");
        }
    }

    BeginMapArmGrace(now, why ? why : "map_change");
    ArmHiraishinLootHold("map_change");
}

// 同图软重连：清锁、丢掉 NM 攻包缓存、要求重新挂台；不套 1.5s map arm。
// wait_onFh 仍挡 CurFh=0；真换图走 OnCombatMapChange。
void OnCombatSameMapResume(const char* why) {
    ports::attack_rpc::InvalidateAfterMapChange();
    gSawOnFhThisMap = false;
    gFoxFillGateUntil = 0;
    ClearLock();
    ClearHitRotateState();
    ports::attack::ForceRelease();
    const int mapId = ports::world::GetMapId();
    LogLine("same-map resume skip map arm map=%d why=%s", mapId, why ? why : "?");
}

}  // namespace

void Init() {
    OpenLog();
    LogLine("Init");
    gEnabled.store(false);
    // 禁止无条件清 HardPause：进图冷启时 AutoSupply worker 可能已 PauseSystems。
    // BIN 14:50:54 Init 抹闸后 ForceApply 再开 F5 → Travel combat_on 刷屏。
    gExternalPauseDepth.store(0);
    const uint32_t keepPause = gHardPauseMask.load(std::memory_order_acquire);
    const bool hard = keepPause != 0;
    gExternalPause.store(hard, std::memory_order_release);
    ports::attack::SetFireSuppressed(hard);
    if (hard) {
        LogLine("Init keep HardPause mask=0x%x (already latched)", (unsigned)keepPause);
    }
    gTeleportEnabled.store(false);
    gHumanWalkEnabled.store(true);
    gState = State::Idle;
    gHeliLatchedThisEnable = false;
    gHeliAirborneUntilMs = 0;
    gSawOnFhThisMap = false;
    ClearLock();
    ClearHitRotateState();
    ClearSoftBan();
    // payload 可能在 Init/worker 之前 SetEnabled 抬过 BAN；此处必须卸掉，否则换图真空坠落。
    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
}

void Shutdown() {
    StopTeleportKickStress();
    StopWorker();
    SetEnabled(false);
    heli::Reset();
    gHeliAirborneUntilMs = 0;
    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::CombatImpact, false);
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
        // 纯普攻：关 F5 = 松攻击键 + 停状态机 + 停旋翼（顺带卸 fh-ban 让人落地）。
        ClearLock();
        ClearHitRotateState();
        ports::attack::ForceRelease();
        gState = State::Idle;
        gSettleUntil = 0;
        gNeedEnableFaceSettle.store(false, std::memory_order_release);
        gEnableHoldUntilMs = 0;
        ClearHiraishinLootHold();
        ports::attack::SetBurstPulse(false);
        gHeliLatchedThisEnable = false;
        heli::Reset();
        gHeliAirborneUntilMs = 0;
        gHeliRtbLatched = false;
        gHeliHoldValid = false;
        // 切回 idle 扫描节奏，别卡在 combat remain 上。
        mob_scan::RequestImmediateScan();
    } else {
        ClearSoftBan();
        ClearLock();
        ClearHitRotateState();
        gState = State::Idle;
        gSettleUntil = 0;
        gNeedEnableFaceSettle.store(true, std::memory_order_release);
        if (gHiraishinEnabled.load(std::memory_order_acquire)) {
            // 站桩输出原地等叠怪，不起飞。点按出刀，不按住、不 burst pulse。
            gEnableHoldUntilMs = 0;
            gHeliAirborneUntilMs = 0;
            gNeedEnableFaceSettle.store(false, std::memory_order_release);
            ArmHiraishinLootHold("f5_enable");
            LogLine("enable hiraishin (gather: no blink, melee tap)");
        } else {
            gEnableHoldUntilMs = x::runtime::NowMs() + kCombatEnableHoldMs;
            LatchCombatHeli("f5_enable");
            // 未挂台前禁止抬起飞宽限：Sync 会按宽限 BAN ON，进图 CurFh=0 时直接 detach。
            if (CombatSpawnAllowsBan()) {
                gHeliAirborneUntilMs = x::runtime::NowMs() + kHeliAirborneGraceMs;
            } else {
                gHeliAirborneUntilMs = 0;
                LogLine("enable defer takeoff (wait foothold/onFh)");
            }
            LogLine("enable hold %ums (takeoff now; first fire delayed)",
                    (unsigned)kCombatEnableHoldMs);
        }
        // 赶路/F6/吸怪寻簇占旋翼时 Reset 会把主人清掉。只清 Combat 时钟不够，直接跳过。
        if (!HeliHeldByPeer()) {
            heli::Reset();
        }
        // 同图热开：人已挂台才清 arm。首次进图 CurFh=0 时若当热开，会跳过 OnCombatMapChange
        // 和 lie_safe，BAN 把真空站姿摘台（BIN 15:46）。
        if (ports::world::IsPlayReady()) {
            const int mapId = ports::world::GetMapId();
            ports::player_combat::CombatCtx player{};
            ports::teleport::FlightState st{};
            const bool onFh =
                ports::teleport::QueryFlightState(st) && st.ok && st.onFh;
            if (mapId > 0 && ports::player_combat::QueryCombatCtx(player) && player.ok && onFh) {
                if (gLastMapId == mapId || gLastMapId < 0) {
                    gLastMapId = mapId;
                    gSawOnFhThisMap = true;
                    if (gMapArmUntilMs) {
                        LogLine("warm start clear arm map=%d", mapId);
                    }
                    gMapArmUntilMs = 0;
                }
            }
        }
        x::features::auto_supply::RecordHangupFarmMap("simple_combat_on");
        // 开打怪：立刻扫一帧，避免卡在 idle 360ms Wait。
        mob_scan::RequestImmediateScan();
        // 热开兜底：即便 Tick 预装未跑到，也先装钩再 Sync（仍可能本拍不 BAN）。
        (void)ports::fly_fh_ban::WarmInstall();
    }
    SyncImpactFhBan();
    LogLine("SetEnabled %d teleport=%d human=%d liveStep=%d standOff=%u minDx=%u fhBan=%d",
            on ? 1 : 0, gTeleportEnabled.load() ? 1 : 0, gHumanWalkEnabled.load() ? 1 : 0,
            gLiveStepEnabled.load() ? 1 : 0, gTeleportStandOff.load(), gTeleportMinDx.load(),
            ports::fly_fh_ban::IsBanActive() ? 1 : 0);
}

bool IsEnabled() { return gEnabled.load(std::memory_order_acquire); }

void NotifyMultiNormalAttackFired() {
    if (!gEnabled.load(std::memory_order_acquire)) return;
    if (!gLock.id) return;
    // 多发 NA 真正挥出：从此刻起算观察窗（hp/lastHitted 用当前锁快照）。
    const DWORD now = GetTickCount();
    float absDx = 0.f;
    ports::player_combat::CombatCtx player{};
    if (ports::player_combat::QueryCombatCtx(player) && player.ok) {
        absDx = std::fabs(gLock.x - player.x);
    }
    ArmWhiffObserve(now, gLock.lastHp, absDx);
    x::features::ports::mob_gather::NoteHangupFire();
}

bool IsFarmingActive() {
    return gEnabled.load(std::memory_order_acquire) &&
           gHardPauseMask.load(std::memory_order_acquire) == 0;
}

bool IsLootPulseActive() {
    // 高价值紧急：打断 Firing，强制放行吸物
    if (IsHighValueLootUrgent()) return true;
    // 未真正挂机：吸物自由跑。
    if (!IsFarmingActive()) return true;
    // 挂机：仅 Firing 让路。Aim/Recover 放行——否则 Impact 循环几乎永不吸
    // （BIN：yield combat_fire_window 刷屏、drops 堆到 30+ 仍 0 行 mode=petmap）。
    return gState != State::Firing;
}

uint32_t LootPulseGeneration() {
    return gLootPulseGen.load(std::memory_order_acquire);
}

bool IsHighValueLootUrgent() { return gHvLootUrgent.load(std::memory_order_acquire); }

void SetHighValueLootUrgent(bool on) {
    const bool prev = gHvLootUrgent.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    if (on) {
        if (!gHvLootPauseHeld.exchange(true, std::memory_order_acq_rel)) {
            AcquireExternalPause();
        }
        ArmLootPulse(GetTickCount(), 200u, /*edge=*/true);
        LogLine("highValueLoot urgent=1 (pause fire → loot first)");
    } else {
        if (gHvLootPauseHeld.exchange(false, std::memory_order_acq_rel)) {
            ReleaseExternalPause();
        }
        LogLine("highValueLoot urgent=0");
    }
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
    // 仅状态变化打一行。BIN 长期只有 cluster=-1 且从无本行 = 从未从 0→1（未真正下发）。
    LogLine("SetClusterPriority %d (prev=%d)", on ? 1 : 0, prev ? 1 : 0);
}

bool IsClusterPriority() { return gClusterPriority.load(std::memory_order_acquire); }

void SetHitRotateEnabled(bool on) {
    const bool prev = gHitRotateEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    if (!on) {
        ClearHitRotateState();
        ports::hit_pin::SetWishOid(0);
    }
    ports::hit_pin::SetWanted(on);
    LogLine("SetHitRotateEnabled %d (prev=%d) hit_pin_armed=%d", on ? 1 : 0, prev ? 1 : 0,
            ports::hit_pin::IsArmed() ? 1 : 0);
}

bool IsHitRotateEnabled() { return gHitRotateEnabled.load(std::memory_order_acquire); }

void SetHitRotateN(uint32_t n) {
    n = xcat::ClampCombatHitRotateN(n);
    const int prev = gHitRotateN.exchange(static_cast<int>(n), std::memory_order_acq_rel);
    if (prev == static_cast<int>(n)) return;
    LogLine("SetHitRotateN %u (prev=%d)", n, prev);
}

uint32_t HitRotateN() {
    return static_cast<uint32_t>(gHitRotateN.load(std::memory_order_acquire));
}

void SetSkipAccMissEnabled(bool on) {
    const bool prev = gSkipAccMissEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetSkipAccMissEnabled %d (prev=%d)", on ? 1 : 0, prev ? 1 : 0);
}

bool IsSkipAccMissEnabled() { return gSkipAccMissEnabled.load(std::memory_order_acquire); }

void SetSkipAccMissN(uint32_t n) {
    n = xcat::ClampCombatSkipAccMissN(n);
    const int prev = gSkipAccMissN.exchange(static_cast<int>(n), std::memory_order_acq_rel);
    if (prev == static_cast<int>(n)) return;
    LogLine("SetSkipAccMissN %u (prev=%d)", n, prev);
}

uint32_t SkipAccMissN() {
    return static_cast<uint32_t>(gSkipAccMissN.load(std::memory_order_acquire));
}

void SetForgeHitEnabled(bool on) {
    const bool prev = gForgeHitEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetForgeHitEnabled %d (prev=%d)", on ? 1 : 0, prev ? 1 : 0);
}

bool IsForgeHitEnabled() { return gForgeHitEnabled.load(std::memory_order_acquire); }

void SetForgeHitFrontBox(uint32_t dx, uint32_t dy) {
    dx = xcat::ClampForgeHitFrontDx(dx);
    dy = xcat::ClampForgeHitFrontDy(dy);
    const uint32_t prevDx = gForgeHitFrontDx.exchange(dx, std::memory_order_acq_rel);
    const uint32_t prevDy = gForgeHitFrontDy.exchange(dy, std::memory_order_acq_rel);
    ports::attack_rpc::SetLockFrontBox(dx, dy);
    if (prevDx == dx && prevDy == dy) return;
    LogLine("SetForgeHitFrontBox dx=%u dy=%u (prev=%u,%u)", dx, dy, prevDx, prevDy);
}

uint32_t ForgeHitFrontDx() { return gForgeHitFrontDx.load(std::memory_order_acquire); }

uint32_t ForgeHitFrontDy() { return gForgeHitFrontDy.load(std::memory_order_acquire); }

void SetTeleportEnabled(bool on) {
    const bool prev = gTeleportEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetTeleportEnabled %d", on ? 1 : 0);
}

bool IsTeleportEnabled() { return gTeleportEnabled.load(std::memory_order_acquire); }

void SetTeleportOneHit(bool on) {
    const bool prev = gTeleportOneHit.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetTeleportOneHit %d", on ? 1 : 0);
}

bool IsTeleportOneHit() { return gTeleportOneHit.load(std::memory_order_acquire); }

void SetImpactApproachEnabled(bool on) {
    const bool prev = gImpactApproachEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    if (on) {
        (void)ports::attack::StopWalk();
    }
    heli::Reset();
    gHeliAirborneUntilMs = 0;
    SyncImpactFhBan();
    LogLine("SetImpactApproachEnabled %d fhBan=%d", on ? 1 : 0,
            ports::fly_fh_ban::IsBanActive() ? 1 : 0);
}

bool IsImpactApproachEnabled() {
    return gImpactApproachEnabled.load(std::memory_order_acquire);
}

void SetAntiJitterEnabled(bool on) {
    const bool prev = gAntiJitterEnabled.exchange(on, std::memory_order_acq_rel);
    // 软悬停与钉点必须同开同关（关防抖只清钉 → ImGui「勾不勾都几乎不抖」）。
    heli::SetSoftSettleEnabled(on);
    if (prev == on) return;
    if (!on) {
        gStationStickValid = false;
        gStationStickLockId = 0;
    }
    LogLine("SetAntiJitterEnabled %d", on ? 1 : 0);
}

bool IsAntiJitterEnabled() {
    return gAntiJitterEnabled.load(std::memory_order_acquire);
}

void SetAntiHugEnabled(bool on) {
    const bool prev = gAntiHugEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;  // IPC 每拍下发全量配置：没变就别刷日志
    // 关掉下一拍即回 mx ± standOff（ComputeDodge 每拍开头都清 gDodge）。
    // 开启也复位一次，免得沿用上次的熄火/放弃状态导致「勾了没反应」。
    RequestDodgeReset(on ? "enable" : "disable");
    LogLine("SetAntiHugEnabled %d", on ? 1 : 0);
}

bool IsAntiHugEnabled() {
    return gAntiHugEnabled.load(std::memory_order_acquire);
}

void SetFlySpeedPct(unsigned pct) {
    const float scale = static_cast<float>(pct) / 100.f;
    const float prev = heli::SpeedScale(heli::Owner::Combat);
    // 打怪与赶路共用面板这一个旋钮：两者都是「自动飞」，分开调没有产品意义。
    // F6 手动飞另有自己的一份（heli::Owner::Fly），见 fly::SetSpeedPct。
    heli::SetSpeedScale(heli::Owner::Combat, scale);
    heli::SetSpeedScale(heli::Owner::Travel, scale);
    heli::SetSpeedScale(heli::Owner::Gather, scale);
    const float now = heli::SpeedScale(heli::Owner::Combat);
    // Clamp 后仍相同就不刷日志：IPC 每次下发全量配置，否则每轮都打一行。
    if (std::fabs(now - prev) < 1e-3f) return;
    LogLine("SetFlySpeedPct %u → %.2fX (req %.2f)%s", pct, now, scale,
            now + 0.05f >= 10.0f ? " full_fire=1" : "");
}

unsigned FlySpeedPct() {
    return static_cast<unsigned>(heli::SpeedScale(heli::Owner::Combat) * 100.f + 0.5f);
}

void SetHumanWalkEnabled(bool on) {
    const bool prev = gHumanWalkEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    if (!on) {
        // 切回瞬移/Impact：立刻清走路锁存，避免 InputX 粘住。
        (void)ports::attack::StopWalk();
    }
    LogLine("SetHumanWalkEnabled %d", on ? 1 : 0);
}

bool IsHumanWalkEnabled() { return gHumanWalkEnabled.load(std::memory_order_acquire); }

void SetHiraishinEnabled(bool on) {
    const bool prev = gHiraishinEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    if (on) {
        (void)ports::attack::StopWalk();
        UnlatchCombatHeli("hiraishin_on");
        if (!HeliHeldByPeer()) {
            heli::Reset();
        }
        gHeliAirborneUntilMs = 0;
        SyncImpactFhBan();
        if (gEnabled.load(std::memory_order_acquire)) {
            gNeedEnableFaceSettle.store(false, std::memory_order_release);
            ArmHiraishinLootHold("mode_on");
        }
        LogLine("enable hiraishin (gather: no blink, melee tap)");
    } else {
        ClearHiraishinLootHold();
        ports::attack::SetBurstPulse(false);
    }
    LogLine("SetHiraishinEnabled %d", on ? 1 : 0);
}

bool IsHiraishinEnabled() { return gHiraishinEnabled.load(std::memory_order_acquire); }

void SetHiraishinLootHoldMs(uint32_t ms) {
    ms = xcat::ClampHiraishinLootHoldMs(ms);
    const DWORD prev = gHiraishinLootHoldMs.exchange(ms, std::memory_order_acq_rel);
    if (prev == ms) return;
    if (ms == 0) ClearHiraishinLootHold();
    LogLine("SetHiraishinLootHoldMs %u (prev=%u)", ms, (unsigned)prev);
}

uint32_t HiraishinLootHoldMs() {
    return gHiraishinLootHoldMs.load(std::memory_order_acquire);
}

void SetHiraishinRangePx(uint32_t px) {
    px = xcat::ClampHiraishinRangePx(px);
    const uint32_t prev = gHiraishinRangePx.exchange(px, std::memory_order_acq_rel);
    if (prev == px) return;
    LogLine("SetHiraishinRangePx %u (prev=%u)", px, prev);
}

uint32_t HiraishinRangePx() {
    return gHiraishinRangePx.load(std::memory_order_acquire);
}

void SetHiraishinFrontBox(uint32_t dx, uint32_t dy) {
    dx = xcat::ClampHiraishinFrontDx(dx);
    dy = xcat::ClampHiraishinFrontDy(dy);
    const uint32_t prevDx = gHiraishinFrontDx.exchange(dx, std::memory_order_acq_rel);
    const uint32_t prevDy = gHiraishinFrontDy.exchange(dy, std::memory_order_acq_rel);
    if (prevDx == dx && prevDy == dy) return;
    LogLine("SetHiraishinFrontBox dx=%u dy=%u (prev=%u,%u)", dx, dy, prevDx, prevDy);
}

uint32_t HiraishinFrontDx() {
    return gHiraishinFrontDx.load(std::memory_order_acquire);
}

uint32_t HiraishinFrontDy() {
    return gHiraishinFrontDy.load(std::memory_order_acquire);
}

void SetLiveStepEnabled(bool on) {
    const bool prev = gLiveStepEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    LogLine("SetLiveStepEnabled %d", on ? 1 : 0);
}

bool IsLiveStepEnabled() { return gLiveStepEnabled.load(std::memory_order_acquire); }

void SetTeleportParams(uint32_t minDx, uint32_t standOff, uint32_t cooldownMs, uint32_t maxHop) {
    if (minDx < 160) minDx = 160;
    if (minDx > 2000) minDx = 2000;
    standOff = xcat::ClampCombatTeleportStandOff(standOff);
    cooldownMs = xcat::ClampCombatTeleportCooldownMs(cooldownMs);
    // 旧默认 400/520/550 会导致远图 hop_chunk_fail；与 payload 落盘迁移一致。
    if (!maxHop || xcat::IsRetiredCombatTeleportMaxHopDefault(maxHop))
        maxHop = xcat::kCombatTeleportMaxHopDefault;
    maxHop = xcat::ClampCombatTeleportMaxHop(maxHop);
    gTeleportMinDx.store(minDx, std::memory_order_release);
    gTeleportStandOff.store(standOff, std::memory_order_release);
    gTeleportCooldownMs.store(cooldownMs, std::memory_order_release);
    gTeleportMaxHop.store(maxHop, std::memory_order_release);
}

void SetStandOffParams(bool custom, uint32_t x, int32_t y) {
    x = xcat::ClampCombatStandOffX(x);
    y = xcat::ClampCombatStandOffY(y);
    const bool prevCustom = gStandOffCustom.exchange(custom, std::memory_order_acq_rel);
    const uint32_t prevX = gStandOffX.exchange(x, std::memory_order_acq_rel);
    const int32_t prevY = gStandOffY.exchange(y, std::memory_order_acq_rel);
    // IPC 每拍下发全量配置：没变就别刷日志。
    if (prevCustom == custom && prevX == x && prevY == y) return;
    LogLine("SetStandOffParams custom=%d x=%u y=%d (builtin x=%.0f y=%.0f)", custom ? 1 : 0, x, y,
            kHeliStandOffPx, kHeliLiftPx);
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
    // 测谎 / 补给 / 遇人 / 换频：先闩落台再置硬闸，避免 GoIdle→Sync 卸 ban → 图底 freefall。
    // ChannelHop 也曾漏网：打怪空中点「随机换频」只停旋翼不落台 → 直坠。
    if ((bit & kSafeLandHoldersMask) != 0 && on) {
        const uint32_t cur = gHardPauseMask.load(std::memory_order_acquire);
        if ((cur & bit) == 0) {
            // 赶路中遇人/换频硬闸：只钉位，禁止 BeginLieSafeLand 抢旋翼
            // （RequestSafeLand 已对 travel 让路；本入口原先漏了）。
            const bool travelSkipLand =
                travel::IsActive() &&
                (bit == static_cast<uint32_t>(HardPauseHolder::ChannelHop) ||
                 bit == static_cast<uint32_t>(HardPauseHolder::Encounter));
            if (travelSkipLand) {
                LogLine("lie_safe_land skip begin why=%s (travel active)",
                        SafeLandBeginWhy(holder));
            } else if ((cur & kSafeLandHoldersMask) == 0) {
                BeginLieSafeLand(SafeLandBeginWhy(holder));
            } else {
                LogLine("lie_safe_land skip begin why=%s (holder already mask=0x%x)",
                        SafeLandBeginWhy(holder), (unsigned)(cur & kSafeLandHoldersMask));
            }
        }
    }
    uint32_t prev = gHardPauseMask.load(std::memory_order_acquire);
    uint32_t next = prev;
    for (;;) {
        next = on ? (prev | bit) : (prev & ~bit);
        if (gHardPauseMask.compare_exchange_weak(prev, next, std::memory_order_acq_rel)) break;
    }
    RefreshExternalPauseEffective();
    if ((bit & kSafeLandHoldersMask) != 0 && !on) {
        const bool wasOn = (prev & bit) != 0;
        // 另一持有者仍要安全站稳时勿拆落台 / 勿提前恢复 Fly ban。
        if (wasOn && (next & kSafeLandHoldersMask) == 0) EndLieSafeLand(SafeLandEndWhy(holder));
    }
}

bool IsSafeLandActive() { return gLieSafeLand.load(std::memory_order_acquire); }

void RequestSafeLand(const char* why) {
    // BIN 9d504e：卖装 stick 赶路中换图若 Combat 抢旋翼会和 Travel 互踩。
    if (travel::IsActive()) {
        LogLine("lie_safe_land request skip travel_active why=%s", why ? why : "?");
        return;
    }
    const char* w = (why && why[0]) ? why : "request";
    if (gLieSafeLand.load(std::memory_order_acquire)) {
        RestartLieSafeLand(w);
    } else {
        BeginLieSafeLand(w);
    }
    // 无其它落台持有者时靠 MapArrive 保闸；AutoSupply 等已持有则升沿只钉位。
    SetHardPause(HardPauseHolder::MapArrive, true);
}

void CancelSafeLand(const char* why) {
    if (!gLieSafeLand.load(std::memory_order_acquire)) return;
    EndLieSafeLand(why && why[0] ? why : "cancel");
    ReleaseMapArriveIfHeld();
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
    OnCombatMapChange("ResetForMapChange");
}

void RequestTeleportToRandomMob() {
    LogLine("tp_test refused (native fill+Doing disabled)");
}

void RequestNativeTeleportCall() {
    LogLine("tp_native refused (native fill+Doing disabled)");
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
    (void)startMs;
    (void)stepMs;
    (void)floorMs;
    (void)hopsPerLevel;
    (void)localShuttle;
    LogLine("kick_stress refused (native fill+Doing disabled) mode=%s",
            modeTag ? modeTag : "?");
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
    LogLine("tp_native_mob refused (native fill+Doing disabled)");
}

void Tick(DWORD nowMs) { TickImpl(nowMs); }

}  // namespace x::features::simple_combat
