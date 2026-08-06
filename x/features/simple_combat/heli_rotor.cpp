// heli_rotor — F5 Impact 贴怪旋翼环实现。设计说明见 heli_rotor.h。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "heli_rotor.h"

#include "../invuln/invuln.h"
#include "../ports/map_bounds_port.h"
#include "../ports/skill_port.h"
#include "../ports/teleport_port.h"
#include "../../runtime/main_thread_pump.h"

#include <atomic>
#include <cmath>

namespace x::features::simple_combat::heli {
namespace {

// 发射节奏：~11Hz。每发一次 = 一个主线程泵 job，太密会拖帧；太疏则两拍之间重力
// 累积的落差变大（90ms = 3 个物理步 = 180px/s 锯齿 ≈ 8px 位置起伏，可接受）。
constexpr DWORD kIssueMs = 90;
// 紧急档要按 tick 率走（旋翼 ~55Hz）。已出界还在往外飞时，每被节奏闸拦一拍就多滑
// 一个 v*Δt：BIN 1ce9a0 里连拦两拍 40ms、以 545px/s 多滑 22px，那正是剩余的全部出界深度。
// 紧急是短促的几拍，按 tick 发不构成持续高频。
constexpr DWORD kIssueEmergencyMs = 20;

// ── 实测物理常量（见头文件 ②③⑤）────────────────────────────────────
constexpr float kGravityPerStep = 60.f;  // px/s，每个物理步
constexpr float kPhysicsStepMs = 30.f;
// 配平窗口上限。主线程长卡顿后 sinceMs 可能是几秒，按实算会发出一发火箭把角色
// 顶穿图顶；超过这个窗口就认了这段下坠，靠包线保护去救，别用一发巨量冲量赌。
constexpr float kMaxTrimWindowMs = 400.f;

// 死区 = 稳态位置残差的上界。控制律在死区内不产生位移意图，只维持速度 0，所以角色
// 会**停在进入死区的那一点**——而进近总是从上方降下来的，残差就一边倒地偏高。
// BIN 14a58c：死区 30 时中位残差 +18px，叠加当时的 +14 抬升 = 稳定悬在怪上方 32px 砍空气。
//
// 纵向曾经取 30 是怕「怪与角色同台时把站定的角色从台上踹飞」（BIN 79947e）。那个
// 隐患现在由 `onFh` 分支兜住（挂台时 cmdVy 强制 0），不再需要靠宽死区代偿；而 fh-ban
// 悬空时根本没有台可踹。收窄到与横向同级即可，锯齿残余约 4px，不会在死区边缘来回抖。
constexpr float kDeadX = 12.f;
constexpr float kDeadY = 12.f;

// P 增益（px/s per px）。两轴的 P 都只算**期望速度**，真正要发的增量由闭环补差（见 Tick）。
constexpr float kKpX = 4.0f;
constexpr float kKpY = 3.0f;

// 包线：越过即 emergency，忽略战斗意图先保命。判定框见 heli_rotor.h 的 kEnvSlackPx
// （**外扩** AABB；曾经内缩 72px，把最外沿台上的怪判成禁区，BIN 4a79e4）。
constexpr float kMaxFallVy = 460.f;  // 落速超此 → 弃战拉平
constexpr float kEnvPushVx = 300.f;
// 救援目标速度（不是增量）。闭环会自己算出「从当前 -600 拉到 +300」需要多大增量。
constexpr float kRescueClimbVy = 300.f;

// 作动器上限：一次能发出的**增量**幅值。它必须大到能把最坏落速一次拉正
// （-600 → +300 需要 900 + 预付重力），否则紧急救援在算术上就不成立。
constexpr float kMaxCmdVy = 900.f;
// 横向同理。反向要发的增量是 |vt| + |v|，最坏 340 + 560 = 900；给到 1200 留余量。
// 这个数**不是**限速：落地速度恒等于 desiredVx（受 caps.vx 约束），钳位只咬命令幅值。
// 曾经把它设成 caps.vx，等于把「从 -556 反向到 +300」需要的 856 削成 340，
// 结果一发只能把速度推到 -256（人还在往外飞），连发四拍才反向 → 出界 43px（BIN c72cff）。
constexpr float kMaxCmdVx = 1200.f;

// 把「想要的速度」翻译成「该发的增量」。作动器实测语义（头文件事实①，11/11 样本吻合）：
//     v_new = clamp(v_old + cmd, ±max(|v_old|, |cmd|))
// 关键在那个 max：**引擎不允许一发冲量把你降到比原速更慢**。所以「同向发个小值来减速」
// 是彻底的空操作——`-557 + (-208) = -765` 会被钳回 max(557,208) = 557，速度纹丝不动。
//
// 于是只有一种情况能直接发 vt，其余一律发差值：
//   · 同向**加速**（|vt| > |v|）：发 vt。v+vt 越过 |vt| 被钳回，落地正好 vt。
//   · 其余（同向减速 / 反向 / 刹停）：发 vt - v。此时 max(|v|, |vt-v|) ≥ |vt|，钳位不咬，
//     落地精确等于 vt。vt=0 时它自然退化成 -v，即一发刹停，不需要额外的特例。
float VxCommandFor(float vt, float v) {
    if (vt * v > 0.f && std::fabs(vt) > std::fabs(v)) return vt;
    return vt - v;
}

// 深度出界：超过安全框这么多就判不可救，缴械让引擎接管（见 Bailed()）。
constexpr float kBailoutPx = 420.f;
// 位置与速度连续这么多拍完全不变 = 状态停更（断线/切图/死亡）。
constexpr int kStaleTicksLimit = 12;

// 每档的**意图**上限，不是作动器上限。下降速度必须低于「一个发射周期内能抵消的量」，
// 否则一旦进入下降就再也拉不平——bea1c3 的 -300 俯冲指令就是这么把人送出图的。
struct ModeCaps {
    float vx;
    float climb;    // 期望上升速度上限
    float descend;  // 期望下降速度上限（正数）
};

ModeCaps CapsFor(Mode m) {
    switch (m) {
        case Mode::Cruise:
            return {560.f, 420.f, 240.f};
        case Mode::Rtb:
            return {480.f, 460.f, 200.f};
        case Mode::Station:
            return {340.f, 300.f, 200.f};
        case Mode::Hold:
            return {260.f, 260.f, 160.f};
        default:
            return {0.f, 0.f, 0.f};
    }
}

// 两次发射之间重力吃掉的速度 = 盈亏线。用**真实耗时**而非标称周期：主线程一拥塞
// 发射就变稀，需要的配平同比变大；写死常数正是历史上净下沉的根源。
float GravityLoss(DWORD sinceMs) {
    float ms = static_cast<float>(sinceMs);
    if (ms < kPhysicsStepMs) ms = kPhysicsStepMs;
    if (ms > kMaxTrimWindowMs) ms = kMaxTrimWindowMs;
    return kGravityPerStep * ms / kPhysicsStepMs;
}

std::atomic<unsigned> gMode{static_cast<unsigned>(Mode::Off)};
std::atomic<float> gSpX{0.f};
std::atomic<float> gSpY{0.f};

DWORD gLastIssueMs = 0;
bool gLastTickFired = false;

std::atomic<bool> gBailed{false};

// 状态停更检测：断线/切图后 QueryFlightState 会一直回同一组死数据，此时任何冲量
// 都是空发。bea1c3 断线后旋翼又对着冻结的 (932,-1188) 发了 500ms。
float gStaleX = 0.f, gStaleY = 0.f, gStaleVx = 0.f, gStaleVy = 0.f;
int gStaleTicks = 0;

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 返回 true = 本拍数据与上一拍逐字节相同，已连续超过阈值。
bool UpdateStaleness(const ports::teleport::FlightState& st) {
    if (st.x == gStaleX && st.y == gStaleY && st.vx == gStaleVx && st.vy == gStaleVy) {
        if (gStaleTicks < kStaleTicksLimit) ++gStaleTicks;
    } else {
        gStaleTicks = 0;
        gStaleX = st.x;
        gStaleY = st.y;
        gStaleVx = st.vx;
        gStaleVy = st.vy;
    }
    return gStaleTicks >= kStaleTicksLimit;
}

}  // namespace

bool Bailed() { return gBailed.load(std::memory_order_acquire); }

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Hold:
            return "hold";
        case Mode::Station:
            return "station";
        case Mode::Cruise:
            return "cruise";
        case Mode::Rtb:
            return "rtb";
        default:
            return "off";
    }
}

