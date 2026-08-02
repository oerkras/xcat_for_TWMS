#pragma once

#include <cstdint>
#include <cstddef>

namespace xcat {

// TWMS ???????launcher <-> payload??? user.ini [core]?
constexpr uint32_t kPayloadControlMagic = 0x58435443u;  // 'XCTC'
constexpr uint32_t kPayloadControlVersion = 1u;
constexpr uint32_t kPayloadControlCoreIniVersion = 39u;
// flyMode: 0=点击飞(A) 1=跟随飞(B)
constexpr uint32_t kFlyModeClick = 0u;
constexpr uint32_t kFlyModeFollow = 1u;
constexpr uint32_t kFlyModeDefault = kFlyModeFollow;
// 每一飞自冷却（点击飞/跟随飞共用）
constexpr uint32_t kFlyHopCdDefaultMs = 16u;
constexpr uint32_t kFlyHopCdMinMs = 5u;
constexpr uint32_t kFlyHopCdMaxMs = 2000u;
// hangup hour mask: bit0=00:00 .. bit23=23:00 (local time).
constexpr uint32_t kHangupScheduleMaskAll = 0x00FFFFFFu;
constexpr uint32_t kWatchdogNoExpSecDefault = 180u;
constexpr uint32_t kWatchdogNoExpSecMin = 30u;
constexpr uint32_t kWatchdogNoExpSecMax = 3600u;
constexpr uint32_t kWatchdogCooldownSecDefault = 20u;
constexpr uint32_t kWatchdogCooldownSecMin = 20u;
constexpr uint32_t kWatchdogCooldownSecMax = 3600u;
constexpr size_t kPayloadWorldNameCap = 64;
// 默认分区：雪吉拉（_Center1）
constexpr int32_t kDefaultWorldId = 1;
constexpr const char* kDefaultWorldName = "雪吉拉";
constexpr uint32_t kMultiSkillGapDefaultMs = 120u;
constexpr uint32_t kMultiSkillGapMinMs = 40u;
constexpr uint32_t kMultiSkillGapMaxMs = 500u;
// 默认 50；下限 5（用户自选；hold 地板同步）。
constexpr uint32_t kSimpleCombatAttackIntervalDefaultMs = 50u;
constexpr uint32_t kSimpleCombatAttackIntervalMinMs = 5u;
constexpr uint32_t kSimpleCombatAttackIntervalMaxMs = 10000u;
// 简易战斗 worker 心跳（状态机 Tick）；越短出刀机会越多，CPU/主线程更忙。
constexpr uint32_t kSimpleCombatTickDefaultMs = 16u;
constexpr uint32_t kSimpleCombatTickMinMs = 5u;
constexpr uint32_t kSimpleCombatTickMaxMs = 100u;
// 与全局 Min 对齐；加速不另抬间隔。
constexpr uint32_t kAttackAccelIntervalFloorMs = 5u;
// 群怪优先：落盘仍用 clusterWeight；0=关，非 0=开（旧 1–100 权重一律视为开）。
constexpr uint32_t kClusterWeightDefault = 0u;
constexpr uint32_t kClusterWeightMax = 100u;
// v38: attack_rpc 实验探针（默认关）；多怪/间隔/伤害占位
constexpr uint32_t kAttackRpcMobsDefault = 1u;
constexpr uint32_t kAttackRpcMobsMin = 1u;
constexpr uint32_t kAttackRpcMobsMax = 15u;
constexpr uint32_t kAttackRpcIntervalDefaultMs = 500u;
constexpr uint32_t kAttackRpcIntervalMinMs = 50u;
constexpr uint32_t kAttackRpcIntervalMaxMs = 5000u;
constexpr uint32_t kAttackRpcDamageDefault = 1u;
constexpr uint32_t kAttackRpcDamageMin = 1u;
constexpr uint32_t kAttackRpcDamageMax = 999999u;
// ?????MovePath Attr=4???????? Attr=4 ?????????BIN 04:40?
constexpr uint32_t kCombatTeleportMinDxDefault = 220u;
constexpr uint32_t kCombatTeleportMinDxMin = 160u;
constexpr uint32_t kCombatTeleportMinDxMax = 2000u;
// ???? ? ?????BIN standOff=90 > fireReach ?????????????????
constexpr uint32_t kCombatTeleportStandOffDefault = 40u;  // 侧位；过贴易 whiff
constexpr uint32_t kCombatTeleportStandOffMin = 12u;
// UI / simple_combat 上界；过大无意义
constexpr uint32_t kCombatTeleportStandOffMax = 200u;
// 贴怪/LiveStep 共用面板冷却；默认 50；下限 5（大 hop 另有 80/120 地板）。
constexpr uint32_t kCombatTeleportCooldownDefaultMs = 50u;
constexpr uint32_t kCombatTeleportCooldownMinMs = 5u;
constexpr uint32_t kCombatTeleportCooldownMaxMs = 8000u;

struct PayloadControl {
    uint32_t magic = kPayloadControlMagic;
    uint32_t version = kPayloadControlVersion;
    uint32_t invuln = 1;  // 默认开
    // v27/v38: 攻击加速（开=跳过动作等待；间隔默认 50，下限 5）
    uint32_t attackAccel = 0;
    // v23: 飞行武装（面板勾选 / F6）；策略由 flyMode 决定；不钉台。
    // 开关为会话态（state/fly_armed），不写入 user.ini；重启 launcher/注入后归零。
    uint32_t fly = 0;
    // v24: 0=点击飞(A) 1=跟随飞(B)；（旧值 2=吸附飞已退役，读入时 Clamp 回默认）
    uint32_t flyMode = kFlyModeDefault;
    // v25: 每一飞间隔 ms（A/B 共用自冷却；C 不使用）
    uint32_t flyHopCdMs = kFlyHopCdDefaultMs;
    uint32_t autoEnter = 0;   // ????????->?????->??
    uint32_t charSlot = 1;    // 1-based ????
    int32_t worldId = kDefaultWorldId;  // 默认雪吉拉
    char worldName[kPayloadWorldNameCap]{"雪吉拉"};
    uint32_t hpPotion = 1;         // 自动加血（默认开）
    uint32_t mpPotion = 1;         // 自动加蓝（默认开）
    uint32_t hpThresholdPct = 50;  // 1-99
    uint32_t mpThresholdPct = 30;  // 1-99
    uint32_t petSummon = 1;              // 自动召唤宠物（默认开）
    uint32_t petSummonRequireFood = 1;   // 1=有粮才召（默认开）
    uint32_t multiSkill = 0;              // ???????
    uint32_t multiSkillGapMs = kMultiSkillGapDefaultMs;
    uint32_t multiSkillSafeStagger = 1;   // ?????????>=120?
    uint32_t simpleCombat = 0;            // ????????
    uint32_t simpleCombatSmartInterval = 0;  // ???????????
    uint32_t simpleCombatAttackIntervalMs = kSimpleCombatAttackIntervalDefaultMs;
    // v39: 打怪状态机 Tick 间隔（首页「TICK值」）；默认 16，下限 5
    uint32_t simpleCombatTickMs = kSimpleCombatTickDefaultMs;
    uint32_t clusterWeight = kClusterWeightDefault;  // 0=最近优先；非0=群怪优先
    // 贴怪瞬移：产品默认开；面板无开关，始终下发开。
    uint32_t simpleCombatTeleport = 1;
    uint32_t simpleCombatTeleportMinDx = kCombatTeleportMinDxDefault;
    uint32_t simpleCombatTeleportStandOff = kCombatTeleportStandOffDefault;
    uint32_t simpleCombatTeleportCooldownMs = kCombatTeleportCooldownDefaultMs;
    // v26: 锁怪后同层微瞬移贴位（近似枫星 LiveStep）；默认关；依赖贴怪瞬移
    uint32_t simpleCombatLiveStep = 0;
    // v38: 结算层攻包探针（Create50+Encode+SendOutPacket）；默认关；实验 TAB
    uint32_t attackRpc = 0;
    uint32_t attackRpcMobs = kAttackRpcMobsDefault;
    uint32_t attackRpcIntervalMs = kAttackRpcIntervalDefaultMs;
    uint32_t attackRpcDamage = kAttackRpcDamageDefault;
    uint32_t autoLie = 0;  // ?????TextCaptcha+LLM / NonFinite ????
    uint32_t autoLieDryRun = 0;         // 1=???LLM ??? OnOk
    uint32_t autoLieAlarmTestSeq = 0;   // ???? payload ????
    uint32_t autoLieMouseSmokeSeq = 0;  // ???????????? UI?
    // v19: hangup "random channel" edge command. Bump on click/F10; payload channel_hop.
    uint32_t manualRejoinSeq = 0;
    // v21: 面板测试贴怪 → fill+Doing 随机怪（原 ImpactBlink 已拆除）
    uint32_t teleportTestSeq = 0;
    // v22: 面板短距 native CALL
    uint32_t teleportNativeTestSeq = 0;
    // v23: 踢号压测 — 随机贴怪 fill+Doing，间隔由慢→快直到断线
    uint32_t teleportKickStressSeq = 0;
    // v24: 细扫档 50→0ms（步进 5，每级 12 跳）
    uint32_t teleportKickStressFineSeq = 0;
    // v25: 钉地板 30→10ms（不到 5/0）
    uint32_t teleportKickStressFine10Seq = 0;
    // v26: 原地短跳（排除距离）
    uint32_t teleportKickStressLocalSeq = 0;
    // v20: ????????? UX????? UserPool + channel_hop?? Reload?
    uint32_t autoRelogin = 0;             // ??????
    uint32_t autoReloginStopCombat = 1;   // ???
    uint32_t autoReloginReconnect = 1;    // ???????
    // ????????? DragManager.CanPerformAction ??????????
    uint32_t dropAlertBypass = 1;
    // Deprecated（经典版）：补给真源为 user.ini [auto_supply]。
    // 字段仅保留结构布局兼容；Read/WritePayloadControl 不再读写，并会清掉 core.autoSell*。
    uint32_t autoSell = 0;
    char autoSellShopMap[64]{};
    uint32_t autoSellReturnFarmSeq = 0;
    uint32_t autoSellAbortSeq = 0;
    // v12 hangup schedule (launcher-only). Off => ignore mask; on => kill/start by hour.
    // Missing key defaults to 0 (never auto-enable).
    uint32_t launcherHangupSchedule = 0;
    uint32_t launcherHangupScheduleMask = kHangupScheduleMaskAll;
    // v13 guardian (launcher-only): no-exp / crash clean relaunch. Default on (fengxing-aligned).
    uint32_t launcherWatchdog = 1;
    uint32_t launcherWatchdogNoExpSec = kWatchdogNoExpSecDefault;
    uint32_t launcherWatchdogCooldownSec = kWatchdogCooldownSecDefault;
    uint64_t writeTickMs = 0;
};

inline uint32_t ClampHangupScheduleMask(uint32_t mask) {
    return mask & kHangupScheduleMaskAll;
}

inline uint32_t ClampWatchdogNoExpSec(uint32_t v) {
    if (v < kWatchdogNoExpSecMin) return kWatchdogNoExpSecMin;
    if (v > kWatchdogNoExpSecMax) return kWatchdogNoExpSecMax;
    return v;
}

inline uint32_t ClampWatchdogCooldownSec(uint32_t v) {
    if (v < kWatchdogCooldownSecMin) return kWatchdogCooldownSecMin;
    if (v > kWatchdogCooldownSecMax) return kWatchdogCooldownSecMax;
    return v;
}

inline uint32_t ClampMultiSkillGapMs(uint32_t ms) {
    if (ms < kMultiSkillGapMinMs) return kMultiSkillGapMinMs;
    if (ms > kMultiSkillGapMaxMs) return kMultiSkillGapMaxMs;
    return ms;
}

inline uint32_t ClampSimpleCombatAttackIntervalMs(uint32_t ms) {
    if (ms < kSimpleCombatAttackIntervalMinMs) return kSimpleCombatAttackIntervalMinMs;
    if (ms > kSimpleCombatAttackIntervalMaxMs) return kSimpleCombatAttackIntervalMaxMs;
    return ms;
}

inline uint32_t ClampSimpleCombatTickMs(uint32_t ms) {
    if (ms < kSimpleCombatTickMinMs) return kSimpleCombatTickMinMs;
    if (ms > kSimpleCombatTickMaxMs) return kSimpleCombatTickMaxMs;
    return ms;
}

// 加速开：间隔不得低于地板（清忙锁后过短会打断上一刀命中窗）。
inline uint32_t EffectiveSimpleCombatAttackIntervalMs(uint32_t ms, uint32_t attackAccel) {
    uint32_t v = ClampSimpleCombatAttackIntervalMs(ms);
    if (attackAccel && v < kAttackAccelIntervalFloorMs) v = kAttackAccelIntervalFloorMs;
    return v;
}

inline uint32_t ClampClusterWeight(uint32_t w) {
    if (w > kClusterWeightMax) return kClusterWeightMax;
    return w;
}

inline uint32_t ClampAttackRpcMobs(uint32_t n) {
    if (n < kAttackRpcMobsMin) return kAttackRpcMobsMin;
    if (n > kAttackRpcMobsMax) return kAttackRpcMobsMax;
    return n;
}

inline uint32_t ClampAttackRpcIntervalMs(uint32_t ms) {
    if (ms < kAttackRpcIntervalMinMs) return kAttackRpcIntervalMinMs;
    if (ms > kAttackRpcIntervalMaxMs) return kAttackRpcIntervalMaxMs;
    return ms;
}

inline uint32_t ClampAttackRpcDamage(uint32_t d) {
    if (d < kAttackRpcDamageMin) return kAttackRpcDamageMin;
    if (d > kAttackRpcDamageMax) return kAttackRpcDamageMax;
    return d;
}

inline uint32_t ClampCombatTeleportMinDx(uint32_t v) {
    if (v < kCombatTeleportMinDxMin) return kCombatTeleportMinDxMin;
    if (v > kCombatTeleportMinDxMax) return kCombatTeleportMinDxMax;
    return v;
}

inline uint32_t ClampCombatTeleportStandOff(uint32_t v) {
    if (v < kCombatTeleportStandOffMin) return kCombatTeleportStandOffMin;
    if (v > kCombatTeleportStandOffMax) return kCombatTeleportStandOffMax;
    return v;
}

inline uint32_t ClampCombatTeleportCooldownMs(uint32_t v) {
    if (v < kCombatTeleportCooldownMinMs) return kCombatTeleportCooldownMinMs;
    if (v > kCombatTeleportCooldownMaxMs) return kCombatTeleportCooldownMaxMs;
    return v;
}

inline uint32_t ClampFlyMode(uint32_t v) {
    if (v > kFlyModeFollow) return kFlyModeDefault;
    return v;
}

inline uint32_t ClampFlyHopCdMs(uint32_t v) {
    if (v < kFlyHopCdMinMs) return kFlyHopCdMinMs;
    if (v > kFlyHopCdMaxMs) return kFlyHopCdMaxMs;
    return v;
}

void PayloadControlSetDefaults(PayloadControl& out);
bool ReadPayloadControl(const char* binDir, PayloadControl& out);
bool WritePayloadControl(const char* binDir, const PayloadControl& control);

// 飞行开关会话态：清零（launcher 启动 / DLL Init）。不改 flyMode / hopCd。
void ClearFlyArmedSession(const char* binDir);

}  // namespace xcat
