#pragma once

#include <cstdint>
#include <cstddef>

namespace xcat {

// TWMS ???????launcher <-> payload??? user.ini [core]?
constexpr uint32_t kPayloadControlMagic = 0x58435443u;  // 'XCTC'
constexpr uint32_t kPayloadControlVersion = 1u;
constexpr uint32_t kPayloadControlCoreIniVersion = 158u;
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
// v59: simpleCombatAttackIntervalMs 出厂默认 123（v100 起不再读盘迁 50/46）
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
// v74: attackAccelClearBusy — 清 ActionBusy（产品文案「攻击无CD」，与首页 attackAccel 入口独立）
// v75: attackAccelClearBusyMinIntervalMs — 清忙锁开启时出刀间隔地板（默认 410）
// v99: 清忙锁出刀地板解除 — Apply 不再 max 抬间隔；默认/下限 1；读盘迁旧默认 410
// v100: 攻击间隔只保留出厂默认 123；取消读盘 50/46→123 迁移
// v101: autoReloginStopGather — 遇人停吸（遇人策略；仅 WS888 解锁后露出）
// v76: restMpAccel — 实验·坐下/椅子回蓝累加器加速（写 WM+0x17C/+0x180；默认关）
// v77: restMpAccelIntervalMs — 写满间隔（默认 2500；过密会踢；BIN 已证真回蓝）
// v78: meleeVeto — 「近战不挥拳」：让普攻那一发 TryDoingMeleeAttack 判负，落到兜底射击。
//      勾上后先测量再决定：观测到近战体内自己转调射击（nest>0）就拒绝拦截 —— 弓正是这样，
//      拦了会把整条伤害链打断（见 ARCHER_SHOOT_VS_BONK_GATE_20260809.md §0″/§0‴）。默认关。
// v79: curFhGateBypass — 实验·地面门旁路（改 GA CurFh 判空跳转；≠ 站立伪装）
// v80: infiniteStars — 实验·无限飞镖：自动维持 4121006 无形镖 + 客户端冻 207xxxx 扣数；默认关
// v81: kInfiniteStarsUserEnabled=false — 实验 TAB 不画入口；不启 worker / 不挂钩；代码保留
// v83: simpleCombatHitRotate / HitRotateN — 「打中换怪」：同一 oid 确认命中 N 次后切攻击盒外最近活怪；活怪<3 停刀
// v84: simpleCombatForgeHit — 打怪出刀自组攻包；落盘 user.ini
// v85: mapAttack — 实验·全图攻击 P2 扩盒（FindHit Rect=本图 AABB，不抬 maxCount）；会话态默认关，不落盘
// v86: 实验 TAB「发一刀伪造攻包」oneshot — 仅会话文件 state/attack_rpc_fire_seq，不写 user.ini，默认不发
// v91: 实验 TAB「清零攻包探针计数」— state/attack_rpc_reset_seq，不写 user.ini；DLL 内 gOkSession 清零
// v92: 实验 TAB 攻包探针 auto_stop 勾灭 — state/attack_rpc_stop_seq；DLL 写、面板读后把 attackRpc 勾掉
// v87: simpleCombatHiraishin — F5 追怪「站桩输出」：原地出刀 + 吸怪；默认关，与空中贴怪/拟人互斥
// v88: simpleCombatHiraishinLootHoldMs — 站桩输出开打前原地站给吸物（ms；0=不等；默认 3000）
// v89: simpleCombatHiraishinRangePx — 站桩输出选怪 hypot（px；0=吸怪圈；现不挡刀）
// v98: simpleCombatHiraishinFrontDx/Dy — 站桩输出面前攻击盒半宽/半高（px；0=该轴不限）
// v90: mobGather — 吸怪（首页卡）；落盘 user.ini
// v93: mobGatherSpeedPct / mobGatherAntiJitter — 吸怪倍率/防抖，与 F5 滑翔同构，落盘 user.ini
// v94: 吸怪正式上首页（挂机卡下方）；不再当实验会话开关清零
// v95: mobGatherMax / RadiusPx / HoldMs / IgnoreQuiet — BIN 硬编码旋钮上首页吸怪卡
// v96: 吸怪独立 TAB；落点/作动器旋钮落盘，不再跟 F5 自定义站距绑死
// v97: 曾删吸怪控权申请（calc_priority 钩 / 造包）
// v99: 吸怪落点不贴台（删 mobGatherSnapXPad / SnapAbove）
// v100: mobGatherApplyCtrl — TAB「申请控制权」；默认关；泵上调官方 ApplyControl
// v101: mobGatherIntervalMs — 吸怪新收间隔（ms）；默认 40；已吸住的仍跟瞄准
// v102: 吸怪自定义落点 X 改为有符号自填（±30000），不再套 F5 0–900
// v103: 吸怪档速/到位/瞄准间隔上 TAB（巡航/进站/悬停档、到位圈、到位Kp、刹车、下滑切断、瞄准）
    // v104: mobGatherSoftReloginSec — 「X秒后触发软重连」；厂默关（叠登会「已登出」）；勾了才拆
    // v122: 主动软重连从吸怪 TAB 挪到首页挂机卡；不再绑吸怪；ini 键仍 mobGatherSoftRelogin*
    // 出过刀才起表 hangup 清 FLAG；出刀后关 F5 仍走完。没出过刀才不计时。卖装/Travel 冻钟。
// v105: mobGatherQuietDelayMs — 吸怪整模块延时启动（ms）；开吸怪 / hold 结束起表；与落地也吸独立
// v106: mobGatherClearRelogin — 吸怪 TAB「清怪重连」；一轮白名单死干净才拆会话（v117 厂默改关）
// v107: simpleCombatHiraishinFrontDx 厂默 280→110（站桩输出「横向」）
// v108: mobGatherFarInFlight — 圈外同时在途上限；0=不限；吸怪 TAB「在途」
// v109: simpleCombatHiraishinFrontDx/Dy 厂默 110×100→80×60（站桩输出面前盒）
// v110: mobGatherSeekCluster — 吸怪 TAB「先飞到最密堆再吸」；默认关=站立吸怪
// v111: mobGatherHomeReturn — 吸怪 TAB「软重连后返回原位」；与寻簇互斥；F5/按钮记 AbsPos
// v112: mobGatherHomeReturn 厂默开；无首次记录（valid/hasMap）不飞
// v113: travelPortalAimLiftY — 调试 TAB「超级赶路」贴门抬升（AbsPos 更大 Y=更高）
// v114: mobGatherLayerYPx — 吸怪 TAB「竖层」；寻簇同层窗 |dY|，默认 200
// v115: mobGatherDyLimPx — 吸怪 TAB「高度闸」；新收 |dY| 上限，默认 1200
// v152: 吸怪半径/高度闸/脚边厂默改 8000
// v116: 站桩输出厂默 静止 0ms、面前盒 30×10（旧 3000 / 80×60 / 110×100 / 280 读盘迁）
// v117: mobGatherWalkDx / mobGatherFeetExemptPx — 吸怪 TAB 履历横移/脚边；清怪重连厂默关
// v118: mobGatherStandOffX/Y 厂默 40/10→29/9（吸怪自定义落点；旧 62/12 一并迁）
// v119: 站桩输出面前盒厂默 30×10→60×10（横向；竖直仍 10）
// v151: 站桩输出面前盒厂默 60×10→100×80（横向/竖直）
// v120: mobGatherHomeReturn 厂默改关（旧盘已有键原样保留）
// v121: mobGatherAimJitterPx — 吸怪 TAB 落点「抖动」；按 oid 稳定散开，0=叠点
// v122: 主动软重连入口改首页挂机卡，不再要求先开吸怪（键名不变）
// v123: simpleCombatTeleportOneHit — 瞬移找怪「每只怪打一下」；默认关
// v124: mobPoolObserve — 怪物刷新感知增强（MI Enter/Leave → ImmediateScan；默认关）
// v125: simpleCombatTeleportMaxHop 厂默 400/520/550→3000（远图一次到位；手改保留）
// v153: simpleCombatTeleportMaxHop 厂默 3000→1500
// v126: simpleCombatTeleportCooldownMs 厂默 200→80；读盘迁旧默认 200
// v127: mobGatherHangupFires — 落地后累计出刀达此主动软重连；与首页秒数先到先拆；厂默 1900
//       simpleCombatForgeHitFrontDx/Dy — 出刀自组攻包独立攻击盒；不与站桩面前盒共用
// v128: 自组攻包攻击盒厂默 60×10→480×420；缺键不再抄站桩盒
//       mobGatherHangupFiresOn — 出刀累计软重连独立勾选；与秒数勾选互不绑架；厂默开
// v129: gatherTabUnlocked — 调试 TAB ws888 解锁镜像；标题/顶栏「刀 n/阈值」显示门控（缺键关）
// v130: secAttackIntercept — 已移除（服端自有计数；字段保留布局，恒 0）
// v131: mobGatherHangupUnbindF5 — 调试 TAB：解除瞬移找怪+F5 强制开秒数闸；缺键关
// v132: secAttackTextHook — 已移除（服端自有计数；字段保留布局，恒 0）
// v133: autoReloginReconnect — 遇人「一直有人就换频」厂默开；旧厂默换频关读盘迁一次
// v154: 遇人策略厂默：先停手·停吸·换频·GM 升级开；隐藏玩家关。撤 v82 无条件改写；旧厂默指纹迁一次
// v155: mobGatherFirstGenOnly — 吸怪 TAB「只吸场上的」；厂默开。缺键=开。
// v156: 遇人总开关厂默开。v154 曾把旧厂默迁成关；v154 厂默指纹迁一次开。藏人仍默认关。
// v157: attackNoCdEncounterUnbind — 调试 TAB 解绑「攻击无CD → 强制遇人三项」；缺键=仍绑定
// v134: simpleCombatSkipAccMiss / SkipAccMissN — 「不打MISS怪」：进盒 ACC 不够连续 N 次后换怪
// v135: simpleCombatForgeHitMobs — 已移除（填充列表/同拍多包踢号；字段保留布局，恒 1）
// v136: simpleCombatForgeHitFillList / MultiPkt — 已移除（已证实踢号；字段保留布局，恒 0）
// v137: simpleCombatSkipAccMiss 厂默关→开（当时 N=2）；旧盘仍为关迁一次。升 version 后用户可再关掉
// v138: simpleCombatSkipAccMissN 厂默 2→1；旧盘仍为 2 迁一次。升 version 后用户可再改
// v148: simpleCombatSkipAccMissN 厂默 1→3；残血 MISS 不再换靶。旧盘仍为 1 迁一次。
// v158: simpleCombatSkipAccMissN 厂默 3→1；旧盘仍为 3 迁一次。升 version 后用户可再改
// v150: 位移夹速厂默开→关（BIN：cap=48 仍 205）；旧盘 on=1 迁一次。升 version 后可再勾。
// v151: 站桩输出面前盒厂默 60×10→100×80；旧横向 60 / 竖直 10 读盘迁一次。手改保留。
// v152: 吸怪半径/高度闸/脚边厂默改 8000；旧 1000·2800 / 1200 / 320 读盘迁一次。手改保留。
// v153: 自动打怪寻怪 simpleCombatTeleportMaxHop 厂默 3000→1500；旧 400/520/550/3000 读盘迁。手改保留。
// v149: mobGatherReconnectHop — 吸怪 TAB「重连换频」；软重连进异频；厂默关。缺键=关。
// v139: 主动软重连旧厂默开+14s 残留迁关一次（v122 已改厂默关，当时没迁盘）
// v140: forceTrade — 实验·强制交易：改 UIUserInfo 人物卡交易按钮等级阈值 global；默认关
//       kForceTradeUserEnabled=false — 实测无效；实验 TAB 置灰；不启 worker / 不改阈值
// v141: mobGatherDispClampOn / mobGatherDispCapPx — 吸怪 TAB「位移夹速」：把被拽怪每帧位移
//       夹到 ≤ cap(px/帧)，不越客户端「怪速+10」门→不触发 prevpos 举报；v150 起厂默关、cap=48
// v142: mobGatherHangupFires 厂默 1900→1700；旧盘仍为 1900/1800 迁一次。升 version 后用户可再改
// v143: mobGatherStrategy — 吸怪开关旁策略：0=A IMPACT（现有悬停）、1=B FH-SNAP（官方绑台）
// v144: mobGatherLandOnArrive — 策略 A 子项「到站落地」：怪水平到站(≤stationR)后松手，交给游戏
//       原生物理自然下坠落台（不再逐帧摘台/定住）；玩家走远(>cruiseR)恢复吸拉。默认关。落点由站距自调。
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
constexpr uint32_t kWatchdogNoExpSecMin = 10u;
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
// 默认 123：仅缺 key / 新装时的出厂值。已有 ini 数值原样保留（不迁 50/46）。
constexpr uint32_t kSimpleCombatAttackIntervalDefaultMs = 123u;
constexpr uint32_t kSimpleCombatAttackIntervalMinMs = 1u;
constexpr uint32_t kSimpleCombatAttackIntervalMaxMs = 10000u;
// 出刀按键 hold 时长（调试 TAB）：OnFuncKey Down→Up 的间隔。
// 实际 hold = min(此值, 面板间隔)，hold ≥ 间隔会让 pendingUp 把下一刀锁死。
// 注意：攻击加速开启时走 Down+Up 同泵的 pulse 路径（hold=0），此值不参与。
constexpr uint32_t kAttackHoldDefaultMs = 5u;
constexpr uint32_t kAttackHoldMinMs = 1u;
constexpr uint32_t kAttackHoldMaxMs = 100u;
// 站桩输出开打前 / 软重连回频落地后原地站（ms）。0=不等。换怪不站。
// 厂默 0；旧厂默 3000 读盘迁过来（显式改过其它值保留）。
constexpr uint32_t kHiraishinLootHoldDefaultMs = 0u;
constexpr uint32_t kHiraishinLootHoldLegacyDefaultMs = 3000u;
constexpr uint32_t kHiraishinLootHoldMinMs = 0u;
constexpr uint32_t kHiraishinLootHoldMaxMs = 30000u;
// 站桩输出人↔怪 hypot 上限（px）。0=不限（当前全图）。不是官方武器盒。
constexpr uint32_t kHiraishinRangeDefaultPx = 0u;
constexpr uint32_t kHiraishinRangeMinPx = 0u;
constexpr uint32_t kHiraishinRangeMaxPx = 8000u;
// 站桩输出面前攻击盒（AbsPos 半宽/半高，px）。0=该轴不限。
// 厂默 100×80；旧横向 280 / 110 / 80 / 30 / 60、旧竖直 100 / 60 / 10 读盘迁（显式改过其它值保留）。
constexpr uint32_t kHiraishinFrontDxDefault = 100u;
constexpr uint32_t kHiraishinFrontDxLegacyDefault = 280u;
constexpr uint32_t kHiraishinFrontDxLegacyDefaultV107 = 110u;
constexpr uint32_t kHiraishinFrontDxLegacyDefaultV109 = 80u;
constexpr uint32_t kHiraishinFrontDxLegacyDefaultV116 = 30u;
constexpr uint32_t kHiraishinFrontDxLegacyDefaultV119 = 60u;
constexpr uint32_t kHiraishinFrontDyDefault = 80u;
constexpr uint32_t kHiraishinFrontDyLegacyDefault = 100u;
constexpr uint32_t kHiraishinFrontDyLegacyDefaultV109 = 60u;
constexpr uint32_t kHiraishinFrontDyLegacyDefaultV116 = 10u;
constexpr uint32_t kHiraishinFrontDxMin = 0u;
constexpr uint32_t kHiraishinFrontDyMin = 0u;
constexpr uint32_t kHiraishinFrontDxMax = 4000u;
constexpr uint32_t kHiraishinFrontDyMax = 2000u;
// 出刀自组攻包钉锁过远尺（与站桩面前盒同钳、不同键）。厂默 480×420。
constexpr uint32_t kForgeHitFrontDxDefault = 480u;
constexpr uint32_t kForgeHitFrontDyDefault = 420u;
constexpr uint32_t kForgeHitFrontDxLegacyDefault = 60u;
constexpr uint32_t kForgeHitFrontDyLegacyDefault = 10u;
constexpr uint32_t kForgeHitFrontDxMin = kHiraishinFrontDxMin;
constexpr uint32_t kForgeHitFrontDyMin = kHiraishinFrontDyMin;
constexpr uint32_t kForgeHitFrontDxMax = kHiraishinFrontDxMax;
constexpr uint32_t kForgeHitFrontDyMax = kHiraishinFrontDyMax;
// 已移除：填充列表 / 同拍多包 / 多怪数。布局占位，恒 1 / 0 / 0。
constexpr uint32_t kForgeHitMobsDefault = 1u;
constexpr uint32_t kForgeHitMobsMin = 1u;
constexpr uint32_t kForgeHitMobsMax = 15u;
constexpr uint32_t kForgeHitFillListDefault = 0u;
constexpr uint32_t kForgeHitMultiPktDefault = 0u;
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
// F5 滑翔 / 自动赶路默认倍率 500%（实测已验档）。可调到顶格 1000%；旧 ini 有值则读盘为准。
constexpr uint32_t kHeliSpeedPctDefault = 500u;
constexpr uint32_t kHeliSpeedPctMin = 25u;
constexpr uint32_t kHeliSpeedPctMax = 1000u;
// F6 手动飞默认倍率 300%：换旋翼前开环等效约 1600 px/s ≈ 2.6X，
// 直接给 1.0X（Cruise 620）会比旧版慢一大截；3X 开箱手感更接近旧手动飞。
constexpr uint32_t kFlySpeedPctDefault = 300u;
// 吸怪倍率：与 F5 滑翔同一套 100%=巡航 620、顶格 1000%；默认仍满火力，不跟 F5 默认绑死。
constexpr uint32_t kMobGatherSpeedPctDefault = 1000u;
constexpr uint32_t kMobGatherSpeedPctMin = kHeliSpeedPctMin;
constexpr uint32_t kMobGatherSpeedPctMax = kHeliSpeedPctMax;
constexpr uint32_t kMobGatherAntiJitterDefault = 1u;
// 吸怪 BIN 旋钮：同时 / 收怪半径 / 白名单超时 / 落地静默是否继续吸。
constexpr uint32_t kMobGatherMaxDefault = 64u;
constexpr uint32_t kMobGatherMaxMin = 1u;
constexpr uint32_t kMobGatherMaxMax = 64u;
// 圈外（巡航圈外）同时在途上限。0=不限。已到脚边的不占这格。
constexpr uint32_t kMobGatherFarInFlightDefault = 0u;
constexpr uint32_t kMobGatherFarInFlightMin = 0u;
constexpr uint32_t kMobGatherFarInFlightMax = 64u;
// 2026-08-23：厂默 1000→8000。旧厂默 2800 / 1000 读盘迁（显式改过其它值保留）。
constexpr uint32_t kMobGatherRadiusDefaultPx = 8000u;
constexpr uint32_t kMobGatherRadiusLegacyDefaultPx = 1000u;
constexpr uint32_t kMobGatherRadiusLegacyDefaultOldPx = 2800u;
constexpr uint32_t kMobGatherRadiusMinPx = 200u;
// 半径只防爆钳，不再业务封顶 8000（落点同款 ±30000）。
constexpr uint32_t kMobGatherRadiusMaxPx = 30000u;
constexpr uint32_t kMobGatherHoldMsDefault = 8000u;
constexpr uint32_t kMobGatherHoldMsMin = 500u;
constexpr uint32_t kMobGatherHoldMsMax = 30000u;
// 新收/泵一批的间隔。默认 40 = 现状。已入白名单的怪仍按瞄准 VTOL，不跟这个闸。
constexpr uint32_t kMobGatherIntervalDefaultMs = 40u;
constexpr uint32_t kMobGatherIntervalMinMs = 40u;
constexpr uint32_t kMobGatherIntervalMaxMs = 5000u;
constexpr uint32_t kMobGatherIgnoreQuietDefault = 0u;
// 延时启动：开吸怪 / 软重连 hold 结束进图后，整模块再等这么久才收怪。0=立刻。
// 与「落地也吸」独立。hold 中硬停并清钟。用户自填，只防 ImGui int / DWORD 溢出。
constexpr uint32_t kMobGatherQuietDelayMsDefault = 0u;
constexpr uint32_t kMobGatherQuietDelayMsMin = 0u;
constexpr uint32_t kMobGatherQuietDelayMsMax = 2147483647u;
constexpr uint32_t kMobGatherStandOffCustomDefault = 1u;
// 厂默 29/9；旧厂默 40/10、62/12 读盘迁过来（显式改过其它值保留）。
constexpr int32_t kMobGatherStandOffXDefault = 29;
constexpr int32_t kMobGatherStandOffYDefault = 9;
constexpr int32_t kMobGatherStandOffXLegacyDefault = 40;
constexpr int32_t kMobGatherStandOffXLegacyDefaultOld = 62;
constexpr int32_t kMobGatherStandOffYLegacyDefault = 10;
constexpr int32_t kMobGatherStandOffYLegacyDefaultOld = 12;
// 自定义落点：用户自填。只留防爆钳，不再套 F5 站距 0–900 / ±600。
constexpr int32_t kMobGatherStandOffXMin = -30000;
constexpr int32_t kMobGatherStandOffXMax = 30000;
constexpr int32_t kMobGatherStandOffYMin = -30000;
constexpr int32_t kMobGatherStandOffYMax = 30000;
// 落点抖动半径。每只怪用 oid 哈希出固定偏移，避免全叠一点。0=不散。
constexpr uint32_t kMobGatherAimJitterDefault = 24u;
constexpr uint32_t kMobGatherAimJitterMin = 0u;
constexpr uint32_t kMobGatherAimJitterMax = 30000u;
constexpr uint32_t kMobGatherStickCreepDefault = 8u;
constexpr uint32_t kMobGatherStickCreepMin = 1u;
constexpr uint32_t kMobGatherStickCreepMax = 40u;
constexpr uint32_t kMobGatherStickStillVDefault = 50u;
constexpr uint32_t kMobGatherStickStillVMin = 0u;
constexpr uint32_t kMobGatherStickStillVMax = 400u;
constexpr uint32_t kMobGatherCruiseRDefault = 140u;
constexpr uint32_t kMobGatherCruiseRMin = 40u;
constexpr uint32_t kMobGatherCruiseRMax = 800u;
constexpr uint32_t kMobGatherStationRDefault = 28u;
constexpr uint32_t kMobGatherStationRMin = 8u;
constexpr uint32_t kMobGatherStationRMax = 200u;
constexpr uint32_t kMobGatherMaxCmdDefault = 4800u;
constexpr uint32_t kMobGatherMaxCmdMin = 620u;
constexpr uint32_t kMobGatherMaxCmdMax = 8000u;
// 位移夹速：每帧位移上限（px/帧）。客户端每帧比对怪移动位移 vs 怪速+10，超门即触发 prevpos 举报
// （逆向见 docs/features/security/怪速举报type21与被动插值.md §7.2）。夹到 ≤ cap 就永不触发。
// 厂默关（v150；BIN 证 cap=48 仍 205）。48px/帧（≈1600px/s@30ms）仍是上限旋钮默认。
constexpr uint32_t kMobGatherDispClampOnDefault = 0u;
constexpr uint32_t kMobGatherDispCapPxDefault = 48u;
constexpr uint32_t kMobGatherDispCapPxMin = 8u;
constexpr uint32_t kMobGatherDispCapPxMax = 400u;
// v143: 吸怪策略。0=A IMPACT（现有 SetImpactNext 悬停）；1=B FH-SNAP（官方绑台）。缺键=A。
constexpr uint32_t kMobGatherStrategyImpact = 0u;
constexpr uint32_t kMobGatherStrategyFhSnap = 1u;
constexpr uint32_t kMobGatherStrategyDefault = kMobGatherStrategyImpact;
// v144: 策略 A「到站落地」。1=到站后松手让怪自然落台；0=保持现有悬停。默认 0。
constexpr uint32_t kMobGatherLandOnArriveDefault = 0u;
// v145: 远怪接力跳距（px/跳）。实机标定：单次连续拉取总距 ≤~1024px 零断连（r=1000 · 9.4min），
// ≥~1253px 必掐（179 场尸检 + r=2800 风暴）。>跳距的远怪改为逐跳接力：每跳 ≤hopPx、到中转点
// 驻留 ~450ms 让服务器移动段落账，再起下一跳。0=关（直拉，旧行为）。
constexpr uint32_t kMobGatherHopPxDefault = 950u;
constexpr uint32_t kMobGatherHopPxMin = 200u;
constexpr uint32_t kMobGatherHopPxMax = 1150u;
// 误把厂默改成 1500 的一版：读盘迁回 950。
constexpr uint32_t kMobGatherHopPxMistakenDefault = 1500u;
constexpr uint32_t kMobGatherKpDefault = 7u;
constexpr uint32_t kMobGatherKpMin = 1u;
constexpr uint32_t kMobGatherKpMax = 20u;
constexpr uint32_t kMobGatherDeadDefault = 6u;
constexpr uint32_t kMobGatherDeadMin = 1u;
constexpr uint32_t kMobGatherDeadMax = 40u;
constexpr uint32_t kMobGatherGravityDefault = 60u;
constexpr uint32_t kMobGatherGravityMin = 0u;
constexpr uint32_t kMobGatherGravityMax = 200u;
// 1X 档速（px/s）。吸速% 再乘。默认抄现状 620 / 480 / 360。
constexpr uint32_t kMobGatherCruiseVDefault = 620u;
constexpr uint32_t kMobGatherCruiseVMin = 100u;
constexpr uint32_t kMobGatherCruiseVMax = 4000u;
constexpr uint32_t kMobGatherStationVDefault = 480u;
constexpr uint32_t kMobGatherStationVMin = 50u;
constexpr uint32_t kMobGatherStationVMax = 4000u;
constexpr uint32_t kMobGatherHoldVDefault = 360u;
constexpr uint32_t kMobGatherHoldVMin = 20u;
constexpr uint32_t kMobGatherHoldVMax = 4000u;
// 到位软钉：误差 ≤ 到位圈 且防抖开 → 改用到位 Kp。刹车只吃 ≥5X 死拍。
constexpr uint32_t kMobGatherSettleErrDefault = 16u;
constexpr uint32_t kMobGatherSettleErrMin = 1u;
constexpr uint32_t kMobGatherSettleErrMax = 400u;
constexpr uint32_t kMobGatherKpSettleDefault = 10u;
constexpr uint32_t kMobGatherKpSettleMin = 1u;
constexpr uint32_t kMobGatherKpSettleMax = 40u;
constexpr uint32_t kMobGatherBrakeMsDefault = 150u;
constexpr uint32_t kMobGatherBrakeMsMin = 20u;
constexpr uint32_t kMobGatherBrakeMsMax = 1000u;
constexpr uint32_t kMobGatherCoastVyDefault = 80u;
constexpr uint32_t kMobGatherCoastVyMin = 0u;
constexpr uint32_t kMobGatherCoastVyMax = 800u;
constexpr uint32_t kMobGatherAimMsDefault = 17u;
constexpr uint32_t kMobGatherAimMsMin = 8u;
constexpr uint32_t kMobGatherAimMsMax = 100u;
constexpr uint32_t kMobGatherSoftReloginDefault = 0u;
constexpr uint32_t kMobGatherSoftReloginSecDefault = 20u;
// v104 厂默 14s 且默认开。v122 改关后旧盘仍 1+14，当残留迁关。
constexpr uint32_t kMobGatherSoftReloginSecLegacyDefault = 14u;
constexpr uint32_t kMobGatherSoftReloginSecMin = 10u;
constexpr uint32_t kMobGatherSoftReloginSecMax = 3600u;
// 落地后累计出刀（与 combat.log「fire id=」同拍 +1）达此主动软重连；与首页秒数先到先拆。勾选独立。
constexpr uint32_t kMobGatherHangupFiresDefault = 1700u;
constexpr uint32_t kMobGatherHangupFiresMin = 0u;
constexpr uint32_t kMobGatherHangupFiresMax = 1900u;
// v127 厂默 1900；会话里还出现过 1800。v142 迁一次。
constexpr uint32_t kMobGatherHangupFiresLegacyDefault = 1900u;
constexpr uint32_t kMobGatherHangupFiresLegacyDefault1800 = 1800u;
constexpr uint32_t kMobGatherHangupFiresOnDefault = 1u;
constexpr uint32_t kMobGatherHangupUnbindF5Default = 0u;
// 清怪重连：吸怪开着才拆。默认关，与首页主动软重连独立。缺键走关；盘上已有键原样保留。
constexpr uint32_t kMobGatherClearReloginDefault = 0u;
// 先飞到最密堆再吸。默认关：站立吸怪（高度闸/履历闸/14s 照旧）。
constexpr uint32_t kMobGatherSeekClusterDefault = 0u;
// v146: 远怪自动巡点。寻簇的跨层/全图版：没持怪时不限 |dY| 找全图最密堆、贴台点也不限层，
// 人飞过去就地吸。配合安全聚拢半径（≤~1100，服务器按「离怪原位总位移 >~1200px」掐线，
// 远怪拉不得只能人过去）。默认关。与「软重连后返回原位」互斥。
constexpr uint32_t kMobGatherPatrolFarDefault = 0u;
// v147 键仍保留（INI 兼容），但 2026-08-22 起硬关：BIN 证伪，读写/apply 都强制 0，永不打 GA .text。
// 详见 docs/features/security/怪速举报type21与被动插值.md §7.12 与 mob_prevpos_patch.h。
constexpr uint32_t kMobGatherAntiReportDefault = 0u;
// 软重连后飞回记录点再吸。默认关。无首次记录不飞。与寻簇互斥。
constexpr uint32_t kMobGatherHomeReturnDefault = 0u;
// v149: 软重连不粘原频。默认关。缺键=关。不改遇人 hop。
constexpr uint32_t kMobGatherReconnectHopDefault = 0u;
// v155: 只吸场上的。默认开。缺键=开。开=勾上时场上 oid 快照才吸；之后新刷不吸。
constexpr uint32_t kMobGatherFirstGenOnlyDefault = 1u;
// 寻簇同层窗 |dY|（AbsPos）。默认 200。用户自填，只防爆钳 30000。0=合法。
constexpr uint32_t kMobGatherLayerYPxDefault = 200u;
constexpr uint32_t kMobGatherLayerYPxMin = 0u;
constexpr uint32_t kMobGatherLayerYPxMax = 30000u;
// 高度闸 |mobY-py|（AbsPos）。默认 8000。用户自填，只防爆钳 30000。0=合法。
// 只挡新 Arm；已吸住的继续。距人 hypot≤脚边 不走这闸。
constexpr uint32_t kMobGatherDyLimPxDefault = 8000u;
constexpr uint32_t kMobGatherDyLimPxLegacyDefault = 1200u;
constexpr uint32_t kMobGatherDyLimPxMin = 0u;
constexpr uint32_t kMobGatherDyLimPxMax = 30000u;
// 履历闸横移 |dx from first seen|。默认 96。用户自填，只防爆钳 30000。0=不挡横移。
constexpr uint32_t kMobGatherWalkDxDefault = 96u;
constexpr uint32_t kMobGatherWalkDxMin = 0u;
constexpr uint32_t kMobGatherWalkDxMax = 30000u;
// 脚边豁免 hypot。默认 8000。用户自填，只防爆钳 30000。0=不开豁免。
constexpr uint32_t kMobGatherFeetExemptPxDefault = 8000u;
constexpr uint32_t kMobGatherFeetExemptPxLegacyDefault = 320u;
constexpr uint32_t kMobGatherFeetExemptPxMin = 0u;
constexpr uint32_t kMobGatherFeetExemptPxMax = 30000u;
constexpr uint32_t kMobScanIntervalDefaultMs = 20u;
constexpr uint32_t kMobScanIntervalLegacyDefaultMs = 50u;
constexpr uint32_t kMobScanIntervalMinMs = 1u;
constexpr uint32_t kMobScanIntervalMaxMs = 500u;
// 与全局 Min 对齐；加速不另抬间隔。
constexpr uint32_t kAttackAccelIntervalFloorMs = 1u;
// 清忙锁专用出刀间隔字段（历史地板）。v99 起 Apply 不再用它抬有效间隔；
// 默认/下限 1，与首页攻击间隔对齐；旧默认 410 读盘迁到 1。
constexpr uint32_t kAttackAccelClearBusyMinIntervalDefaultMs = 1u;
constexpr uint32_t kAttackAccelClearBusyMinIntervalMinMs = 1u;
constexpr uint32_t kAttackAccelClearBusyMinIntervalMaxMs = 1000u;
constexpr uint32_t kAttackAccelClearBusyMinIntervalLegacyDefaultMs = 410u;

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
// false：实验 TAB「自动召唤宠物 / 有粮才召」置灰；读盘/落盘/Apply 强制关，防更新残留仍召宠。
constexpr bool kPetSummonUserEnabled = false;
// false：实验 TAB 不画「无限飞镖」入口；读盘/落盘/Apply 强制关，不启 worker、不挂钩。
constexpr bool kInfiniteStarsUserEnabled = false;
// false：实验 TAB「强制交易」置灰；读盘/落盘/Apply 强制关，不启 worker、不改 UIUserInfo 阈值
//（实测改客户端 15 级门无效，服端仍拒；避免再碰人物卡/交易闸挡原业务）。
constexpr bool kForceTradeUserEnabled = false;
// false：实验 TAB「拍卖原生按钮（一次）」置灰；IPC 脉冲不点状态栏 17、不写等级/建角。
// 代码保留；野外开拍卖走 auctionTownBypass，不走本探针。
constexpr bool kAuctionGateProbeUserEnabled = false;
// 群怪优先：落盘仍用 clusterWeight；0=关，非 0=开（旧 1–100 权重一律视为开）。
constexpr uint32_t kClusterWeightDefault = 0u;
constexpr uint32_t kClusterWeightMax = 100u;
// 打中换怪：同一 oid 确认命中 N 次后改打攻击盒外最近活怪（空刀不计；打偏记真实 oid）。
// 场上活怪 < 3 停刀（BOSS 图保护）。默认关。
constexpr uint32_t kCombatHitRotateDefault = 0u;
constexpr uint32_t kCombatHitRotateNDefault = 2u;
constexpr uint32_t kCombatHitRotateNMin = 1u;
constexpr uint32_t kCombatHitRotateNMax = 20u;
// 不打 MISS 怪：进盒但 ACC 不够、连续 N 次 Damage=0 后禁锁换靶。默认开 / N=1。
// 不预调 CheckPDamageMiss（吃 Rand32）。残血 / 已打出真伤的浮动 MISS 不计。
constexpr uint32_t kCombatSkipAccMissDefault = 1u;
constexpr uint32_t kCombatSkipAccMissNDefault = 1u;
constexpr uint32_t kCombatSkipAccMissNMin = 1u;
constexpr uint32_t kCombatSkipAccMissNMax = 5u;
// 瞬移找怪「每只怪打一下」：出一刀后切下一只（走原选怪）。默认关。
constexpr uint32_t kCombatTeleportOneHitDefault = 0u;
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

// ── F5 自定义站距（面板「自定义站距」勾选） ──────────────────────────
// 水平 X：空中贴怪 / 拟人 / 瞬移找怪共用。地面 ClampStandOff 再夹进
// kCombatTeleportStandOffMin/Max（12–200），避免贴怪心或 hop 过远。
// 垂直 Y：只给空中贴怪悬停；拟人/瞬移贴台，不吃 Y。
//
// 关：X 用 kCombatStandOffXDefault（60），Y 用内置 kHeliLiftPx。
// 开：完全听用户的 —— 远程职业站在自己的射程上打，命中率由用户自己调。
//     ⚠️ 空中出刀闸与到位判据会**跟着放大**，否则站到 250px 会被 |dx|<=120 的硬闸一票否决。
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
// 历史「贴怪 Native 自冷」落盘字段；hop 间距已改认 Settling/PosSane，此值不再下发 Native。
// 下限 5 仅防离谱读盘。换频/开趟暂停走 ForceNativeCooldown，与本字段无关。
constexpr uint32_t kCombatTeleportCooldownDefaultMs = 80u;
constexpr uint32_t kCombatTeleportCooldownMinMs = 5u;
constexpr uint32_t kCombatTeleportCooldownMaxMs = 8000u;
constexpr uint32_t kCombatTeleportCooldownLegacyDefaultMs = 200u;
// 单次贴怪 hop（px）；更远分段贴近。调试 TAB 可调。
// 默认 1500。只夹下限，无上限。旧厂默 400 / 520 / 550 / 3000 读盘迁。
constexpr uint32_t kCombatTeleportMaxHopDefault = 1500u;
constexpr uint32_t kCombatTeleportMaxHopMin = 350u;
// 旧默认迁移：400→520→550→3000→1500；显式调过其它值保留。
constexpr uint32_t kCombatTeleportMaxHopLegacyDefault = 400u;
constexpr uint32_t kCombatTeleportMaxHopPrevDefault = 520u;
constexpr uint32_t kCombatTeleportMaxHopPrevDefault2 = 550u;
constexpr uint32_t kCombatTeleportMaxHopPrevDefault3 = 3000u;
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
// 0=关闭背压；上限须等于泵队列容量 kQueueCap（main_thread_pump.cpp，当前 12）。
constexpr uint32_t kPumpQueueCap = 12u;
constexpr uint32_t kPumpCongestionDefault = 9u;  // 3/4 × 12
constexpr uint32_t kPumpCongestionMin = 0u;
constexpr uint32_t kPumpCongestionMax = kPumpQueueCap;
// 每 tick Drain 最多跑几个排队 job；须 ∈[1, kQueueCap]。
// 默认 8：槽加到 12 后仍不把一整队托管活倒进同一 Unity 帧。
constexpr uint32_t kPumpDrainBudgetDefault = 8u;
constexpr uint32_t kPumpDrainBudgetMin = 1u;
constexpr uint32_t kPumpDrainBudgetMax = kPumpQueueCap;
// 超级赶路贴门抬升（AbsPos 更大 Y=更高）。末段 / recover / 发门带空悬停共用。
constexpr uint32_t kTravelPortalAimLiftDefault = 16u;
constexpr uint32_t kTravelPortalAimLiftMin = 4u;
constexpr uint32_t kTravelPortalAimLiftMax = 64u;

struct PayloadControl {
    uint32_t magic = kPayloadControlMagic;
    uint32_t version = kPayloadControlVersion;
    uint32_t invuln = 1;  // 默认开
    // v27/v38: 攻击加速（开=跳过动作等待；间隔默认见 kSimpleCombatAttackIntervalDefaultMs）
    // 首页入口已关（kAttackAccelUserEnabled）；「攻击无CD」见 attackAccelClearBusy（吸怪 TAB，未解锁不可用）。
    uint32_t attackAccel = 0;
    // 实验：周期写 LocalUser ActionBusy=-1（清引擎忙锁）；默认关。不绑首页 attackAccel。
    // 不是技能无 CD；仍受面板间隔 / 服端校验约束。
    uint32_t attackAccelClearBusy = 0;
    // 清忙锁开启时的出刀间隔地板（ms）；默认 200。不改写面板间隔落盘。
    uint32_t attackAccelClearBusyMinIntervalMs = kAttackAccelClearBusyMinIntervalDefaultMs;
    // v157: 调试 TAB 解绑「攻击无CD → 强制遇人三项」。缺键=仍绑定（0）。不改吸怪遇人强制。
    uint32_t attackNoCdEncounterUnbind = 0;
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
    uint32_t petSummon = 0;              // 用户入口已关（kPetSummonUserEnabled）；字段保留防旧 ini / 日后重开
    uint32_t petSummonRequireFood = 0;   // 同上；有粮才召
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
    // v124: 怪物刷新感知增强（实验 TAB）。MI 观察进/离场 → RequestImmediateScan；默认关。
    uint32_t mobPoolObserve = 0;
    // v40: 出刀按键 hold（调试 TAB）；默认 5，实际取 min(此值, 间隔)
    uint32_t simpleCombatAttackHoldMs = kAttackHoldDefaultMs;
    uint32_t clusterWeight = kClusterWeightDefault;  // 0=最近优先；非0=群怪优先
    // v83: 打中换怪（默认关）。开：本角色对同一 oid 确认命中 N 次后改打攻击盒外最近活怪；活怪<3 停刀。
    uint32_t simpleCombatHitRotate = kCombatHitRotateDefault;
    uint32_t simpleCombatHitRotateN = kCombatHitRotateNDefault;
    // v134/v137/v138: 不打 MISS 怪（默认开 / N=1）。开：本角色进盒连续 N 次 Damage=0 后换靶。
    uint32_t simpleCombatSkipAccMiss = kCombatSkipAccMissDefault;
    uint32_t simpleCombatSkipAccMissN = kCombatSkipAccMissNDefault;
    // v84: 出刀自组攻包。落盘 user.ini；会话文件只给面板↔DLL 热切换。
    uint32_t simpleCombatForgeHit = 0;
    // v127: 自组攻包钉锁攻击盒半宽/半高（AbsPos px；0=该轴不限）。不与站桩面前盒共用。
    uint32_t simpleCombatForgeHitFrontDx = kForgeHitFrontDxDefault;
    uint32_t simpleCombatForgeHitFrontDy = kForgeHitFrontDyDefault;
    // 已移除：填充列表 / 同拍多包 / 多怪。保留布局；读盘恒 1 / 0 / 0。
    uint32_t simpleCombatForgeHitMobs = kForgeHitMobsDefault;
    uint32_t simpleCombatForgeHitFillList = kForgeHitFillListDefault;
    uint32_t simpleCombatForgeHitMultiPkt = kForgeHitMultiPktDefault;
    // v85: 实验·全图攻击。会话态（state/map_attack_armed），不写入 user.ini；
    // 重启 launcher 归零。P1 只打 MapAtk 日志，不改 Rect/maxCount。
    uint32_t mapAttack = 0;
    // v90/v94: 吸怪。落盘 user.ini；默认关。勾选后周期把本地模拟怪吸到面前空点（不贴台）。
    uint32_t mobGather = 0;
    // v143: 0=A IMPACT，1=B FH-SNAP。缺键=A，不改现有 IMPACT。
    uint32_t mobGatherStrategy = kMobGatherStrategyDefault;
    // v144: 策略 A 子项「到站落地」。到站后松手让怪自然落台；默认 0（保持悬停）。
    uint32_t mobGatherLandOnArrive = kMobGatherLandOnArriveDefault;
    // v145: 远怪接力跳距 px/跳（0=关直拉）。单跳 ≤此值 + 到点驻留，绕开服务器 ~1200px 拉距掐线。
    uint32_t mobGatherHopPx = kMobGatherHopPxDefault;
    uint32_t mobGatherSpeedPct = kMobGatherSpeedPctDefault;
    uint32_t mobGatherAntiJitter = kMobGatherAntiJitterDefault;
    uint32_t mobGatherMax = kMobGatherMaxDefault;
    uint32_t mobGatherFarInFlight = kMobGatherFarInFlightDefault;
    uint32_t mobGatherRadiusPx = kMobGatherRadiusDefaultPx;
    uint32_t mobGatherHoldMs = kMobGatherHoldMsDefault;
    uint32_t mobGatherIntervalMs = kMobGatherIntervalDefaultMs;
    uint32_t mobGatherIgnoreQuiet = kMobGatherIgnoreQuietDefault;
    uint32_t mobGatherQuietDelayMs = kMobGatherQuietDelayMsDefault;
    uint32_t mobGatherStandOffCustom = kMobGatherStandOffCustomDefault;
    int32_t mobGatherStandOffX = kMobGatherStandOffXDefault;
    int32_t mobGatherStandOffY = kMobGatherStandOffYDefault;
    uint32_t mobGatherAimJitterPx = kMobGatherAimJitterDefault;
    uint32_t mobGatherStickCreepPx = kMobGatherStickCreepDefault;
    uint32_t mobGatherStickStillV = kMobGatherStickStillVDefault;
    uint32_t mobGatherCruiseR = kMobGatherCruiseRDefault;
    uint32_t mobGatherStationR = kMobGatherStationRDefault;
    uint32_t mobGatherMaxCmd = kMobGatherMaxCmdDefault;
    uint32_t mobGatherKp = kMobGatherKpDefault;
    // v141: 位移夹速。开=把被拽怪每帧位移夹到 ≤ mobGatherDispCapPx，规避「怪速+10」举报门。
    uint32_t mobGatherDispClampOn = kMobGatherDispClampOnDefault;
    uint32_t mobGatherDispCapPx = kMobGatherDispCapPxDefault;
    uint32_t mobGatherDead = kMobGatherDeadDefault;
    uint32_t mobGatherGravity = kMobGatherGravityDefault;
    uint32_t mobGatherCruiseV = kMobGatherCruiseVDefault;
    uint32_t mobGatherStationV = kMobGatherStationVDefault;
    uint32_t mobGatherHoldV = kMobGatherHoldVDefault;
    uint32_t mobGatherSettleErr = kMobGatherSettleErrDefault;
    uint32_t mobGatherKpSettle = kMobGatherKpSettleDefault;
    uint32_t mobGatherBrakeMs = kMobGatherBrakeMsDefault;
    uint32_t mobGatherCoastVy = kMobGatherCoastVyDefault;
    uint32_t mobGatherAimMs = kMobGatherAimMsDefault;
    // v104/v122: 主动软重连（首页挂机卡）。ini 键仍 mobGatherSoftRelogin*。不绑吸怪。
    // 追怪=瞬移找怪且 F5 开着时强制起表（不改本字段落盘）；卖装/赶路冻钟。
    uint32_t mobGatherSoftRelogin = kMobGatherSoftReloginDefault;
    uint32_t mobGatherSoftReloginSec = kMobGatherSoftReloginSecDefault;
    // v127/v142: 落地后累计出刀阈值。缺键厂默 1700。与秒数先到先拆。
    uint32_t mobGatherHangupFires = kMobGatherHangupFiresDefault;
    // v128: 出刀累计软重连独立勾选。0=关（秒数勾选仍可拆）。缺键厂默开。
    uint32_t mobGatherHangupFiresOn = kMobGatherHangupFiresOnDefault;
    // v131: 解除瞬移找怪+F5 强制开秒数闸。缺键关。秒数闸只跟首页勾选。
    uint32_t mobGatherHangupUnbindF5 = kMobGatherHangupUnbindF5Default;
    // v129: 启动器调试 TAB ws888 解锁。0=标题/顶栏不画刀数。不替代注册表真源。
    uint32_t gatherTabUnlocked = 0;
    uint32_t mobGatherClearRelogin = kMobGatherClearReloginDefault;
    // v100: 申请控制权。默认关。开：吸怪持有期泵上调官方 ApplyControl（不造包）。
    uint32_t mobGatherApplyCtrl = 0;
    // v155: 只吸场上的。默认开。开：勾上时场上已有 oid 才吸；拒之后图刷/分裂/召唤。
    uint32_t mobGatherFirstGenOnly = kMobGatherFirstGenOnlyDefault;
    // v110: 先飞到最密堆再吸。默认关。开：人飞到簇质心后再 Arm，寻路冻 14s。
    uint32_t mobGatherSeekCluster = kMobGatherSeekClusterDefault;
    // v146: 远怪自动巡点（寻簇跨层/全图版）。默认关。与 homeReturn 互斥。
    uint32_t mobGatherPatrolFar = kMobGatherPatrolFarDefault;
    // v147: 防举报（实验）。默认关。开=改 GA .text 让 Mob 永走正常移动包、不发 prevpos 举报。
    uint32_t mobGatherAntiReport = kMobGatherAntiReportDefault;
    // v111: 软重连后返回原位。默认关。与 seekCluster 互斥。
    uint32_t mobGatherHomeReturn = kMobGatherHomeReturnDefault;
    int32_t mobGatherHomeX = 0;
    int32_t mobGatherHomeY = 0;
    int32_t mobGatherHomeMapId = 0;
    uint32_t mobGatherHomeValid = 0;
    uint32_t mobGatherHomeHasMap = 0;
    // v149: 吸怪「重连换频」。0=关（默认）。开：hangup/被动软重连进异频。遇人 hop 已选频则不抢。
    uint32_t mobGatherReconnectHop = kMobGatherReconnectHopDefault;
    // v114: 竖层 px。寻簇同层窗。用户自填；缺键走厂默。0=合法。
    uint32_t mobGatherLayerYPx = kMobGatherLayerYPxDefault;
    // v115: 高度闸 px。新收 |dY| 上限。用户自填；缺键走厂默。0=合法。
    uint32_t mobGatherDyLimPx = kMobGatherDyLimPxDefault;
    // v117: 履历横移 px。用户自填；缺键走厂默。0=不挡横移。
    uint32_t mobGatherWalkDx = kMobGatherWalkDxDefault;
    // v117: 脚边 hypot。用户自填；缺键走厂默。0=不开豁免。
    uint32_t mobGatherFeetExemptPx = kMobGatherFeetExemptPxDefault;
    // fill+Doing 贴怪：F5 追怪「瞬移找怪」；默认关。
    uint32_t simpleCombatTeleport = 0;
    // v123: 瞬移找怪「每只怪打一下」。仅瞬移模式生效；默认关。
    uint32_t simpleCombatTeleportOneHit = kCombatTeleportOneHitDefault;
    // 空中贴怪（ini: simpleCombatAirApproach；旧键 ImpactApproach 读盘兜底）。
    uint32_t simpleCombatImpactApproach = 1;
    // v52: 空中贴怪飞行速度倍率（%）。100 = 基准 1.0X。仅在空中贴怪开启时生效。
    uint32_t simpleCombatFlySpeedPct = kHeliSpeedPctDefault;
    // 拟人位移：同层走路贴近后 A 键出刀；与空中贴怪互斥（空中开时恒 0）。
    uint32_t simpleCombatHumanWalk = 0;
    // 站桩输出：原地出刀 + 吸怪到身边。默认关；与空中贴怪/拟人互斥。
    uint32_t simpleCombatHiraishin = 0;
    // 站桩输出开打前原地站给吸物（ms）。0=不等（厂默）。缺键走 0；旧厂默 3000 读盘迁。
    uint32_t simpleCombatHiraishinLootHoldMs = kHiraishinLootHoldDefaultMs;
    // 站桩输出选怪距离上限（px，人↔怪 hypot）。0=吸怪圈。
    uint32_t simpleCombatHiraishinRangePx = kHiraishinRangeDefaultPx;
    // 站桩输出面前攻击盒半宽/半高（px）。0=该轴不限。
    uint32_t simpleCombatHiraishinFrontDx = kHiraishinFrontDxDefault;
    uint32_t simpleCombatHiraishinFrontDy = kHiraishinFrontDyDefault;
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
    // v20/v82/v133/v154/v156: 遇人策略 UX（UserPool + channel_hop，非 Reload）
    // v156 厂默：总开关开；先停手开；一直有人就换频开；GM/隐身升级开；遇人停吸开；隐藏玩家关
    uint32_t autoRelogin = 1;             // 检测同图玩家（总开关；默认开）
    uint32_t autoReloginStopCombat = 1;   // 先停手（普通遇人）
    uint32_t autoReloginReconnect = 1;    // 一直有人就换频（普通遇人）
    // v60: GM/隐身升级（Admin·Manager 或客户端隐身 → 立刻停手/换频 + 强制 Alarm；默认开）
    uint32_t autoReloginGmEscalate = 1;
    // v101/v154: 遇人停吸。厂默开。吸怪开启时仍强制打开（连同检测/换频），避免别人看见吸怪。
    uint32_t autoReloginStopGather = 1;
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
    uint32_t dropAlertBypass = 1;  // 默认开：开着会抑制客户端警戒
    // 野外可开拍卖：数据面强制 MapDataInfo.IsTown=1（仅客户端；默认开）。
    // 服端可能断线；与挂机「守护模式」叠加会干净重拉——挂机/守护时建议关。
    uint32_t auctionTownBypass = 1;
    // 实验 TAB 一次探针：主泵点官方状态栏拍卖按钮。用户入口已关（kAuctionGateProbeUserEnabled）。
    uint32_t auctionGateProbeSeq = 0;
    // 实验 TAB 一次：主泵打开 UICheat IMGUI GM overlay（CreateInstance+Open；不直调 OnGUI）。
    uint32_t uiCheatOverlaySeq = 0;
    // v76/v77: 实验·坐下/椅子回蓝（刷 WM 累加器；默认关）。BIN 已证真蓝会动；过密踢。
    uint32_t restMpAccel = 0;
    uint32_t restMpAccelIntervalMs = kRestMpAccelIntervalDefaultMs;
    // v130/v132：拦截 / .text 已移除。字段保留布局，恒 0。
    uint32_t secAttackIntercept = 0;
    uint32_t secAttackTextHook = 0;
    // v80/v81: 实验·无限飞镖。用户入口已关（kInfiniteStarsUserEnabled）；字段保留防旧 ini / 日后重开。
    uint32_t infiniteStars = 0;
    // v140: 实验·强制交易。用户入口已关（kForceTradeUserEnabled）；字段保留防旧 ini / 日后重开。
    uint32_t forceTrade = 0;
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
    // v113: 贴门抬升 px（调试 TAB「超级赶路」）。0 读盘当缺省 → 默认 16。
    uint32_t travelPortalAimLiftY = kTravelPortalAimLiftDefault;
    uint64_t writeTickMs = 0;
};

// 吸怪开着时强制：检测同图 + 遇人停吸 + 一直有人就换频。不改「先停手」。
inline void ApplyMobGatherEncounterForce(PayloadControl& c) {
    if (c.mobGather == 0) return;
    c.autoRelogin = 1;
    c.autoReloginReconnect = 1;
    c.autoReloginStopGather = 1;
}

// 「攻击无CD」开着时强制：检测同图 + 先停手 + 一直有人就换频。
// 不改遇人停吸 / GM 升级 / 隐藏玩家；关无CD 后不把这三项写回 0（与吸怪强制同口径）。
// attackNoCdEncounterUnbind=1（调试 TAB「解绑」）时跳过，遇人三项跟用户勾选。
inline void ApplyAttackNoCdEncounterForce(PayloadControl& c) {
    if (c.attackAccelClearBusy == 0) return;
    if (c.attackNoCdEncounterUnbind != 0) return;
    c.autoRelogin = 1;
    c.autoReloginStopCombat = 1;
    c.autoReloginReconnect = 1;
}

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

inline uint32_t ClampHiraishinLootHoldMs(uint32_t ms) {
    if (ms > kHiraishinLootHoldMaxMs) return kHiraishinLootHoldMaxMs;
    return ms;
}

inline uint32_t ClampHiraishinRangePx(uint32_t px) {
    if (px > kHiraishinRangeMaxPx) return kHiraishinRangeMaxPx;
    return px;
}

inline uint32_t ClampHiraishinFrontDx(uint32_t px) {
    if (px > kHiraishinFrontDxMax) return kHiraishinFrontDxMax;
    return px;
}

inline uint32_t ClampHiraishinFrontDy(uint32_t px) {
    if (px > kHiraishinFrontDyMax) return kHiraishinFrontDyMax;
    return px;
}

inline uint32_t ClampForgeHitFrontDx(uint32_t px) { return ClampHiraishinFrontDx(px); }
inline uint32_t ClampForgeHitFrontDy(uint32_t px) { return ClampHiraishinFrontDy(px); }

inline uint32_t ClampForgeHitMobs(uint32_t n) {
    if (n < kForgeHitMobsMin) return kForgeHitMobsMin;
    if (n > kForgeHitMobsMax) return kForgeHitMobsMax;
    return n;
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

inline uint32_t ClampMobGatherSpeedPct(uint32_t pct) { return ClampHeliSpeedPct(pct); }

inline uint32_t ClampMobGatherMax(uint32_t n) {
    if (n < kMobGatherMaxMin) return kMobGatherMaxMin;
    if (n > kMobGatherMaxMax) return kMobGatherMaxMax;
    return n;
}

inline uint32_t ClampMobGatherFarInFlight(uint32_t n) {
    if (n > kMobGatherFarInFlightMax) return kMobGatherFarInFlightMax;
    return n;
}

inline uint32_t ClampMobGatherHopPx(uint32_t px) {
    if (px == 0u) return 0u;  // 0=关（直拉）
    if (px < kMobGatherHopPxMin) return kMobGatherHopPxMin;
    if (px > kMobGatherHopPxMax) return kMobGatherHopPxMax;
    return px;
}

inline uint32_t ClampMobGatherRadiusPx(uint32_t px) {
    if (px < kMobGatherRadiusMinPx) return kMobGatherRadiusMinPx;
    if (px > kMobGatherRadiusMaxPx) return kMobGatherRadiusMaxPx;
    return px;
}

inline uint32_t ClampMobGatherLayerYPx(uint32_t px) {
    if (px > kMobGatherLayerYPxMax) return kMobGatherLayerYPxMax;
    return px;
}

inline uint32_t ClampMobGatherDyLimPx(uint32_t px) {
    if (px > kMobGatherDyLimPxMax) return kMobGatherDyLimPxMax;
    return px;
}

inline uint32_t ClampMobGatherWalkDx(uint32_t px) {
    if (px > kMobGatherWalkDxMax) return kMobGatherWalkDxMax;
    return px;
}

inline uint32_t ClampMobGatherFeetExemptPx(uint32_t px) {
    if (px > kMobGatherFeetExemptPxMax) return kMobGatherFeetExemptPxMax;
    return px;
}

inline uint32_t ClampMobGatherHoldMs(uint32_t ms) {
    if (ms < kMobGatherHoldMsMin) return kMobGatherHoldMsMin;
    if (ms > kMobGatherHoldMsMax) return kMobGatherHoldMsMax;
    return ms;
}

inline uint32_t ClampMobGatherIntervalMs(uint32_t ms) {
    if (ms < kMobGatherIntervalMinMs) return kMobGatherIntervalMinMs;
    if (ms > kMobGatherIntervalMaxMs) return kMobGatherIntervalMaxMs;
    return ms;
}

inline uint32_t ClampMobGatherQuietDelayMs(uint32_t ms) {
    if (ms > kMobGatherQuietDelayMsMax) return kMobGatherQuietDelayMsMax;
    return ms;
}

inline uint32_t ClampMobGatherU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline uint32_t ClampMobGatherStickCreep(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherStickCreepMin, kMobGatherStickCreepMax);
}
inline uint32_t ClampMobGatherStickStillV(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherStickStillVMin, kMobGatherStickStillVMax);
}
inline uint32_t ClampMobGatherCruiseR(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherCruiseRMin, kMobGatherCruiseRMax);
}
inline uint32_t ClampMobGatherStationR(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherStationRMin, kMobGatherStationRMax);
}
inline uint32_t ClampMobGatherMaxCmd(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherMaxCmdMin, kMobGatherMaxCmdMax);
}
inline uint32_t ClampMobGatherKp(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherKpMin, kMobGatherKpMax);
}
inline uint32_t ClampMobGatherDead(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherDeadMin, kMobGatherDeadMax);
}
inline uint32_t ClampMobGatherGravity(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherGravityMin, kMobGatherGravityMax);
}
inline uint32_t ClampMobGatherCruiseV(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherCruiseVMin, kMobGatherCruiseVMax);
}
inline uint32_t ClampMobGatherStationV(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherStationVMin, kMobGatherStationVMax);
}
inline uint32_t ClampMobGatherHoldV(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherHoldVMin, kMobGatherHoldVMax);
}
inline uint32_t ClampMobGatherSettleErr(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherSettleErrMin, kMobGatherSettleErrMax);
}
inline uint32_t ClampMobGatherKpSettle(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherKpSettleMin, kMobGatherKpSettleMax);
}
inline uint32_t ClampMobGatherBrakeMs(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherBrakeMsMin, kMobGatherBrakeMsMax);
}
inline uint32_t ClampMobGatherCoastVy(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherCoastVyMin, kMobGatherCoastVyMax);
}
inline uint32_t ClampMobGatherAimMs(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherAimMsMin, kMobGatherAimMsMax);
}
inline uint32_t ClampMobGatherSoftReloginSec(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherSoftReloginSecMin, kMobGatherSoftReloginSecMax);
}
inline uint32_t ClampMobGatherHangupFires(uint32_t v) {
    return ClampMobGatherU32(v, kMobGatherHangupFiresMin, kMobGatherHangupFiresMax);
}
inline int32_t ClampMobGatherStandOffX(int32_t x) {
    if (x < kMobGatherStandOffXMin) return kMobGatherStandOffXMin;
    if (x > kMobGatherStandOffXMax) return kMobGatherStandOffXMax;
    return x;
}
inline int32_t ClampMobGatherStandOffY(int32_t y) {
    if (y < kMobGatherStandOffYMin) return kMobGatherStandOffYMin;
    if (y > kMobGatherStandOffYMax) return kMobGatherStandOffYMax;
    return y;
}

