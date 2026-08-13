#pragma once

#include <cstdint>
#include <cstddef>

namespace xcat {

// TWMS ???????launcher <-> payload??? user.ini [core]?
constexpr uint32_t kPayloadControlMagic = 0x58435443u;  // 'XCTC'
constexpr uint32_t kPayloadControlVersion = 1u;
constexpr uint32_t kPayloadControlCoreIniVersion = 82u;
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
// v64: simpleCombatStandOffCustom/X/Y — F5 空中贴怪自定义站距（远程职业）
// v65: 攻击加速用户入口暂关（面板置灰；读盘/下发强制 attackAccel=0）
// v66: simpleCombatGroundSpoof — 站立伪装（出刀瞬间种 CurFh；默认开）
// v69: simpleCombatAntiHug — 防贴脸退避（LiveStep）；复用自定义站距 X/Y 作躲避半径；默认关
// v70: pointBlankShoot 拆除 — 「不挥弓」已证伪（改客户端分支会打断整条伤害链），
//      改由 simpleCombatAntiHug 从物理上避开挥弓框；字段与 ini key 一并清掉。
//      结论见 Dumps/runtime/ARCHER_SHOOT_VS_BONK_GATE_20260809.md
// v71: attackAccelPartyBoosterValue — PartyBooster TempStats[4] 加数滑条（默认 -8，范围 [-8,0]）
// v72: attackAccelBreakDegreeFloor — 破 B 系 degree 下限（改 GA 种子；与 Party 独立）
// v73: attackAccelBreakDegreeFloorLo — 破限目标 lo 滑条（默认 -10，范围 [-10,0]）
// v74: attackAccelClearBusy — 实验·清 ActionBusy 忙锁（与首页 attackAccel 入口独立）
// v75: attackAccelClearBusyMinIntervalMs — 清忙锁开启时出刀间隔地板（默认 410）
// v76: restMpAccel — 实验·坐下/椅子回蓝累加器加速（写 WM+0x17C/+0x180；默认关）
// v77: restMpAccelIntervalMs — 写满间隔（默认 2500；过密会踢；BIN 已证真回蓝）
// v78: meleeVeto — 「近战不挥拳」：让普攻那一发 TryDoingMeleeAttack 判负，落到兜底射击。
//      勾上后先测量再决定：观测到近战体内自己转调射击（nest>0）就拒绝拦截 —— 弓正是这样，
//      拦了会把整条伤害链打断（见 ARCHER_SHOOT_VS_BONK_GATE_20260809.md §0″/§0‴）。默认关。
// v79: curFhGateBypass — 实验·地面门旁路（改 GA CurFh 判空跳转；≠ 站立伪装）
// v80: infiniteStars — 实验·无限飞镖：自动维持 4121006 无形镖 + 客户端冻 207xxxx 扣数；默认关
// v81: kInfiniteStarsUserEnabled=false — 实验 TAB 不画入口；不启 worker / 不挂钩；代码保留
constexpr int32_t kImpactImpulseDirDefault = 1;
constexpr uint32_t kImpactImpulseVxDefault = 400u;
constexpr uint32_t kImpactImpulseVyDefault = 200u;
constexpr uint32_t kImpactImpulseVxMax = 5000u;
constexpr uint32_t kImpactImpulseVyMax = 5000u;
// P0 近距 hop：有符号 Δx（px）；验收档 80/120/160
constexpr int32_t kImpactHopDeltaXDefault = 120;
constexpr int32_t kImpactHopDeltaXMin = -400;
constexpr int32_t kImpactHopDeltaXMax = 400;
constexpr uint32_t kFrameLockFpsDefault = 1000u;
constexpr uint32_t kFrameLockFpsMin = 15u;
// 软顶：仅防离谱输入；120/240/360/480/640/720/860/1000 只是 UI 预设，不是业务上限。
constexpr uint32_t kFrameLockFpsMax = 10000u;
// flyMode: 0=Impact·NockBack  1=Impact·SetImpactNext（fill+Doing 瞬移飞已禁用）
constexpr uint32_t kFlyModeImpactNockBack = 0u;
constexpr uint32_t kFlyModeImpactSetNext = 1u;
constexpr uint32_t kFlyModeDefault = kFlyModeImpactNockBack;
// 兼容旧名（语义已变：不再是点击/跟随）
constexpr uint32_t kFlyModeClick = kFlyModeImpactNockBack;
constexpr uint32_t kFlyModeFollow = kFlyModeImpactSetNext;
// 目标点刷新间隔（历史名 HopCd，语义见 fly.h）。
//
// 默认从 120 降到 40：120ms 下目标本身每秒才更新 8 次，光标扫得快时目标点自己就是陈的。
// 40 是用户实测手感的落点（BIN a0ab58 手动调到 40 后明显改善）。
//
// ★ **下限 5 只是不挡路，不代表真能跑到 5ms**——真实地板由系统时钟分辨率决定，实测 15.625ms。
//
// 证据（BIN 5c3950 + a0ab58，104 个 `Fly heli … since=` 样本）：取值只有
// 0 / 15·16 / 31·32 / 47 / 62·63 / 78·79，**全是 15.625 的整数倍，没有一个中间值**。
// 刷新闸 `AimReady()` 比的是 `GetTickCount()` 差值，它每 15.625ms 才跳一格；worker 的
// `Sleep(8)` 同理实际睡 ~15.6ms。所以 5 / 10 / 15 / 16 走的是完全相同的代码路径。
//
// 保留 5 这个下限是为了**不写死一个会过期的假设**：若哪天进程时钟被提到 1ms
// （Unity 或别的进程调 timeBeginPeriod），这个旋钮就自动开始咬合，不必再改代码。
// 想真正跑到 5ms 只能自己 timeBeginPeriod(1)，那是**进程级副作用**（影响游戏自身计时与功耗），
// 不该由本功能替用户决定 —— 要做也得是显式开关。
//
// 主泵代价：BIN 5c3950 在 16ms 下跑满一轮，`aim pump timeout` / `ScreenToWorld fail` /
// 拥堵均为 **0**，说明当前档位离主泵瓶颈还远，卡的确实是时钟不是负载。
//
// 另注：加了速度前馈之后（见 heli::Setpoint::leadVx），刷新间隔对跟手的影响已降一个量级——
// 两次刷新之间角色会按估出的光标速度继续走，不再是钉住旧点干等。
// 默认 120→40→**16**：16 正好压在下面那条 15.625ms 的时钟地板上，也就是「这套时钟能给到的
// 最密刷新」。再往下不会更跟手（同一条路径），往上则白白让出跟手度，所以 16 就是最优默认。
constexpr uint32_t kFlyHopCdDefaultMs = 16u;
constexpr uint32_t kFlyHopCdMinMs = 5u;
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
// 多发普攻：冷启动地板（尚无 degree=6 基准样本时）。
// 有样本后 = EstimateDamageDelayScaleMs(学到的 base@deg6)，随攻速档即时缩放。
constexpr uint32_t kMultiSkillNativeNaFloorBootstrapMs = 600u;
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
// 空中贴怪的飞行速度倍率，百分比。100 = 基准 1.0X（Cruise 620 / Rtb 660 / Station 480 / Hold 360）。
// 只缩放旋翼各档的「意图」上限；位置包线、撞墙预刹、深度缴械这些**绝对**阈值不跟随。
//
// ⚠️ 落速闸 kMaxFallVy **必须**跟随（已改成函数 FallGateVy）。它不是绝对阈值而是
//    「想快降 vs 失控」的判别式，钉死会在高倍率下反号成极限环（BIN 2d6176 抖动事故）。
//
// 上限 1000（Cruise 6200 px/s）。这个数由作动器上限 kMaxCmdV=8000 反解：
// 可救性要求 cap ≤ 8000−230−300−60 = 7410，而 10X 下最快的 Rtb 是 660×10 = 6600 < 7410。
// heli_rotor 里有同源的 kSpeedScaleMax 与 static_assert，两处不会再走散。
//
// 曾短暂设成 175，是因为当时还要求「满速反向一拍完成」(cap ≤ C/2)；那条是性能优化不是
// 安全需求，已由 heli_rotor 的可达集钳位取代。
//
// ⚠️ 两条实机约束，调高前先想清楚：
//   · 作动器实测到 cmd=3231/v=3100（5X，零饱和），8000 是从那儿起的 2.1 倍外推、未验。
//   · 撞墙预刹按 room/0.2s 限速 ⇒ **高倍率在小图上拿不满**：离墙 800px 时最快只有
//     4000 px/s，与倍率无关。10X 的收益随地图尺寸递减。
//   · 服务端对 6200 px/s（约合法步行速 50 倍）的容忍度从未测过。加档必须配合看日志。
// F5 滑翔 / 自动赶路默认倍率：面板顶格 1000%（满火力死拍档）。可调低；旧 ini 有值则读盘为准。
//
// ⚠️ 两条实机约束，调高前先想清楚：
//   · 作动器实测到 cmd=3231/v=3100（5X，零饱和），8000 是从那儿起的 2.1 倍外推、未验。
//   · 撞墙预刹按 room/0.2s 限速 ⇒ **高倍率在小图上拿不满**：离墙 800px 时最快只有
//     4000 px/s，与倍率无关。10X 的收益随地图尺寸递减。
//   · 服务端对 6200 px/s（约合法步行速 50 倍）的容忍度从未测过。加档必须配合看日志。
constexpr uint32_t kHeliSpeedPctDefault = 1000u;
constexpr uint32_t kHeliSpeedPctMin = 25u;
constexpr uint32_t kHeliSpeedPctMax = 1000u;
// F6 手动飞默认倍率 300%：换旋翼前开环等效约 1600 px/s ≈ 2.6X，
// 直接给 1.0X（Cruise 620）会比旧版慢一大截；3X 开箱手感更接近旧手动飞。
constexpr uint32_t kFlySpeedPctDefault = 300u;
constexpr uint32_t kMobScanIntervalDefaultMs = 20u;
constexpr uint32_t kMobScanIntervalLegacyDefaultMs = 50u;
constexpr uint32_t kMobScanIntervalMinMs = 1u;
constexpr uint32_t kMobScanIntervalMaxMs = 500u;
// 与全局 Min 对齐；加速不另抬间隔。
constexpr uint32_t kAttackAccelIntervalFloorMs = 1u;
// 清忙锁专用出刀间隔地板：拆掉 ActionBusy 后防止贴着面板 1/70ms 狂射。
// 只在 Apply→SetAttackIntervalMs 时抬有效间隔，不回写面板「间隔」落盘值。
constexpr uint32_t kAttackAccelClearBusyMinIntervalDefaultMs = 410u;
constexpr uint32_t kAttackAccelClearBusyMinIntervalMinMs = 50u;
constexpr uint32_t kAttackAccelClearBusyMinIntervalMaxMs = 1000u;

// PartyBooster（TempStats[4].Value）加数：越负越快，引擎 degree 夹 [2,10]；-8 已够顶格。
constexpr int32_t kAttackAccelPartyBoosterValueDefault = -8;
constexpr int32_t kAttackAccelPartyBoosterValueMin = -8;
constexpr int32_t kAttackAccelPartyBoosterValueMax = 0;
// 破 degree 下限目标 lo（开破限时写入 GA 种子）；越负越快；deg=-10 → 延迟×0。
constexpr int32_t kAttackAccelBreakDegreeFloorLoDefault = -10;
constexpr int32_t kAttackAccelBreakDegreeFloorLoMin = -10;
constexpr int32_t kAttackAccelBreakDegreeFloorLoMax = 0;
// false：首页「攻击加速」置灰不可选；读盘/Normalize/Apply 强制关闭，防旧 ini 误开。
constexpr bool kAttackAccelUserEnabled = false;
// false：实验 TAB「攻速槽 nBooster_」置灰；读盘/落盘/Apply 强制关（写 -8 有指纹风险）。
constexpr bool kAttackAccelBoosterUserEnabled = false;
// false：实验 TAB「技能满级」置灰；读盘/落盘/Apply 强制关，不启 worker（服端伤不认客户端等级）。
constexpr bool kSkillMaxLevelUserEnabled = false;
// false：实验 TAB「普攻必出终极一击」置灰；读盘/落盘/Apply 强制关，不启 FaForce worker
//（曾 off-pump GetSkill → GA+0x3a0bde TLS AV；功能已弃用，代码保留防日后重开）。
constexpr bool kFinalAttackForceUserEnabled = false;
// false：实验 TAB 不画「无限飞镖」入口；读盘/落盘/Apply 强制关，不启 worker、不挂钩。
constexpr bool kInfiniteStarsUserEnabled = false;
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
// rest_mp_accel：两次写满累加器间隔。自然约 5~10s；默认 2.5s；过小会踢。
constexpr uint32_t kRestMpAccelIntervalDefaultMs = 2500u;
constexpr uint32_t kRestMpAccelIntervalMinMs = 50u;
constexpr uint32_t kRestMpAccelIntervalMaxMs = 10000u;
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

// ── F5 空中贴怪：自定义站距（面板「自定义站距」勾选） ──────────────────
// 与上面那组 kCombatTeleportStandOff* **无关**：那组是瞬移/拟人落点用的，还会乘进
// InHitBand（×1.55）等地面判定；这组只喂直升机悬停点与它自己的闸门。
//
// 关：用内置近战最优值（simple_combat::kHeliStandOffPx / kHeliLiftPx），
//     那是 3,265 个观察窗实测出来的（见模块设计文档「站距」节）。
// 开：完全听用户的 —— 远程职业站在自己的射程上打，命中率由用户自己调。
//     ⚠️ 出刀闸与到位判据会**跟着放大**，否则站到 250px 会被 |dx|<=120 的硬闸一票否决。
constexpr uint32_t kCombatStandOffCustomDefault = 0u;
// X = 人↔怪心的水平目标距离（px，恒正；左右哪一侧由飞控自己选）。
// 自定义开箱默认 60/-4（与内置空中站位一致）；关勾选仍走同一组内置值。
constexpr uint32_t kCombatStandOffXDefault = 60u;
constexpr uint32_t kCombatStandOffXMin = 0u;
// 上界给到 900：够覆盖弓/弩/法师的屏内射程；再远怪也出屏了，选靶先饿死。
constexpr uint32_t kCombatStandOffXMax = 900u;
// Y = 相对怪心的垂直偏移（px，**带符号**；+Y 向上 ⇒ 正数=站在怪上方）。
// 用 IniGetI32 直存负号，别偏置编码：user.ini 是用户会直接改的文件。
constexpr int32_t kCombatStandOffYDefault = -4;
constexpr int32_t kCombatStandOffYMin = -600;
constexpr int32_t kCombatStandOffYMax = 600;
// 站立伪装默认开：滑翔时 CurFh 恒空，一切「必须站立」的技能（蝸牛術、部分职业主
// 攻技）全被引擎那道内联地面门拒掉，不开等于这些职业没法用 F5。种台只在同步派发
// 的两侧各一次、随即清回 null，物理与飞控都看不见。见 ground_spoof.h。
constexpr uint32_t kCombatGroundSpoofDefault = 1u;
// F5 空中贴怪「防抖」：到位后钉住站位点（仍 Station，不改 Hold）。关=立即回退旧跟点行为。
constexpr uint32_t kCombatAntiJitterDefault = 1u;
// 贴怪/LiveStep 共用面板冷却；默认 200；下限 5（大 hop 另有 80/120 地板）。
constexpr uint32_t kCombatTeleportCooldownDefaultMs = 200u;
constexpr uint32_t kCombatTeleportCooldownMinMs = 5u;
constexpr uint32_t kCombatTeleportCooldownMaxMs = 8000u;
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
    // 首页入口已关（kAttackAccelUserEnabled）；实验清忙锁见 attackAccelClearBusy。
    uint32_t attackAccel = 0;
    // 实验：周期写 LocalUser ActionBusy=-1（清引擎忙锁）；默认关。不绑首页 attackAccel。
    // 不是技能无 CD；仍受面板间隔 / 服端校验约束。
    uint32_t attackAccelClearBusy = 0;
    // 清忙锁开启时的出刀间隔地板（ms）；默认 200。不改写面板间隔落盘。
    uint32_t attackAccelClearBusyMinIntervalMs = kAttackAccelClearBusyMinIntervalDefaultMs;
    // 实验：砍动作层 layer+0x14 倒计时（默认关；不改变 attackAccel 语义）
    uint32_t attackAccelCutLayer = 0;
    // 实验：跳过 PrepareActionLayer（默认关；LocalUser 虚表；实验 TAB）
    uint32_t attackAccelSkipPrepare = 0;
    // 实验：SecondaryStat.nBooster_ 攻速槽（默认关）。与 attackAccel 完全独立。
    // 用户入口已关（kAttackAccelBoosterUserEnabled）；字段保留防旧 ini / 日后重开。
    uint32_t attackAccelBooster = 0;
    // 实验：A 系 nSpeed_@0x84=+40（GetActionSpeed → Prepare clamp 140）；默认关。
    uint32_t attackAccelActionSpeed = 0;
    // 实验：TempStats[4].Value(PartyBooster)（B 系 degree 加数）；默认关。
    uint32_t attackAccelPartyBooster = 0;
    // PartyBooster 写入值（越负越快；默认 -8；范围见 kAttackAccelPartyBoosterValue*）。
    int32_t attackAccelPartyBoosterValue = kAttackAccelPartyBoosterValueDefault;
    // 实验：破 CalcWeaponAttackSpeedTier 下限（写 GA .data 独占种子；默认关）。
    // 与 PartyBooster 完全独立：不开本项时 PB=-8 仍夹到 deg=2。
    uint32_t attackAccelBreakDegreeFloor = 0;
    // 破限目标 lo（默认 -10；范围见 kAttackAccelBreakDegreeFloorLo*）。
    int32_t attackAccelBreakDegreeFloorLo = kAttackAccelBreakDegreeFloorLoDefault;
    // 已关停：读写一律压回 1，清掉实验期落盘的 2/3。
    uint32_t attackSameFrameBurst = kAttackSameFrameBurstDefault;
    // v23: 飞行武装（面板勾选 / F6）；策略由 flyMode 决定；不钉台。
    // 开关为会话态（state/fly_armed），不写入 user.ini；重启 launcher/注入后归零。
    uint32_t fly = 0;
    // v24: 0=Impact NockBack 1=Impact SetImpactNext（瞬移飞已禁用；旧点击/跟随语义废）
    uint32_t flyMode = kFlyModeDefault;
    // v25: 每一飞间隔 ms（两条 Impact 路线共用自冷却）
    // 换旋翼后语义变为「目标点刷新间隔」：冲量节奏由 A 层自控，这里只管多久重算一次 STW。
    uint32_t flyHopCdMs = kFlyHopCdDefaultMs;
    // F6 手动飞的速度倍率（%）。与 simpleCombatFlySpeedPct **分开存**：旧 F6 开环等效约
    // 1600 px/s，而 Cruise 基准只有 620，手动飞与自动打怪共用一个旋钮必有一方别扭。
    uint32_t flySpeedPct = kFlySpeedPctDefault;
    uint32_t autoEnter = 1;   // 默认开：分区→最少人频道→选角（图例：雪吉拉 / 槽1）
    uint32_t charSlot = 1;    // 1-based 角色槽
    int32_t worldId = kDefaultWorldId;  // 默认雪吉拉（worldId=1）
    char worldName[kPayloadWorldNameCap]{"雪吉拉"};
    uint32_t hpPotion = 1;         // 自动加血（默认开）
    uint32_t mpPotion = 1;         // 自动加蓝（默认开）
    uint32_t hpThresholdPct = 50;  // 1-99
    uint32_t mpThresholdPct = 30;  // 1-99
    uint32_t petSummon = 0;              // 自动召唤宠物（默认关）
    uint32_t petSummonRequireFood = 0;   // 1=有粮才召（默认关）
    uint32_t multiSkill = 0;              // ???????
    uint32_t multiSkillGapMs = kMultiSkillGapDefaultMs;
    uint32_t multiSkillSafeStagger = 1;   // ?????????>=120?
    // v44: 多发可选直发（技能 SendSkillUseRequest + 普攻 Create50；默认关；失败回退）
    uint32_t multiSkillSendUseRequest = 0;
    uint32_t simpleCombat = 0;            // ????????
    uint32_t simpleCombatSmartInterval = 0;  // 智能间隔：面板间隔附近 ±40ms 抖动（默认关）
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
    // v52: 空中贴怪飞行速度倍率（%）。100 = 基准 1.0X。仅在空中贴怪开启时生效。
    uint32_t simpleCombatFlySpeedPct = kHeliSpeedPctDefault;
    // 拟人位移：同层走路贴近后 A 键出刀；与空中贴怪互斥（空中开时恒 0）。
    uint32_t simpleCombatHumanWalk = 0;
    uint32_t simpleCombatTeleportMinDx = kCombatTeleportMinDxDefault;
    uint32_t simpleCombatTeleportStandOff = kCombatTeleportStandOffDefault;
    // v64: F5 空中贴怪自定义站距（远程职业）；关则用内置近战最优值。见上方常量注释。
    uint32_t simpleCombatStandOffCustom = kCombatStandOffCustomDefault;
    uint32_t simpleCombatStandOffX = kCombatStandOffXDefault;
    int32_t simpleCombatStandOffY = kCombatStandOffYDefault;
    // v66: 站立伪装 —— 出刀那一瞬把 CurFh 种回合法台，骗过技能的「必须站立」门。
    uint32_t simpleCombatGroundSpoof = kCombatGroundSpoofDefault;
    // v67: 空中贴怪防抖 —— 到位后冻结站位点，模式仍 Station（可 ini/面板一键关）。
    uint32_t simpleCombatAntiJitter = kCombatAntiJitterDefault;
    // v69: 防贴脸退避 —— 站距 X/Y 内有任何怪（含锁定目标）就把站位点推开。默认关；
    // 需同时开「自定义站距」+ 空中贴怪，任一不满足即静默不生效（见 simple_combat 退避总闸）。
    uint32_t simpleCombatAntiHug = 0;
    // v78: 近战不挥拳 —— 普攻的 TryDoingMeleeAttack 判负，交给分发器的兜底射击。默认关；
    // 模块内自带「先测量再拦」判决，弓/弩一定拿不到 safe（伤害源就在近战体内）。
    uint32_t meleeVeto = 0;
    uint32_t simpleCombatTeleportCooldownMs = kCombatTeleportCooldownDefaultMs;
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
    // v79: 地面门旁路 —— 改 Magic/Shoot/Prepare 的 CurFh 判空跳转（.text）；默认关；实验 TAB。
    // 与 simpleCombatGroundSpoof（出刀瞬间种台）独立，可单独开做 A/B。
    uint32_t curFhGateBypass = 0;
    uint32_t autoLie = 1;  // 默认开：TextCaptcha+LLM / NonFinite 物理跟随
    uint32_t autoLieDryRun = 0;         // 1=???LLM ??? OnOk
    uint32_t autoLieMouseRegionOverlay = 0;  // 调试：NonFinite 题目区域叠层
    uint32_t autoLieAlarmTestSeq = 0;   // ???? payload ????
    uint32_t autoLieMouseSmokeSeq = 0;  // ???????????? UI?
    uint32_t autoLieMouseSimSeq = 0;    // 调试：内置轨迹模拟答题（bump 触发）
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
    // v61: 软重连试连（首页单勾选；同时武装 Galaxy token 只读采证）。默认开。
    // 亦可用 soft_login_probe.on / SOFT_LOGIN_PROBE=1；旧 galaxy_token_probe.on 仍可单独采证。
    uint32_t galaxyTokenProbe = 1;  // 与 softLoginProbe 同步写入；保留字段兼容旧 ini
    uint32_t softLoginProbe = 1;
    // v62: 调试 TAB「关闭断线弹窗」— bump 后载荷走 CloseDialog+SetActive（不点確認）
    uint32_t softLoginDismissSeq = 0;
    // v20/v82: 遇人策略 UX（UserPool + channel_hop，非 Reload）
    // v82 厂默：检测开；普通停手/换频关；仅 GM/隐身升级开
    uint32_t autoRelogin = 1;             // 检测同图玩家
    uint32_t autoReloginStopCombat = 0;   // 先停手（普通遇人）
    uint32_t autoReloginReconnect = 0;    // 一直有人就换频（普通遇人）
    // v60: GM/隐身升级（Admin·Manager 或客户端隐身 → 立刻停手/换频 + 强制 Alarm；默认开）
    uint32_t autoReloginGmEscalate = 1;
    // v46: 隐藏同图其他玩家（UserPool 远程 → AvatarRoot.SetActive；默认关）
    uint32_t hideOtherPlayers = 0;
    // v47: 引擎帧率锁（vSync=0 + Application.targetFrameRate；非显示器硬件刷新率）
    uint32_t frameLock = 1;  // 默认开，目标见 kFrameLockFpsDefault
    uint32_t frameLockFps = kFrameLockFpsDefault;
    // v48: 普攻必出终极一击（Final Attack prop→100；默认关）
    // 用户入口已关（kFinalAttackForceUserEnabled）；字段保留防旧 ini / 日后重开。
    uint32_t finalAttackForce = 0;
    // v54: 已学技能按满级（改 SkillRecord/Ex 等级；默认关）
    // 用户入口已关（kSkillMaxLevelUserEnabled）；字段保留防旧 ini / 日后重开。
    uint32_t skillMaxLevel = 0;
    // ????????? DragManager.CanPerformAction ??????????
    uint32_t dropAlertBypass = 0;  // 默认关：开着会抑制客户端警戒
    // 野外可开拍卖：数据面强制 MapDataInfo.IsTown=1（仅客户端；默认开）。
    // 服端可能断线；与挂机「守护模式」叠加会干净重拉——挂机/守护时建议关。
    uint32_t auctionTownBypass = 1;
    // v76/v77: 实验·坐下/椅子回蓝（刷 WM 累加器；默认关）。BIN 已证真蓝会动；过密踢。
    uint32_t restMpAccel = 0;
    uint32_t restMpAccelIntervalMs = kRestMpAccelIntervalDefaultMs;
    // v80/v81: 实验·无限飞镖。用户入口已关（kInfiniteStarsUserEnabled）；字段保留防旧 ini / 日后重开。
    uint32_t infiniteStars = 0;
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

inline uint32_t ClampHeliSpeedPct(uint32_t pct) {
    if (pct < kHeliSpeedPctMin) return kHeliSpeedPctMin;
    if (pct > kHeliSpeedPctMax) return kHeliSpeedPctMax;
    return pct;
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

inline uint32_t ClampAttackAccelClearBusyMinIntervalMs(uint32_t ms) {
    if (ms < kAttackAccelClearBusyMinIntervalMinMs) return kAttackAccelClearBusyMinIntervalMinMs;
    if (ms > kAttackAccelClearBusyMinIntervalMaxMs) return kAttackAccelClearBusyMinIntervalMaxMs;
    return ms;
}

// Apply 用：面板间隔 +（可选）首页加速地板 + 清忙锁专用地板。不用于改写落盘间隔。
inline uint32_t EffectiveAttackIntervalForApply(uint32_t panelMs, uint32_t attackAccel,
                                               uint32_t clearBusy, uint32_t clearBusyMinMs) {
    uint32_t v = EffectiveSimpleCombatAttackIntervalMs(panelMs, attackAccel);
    if (clearBusy) {
        const uint32_t floor = ClampAttackAccelClearBusyMinIntervalMs(
            clearBusyMinMs ? clearBusyMinMs : kAttackAccelClearBusyMinIntervalDefaultMs);
        if (v < floor) v = floor;
    }
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

inline uint32_t ClampRestMpAccelIntervalMs(uint32_t ms) {
    if (ms < kRestMpAccelIntervalMinMs) return kRestMpAccelIntervalMinMs;
    if (ms > kRestMpAccelIntervalMaxMs) return kRestMpAccelIntervalMaxMs;
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

inline uint32_t ClampCombatStandOffX(uint32_t v) {
    if (v < kCombatStandOffXMin) return kCombatStandOffXMin;
    if (v > kCombatStandOffXMax) return kCombatStandOffXMax;
    return v;
}

inline int32_t ClampCombatStandOffY(int32_t v) {
    if (v < kCombatStandOffYMin) return kCombatStandOffYMin;
    if (v > kCombatStandOffYMax) return kCombatStandOffYMax;
    return v;
}

inline int32_t ClampAttackAccelPartyBoosterValue(int32_t v) {
    if (v < kAttackAccelPartyBoosterValueMin) return kAttackAccelPartyBoosterValueMin;
    if (v > kAttackAccelPartyBoosterValueMax) return kAttackAccelPartyBoosterValueMax;
    return v;
}

inline int32_t ClampAttackAccelBreakDegreeFloorLo(int32_t v) {
    if (v < kAttackAccelBreakDegreeFloorLoMin) return kAttackAccelBreakDegreeFloorLoMin;
    if (v > kAttackAccelBreakDegreeFloorLoMax) return kAttackAccelBreakDegreeFloorLoMax;
    return v;
}

inline uint32_t ClampCombatTeleportCooldownMs(uint32_t v) {
    if (v < kCombatTeleportCooldownMinMs) return kCombatTeleportCooldownMinMs;
    if (v > kCombatTeleportCooldownMaxMs) return kCombatTeleportCooldownMaxMs;
    return v;
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
