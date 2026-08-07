#pragma once

#include <cstdint>
#include <cstddef>

namespace xcat {

// TWMS ???????launcher <-> payload??? user.ini [core]?
constexpr uint32_t kPayloadControlMagic = 0x58435443u;  // 'XCTC'
constexpr uint32_t kPayloadControlVersion = 1u;
constexpr uint32_t kPayloadControlCoreIniVersion = 61u;
// v47: 引擎帧率锁（非显示器 Hz）
// v48: finalAttackForce — 普攻必出终极一击（SkillLevelData.Prop=100）
// v49: finalAttackForce — Prop=100 + 强制注册 FinalAttack / TryDoingFinalAttack
// v50: finalAttackForce — GetItem(-11) 读武器 + 已学 FA 武器回退（修 no weapon）
// v51: mobScanIntervalMs — 打怪开启时 MobPool 扫描周期（首页「怪物读取速度」）
// v52: finalAttackForce — 只写 FinalAttack 结构，不再主动调 TryDoingFinalAttack/GetItem
// v53: finalAttackForce — StartTick 改游戏钟 + 待发刷新 + Equipped 读武器 + 再调 Doing
// v54: skillMaxLevel — 已学技能 SkillRecord 等级按 GetMaxLevel 生效
// v55: pumpDrainBudget — 主线程泵每 tick Drain 上限（调试 TAB）
// v56: mobScanIntervalMs 默认 50→20；读盘迁旧默认 + 换怪按需刷新
// v57: 位移试推 A/B oneshot；ini=moveProbe*（旧 impact* 读盘兜底、写盘擦除）
// v59: simpleCombatAttackIntervalMs 默认 46→123；读盘迁旧默认 50/46
constexpr int32_t kImpactImpulseDirDefault = 1;
constexpr uint32_t kImpactImpulseVxDefault = 400u;
constexpr uint32_t kImpactImpulseVyDefault = 200u;
constexpr uint32_t kImpactImpulseVxMax = 5000u;
constexpr uint32_t kImpactImpulseVyMax = 5000u;
// P0 近距 hop：有符号 Δx（px）；验收档 80/120/160
constexpr int32_t kImpactHopDeltaXDefault = 120;
constexpr int32_t kImpactHopDeltaXMin = -400;
constexpr int32_t kImpactHopDeltaXMax = 400;
constexpr uint32_t kFrameLockFpsDefault = 120u;
constexpr uint32_t kFrameLockFpsMin = 15u;
// 软顶：仅防离谱输入；120/240/360/480/640/720 只是 UI 预设，不是业务上限。
constexpr uint32_t kFrameLockFpsMax = 10000u;
// flyMode: 0=Impact·NockBack  1=Impact·SetImpactNext（fill+Doing 瞬移飞已禁用）
constexpr uint32_t kFlyModeImpactNockBack = 0u;
constexpr uint32_t kFlyModeImpactSetNext = 1u;
constexpr uint32_t kFlyModeDefault = kFlyModeImpactNockBack;
// 兼容旧名（语义已变：不再是点击/跟随）
constexpr uint32_t kFlyModeClick = kFlyModeImpactNockBack;
constexpr uint32_t kFlyModeFollow = kFlyModeImpactSetNext;
// 每一飞自冷却（两条 Impact 路线共用）；默认 400；下限 1 仅供压测跟手
constexpr uint32_t kFlyHopCdDefaultMs = 120u;
// 过低 CD 会 STW+Impact 双排队打满 Unity 主线程（卡顿/MainPump timeout）。
constexpr uint32_t kFlyHopCdMinMs = 40u;
constexpr uint32_t kFlyHopCdMaxMs = 2000u;
// hangup hour mask: bit0=00:00 .. bit23=23:00 (local time).
constexpr uint32_t kHangupScheduleMaskAll = 0x00FFFFFFu;
constexpr uint32_t kWatchdogNoExpSecDefault = 180u;
constexpr uint32_t kWatchdogNoExpSecMin = 30u;
constexpr uint32_t kWatchdogNoExpSecMax = 3600u;
// 冷启未进图最坏约 4×N+确认：主门 2N（进程起后）+次门 N+未进图计时 N+确认。
// 进图后经验/状态停滞门槛为 N（+确认）。踢线武装后立刻重拉。
constexpr uint32_t kWatchdogCooldownSecDefault = 20u;
constexpr uint32_t kWatchdogCooldownSecMin = 20u;
constexpr uint32_t kWatchdogCooldownSecMax = 3600u;
constexpr size_t kPayloadWorldNameCap = 64;
// 默认分区：雪吉拉（_Center1）
constexpr int32_t kDefaultWorldId = 1;
constexpr const char* kDefaultWorldName = "雪吉拉";
constexpr uint32_t kMultiSkillGapDefaultMs = 120u;
constexpr uint32_t kMultiSkillGapMinMs = 1u;
constexpr uint32_t kMultiSkillGapMaxMs = 500u;
// 默认 123：面板「间隔」出厂值（约 8.1 刀/秒）。下限 1（用户自选；hold 地板同步）。
constexpr uint32_t kSimpleCombatAttackIntervalDefaultMs = 123u;
constexpr uint32_t kSimpleCombatAttackIntervalMinMs = 1u;
constexpr uint32_t kSimpleCombatAttackIntervalMaxMs = 10000u;
// 读盘迁移：旧默认 50 / 46 → 123；显式调过其它值保留。
constexpr uint32_t kSimpleCombatAttackIntervalLegacyDefaultMs = 50u;
constexpr uint32_t kSimpleCombatAttackIntervalLegacyDefaultMs46 = 46u;
// 出刀按键 hold 时长（调试 TAB）：OnFuncKey Down→Up 的间隔。
// 实际 hold = min(此值, 面板间隔)，hold ≥ 间隔会让 pendingUp 把下一刀锁死。
// 注意：攻击加速开启时走 Down+Up 同泵的 pulse 路径（hold=0），此值不参与。
constexpr uint32_t kAttackHoldDefaultMs = 5u;
constexpr uint32_t kAttackHoldMinMs = 1u;
constexpr uint32_t kAttackHoldMaxMs = 100u;
// 简易战斗 worker 心跳（状态机 Tick）；越短出刀机会越多，CPU/主线程更忙。
constexpr uint32_t kSimpleCombatTickDefaultMs = 16u;
constexpr uint32_t kSimpleCombatTickMinMs = 1u;
constexpr uint32_t kSimpleCombatTickMaxMs = 100u;
// 打怪开时 mob_scan 刷新周期；越小越快看见新怪/尸体，CPU 更高。闲置仍用 worker 内固定 360ms。
// 默认 20：对齐 Sleep 地板与抢怪体验；旧默认 50 读盘时迁到 20（显式其它值保留）。
constexpr uint32_t kMobScanIntervalDefaultMs = 20u;
constexpr uint32_t kMobScanIntervalLegacyDefaultMs = 50u;
constexpr uint32_t kMobScanIntervalMinMs = 1u;
constexpr uint32_t kMobScanIntervalMaxMs = 500u;
// 与全局 Min 对齐；加速不另抬间隔。
constexpr uint32_t kAttackAccelIntervalFloorMs = 1u;
// 群怪优先：落盘仍用 clusterWeight；0=关，非 0=开（旧 1–100 权重一律视为开）。
constexpr uint32_t kClusterWeightDefault = 0u;
constexpr uint32_t kClusterWeightMax = 100u;
// 同帧连打探针已关停（实测不增伤）；字段保留仅为清掉旧 ini 的 2/3。
constexpr uint32_t kAttackSameFrameBurstDefault = 1u;
constexpr uint32_t kAttackSameFrameBurstMin = 1u;
constexpr uint32_t kAttackSameFrameBurstMax = 1u;  // 上限=1：强制单刀
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
// 出刀站距（人↔怪心水平目标距离）；默认压到下限，命中带≈站距×1.55。
constexpr uint32_t kCombatTeleportStandOffDefault = 12u;
constexpr uint32_t kCombatTeleportStandOffMin = 12u;
// 旧默认 25（再早 BIN 试过 40）；显式调过非 25 的保留。
constexpr uint32_t kCombatTeleportStandOffLegacyDefault = 25u;
// UI / simple_combat 上界；过大无意义
constexpr uint32_t kCombatTeleportStandOffMax = 200u;
// 贴怪/LiveStep 共用面板冷却；默认 200；下限 5（大 hop 另有 80/120 地板）。
constexpr uint32_t kCombatTeleportCooldownDefaultMs = 200u;
constexpr uint32_t kCombatTeleportCooldownMinMs = 5u;
constexpr uint32_t kCombatTeleportCooldownMaxMs = 8000u;
// 跨层 fill 后额外互斥（与贴怪节流独立）；0=关。首页面板可调。
constexpr uint32_t kCombatCrossLayerFillGateDefaultMs = 280u;
constexpr uint32_t kCombatCrossLayerFillGateMinMs = 0u;
constexpr uint32_t kCombatCrossLayerFillGateMaxMs = 2000u;
// 10 秒滚动窗口内 fill 位移预算（px）；0=关。踢线主因经证实是 land_miss（写入位置被引擎
// 打回票），位移限速属粗放兜底且实测卡掉四成时间（a7dc3e），故默认关，留旋钮备用。
constexpr uint32_t kCombatFillBudgetPxDefault = 0u;
constexpr uint32_t kCombatFillBudgetPxMin = 0u;
constexpr uint32_t kCombatFillBudgetPxMax = 40000u;
// 单次贴怪 hop 上限（px）；更远分段贴近。调试 TAB 可调。
// 默认 550（=上限）：盖住常见中台→顶台垂直落差。
constexpr uint32_t kCombatTeleportMaxHopDefault = 550u;
constexpr uint32_t kCombatTeleportMaxHopMin = 350u;
constexpr uint32_t kCombatTeleportMaxHopMax = 550u;
// 旧默认迁移：400→520→550；显式调过其它值保留。
constexpr uint32_t kCombatTeleportMaxHopLegacyDefault = 400u;
constexpr uint32_t kCombatTeleportMaxHopPrevDefault = 520u;
// 加速秒杀早切（lastHitted 确认后切怪；0 maxHp = 关此道；默认关）
constexpr uint32_t kCombatOneshotMaxHpDefault = 0u;
// 勾选启用 /「均衡」档回填的表血上限（非落盘缺省）
constexpr uint32_t kCombatOneshotMaxHpWhenOn = 120u;
constexpr uint32_t kCombatOneshotMaxHpMin = 0u;
constexpr uint32_t kCombatOneshotMaxHpMax = 50000u;
constexpr uint32_t kCombatOneshotMinBumpsDefault = 1u;
constexpr uint32_t kCombatOneshotMinBumpsMin = 0u;  // 0=射后不管（不等 lastHitted）
constexpr uint32_t kCombatOneshotMinBumpsMax = 20u;
constexpr uint32_t kCombatOneshotMinFiresDefault = 3u;
constexpr uint32_t kCombatOneshotMinFiresMin = 1u;
constexpr uint32_t kCombatOneshotMinFiresMax = 30u;
constexpr uint32_t kCombatOneshotMinLagMsDefault = 40u;
constexpr uint32_t kCombatOneshotMinLagMsMin = 0u;
constexpr uint32_t kCombatOneshotMinLagMsMax = 2000u;
// 射后不管：弃刀后禁止 fill 的最短间隔（防一刀一传送 GC）
// d8d80e：默认 280 时每次早切换怪卡 ~280ms，面板瞬移间隔拉到 5 仍体感慢。
// 默认改 0；需要防 GC 时用户再在「早切禁瞬移」自行加回。
// 旧默认 280：读盘时迁到 0（与 maxHop legacy 同策略；显式改成其它值保留）。
constexpr uint32_t kCombatOneshotFoxFillGapDefaultMs = 0u;
constexpr uint32_t kCombatOneshotFoxFillGapLegacyDefaultMs = 280u;
constexpr uint32_t kCombatOneshotFoxFillGapMinMs = 0u;
constexpr uint32_t kCombatOneshotFoxFillGapMaxMs = 2000u;
// 泵背压拥堵阈值：主线程泵排队 job 数达到该值即视为拥堵，出刀/瞬移让路。
// 0=关闭背压；上限须等于泵队列容量 kQueueCap（main_thread_pump.cpp，当前 8）。
constexpr uint32_t kPumpCongestionDefault = 6u;
constexpr uint32_t kPumpCongestionMin = 0u;
constexpr uint32_t kPumpCongestionMax = 8u;
// 每 tick Drain 最多跑几个排队 job；须 ∈[1, kQueueCap]。默认 8=抽干整队。
constexpr uint32_t kPumpDrainBudgetDefault = 8u;
constexpr uint32_t kPumpDrainBudgetMin = 1u;
constexpr uint32_t kPumpDrainBudgetMax = 8u;

struct PayloadControl {
    uint32_t magic = kPayloadControlMagic;
    uint32_t version = kPayloadControlVersion;
    uint32_t invuln = 1;  // 默认开
    // v27/v38: 攻击加速（开=跳过动作等待；间隔默认见 kSimpleCombatAttackIntervalDefaultMs）
    uint32_t attackAccel = 0;
    // 实验：砍动作层 layer+0x14 倒计时（默认关；不改变 attackAccel 语义）
    uint32_t attackAccelCutLayer = 0;
    // 实验：跳过 PrepareActionLayer（默认关；LocalUser 虚表；实验 TAB）
    uint32_t attackAccelSkipPrepare = 0;
    // 实验：SecondaryStat.nBooster_ 攻速槽（默认关）。与 attackAccel 完全独立，
    // 可单开做「不碰忙锁」对照 —— attackAccel 会顺带下发 animBusyOverride/immediateUp，
    // 若挂在同一开关上就没法把 booster 的净效果量出来。
    uint32_t attackAccelBooster = 0;
    // 已关停：读写一律压回 1，清掉实验期落盘的 2/3。
    uint32_t attackSameFrameBurst = kAttackSameFrameBurstDefault;
    // v23: 飞行武装（面板勾选 / F6）；策略由 flyMode 决定；不钉台。
    // 开关为会话态（state/fly_armed），不写入 user.ini；重启 launcher/注入后归零。
    uint32_t fly = 0;
    // v24: 0=Impact NockBack 1=Impact SetImpactNext（瞬移飞已禁用；旧点击/跟随语义废）
    uint32_t flyMode = kFlyModeDefault;
    // v25: 每一飞间隔 ms（两条 Impact 路线共用自冷却）
    uint32_t flyHopCdMs = kFlyHopCdDefaultMs;
    uint32_t autoEnter = 1;   // 默认开：分区→最少人频道→选角（图例：雪吉拉 / 槽1）
    uint32_t charSlot = 1;    // 1-based 角色槽
    int32_t worldId = kDefaultWorldId;  // 默认雪吉拉（worldId=1）
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
    // v44: 多发可选直发（技能 SendSkillUseRequest + 普攻 Create50；默认关；失败回退）
    uint32_t multiSkillSendUseRequest = 0;
    uint32_t simpleCombat = 0;            // ????????
    uint32_t simpleCombatSmartInterval = 0;  // ???????????
    uint32_t simpleCombatAttackIntervalMs = kSimpleCombatAttackIntervalDefaultMs;
    // v39: 打怪状态机 Tick 间隔（首页「TICK值」）；默认 16，下限 5
    uint32_t simpleCombatTickMs = kSimpleCombatTickDefaultMs;
    // v51: 打怪开时 MobPool 扫描周期（首页「怪物读取速度」）；默认 20
    uint32_t mobScanIntervalMs = kMobScanIntervalDefaultMs;
    // v40: 出刀按键 hold（调试 TAB）；默认 5，实际取 min(此值, 间隔)
    uint32_t simpleCombatAttackHoldMs = kAttackHoldDefaultMs;
    uint32_t clusterWeight = kClusterWeightDefault;  // 0=最近优先；非0=群怪优先
    // fill+Doing 已废：强制关。位移统一 Impact（F5）/ 拟人。
    uint32_t simpleCombatTeleport = 0;
    // 空中贴怪（ini: simpleCombatAirApproach；旧键 ImpactApproach 读盘兜底）。
    uint32_t simpleCombatImpactApproach = 1;
    // 拟人位移：同层走路贴近后 A 键出刀；与空中贴怪互斥（空中开时恒 0）。
    uint32_t simpleCombatHumanWalk = 0;
    uint32_t simpleCombatTeleportMinDx = kCombatTeleportMinDxDefault;
    uint32_t simpleCombatTeleportStandOff = kCombatTeleportStandOffDefault;
    uint32_t simpleCombatTeleportCooldownMs = kCombatTeleportCooldownDefaultMs;
    // v46: 跨层 fill 后额外门控（ms）；0=关；切段中间跳不武装
    uint32_t simpleCombatCrossLayerFillGateMs = kCombatCrossLayerFillGateDefaultMs;
    // v47: 10s 滚动窗口 fill 位移预算（px）；0=关
    uint32_t simpleCombatFillBudgetPx = kCombatFillBudgetPxDefault;
    // v45: 单次贴怪 hop 上限（px）；调试 TAB
    uint32_t simpleCombatTeleportMaxHop = kCombatTeleportMaxHopDefault;
    // v26: 锁怪后同层微瞬移贴位（近似枫星 LiveStep）；默认关；依赖贴怪瞬移
    uint32_t simpleCombatLiveStep = 0;
    // v42: 加速秒杀早切（表 maxHP + lastHitted）；maxHp=0 关此道
    uint32_t simpleCombatOneshotMaxHp = kCombatOneshotMaxHpDefault;
    uint32_t simpleCombatOneshotMinBumps = kCombatOneshotMinBumpsDefault;
    uint32_t simpleCombatOneshotMinFires = kCombatOneshotMinFiresDefault;
    uint32_t simpleCombatOneshotMinLagMs = kCombatOneshotMinLagMsDefault;
    // v43: 射后不管后最短 fill 间隔；0=不闸（易 GC）
    uint32_t simpleCombatOneshotFoxFillGapMs = kCombatOneshotFoxFillGapDefaultMs;
    // v44: 泵背压拥堵阈值（排队 job 数）；0=关背压
    uint32_t pumpCongestionThreshold = kPumpCongestionDefault;
    // v49: 每 tick Drain 预算（1–8）；默认 8=抽干
    uint32_t pumpDrainBudget = kPumpDrainBudgetDefault;
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
    // v57: 位移试推 A/B（面板 bump seq）；ini=moveProbe*（旧 impact* 读盘兜底）
    uint32_t impactNockBackTestSeq = 0;
    uint32_t impactSetNextTestSeq = 0;
    int32_t impactImpulseDir = kImpactImpulseDirDefault;  // ±1；其它值=跟朝向
    uint32_t impactImpulseVx = kImpactImpulseVxDefault;
    uint32_t impactImpulseVy = kImpactImpulseVyDefault;
    // v58: 短推试推；ini=moveHop*（旧 impactHop* 读盘兜底）；force=1 旁路无敌
    uint32_t impactHopTestSeq = 0;
    int32_t impactHopDeltaX = kImpactHopDeltaXDefault;
    uint32_t impactHopForce = 0;
    // 调试采证：inline hook MovePath.Flush，dump C→S UserMove 的 MoveElem。默认关，
    // 仅测试时经「调试」TAB 开启（本仓禁止常驻 inline hook）。见 movepath_flush_probe。
    uint32_t movepathFlushProbe = 0;
    // v61: 软重连试连（首页单勾选；同时武装 Galaxy token 只读采证）。
    // 亦可用 soft_login_probe.on / SOFT_LOGIN_PROBE=1；旧 galaxy_token_probe.on 仍可单独采证。
    uint32_t galaxyTokenProbe = 0;  // 与 softLoginProbe 同步写入；保留字段兼容旧 ini
    uint32_t softLoginProbe = 0;
    // v20: 遇人策略 UX（UserPool + channel_hop，非 Reload）
    uint32_t autoRelogin = 0;             // 检测同图玩家
    uint32_t autoReloginStopCombat = 1;   // 先停手
    uint32_t autoReloginReconnect = 1;    // 一直有人就换频
    // v60: GM/隐身升级（Admin·Manager 或客户端隐身 → 立刻停手/换频 + 强制 Alarm；默认开）
    uint32_t autoReloginGmEscalate = 1;
    // v46: 隐藏同图其他玩家（UserPool 远程 → AvatarRoot.SetActive；默认关）
    uint32_t hideOtherPlayers = 0;
    // v47: 引擎帧率锁（vSync=0 + Application.targetFrameRate；非显示器硬件刷新率）
    uint32_t frameLock = 0;
    uint32_t frameLockFps = kFrameLockFpsDefault;
    // v48: 普攻必出终极一击（Final Attack prop→100；默认关）
    uint32_t finalAttackForce = 0;
    // v54: 已学技能按满级（改 SkillRecord/Ex 等级；默认关）
    uint32_t skillMaxLevel = 0;
    // ????????? DragManager.CanPerformAction ??????????
    uint32_t dropAlertBypass = 0;  // 默认关：开着会抑制客户端警戒
    // 野外可开拍卖：数据面强制 MapDataInfo.IsTown=1（仅客户端；默认关）。
    // 服端可能断线；与挂机「守护模式」叠加会干净重拉——故默认关。
    uint32_t auctionTownBypass = 0;
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

inline uint32_t ClampAttackHoldMs(uint32_t ms) {
    if (ms < kAttackHoldMinMs) return kAttackHoldMinMs;
    if (ms > kAttackHoldMaxMs) return kAttackHoldMaxMs;
    return ms;
}

inline uint32_t ClampAttackSameFrameBurst(uint32_t n) {
    if (n < kAttackSameFrameBurstMin) return kAttackSameFrameBurstMin;
    if (n > kAttackSameFrameBurstMax) return kAttackSameFrameBurstMax;
    return n;
}

inline uint32_t ClampSimpleCombatTickMs(uint32_t ms) {
    if (ms < kSimpleCombatTickMinMs) return kSimpleCombatTickMinMs;
    if (ms > kSimpleCombatTickMaxMs) return kSimpleCombatTickMaxMs;
    return ms;
}

inline uint32_t ClampMobScanIntervalMs(uint32_t ms) {
    if (ms < kMobScanIntervalMinMs) return kMobScanIntervalMinMs;
    if (ms > kMobScanIntervalMaxMs) return kMobScanIntervalMaxMs;
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
inline uint32_t ClampCombatCrossLayerFillGateMs(uint32_t v) {
    if (v > kCombatCrossLayerFillGateMaxMs) return kCombatCrossLayerFillGateMaxMs;
    return v;  // 0 = 关闭门控
}

inline uint32_t ClampCombatFillBudgetPx(uint32_t v) {
    if (v > kCombatFillBudgetPxMax) return kCombatFillBudgetPxMax;
    return v;  // 0 = 关闭预算
}

inline uint32_t ClampCombatTeleportMaxHop(uint32_t v) {
    if (v < kCombatTeleportMaxHopMin) return kCombatTeleportMaxHopMin;
    if (v > kCombatTeleportMaxHopMax) return kCombatTeleportMaxHopMax;
    return v;
}

inline uint32_t ClampCombatOneshotMaxHp(uint32_t v) {
    if (v > kCombatOneshotMaxHpMax) return kCombatOneshotMaxHpMax;
    return v;
}

inline uint32_t ClampCombatOneshotMinBumps(uint32_t v) {
    if (v < kCombatOneshotMinBumpsMin) return kCombatOneshotMinBumpsMin;
    if (v > kCombatOneshotMinBumpsMax) return kCombatOneshotMinBumpsMax;
    return v;
}

inline uint32_t ClampCombatOneshotMinFires(uint32_t v) {
    if (v < kCombatOneshotMinFiresMin) return kCombatOneshotMinFiresMin;
    if (v > kCombatOneshotMinFiresMax) return kCombatOneshotMinFiresMax;
    return v;
}

inline uint32_t ClampCombatOneshotMinLagMs(uint32_t v) {
    if (v > kCombatOneshotMinLagMsMax) return kCombatOneshotMinLagMsMax;
    return v;
}

inline uint32_t ClampCombatOneshotFoxFillGapMs(uint32_t v) {
    if (v > kCombatOneshotFoxFillGapMaxMs) return kCombatOneshotFoxFillGapMaxMs;
    return v;
}

inline uint32_t ClampPumpCongestion(uint32_t v) {
    if (v > kPumpCongestionMax) return kPumpCongestionMax;
    return v;
}

inline uint32_t ClampPumpDrainBudget(uint32_t v) {
    if (v < kPumpDrainBudgetMin) return kPumpDrainBudgetMin;
    if (v > kPumpDrainBudgetMax) return kPumpDrainBudgetMax;
    return v;
}

inline uint32_t ClampFlyMode(uint32_t v) {
    if (v > kFlyModeImpactSetNext) return kFlyModeDefault;
    return v;
}

inline uint32_t ClampFrameLockFps(uint32_t v) {
    if (v < kFrameLockFpsMin) return kFrameLockFpsMin;
    if (v > kFrameLockFpsMax) return kFrameLockFpsMax;
    return v;
}

inline uint32_t ClampFlyHopCdMs(uint32_t v) {
    if (v < kFlyHopCdMinMs) return kFlyHopCdMinMs;
    if (v > kFlyHopCdMaxMs) return kFlyHopCdMaxMs;
    return v;
}

inline uint32_t ClampImpactImpulseSpeed(uint32_t v) {
    if (v > kImpactImpulseVxMax) return kImpactImpulseVxMax;
    return v;
}

inline int32_t ClampImpactImpulseDir(int32_t d) {
    if (d == 1 || d == -1) return d;
    return 0;  // 0=跟朝向
}

inline int32_t ClampImpactHopDeltaX(int32_t dx) {
    if (dx < kImpactHopDeltaXMin) return kImpactHopDeltaXMin;
    if (dx > kImpactHopDeltaXMax) return kImpactHopDeltaXMax;
    return dx;
}

void PayloadControlSetDefaults(PayloadControl& out);
bool ReadPayloadControl(const char* binDir, PayloadControl& out);
bool WritePayloadControl(const char* binDir, const PayloadControl& control);

// 飞行开关会话态：清零（launcher 启动 / DLL Init）。不改 flyMode / hopCd。
void ClearFlyArmedSession(const char* binDir);

}  // namespace xcat