inline uint32_t ClampMobGatherAimJitter(uint32_t px) {
    if (px > kMobGatherAimJitterMax) return kMobGatherAimJitterMax;
    return px;
}

inline uint32_t ClampMobGatherDispCapPx(uint32_t px) {
    if (px < kMobGatherDispCapPxMin) return kMobGatherDispCapPxMin;
    if (px > kMobGatherDispCapPxMax) return kMobGatherDispCapPxMax;
    return px;
}

inline uint32_t ClampMobGatherStrategy(uint32_t v) {
    if (v > kMobGatherStrategyFhSnap) return kMobGatherStrategyDefault;
    return v;
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

// Apply 用：面板间隔 +（可选）首页加速地板。清忙锁不再另抬间隔（v99 解除）。
inline uint32_t EffectiveAttackIntervalForApply(uint32_t panelMs, uint32_t attackAccel,
                                               uint32_t /*clearBusy*/,
                                               uint32_t /*clearBusyMinMs*/) {
    return EffectiveSimpleCombatAttackIntervalMs(panelMs, attackAccel);
}

inline uint32_t ClampCombatHitRotateN(uint32_t n) {
    if (n < kCombatHitRotateNMin) return kCombatHitRotateNMin;
    if (n > kCombatHitRotateNMax) return kCombatHitRotateNMax;
    return n;
}

inline uint32_t ClampCombatSkipAccMissN(uint32_t n) {
    if (n < kCombatSkipAccMissNMin) return kCombatSkipAccMissNMin;
    if (n > kCombatSkipAccMissNMax) return kCombatSkipAccMissNMax;
    return n;
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
    return v;
}
inline bool IsRetiredCombatTeleportMaxHopDefault(uint32_t v) {
    return v == kCombatTeleportMaxHopLegacyDefault || v == kCombatTeleportMaxHopPrevDefault ||
           v == kCombatTeleportMaxHopPrevDefault2 || v == kCombatTeleportMaxHopPrevDefault3;
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

inline uint32_t ClampTravelPortalAimLiftY(uint32_t v) {
    if (v < kTravelPortalAimLiftMin) return kTravelPortalAimLiftMin;
    if (v > kTravelPortalAimLiftMax) return kTravelPortalAimLiftMax;
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
// 出刀自组攻包会话文件：兼容旧会话；正式开关已落盘 user.ini，启动不再清零。
void ClearForgeHitSession(const char* binDir);
// 全图攻击会话态：清零（launcher 启动）。不落盘。
void ClearMapAttackSession(const char* binDir);
// 吸怪会话文件：兼容旧会话；正式开关已落盘 user.ini，启动不再清零。
void ClearMobGatherSession(const char* binDir);
// 伪造攻包 oneshot seq：清零（launcher 启动）。不写 user.ini。
void ClearAttackRpcFireSeq(const char* binDir);
uint32_t ReadAttackRpcFireSeq(const char* binDir);
bool WriteAttackRpcFireSeq(const char* binDir, uint32_t seq);
// 清零攻包探针 session cap：bump seq；payload 读到后 ResetSessionCap。不写 user.ini。
void ClearAttackRpcResetSeq(const char* binDir);
uint32_t ReadAttackRpcResetSeq(const char* binDir);
bool WriteAttackRpcResetSeq(const char* binDir, uint32_t seq);
// auto_stop 勾灭：payload 写 seq，面板读到后把勾选关掉并写回 user.ini。
void ClearAttackRpcStopSeq(const char* binDir);
uint32_t ReadAttackRpcStopSeq(const char* binDir);
bool WriteAttackRpcStopSeq(const char* binDir, uint32_t seq);
// 吸怪 oneshot seq：清零（launcher 启动）。不写 user.ini。
void ClearMobGatherFireSeq(const char* binDir);
uint32_t ReadMobGatherFireSeq(const char* binDir);
bool WriteMobGatherFireSeq(const char* binDir, uint32_t seq);
// 吸怪高度闸扫描 seq：调试按钮 1→+100/s→2000。不写 user.ini。
void ClearMobGatherDyRampSeq(const char* binDir);
uint32_t ReadMobGatherDyRampSeq(const char* binDir);
bool WriteMobGatherDyRampSeq(const char* binDir, uint32_t seq);
// 吸怪「记录人物坐标」oneshot seq。不写 user.ini；DLL 记 AbsPos 后再落盘 home 字段。
void ClearMobGatherHomeRecordSeq(const char* binDir);
uint32_t ReadMobGatherHomeRecordSeq(const char* binDir);
bool WriteMobGatherHomeRecordSeq(const char* binDir, uint32_t seq);

}  // namespace xcat
