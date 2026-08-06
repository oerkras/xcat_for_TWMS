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
// ④ **上升没有引擎封顶。** 历史结论「合速钳在 [-670,+110]」与「爬升上限 150」都是
//    **错的**——那是把本模块自己的 caps.vy 当成了引擎特性（150 = cmd 210 减一帧重力）。
//    已实测 cmd=300 整值落地（dvy=+240）。300 以上未测，别反向臆断成"无限"。
//
// ⑤ 由 ②③ 直接推出**唯一要记住的算术**：两次发射间隔 T ms，重力吃掉 `60*T/30 = 2T`
//    px/s。**单次发射量低于 2T 就是净下沉**，与 Kp、死区、模式全都无关。
//    历史事故全部栽在这条上：`kHoverVy=90`（90ms 周期只给了半油门）、紧急上拉
//    `kEnvPushVy=120`（低于 180 盈亏线）⇒ 越"救"越掉，bea1c3 从 y=-270 一路掉到
//    -1188 后被服务端断线。所以配平必须由**距上次发射的真实耗时**算出来，而不是
//    写死常数——主线程一卡顿，发射变稀，需要的配平就同比变大。

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
};

// 出界判定余量：**向外扩**，不是向内缩。
//
// ⚠️ FH AABB 就是「可站区域」——怪站在最外沿那条台上是常态，不是危险。历史上 A 层包线、
// B 层 RTB、站位点夹取三处都用 `kLandMarginPx + 48 = 72` 向内缩，等于把 AABB 最外一圈
// 判成禁区；而最低/最高台上的怪恰好全在那一圈里，站位点被顶开约 50~72px，永远进不了
// ±35 的出刀带 → 每只怪 2600ms 进近超时换靶。现象就是「头顶和脚下的怪都打不中，
// 只有地图中段能命中」（BIN 4a79e4，连续四个目标全部超时）。
//
// 那个 72 是从**瞬移落点**的安全内缩（`kLandMarginPx`）借来的，语义不通用：落点要避开
// 边界，悬停点不用——怪的坐标本身就是该处合法的证据。
//
// 现在统一为：**AABB 之内一律合法，超出这么多才算真出界**。三处必须用同一个数，
// 否则两层会互相打架（一层想下去、另一层判紧急往上拉）。
// 真·坠落另有 `vy < -kMaxFallVy` 的速度闸兜底，不依赖位置阈值。
//
// 这个数只留一点点抖动余量，不当缓冲区用：界外没有台也没有怪，多待一拍都是在赌
// 服务端的坐标校验。留 48 时实测冲出 73px 并在界外继续出刀，一轮掉线两次
//（BIN c9b8dc）。真正防过冲的是 A 层的撞墙预刹，不是把这个数放大。
constexpr float kEnvSlackPx = 8.f;

// RTB 把角色拉回到 AABB 内缩这么多的位置，而不是刚好贴着边沿。纯粹是迟滞：
// 拉到边沿上会立刻又落进「出界」判定，模式在 rtb/station 之间抖。
// 只有 24px，远小于 ±35 的出刀带，所以边沿台上的怪照样够得着。
constexpr float kEnvReturnInsetPx = 24.f;

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

void SetSetpoint(const Setpoint& sp);
Setpoint CurrentSetpoint();
void Disarm();  // = SetSetpoint({Mode::Off})

// 每个 combat tick 都调（内部自控 ~11Hz 发射节奏，不必外部限频）。
// 返回 true = 本 tick 真发了冲量。out 可为空。
bool Tick(DWORD now, Telemetry* out);

// F5 开关/换图时清发射时钟与符号自检状态。
void Reset();

const char* ModeName(Mode m);

}  // namespace x::features::simple_combat::heli