void SetSetpoint(const Setpoint& sp) {
    gSpX.store(sp.x, std::memory_order_release);
    gSpY.store(sp.y, std::memory_order_release);
    gMode.store(static_cast<unsigned>(sp.mode), std::memory_order_release);
}

Setpoint CurrentSetpoint() {
    Setpoint sp{};
    sp.mode = static_cast<Mode>(gMode.load(std::memory_order_acquire));
    sp.x = gSpX.load(std::memory_order_acquire);
    sp.y = gSpY.load(std::memory_order_acquire);
    return sp;
}

void Disarm() { SetSetpoint(Setpoint{}); }

void Reset() {
    Disarm();
    gLastIssueMs = 0;
    gLastTickFired = false;
    gBailed.store(false, std::memory_order_release);
    gStaleTicks = 0;
}

bool Tick(DWORD now, Telemetry* out) {
    Telemetry tm{};
    const Setpoint sp = CurrentSetpoint();
    tm.mode = sp.mode;
    tm.spX = sp.x;
    tm.spY = sp.y;

    // 状态查询放在 mode 闸之前：Bailed 后飞控会立刻 Disarm，若查询在 Off 之后就永远
    // 看不到「已落地」这个复位条件，缴械状态会卡死到下一次换图。
    ports::teleport::FlightState st{};
    const bool haveState = ports::teleport::QueryFlightState(st) && st.ok;
    if (haveState) {
        tm.haveState = true;
        tm.x = st.x;
        tm.y = st.y;
        tm.vx = st.vx;
        tm.vy = st.vy;
        tm.onFh = st.onFh;
        if (st.onFh) {  // 引擎把人接住了 = 脱险，可重新武装
            gBailed.store(false, std::memory_order_release);
            gStaleTicks = 0;
        }
    }

    if (sp.mode == Mode::Off) {
        tm.guard = "off";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (!haveState) {
        tm.guard = "no_state";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (Bailed()) {
        tm.guard = "bailed";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (UpdateStaleness(st)) {  // 断线/切图：再发也只是对着死数据空转
        gBailed.store(true, std::memory_order_release);
        tm.guard = "stale";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }

    const ModeCaps caps = CapsFor(sp.mode);
    const DWORD sinceMs = gLastIssueMs ? (now - gLastIssueMs) : static_cast<DWORD>(kIssueMs);
    const float trim = GravityLoss(sinceMs);
    tm.sinceMs = sinceMs;
    tm.trimVy = trim;

    // 两轴都先算「这一周期想要的速度」，作动器增量最后统一换算。
    float desiredVx = 0.f;
    const float errX = sp.x - st.x;
    const float errY = sp.y - st.y;
    if (std::fabs(errX) > kDeadX) desiredVx = kKpX * errX;

    // 竖直：先定「这一周期想要的平均速度」，作动器指令另算。+Y 向上 ⇒ errY>0 = 往上。
    float desiredVy = 0.f;
    if (!st.onFh && std::fabs(errY) > kDeadY) desiredVy = kKpY * errY;

    // ── 安全包线 ────────────────────────────────────────────────────
    bool emergency = false;
    if (!st.onFh && st.vy < -kMaxFallVy) {  // 高速下坠：弃战拉平
        desiredVy = kRescueClimbVy;
        emergency = true;
    }
    ports::map_bounds::Rect r{};
    if (ports::map_bounds::QueryPlayBounds(0, &r) && r.ok) {
        const float rawL = static_cast<float>(r.left);
        const float rawR = static_cast<float>(r.right);
        // 外扩，不是内缩：AABB 内一律合法（含最外沿的台）。见 heli_rotor.h 的 kEnvSlackPx。
        const float l = rawL - kEnvSlackPx;
        const float ri = rawR + kEnvSlackPx;
        const float t = static_cast<float>(r.top) - kEnvSlackPx;
        const float b = static_cast<float>(r.bottom) + kEnvSlackPx;
        if (ri > l && b > t) {
            if (st.x < l) {
                desiredVx = kEnvPushVx;
                emergency = true;
            } else if (st.x > ri) {
                desiredVx = -kEnvPushVx;
                emergency = true;
            }
            // Rect 的 top/bottom 是**数值**含义（top=min y）；+Y 向上 ⇒ top 才是图底。
            if (st.y < t) {  // 已跌破最低台：这是唯一会掉出地图的方向，全力上拉
                desiredVy = kRescueClimbVy;
                emergency = true;
                if (st.y < t - kBailoutPx) {
                    // 掉这么深已经不是控制问题，冲量也追不回来。缴械让引擎接管落地/复位，
                    // 别像 bea1c3 那样对着注定的结局继续空发到断线。
                    gBailed.store(true, std::memory_order_release);
                    tm.guard = "bailout";
                    gLastTickFired = false;
                    if (out) *out = tm;
                    return false;
                }
            } else if (st.y > b) {
                // 已在最高台之上：下降是免费的，给个受控下沉速度而不是放任自由落体。
                desiredVy = -caps.descend;
                emergency = true;
            }

            // ── 撞墙预刹 ────────────────────────────────────────────
            // X 是**设速度**语义：这一拍发多少，接下来整个发射周期就以多少速度平移。
            // 于是只要让「本周期位移」落在 AABB 内，就不可能冲出边界——从源头消灭过冲。
            // 事后靠 RTB 拉回是追不上的：kEnvPushVx=300 反不过 448 的入射速度。
            //
            // BIN c9b8dc：怪在最左台，角色带 vx=-448 扑过去，AABB 左沿 -585 却冲到 -658
            //（出界 73px）并在界外继续出刀，服务端判非法坐标，一轮掉线两次。
            //
            // 视野取 150ms 而非标称的 90ms：主线程一卡顿发射就变稀（实测 since 到过
            // 106ms），按标称算会刹不住。早刹几十毫秒不影响出刀，冲出去要掉线。
            // 视野按「一次错过的发射」给足：紧急档 45ms，卡顿时实测能拖到 106ms，取 200ms。
            constexpr float kBrakeHorizonSec = 0.200f;
            const float roomR = (rawR - st.x) / kBrakeHorizonSec;  // 允许的最大 +vx
            const float roomL = (rawL - st.x) / kBrakeHorizonSec;  // 允许的最小 vx（界内为负）
            // 已在界外时 room 同号，这两句会把意图顶成内推，与紧急推同向且力度更足。
            if (desiredVx > roomR) desiredVx = roomR;
            if (desiredVx < roomL) desiredVx = roomL;
        }
    }
    tm.emergency = emergency;

    // 意图限幅：下降速度必须低于「一个发射周期内能抵消掉的量」，否则一旦下沉就再也
    // 拉不平。bea1c3 的 sp.y 指到下层怪 → desiredVy 算出 -864 → 直接送出图。
    desiredVy = Clamp(desiredVy, -caps.descend, caps.climb);
    tm.desiredVy = desiredVy;

    // 叠加语义 ⇒ 发的是**增量**：把当前速度补到目标，再预付本周期的重力损耗。
    //
    // 目标是让**周期平均**速度等于 desiredVy（平均为 0 才是真不漂）。设发射前速度 vp、
    // 周期 N 步，冲量落地那一帧也吃一步重力（头文件事实③），于是周期内速度依次为
    // vp+cmd-60, vp+cmd-120, …, vp+cmd-60N，平均 = vp + cmd - (trim/2 + 30)。
    // 令其等于 desiredVy 即得下式。末尾那个 kGravityPerStep/2 就是"落地帧那一步"，
    // 漏掉它稳态会稳定下沉 30px/s —— 十秒 300px，正是过去"看着在悬停却越飘越低"的量级。
    const float feedforward = trim * 0.5f + kGravityPerStep * 0.5f;
    float cmdVy = st.onFh ? 0.f : (desiredVy - st.vy) + feedforward;

    // 限幅落在**意图速度**上（这才是落地速度），再换算成增量。反过来钳增量会把反向
    // 所需的 |vt|+|v| 削掉，那正是 c72cff 里刹不住的原因。
    desiredVx = Clamp(desiredVx, -caps.vx, caps.vx);
    tm.desiredVx = desiredVx;
    float cmdVx = VxCommandFor(desiredVx, st.vx);

    // 闸门之前就落盘：被闸掉的拍也要能在日志里看出「本来想发多少」，否则
    // cadence 行一律显示 cmd=(0,0)，排查时会被误读成「控制律算出了 0」。
    cmdVx = Clamp(cmdVx, -kMaxCmdVx, kMaxCmdVx);
    cmdVy = Clamp(cmdVy, -kMaxCmdVy, kMaxCmdVy);
    tm.cmdVx = cmdVx;
    tm.cmdVy = cmdVy;

    // ── 闸门 ─────────────────────────────────────────────────────────
    // emergency 只让过节奏闸与技能前置闸；无敌闸不能绕（Impact 端口自身也会拒）。
    if (!x::features::invuln::IsEnabled()) {
        tm.guard = "invuln_off";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (!emergency) {
        // 拥塞闸只在**挂着台**时生效：脚下有地板，少发几拍无非是站着不动。
        // 悬空时反过来——拥塞正是最需要旋翼续命的时刻，此时让闸拦下就是自由落体，
        // 而卡顿又恰好是历史掉图的放大器。改由 trim 按真实耗时补偿这段空窗。
        if (st.onFh && x::runtime::main_thread::IsCongested()) {
            tm.guard = "congested";
            gLastTickFired = false;
            if (out) *out = tm;
            return false;
        }
        int prepSkill = 0;
        if (ports::skill::IsPreparingSkill(&prepSkill)) {
            tm.guard = "skill_prep";
            gLastTickFired = false;
            if (out) *out = tm;
            return false;
        }
    }
    const DWORD cadence = emergency ? kIssueEmergencyMs : kIssueMs;
    if (gLastIssueMs && (now - gLastIssueMs) < cadence) {
        tm.guard = "cadence";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }

    if (!emergency && std::fabs(cmdVx) < 8.f && std::fabs(cmdVy) < 8.f) {
        tm.guard = "deadband";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }

    ports::teleport::ImpactVelOpts vopts{};
    // 端口这层只做防呆兜底。按 caps.vx 收窄会削掉反向所需的大增量（同 cmdVy 那条注释）；
    // 真正的限速是上面的 desiredVx——落地速度恒等于它，与命令幅值无关。
    vopts.maxAbsVx = kMaxCmdVx;
    // 端口这一层只做防呆兜底，真正的意图限幅在上面的 desiredVy；这里再按档位收窄
    // 会把「从 -600 拉平」需要的那一发大增量削掉，等于恢复历史上的净下沉。
    vopts.maxAbsVy = kMaxCmdVy;
    vopts.minAbs = 6.f;  // 叠加语义下写 0 是空操作，跳过即可省一个泵 job
    vopts.quietLog = true;
    const bool ok = ports::teleport::ImpactSetVelocity(
        cmdVx, cmdVy, ports::teleport::ImpactRoute::SetImpactNext, vopts);
    if (!ok) {
        tm.guard = "impact_fail";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    gLastIssueMs = now;
    gLastTickFired = true;
    tm.fired = true;
    if (out) *out = tm;
    return true;
}

}  // namespace x::features::simple_combat::heli
