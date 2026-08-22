#pragma once
// heli_rotor — F5 Impact 贴怪的「旋翼环」（近战直升机 A 层）
//
// 为什么单独一层：Impact 冲量原来只在 State::MoveTo 里发，一进 Firing 整个出刀窗口
// 零冲量；而 fh-ban armed 时引擎持续加重力且不许挂台 —— 于是出刀就等于自由落体，
// 几百 ms 后已经掉到怪下方甚至掉出图底。旋翼必须与打怪状态机彻底解耦、永不停转。
//
// 分层（详见 docs/features/simple_combat/模块设计.md）：
//   A 旋翼（本文件）：读 Ap 位置+速度 → 横向 P 控 / 纵向闭环配平 + 安全包线 → 冲量
//   B 飞控（simple_combat FSM）：只发布 setpoint + 模式
//   C 武器（simple_combat Firing）：进命中带就砍，出刀期间旋翼照转
//
// ★ 竖直物理模型（BIN bea1c3 · keypad_walk_bin.log 逐帧速度采样实测，勿凭直觉改）
//
// ① **+Y 向上。** `foothold_path.cpp:37` 实机记录：掉到**下层** fh60@(786,**-1463**)，
//    下层 y 更负。（同文件第 24 行那句「MS +Y 向下」是错的，别再引用。）
//
// ② **重力每物理步固定 -60 px/s，步长 30ms** ⇒ g = 2000 px/s²，与当前速度无关。
//    bea1c3 全程 1128 个样本里 `dvy=-60` 出现 315 次，没有第二个众数。
//
// ③ **两轴都是叠加语义**，整值落地；竖直落地那一帧同时吃一步重力：
//    实测 `-99 + 210 - 60 = +51` ✓、`-69 + 210 - 60 = +81` ✓。写 0 = 空操作。
//
//    横向还多一层**钳位**，完整式子是（c72cff + 1ce9a0 共 11 组样本全中）：
//        v_new = clamp(v_old + cmd, ±max(|v_old|, |cmd|))
//    那个 `max` 是重点：**引擎不允许一发冲量把你降到比原速更慢**。
//        反向：232+(-300)=-68 ✓｜-325+300=-24 ✓｜-556+300=-255 ✓｜-255+300=45 ✓｜544+(-844)=-300 ✓
//        同向加速：-68+(-300)=-368 → 钳 300 → -300 ✓｜45+300=345 → 钳 300 → 300 ✓｜0+(-560)=-560 ✓
//        同向减速：-557+(-208)=-765 → 钳 **557** → 仍是 -557（**指令被无视**）✓
//                  -557+(-89) 同理，速度纹丝不动 ✓
//
//    ⚠️ ~~「X 是设速度（cmd=-340 → vx=-339）」~~ 是**错的**，别再照抄。那条结论取自
//    静止起步的样本，而 `0 + cmd` 与 `set(cmd)` 在静止态完全同形，根本区分不了两个模型。
//    照它写出的控制律在**高速时全废**：想减速发小值 → 被 `max` 钳成空操作，速度不变；
//    想反向发 +300 而 v=-556 → 落地 -256，人继续往外飞。这就是历轮出界的成因。
//    换算见 `VxCommandFor()`：除同向加速外一律发 `vt - v`，各工况都能一发到位。
//
// ④ **竖直在 ±900 指令内没有引擎封顶，且落地线性。** 历史结论「合速钳在 [-670,+110]」
//    与「爬升上限 150」都是**错的**——那是把本模块自己的 caps.vy 当成了引擎特性
//    （150 = cmd 210 减一帧重力）。
//    实测（全归档 20363 条遥测 / `Dumps/runtime/_vy_actuator.py`）：
//      · 指令 |cmdVy| 实际发到过 **900**；
//      · 实际速度上升达 **+840**、下降达 **-670**，两个方向都没有平台；
//      · 单发落地验证的残差全是 60 的整数倍（= 步数估计差一帧），叠加式精确成立。
//    900 以上仍未测；kMaxCmdVy=1200 是同一线性段内的外推，不是新机制，但也别当已验。
//
// ⑤ 由 ②③ 直接推出**唯一要记住的算术**：两次发射间隔 T ms，重力吃掉 `60*T/30 = 2T`
//    px/s。**单次发射量低于 2T 就是净下沉**，与 Kp、死区、模式全都无关。
//    历史事故全部栽在这条上：`kHoverVy=90`（90ms 周期只给了半油门）、紧急上拉
//    `kEnvPushVy=120`（低于 180 盈亏线）⇒ 越"救"越掉，bea1c3 从 y=-270 一路掉到
//    -1188 后被服务端断线。所以配平必须由**距上次发射的真实耗时**算出来，而不是
//    写死常数——主线程一卡顿，发射变稀，需要的配平就同比变大。
//
// 进站律 / 当前 IDB 的 ApplyImpact RVA / 客户 0.32 vs 本机 0.15 BIN：
//   docs/features/simple_combat/满火力进站与竖直权限.md
//   （模块设计.md §4.0 ②③ 过期句以本头文件 + 该笔记为准）

#include <Windows.h>

namespace x::features::simple_combat::heli {

enum class Mode : unsigned {
    Off = 0,      // 不发冲量（F5 关 / Impact 关 / 暂停 / 已落地）
    Hold = 1,     // 无目标：钉住 setpoint 原地悬停（等换目标或等落地宽限到点）
    Station = 2,  // 有目标：怪旁站位悬停，柔速强收敛
    Cruise = 3,   // 远距转场：放宽水平速度
    Rtb = 4,      // 出界/超包线：拉回安全点，忽略战斗意图
};

struct Setpoint {
    Mode mode = Mode::Off;
    float x = 0.f;
    float y = 0.f;
    // 目标点自身的移动速度（px/s），**前馈**项，不参与反馈。
    //
    // 纯比例控制跟移动目标必然留稳态滞后：desiredV = Kp·err ⇒ err = V目标/Kp。Kp=7 时
    // 光标扫到 800px/s 就永久落后 114px（BIN 752824 实测中位数正是 114）—— 这就是 F6
    // 「不跟手」的全部来源，与档位上限无关（当时意图速度 792，两档 1440/1860 都没咬住）。
    // 把目标速度直接前馈进意图，稳态滞后即归零，且因为是开环项不动闭环极点、不带来振荡。
    //
    // 只有「目标在动且知道它多快」的驱动方才该填：F6 跟随鼠标填光标世界速度；F5 打怪
    // 留 0 —— 它的 setpoint 是在怪之间**跳变**的，差分出来的不是速度而是跳变毛刺。
    float leadVx = 0.f;
    float leadVy = 0.f;

    // 自由空域：放弃「把角色关在 AABB⊕slack 里」的那套包线。给**手动驾驶**用。
    //
    // 为什么要有这个开关：包线是为**自动**打怪设计的——那时没人盯着，角色必须自己别飞丢。
    // F6 是用户拿鼠标在开，包线就成了跟驾驶员抢方向盘：他指哪儿、机器往回顶哪儿。
    // 何况 AABB 只是**可站立面**的包围盒，从来就不等于合法空域（见下方 kEnvSlackYPx 长注），
    // 拿它当围栏对手动飞是结构性过严。
    //
    // 放开的：左右内推、上沿下沉、以及这三个方向的撞墙预刹。它们之外都是纯空气，
    // 飞出去最坏什么也不发生。
    //
    // ★ **不放开向下**：穿出最低台是四个方向里唯一会毁掉会话的（引擎判掉图 → 重载/断线，
    // 就是 a69130 野猪图那次「越界重拉」）。所以 `st.y < t` 的上拉、底侧预刹与
    // `kBailoutPx` 深度缴械对所有模式恒生效——这不是限制驾驶员，是拦住唯一那扇会摔死的门。
    bool unbounded = false;
};

// ★ 合法空域 = FH AABB ⊕ 近战够怪半径。两轴的余量**不同**，别再合并成一个数。
//
// 概念前提（`map_bounds_port.cpp` 取的是全部 foothold 的包围盒，`src="fh"`）：
// **AABB 是「可站立面」的范围，不是「合法空域」的范围。** 站在最低那块台上的角色，
// 脚下坐标天然就**等于** `Rect.top`；怪也一样。所以「越过 AABB = 出界」这个判据对贴地
// 打怪的飞行角色是结构性误判 —— 不是余量给少了，是拿站立面当空域用了。
//
// 合法空域必须包含「够到站在某个面上的怪所必需的位置」，而那个半径来自本模块自己的
// 战斗常量、**与地图无关**（这正是它能全类型地图适用的原因，不需要逐图调参）：
//     竖直 = 出刀带 kHeliFireMaxDy(35) + 悬停死区 kDeadY(12) ≈ 48
//
// 竖直放宽的实证（BIN b3d1bd，M0002 跨 15 张图 16 分钟）：
//   · 反推各图边界 = rtb 拉回目标 − kEnvReturnInsetPx，与怪的最小 Y 差恒为 21~24px
//     ⇒ AABB 下沿就压在最低那排台上，怪就站在那儿
//   · 110020000 整图 40 只怪 **100%** 贴最低台；110040000 亦 100%；110030000 60%
//     ⇒ 这不是「边界怪」，是普通地面层。跳过它们等于整张图不打
//   · 实测越线深度：中位 **3px**、最大 32px、100% 落在 35px 出刀带内 —— 纯误判
//   · 代价：970 次 oob_hold 禁刀 + 73 次 RTB 把角色从怪身边拽走 24px + 18 次进近超时
//
// 横向**不跟着放**：同一份日志里横向 RTB 只有 4 次（竖直 73 次），而横向过冲才是真正
// 撞掉线的那条 —— 留 48 时实测冲出 73px 并在界外继续出刀，一轮掉线两次（BIN c9b8dc）。
// 那次的根因是作动器模型算错（已由 `VxCommandFor` 修正），但横向没有「必须飞到怪那一
// 高度」的几何刚需，没有理由陪着放宽。真正防横向过冲的是 A 层的撞墙预刹。
//
// 两个数各自在三处（A 层包线 / B 层 `NeedsHeliRtb` / 出刀硬闸 `PlayerOutOfPlayBounds`）
// 必须一致，否则两层互相打架：一层想下去、另一层判紧急往上拉。
// 真·坠落另有 `vy < -kMaxFallVy` 的**速度**闸与 `kBailoutPx` 兜底，不依赖这两个位置阈值。
//
// 更早的反面教材：三处曾用 `kLandMarginPx + 48 = 72` 向**内缩**，把 AABB 最外一圈判成
// 禁区，最低/最高台上的怪站位点被顶开 50~72px，永远进不了出刀带 → 每只怪进近超时换靶，
// 现象是「头顶和脚下的怪都打不中，只有地图中段能命中」（BIN 4a79e4）。那个 72 借自
// **瞬移落点**的安全内缩，语义不通用：落点要避开边界，悬停点不用。
constexpr float kEnvSlackXPx = 8.f;
constexpr float kEnvSlackYPx = 48.f;

// RTB 把角色拉回到 AABB 内缩这么多的位置，而不是刚好贴着边沿。纯粹是迟滞：
// 拉到边沿上会立刻又落进「出界」判定，模式在 rtb/station 之间抖。
// 只有 24px，远小于 ±35 的出刀带，所以边沿台上的怪照样够得着。
constexpr float kEnvReturnInsetPx = 24.f;

// 撞墙预刹的**横向**刹车线，从 AABB 左右沿各自内缩这么多。
//
// 为什么需要：`ClampToAirspace` 允许 setpoint 外扩到 rawR + kEnvSlackXPx，预刹却按 rawR
// 收，于是「setpoint 往外拉 vs 预刹往里顶」的稳态解就是**紧贴 rawR**。BIN bf5f9f 实测
// 14:02:11 与 14:02:27 两次停在 x=1369（101030001 rawR=1373，净余量 4px，vx 已被刹到 0，
// setpoint 分别是 1376 / 1381）。判据上没出界，但 4px 等于没有余量：发射一稀疏就出去了。
// 跳到最右台的野猪（tpl 2230102，同图实测最远 x=1364）能把这个状态稳定复现。
//
// 与 4a79e4 那次事故的区别 —— 那次内缩的是 **setpoint**，站位点被顶出出刀带，边沿台上的
// 怪永远够不着；这里内缩的是**预刹的速度钳位线**，setpoint 照旧指到怪身上，只是人停在
// 离墙 24px 处出刀。横向出刀带 kHeliFireMaxDx = 120：怪就算站在 rawR 上，从 rawR−24 出刀
// dx 也只有 24，离禁飞还差 96px，几何上零代价。
//
// 只作用在 X 轴。竖直出刀带只有 ±45，且最低/最高台上站着怪是常态，内缩就直接出带——
// 那正是 4a79e4 的翻车方式，别对称化。
//
// 取值与 kEnvReturnInsetPx 同为 24：RTB 也是拉回内缩 24 的位置，两处口径一致，
// 免得预刹停在 A、RTB 又拽到 B 互相拉扯。
constexpr float kBrakeInsetXPx = 24.f;

// 撞墙预刹的**竖直**刹车线内缩量。与 X 同为 24，理由同构，但成因值得单独记一笔。
//
// ★ 这里修掉的是一类结构性缺陷：**「目标点的钳位线」与「紧急机动的触发线」不可以是同一条**。
// `ClampToAirspace` 把 setpoint 夹到 b，而 Tick 里 `st.y > b` 就是紧急下压的触发条件 ——
// 于是「指到图外」的稳态解是**贴着绊线悬停**，过冲 1px 就吃一发满档下压，砸下去几十 px
// 再被 P 项拽回来，周而复始。这不是调参问题，是 bang-bang 控制器被要求停在自己的开关上。
//
// BIN a0ab58 实测（map 101030000，B=345 ⇒ b=393）：34 个遥测样本里 25 个 tgt.y 恰好 =393；
// send.log 10Hz 轨迹显示平时保持冲量只有 ±124（90ms 的重力配平），却周期性穿插
// vy=-419 的下压，随后以 -599/-659 砸落 45~65px。用户看到的就是「到点后掉落又吸附」。
//
// 为什么内缩**预刹线**而不是钳位线：内缩 setpoint 正是 4a79e4 的翻车方式（站位点被顶出
// ±45 出刀带，边沿台上的怪永远够不着）。内缩预刹线则不动 setpoint，只让人**停在**离绊线
// 24px 处 —— 与 X 轴 kBrakeInsetXPx 的取舍完全同构，且几何上同样免费：
//   · b − 24 = rawB + 24，怪站最高台（rawB）时 dy=24 ≤ kHeliFireMaxDy(45)，在带内 ✓
//   · t + 24 = rawT − 24，怪站最低台（rawT）时 dy=24 ≤ 45，同样在带内 ✓
// 也就是说这 24px 全部花在 AABB 之外的纯余量壳层里，战斗几何一分未动。
//
// 副产品：两条紧急触发线从此恒在停靠线之外 24px，等于白拿一段迟滞。近界区间交还给
// 预刹这个**连续**控制器（roomB/roomT 本就随距离线性收速），紧急分支退回它该待的位置 ——
// 只处理真异常，而不是参与日常闭环。
constexpr float kBrakeInsetYPx = 24.f;

// 越过空域上沿后的受控下沉速度（F6 / 非交战）。Combat/Gather 不走顶侧紧急，见 Tick。
//
// 原实现用 `-caps.speed`，与另外三个方向（kEnvPushVx / kRescueClimbVy 都是固定 300）不对称，
// 而且会**跟着用户的速度倍率放大**：1X 是 480，5X 就是 2400。可上沿之外是纯空气，
// 「回来」本身没有紧迫性，用户把倍率调高更不该换来更猛的下砸。固定成与其余方向同一口径。
constexpr float kEnvSinkVy = 300.f;

// 把一个 setpoint 夹进合法空域（AABB ⊕ 两轴 slack，口径同 A 层包线）。
//
// 必须夹：A 层包线作用在**玩家实际位置**上，不看 setpoint。指到图外的话，就变成
// 「包线往回推 vs setpoint 往外拉」的边界震荡——这是抖动类事故的经典成因。
//
// ⚠️ 只许**外扩**、绝不许向内缩。曾用 `kLandMarginPx + 48 = 72` 内缩，把 AABB 最外一圈
// 判成禁区，最低/最高台上的怪全在那一圈里 → 站位点被顶开永远进不了出刀带（BIN 4a79e4）。
// 那个 72 借自**瞬移落点**的安全内缩，语义不通用：落点要避边界，悬停点不用。
//
// 无 bounds 数据时原样返回 true（宁可不夹也不误杀）。
// F6 等仍可用本函数；**F5 Combat 可位移区见 ClampToCombatMoveBounds（raw L/R）**。
bool ClampToAirspace(float* x, float* y);

// 仅 F5 自动打怪可位移区：左右 = raw FH AABB（不再中心 ×0.95）。
// 站位 / RTB / oob_hold / Combat 预刹必须同框。0.95 曾把站位钉在工作框沿、RTB 再往里
// 收一截，左缘怪够不着出刀 AABB（BIN 21:16：框 L=-368、RTB sp=-320、怪 x=-414、dx=94>73）。
// 穿墙改由 kBrakeInsetXPx=24 + 满火力 X 预刹 90ms 管，勿再叠一层假墙。
// ★ **只约束左右**。竖直不夹不闸——BIN 10:24 站位被钉死 `sp.y=图底×0.95`（如 -607）。
//   真下穿图底仍由 A 层 raw±slack 的恒生效上拉/bailout 保命。
// **不含** Owner::Travel（超级赶路贴边门，走 raw±slack）。
// ★ 回退：改回 0.95f（恢复 21:16 前的假墙；贴边怪会再打不着）。
constexpr float kCombatMoveBoundsScale = 1.0f;

// 写出 move 框（L/R=raw×scale；T/B 仍写出供诊断，业务竖直勿当闸）。
// 成功且非退化返回 true；无 bounds / 非法 raw 返回 false（调用方应放行）。
bool QueryCombatMoveBounds(float* left, float* top, float* right, float* bottom);

// 点是否在可位移**左右**框内（忽略 Y）。无 bounds 时返回 true（不误杀）。
bool PointInCombatMoveBounds(float x, float y);

// 把点的 **X** 夹进可位移左右框的墙刹线（raw ± kBrakeInsetXPx）；Y 原样。
// 无 bounds 时原样返回 true。
bool ClampToCombatMoveBounds(float* x, float* y);

// 一次 tick 的遥测。由 simple_combat 写进 combat.log（BIN 分析都在那张日志里）。
struct Telemetry {
    bool haveState = false;
    Mode mode = Mode::Off;
    float x = 0.f, y = 0.f;
    float vx = 0.f, vy = 0.f;
    float spX = 0.f, spY = 0.f;
    float cmdVx = 0.f, cmdVy = 0.f;
    bool fired = false;
    bool emergency = false;  // 触发包线保护（超速下坠 / 越界）
    bool onFh = false;
    const char* guard = "";  // 未发射原因："" = 已发射
    float desiredVx = 0.f;   // 本拍想要的横向速度。落地速度应精确等于它，cmdVx 只是增量
    float desiredVy = 0.f;   // 本拍想要的平均竖直速度（+ = 上升）
    float trimVy = 0.f;      // 按真实间隔算出的重力预付量。它就是「盈亏线」
    DWORD sinceMs = 0;       // 距上次发射的真实耗时。trimVy = 2 * sinceMs
};

// 已判定不可救（状态停更 / 深度出界）。飞控据此卸 fh-ban 让引擎接管落地，
// 别再对着一个死掉的状态空发冲量（bea1c3 断线后仍发了 500ms）。
bool Bailed();
// 清缴械闩。断线/切图会 Release 掉 owner，而 Tick 在 !Owns 时早退、永远看不到 onFh，
// 残留 Bailed 会让 SyncImpactFhBan 永久拒绝 TryAcquire（upload 7848f4：软重连后
// rotor inbound 但无 heli mode、连发 heli_timeout）。
void ClearBailed();

// ── 所有权 ────────────────────────────────────────────────────────────
// 旋翼是**单例**：一份 setpoint、一份发射时钟。驱动方：F5 打怪 / 赶路 / F6 手动飞 /
// 吸怪寻簇（可选，默认不占）。各方各写各的就是抢方向盘。
//
// ★ 硬闸时 `Tick` 先 GoIdle 再 TickHeliRotor：停的是贴怪 setpoint，旋翼仍转（落台 / fh-ban）。
//   未硬闸的提前 return（skill_prepare / arm_grace / 池未热身）仍必须先转旋翼，停一拍就是掉一段。
//   也就是说旋翼**必须一直转**，能换的只有 setpoint 的来源 —— 所以要的是交接，不是暂停。
//
// 交接语义：`Acquire` 抢占式，后来者直接接管（手动 F6 压过自动，符合直觉）。被抢走的
// 一方 `SetSetpoint`/`Tick` 静默变空操作，它自己的循环不用改，也不会把 fh-ban 拆掉。
// 释放后下一个 `Acquire` 立刻能接回来，中间旋翼一拍没停。
enum class Owner : unsigned { None = 0, Combat, Travel, Fly, Gather };

// 抢占式接管，恒成功。给**手动**入口用（F6）：人按了键就该立刻听人的。
// 返回值只表示「是否发生了易主」，用于打日志。
bool Acquire(Owner o);

// 仅当旋翼无主时接管。给**自动**入口用（打怪 / 赶路），每 tick 调都行，很便宜。
//
// ★ 自动方必须用这个而不是 Acquire，否则 F6 抢过去的下一拍就被抢回来，两边对拽。
// ★ 也必须**每 tick 都调**而不是只在武装时调一次：F6 释放后旋翼变无主，若自动方
//   不主动接回来，fh-ban 还挂着而没人发冲量 = 自由落体。
bool TryAcquire(Owner o);
// 交还。仅当当前持有者就是 o 时才生效，避免把别人的所有权误释放。
void Release(Owner o);
Owner CurrentOwner();
const char* OwnerName(Owner o);

// 下面两个都要带 owner：不是持有者时静默 no-op。这是把「三方互斥」从口头约定
// 变成代码事实的唯一位置，别加无 owner 的重载。
void SetSetpoint(Owner o, const Setpoint& sp);
Setpoint CurrentSetpoint();
void Disarm(Owner o);  // = SetSetpoint(o, {Mode::Off})；持有者会清发射时钟

// 每个驱动 tick 都调（内部自控 ~11Hz 发射节奏，不必外部限频）。
// 返回 true = 本 tick 真发了冲量。out 可为空。非持有者恒返回 false。
bool Tick(Owner o, DWORD now, Telemetry* out);

// F5 开关/换图时清发射时钟与符号自检状态。
void Reset();

// 到位软悬停（16ms 加密 + 软钉 Y）。与面板「防抖」同开同关：关=回 90ms/进近死区，
// 避免只关钉点、软钉仍开导致「勾不勾都几乎不抖」。默认开。
void SetSoftSettleEnabled(bool on);
bool SoftSettleEnabled();

// 飞行速度倍率。1.0 = 基准（Cruise 620 / Rtb 660 / Station 480 / Hold 360）。
//
// 只作用于 `CapsFor()` 给出的「意图」上限，**不碰**下面这些：
//   · 作动器上限 kMaxCmdVx/Vy —— 硬件面，倍率再高也越不过去（它同时也是最高可达速度）
//   · 撞墙预刹、位置包线、深度缴械 —— 判据是位置与时间，与速度无关。跟着倍率缩会让
//     低倍率反而更容易掉出图（救援权限被一起削），这是反直觉但必须守住的边界。
//   · Rtb 档取 `max(基准, 基准×倍率)` —— 只许更快、不许更慢。自救不该因为用户
//     想慢点打怪就变弱。
// 唯一**必须**跟随倍率的是落速闸 `FallGateVy()`：它是「想快降 vs 失控」的判别式而非
// 绝对阈值，钉死会在高倍率下反号成极限环（BIN 2d6176 抖动事故）。
//
// 越界会被 Clamp 到 [kSpeedScaleMin, kSpeedScaleMax]。上限由**可救性**反解
// （kIntentCeilV / Rtb 基准，约 11X；面板顶到 1000%=10X），不是手写常数——曾经这里
// 写死 3.0、面板写 500，结果实机只跑 3.00X（BIN 2e63d5）。改动上限请改 kMaxCmdVy。
//
// ★ 面板拉到 **1000%（10.0X）** 时：
//   1) Cruise/Station 死拍 desiredV = err/T（远距顶满；近距按距离收油）
//   2) 档位意图顶到 kIntentCeilV（≈7410）；Rtb 同步抬
//   3) Travel/Fly 可选对站点预刹（kFullFireApproachBrake，0.15s）
//   4) Combat/Gather 满火力默认不对站点预刹（kFullFireCombatApproachBrake=false）
//   5) 换靶极限合速（kFullFireCombatLimitDash）：进近 16ms 发拍，剩余 > cap·T 时
//      矢量顶满 ≈7410，末段死拍。撞墙预刹视野 2 拍；墙框内沿航向缩合速。
//      竖速超出意图档时先卸战斗 X（BIN 19:00 紧急分轴横推+砸落），紧急不再走分轴钳。
//      出刀 dy 带内卸竖速只发一拍，随后锁过物理步（BIN 19:40 连发泵抖）。
//      Combat 不走顶侧紧急 sink（BIN 19:47 起飞过冲顶蹦床）；底侧上拉与左右 raw 预刹仍在。
// 其它倍率仍是 Kp·err + 分档限速。撞墙预刹 / 包线 / 可达集不撤。
//
// 倍率**按 Owner 各存一份**：手动飞和自动打怪对手感的诉求不同（F6 旧实现等效约
// 1600 px/s，比 F5 的 Cruise 620 快得多，共用一个旋钮必然有一方别扭）。
// 生效的是**当前持有者**那一份，交接时自动跟着换，不需要谁去存档/恢复。
void SetSpeedScale(Owner o, float scale);
float SpeedScale(Owner o);

const char* ModeName(Mode m);

}  // namespace x::features::simple_combat::heli
