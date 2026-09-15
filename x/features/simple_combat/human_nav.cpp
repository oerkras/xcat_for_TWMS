#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "human_nav.h"

#include "../ports/attack_input_port.h"
#include "../ports/foothold_path.h"
#include "../ports/foothold_port.h"
#include "../ports/mob_pool_port.h"
#include "../ports/nav_memory.h"
#include "../ports/teleport_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../ui/player_vitals.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace x::features::simple_combat::human_nav {
namespace {

using ports::foothold_path::EdgeKind;
using ports::foothold_path::FirstAction;
using ports::foothold_path::WalkAhead;

// ───────── 调参（集中放；每条阈值标出处） ─────────
constexpr float kWxTol = 20.f;           // 到路点
constexpr float kGrabWx = 6.f;           // 竖直跳抓绳窗：偏 13px 连跳 12 次都抓不上（BIN 12:29）
constexpr float kFloorGrabWx = 12.f;     // 落地梯：站住 ↑ 即可
constexpr float kFloorGrabTightWx = 4.f; // 12px 内 1.5s 没上绳 → 收到这么近再试
constexpr float kApproachSpotWx = 18.f;  // 走到绳下多近算到
constexpr float kAirSteerStopWx = 8.f;   // 空中贴到绳前多近停左右
constexpr float kNearRopeUpWx = 40.f;    // 贴绳途中就按住 ↑
constexpr float kEdgeJumpPx = 10.f;      // 台外绳：离台沿多近起跳（绳离台沿太远时的兜底）
// 台外绳的起跳点按「顶点时刻到绳」算，不是「到台沿再跳」。绳只在**下落段**抓得住：upload 2026-09-09
// 16:00（0CABDEGD）三次成功抓绳（16:00:56 台外 / 16:01:05、16:02:49 竖直）全在起跳后 ≈300ms、
// 高 77~78px 的顶点处 onRope，上升段从不抓；两次失手（16:00:38 / 16:00:47）起跳点离绳 23 / 26px，
// 顶点前就以 125px/s 飞过绳（顶点时已过绳 14 / 11px），空中反向只减速 ≈170px/s²，拉不回来，
// 掉到下面一层再绕 9s 一轮。抓绳窗实测 ≈±8~11px（过绳 7.5px 抓住、11.5px 没抓住）。
// 起跳距离 = |vx|×顶点时间 + 提前量：顶点时人在绳前 2~10px（30Hz 一拍 4px + 键延迟 4px 的量化），
// 下落第一帧起就在窗里，按住方向以 125px/s 穿过 ±8 窗还有 ≈4 帧。
constexpr DWORD kJumpApexMs = 306;        // kJumpV0 525 / g 1714（构图同一套常量）
constexpr float kWalkVx = 125.f;          // 地面跑满速（BIN 全程 vx=±125）
constexpr float kEdgeGrabLeadPx = 10.f;   // 顶点时留在绳前的余量
constexpr float kEdgeGrabRunUpDx = 90.f;  // 台外绳离绳这么近就进助跑（起跳判定在助跑态里）
constexpr float kEdgeGrabBackupPx = 14.f; // 比「满速起跳点」还近这么多以内 → 先退开再冲
constexpr float kEdgeGrabMinLaunchDx = 26.f;  // 慢速也最迟在这起跳（再近必过绳）
constexpr float kGrabMissBelowPx = 24.f;  // 悬空且低于绳底这么多 = 没抓住（AbsPos 更大 Y = 更高）
constexpr int kGrabMaxMisses = 3;         // 同一条跳抓边失手掉到别的台 3 次 → 死边
constexpr float kSettleVx = 45.f;        // 停稳
constexpr float kRunVx = 100.f;          // 助跑到此速度再起跳
constexpr float kKnockVx = 220.f;        // 击退判定（走路 125 不算）
constexpr float kBlockedVx = 15.f;
constexpr float kBlockedDx = 4.f;
constexpr float kWaitDx = 140.f;         // 目标朝自己走来且在此距离内 → 停步等
constexpr float kWaitTgtVx = 40.f;
constexpr float kWaitDy = 24.f;          // 与「同一块地面」口径一致（台阶上层的怪不等）
constexpr float kStepUpDy = 12.f;        // 楼梯台阶：目标台更高多少才算要跳
constexpr float kJumpReachPx = 84.f;     // 一跳能上的高差上限（起跳 vy≈470、滞空≈600ms → 顶点≈73）
constexpr float kClimbOffDy = 2.f;
constexpr float kVertYDead = 12.f;
constexpr float kEspJumpMinRemain = 22.f;

constexpr DWORD kDirMinHoldMs = 220;   // 方向最短保持：跳后 300ms 就回头（BIN 12:15）
constexpr DWORD kReactMinMs = 90;      // 换新目标的反应延迟
constexpr DWORD kReactMaxMs = 220;
constexpr DWORD kReactIdleMs = 400;    // 距上次按键超过此值才算「新开走」
constexpr DWORD kReplanMs = 1500;      // 走路中复核规划
constexpr DWORD kSettleMs = 150;
constexpr DWORD kSettleGiveUpMs = 400;
constexpr DWORD kRunUpMaxMs = 420;
constexpr DWORD kJumpAirMaxMs = 1500;
constexpr DWORD kAcrossTimeoutMs = 8000;   // 极限跳：走到起跳点 + 一跳 + 落地
constexpr float kAcrossTriggerPx = 6.f;    // 离起跳点这么近就按跳（30Hz 一拍走 4px，别冲过崖沿）
constexpr float kAcrossMinLaunchVx = 60.f; // 到点时朝前至少这么快才起跳，否则退回去助跑
constexpr int kAcrossBackUpPx = 40;        // 助跑距离
constexpr int kAcrossMaxTries = 3;         // 落回原台 3 次 = 这条边跳不过去
constexpr float kJumpUpTolPx = 10.f;       // 跳上头顶台：离起跳 X 这么近算到（另须在上台 X 范围内）
constexpr float kJumpUpMaxVx = 15.f;       // 起跳前横速上限（0.6s 滞空漂 ≤9px）
constexpr float kRopeJumpYTol = 6.f;       // 爬到起跳 Y 的容差
constexpr float kRopeGrabXTol = 14.f;      // 飞到目标绳：X 进这个范围且 onRope 才算抓住
constexpr DWORD kJumpLandMinMs = 120;  // 起跳后至少这么久才认落地（同拍还挂台）
constexpr DWORD kJumpPulseMs = 90;
constexpr DWORD kEspJumpCdMs = 380;
// 赶路跳怪起跳窗（按怪自己的横速分三种，见 MobAheadToDodge）：一跳只飞 76px。
constexpr float kDodgeMobDxMin = 30.f;     // 迎面来的怪：下限；上限按怪速算（36 + |rel|·0.61），夹在 [44, 64]
constexpr float kDodgeMobDxMax = 64.f;
constexpr float kDodgeFlightSec = 0.612f;  // 同高一跳的滞空 2·v0/g
constexpr float kDodgeStillDxMin = 34.f;   // 站着的怪：落点 76 要越过它半身 28 + 余量；再近起跳时已贴身
constexpr float kDodgeStillDxMax = 44.f;
constexpr float kDodgeMovingVx = 15.f;     // |怪速| 低于此算站着（怪走速 20~120）
constexpr float kDodgeMobDy = 50.f;        // 同层窗；AbsPos 更大 Y = 更高
constexpr DWORD kBlockedMs = 900;      // 顶住多久算卡
constexpr DWORD kEarlyRelatchMs = 300; // 起步按住这么久还没动就先补一次边沿（键被吞）
constexpr DWORD kReverseStepMinMs = 200;  // 卡住退一步的时长范围（每次抽；固定值是签名）
constexpr DWORD kReverseStepMaxMs = 340;
constexpr DWORD kWaitMaxMs = 900;
constexpr DWORD kWaitRearmMs = 1500;
// 等怪要有迟滞：目标横速是按怪位置差分的 EMA，一拍没动就翻成「掉头」——upload 2026-09-09 16:02
// 6 次 Wait 有 4 次 45~56ms 就 tgt_turned，走路键 Stop→Hold 抖一下、1.5s 内还不许再等。
// 进：连续 ≥60ms 判「朝我来」；退：按 |dx| 有没有在缩（不看速度），260ms 没缩才算掉头。
constexpr DWORD kWaitEnterConfirmMs = 60;
constexpr DWORD kWaitStallMs = 260;
constexpr float kWaitProgressPx = 3.f;
constexpr DWORD kGrabRetryMs = 800;
constexpr DWORD kNudgeMs = 45;         // 对齐微步按键时长
constexpr DWORD kNudgeSettleMs = 140;  // 微步后等横速消
constexpr DWORD kRopeSureMs = 700;     // 起跳后仍悬空 → 已在绳上（一跳约 600ms）
constexpr DWORD kFloorGrabWaitMs = 1500;
constexpr DWORD kClimbYStallMs = 900;
constexpr DWORD kClimbTimeoutMs = 14000;
constexpr DWORD kGrabTimeoutMs = 9000;
constexpr DWORD kHopOffGapMs = 450;   // 第一下没脱离（仍 onRope）多久补第二下
constexpr DWORD kHopOffLeadMs = 60;   // 进 HopOff 后先松 ↓ + 按实方向 60~100ms（每次抽）再按跳
constexpr DWORD kHopOffTimeoutMs = 3000;
constexpr DWORD kFallRetryMs = 420;
constexpr DWORD kFallTimeoutMs = 5000;
constexpr DWORD kFallWalkOffTimeoutMs = 9000;
constexpr DWORD kRecoverMaxMs = 1200;
constexpr DWORD kWxTimeoutMs = 7000;
constexpr DWORD kWxTimeoutCapMs = 25000;
constexpr DWORD kStuckJumpWindowMs = 4000;
constexpr DWORD kSenseLogMs = 1000;
constexpr DWORD kLoopWindowMs = 6000;    // 同一条边 6s 内第 3 次开工 = 来回摆
constexpr DWORD kForceSkipRearmMs = 10000;  // 无视 hop skip 兜底规划的最小间隔
constexpr float kOnTopDx = 10.f;         // 与调用方 kMinLandAway 同口径：贴怪心不算可打
constexpr float kFidgetMinDx = 160.f;    // 离怪比这远才允许顿一下
constexpr DWORD kFidgetMinMs = 150;
constexpr DWORD kFidgetMaxMs = 320;
constexpr DWORD kFidgetGapMinMs = 4000;
constexpr DWORD kFidgetGapMaxMs = 9000;
constexpr float kSidestepDx = 22.f;      // 叠在怪身上时退到这么远再打
constexpr DWORD kHopOffWalkMs = 600;     // 绳顶先 ↑+方向走上去，这么久没上去才补跳
constexpr DWORD kPostFireLockMs = 950;   // 出刀后动画锁：走路键无效，别当顶住 / 别数微步
constexpr DWORD kKnockStunMs = 700;      // 被击退落地后硬直：同上
constexpr int kHopOffRoomPx = 48;        // 下绳两侧都有这么多台面才按目标方向选
constexpr int kGrabMaxAttempts = 3;
constexpr int kAlignMaxNudges = 6;
constexpr int kDropMaxNudges = 10;     // 穿台点微步预算
constexpr int kMaxStuckJumps = 2;
constexpr int kDropSafeSearchPx = 48;    // 穿台点不在安全带时两侧找多远
constexpr DWORD kDropProneMs = 140;      // 穿台：↓ 先按住这么久再 Alt
// 下绳：低于顶台面这么多才允许侧跳脱离。绳上侧跳只抬 ≈17px（BIN 13:55:26 y=-154 起跳、0.21s 后
// -138 且 vy=+38 → 顶点 ≈+17），40px 余量顶点仍在顶台面下 20px 以上，不会落回顶台；下绳 100px/s，
// 60→40 少爬 0.2s，用户看到的「先往下爬一段」更短。
constexpr float kDismountBelowTopPx = 40.f;
constexpr float kDismountMinDropPx = 80.f;   // 离底不足此值直接滑到底
constexpr float kDismountProbeDx = 28.f;     // 侧跳落点横向探测偏移

// ───────── 日志 ─────────
void LogNav(const char* fmt, ...) {
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    x::runtime::LogI("SimpleCombat", "%s", body);

    char buf[1000];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    (void)x::runtime::AppendDbgLogA(dir, "combat.log", buf, (DWORD)n);
}

const char* KindName(EdgeKind k) {
    switch (k) {
        case EdgeKind::Walk:
            return "walk";
        case EdgeKind::ClimbUp:
            return "climb_up";
        case EdgeKind::ClimbDown:
            return "climb_down";
        case EdgeKind::FallDown:
            return "fall";
        case EdgeKind::JumpAcross:
            return "jump_across";
        case EdgeKind::RopeJump:
            return "rope_jump";
        case EdgeKind::JumpUp:
            return "jump_up";
        default:
            return "?";
    }
}

int Sign(float v) { return v < 0.f ? -1 : 1; }

// MoveAction 是梯（14/15）或绳（16/17）。18 是死亡躺尸，19 未见，都不算。
bool IsRopeMa(int ma) { return ma >= 14 && ma <= 17; }

// 确定性抖动（反应延迟用），不引 <random>。
DWORD Jitter(DWORD seed, DWORD lo, DWORD hi) {
    if (hi <= lo) return lo;
    uint32_t x = seed * 1664525u + 1013904223u;
    x ^= x >> 13;
    return lo + (x % (hi - lo + 1));
}

// ───────── 感知 ─────────
struct Sense {
    DWORD now = 0;
    float x = 0.f, y = 0.f, vx = 0.f, vy = 0.f;
    bool stOk = false;
    bool onFh = false;
    int ma = -1;
    uint32_t cur = 0;       // PeekCurFhId
    bool grounded = false;  // onFh && cur
    bool onRope = false;    // 悬空且 MoveAction 是绳梯
    uint32_t pfh = 0;       // 规划用玩家 FH
    bool tOk = false;       // 目标站点解析成功
    float tx = 0.f, ty = 0.f;
    uint32_t tfh = 0;
    float dx = 0.f, dy = 0.f;  // 目标 - 自己
    float tvx = 0.f;           // 目标横速 EMA（px/s）
    int rel = 0;               // +1 远离 / -1 靠近 / 0 基本不动
    bool travelPortal = false;
};

struct TargetTrack {
    bool ok = false;
    float x = 0.f;
    DWORD ms = 0;
    float vx = 0.f;
};
TargetTrack gTgt;

void TrackTarget(Sense& s) {
    if (!std::isfinite(s.tx)) {
        gTgt = {};
        return;
    }
    if (gTgt.ok && gTgt.ms) {
        const DWORD dt = s.now - gTgt.ms;
        const float jump = std::fabs(s.tx - gTgt.x);
        if (dt >= 10 && dt <= 400 && jump < 200.f) {
            const float inst = (s.tx - gTgt.x) * 1000.f / static_cast<float>(dt);
            gTgt.vx = gTgt.vx * 0.6f + inst * 0.4f;
        } else if (dt > 400 || jump >= 200.f) {
            gTgt.vx = 0.f;
        } else {
            // dt 太小：保留上一拍估计
        }
    }
    gTgt.ok = true;
    gTgt.x = s.tx;
    gTgt.ms = s.now;
    s.tvx = gTgt.vx;
    if (std::fabs(s.tvx) < kWaitTgtVx || std::fabs(s.dx) < 1.f) {
        s.rel = 0;
    } else {
        s.rel = (Sign(s.tvx) == Sign(s.dx)) ? 1 : -1;
    }
}

Sense Perceive(DWORD now, float px, float py, float lockX, float lockY, uint32_t pfhHint,
               bool travelPortal, uint32_t tfhHint = 0) {
    Sense s{};
    s.now = now;
    s.x = px;
    s.y = py;
    s.travelPortal = travelPortal;
    ports::teleport::FlightState st{};
    s.stOk = ports::teleport::QueryFlightState(st) && st.ok;
    if (s.stOk) {
        s.vx = st.vx;
        s.vy = st.vy;
        s.onFh = st.onFh;
        s.ma = st.ma;
    }
    s.cur = ports::foothold::PeekCurFhId();
    s.grounded = s.stOk && s.onFh && s.cur != 0;
    // MoveAction（BIN 12:54~14:28 实测，低位=朝向）：2/3 走，6/7 跳/落，8/9 站，14/15 梯，16/17 绳，
    // **18 = 死亡躺尸**（upload 2026-09-09 10:34：hp=0 后 ma=18 定住 30s）。旧口径 14~19 把尸体当绳上，
    // 每 3s 一次 rope_unknown_bail → HopOff 对着尸体按跳。
    s.onRope = !s.grounded && IsRopeMa(s.ma);
    s.pfh = s.cur ? s.cur : pfhHint;
    if (s.cur && pfhHint && pfhHint != s.cur) {
        // 脚下 CurFh 是立面/墙段（构图里不连任何 Walk 边）：用调用方贴出的台规划，
        // 否则整图 no_path（BIN 17:13 fromComp=34）。
        ports::foothold_path::FhGeomInfo gi{};
        if (ports::foothold_path::FhGeom(s.cur, &gi) && gi.vertical && gi.walkDeg == 0) s.pfh = pfhHint;
    }
    if (!s.pfh && s.onRope) {
        // 挂在绳上：规划起点就是这根绳的节点（接力绳两头可能都没台，Snap 会贴到几百像素外的台）。
        ports::foothold_path::RopeInfo rope{};
        if (ports::foothold_path::FindRopeAt(px, py, 12, &rope) && rope.nodeId) s.pfh = rope.nodeId;
    }
    if (!s.pfh) {
        float sx = 0.f, sy = 0.f;
        uint32_t snapFh = 0;
        if (ports::foothold_path::SnapStandAt(px, py, &sx, &sy, &snapFh, /*preferFlat=*/false,
                                              /*avoidWalkJunction=*/!travelPortal,
                                              /*cliffInset=*/!travelPortal) &&
            snapFh)
            s.pfh = snapFh;
    }
    float msx = 0.f, msy = 0.f;
    uint32_t mfh = 0;
    s.tOk = ports::foothold_path::SnapStandAt(lockX, lockY, &msx, &msy, &mfh, /*preferFlat=*/false,
                                              /*avoidWalkJunction=*/!travelPortal,
                                              /*cliffInset=*/!travelPortal) &&
            mfh;
    s.tfh = s.tOk ? mfh : 0;
    s.tx = lockX;
    s.ty = lockY;
    // 调用方已知目标台（休息=绳底 dnFh）：禁止 SnapStandAt 把 (x, dnY) 吸到下一层
    //（BUILD198：绳 dnY=-1725 / dnFh=198，却 snap 到 tfh=4 @ y=-1785，hops=0 对着绳 X 走）。
    if (tfhHint) {
        s.tfh = tfhHint;
        s.tOk = true;
        // 绳节点 FhYAt 会给出绳段中点：爬向绳顶 overlap 时必须保留调用方的 lockY
        //（portal.y），否则 ty=中点会让 ResumeOnRope 在过半后改爬 ↓。
        if (!ports::foothold_path::IsRopeNodeId(tfhHint)) {
            float hy = 0.f;
            if (ports::foothold_path::FhYAt(tfhHint, lockX, &hy)) s.ty = hy;
        }
    }
    s.dx = s.tx - px;
    s.dy = s.ty - py;
    TrackTarget(s);
    return s;
}

// ───────── 执行（带惯性） ─────────
struct Motor {
    int dir = 0;        // 当前按住的左右
    int lastDir = 0;    // 上一次非零方向
    DWORD dirMs = 0;    // 上次改方向时刻
    int vert = 0;
    DWORD jumpMs = 0;
    bool keyFail = false;

    bool Walk(DWORD now, int want, bool urgent = false) {
        if (want != -1 && want != 1) return Stop();
        if (want == dir) return Note(ports::attack::HoldWalk(want));
        // 反向最短保持：刚换过向 / 刚停下就要反走 → 先按原向或先站着（惯性）。
        if (!urgent && dirMs && now - dirMs < kDirMinHoldMs) {
            if (dir != 0) return Note(ports::attack::HoldWalk(dir));
            if (want == -lastDir) return Stop();
        }
        dir = want;
        lastDir = want;
        dirMs = now;
        return Note(ports::attack::HoldWalk(want));
    }
    bool Stop() {
        dir = 0;
        return Note(ports::attack::StopWalk());
    }
    // 键按着人却不走：同帧补松→按边沿（方向不变，看不出来）。
    bool Refresh() { return Note(ports::attack::RefreshWalk()); }
    bool Up(int v) {
        if (v == 0) {
            vert = 0;
            return Note(ports::attack::ReleaseVertical());
        }
        vert = v;
        return Note(ports::attack::HoldVertical(v));
    }
    bool Jump(DWORD now) {
        jumpMs = now;
        return Note(ports::attack::PulseJump(kJumpPulseMs));
    }
    void ReleaseJump() { (void)ports::attack::ReleaseJump(); }
    bool Note(bool ok) {
        if (!ok) keyFail = true;
        return ok;
    }
};
Motor gMotor;

// ───────── 状态机 ─────────
enum class Mode : uint8_t { Idle = 0, WalkTo, Jump, Grab, Climb, HopOff, Drop, Recover, Dead };
enum class Sub : uint8_t {
    None = 0,
    RunUp,     // Jump / Grab：助跑
    Launch,    // 起跳这一拍
    Air,       // 腾空
    Approach,  // Grab：走向绳下
    Align,     // Grab：微步对齐到 ≤6px
    Settle,    // Grab：停稳
    Reverse,   // WalkTo：卡住退一步
    Wait,      // WalkTo：等目标走来
    Sidestep,  // WalkTo：怪叠在身上，退到侧位
    DropThrough,
    WalkOff,
};

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Idle:
            return "Idle";
        case Mode::WalkTo:
            return "WalkTo";
        case Mode::Jump:
            return "Jump";
        case Mode::Grab:
            return "Grab";
        case Mode::Climb:
            return "Climb";
        case Mode::HopOff:
            return "HopOff";
        case Mode::Drop:
            return "Drop";
        case Mode::Recover:
            return "Recover";
        case Mode::Dead:
            return "Dead";
        default:
            return "?";
    }
}

const char* SubName(Sub s) {
    switch (s) {
        case Sub::None:
            return "";
        case Sub::RunUp:
            return "run";
        case Sub::Launch:
            return "launch";
        case Sub::Air:
            return "air";
        case Sub::Approach:
            return "approach";
        case Sub::Align:
            return "align";
        case Sub::Settle:
            return "settle";
        case Sub::Reverse:
            return "reverse";
        case Sub::Wait:
            return "wait";
        case Sub::Sidestep:
            return "sidestep";
        case Sub::DropThrough:
            return "drop";
        case Sub::WalkOff:
            return "walkoff";
        default:
            return "?";
    }
}

const char* ProgName(Progress p) {
    switch (p) {
        case Progress::Idle:
            return "idle";
        case Progress::Moving:
            return "moving";
        case Progress::Chasing:
            return "chasing";
        case Progress::Waiting:
            return "waiting";
        case Progress::Blocked:
            return "blocked";
        case Progress::Recovering:
            return "recover";
        default:
            return "?";
    }
}

struct Sm {
    Mode mode = Mode::Idle;
    Sub sub = Sub::None;
    DWORD modeMs = 0;
    DWORD subMs = 0;

    // 规划缓存
    bool planOk = false;
    FirstAction act{};
    DWORD planMs = 0;
    uint32_t planTgtFh = 0;
    DWORD actStartMs = 0;
    bool chase = false;  // Walk hops==0：朝目标走

    // WalkTo
    int walkDir = 0;
    DWORD reactUntil = 0;
    DWORD lastKeyMs = 0;
    float blockX = 0.f;
    DWORD blockedSince = 0;
    int blockLevel = 0;
    DWORD reverseMs = 0;  // 本次退一步要退多久（进 Reverse 前抽）
    DWORD relatchMs = 0;  // 上次平地补边沿时刻（只为日志 / 采证）
    bool earlyRelatchDone = false;  // 本次卡住已做过 300ms 早补边沿
    bool landHoldDecided = false;   // 跨台跳：过顶点后已决定落地松不松键
    bool landHold = false;          // 跨台跳：一路按到落地（落地台宽、下一步同向）
    DWORD waitSince = 0;
    DWORD waitDoneMs = 0;
    DWORD relNegSince = 0;    // 目标连续「朝我来」起点（进 Wait 要确认 ≥60ms）
    float waitLastAdx = 0.f;  // Wait 期间见过的最小 |dx|：还在缩就是还在来
    DWORD waitProgressMs = 0;
    int sideDir = 0;
    DWORD fidgetNextMs = 0;  // 下次顿一下的时刻
    DWORD fidgetUntil = 0;   // 正在顿
    int stuckJumps = 0;
    DWORD stuckJumpFirstMs = 0;
    WalkAhead ahead = WalkAhead::Floor;
    bool afterBlockedReplan = false;  // 卡死后重规划仍是同一条边 → 判不可达
    DWORD forceSkipMs = 0;            // 上次无视 hop skip 兜底规划的时刻

    // Jump
    bool acrossBackingUp = false;  // 极限跳：到点时没速度 / 越过起跳点 → 先退回去再冲
    DWORD acrossBackMs = 0;
    float acrossBackPx = 40.f;     // 本次退多远（按台面余量算，≤40）
    DWORD jumpUpSettleMs = 0;      // 跳上头顶台：到点开始停稳的时刻
    bool dropOverTgt = false;      // 穿台点已放进落点台 X 范围内 → 穿之前人也必须在范围内
    int jumpDir = 0;
    bool jumpRun = false;
    const char* jumpWhy = "";
    Mode jumpReturn = Mode::WalkTo;
    DWORD launchMs = 0;
    // 起跳后有没有真离地 / 空中最大横速：分清「几何够不着」和「键没进去」。失焦时起跳常没横速
    //（BIN 13:59:40 Jump.air vx=0），按几何失败数 3 次就把好边标死。
    bool sawAir = false;
    float airMaxVx = 0.f;
    int inputFails = 0;
    int actModelMs = -1;  // 本动作按耗时模型的估计（校准用；-1 = 没算）

    // Grab / Climb / HopOff
    int32_t spotX = 0;
    bool needJump = false;
    bool pastEdge = false;
    int edgeDir = 0;            // 台外绳：起跳方向（判「飞过头」用）
    bool grabMiss = false;      // 跳抓：本次跳已判没抓住（正在往下掉），别再当 700ms 悬空=上绳
    const char* grabMissWhy = "";
    DWORD grabJumpMs = 0;       // 本动作最近一次抓绳起跳（重试冷却只看它，不看极限跳 / ESP 跳）
    int attempts = 0;
    int nudges = 0;
    bool floorTight = false;    // 落地梯 12px 内没上去过 → 之后对齐收到 4px
    DWORD nudgeUntil = 0;
    DWORD settleMs = 0;
    DWORD grabLandMs = 0;
    float climbY = 0.f;
    DWORD climbYMs = 0;
    int offDir = 0;
    DWORD offJumpMs = 0;
    bool offGrab = false;         // RopeJump 抓另一根绳：离绳后按住 ↑ 飞过去
    DWORD hopOffNoJumpUntil = 0;  // 绳顶先走上去不跳，到此时刻还没上去才补跳
    int stallStage = 0;      // 绳上 Y 不动：0 正常 / 1 松键重按 / 2 侧跳脱离
    DWORD regripUntil = 0;
    bool ropeResume = false; // 本动作是「人已在绳上」时合成的

    // Drop
    bool walkOff = false;
    bool dropJumped = false;
    DWORD dropJumpMs = 0;
    int32_t dropX = 0;   // 穿台点 / 走崖端点
    int dropDir = 0;     // 走崖方向
    int dropTries = 0;
    int dropNudges = 0;  // 本次穿台朝 dropX 微步了几次（超预算 = 可穿窗停不进去，换边）

    // 长周期绕圈检测：同一目标的一趟里，同一条非走路边 90s 内第 5 次开工 → 跳过该边 60s
    struct CycleRec {
        EdgeKind kind = EdgeKind::Walk;
        uint32_t from = 0, to = 0;
        DWORD ms = 0;
    };
    static constexpr int kCycleHist = 24;
    CycleRec cycle[kCycleHist]{};
    int cycleN = 0;

    // 来回摆检测：最近几次非追怪动作
    struct ActHist {
        EdgeKind kind = EdgeKind::Walk;
        uint32_t from = 0, to = 0;
        DWORD ms = 0;
    } hist[4]{};
    int histN = 0;

    // Recover
    Mode recoverReturn = Mode::Idle;
    DWORD stunUntil = 0;  // 被击退落地后的硬直窗

    // 输出
    Progress prog = Progress::Idle;
    bool actionChanged = false;
    const char* failWhy = "";
    Result fail = Result::Continue;

    DWORD senseLogMs = 0;
};
Sm g;
Stats gStats;

// 前置声明（定义在规划 / 走路小节里，StartAction 先用到）。
int32_t PlanX(float x);
void NoteActionDone(const Sense& s);

// 低血无药挂绳休息（TickRest / DriveClimb 共用）
struct RestCtx {
    bool active = false;
    bool ropeOk = false;
    ports::foothold_path::RopeInfo rope{};
    float dnY = 0.f;   // 绳底台在绳 X 处的台面 Y
    float hangY = 0.f; // 挂着的高度（AbsPos）
    DWORD startMs = 0;
    DWORD logMs = 0;
};
RestCtx gRest;
constexpr float kRestAboveFloorPx = 70.f;  // 至少高出底台这么多，地面怪碰不到
constexpr float kRestBelowTopPx = 40.f;    // 离顶留这么多，别爬过头走上台
constexpr float kRestWrongLayerPx = 48.f;  // 人/目标台与绳底台 |ΔY| 超过此值 = 错层
constexpr DWORD kRestStuckMs = 8000;       // 没上绳还在走 X 太久 → 放弃（BUILD198 错层走了 21s）

void EnterMode(Mode m, Sub s, DWORD now, const char* why, const Sense& sn) {
    if (g.mode != m || g.sub != s) {
        LogNav("human_sm %s%s%s→%s%s%s why=%s x=%.0f y=%.0f vx=%.0f cur=%u", ModeName(g.mode),
               g.sub != Sub::None ? "." : "", SubName(g.sub), ModeName(m), s != Sub::None ? "." : "",
               SubName(s), why ? why : "", sn.x, sn.y, sn.vx, (unsigned)sn.cur);
    }
    if (g.mode != m) g.modeMs = now;
    g.mode = m;
    g.sub = s;
    g.subMs = now;
}

void EnterSub(Sub s, DWORD now, const char* why, const Sense& sn) { EnterMode(g.mode, s, now, why, sn); }

void Fail(Result r, const char* why) {
    g.fail = r;
    g.failWhy = why;
}

bool SameAction(const FirstAction& a, const FirstAction& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == EdgeKind::Walk && a.hops == 0 && b.hops == 0) return true;
    return a.toFh == b.toFh && a.wx == b.wx;
}

// ───────── 几何小工具 ─────────
// 上绳该按 ↑ 还是 ↓。RopeJump 的 toFh 是落点台 / 目标绳节点，跟怎么上**本**绳无关：
// 人在本绳顶端以上（站顶台）按 ↓，在绳底以下（站底台）按 ↑。
int ClimbVert(float py, uint32_t toFh, int32_t wx, EdgeKind kind, int32_t ropeYBot = 0, int32_t ropeYTop = 0) {
    if (kind == EdgeKind::RopeJump && ropeYTop > ropeYBot) {
        if (py >= static_cast<float>(ropeYTop) - kVertYDead) return -1;  // AbsPos：更大 Y = 更高
        if (py <= static_cast<float>(ropeYBot) + kVertYDead) return 1;
        return 1;
    }
    float ox = 0.f, oy = 0.f;
    if (toFh && !ports::foothold_path::IsRopeNodeId(toFh) &&
        ports::foothold_path::SnapOnFh(toFh, static_cast<float>(wx), &ox, &oy)) {
        if (oy > py + kVertYDead) return 1;
        if (oy < py - kVertYDead) return -1;
    }
    if (kind == EdgeKind::ClimbUp) return 1;
    if (kind == EdgeKind::ClimbDown) return -1;
    return -1;
}

// 日志用：绳节点 id 打成 rope#N，真台照打数字。
const char* NodeStr(uint32_t id, char* buf, size_t n) {
    if (ports::foothold_path::IsRopeNodeId(id))
        snprintf(buf, n, "rope#%u", (unsigned)(id - ports::foothold_path::kRopeNodeIdBase));
    else
        snprintf(buf, n, "%u", (unsigned)id);
    return buf;
}

// 下跳方案：优先在本段安全带内穿台（↓+Alt）；穿台点不安全就两侧找最近安全点；
// 只有 wx 贴着**真悬崖端**（该端前方 Pit）才走崖，且方向朝离 wx 最近的那一端。
// 旧 CliffWalkDir 按「离人最近的端点」走：走到邻台又被规划送回来，来回 8 次
//（BIN 12:56 fh21↔fh26 wx=1867）。
bool FhTwins(uint32_t a, uint32_t b);
bool PlanDrop(const Sense& s) {
    const uint32_t from = g.act.fromFh;
    const int wx = g.act.wx;
    g.dropX = wx;
    g.dropDir = 0;
    g.walkOff = false;
    g.dropOverTgt = false;
    int xmin = 0, xmax = 0;
    if (!ports::foothold_path::FhXRange(from, &xmin, &xmax)) return false;
    // 构图把走崖边的 wx 放在段外（端点往外 10px）：直接朝那一端走出去，不试穿台。
    if (wx < xmin || wx > xmax) {
        g.walkOff = true;
        g.dropDir = (wx > xmax) ? 1 : -1;
        g.dropX = (wx > xmax) ? xmax : xmin;
        return true;
    }
    // 绳顶 ±20px 内按 ↓ 是上绳不是穿台（BIN 2026-09-09 x=668 绳顶台：穿台点 664 → resume_on_rope → 爬顶 →
    // 下台 → 再穿，3 轮 loop_break）：穿台点旁有绳的一律不算安全点。
    auto ropeFree = [&](int x) -> bool {
        float fx = 0.f, fy = s.y;
        (void)ports::foothold_path::SnapOnFh(from, static_cast<float>(x), &fx, &fy, false, false);
        return !ports::foothold_path::RopeNearDropPoint(static_cast<float>(x), fy);
    };
    // 两档安全带：strict = 崖沿内缩 36px（走过去带着走速也不会冲出崖）；loose = 只避段端 10px（站着按 ↓ 用不着
    // 36px：84px 的陡坡两头都是崖，内缩后只剩 12px 带，再避开绳就一个点都不剩——离线 sim 107000402 fh473）。
    auto safeAt = [&](int x, bool strict) -> bool {
        if (x < xmin + 4 || x > xmax - 4) return false;
        if (!ropeFree(x)) return false;
        if (strict)
            return ports::foothold_path::IsXSafeOnFh(from, static_cast<float>(x), /*avoidWalkJunction=*/false,
                                                     /*cliffInset=*/true);
        return x >= xmin + 10 && x <= xmax - 10 &&
               ports::foothold_path::IsXSafeOnFh(from, static_cast<float>(x), /*avoidWalkJunction=*/false,
                                                 /*cliffInset=*/false);
    };
    auto safe = [&](int x) -> bool { return safeAt(x, true); };
    // 穿台点还得在落点台的 X 范围里（留 6px），否则穿过去落到再下一层。优先「安全带 ∩ 落点台」，
    // 其次只要在落点台上（本来就要离开这块台，崖沿内缩对穿台没意义）。
    int tmin = 0, tmax = 0;
    const bool haveTgt = g.act.toFh && ports::foothold_path::FhXRange(g.act.toFh, &tmin, &tmax) && tmax - tmin >= 12;
    int tm = haveTgt ? (tmax - tmin) / 4 : 0;  // 边距随台宽缩（与 DriveDrop 同口径）
    if (tm > 6) tm = 6;
    if (tm < 2) tm = 2;
    // 这一列按 ↓ 真会落到落点台上（不是夹在中间的另一块；离线 sim 103000003 fh15→fh9：wx=336 那一列是 fh9，
    // 崖沿内缩把穿台点挪到 320，320 那一列下面先是 fh16）。
    auto columnOk = [&](int x) -> bool {
        if (!haveTgt) return true;
        float fx = 0.f, fy = s.y;
        (void)ports::foothold_path::SnapOnFh(from, static_cast<float>(x), &fx, &fy, false, false);
        const uint32_t below = ports::foothold_path::FirstFhBelow(static_cast<float>(x), fy);
        return below != 0 && FhTwins(below, g.act.toFh);
    };
    auto overTgt = [&](int x) -> bool { return !haveTgt || (x >= tmin + tm && x <= tmax - tm); };
    // 先 strict 再 loose：离 wx 最近、在落点台范围里、这一列真落到落点台的点。点两侧 ±4px 也得成立——
    // 可穿窗只有 1~2px 宽时微步永远停不进去（离线 sim 107000402 fh473：绳口 -1356 的 14px 避让与邻台列夹出
    // 一个 2px 窗，左右蹭 5s 超时）；窄窗宁可不要，交给后面的档 / 换边。
    auto okAt = [&](int x, bool strict) -> bool { return safeAt(x, strict) && overTgt(x) && columnOk(x); };
    auto okBand = [&](int x, bool strict) -> bool { return okAt(x, strict) && okAt(x - 4, strict) && okAt(x + 4, strict); };
    g.dropOverTgt = haveTgt;
    for (int tier = 0; tier < 2; ++tier) {
        const bool strict = tier == 0;
        if (okBand(wx, strict)) return true;
        for (int d = 4; d <= kDropSafeSearchPx; d += 4) {
            if (okBand(wx - d, strict)) {
                g.dropX = wx - d;
                return true;
            }
            if (okBand(wx + d, strict)) {
                g.dropX = wx + d;
                return true;
            }
        }
    }
    if (haveTgt) {
        // 第三档：落点台范围里离 wx 最近、旁边没绳、这一列真落到落点台的点（不管段端）。
        const int lo = (std::max)(xmin + 4, tmin + tm), hi = (std::min)(xmax - 4, tmax - tm);
        if (lo <= hi) {
            const int mid = (std::min)((std::max)(wx, lo), hi);
            auto ok3 = [&](int x) -> bool {
                return x >= lo && x <= hi && ropeFree(x) && columnOk(x) && ropeFree(x - 4) && columnOk(x - 4) &&
                       ropeFree(x + 4) && columnOk(x + 4);
            };
            // 注意 X 可以是负数：找到与否单独记（用 -1 当哨兵会把 x=-641 当「没找到」，整档白搜）。
            bool found = false;
            int best = 0;
            for (int d = 0; d <= hi - lo && !found; d += 4) {
                if (ok3(mid - d)) { best = mid - d; found = true; }
                else if (ok3(mid + d)) { best = mid + d; found = true; }
            }
            if (found) {
                g.dropX = best;
                return true;
            }
        }
    }
    // 落点台那一列找不到干净的穿台点：随便穿到哪块再规划（DriveDrop 不再核脚下那一列）。
    g.dropOverTgt = false;
    for (int tier = 0; tier < 2; ++tier) {
        const bool strict = tier == 0;
        if (safeAt(wx, strict)) return true;
        for (int d = 4; d <= kDropSafeSearchPx; d += 4) {
            if (safeAt(wx - d, strict)) {
                g.dropX = wx - d;
                return true;
            }
            if (safeAt(wx + d, strict)) {
                g.dropX = wx + d;
                return true;
            }
        }
    }
    // 贴崖端：往那一端走出去。
    const int end = (std::abs(wx - xmin) <= std::abs(wx - xmax)) ? xmin : xmax;
    const int dir = (end == xmin) ? -1 : 1;
    const float probeX = static_cast<float>(end) - static_cast<float>(dir) * 8.f;
    float fx = 0.f, fy = s.y;
    (void)ports::foothold_path::SnapOnFh(from, probeX, &fx, &fy, false, false);
    const auto ahead = ports::foothold_path::ProbeWalkAhead(probeX, fy, dir, from, 48);
    if (ahead == WalkAhead::Pit) {
        g.walkOff = true;
        g.dropDir = dir;
        g.dropX = end;
        return true;
    }
    // 不是崖（邻台接着）：本段任意安全点穿台；短到没有安全带就取段中（段中挨着绳就往两边让，让不开 = 没穿台点，
    // 交给规划换边——站在绳口按 ↓ 上绳不穿台，以前在这儿干站 5s 超时）。
    for (int x = xmin + 8; x <= xmax - 8; x += 8) {
        if (safe(x)) {
            g.dropX = x;
            return true;
        }
    }
    if (xmax - xmin < 12) return false;
    const int mid = (xmin + xmax) / 2;
    for (int d = 0; d <= (xmax - xmin) / 2 - 6; d += 4) {
        if (ropeFree(mid - d) && mid - d >= xmin + 6) { g.dropX = mid - d; return true; }
        if (ropeFree(mid + d) && mid + d <= xmax - 6) { g.dropX = mid + d; return true; }
    }
    return false;
}

// 下绳必须带横向速度：台心常在绳 X 上，原地跳会弹回原绳（BIN 11:30）。
// 两侧台面都够宽时朝目标那边下（少走回头路）；否则朝宽的一侧。
int ClimbOffWalkDir(uint32_t toFh, int32_t ropeX, float px, float targetX = 0.f, bool haveTarget = false) {
    int xmin = 0, xmax = 0;
    if (ports::foothold_path::FhXRange(toFh, &xmin, &xmax) && xmax > xmin) {
        const int left = ropeX - xmin;
        const int right = xmax - ropeX;
        if (haveTarget && left >= kHopOffRoomPx && right >= kHopOffRoomPx &&
            std::fabs(targetX - static_cast<float>(ropeX)) >= 1.f)
            return (targetX > static_cast<float>(ropeX)) ? 1 : -1;
        if (right > left + 4) return 1;
        if (left > right + 4) return -1;
        const int mid = (xmin + xmax) / 2;
        if (mid != ropeX) return (mid > ropeX) ? 1 : -1;
    }
    return (px >= static_cast<float>(ropeX)) ? 1 : -1;
}

// 该 X 上有没有 Y 在 deckY±tol 内的台面（非墙）。绳顶台面常是几段 FH 接起来的，只看 toFh
// 一段的 X 范围会把「另一侧其实有台」判成没路（BIN 16:00:44 / 16:00:53 绳顶先朝左走一步再回头往右）。
uint32_t FloorNear(float x, float y, int tol) {
    ports::foothold_path::ColumnHit hits[6]{};
    int total = 0;
    const int n = ports::foothold_path::ProbeColumn(x, y, tol, hits, 6, &total);
    for (int i = 0; i < n; ++i)
        if (!hits[i].wall) return hits[i].fh;
    return 0;
}

bool DeckSideHasFloor(int32_t ropeX, int dir, float deckY) {
    const float probes[2] = {20.f, static_cast<float>(kHopOffRoomPx)};
    for (float d : probes)
        if (!FloorNear(static_cast<float>(ropeX) + static_cast<float>(dir) * d, deckY, 20)) return false;
    return true;
}

// 绳顶 / 绳底走上台面的方向：两侧都有 ≥48px 连续台面就朝目标那边；只有一侧有就那侧；
// 都探不到才退回按 toFh 段宽判。
int DeckOffDir(uint32_t toFh, int32_t ropeX, float deckY, float px, float targetX, bool haveTarget,
               bool* outL = nullptr, bool* outR = nullptr) {
    const bool L = DeckSideHasFloor(ropeX, -1, deckY);
    const bool R = DeckSideHasFloor(ropeX, 1, deckY);
    if (outL) *outL = L;
    if (outR) *outR = R;
    if (L != R) return L ? -1 : 1;
    if (L && R && haveTarget && std::fabs(targetX - static_cast<float>(ropeX)) >= 1.f)
        return (targetX > static_cast<float>(ropeX)) ? 1 : -1;
    return ClimbOffWalkDir(toFh, ropeX, px, targetX, haveTarget);
}

bool DestOnFh(uint32_t toFh, int32_t wx, float* outX, float* outY) {
    return toFh && ports::foothold_path::SnapOnFh(toFh, static_cast<float>(wx), outX, outY,
                                                  /*avoidWalkJunction=*/false, /*cliffInset=*/false);
}

// 楼梯台阶：目标台更高且已贴近本段端点 → 该跳上去（BIN 12:01 撞立面 no_move）。
// 高差要在**本段端点那个 X** 上比：斜坡上坡时人脚下的 Y 比端点低几十像素，拿脚下 Y 比会把
// 相接的斜坡段当台阶，每个接缝都空跳一次（upload 2026-09-09 map101040000 全程 why=step）。
bool NearWalkStepUp(float px, int dir, uint32_t fromFh, uint32_t toFh) {
    if (!fromFh || !toFh || (dir != -1 && dir != 1)) return false;
    int xmin = 0, xmax = 0;
    if (!ports::foothold_path::FhXRange(fromFh, &xmin, &xmax)) return false;
    const int end = (dir > 0) ? xmax : xmin;
    if (std::fabs(px - static_cast<float>(end)) > 40.f) return false;
    float fyEnd = 0.f, tyEnd = 0.f;
    if (!ports::foothold_path::FhYAt(fromFh, static_cast<float>(end), &fyEnd)) return false;
    if (!ports::foothold_path::FhYAt(toFh, static_cast<float>(end), &tyEnd)) return false;
    const float gap = tyEnd - fyEnd;  // AbsPos：更大 Y = 更高
    if (gap < kStepUpDy) return false;         // 同高相接 / 斜坡：走过去
    if (gap > kJumpReachPx) return false;      // 一跳上不去的高差不要在端点空跳
    return true;
}

float EdgeDist(uint32_t fh, float px, int dir) {
    int xmin = 0, xmax = 0;
    if (!ports::foothold_path::FhXRange(fh, &xmin, &xmax)) return 1e9f;
    return (dir > 0) ? static_cast<float>(xmax) - px : px - static_cast<float>(xmin);
}

bool MaLooksRope(const Sense& s) { return s.onRope; }

// 台外绳起跳距离：顶点时刻恰在绳前 kEdgeGrabLeadPx（见常量处的 BIN 推导）。vx 用起跳当拍实测：
// 空中不按键横速不衰减，所以顶点 X = 起跳 X + vx×0.306s。
float EdgeGrabLaunchDx(float vx) {
    const float d = std::fabs(vx) * static_cast<float>(kJumpApexMs) / 1000.f + kEdgeGrabLeadPx;
    return d < kEdgeGrabMinLaunchDx ? kEdgeGrabMinLaunchDx : d;
}

// 台外绳失手记忆：飞过头 / 掉下去后人落在别的台，动作会被重规划推翻、g.attempts 归零，
// 同一条边就能无限重试（本 BIN 一轮 9s）。按 (map, from, to) 记次数，满 3 次标死边交给 Dijkstra 绕。
struct EdgeMiss {
    int32_t mapId = 0;
    uint32_t from = 0, to = 0;
    int count = 0;
    DWORD ms = 0;
};
EdgeMiss gEdgeMiss[8]{};
constexpr DWORD kEdgeMissForgetMs = 10 * 60 * 1000;

int NoteEdgeGrabMiss(uint32_t from, uint32_t to, DWORD now) {
    ports::foothold_path::GraphMeta meta{};
    const int32_t mapId = ports::foothold_path::GetGraphMeta(&meta) ? meta.mapId : 0;
    int slot = -1;
    for (int i = 0; i < 8; ++i) {
        auto& e = gEdgeMiss[i];
        if (e.count && (e.mapId != mapId || now - e.ms > kEdgeMissForgetMs)) e = {};
        if (e.count && e.from == from && e.to == to) {
            slot = i;
            break;
        }
        if (slot < 0 && !e.count) slot = i;
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < 8; ++i)
            if (gEdgeMiss[i].ms < gEdgeMiss[slot].ms) slot = i;
        gEdgeMiss[slot] = {};
    }
    auto& e = gEdgeMiss[slot];
    e.mapId = mapId;
    e.from = from;
    e.to = to;
    e.ms = now;
    return ++e.count;
}

// 输入被引擎忽略的窗口：出刀动画锁 / 击退硬直。此时 vx=0 不是顶住，微步也白按。
bool InputLocked(const Sense& s) {
    if (ports::attack::MsSinceLastFire() < kPostFireLockMs) return true;
    return g.stunUntil && s.now < g.stunUntil;
}

// (x, y) 正下方最近的可站台；没有 → 0。绳上侧跳脱离前用它确认落点就是目标台。
uint32_t FirstFhBelow(float x, float y, int window) {
    ports::foothold_path::ColumnHit hits[12]{};
    int total = 0;
    const int n = ports::foothold_path::ProbeColumn(x, y, window, hits, 12, &total);
    uint32_t best = 0;
    int bestY = INT_MIN;
    for (int i = 0; i < n; ++i) {
        if (hits[i].wall) continue;
        const int fy = hits[i].y;
        if (static_cast<float>(fy) >= y - 8.f) continue;  // AbsPos：更小 Y = 更低
        if (fy > bestY) {
            bestY = fy;
            best = hits[i].fh;
        }
    }
    return best;
}

// ───────── 规划 ─────────
bool NeedReplan(const Sense& s) {
    if (!g.planOk) return true;
    if (g.mode == Mode::Idle) return true;
    // 空中 / 抓绳 / 绳上 / 下绳 / 下跳 / 恢复：动作没完不换脑子。
    if (!s.grounded) return false;
    switch (g.mode) {
        case Mode::Jump:
        case Mode::Climb:
        case Mode::HopOff:
        case Mode::Recover:
            return false;
        case Mode::Grab:
            if (g.sub != Sub::Approach) return false;
            break;
        case Mode::Drop:
            if (g.sub == Sub::Air || g.dropJumped) return false;
            break;
        default:
            break;
    }
    if (s.tfh != g.planTgtFh) {
        g.cycleN = 0;  // 换了目标台：绕圈计数从头数
        return true;
    }
    if (s.cur != g.act.fromFh) return true;  // 换了脚下台：到站 / 漂移
    if (g.mode == Mode::WalkTo && s.now - g.planMs >= kReplanMs) return true;
    return false;
}

bool LoopBreak(const FirstAction& act, DWORD now);

// allowSame=true：与当前动作同一条边则只更新几何，不重置计数（追怪换台 / 复核规划）。
void StartAction(const FirstAction& act, const Sense& s, bool allowSame) {
    const bool sameId = allowSame && SameAction(g.act, act);
    g.act = act;
    g.chase = act.kind == EdgeKind::Walk && act.hops == 0;
    if (sameId && g.mode != Mode::Idle) return;
    if (sameId && g.afterBlockedReplan) {
        // 卡死 → 跳过该边重规划 → 还是这条边：没别的路，交给调用方 ban。
        g.afterBlockedReplan = false;
        Fail(Result::Unreachable, "blocked");
        return;
    }
    g.afterBlockedReplan = false;

    if (!sameId && !g.chase && LoopBreak(act, s.now)) {
        g.planOk = false;
        EnterMode(Mode::Idle, Sub::None, s.now, "loop_break", s);
        return;
    }

    if (!sameId) {
        g.actStartMs = s.now;
        g.actionChanged = true;
        g.blockLevel = 0;
        g.blockedSince = 0;
        g.blockX = s.x;
        g.attempts = 0;
        g.inputFails = 0;
        g.sawAir = false;
        g.airMaxVx = 0.f;
        g.actModelMs = (!g.chase && act.fromFh && act.toFh)
                           ? ports::foothold_path::EstimateEdgeMs(act.fromFh, act.toFh, act.kind, act.wx,
                                                                  PlanX(s.x))
                           : -1;
        ++gStats.actionsStarted;
        g.nudges = 0;
        g.floorTight = false;
        g.nudgeUntil = 0;
        g.settleMs = 0;
        g.walkOff = false;
        g.dropJumped = false;
        g.dropJumpMs = 0;
        g.dropTries = 0;
        g.dropNudges = 0;
        g.offDir = 0;
        g.offJumpMs = 0;
        g.offGrab = false;
        g.hopOffNoJumpUntil = 0;
        g.edgeDir = 0;
        g.grabMiss = false;
        g.grabMissWhy = "";
        g.grabJumpMs = 0;
        g.climbY = s.y;
        g.climbYMs = s.now;
        g.walkDir = 0;
        g.waitSince = 0;
        g.stallStage = 0;
        g.regripUntil = 0;
        g.ropeResume = false;
    }

    switch (act.kind) {
        case EdgeKind::Walk:
            // 新开走：人不会零延迟起步（换新目标 / 刚打完）。
            if (g.chase && (!g.lastKeyMs || s.now - g.lastKeyMs > kReactIdleMs) && !s.travelPortal) {
                g.reactUntil = s.now + Jitter(s.now, kReactMinMs, kReactMaxMs);
            } else {
                g.reactUntil = 0;
            }
            EnterMode(Mode::WalkTo, Sub::None, s.now, g.chase ? "chase" : "hop", s);
            break;
        case EdgeKind::ClimbUp:
        case EdgeKind::ClimbDown:
        case EdgeKind::RopeJump: {
            if (ports::foothold_path::IsRopeNodeId(act.fromFh)) {
                // 从绳节点出发 = 人已经挂在这根绳上：不用走、不用抓，直接爬到位。
                g.stallStage = 0;
                g.regripUntil = 0;
                g.offGrab = false;
                g.hopOffNoJumpUntil = 0;
                g.climbY = s.y;
                g.climbYMs = s.now;
                g.ropeResume = true;
                EnterMode(Mode::Climb, Sub::None, s.now, "from_rope_node", s);
                break;
            }
            // RopeJump 的 wy 是起跳 Y，不是绳底；抓绳判定要用真绳底。
            const int32_t ropeBot = act.kind == EdgeKind::RopeJump ? act.ropeYBot : act.wy;
            int32_t walkX = act.wx;
            bool needJump = false;
            (void)ports::foothold_path::ClimbGrabHint(act.fromFh, act.wx, ropeBot, &walkX, &needJump);
            int xmin = 0, xmax = 0;
            g.pastEdge = false;
            if (ports::foothold_path::FhXRange(act.fromFh, &xmin, &xmax))
                g.pastEdge = act.wx < xmin - 6 || act.wx > xmax + 6;
            // 绳在本段 X 范围外，但绳下方同一高度有台且与本段同一条 Walk 链（落地绳，只是站在邻段上）：
            // 走过去按 ↑ 就是，不做台外助跑跳抓（离线 sim 107000401 fh616→绳 x=-756 在绳下蹭 9s 不跳）。
            if (g.pastEdge && act.kind != EdgeKind::RopeJump) {
                const uint32_t under = FloorNear(static_cast<float>(act.wx), s.y, 12);
                ports::foothold_path::FhGeomInfo a{}, b{};
                if (under && under != act.fromFh && ports::foothold_path::FhGeom(under, &a) &&
                    ports::foothold_path::FhGeom(act.fromFh, &b) && a.walkComp && a.walkComp == b.walkComp) {
                    float ux = 0.f, uy = s.y;
                    (void)ports::foothold_path::SnapOnFh(under, static_cast<float>(act.wx), &ux, &uy, false, false);
                    // 绳底离那块地板 ≤60px 才够得着（站着 ↑ / 原地竖直跳 +80）；更高的绳侧台助跑才是对的。
                    if (static_cast<float>(ropeBot) - uy <= 60.f) {
                        g.pastEdge = false;
                        needJump = (static_cast<float>(ropeBot) - uy) > 4.f;
                        walkX = act.wx;
                        LogNav("human_climb floor_under_rope fh=%u y=%.0f ropeBot=%d jump=%d (from=%u treated as floor)",
                               (unsigned)under, uy, (int)ropeBot, needJump ? 1 : 0, (unsigned)act.fromFh);
                    }
                }
            }
            g.needJump = needJump;
            g.spotX = walkX;  // 已钳进本段 2px 内：绳在段沿（穿洞绳）时别把人走进洞里
            EnterMode(Mode::Grab, Sub::Approach, s.now,
                      act.kind == EdgeKind::RopeJump
                          ? (act.ropeId2 ? "rope_to_rope" : "rope_side_jump")
                          : (g.pastEdge ? "rope_off_edge" : (needJump ? "rope_hang" : "rope_floor")),
                      s);
            break;
        }
        case EdgeKind::FallDown:
            if (!PlanDrop(s)) {
                Fail(Result::Unreachable, "no_drop_x");
                return;
            }
            if (!g.walkOff)
                LogNav("human_fall plan dropX=%d wx=%d overTgt=%d from=%u to=%u", (int)g.dropX, (int)act.wx,
                       g.dropOverTgt ? 1 : 0, (unsigned)act.fromFh, (unsigned)act.toFh);
            EnterMode(Mode::Drop, Sub::None, s.now, g.walkOff ? "fall_cliff" : "fall_through", s);
            break;
        case EdgeKind::JumpAcross:
            // 极限跳：先走到起跳点（DriveWalk 到点起跳，空中保住方向）。同一条边落回原台 3 次 → 死边。
            g.reactUntil = 0;
            g.acrossBackingUp = false;
            g.walkDir = Sign(static_cast<float>(act.aimX) - static_cast<float>(act.wx));
            EnterMode(Mode::WalkTo, Sub::None, s.now, "across_runup", s);
            break;
        case EdgeKind::JumpUp:
            // 原地跳上头顶的台：走到 wx 站稳，竖直跳，落到上台。落回原台 3 次 → 死边。
            g.reactUntil = 0;
            g.acrossBackingUp = false;
            g.walkDir = 0;
            EnterMode(Mode::WalkTo, Sub::None, s.now, "jump_up_walk", s);
            break;
    }
    if (!g.chase && !sameId) {
        char fb[24], tb[24];
        LogNav("human_nav start kind=%s hops=%d wx=%d wy=%d aim=%d rope2=%d from=%s to=%s dropX=%d",
               KindName(act.kind), act.hops, (int)act.wx, (int)act.wy, (int)act.aimX, (int)act.ropeId2,
               NodeStr(act.fromFh, fb, sizeof(fb)), NodeStr(act.toFh, tb, sizeof(tb)),
               act.kind == EdgeKind::FallDown ? (int)g.dropX : 0);
    }
}

// 同一条边 6s 内第 3 次开工 = 在两台之间来回摆（BIN 12:55 fh2↔fh12 助跑跳 / 12:56 fh21↔fh26 走崖）。
// 跳过该边让规划换路；没别的路则下一拍 no_path 交给调用方。
// 长周期绕圈：同一条非走路边 90s 内第 5 次开工（每次都「成功」了但整条路兜回原地——跳过去落到别处、
// 没落回原台不计失手、也没标死边；离线 sim lat100 101040000 jump_up 212→141 → across 141→136 → 落回 210，
// 99 轮到超时）。6s 窗的 LoopBreak 抓不到这种 5~8s 一圈的。命中就临时跳过该边让规划换路。
// 只在同一个目标台的一趟里数（换目标 / Reset 清零）：打怪时同一根绳 90s 上 5 次是正常的，不能误判。
constexpr DWORD kCycleWindowMs = 90000;
constexpr int kCycleHits = 5;
bool CycleBreak(const FirstAction& act, DWORD now) {
    if (act.kind == EdgeKind::Walk) return false;  // 走路边追怪来回是常态
    int hits = 0;
    const int cnt = g.cycleN < Sm::kCycleHist ? g.cycleN : Sm::kCycleHist;
    for (int i = 0; i < cnt; ++i) {
        const auto& h = g.cycle[i];
        if (h.kind == act.kind && h.from == act.fromFh && h.to == act.toFh && now - h.ms < kCycleWindowMs) ++hits;
    }
    g.cycle[g.cycleN % Sm::kCycleHist] = {act.kind, act.fromFh, act.toFh, now};
    ++g.cycleN;
    if (hits + 1 < kCycleHits) return false;
    ++gStats.loopBreak;
    LogNav("human_nav cycle_break kind=%s from=%u to=%u hits=%d/90s — skip edge 60s", KindName(act.kind),
           (unsigned)act.fromFh, (unsigned)act.toFh, hits + 1);
    ports::foothold_path::AddHopSkip(act.fromFh, act.toFh, act.kind, 60000);
    return true;
}

bool LoopBreak(const FirstAction& act, DWORD now) {
    int hits = 0;
    const int cnt = g.histN < 4 ? g.histN : 4;
    for (int i = 0; i < cnt; ++i) {
        const auto& h = g.hist[i];
        if (h.kind == act.kind && h.from == act.fromFh && h.to == act.toFh && now - h.ms < kLoopWindowMs)
            ++hits;
    }
    g.hist[g.histN % 4] = {act.kind, act.fromFh, act.toFh, now};
    ++g.histN;
    if (hits < 2) return CycleBreak(act, now);
    ++gStats.loopBreak;
    LogNav("human_nav loop_break kind=%s from=%u to=%u hits=%d — skip edge", KindName(act.kind),
           (unsigned)act.fromFh, (unsigned)act.toFh, hits + 1);
    ports::foothold_path::AddHopSkip(act.fromFh, act.toFh, act.kind);
    return true;
}

// 规划用的 X：站的 X / 目标 X 让 PlanFirst 按耗时挑路（走到绳下多远、落地后再走多远）；NaN 交给它用段中点。
int32_t PlanX(float x) {
    return std::isfinite(x) ? static_cast<int32_t>(std::lround(x)) : ports::foothold_path::kPlanNoX;
}

// 一跳干净完成（没失手、没卡住）：把实测耗时喂给耗时模型校准；有失手的那次不喂——失手成本走风险项。
void NoteActionDone(const Sense& s) {
    ++gStats.hopsDone;
    if (g.chase || g.actModelMs <= 0 || !g.actStartMs) return;
    if (g.attempts > 0 || g.inputFails > 0 || g.blockLevel > 0) return;
    const DWORD observed = s.now - g.actStartMs;
    if (observed < 150 || observed > 60000) return;
    ports::foothold_path::NoteEdgeObserved(g.act.kind, g.actModelMs, static_cast<int>(observed));
    g.actModelMs = -1;
}

// 按键→引擎起跳的延迟自测：从 PulseJump 到第一拍看见离地，EMA。30Hz 采样下无延迟也会量到 ≈33ms，
// 卡顿 / 注入排队时 66~100ms。起跳点判定按它把起跳提前 vx·延迟（离线 sim 66ms 延迟：崖沿起跳晚 8px、
// 20px 宽的窄台落不上、台外绳飞过头，成功率 98.4% → 93.5%）。
float gJumpLatMs = 40.f;
int gJumpLatSamples = 0;
bool gJumpLatLoaded = false;
constexpr float kJumpLatMinMs = 33.f, kJumpLatMaxMs = 200.f, kJumpLatBaseMs = 20.f;
// 提前量封顶：一次 200ms 的卡顿样本别把起跳点提前 22px——极限跳落点只有 14px 余量，提前太多会跳不到。
constexpr float kJumpLeadMaxPx = 12.f;
// 跨会话记忆：每台机器的键延迟基本是常数，重启后第一跳就带着上次学到的提前量（EMA 从 40ms 冷启动要 3~4 跳才
// 追上 100ms，头几跳起跳晚 8~12px；离线 sim lat100 极限跳 elsewhere 的一半是头一跳）。每 20 个样本落盘一次。
void EnsureJumpLatLoaded() {
    if (gJumpLatLoaded) return;
    gJumpLatLoaded = true;
    float ms = 0.f;
    int n = 0;
    if (ports::nav_memory::LoadJumpLatency(&ms, &n)) {
        gJumpLatMs = ms;
        gJumpLatSamples = n;
        LogNav("human_jump latency restored %.0fms (n=%d from navmem)", ms, n);
    }
}
void NoteJumpLatency(DWORD lat) {
    EnsureJumpLatLoaded();
    float v = static_cast<float>(lat);
    if (v < kJumpLatMinMs) v = kJumpLatMinMs;
    if (v > kJumpLatMaxMs) v = kJumpLatMaxMs;
    // 头几个样本权重大一点，快速离开 40ms 的默认值；之后 0.3 跟踪。
    const float alpha = gJumpLatSamples < 3 ? 0.5f : 0.3f;
    gJumpLatMs += alpha * (v - gJumpLatMs);
    ++gJumpLatSamples;
    if (gJumpLatSamples % 20 == 0) ports::nav_memory::SaveJumpLatency(gJumpLatMs, gJumpLatSamples);
}
// 倒退助跑时离台另一端至少留多远再掉头：掉头本身滑几像素 + 停键/掉头键各晚一拍还在走（两拍，取 1.5 倍提前量）。
// 100ms 键延迟：退到离端 21px 掉头仍滑出窄台（离线 sim lat100 101040000 fh405 → 掉下去 28 次）。
float JumpLeadPx(float vx);
float BackUpMarginPx() { return 12.f + 1.5f * JumpLeadPx(kWalkVx); }

// 以当前横速 vx 起跳，键延迟这段时间还会走多远（提前量，像素）。
float JumpLeadPx(float vx) {
    EnsureJumpLatLoaded();
    const float lat = gJumpLatMs - kJumpLatBaseMs;
    const float lead = lat > 0.f ? std::fabs(vx) * lat / 1000.f : 0.f;
    return lead > kJumpLeadMaxPx ? kJumpLeadMaxPx : lead;
}

// 「到了目标台」：CurFh 就是它，或人站在一块与它重合的孪生台上（同一处两条 foothold 记录，引擎报哪条不可控；
// 离线 sim 101020000 fh895/fh47：绳侧跳落点 (227,1699) 正是 aim，却因 cur=895≠47 判 miss，3 轮 loop_break）。
bool OnFhOrTwin(const Sense& s, uint32_t fh) {
    if (!fh) return false;
    if (s.cur == fh) return true;
    if (!s.grounded || !s.cur || ports::foothold_path::IsRopeNodeId(fh)) return false;
    int xmin = 0, xmax = 0;
    if (!ports::foothold_path::FhXRange(fh, &xmin, &xmax)) return false;
    if (s.x < static_cast<float>(xmin) - 2.f || s.x > static_cast<float>(xmax) + 2.f) return false;
    float fx = 0.f, fy = s.y;
    if (!ports::foothold_path::SnapOnFh(fh, s.x, &fx, &fy, false, false)) return false;
    return std::fabs(fy - s.y) <= 6.f;
}

// 两块台是不是「孪生」（X 范围重合 ≥8px、重合段中点台面 Y 差 ≤6）：同一处两条 foothold 记录。
bool FhTwins(uint32_t a, uint32_t b) {
    if (!a || !b) return false;
    if (a == b) return true;
    ports::foothold_path::FhGeomInfo ga{}, gb{};
    if (!ports::foothold_path::FhGeom(a, &ga) || !ports::foothold_path::FhGeom(b, &gb)) return false;
    const int lo = (std::max)((std::min)(ga.x1, ga.x2), (std::min)(gb.x1, gb.x2));
    const int hi = (std::min)((std::max)(ga.x1, ga.x2), (std::max)(gb.x1, gb.x2));
    if (hi - lo < 8) return false;
    const float mid = static_cast<float>(lo + hi) * 0.5f;
    float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
    if (!ports::foothold_path::SnapOnFh(a, mid, &ax, &ay, false, false) ||
        !ports::foothold_path::SnapOnFh(b, mid, &bx, &by, false, false))
        return false;
    return std::fabs(ay - by) <= 6.f;
}

// 空中采样：离过地、空中最大横速（起跳有没有带出速度）。
void NoteAirSample(const Sense& s) {
    if (s.grounded) return;
    // 只认起跳后 300ms 内的离地；键被吃后过很久才走出崖沿的不算延迟样本。
    if (!g.sawAir && g.launchMs && s.now - g.launchMs <= 300) NoteJumpLatency(s.now - g.launchMs);
    g.sawAir = true;
    const float avx = std::fabs(s.vx);
    if (avx > g.airMaxVx) g.airMaxVx = avx;
}

// 一次「跳了却没成」到底算几何失败还是键没进去：没离地 / 助跑跳空中横速不到走速一半 = 键没进去。
// 键没进去不计几何次数，只补一次跨帧边沿再来；连着 3 次键都没进去才按一次几何失败算（免得死循环）。
bool JumpWasInputFailure(bool needHorizontal) {
    const bool noAir = !g.sawAir;
    const bool noVx = needHorizontal && g.airMaxVx < kRunVx * 0.5f;
    if (!noAir && !noVx) return false;
    ++g.inputFails;
    ++gStats.inputFails;
    if (g.inputFails >= 3) {
        g.inputFails = 0;
        return false;  // 三次都没进去：当一次几何失败，别永远原地重试
    }
    (void)gMotor.Refresh();
    return true;
}

bool Replan(const Sense& s, TickOut& out) {
    FirstAction act{};
    if (s.travelPortal && !s.tfh) {
        act.ok = true;
        act.kind = EdgeKind::Walk;
        act.fromFh = s.pfh;
        act.toFh = 0;
        act.wx = static_cast<int32_t>(std::lround(s.tx));
        act.wy = static_cast<int32_t>(std::lround(s.ty));
        act.hops = 0;
    } else if (!ports::foothold_path::PlanFirst(s.pfh, s.tfh, &act, /*ignoreSkips=*/false, PlanX(s.x),
                                                PlanX(s.tx)) ||
               !act.ok) {
        // 被跳过的边是唯一路：与其把对面整层判 no_path 连 ban（BIN 14:25:14 三只），不如再试那条边。
        // 10s 内只兜一次，真过不去就让 no_path 出去。
        const bool skipsPending = ports::foothold_path::HopSkipCount() > 0;
        const bool recentlyForced = g.forceSkipMs && s.now - g.forceSkipMs < kForceSkipRearmMs;
        if (skipsPending && !recentlyForced &&
            ports::foothold_path::PlanFirst(s.pfh, s.tfh, &act, /*ignoreSkips=*/true, PlanX(s.x),
                                            PlanX(s.tx)) &&
            act.ok) {
            g.forceSkipMs = s.now;
            LogNav("human_nav plan_ignore_skips kind=%s from=%u to=%u hops=%d", KindName(act.kind),
                   (unsigned)act.fromFh, (unsigned)act.toFh, act.hops);
        } else {
            ++gStats.noPath;
            out.result = Result::Unreachable;
            out.why = "no_path";
            out.fromFh = s.pfh;
            out.toFh = s.tfh;
            return false;
        }
    }
    const bool hadPlan = g.planOk;
    const bool arrived = hadPlan && g.act.hops > 0 && s.grounded && OnFhOrTwin(s, g.act.toFh);
    if (arrived) {
        LogNav("human_nav hop_done kind=%s cur=%u to=%u", KindName(g.act.kind), (unsigned)s.cur,
               (unsigned)g.act.toFh);
        NoteActionDone(s);
        g.actionChanged = true;
    }
    g.planOk = true;
    g.planMs = s.now;
    g.planTgtFh = s.tfh;
    StartAction(act, s, /*allowSame=*/hadPlan && !arrived);
    return true;
}

DWORD ActionTimeout(const Sense& s) {
    switch (g.act.kind) {
        case EdgeKind::ClimbUp:
        case EdgeKind::ClimbDown:
        case EdgeKind::RopeJump: {
            if (g.mode != Mode::Climb && g.mode != Mode::HopOff) return kGrabTimeoutMs;
            // 爬绳时限按绳长算：长绳（101020000 x=-98 长 1392px）按 ~100px/s 爬要 14s，死数 14s 在离底 96px 处
            // 超时 → 白白重规划一轮。爬 ↑ 从绳底到顶台、爬 ↓ 从顶到底台，加 6s 余量；卡住由 yStall/regrip 管。
            // 顶台/底台可能是 <16px 的碎片（SnapOnFh 不认），拿绳本身的长度兜底。
            float destX = 0.f, destY = 0.f;
            float span = 0.f;
            if (DestOnFh(g.act.toFh, g.act.wx, &destX, &destY)) {
                span = std::fabs(destY - static_cast<float>(g.act.wy));
            } else {
                ports::foothold_path::RopeInfo ri{};
                if (ports::foothold_path::FindRopeAt(static_cast<float>(g.act.wx), static_cast<float>(g.act.wy), 8, &ri))
                    span = static_cast<float>(ri.yTop - ri.yBot);
            }
            const DWORD byLen = 6000u + static_cast<DWORD>(span * 1000.f / 90.f);
            return byLen > kClimbTimeoutMs ? byLen : kClimbTimeoutMs;
        }
        case EdgeKind::FallDown:
            return g.walkOff ? kFallWalkOffTimeoutMs : kFallTimeoutMs;
        case EdgeKind::JumpAcross:
            return kAcrossTimeoutMs;
        default:
            break;
    }
    if (g.chase) return 0;  // 追怪的超时由调用方管（含失焦冻结）
    const float adx = std::fabs(static_cast<float>(g.act.wx) - s.x);
    DWORD ms = 4000u + static_cast<DWORD>(adx * 8.f);
    if (ms < kWxTimeoutMs) ms = kWxTimeoutMs;
    if (ms > kWxTimeoutCapMs) ms = kWxTimeoutCapMs;
    return ms;
}

// ───────── 跳 ─────────
void BeginJump(const Sense& s, int dir, const char* why, bool run, Mode ret) {
    g.jumpDir = dir;
    g.jumpWhy = why;
    g.jumpRun = run;
    g.jumpReturn = ret;
    g.launchMs = 0;
    // 速度得是朝跳的方向：被顶飞 vx=-239 时按「够快」直接起跳会往反方向飞（BIN 18:32:51）。
    const bool fastEnough = s.vx * static_cast<float>(dir) >= kRunVx;
    EnterMode(Mode::Jump, (run && !fastEnough) ? Sub::RunUp : Sub::Launch, s.now, why, s);
}

void DoLaunch(const Sense& s, const char* why, int dir, float remain) {
    gMotor.Jump(s.now);
    g.launchMs = s.now;
    g.sawAir = false;
    g.airMaxVx = 0.f;
    g.landHoldDecided = false;
    g.landHold = false;
    LogNav("human_jump why=%s dir=%d cur=%u remain=%.0f vx=%.0f", why, dir, (unsigned)s.cur, remain,
           s.vx);
}

// 跨台跳过顶点时决定「落地要不要松键」。松键是为了键晚 66~100ms 时不带着走速冲出窄台（lat100 下连跳
// 157 次落地即走出崖沿），代价是每次跨台跳落地都站一下（松键落地 → Idle → 下一拍重规划 → 再按键 → 键延迟
// → 起步加速，实机约 150~250ms；BIN 09-09 16:03 赶路 110s 里 15 次这种落地，用户看到的「落地会停一下」）。
// 落地台够宽、预计落点稳在台上、下一步还是朝这个方向走 ≥40px 时就不松：键一直按着，落地那一拍接着走。
bool DecideLandHold(const Sense& s) {
#ifdef XCAT_SIM_NO_LANDHOLD  // 离线 sim 对照组：落地一律松键
    (void)s;
    return false;
#endif
    const int d = g.jumpDir;
    const uint32_t toFh = g.act.toFh;
    const char* no = nullptr;
    float predX = 0.f, nextX = 0.f;
    int jmin = 0, jmax = 0;
    if (!d || !toFh || !s.tfh) {
        no = "no_target";
    } else if (!ports::foothold_path::FhXRange(toFh, &jmin, &jmax) || jmax - jmin < 24) {
        no = "narrow";  // 墙 / 小块；宽一点的窄台由下面「落地后还有没有路」把关
    } else {
        // 预计落点：顶点到台面的下落时间 × 当前横速（横速不靠键维持，松不松都一样）
        float landY = 0.f;
        if (!ports::foothold_path::FhYAt(toFh, static_cast<float>(g.act.aimX), &landY)) return false;
        const float drop = s.y - landY;  // AbsPos：更大 Y = 更高；正 = 台面在脚下
        if (drop < 0.f) {
            no = "above";  // 台面比顶点还高：跳不上去，别赌
        } else {
            const float tFall = std::sqrt(2.f * drop / ports::foothold_path::kJumpGravityPxPerSec2);
            predX = s.x + s.vx * tFall;
            const float nearEdge = static_cast<float>(d > 0 ? jmin : jmax);
            const float farEdge = static_cast<float>(d > 0 ? jmax : jmin);
            if ((predX - nearEdge) * static_cast<float>(d) < 10.f) {
                no = "short";  // 可能落回原台 / 擦边
            } else if ((farEdge - predX) * static_cast<float>(d) < 60.f) {
                no = "no_room";  // 落地后没多少路可跑（键晚 100ms 也就多走 12px，60 够）
            } else if (s.tfh == toFh) {
                nextX = s.tx;  // 目标就在落地台上
            } else {
                // 从落地台再规划一步：下一步朝哪走
                ports::foothold_path::FirstAction next{};
                if (!ports::foothold_path::PlanFirst(toFh, s.tfh, &next, /*ignoreSkips=*/false,
                                                     static_cast<int>(predX), PlanX(s.tx)) ||
                    !next.ok)
                    no = "no_plan";
                else
                    nextX = static_cast<float>(next.wx);
            }
            if (!no && (nextX - predX) * static_cast<float>(d) < 40.f) no = "turns";  // 下一步不是朝这边走 / 就在脚边
        }
    }
    if (no) {
        LogNav("human_jump land_release why=%s x=%.0f vx=%.0f pred=%.0f next=%.0f to=%u[%d,%d]", no, s.x, s.vx, predX,
               nextX, (unsigned)toFh, jmin, jmax);
        return false;
    }
    return true;
}

void DriveJump(const Sense& s) {
    const float remain = g.chase ? std::fabs(s.dx) : std::fabs(static_cast<float>(g.act.wx) - s.x);
    switch (g.sub) {
        case Sub::RunUp:
            // 助跑：先把横速带起来再起跳，原地起跳没惯性、够不到落点。
            gMotor.Up(0);
            gMotor.Walk(s.now, g.jumpDir, /*urgent=*/true);
            if (InputLocked(s)) {
                // 出刀动画锁 / 击退硬直里跑不起来：助跑计时从解锁后算（BIN 18:32:49 vx=0 满 420ms 原地起跳）。
                g.subMs = s.now;
                break;
            }
            if (s.vx * static_cast<float>(g.jumpDir) >= kRunVx || s.now - g.subMs >= kRunUpMaxMs) {
                DoLaunch(s, g.jumpWhy, g.jumpDir, remain);
                EnterSub(Sub::Air, s.now, "launched", s);
            }
            break;
        case Sub::Launch:
            gMotor.Up(0);
            if (g.jumpRun) gMotor.Walk(s.now, g.jumpDir, /*urgent=*/true);
            DoLaunch(s, g.jumpWhy, g.jumpDir, remain);
            EnterSub(Sub::Air, s.now, "launched", s);
            break;
        case Sub::Air:
        default: {
            // 空中保持方向（保住动量）；原地跳则不碰左右。
            // 极限跳：X 已过落点 aim 就松方向键——松键不减空中横速，但落地那一拍不再带着走速往前跑。
            // 落到窄台离台沿 3~5px、键晚 66ms 还按着 → 直接走出台沿掉回下层，整条路重来（离线 sim lat66
            // 101030400 fh117→fh112 ×28 轮循环到超时）。
            // 过了顶点也松：空中横速不靠键维持（反向键才减速），松早一点落地那一拍就是停的。键晚 100ms 时
            // 「过 aim 才松」的停键落地后 70ms 才到，人已带着 125 冲向下一个崖沿（lat100 下 jump quest 连跳
            // 157 次落地即走出崖沿）。
            const bool acrossAir = g.act.kind == EdgeKind::JumpAcross && !g.chase && g.jumpDir != 0;
            const bool pastAim = acrossAir && (s.x - static_cast<float>(g.act.aimX)) * static_cast<float>(g.jumpDir) >= -2.f;
            const bool pastApex = acrossAir && g.sawAir && s.vy <= 0.f;
            // 过顶点那一拍决定要不要一路按到落地（见 DecideLandHold）；决定只做一次。
            if (acrossAir && pastApex && !g.landHoldDecided) {
                g.landHoldDecided = true;
                g.landHold = DecideLandHold(s);
                if (g.landHold) LogNav("human_jump land_hold x=%.0f vx=%.0f aim=%d to=%u", s.x, s.vx, (int)g.act.aimX, (unsigned)g.act.toFh);
            }
            const bool keepKey = g.jumpDir != 0 && (g.landHold || (!pastAim && !pastApex));
            if (keepKey) gMotor.Walk(s.now, g.jumpDir, /*urgent=*/true);
            else if (pastAim || pastApex) gMotor.Stop();
            NoteAirSample(s);
            const DWORD since = s.now - g.launchMs;
            if (s.grounded && since >= kJumpLandMinMs) {
                // 按着键落地却没落到目标台（落回原台 / 落到别处）：立刻松，别带着走速冲崖沿
                if (g.landHold && !OnFhOrTwin(s, g.act.toFh)) gMotor.Stop();
                const bool acrossKind = g.act.kind == EdgeKind::JumpAcross || g.act.kind == EdgeKind::JumpUp;
                if (acrossKind && !g.chase) {
                    // 落回原台 = 没跳过去 / 没跳上去。先分清是几何够不着还是键没进去（没离地 / 空中没横速）：
                    // 键没进去不计几何次数，补个边沿再来；否则失焦时 3 次白摔就把好边标死。
                    const char* verdict = OnFhOrTwin(s, g.act.toFh) ? "OK" : (s.cur == g.act.fromFh ? "fell_back" : "elsewhere");
                    if (s.cur == g.act.fromFh) {
                        if (JumpWasInputFailure(/*needHorizontal=*/g.act.kind == EdgeKind::JumpAcross)) {
                            verdict = "no_input";
                        } else {
                            ++g.attempts;
                            ++gStats.acrossFellBack;
                        }
                    }
                    LogNav("human_jump %s_landed cur=%u to=%u x=%.0f y=%.0f aim=%d %s tries=%d air=%d maxvx=%.0f",
                           g.act.kind == EdgeKind::JumpUp ? "up" : "across", (unsigned)s.cur, (unsigned)g.act.toFh,
                           s.x, s.y, (int)g.act.aimX, verdict, g.attempts, g.sawAir ? 1 : 0, g.airMaxVx);
                }
                EnterMode(g.jumpReturn, Sub::None, s.now, "landed", s);
            } else if (since >= kJumpAirMaxMs) {
                g.planOk = false;
                EnterMode(Mode::Idle, Sub::None, s.now, "air_timeout", s);
            }
            break;
        }
    }
    g.prog = Progress::Moving;
}

// ───────── 走 ─────────
bool StuckJumpBudget(DWORD now) {
    if (!g.stuckJumpFirstMs || now - g.stuckJumpFirstMs > kStuckJumpWindowMs) {
        g.stuckJumpFirstMs = now;
        g.stuckJumps = 0;
    }
    return g.stuckJumps < kMaxStuckJumps;
}

bool WaitRule(const Sense& s) {
    if (!g.chase || s.travelPortal) return false;
    if (g.waitDoneMs && s.now - g.waitDoneMs < kWaitRearmMs) return false;
    if (std::fabs(s.dy) > kWaitDy) return false;
    if (std::fabs(s.dx) > kWaitDx) return false;
    // 一拍的速度估计会抖：连续 ≥60ms 朝我来才停步。
    return s.rel < 0 && g.relNegSince && s.now - g.relNegSince >= kWaitEnterConfirmMs;
}

// 前方 20px 一列里有比脚下更高、一跳够得着的台面 → 真有东西挡着（箱子 / 台阶立面）。
bool ObstacleAhead(const Sense& s) {
    ports::foothold_path::ColumnHit hits[8]{};
    int total = 0;
    const float probeX = s.x + static_cast<float>(g.walkDir) * 20.f;
    const int n = ports::foothold_path::ProbeColumn(probeX, s.y, 120, hits, 8, &total);
    for (int i = 0; i < n; ++i) {
        const float fy = static_cast<float>(hits[i].y);
        if (hits[i].wall) return true;
        if (fy > s.y + 8.f && fy <= s.y + kJumpReachPx) return true;  // AbsPos：更大 Y = 更高
    }
    return false;
}

void UpdateBlocked(const Sense& s) {
    if (!s.grounded || gMotor.dir == 0 || g.sub == Sub::Wait || g.sub == Sub::Reverse ||
        g.sub == Sub::Sidestep || (g.reactUntil && s.now < g.reactUntil) || InputLocked(s)) {
        g.blockedSince = 0;
        g.blockX = s.x;
        return;
    }
    if (std::fabs(s.x - g.blockX) >= kBlockedDx || std::fabs(s.vx) >= kBlockedVx) {
        g.blockedSince = 0;
        g.blockX = s.x;
        return;
    }
    if (!g.blockedSince) g.blockedSince = s.now;
}

void Escalate(const Sense& s, TickOut& out) {
    g.blockedSince = 0;
    g.blockX = s.x;
    const int level = g.blockLevel++;
    LogNav("human_blocked level=%d dir=%d x=%.0f cur=%u ahead=%d chase=%d", level, g.walkDir, s.x,
           (unsigned)s.cur, (int)g.ahead, g.chase ? 1 : 0);
    if (level == 0) {
        // 采证：顶住的这一段和目标段的几何（fh35 x=1074 立面到底多高，日志里一直看不出）。
        ports::foothold_path::FhGeomInfo gf{}, gt{};
        const bool okF = ports::foothold_path::FhGeom(g.act.fromFh, &gf);
        const bool okT = g.act.toFh && ports::foothold_path::FhGeom(g.act.toFh, &gt);
        LogNav("human_blocked geom from=%u (%d,%d)-(%d,%d) v=%d deg=%d comp=%d | to=%u (%d,%d)-(%d,%d) v=%d deg=%d comp=%d",
               (unsigned)g.act.fromFh, okF ? gf.x1 : 0, okF ? gf.y1 : 0, okF ? gf.x2 : 0, okF ? gf.y2 : 0,
               okF ? (gf.vertical ? 1 : 0) : -1, okF ? gf.walkDeg : -1, okF ? gf.walkComp : -1,
               (unsigned)g.act.toFh, okT ? gt.x1 : 0, okT ? gt.y1 : 0, okT ? gt.x2 : 0, okT ? gt.y2 : 0,
               okT ? (gt.vertical ? 1 : 0) : -1, okT ? gt.walkDeg : -1, okT ? gt.walkComp : -1);
        ports::foothold_path::ColumnHit hits[6]{};
        int total = 0;
        const float probeX = s.x + static_cast<float>(g.walkDir) * 20.f;
        const int n = ports::foothold_path::ProbeColumn(probeX, s.y, 160, hits, 6, &total);
        char buf[256];
        int off = snprintf(buf, sizeof(buf), "human_blocked column x=%.0f n=%d:", probeX, total);
        for (int i = 0; i < n && off > 0 && off < (int)sizeof(buf) - 32; ++i)
            off += snprintf(buf + off, sizeof(buf) - off, " fh%u@y%d%s", (unsigned)hits[i].fh, hits[i].y,
                            hits[i].wall ? "w" : "");
        LogNav("%s", buf);
    }
    // 前方平地（没有更高的台、探针没报箱子）顶住 = 键没进去，不是撞墙：
    // 先同帧补边沿，再退一步（退一步本身就是一次反向边沿），最后才考虑跳。
    // 空地原地跳是用户最反感的机器人动作（BIN 10:26:36 / 10:28:13：平地 vx=0 → 跳 → 还是 0 → 退步才动）。
    const bool obstacle = g.ahead == WalkAhead::Jump || ObstacleAhead(s);
    const bool canJump = obstacle && g.ahead != WalkAhead::Pit && StuckJumpBudget(s.now);
    if (level == 0) ++gStats.blocked;
    // 退一步的时长不固定：200~340ms 抽一个（固定 260 是签名）。
    g.reverseMs = Jitter(s.now ^ 0x7f4au, kReverseStepMinMs, kReverseStepMaxMs);
    if (level == 0) {
        if (!obstacle) {
            (void)gMotor.Refresh();
            ++gStats.relatch;
            g.relatchMs = s.now;
            LogNav("human_blocked relatch dir=%d x=%.0f", g.walkDir, s.x);
            return;
        }
        if (canJump) {
            ++g.stuckJumps;
            BeginJump(s, g.walkDir, "stuck", /*run=*/false, Mode::WalkTo);
            return;
        }
        ++gStats.reverse;
        EnterSub(Sub::Reverse, s.now, "back_off", s);
        return;
    }
    if (level == 1) {
        ++gStats.reverse;
        EnterSub(Sub::Reverse, s.now, "back_off", s);
        return;
    }
    if (level == 2) {
        if (canJump) {
            ++g.stuckJumps;
            BeginJump(s, g.walkDir, "stuck", /*run=*/false, Mode::WalkTo);
            return;
        }
        ++gStats.reverse;
        EnterSub(Sub::Reverse, s.now, "back_off", s);
        return;
    }
    if (!g.chase && g.act.fromFh && g.act.toFh) {
        ports::foothold_path::AddHopSkip(g.act.fromFh, g.act.toFh, g.act.kind);
        g.planOk = false;
        g.blockLevel = 0;
        g.afterBlockedReplan = true;
        EnterMode(Mode::Idle, Sub::None, s.now, "blocked_replan", s);
        return;
    }
    Fail(Result::Unreachable, "blocked");
    out.fromFh = g.act.fromFh;
    out.toFh = g.act.toFh;
}

// 超级赶路走路贴门：前方同层活怪则助跑跳过去（不求完美无伤，少贴身）。
// 一跳 76px（125px/s × 0.61s，顶点 80px；脚离地 ≥28px 的那段只在起跳后 7~69px 之间）。起跳窗按怪**自己的横速**
//（VecCtrl.Ap.V，怪与玩家同一套飞控字段；MobLite.vx / motionOk）分两档，别拿一个死窗套：
//   · 迎面来（rel ≤ −15px/s）：30~60px。它这 0.6s 还会再走 20~30px 到我脚下，早一点起跳正好在顶点从它头上过。
//   · 站着 / 同向走开 / 读不到速度：34~44px。落点 76px 要越过它半身 28px + 余量 → 起跳时它得在 ≤ 44px；再近起跳时
//     已贴身。BIN 追问 2026-09-10：199 包用 30~60 一刀切，站着的怪在 55~60px 处就起跳 → 落在它身上挨打，就是这条。
// 位置按 vx × 快照年龄外推（空闲 mob_scan 360ms 一帧，迎面怪这段能走 16px）。
// 离线 sim 12 只怪/图 × 1240 趟，怪走 1.5~4s 停 1~3s：不躲 2460 · 199 死窗 30~60 → 1708 · 本版 → **1425**（−17%）；
// 怪永不停步的老模型：1633 → 1553。同向走开的怪试过「不跳」（1579 / 1681，更差）、「30~60 宽窗」（1485 / 1592）、
// 「16~30 贴身跳」（1546 / 1647），都不如按 34~44 跳；读不到怪速全按窄窗 1773——所以 motionOk 很要紧。
bool MobAheadToDodge(const Sense& s, int dir) {
#ifdef XCAT_SIM_NO_DODGE  // 离线 sim 对照组：完全不躲
    (void)s; (void)dir;
    return false;
#endif
    if (!s.travelPortal || !s.grounded || dir == 0) return false;
    if (g.ahead == WalkAhead::Pit) return false;
    ports::mob::Snapshot snap{};
    if (!ports::mob::GetCached(snap) || !snap.ok) return false;
#ifdef XCAT_SIM_FIXED_WINDOW  // 离线 sim 对照组：199 的一刀切 30~60（不看怪速）
    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0 || std::fabs(m.y - s.y) > kDodgeMobDy) continue;
        const float ah = (m.x - s.x) * static_cast<float>(dir) - JumpLeadPx(kWalkVx);
        if (ah >= 30.f && ah <= 60.f) return true;
    }
    return false;
#endif
    const float d = static_cast<float>(dir);
    const float age = (snap.tickMs && s.now > static_cast<DWORD>(snap.tickMs))
                          ? static_cast<float>(s.now - static_cast<DWORD>(snap.tickMs)) / 1000.f
                          : 0.f;
    const float ageClamped = age > 0.6f ? 0.6f : age;
    int bestMob = 0;
    float bestAhead = 1e9f, bestDy = 0.f, bestVx = 0.f;
    bool bestMotion = false;
    int bestMa = -1;
    for (int i = 0; i < snap.count; ++i) {
        const auto& m = snap.mobs[i];
        if (!m.ready || m.deadType != 0 || m.hpPct <= 0) continue;
        const float dy = m.y - s.y;
        if (std::fabs(dy) > kDodgeMobDy) continue;
        const float mx = m.motionOk ? m.x + m.vx * ageClamped : m.x;
        const float ahead = (mx - s.x) * d;
        if (ahead < 0.f || ahead >= bestAhead) continue;  // 身后 / 已叠在身上的不管：跳了也是白跳
        bestAhead = ahead;
        bestMob = m.id;
        bestDy = dy;
        bestVx = m.motionOk ? m.vx : 0.f;
        bestMotion = m.motionOk;
        bestMa = m.ma;
    }
    if (bestMob == 0) return false;
    const float rel = bestVx * d;  // 怪沿我前进方向的速度：负 = 迎面
    float lo = kDodgeStillDxMin, hi = kDodgeStillDxMax;
    const char* kind = "still";
    if (bestMotion && rel <= -kDodgeMovingVx) {
        // 迎面：落地要越过它 40px → 起跳时它在 36 + |rel|·0.61 处（它这 0.61s 自己走过来的那段）。
        // 快怪 (45px/s) 63、慢怪 (20px/s) 48。sim 里怪全是 45px/s，本式 1460 ≈ 死上限 60 的 1425（同档噪声）；
        // 按 30px 余量算（快怪 73）1541 更差——落地那一拍离怪只剩 30 太贴。留本式是为了实机的慢怪：
        // 绿水灵那种 20~30px/s 迎面来，死上限 60 落地只越过 28px，正压在身上。
        lo = kDodgeMobDxMin;
        hi = 36.f - rel * kDodgeFlightSec;
        if (hi < kDodgeStillDxMax) hi = kDodgeStillDxMax;
        if (hi > kDodgeMobDxMax) hi = kDodgeMobDxMax;
        kind = "toward";
    } else if (bestMotion && rel >= kDodgeMovingVx) {
        kind = "away";  // 同向走开：也按站着的窄窗跳（见函数头注释的扫参）
    }
    // 跳键到真起跳这段（实机 60~100ms）人还在走 8~12px：窗按「起跳那一刻」算
    const float atLaunch = bestAhead - JumpLeadPx(kWalkVx);
    if (atLaunch < lo || atLaunch > hi) return false;
    LogNav("human_esp dodge_plan mob=%d kind=%s ahead=%.0f vx=%.0f ma=%d dy=%.0f lead=%.0f age=%.0fms", bestMob, kind,
           bestAhead, bestVx, bestMa, bestDy, JumpLeadPx(kWalkVx), ageClamped * 1000.f);
    return true;
}

void DriveWalk(const Sense& s, TickOut& out) {
    const float targetX = g.chase ? s.tx : static_cast<float>(g.act.wx);
    const float dxw = targetX - s.x;
    const float adx = std::fabs(dxw);
    gMotor.Up(0);

    if (g.chase && !std::isfinite(dxw)) {
        gMotor.Stop();
        g.prog = Progress::Idle;
        return;
    }

    // 极限跳：朝起跳点全速走，到点立刻起跳（不管速度够不够——再走一拍就冲下崖了），空中保方向。
    if (g.act.kind == EdgeKind::JumpAcross && !g.chase) {
        const int d = Sign(static_cast<float>(g.act.aimX) - static_cast<float>(g.act.wx));
        if (g.attempts >= kAcrossMaxTries) {
            ports::foothold_path::MarkEdgeDead(g.act.fromFh, g.act.toFh, g.act.kind);
            ++gStats.acrossFail;
            ++gStats.deadEdges;
            LogNav("human_jump across_fail from=%u to=%u wx=%d aim=%d tries=%d — edge dead for this map",
                   (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (int)g.act.wx, (int)g.act.aimX,
                   g.attempts);
            Fail(Result::Unreachable, "across_fail");
            out.fromFh = g.act.fromFh;
            out.toFh = g.act.toFh;
            return;
        }
        const float ahead = static_cast<float>(d) * (static_cast<float>(g.act.wx) - s.x);  // >0 起跳点在前
        g.prog = Progress::Moving;
        // 退回助跑的距离按台面实际剩多少算：起跳点后方到段尾的余量（构图最少留 16px），最多 40，
        // 别按死数 40 一路退出崖沿（离线 sim map30000 fh26：起跳点 1103、台尾 1119，退到 1143 掉进虚空）。
        const float backMargin = BackUpMarginPx();  // 掉头本身滑几像素 + 键延迟这段还在走
        auto roomBehind = [&]() -> float {
            return s.grounded ? EdgeDist(s.cur, s.x, -d) - backMargin : 0.f;
        };
        auto beginBackUp = [&]() {
            g.acrossBackingUp = true;
            g.acrossBackMs = s.now;
            float room = roomBehind();
            if (room > static_cast<float>(kAcrossBackUpPx)) room = static_cast<float>(kAcrossBackUpPx);
            if (room < 6.f) room = 6.f;
            g.acrossBackPx = room;
        };
        auto launchNow = [&](const char* why) {
            g.jumpDir = d;
            g.jumpWhy = why;
            g.jumpRun = true;
            g.jumpReturn = Mode::Idle;
            gMotor.Walk(s.now, d, /*urgent=*/true);
            gMotor.Up(0);
            DoLaunch(s, why, d, ahead);
            EnterMode(Mode::Jump, Sub::Air, s.now, why, s);
        };
        if (g.acrossBackingUp) {
            // 离段尾 8px（+键延迟）就掉头：掉头本身要滑几像素，贴到 4px 再掉头会滑出崖沿。
            const bool atBack = s.grounded && EdgeDist(s.cur, s.x, -d) <= backMargin;
            if (ahead >= g.acrossBackPx || atBack || s.now - g.acrossBackMs > 1200) {
                g.acrossBackingUp = false;
            } else {
                gMotor.Walk(s.now, -d, /*urgent=*/true);
                return;
            }
        }
        // 从**当前位置**起跳，落点还在不在落点台上（构图按 92% 横速留的余量，全速飞要长 9%；键延迟这段人还在
        // 往前走，落点再往前挪一个提前量；起步那几像素也算进去）。lat100 下 248 次 across 落到别处，p50 落点比
        // aim 远 16px、p90 远 46px——越过起跳点后「就地起跳」不算这两项就往台外飞。
        auto landsOnTarget = [&]() -> bool {
            const float flight = std::fabs(static_cast<float>(g.act.aimX) - static_cast<float>(g.act.wx));
            int tmin = 0, tmax = 0;
            if (!g.act.toFh || !ports::foothold_path::FhXRange(g.act.toFh, &tmin, &tmax) || tmax <= tmin) return false;
            const float landX = s.x + static_cast<float>(d) * (flight / 0.92f + 6.f + JumpLeadPx(kWalkVx));
            return landX >= static_cast<float>(tmin) + 8.f && landX <= static_cast<float>(tmax) - 8.f &&
                   EdgeDist(s.cur, s.x, d) > 10.f;
        };
        // 已越过起跳点（哪怕只过 1px）都不许就地按跳：按下去键延迟这段人还在冲，崖沿只在起跳点外 4px，
        // 跳键到时人已经走出崖沿（lat100 下 remain∈[-6,0) 的 157 次全掉了下去）。过了点就按下面的
        // 「退回去 / 就地能落到台上再跑起来跳」处理。
        if (ahead < 0.f) {
            // 越过起跳点（落回原台落远了 / 刚从别处落到起跳点前方）。退回去重新助跑要掉头两次，起跳点
            // 后方台面又常只剩几像素——键延迟 66ms 就走出崖沿（离线 sim 105040300 fh43：wx=-201、
            // 台尾 -215，退着掉到 212px 下面再爬上来，无限循环）。先算从**当前位置**起跳落点还在不在
            // 落点台上：在就朝前跑起来直接跳，不退。
            const bool fits = landsOnTarget();
            if (fits && s.grounded) {
                gMotor.Walk(s.now, d, /*urgent=*/true);
                if (s.vx * static_cast<float>(d) >= kAcrossMinLaunchVx) launchNow("across_here");
                return;
            }
            beginBackUp();
            gMotor.Walk(s.now, -d, /*urgent=*/true);
            return;
        }
        if (s.grounded && ahead <= kAcrossTriggerPx + JumpLeadPx(s.vx)) {
            if (s.vx * static_cast<float>(d) < kAcrossMinLaunchVx) {
                // 到点却没朝前的速度（刚站住 / 刚退回来 / 刚爬到绳顶）：原地跳够不着，退回去助跑；
                // 身后没地方退（绳顶台窄、起跳点贴着段尾）就原地起步朝前跑，跑起来立刻跳——晚几像素
                // 起跳落点还有 14px 余量，退出崖沿掉下去才是真输（离线 sim 110020000 fh47 绕了 120s）。
                if (roomBehind() >= 6.f) {
                    beginBackUp();
                    gMotor.Walk(s.now, -d, /*urgent=*/true);
                    return;
                }
                // 身后没地方退、就地起步：跑起来时人已过起跳点 8~20px，落点若因此出了落点台远端就别跳——
                // 跳了必掉台下（lat100 下的 across elsewhere 主力）；这条边在这个起点上做不了，换路。
                if (!landsOnTarget() && s.vx * static_cast<float>(d) >= kAcrossMinLaunchVx) {
                    LogNav("human_jump across_no_room x=%.0f wx=%d aim=%d to=%u — would overshoot, skip edge", s.x,
                           (int)g.act.wx, (int)g.act.aimX, (unsigned)g.act.toFh);
                    gMotor.Stop();
                    Fail(Result::Unreachable, "across_no_room");
                    return;
                }
                gMotor.Walk(s.now, d, /*urgent=*/true);
                if (s.vx * static_cast<float>(d) >= kAcrossMinLaunchVx) launchNow("across_short");
                return;
            }
            // 起跳点离崖沿只有几像素，等下一拍再按跳（30Hz 一拍走 4px）就走出崖沿变成掉下去
            //（离线 sim 110010000 fh123→fh117：wx=251、台尾 253，落到 240px 下的 fh86）。当拍就按。
            launchNow("across");
            return;
        }
        g.walkDir = d;
        gMotor.Walk(s.now, d, /*urgent=*/true);
        g.lastKeyMs = s.now;
        return;
    }
    // 原地跳上头顶的台：走到起跳 X（±kJumpUpTolPx），停稳（竖直跳不带横速，窄台 / 错位台才落得准），
    // 起跳后 DriveJump 不碰左右；落回原台计一次，3 次 → 死边。
    if (g.act.kind == EdgeKind::JumpUp && !g.chase) {
        if (g.attempts >= kAcrossMaxTries) {
            ports::foothold_path::MarkEdgeDead(g.act.fromFh, g.act.toFh, g.act.kind);
            ++gStats.acrossFail;
            ++gStats.deadEdges;
            LogNav("human_jump up_fail from=%u to=%u wx=%d wy=%d tries=%d — edge dead for this map",
                   (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (int)g.act.wx, (int)g.act.wy, g.attempts);
            Fail(Result::Unreachable, "jump_up_fail");
            out.fromFh = g.act.fromFh;
            out.toFh = g.act.toFh;
            return;
        }
        g.prog = Progress::Moving;
        const float dxUp = static_cast<float>(g.act.wx) - s.x;
        // 落点要在上台 X 范围内（两边各留 2）：上台可能只比下台多出 8px，按 wx±6 停会停到台外。
        int jmin = 0, jmax = 0;
        float lo = static_cast<float>(g.act.wx) - kJumpUpTolPx, hi = static_cast<float>(g.act.wx) + kJumpUpTolPx;
        if (g.act.toFh && ports::foothold_path::FhXRange(g.act.toFh, &jmin, &jmax) && jmax > jmin) {
            lo = static_cast<float>(jmin) + 2.f;
            hi = static_cast<float>(jmax) - 2.f;
            if (lo > hi) lo = hi = static_cast<float>(g.act.wx);
        }
        // 既要在上台范围内，也要贴着 wx（±10）：上台是斜坡时构图把 wx 放在高差最小处，站偏 20px 就够不着。
        const bool inside = s.x >= lo && s.x <= hi && std::fabs(dxUp) <= kJumpUpTolPx;
        if (!inside) {
            g.jumpUpSettleMs = 0;
            g.walkDir = Sign(dxUp);
            if (std::fabs(dxUp) > 24.f) {
                gMotor.Walk(s.now, g.walkDir, /*urgent=*/false);
                g.nudgeUntil = 0;
            } else if (!g.nudgeUntil) {
                // 近了改微步（45ms 按 + 140ms 等）：全速冲进 8px 的带一停就滑出去，来回摆。
                gMotor.Walk(s.now, g.walkDir, /*urgent=*/true);
                g.nudgeUntil = s.now + kNudgeMs;
            } else if (s.now < g.nudgeUntil) {
                gMotor.Walk(s.now, g.walkDir, /*urgent=*/true);
            } else {
                gMotor.Stop();
                if (s.now - g.nudgeUntil >= kNudgeSettleMs) g.nudgeUntil = 0;
            }
            g.lastKeyMs = s.now;
            return;
        }
        g.nudgeUntil = 0;
        gMotor.Stop();
        if (!s.grounded) return;
        if (!g.jumpUpSettleMs) g.jumpUpSettleMs = s.now;
        const bool still = std::fabs(s.vx) <= kJumpUpMaxVx;
        if (!still && s.now - g.jumpUpSettleMs < kSettleGiveUpMs) return;
        if (InputLocked(s)) return;
        g.jumpDir = 0;
        g.jumpWhy = "up";
        g.jumpRun = false;
        g.jumpReturn = Mode::Idle;
        g.launchMs = 0;
        g.jumpUpSettleMs = 0;
        LogNav("human_jump up_launch x=%.0f wx=%d band=[%.0f,%.0f] vx=%.0f y=%.0f to=%u", s.x, (int)g.act.wx, lo,
               hi, s.vx, s.y, (unsigned)g.act.toFh);
        EnterMode(Mode::Jump, Sub::Launch, s.now, "jump_up", s);
        return;
    }
    // 怪叠在身上（|dx|<10 调用方不算可打）：退到侧位再打，别站着等它走开。
    // 方向进态时定死（dx 过零会来回翻），退的那侧前方是坑就换另一侧，两侧都是坑就算了。
    if (g.chase && !s.travelPortal && s.grounded) {
        if (g.sub == Sub::Sidestep) {
            if (adx >= kSidestepDx || s.now - g.subMs > 900) {
                EnterSub(Sub::None, s.now, adx >= kSidestepDx ? "sidestep_done" : "sidestep_max", s);
            } else {
                gMotor.Walk(s.now, g.sideDir, /*urgent=*/true);
                g.lastKeyMs = s.now;
                g.prog = Progress::Moving;
                return;
            }
        } else if (adx < kOnTopDx && g.sub == Sub::None) {
            int dir = (std::fabs(dxw) >= 1.f) ? -Sign(dxw) : (gMotor.lastDir ? -gMotor.lastDir : 1);
            if (ports::foothold_path::ProbeWalkAhead(s.x, s.y, dir, s.cur, 24) == WalkAhead::Pit) dir = -dir;
            if (ports::foothold_path::ProbeWalkAhead(s.x, s.y, dir, s.cur, 24) != WalkAhead::Pit) {
                g.sideDir = dir;
                EnterSub(Sub::Sidestep, s.now, "mob_on_top", s);
                gMotor.Walk(s.now, dir, /*urgent=*/true);
                g.prog = Progress::Moving;
                return;
            }
        }
    }
    if (g.chase && adx < 1.f) {
        gMotor.Stop();
        g.prog = Progress::Idle;
        return;
    }
    if (g.reactUntil && s.now < g.reactUntil) {
        gMotor.Stop();
        g.prog = Progress::Idle;
        return;
    }
    g.reactUntil = 0;

    // 卡住退一步
    if (g.sub == Sub::Reverse) {
        gMotor.Walk(s.now, -g.walkDir, /*urgent=*/true);
        const DWORD stepMs = g.reverseMs ? g.reverseMs : kReverseStepMinMs;
        if (s.now - g.subMs >= stepMs) EnterSub(Sub::None, s.now, "back_off_done", s);
        g.prog = Progress::Blocked;
        return;
    }

    // 目标朝自己走来：停步等它进带（passive 怪乱走，限时）。
    if (s.rel < 0) {
        if (!g.relNegSince) g.relNegSince = s.now;
    } else {
        g.relNegSince = 0;
    }
    if (g.sub == Sub::Wait) {
        // 退出看 |dx| 有没有在缩，不看一拍的速度符号：怪位置是采样的，速度 EMA 一拍没动就翻号。
        const float adxNow = std::fabs(s.dx);
        if (adxNow < g.waitLastAdx - kWaitProgressPx) {
            g.waitLastAdx = adxNow;
            g.waitProgressMs = s.now;
        }
        const bool stalled = s.now - g.waitProgressMs >= kWaitStallMs;
        const bool keep = adxNow <= kWaitDx && s.now - g.subMs < kWaitMaxMs && !stalled;
        if (!keep) {
            g.waitDoneMs = s.now;
            EnterSub(Sub::None, s.now, (stalled || adxNow > kWaitDx) ? "tgt_turned" : "wait_max", s);
        } else {
            gMotor.Stop();
            g.prog = Progress::Waiting;
            return;
        }
    } else if (WaitRule(s)) {
        g.waitLastAdx = std::fabs(s.dx);
        g.waitProgressMs = s.now;
        EnterSub(Sub::Wait, s.now, "tgt_coming", s);
        gMotor.Stop();
        g.prog = Progress::Waiting;
        return;
    }

    const int wantDir = Sign(dxw);
    if (!g.walkDir) {
        g.walkDir = wantDir;
    } else if (!s.grounded) {
        // 腾空不折返：走下台阶会带着走速越过路点 20~35px（BIN 13:58:04 wx=2475 落到 2497），
        // 空中按反向只是把人转个身、落地重规划又转回来——用户看到的「下台阶左右回头」就是这个。
    } else if (g.chase || adx > kWxTol) {
        g.walkDir = wantDir;  // 路点带内不折返（惯性）
    }

    UpdateBlocked(s);
    if (!g.blockedSince) g.earlyRelatchDone = false;
    if (g.blockedSince && s.now - g.blockedSince >= kBlockedMs) {
        Escalate(s, out);
        return;
    }
    // 起步边沿被吞的早补：按住方向 300ms 人还站着（ma 站立、vx≈0）、前方又是平地 → 先补一次「松→按」边沿，
    // 不等 900ms 的 blocked。BIN 2026-09-09 16:00:19 走路刚 armed 第一次 Hold 没进（set=1 kbd=1 但 ma=4 不动），
    // 站了 1s 才靶 relatch 走起来；每次起步白站 1s 用户看得见。只补一次，补了还不动交给 Escalate。
    if (g.blockedSince && !g.earlyRelatchDone && s.now - g.blockedSince >= kEarlyRelatchMs && s.grounded &&
        !IsRopeMa(s.ma) && g.ahead != WalkAhead::Jump && !ObstacleAhead(s)) {
        g.earlyRelatchDone = true;
        (void)gMotor.Refresh();
        ++gStats.relatch;
        g.relatchMs = s.now;
        LogNav("human_blocked early_relatch dir=%d x=%.0f ma=%d", g.walkDir, s.x, s.ma);
    }

    // ESP：箱子 / 缺口 / 台阶。悬崖立面不当障碍，纯坑不跳。
    g.ahead = s.grounded ? ports::foothold_path::ProbeWalkAhead(s.x, s.y, g.walkDir, s.cur, 42)
                         : WalkAhead::Floor;
    const bool stepUp = !g.chase && s.grounded && NearWalkStepUp(s.x, g.walkDir, g.act.fromFh, g.act.toFh);
    const bool jumpCd = gMotor.jumpMs && s.now - gMotor.jumpMs < kEspJumpCdMs;
    if (s.grounded && !jumpCd && (stepUp || (g.ahead == WalkAhead::Jump && adx >= kEspJumpMinRemain))) {
        BeginJump(s, g.walkDir, stepUp ? "step" : "esp", /*run=*/true, Mode::WalkTo);
        gMotor.Walk(s.now, g.walkDir, /*urgent=*/true);
        g.prog = Progress::Moving;
        return;
    }
    if (s.grounded && !jumpCd && MobAheadToDodge(s, g.walkDir)) {
        BeginJump(s, g.walkDir, "dodge", /*run=*/true, Mode::WalkTo);
        gMotor.Walk(s.now, g.walkDir, /*urgent=*/true);
        g.prog = Progress::Moving;
        LogNav("human_esp dodge dir=%d x=%.0f", g.walkDir, s.x);
        return;
    }
    if (g.ahead == WalkAhead::Pit) {
        static DWORD sPit = 0;
        if (!sPit || s.now - sPit > 800) {
            sPit = s.now;
            LogNav("human_esp pit dir=%d cur=%u x=%.0f", g.walkDir, (unsigned)s.cur, s.x);
        }
    }

    // 长距离偶尔顿一下（150~320ms，间隔 4~9s）：人不会匀速直线走满全程。离目标近了不顿。
    // 巡逻 / 贴门 / 去挂绳（travelPortal 口径）同样顿。
    if (g.chase && adx > kFidgetMinDx) {
        if (!g.fidgetNextMs) g.fidgetNextMs = s.now + Jitter(s.now ^ 0x9e37u, kFidgetGapMinMs, kFidgetGapMaxMs);
        if (g.fidgetUntil && s.now < g.fidgetUntil) {
            gMotor.Stop();
            g.prog = Progress::Moving;
            return;
        }
        if (s.now >= g.fidgetNextMs) {
            g.fidgetUntil = s.now + Jitter(s.now ^ 0x5bd1u, kFidgetMinMs, kFidgetMaxMs);
            g.fidgetNextMs = s.now + Jitter(s.now ^ 0x27d4u, kFidgetGapMinMs, kFidgetGapMaxMs);
            g.blockX = s.x;
            g.blockedSince = 0;
            gMotor.Stop();
            g.prog = Progress::Moving;
            return;
        }
    }

    gMotor.Walk(s.now, g.walkDir);
    g.lastKeyMs = s.now;
    g.prog = (g.chase && s.rel > 0) ? Progress::Chasing : Progress::Moving;
}

// ───────── 抓绳 ─────────
// 进 Air 必须记起跳/离地时刻：落地判定与「700ms 仍悬空 = 上绳」都以它为基准
//（BIN 14:23:49 align→air 没记，落地 6s 都判不出，人在下面被怪推着走）。
void EnterGrabAir(const Sense& s, const char* why) {
    if (!g.launchMs) g.launchMs = s.now;
    // 台外绳：记下朝绳方向，空中判「飞过头」用（走着掉下台沿的也算）。
    if (g.pastEdge && !g.edgeDir) g.edgeDir = Sign(static_cast<float>(g.act.wx) - s.x);
    EnterSub(Sub::Air, s.now, why, s);
}

void GrabLandedBack(const Sense& s, TickOut& out) {
    // 竖直跳抓绳：起跳了却从没离地 = 跳键被吃（失焦 / 出刀锁尾），不算这根绳够不着。
    const bool inputFail = g.needJump && g.launchMs && JumpWasInputFailure(/*needHorizontal=*/false);
    if (inputFail) {
        LogNav("human_climb grab_nojump wx=%d try=%d fails=%d — key eaten, retry", (int)g.act.wx, g.attempts,
               g.inputFails);
    } else {
        ++g.attempts;
    }
    g.settleMs = 0;
    g.nudgeUntil = 0;
    g.nudges = 0;  // 每次重来都可以再微步（BIN 18:32:39 计数没清 → 立刻 align_giveup）
    g.launchMs = 0;
    g.grabMiss = false;
    g.grabMissWhy = "";
    if (g.attempts >= kGrabMaxAttempts) {
        ++gStats.grabFail;
        // 悬空绳竖直跳 3 次都落回原台 = 这根绳从这块台物理上够不着（图上高差算错 / 绳底比看起来高）。
        // 记成整图死边：8s ban 到期后规划又会选它，人就一轮轮在高绳下面跳（upload 2026-09-09 11:43
        // fh66→绳 1606 每 25s 一轮）。该绳只能从别的台 / 别的绳过去，让 Dijkstra 自己绕。
        if (g.needJump && g.act.fromFh && g.act.toFh) {
            ports::foothold_path::MarkEdgeDead(g.act.fromFh, g.act.toFh, g.act.kind);
            ++gStats.deadEdges;
            LogNav("human_climb rope_unreachable from=%u to=%u wx=%d wy=%d py=%.0f dy=%.0f — edge dead for this map (n=%d)",
                   (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (int)g.act.wx, (int)g.act.wy, s.y,
                   static_cast<float>(g.act.wy) - s.y, ports::foothold_path::DeadEdgeCount());
        }
        Fail(Result::Timeout, "grab_fail");
        out.fromFh = g.act.fromFh;
        out.toFh = g.act.toFh;
        return;
    }
    // 落得远（被顶飞 / 台外绳）就先走回去，别在 60px 外按 ↓ 等 1.5s。
    const float adxRope = std::fabs(static_cast<float>(g.act.wx) - s.x);
    const Sub next = (g.pastEdge || adxRope > kApproachSpotWx) ? Sub::Approach : Sub::Align;
    EnterMode(Mode::Grab, next, s.now, "landed_back", s);
}

// 被怪顶飞（不是我们起的跳）：走 Recover，不算一次抓绳尝试。
bool GrabKnocked(const Sense& s) {
    if (s.onRope) return false;
    if (std::fabs(s.vx) < kKnockVx) return false;
    if (s.grounded) return true;
    // 空中 |vx|≥220：助跑跳最多 125，只能是被顶
    return true;
}

void GrabToRecover(const Sense& s, const char* why) {
    g.recoverReturn = Mode::Grab;
    g.launchMs = 0;
    g.settleMs = 0;
    g.nudgeUntil = 0;
    LogNav("human_climb knock vx=%.0f vy=%.0f wx=%d px=%.0f cur=%u sub=%s", s.vx, s.vy, (int)g.act.wx,
           s.x, (unsigned)s.cur, SubName(g.sub));
    EnterMode(Mode::Recover, Sub::None, s.now, why, s);
}

void DriveGrab(const Sense& s, TickOut& out) {
    const int vert = ClimbVert(s.y, g.act.toFh, g.act.wx, g.act.kind, g.act.ropeYBot, g.act.ropeYTop);
    const float ropeX = static_cast<float>(g.act.wx);
    const float adxRope = std::fabs(ropeX - s.x);
    const float adxSpot = std::fabs(static_cast<float>(g.spotX) - s.x);
    const int towardRope = Sign(ropeX - s.x);
    const int towardSpot = Sign(static_cast<float>(g.spotX) - s.x);
    g.prog = Progress::Moving;

    // 被击退（走路 125 不算；地面或空中都算）：先站稳再来，不算抓绳尝试。
    // BIN 18:32:36 Align 里被蜗牛顶飞 → 当成 airborne 抓绳 → landed_back 计次 → 3 次 grab_fail。
    if (g.sub != Sub::RunUp && GrabKnocked(s)) {
        GrabToRecover(s, "knock");
        return;
    }

    switch (g.sub) {
        case Sub::Approach: {
            if (!s.grounded) {
                // 走着走着离台了（台外绳 / 走过头 / 走路途中就上了绳）。没按跳就腾空的把此刻当起跳时刻
                //（sawAir 置 true：不进跳键延迟样本）。
                if (!s.onRope) {
                    g.launchMs = s.now;
                    g.sawAir = true;
                }
                EnterGrabAir(s, s.onRope ? "mounted" : "airborne");
                return;
            }
            // 走着按 ↑ 无害且能顺路上绳；↓ 走着按会趴下走不动，只在停稳后按。
            gMotor.Up((vert == 1 && adxRope <= kNearRopeUpWx) ? 1 : 0);
            // 台外绳：起跳点在绳前 ≈48px（不是台沿）。离绳 62~90px 进助跑；已经比 62 近（落回原台 /
            // 极限跳落点离绳近）先退开再冲，否则跑不满速就到起跳点、或过了起跳点才起跳（必飞过头）。
            if (g.needJump && g.pastEdge) {
                const float backTo = EdgeGrabLaunchDx(kWalkVx) + kEdgeGrabBackupPx;
                // 退开时离另一端留 12px + 1.5×键延迟走的距离：贴到 7px 掉头，键晚 66ms 就退出窄台
                //（离线 sim 110010000 fh160；101030403 fh11 退到离端 6px 掉头仍滑出去）。
                const float backMargin = BackUpMarginPx();
                if (adxRope < backTo && EdgeDist(s.cur, s.x, -towardRope) > backMargin) {
                    gMotor.Up(0);
                    gMotor.Walk(s.now, -towardRope, /*urgent=*/true);
                    g.lastKeyMs = s.now;
                    return;
                }
                if (adxRope <= kEdgeGrabRunUpDx) {
                    EnterSub(Sub::RunUp, s.now, "edge_runup", s);
                    return;
                }
            }
            if (adxSpot > kApproachSpotWx) {
                gMotor.Walk(s.now, towardSpot);
                g.lastKeyMs = s.now;
                return;
            }
            if (!g.needJump) {
                EnterSub(adxRope <= kFloorGrabWx ? Sub::Settle : Sub::Align, s.now, "floor_rope", s);
            } else if (g.pastEdge) {
                EnterSub(Sub::RunUp, s.now, "edge_runup", s);
            } else {
                EnterSub(Sub::Align, s.now, "align", s);
            }
            return;
        }
        case Sub::Align: {
            // 悬空梯必须竖直跳，横向偏差 >6px 抓不到（BIN 12:29 偏 13px 连跳 12 次）；落地梯 12px。
            // 微步期间 ↓ 不能按（趴下就挪不动），↑ 可以。
            gMotor.Up(vert == 1 ? 1 : 0);
            if (!s.grounded) {
                if (!s.onRope) {
                    g.launchMs = s.now;  // 微步走出台沿 / 被顶：腾空时刻当起跳时刻，别沿用旧跳的 launchMs
                    g.sawAir = true;
                }
                EnterGrabAir(s, s.onRope ? "mounted" : "airborne");
                return;
            }
            // 落地梯第一次 12px 内就试；站着 ↑/↓ 1.5s 没上绳再来对齐时收到 4px（游戏的上绳 X 窗
            // 比 12 窄时，旧口径 adx=12 不算「要微步」，原地干等 3 轮就 grab_fail）。
            const float alignWx = g.needJump ? kGrabWx : (g.nudges > 0 || g.floorTight ? kFloorGrabTightWx : kFloorGrabWx);
            if (adxRope <= alignWx || g.nudges >= kAlignMaxNudges) {
                gMotor.Stop();
                EnterSub(Sub::Settle, s.now, adxRope <= alignWx ? "aligned" : "align_giveup", s);
                return;
            }
            if (InputLocked(s)) {
                // 出刀动画锁 / 击退硬直里按键无效：等，不数微步。
                gMotor.Stop();
                g.nudgeUntil = 0;
                return;
            }
            if (!g.nudgeUntil) {
                gMotor.Walk(s.now, towardRope, /*urgent=*/true);
                g.nudgeUntil = s.now + kNudgeMs;
            } else if (s.now < g.nudgeUntil) {
                gMotor.Walk(s.now, towardRope, /*urgent=*/true);
            } else {
                gMotor.Stop();
                if (s.now - g.nudgeUntil >= kNudgeSettleMs) {
                    g.nudgeUntil = 0;
                    ++g.nudges;
                }
            }
            return;
        }
        case Sub::Settle: {
            if (s.grounded && adxRope > kApproachSpotWx) {
                // 离绳太远（被顶开 / 微步没走动）：回去走，别在这按 ↓ 等。
                g.settleMs = 0;
                EnterSub(Sub::Approach, s.now, "settle_far", s);
                return;
            }
            gMotor.Stop();
            gMotor.Up(vert);
            if (!g.settleMs) {
                g.settleMs = s.now;
                LogNav("human_climb settle wx=%d spot=%d hang=%d past=%d adx=%.0f vx=%.0f py=%.0f",
                       (int)g.act.wx, (int)g.spotX, g.needJump ? 1 : 0, g.pastEdge ? 1 : 0, adxRope,
                       s.vx, s.y);
            }
            if (!s.grounded) {
                // 落地梯 ↑ 直接上绳。
                g.launchMs = 0;
                EnterGrabAir(s, "mounted");
                return;
            }
            const DWORD since = s.now - g.settleMs;
            if (!g.needJump) {
                // 站着按 ↑/↓ 没上绳：先收紧到 4px 内再试，还不行才算一次失手。
                if (since >= kFloorGrabWaitMs) {
                    if (adxRope > kFloorGrabTightWx && g.nudges < kAlignMaxNudges) {
                        g.settleMs = 0;
                        g.floorTight = true;
                        EnterSub(Sub::Align, s.now, "floor_nudge", s);
                    } else {
                        GrabLandedBack(s, out);
                    }
                }
                return;
            }
            const bool settled = std::fabs(s.vx) <= kSettleVx && since >= kSettleMs;
            const bool waited = since >= kSettleGiveUpMs;
            // 冷却只看本动作的抓绳跳：刚做完极限跳 / ESP 跳落地就到绳下，不该再干站 800ms。
            const bool cd = g.grabJumpMs && s.now - g.grabJumpMs < kGrabRetryMs;
            if ((settled || waited) && !cd) {
                // 停稳后再核一次 X：进 Settle 时 adx=2 但停键晚到，人还滑了十几像素（100ms 键延迟滑 12px），
                // 站偏了竖直跳必抓空。偏出 6px 回 Align 微步，不算失手。
                if (adxRope > kGrabWx && g.nudges < kAlignMaxNudges) {
                    g.settleMs = 0;
                    EnterSub(Sub::Align, s.now, "settle_drift", s);
                    return;
                }
                g.grabJumpMs = s.now;
                DoLaunch(s, "grab", 0, adxRope);
                EnterGrabAir(s, "vertical_jump");
            }
            return;
        }
        case Sub::RunUp: {
            // 台外绳：带着横向动量朝绳跳出去，空中 ↑ 抓。绳只在下落段抓得住，起跳点要让
            // **顶点**落在绳前几像素（EdgeGrabLaunchDx），不是跑到台沿再跳（见常量处 BIN 推导）。
            gMotor.Up(0);
            if (!s.grounded) {
                // 没按跳就腾空 = 走出了台沿（退助跑退过头 / 键延迟）：别当成本次的跳；把腾空时刻当起跳时刻
                // 供 since 用，miss 判定按落点处理。朝绳那侧走出去的还有可能顺势抓到。
                // sawAir 置 true：这不是按键后的离地，不能进跳键延迟的样本。
                g.edgeDir = towardRope;
                g.launchMs = s.now;
                g.sawAir = true;
                EnterGrabAir(s, "off_edge");
                return;
            }
            // 冷却只看本动作的抓绳跳（极限跳落地接台外绳时旧口径要干站 ~350ms，BIN 16:00:38）。
            const bool cd = g.grabJumpMs && s.now - g.grabJumpMs < kGrabRetryMs;
            if (cd) {
                // 上一次抓绳跳的冷却里别往台沿冲（冲过起跳点再跳必飞过头）：站着等，冷却完再决定退还是冲。
                gMotor.Stop();
                g.subMs = s.now;
                return;
            }
            const float edge = EdgeDist(s.cur, s.x, towardRope);
            const float fullDx = EdgeGrabLaunchDx(kWalkVx);
            const float vTo = s.vx * static_cast<float>(towardRope);  // 朝绳的横速（背着绳为负）
            const bool running = vTo >= kRunVx;
            // 起跳距离随当拍**朝绳**的横速缩放（vx=62 → 29px，跑满 125 → 48px）+ 键延迟提前量；背着绳跑
            //（刚落地还带着反向动量，离线 sim 50001 fh66）不算速度，等掉头跑起来再说。
            const float launchDx = EdgeGrabLaunchDx(vTo > 0.f ? vTo : 0.f) + JumpLeadPx(vTo > 0.f ? vTo : 0.f);
            // 已在起跳点内侧：没跑起来的（冷却里站着 / 落回来的位置太近）退开再冲；**跑着**但比本速起跳点还近
            // 超过抓绳 X 窗（10px）的也退——现在起跳顶点必飞过绳（离线 sim 102000000 fh352：落地后带着 -125
            // 直接进 Grab、adx=20 就跳，顶点在绳后 18px → overshoot ×4；lat100 下 47 次 overshoot 全是这样）。
            const bool tooClose = running ? (adxRope < launchDx - kRopeGrabXTol) : (adxRope < fullDx);
            float minLaunchVx = kAcrossMinLaunchVx;
            if (tooClose) {
                // 退的门槛比 Approach 停退的门槛多 16px（滞回）：Approach 退到离端 = 边距就停、转身 2px 又满足
                // 「> 边距」再退 → 退/冲 0.5s 一轮直到 timeout（离线 sim 103010000 fh460 ×17）。
                if (EdgeDist(s.cur, s.x, -towardRope) > BackUpMarginPx() + 16.f) {
                    gMotor.Stop();
                    EnterSub(Sub::Approach, s.now, "edge_backup", s);
                    return;
                }
                // 身后没地方退（窄台）：绳只在下落段抓得住，穿绳必须在顶点之后 → 刹车到「顶点时刻刚到绳」
                // 的速度再跳：v* = (adx − 6) / 顶点时间，夹在 [30, 125]；这种慢起跳不受 60px/s 起跳下限约束
                //（空中不再朝绳推，见 Air 里的 tCross 判定，否则空中加速又把穿绳时刻提前到上升段）。
                float vStar = (adxRope - 6.f) * 1000.f / static_cast<float>(kJumpApexMs);
                if (vStar < 30.f) vStar = 30.f;
                if (vStar > kWalkVx) vStar = kWalkVx;
                if (vTo > vStar + 15.f) {
                    gMotor.Stop();
                    return;
                }
                if (vStar < minLaunchVx) minLaunchVx = vStar;
            }
            gMotor.Walk(s.now, towardRope, /*urgent=*/true);
            // 绳离台沿太远（到台沿还没到起跳距离）→ 台沿兜底起跳。
            const bool atLaunch = vTo >= minLaunchVx && adxRope <= launchDx;
            const bool atEdge = edge <= kEdgeJumpPx + JumpLeadPx(vTo > 0.f ? vTo : 0.f) && vTo > 0.f;
            if (atLaunch || atEdge) {
                g.edgeDir = towardRope;
                g.grabJumpMs = s.now;
                LogNav("human_climb edge_launch why=%s adx=%.0f want=%.0f edge=%.0f vx=%.0f apex_x=%.0f rope=%d",
                       atLaunch ? "apex" : "edge", adxRope, launchDx, edge, s.vx,
                       s.x + s.vx * static_cast<float>(kJumpApexMs) / 1000.f, (int)g.act.wx);
                DoLaunch(s, "grab_run", towardRope, adxRope);
                EnterGrabAir(s, "edge_jump");
            }
            return;
        }
        case Sub::Air:
        default: {
            const DWORD since = g.launchMs ? s.now - g.launchMs : kJumpLandMinMs;
            // 跳抓没抓住的两种样子：台外绳飞过绳 >14px 还悬空（反向只减速 170px/s²，回不来）/
            // 任何跳抓掉到绳底以下。判定后松 ↑（别顺手抓下面别的绳爬到不知哪去），等落地按落点处理；
            // 也别再让「悬空 700ms = 上绳」把它当成 Climb（BIN 16:00:39 x=197 y=-185 假 on_rope）。
            if (g.needJump && !g.grabMiss && !s.grounded && !MaLooksRope(s) && since >= kJumpLandMinMs) {
                // 绳底：PlanFirst 给的 ropeYBot；合成动作（休息）只有 wy=绳底。ClimbDown 的 wy 是绳顶，不能用。
                const bool haveBot = g.act.ropeYBot != 0 || g.act.kind == EdgeKind::ClimbUp;
                const float ropeBot = static_cast<float>(g.act.ropeYBot ? g.act.ropeYBot : g.act.wy);
                const float past =
                    (g.pastEdge && g.edgeDir) ? (s.x - ropeX) * static_cast<float>(g.edgeDir) : 0.f;
                const bool overshoot = past > kRopeGrabXTol;
                // 「掉到绳底以下」只在下落段判：起跳 132ms 时人还在上升（+54px），绳底高 74~78px 的绳
                // 这一拍必然「还在绳底之下」，旧口径当场判 miss 松 ↑ → 到顶抓不住 → 3 次标死边
                //（离线 sim 110040000 x=-1476 / 103010000 fh331 复现）。
                const bool below = haveBot && s.vy <= 0.f && s.y < ropeBot - kGrabMissBelowPx;
                if (overshoot || below) {
                    g.grabMiss = true;
                    g.grabMissWhy = overshoot ? "overshoot" : "below";
                    LogNav("human_climb grab_miss why=%s edge=%d x=%.0f y=%.0f past=%.0f vx=%.0f vy=%.0f "
                           "ropeBot=%.0f since=%ums try=%d",
                           g.grabMissWhy, g.pastEdge ? 1 : 0, s.x, s.y, past, s.vx, s.vy, ropeBot,
                           (unsigned)since, g.attempts);
                }
            }
            // 跳抓：空中只按 ↑；站着上绳（落地梯 / 绳顶 ↓）沿原方向。
            // 台外绳离绳还远才允许朝绳飘（BIN 11:55 vx=±125 穿绳窗口）。
            gMotor.Up(g.grabMiss ? 0 : (g.needJump ? 1 : vert));
            // 绳只在下落段抓得住：还在上升且按现在的横速会在顶点**之前**就穿过绳 → 别再朝绳推（空中推会加速），
            // 反向轻刹把穿绳时刻拖到顶点之后。离线 sim：慢起跳 vx=25 空中推到 78，顶点前 4px 穿绳 → overshoot ×8。
            const float vToAir = s.vx * static_cast<float>(towardRope);
            const float tUp = s.vy > 0.f ? s.vy / ports::foothold_path::kJumpGravityPxPerSec2 : 0.f;
            const float tCross = vToAir > 5.f ? adxRope / vToAir : 9.f;
            const bool tooFast = g.pastEdge && !g.grabMiss && s.vy > 0.f && tCross < tUp + 0.03f;
            if (tooFast) {
                gMotor.Walk(s.now, -towardRope, /*urgent=*/true);
            } else if (g.pastEdge && !g.grabMiss && adxRope > kAirSteerStopWx) {
                gMotor.Walk(s.now, towardRope, /*urgent=*/true);
            } else {
                gMotor.Stop();
            }
            static DWORD sAir = 0;
            if (!sAir || s.now - sAir > 800) {
                sAir = s.now;
                LogNav("human_climb air wx=%d adx=%.0f vx=%.0f vy=%.0f ma=%d walk=%d try=%d to=%u%s",
                       (int)g.act.wx, adxRope, s.vx, s.vy, s.ma,
                       (g.pastEdge && !g.grabMiss && adxRope > kAirSteerStopWx) ? towardRope : 0,
                       g.attempts, (unsigned)g.act.toFh, g.grabMiss ? " miss" : "");
            }
            if (s.grounded && since >= kJumpLandMinMs) {
                if (OnFhOrTwin(s, g.act.toFh)) {
                    NoteActionDone(s);
                    g.planOk = false;
                    EnterMode(Mode::Idle, Sub::None, s.now, "climb_done", s);
                    return;
                }
                if (s.cur != g.act.fromFh) {
                    // 落到别的台：让规划重新算。跳抓失手掉到下面 = 这条边的一次失败，
                    // 规划会把 attempts 归零，所以按边记；3 次标死边，别 9s 一轮无限绕（BIN 16:00 3 轮）。
                    if (g.grabMiss && g.act.fromFh && g.act.toFh) {
                        const int n = NoteEdgeGrabMiss(g.act.fromFh, g.act.toFh, s.now);
                        LogNav("human_climb grab_miss_landed why=%s from=%u to=%u cur=%u x=%.0f y=%.0f n=%d",
                               g.grabMissWhy, (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (unsigned)s.cur,
                               s.x, s.y, n);
                        if (n >= kGrabMaxMisses) {
                            ports::foothold_path::MarkEdgeDead(g.act.fromFh, g.act.toFh, g.act.kind);
                            ++gStats.deadEdges;
                            ++gStats.grabFail;
                            LogNav("human_climb rope_unreachable from=%u to=%u wx=%d wy=%d — edge grab missed %d× "
                                   "(edge dead for this map, n=%d)",
                                   (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (int)g.act.wx, (int)g.act.wy, n,
                                   ports::foothold_path::DeadEdgeCount());
                        }
                    }
                    g.planOk = false;
                    EnterMode(Mode::Idle, Sub::None, s.now, "landed_elsewhere", s);
                    return;
                }
                GrabLandedBack(s, out);
                return;
            }
            // 「悬空 700ms = 已在绳上」只在人真像挂着（横竖速≈0、X 在绳上）时才信：走出台沿掉下去时
            // launchMs 是上一跳的旧值，since 一上来就 >700 → 半空判成 on_rope，Climb 里等到落地
            //（离线 sim 110010000 fh160 退助跑退出窄台 → 假 on_rope → 掉 350px → 爬回来 → 再退出去）。
            const bool hangsStill = std::fabs(s.vx) < 5.f && std::fabs(s.vy) < 5.f && adxRope <= kRopeGrabXTol;
            if (!s.grounded && (MaLooksRope(s) || (since >= kRopeSureMs && !g.grabMiss && hangsStill))) {
                g.climbY = s.y;
                g.climbYMs = s.now;
                g.stallStage = 0;
                EnterMode(Mode::Climb, Sub::None, s.now, MaLooksRope(s) ? "on_rope_ma" : "on_rope", s);
            }
            return;
        }
    }
}

// ───────── 绳上 / 下绳 ─────────
void DriveClimb(const Sense& s, TickOut& out) {
    g.prog = Progress::Moving;
    if (s.grounded) {
        if (OnFhOrTwin(s, g.act.toFh)) {
            NoteActionDone(s);
            g.planOk = false;
            EnterMode(Mode::Idle, Sub::None, s.now, "climb_done", s);
            return;
        }
        if (s.cur == g.act.fromFh) {
            LogNav("human_climb fell_back cur=%u try=%d", (unsigned)s.cur, g.attempts);
            GrabLandedBack(s, out);
            return;
        }
        g.planOk = false;
        EnterMode(Mode::Idle, Sub::None, s.now, "landed_elsewhere", s);
        return;
    }
    const int vert = ClimbVert(s.y, g.act.toFh, g.act.wx, g.act.kind, g.act.ropeYBot, g.act.ropeYTop);
    gMotor.Stop();

    // 休息：只爬到挂点就松键挂着，不到顶、不下绳。
    if (gRest.active && gRest.ropeOk) {
        if (s.y >= gRest.hangY - 6.f) {
            gMotor.Up(0);
            g.prog = Progress::Waiting;
        } else {
            gMotor.Up(1);
            g.prog = Progress::Moving;
        }
        return;
    }

    if (std::fabs(s.y - g.climbY) >= 8.f) {
        g.climbY = s.y;
        g.climbYMs = s.now;
        g.stallStage = 0;
    }
    const DWORD stallMs = g.climbYMs ? (s.now - g.climbYMs) : 0;
    const bool yStall = stallMs >= kClimbYStallMs;
    // Y 不动的自救：① 松键 120ms 再按（键态被外来事件冲掉 / 卡绳段接缝，BIN 14:28:30 y=-253 停 3s）
    //              ② 还不动 → 侧跳脱离，落到哪算哪再规划；不再干等到 timeout 挂在绳上。
    if (yStall && g.stallStage == 0) {
        g.stallStage = 1;
        g.regripUntil = s.now + 120;
        LogNav("human_climb regrip y=%.0f ma=%d wx=%d", s.y, s.ma, (int)g.act.wx);
    }
    if (g.stallStage == 1 && s.now < g.regripUntil) {
        gMotor.Up(0);
        return;
    }
    // 绳上侧跳 / 绳到绳：爬到起跳 Y 就朝落点跳，不走顶/底逻辑。
    if (g.act.kind == EdgeKind::RopeJump) {
        const float dy = static_cast<float>(g.act.wy) - s.y;
        if (std::fabs(dy) <= kRopeJumpYTol || (yStall && std::fabs(dy) <= 40.f)) {
            g.offDir = Sign(static_cast<float>(g.act.aimX) - static_cast<float>(g.act.wx));
            g.offGrab = g.act.ropeId2 != 0;
            g.offJumpMs = 0;
            g.hopOffNoJumpUntil = 0;
            char tb[24];
            LogNav("human_climb rope_jump dir=%d y=%.0f y0=%d aim=%d grab=%d to=%s", g.offDir, s.y,
                   (int)g.act.wy, (int)g.act.aimX, g.offGrab ? 1 : 0, NodeStr(g.act.toFh, tb, sizeof(tb)));
            EnterMode(Mode::HopOff, Sub::None, s.now, g.offGrab ? "rope_to_rope" : "rope_side_jump", s);
            return;
        }
        gMotor.Up(dy > 0.f ? 1 : -1);
        if (stallMs >= kClimbYStallMs * 2 && g.stallStage <= 1) {
            g.stallStage = 2;
            g.offDir = ClimbOffWalkDir(g.act.fromFh, g.act.wx, s.x);
            g.offJumpMs = 0;
            LogNav("human_climb stall_bail y=%.0f ma=%d wx=%d dir=%d (rope_jump)", s.y, s.ma, (int)g.act.wx,
                   g.offDir);
            EnterMode(Mode::HopOff, Sub::None, s.now, "stall_bail", s);
        }
        return;
    }
    gMotor.Up(vert);
    float destX = 0.f, destY = 0.f;
    const bool destOk = DestOnFh(g.act.toFh, g.act.wx, &destX, &destY);
    bool atDeck = false;
    if (destOk) {
        if (g.act.kind == EdgeKind::ClimbUp) {
            // 爬到顶再走上去。提前 30px 侧跳 13 次摔回底台 10 次（BIN 17:14~17:16 y=-155 → -365）。
            atDeck = s.y >= destY - kClimbOffDy || (yStall && s.y >= destY - 40.f);
        } else {
            // 下绳到底通常自动落台；卡住且已到底附近就跳下去。
            atDeck = yStall && s.y <= destY + 40.f;
        }
    }
    if (atDeck) {
        bool floorL = false, floorR = false;
        g.offDir = DeckOffDir(g.act.toFh, g.act.wx, destY, s.x, s.tx, s.tOk, &floorL, &floorR);
        LogNav("human_climb deck dir=%d floorL=%d floorR=%d tgt=%.0f rope=%d deckY=%.0f to=%u", g.offDir,
               floorL ? 1 : 0, floorR ? 1 : 0, s.tOk ? s.tx : 0.f, (int)g.act.wx, destY, (unsigned)g.act.toFh);
        g.offJumpMs = 0;
        // 向上到顶：按住 ↑+方向自然走上台，不跳；kHopOffWalkMs 还没上去才补跳。
        g.hopOffNoJumpUntil = s.now + kHopOffWalkMs;
        EnterMode(Mode::HopOff, Sub::None, s.now, yStall ? "deck_stall" : "deck", s);
        return;
    }
    // 下绳不用滑到底：已低于顶台面、离底还远、侧面正下方就是目标台 → 方向键+跳脱离（横版惯例）。
    if (destOk && g.act.kind == EdgeKind::ClimbDown) {
        float topX = 0.f, topY = 0.f;
        // 从绳节点出发（接力跳上来的）没有顶台可查：顶就是绳顶。
        bool topOk = false;
        if (ports::foothold_path::IsRopeNodeId(g.act.fromFh)) {
            topOk = g.act.ropeYTop > g.act.ropeYBot;
            topY = static_cast<float>(g.act.ropeYTop);
        } else {
            topOk = DestOnFh(g.act.fromFh, g.act.wx, &topX, &topY);
        }
        const float belowTop = topOk ? (topY - s.y) : 1e9f;
        const float remain = s.y - destY;
        if (belowTop >= kDismountBelowTopPx && remain >= kDismountMinDropPx) {
            int tmin = 0, tmax = 0;
            if (ports::foothold_path::FhXRange(g.act.toFh, &tmin, &tmax)) {
                const int pref = ClimbOffWalkDir(g.act.toFh, g.act.wx, s.x, s.tx, s.tOk);
                const int dirs[2] = {pref, -pref};
                for (int i = 0; i < 2; ++i) {
                    const int d = dirs[i];
                    const float px = static_cast<float>(g.act.wx) + static_cast<float>(d) * kDismountProbeDx;
                    if (px < static_cast<float>(tmin) + 4.f || px > static_cast<float>(tmax) - 4.f) continue;
                    const uint32_t below = FirstFhBelow(px, s.y, static_cast<int>(remain) + 24);
                    if (below != g.act.toFh) continue;
                    g.offDir = d;
                    g.offJumpMs = 0;
                    LogNav("human_climb dismount dir=%d y=%.0f top=%.0f dest=%.0f remain=%.0f to=%u", d,
                           s.y, topY, destY, remain, (unsigned)g.act.toFh);
                    EnterMode(Mode::HopOff, Sub::None, s.now, "dismount", s);
                    return;
                }
            }
        }
    }
    if (stallMs >= kClimbYStallMs * 2 && g.stallStage <= 1) {
        // uf=0 天空绳：目标是绳节点本身，顶上没有台。继续挂着按 ↑（隐藏门 overlap），
        // 禁止 stall_bail 跳进虚空。
        if (g.act.kind == EdgeKind::ClimbUp &&
            ports::foothold_path::IsRopeNodeId(g.act.toFh)) {
            gMotor.Up(1);
            return;
        }
        // 重按也没用：跳下绳，落地后按脚下台重新规划。
        g.stallStage = 2;
        g.offDir = ClimbOffWalkDir(g.act.toFh, g.act.wx, s.x);
        g.offJumpMs = 0;
        LogNav("human_climb stall_bail y=%.0f ma=%d wx=%d dir=%d", s.y, s.ma, (int)g.act.wx, g.offDir);
        EnterMode(Mode::HopOff, Sub::None, s.now, "stall_bail", s);
        return;
    }
    (void)out;
}

void DriveHopOff(const Sense& s) {
    g.prog = Progress::Moving;
    if (!g.offDir) g.offDir = ClimbOffWalkDir(g.act.toFh, g.act.wx, s.x);
    gMotor.Walk(s.now, g.offDir, /*urgent=*/true);
    if (g.hopOffNoJumpUntil && s.now < g.hopOffNoJumpUntil) {
        // 绳顶：↑ 继续按着 + 方向，横版惯例是直接走上台面；只有走不上去才跳。
        gMotor.Up(1);
        if (s.grounded && s.now - g.modeMs >= 60) {
            const bool onDeck = OnFhOrTwin(s, g.act.toFh);
            if (onDeck) NoteActionDone(s);
            g.planOk = false;
            EnterMode(Mode::Idle, Sub::None, s.now, onDeck ? "deck_walk_done" : "deck_walk_landed", s);
        }
        return;
    }
    // 绳到绳：还挂在原绳上只按方向（↑+跳在绳上会变成继续爬），离绳后按住 ↑ 飞过去抓。
    const bool stillOnOrigin =
        s.onRope && std::fabs(s.x - static_cast<float>(g.act.wx)) <= 8.f;
    if (g.offGrab) {
        gMotor.Up(stillOnOrigin ? 0 : 1);
        if (s.onRope && !stillOnOrigin &&
            std::fabs(s.x - static_cast<float>(g.act.aimX)) <= kRopeGrabXTol) {
            char tb[24];
            LogNav("human_climb rope_hop_grabbed x=%.0f y=%.0f aim=%d to=%s", s.x, s.y, (int)g.act.aimX,
                   NodeStr(g.act.toFh, tb, sizeof(tb)));
            if (ports::foothold_path::IsRopeNodeId(g.act.toFh)) {
                // 落点就是这根绳的节点：到站。下一拍 Tick 见人在绳上没计划 → ResumeOnRope 从
                // 绳节点规划下一步（爬顶出去 / 再接力跳）。
                NoteActionDone(s);
                g.planOk = false;
                gMotor.Up(0);
                EnterMode(Mode::Idle, Sub::None, s.now, "rope_relay_grabbed", s);
                return;
            }
            // 旧口径（toFh 是目标绳的顶台）：把动作改写成在这根绳上爬到顶。
            g.act.kind = EdgeKind::ClimbUp;
            g.act.wx = g.act.aimX;
            g.act.ropeId = g.act.ropeId2;
            g.act.ropeId2 = 0;
            g.offGrab = false;
            g.climbY = s.y;
            g.climbYMs = s.now;
            g.stallStage = 0;
            EnterMode(Mode::Climb, Sub::None, s.now, "rope_hop_grabbed", s);
            return;
        }
    } else {
        gMotor.Up(0);
    }
    // 先松 ↓、把方向键按实一拍再按跳：三键同拍游戏常把这一跳吃掉（BIN 13:58:33.55 第一下没脱离，
    // 挂着 700ms 才靠第二下下来）。只在还挂在绳上时才补第二下；已经腾空再按跳是空按（BIN 10:23:17）。
    const bool leadOk = s.now - g.modeMs >= Jitter(g.modeMs ^ 0x2c1bu, kHopOffLeadMs, kHopOffLeadMs + 40);
    if ((!g.offJumpMs && leadOk) || (g.offJumpMs && s.onRope && s.now - g.offJumpMs >= kHopOffGapMs)) {
        gMotor.Jump(s.now);
        g.offJumpMs = s.now;
        float destX = 0.f, destY = 0.f;
        (void)DestOnFh(g.act.toFh, g.act.wx, &destX, &destY);
        LogNav("human_jump why=off dir=%d cur=%u remain=%.0f vx=%.0f", g.offDir, (unsigned)s.cur,
               std::fabs(destX - s.x), s.vx);
    }
    static DWORD sOff = 0;
    if (!sOff || s.now - sOff > 400) {
        sOff = s.now;
        LogNav("human_climb off dir=%d py=%.0f to=%u", g.offDir, s.y, (unsigned)g.act.toFh);
    }
    if (s.grounded && s.now - g.modeMs >= 150) {
        const bool onTarget = OnFhOrTwin(s, g.act.toFh);
        if (onTarget) NoteActionDone(s);
        // 绳到绳没抓住掉到台上 = 这条边失手一次：按边记，3 次标死边（规划推翻后 attempts 归零，
        // 不记就会一轮轮重来）。绳侧台跳落到别的台同理——落在目标台同一条 Walk 链的邻段不算失手
        //（跳远了 30px 落到隔壁段，功能上到了）。
        bool landedOk = onTarget;
        if (!landedOk && !g.offGrab && s.cur && g.act.toFh && !ports::foothold_path::IsRopeNodeId(g.act.toFh)) {
            ports::foothold_path::FhGeomInfo a{}, b{};
            if (ports::foothold_path::FhGeom(s.cur, &a) && ports::foothold_path::FhGeom(g.act.toFh, &b) &&
                a.walkComp && a.walkComp == b.walkComp)
                landedOk = true;
        }
        if (g.act.kind == EdgeKind::RopeJump && !landedOk && g.act.fromFh && g.act.toFh && g.offJumpMs) {
            const int n = NoteEdgeGrabMiss(g.act.fromFh, g.act.toFh, s.now);
            char fb[24], tb[24];
            LogNav("human_climb rope_jump_miss grab=%d from=%s to=%s cur=%u x=%.0f y=%.0f aim=%d n=%d",
                   g.offGrab ? 1 : 0, NodeStr(g.act.fromFh, fb, sizeof(fb)), NodeStr(g.act.toFh, tb, sizeof(tb)),
                   (unsigned)s.cur, s.x, s.y, (int)g.act.aimX, n);
            if (n >= kGrabMaxMisses) {
                ports::foothold_path::MarkEdgeDead(g.act.fromFh, g.act.toFh, g.act.kind);
                ++gStats.deadEdges;
                LogNav("human_climb rope_jump_dead from=%s to=%s wy=%d aim=%d — missed %d× (edge dead, n=%d)",
                       NodeStr(g.act.fromFh, fb, sizeof(fb)), NodeStr(g.act.toFh, tb, sizeof(tb)), (int)g.act.wy,
                       (int)g.act.aimX, n, ports::foothold_path::DeadEdgeCount());
            }
        }
        g.planOk = false;
        EnterMode(Mode::Idle, Sub::None, s.now, onTarget ? "hop_off_done" : "hop_off_landed", s);
        return;
    }
    if (s.now - g.modeMs >= kHopOffTimeoutMs) {
        g.planOk = false;
        EnterMode(Mode::Idle, Sub::None, s.now, "hop_off_timeout", s);
    }
}

// ───────── 下跳 ─────────
void DriveDrop(const Sense& s) {
    g.prog = Progress::Moving;
    const float adx = std::fabs(static_cast<float>(g.act.wx) - s.x);
    if (!s.grounded) {
        // 腾空：松 ↓（免得穿过落点台），等落地重规划。
        gMotor.Up(0);
        if (g.sub != Sub::Air) EnterSub(Sub::Air, s.now, "airborne", s);
        return;
    }
    if (g.sub == Sub::Air) {
        // 落地
        const bool onTarget = OnFhOrTwin(s, g.act.toFh);
        if (onTarget) NoteActionDone(s);
        g.planOk = false;
        EnterMode(Mode::Idle, Sub::None, s.now, onTarget ? "fall_done" : "fall_landed", s);
        return;
    }
    (void)adx;
    if (g.walkOff) {
        // 走崖：朝离 wx 最近的那一端走出去（方向由 PlanDrop 定死，途中不改）。
        if (g.sub != Sub::WalkOff) {
            EnterSub(Sub::WalkOff, s.now, "cliff", s);
            LogNav("human_fall walkoff wx=%d end=%d dir=%d from=%u to=%u px=%.0f", (int)g.act.wx,
                   (int)g.dropX, g.dropDir, (unsigned)g.act.fromFh, (unsigned)g.act.toFh, s.x);
        }
        gMotor.Up(0);
        gMotor.ReleaseJump();
        int dir = g.dropDir;
        if (!dir) dir = Sign(static_cast<float>(g.dropX) - s.x);
        gMotor.Walk(s.now, dir, /*urgent=*/true);
        return;
    }
    const float adxDrop = std::fabs(static_cast<float>(g.dropX) - s.x);
    // 穿台点 20px 容差不够：落点台可能只从穿台点旁边几像素才开始（离线 sim 101030402：dropX=1271、
    // 台从 1260 起，人在 1252 就按 ↓+跳，穿过去落到 520px 下面的一层）。人的 X 必须真在落点台的
    // X 范围内（留 6px）才穿；近了改微步免得冲过头。
    bool overTarget = true;
    int tmin = 0, tmax = 0;
    if (g.dropOverTgt && g.act.toFh && ports::foothold_path::FhXRange(g.act.toFh, &tmin, &tmax) && tmax - tmin >= 12) {
        // 边距随落点台宽缩：16px 的窄台留 6px 只剩 4px 窗，微步 + 键延迟根本停不进去（离线 sim 101020000
        // fh863→fh89 5s×3 超时）；按台宽 1/4、2~6px 之间。
        float m = static_cast<float>(tmax - tmin) * 0.25f;
        if (m > 6.f) m = 6.f;
        if (m < 2.f) m = 2.f;
        // 已经在按 ↓ 穿台：窗口放宽到落点台整段（滞回）。带着 100px/s 进窗、停键晚 66ms 又滑出内缩窗 →
        // drop_drift / at_drop_x 每 100ms 一轮直到超时（离线 sim lat66 102020000 fh177→35 ×3）。
        if (g.sub == Sub::DropThrough) m = 0.f;
        overTarget = s.x >= static_cast<float>(tmin) + m && s.x <= static_cast<float>(tmax) - m;
    }
    // 人当前站的位置旁边有绳（构图/PlanDrop 都避开了 dropX，但 ±20px 容差里可能又靡到绳口）：这儿按 ↓ 会上绳，
    // 继续朝 dropX 微步。同理，脚下这一列第一块台若不是落点台（dropX 那一列是，人站的 ±20px 这一列却夹着
    // 另一块：离线 sim 101030404 fh79 dropX=1456 落 fh105，人在 1440 按 ↓ 落到 fh77 → 跳回去再穿 ×3）也不按。
    const uint32_t belowHere = (g.dropOverTgt && g.act.toFh) ? ports::foothold_path::FirstFhBelow(s.x, s.y) : g.act.toFh;
    const bool ropeHere = ports::foothold_path::RopeNearDropPoint(s.x, s.y) ||
                          (belowHere != 0 && g.act.toFh != 0 && !FhTwins(belowHere, g.act.toFh));
    // 进窗时还带着速度：先停稳再按 ↓（否则 ↓ 到达时人已滑出窗）。
    if (g.sub != Sub::DropThrough && adxDrop <= kWxTol && overTarget && !ropeHere && std::fabs(s.vx) > kSettleVx) {
        gMotor.Up(0);
        gMotor.ReleaseJump();
        gMotor.Stop();
        g.nudgeUntil = 0;
        return;
    }
    if (adxDrop > kWxTol || !overTarget || ropeHere) {
        gMotor.Up(0);
        gMotor.ReleaseJump();
        // 进过穿台窗又滑出去（停键晚到）：退回 None，回到窗里重新按「↓ 蹲稳 140ms 再跳」的节拍，
        // 否则 subMs 是旧值，一进窗就同拍齐按，引擎不认穿台。
        if (g.sub == Sub::DropThrough) EnterSub(Sub::None, s.now, "drop_drift", s);
        const int dir = Sign(static_cast<float>(g.dropX) - s.x);
        if (adxDrop > 24.f) {
            gMotor.Walk(s.now, dir);
            g.nudgeUntil = 0;
        } else if (!g.nudgeUntil) {
            // 微步时长按还差多少算（走速 125px/s ≈ 8ms/px），最短 20ms：绳口 + 落点台内缩把可穿窗夹到几像素宽时
            //（离线 sim lat66 102020000 fh177：绳 x=69、窗 (89,95]），45ms 定长一步 6~8px 永远在窗两边来回。
            DWORD pulse = static_cast<DWORD>(adxDrop * 8.f);
            if (pulse < 20) pulse = 20;
            if (pulse > kNudgeMs) pulse = kNudgeMs;
            // 微步预算：来回蹭了 10 步还没停进可穿窗（窗比一步还窄 / 绳口与邻台列夹死）→ 这条边作废换路，
            // 别干等 5s 超时。
            if (++g.dropNudges > kDropMaxNudges) {
                LogNav("human_fall no_band x=%.0f dropX=%d rope=%d below=%u to=%u — nudged %d×, skip edge", s.x,
                       (int)g.dropX, ropeHere ? 1 : 0, (unsigned)belowHere, (unsigned)g.act.toFh, g.dropNudges - 1);
                gMotor.Stop();
                Fail(Result::Unreachable, "drop_band");
                return;
            }
            gMotor.Walk(s.now, dir, /*urgent=*/true);
            g.nudgeUntil = s.now + pulse;
        } else if (s.now < g.nudgeUntil) {
            gMotor.Walk(s.now, dir, /*urgent=*/true);
        } else {
            gMotor.Stop();
            if (s.now - g.nudgeUntil >= kNudgeSettleMs) g.nudgeUntil = 0;
        }
        return;
    }
    g.nudgeUntil = 0;
    if (g.sub != Sub::DropThrough) EnterSub(Sub::DropThrough, s.now, "at_drop_x", s);
    gMotor.Stop();
    gMotor.Up(-1);
    if (!g.dropJumped) {
        // ↓ 先按住蹲稳再 Alt：同拍齐按引擎不认穿台（BIN 14:24:09 fh23 原地不动 → dropfail）。
        if (s.now - g.subMs < kDropProneMs || std::fabs(s.vx) > kSettleVx) return;
        gMotor.Jump(s.now);
        g.dropJumped = true;
        g.dropJumpMs = s.now;
        LogNav("human_fall jump x=%d wx=%d from=%u to=%u", (int)g.dropX, (int)g.act.wx,
               (unsigned)g.act.fromFh, (unsigned)g.act.toFh);
    } else if (s.cur == g.act.fromFh && s.now - g.dropJumpMs >= kFallRetryMs) {
        // 穿不下去：再试一次（首跳可能被 ↓ 蹲下动画吃掉）；仍不行只有真崖端才走出去，否则这条边作废。
        if (g.dropTries < 1) {
            ++g.dropTries;
            g.dropJumped = false;
            g.subMs = s.now;
            LogNav("human_fall retry x=%d from=%u to=%u", (int)g.dropX, (unsigned)g.act.fromFh,
                   (unsigned)g.act.toFh);
            return;
        }
        int xmin = 0, xmax = 0;
        bool cliff = false;
        if (ports::foothold_path::FhXRange(g.act.fromFh, &xmin, &xmax)) {
            const int end = (std::abs(g.act.wx - xmin) <= std::abs(g.act.wx - xmax)) ? xmin : xmax;
            const int dir = (end == xmin) ? -1 : 1;
            const float probeX = static_cast<float>(end) - static_cast<float>(dir) * 8.f;
            float fx = 0.f, fy = s.y;
            (void)ports::foothold_path::SnapOnFh(g.act.fromFh, probeX, &fx, &fy, false, false);
            if (ports::foothold_path::ProbeWalkAhead(probeX, fy, dir, g.act.fromFh, 48) ==
                WalkAhead::Pit) {
                cliff = true;
                g.dropDir = dir;
                g.dropX = end;
            }
        }
        g.dropJumped = false;
        if (cliff) {
            g.walkOff = true;
            LogNav("human_fall dropfail walkoff wx=%d end=%d from=%u to=%u", (int)g.act.wx,
                   (int)g.dropX, (unsigned)g.act.fromFh, (unsigned)g.act.toFh);
        } else {
            LogNav("human_fall dropfail no_cliff wx=%d from=%u to=%u — skip edge", (int)g.act.wx,
                   (unsigned)g.act.fromFh, (unsigned)g.act.toFh);
            ports::foothold_path::AddHopSkip(g.act.fromFh, g.act.toFh, g.act.kind);
            g.planOk = false;
            EnterMode(Mode::Idle, Sub::None, s.now, "drop_fail", s);
        }
    }
}

// ───────── 人已经挂在绳上 ─────────
// 上一动作失败 / 换目标时正爬着：脚下没台，按 hint 猜的台规划走路，走路键在绳上没用，
// 7.8s 超时换目标再来一遍，人永远挂着（BIN 14:28:32~14:29:11 x=381 y=-253 ma=16）。
// 反查这根绳，按「上/下哪头到目标更近」合成一段爬绳动作直接进 Climb；查不到绳就跳下来再规划。
// 挂在绳上、状态机没在管这根绳：从**绳节点**规划下一步（爬到顶出去 / 爬到底出去 / 侧跳到台 /
// 接力跳到下一根绳），这就是「有些绳梯必须从别的绳梯接力跳过去」的落地：跳上接力绳后本函数
// 接管，PlanFirst 从绳节点出发给出下一跳。规划不出来（没目标 / 图里没这根绳）才退回
// 「有哪头往哪头爬」的老口径；两头都没台又没路 → 跳下去落到哪算哪。
void ResumeOnRope(const Sense& s) {
    ports::foothold_path::RopeInfo rope{};
    const bool onKnown = ports::foothold_path::FindRopeAt(s.x, s.y, 12, &rope);
    if (onKnown && rope.nodeId && s.tfh) {
        FirstAction plan{};
        if (ports::foothold_path::PlanFirst(rope.nodeId, s.tfh, &plan, /*ignoreSkips=*/false, PlanX(s.x),
                                            PlanX(s.tx)) &&
            plan.ok && plan.kind != EdgeKind::Walk) {
            g.act = plan;
            g.chase = false;
            g.planOk = true;
            g.planMs = s.now;
            g.planTgtFh = s.tfh;
            g.actStartMs = s.now;
            g.actionChanged = true;
            g.attempts = 0;
            g.stallStage = 0;
            g.regripUntil = 0;
            g.offDir = 0;
            g.offJumpMs = 0;
            g.offGrab = false;
            g.hopOffNoJumpUntil = 0;
            g.climbY = s.y;
            g.climbYMs = s.now;
            g.ropeResume = true;
            char tb[24];
            LogNav("human_rope_resume plan kind=%s x=%d y=%.0f wy=%d aim=%d rope2=%d to=%s hops=%d uf=%d "
                   "tgt=(%.0f,%.0f)",
                   KindName(plan.kind), rope.x, s.y, (int)plan.wy, (int)plan.aimX, (int)plan.ropeId2,
                   NodeStr(plan.toFh, tb, sizeof(tb)), plan.hops, rope.upperFh ? 1 : 0, s.tx, s.ty);
            EnterMode(Mode::Climb, Sub::None, s.now, "resume_on_rope", s);
            return;
        }
    }
    // 只认得出一头的绳（绳到绳跳上来的高绳没底台）也接管：有哪头就往哪头爬。
    const bool found = onKnown && (rope.upFh || rope.dnFh);
    FirstAction act{};
    act.ok = true;
    if (!found) {
        act.kind = EdgeKind::ClimbDown;
        act.fromFh = s.pfh;
        act.toFh = 0;
        act.wx = static_cast<int32_t>(std::lround(s.x));
        act.wy = static_cast<int32_t>(std::lround(s.y));
        act.hops = 1;
        g.act = act;
        g.chase = false;
        g.planOk = true;
        g.planMs = s.now;
        g.planTgtFh = s.tfh;
        g.actStartMs = s.now;
        g.actionChanged = true;
        g.offDir = (s.dx >= 0.f) ? 1 : -1;
        g.offJumpMs = 0;
        LogNav("human_rope_resume no_rope x=%.0f y=%.0f ma=%d — bail dir=%d", s.x, s.y, s.ma,
               g.offDir);
        EnterMode(Mode::HopOff, Sub::None, s.now, "rope_unknown_bail", s);
        return;
    }
    FirstAction up{}, dn{};
    const bool upOk =
        rope.upFh && s.tfh && ports::foothold_path::PlanFirst(rope.upFh, s.tfh, &up) && up.ok;
    const bool dnOk =
        rope.dnFh && s.tfh && ports::foothold_path::PlanFirst(rope.dnFh, s.tfh, &dn) && dn.ok;
    bool goUp;
    if (!rope.dnFh) goUp = true;
    else if (!rope.upFh) goUp = false;
    else if (upOk != dnOk) goUp = upOk;
    else if (upOk) goUp = up.hops < dn.hops || (up.hops == dn.hops && s.ty >= s.y);
    else goUp = s.ty >= s.y;
    act.kind = goUp ? EdgeKind::ClimbUp : EdgeKind::ClimbDown;
    act.fromFh = goUp ? rope.dnFh : rope.upFh;
    act.toFh = goUp ? rope.upFh : rope.dnFh;
    act.wx = rope.x;
    act.wy = goUp ? rope.yBot : rope.yTop;
    act.hops = 1 + (goUp ? (upOk ? up.hops : 0) : (dnOk ? dn.hops : 0));
    g.act = act;
    g.chase = false;
    g.planOk = true;
    g.planMs = s.now;
    g.planTgtFh = s.tfh;
    g.actStartMs = s.now;
    g.actionChanged = true;
    g.attempts = 0;
    g.stallStage = 0;
    g.regripUntil = 0;
    g.offDir = 0;
    g.offJumpMs = 0;
    g.climbY = s.y;
    g.climbYMs = s.now;
    g.ropeResume = true;
    LogNav("human_rope_resume x=%d up=%u dn=%u go=%s hops=%d y=%.0f ma=%d tgt=(%.0f,%.0f)", rope.x,
           (unsigned)rope.upFh, (unsigned)rope.dnFh, goUp ? "up" : "down", act.hops, s.y, s.ma, s.tx,
           s.ty);
    EnterMode(Mode::Climb, Sub::None, s.now, "resume_on_rope", s);
}

// ───────── 挂绳休息 ─────────
void RestBegin(DWORD now, float px, float py) {
    gRest = {};
    gRest.active = true;
    gRest.startMs = now;
    ports::foothold_path::RopeInfo rope{};
    if (!ports::foothold_path::FindNearestRope(px, py, &rope) || !rope.upFh || !rope.dnFh) {
        LogNav("human_rest no_rope x=%.0f y=%.0f", px, py);
        return;
    }
    float dnY = static_cast<float>(rope.yBot);
    (void)ports::foothold_path::FhYAt(rope.dnFh, static_cast<float>(rope.x), &dnY);
    float hangY = (static_cast<float>(rope.yTop) + static_cast<float>(rope.yBot)) * 0.5f;
    if (hangY < dnY + kRestAboveFloorPx) hangY = dnY + kRestAboveFloorPx;
    if (hangY > static_cast<float>(rope.yTop) - kRestBelowTopPx)
        hangY = static_cast<float>(rope.yTop) - kRestBelowTopPx;
    gRest.rope = rope;
    gRest.dnY = dnY;
    gRest.hangY = hangY;
    gRest.ropeOk = true;
    LogNav("human_rest begin rope x=%d top=%d bot=%d up=%u dn=%u hangY=%.0f from=(%.0f,%.0f)", rope.x,
           rope.yTop, rope.yBot, (unsigned)rope.upFh, (unsigned)rope.dnFh, hangY, px, py);
}

// ───────── 恢复 ─────────
void DriveRecover(const Sense& s) {
    g.prog = Progress::Recovering;
    gMotor.Stop();
    if (g.recoverReturn != Mode::Grab && g.recoverReturn != Mode::Climb) gMotor.Up(0);
    const bool calm = s.grounded && std::fabs(s.vx) < kSettleVx;
    if (calm || s.now - g.modeMs >= kRecoverMaxMs) {
        // 落稳后还有一段硬直：期间 vx=0 不算顶住、微步不计数。
        g.stunUntil = s.now + kKnockStunMs;
        g.planOk = false;
        EnterMode(Mode::Idle, Sub::None, s.now, calm ? "calm" : "recover_timeout", s);
    }
}

Phase PhaseOf(const Sense& s) {
    (void)s;
    switch (g.mode) {
        case Mode::WalkTo:
        case Mode::Jump:
        case Mode::Recover:
        case Mode::Idle:
            if (g.planOk && g.act.kind == EdgeKind::FallDown) return Phase::Fall;
            if (g.planOk && (g.act.kind == EdgeKind::ClimbUp || g.act.kind == EdgeKind::ClimbDown ||
                             g.act.kind == EdgeKind::RopeJump))
                return Phase::Climb;
            return (g.planOk && !g.chase) ? Phase::ApproachWx : Phase::WalkToMob;
        case Mode::Grab:
            return g.sub == Sub::Approach ? Phase::ApproachWx : Phase::Climb;
        case Mode::Climb:
        case Mode::HopOff:
            return Phase::Climb;
        case Mode::Drop:
            return Phase::Fall;
        default:
            return Phase::WalkToMob;
    }
}

int gSenseHp = -1, gSenseMhp = -1;  // 1Hz 快照里带 HP：死亡复盘要有血量时间线（upload 2026-09-09 10:34 只能反推）

void LogSense(const Sense& s) {
    if (g.senseLogMs && s.now - g.senseLogMs < kSenseLogMs) return;
    g.senseLogMs = s.now;
    LogNav("human_sense st=%s%s%s x=%.0f y=%.0f vx=%.0f vy=%.0f ma=%d cur=%u hp=%d/%d tgt=(%.0f,%.0f) tfh=%u "
           "rel=%d tvx=%.0f plan=%s hops=%d wx=%d prog=%s dir=%d",
           ModeName(g.mode), g.sub != Sub::None ? "." : "", SubName(g.sub), s.x, s.y, s.vx, s.vy, s.ma,
           (unsigned)s.cur, gSenseHp, gSenseMhp, s.tx, s.ty, (unsigned)s.tfh, s.rel, s.tvx,
           g.planOk ? KindName(g.act.kind) : "-", g.planOk ? g.act.hops : 0,
           g.planOk ? (int)g.act.wx : 0, ProgName(g.prog), gMotor.dir);
}

// 脱绳（无目标）：方向键+跳，落到哪算哪。
struct OffRopeCtx {
    bool active = false;
    int dir = 0;
    DWORD jumpMs = 0;
    DWORD startMs = 0;
};
OffRopeCtx gOffRope;

std::recursive_mutex gNavMu;
using NavLock = std::lock_guard<std::recursive_mutex>;

bool OnRopeLive(ports::teleport::FlightState* outSt = nullptr) {
    ports::teleport::FlightState st{};
    const bool ok = ports::teleport::QueryFlightState(st) && st.ok;
    if (outSt) *outSt = st;
    return ok && !st.onFh && IsRopeMa(st.ma);
}

// ───────── 死亡 ─────────
// 真源 = CharacterStat.hp（autopot / timed_keys 同一读法，纯内存）。250ms 缓存，用 GetTickCount
// 计时（调用方的 now 有 NowMs / GetTickCount 两种时钟，不能拿来比）。
// 不拿 MoveAction=18 单独判死：只见过一个样本，判错会把活人冻住；只做日志佐证。
constexpr DWORD kDeadPollMs = 250;
DWORD gDeadPollTick = 0;
bool gDeadCached = false;
int gDeadHp = 0, gDeadMhp = 0;

bool PlayerDead() {
    const DWORD tick = GetTickCount();
    if (gDeadPollTick && tick - gDeadPollTick < kDeadPollMs) return gDeadCached;
    gDeadPollTick = tick;
    x::ui::player::Vitals vit{};
    const bool ok = x::ui::player::Read(vit);
    gDeadCached = ok && x::ui::player::IsDead(vit);
    gDeadHp = ok ? vit.hp : -1;
    gDeadMhp = ok ? vit.mhp : -1;
    gSenseHp = gDeadHp;
    gSenseMhp = gDeadMhp;
    return gDeadCached;
}

void ResetUnlocked() {
    g = {};
    gMotor = {};
    gTgt = {};
    if (gRest.active) LogNav("human_rest cleared by reset");
    gRest = {};
    gOffRope = {};
}

// 死了：一次性全部松键，进 Dead 态；之后每拍只回 Dead，不规划不按键。
void DriveDead(DWORD now, float px, float py, TickOut& out) {
    if (g.mode != Mode::Dead) {
        ports::teleport::FlightState st{};
        const bool stOk = ports::teleport::QueryFlightState(st) && st.ok;
        LogNav("human_dead enter hp=%d/%d ma=%d x=%.0f y=%.0f from=%s", gDeadHp, gDeadMhp,
               stOk ? st.ma : -1, px, py, ModeName(g.mode));
        Sense sn{};
        sn.now = now;
        sn.x = px;
        sn.y = py;
        EnterMode(Mode::Dead, Sub::None, now, "hp0", sn);
        g.planOk = false;
        gMotor.Stop();
        gMotor.Up(0);
        gMotor.ReleaseJump();
        (void)ports::attack::StopNav();
        if (gRest.active) LogNav("human_rest cleared by death");
        gRest = {};
        gOffRope = {};
    }
    out.result = Result::Dead;
    out.phase = Phase::Dead;
    out.progress = Progress::Idle;
    out.why = "dead";
    out.state = ModeName(g.mode);
}

}  // namespace

void Reset() {
    NavLock lk(gNavMu);
    // 休息/脱绳上下文一并清：赶路 WalkStick 起步会 Reset，若留着 gRest，
    // 途中爬绳会被 DriveClimb 当「休息」停在半空不上顶。调用方下一拍 TickRest 会重新找绳。
    ResetUnlocked();
}

bool IsDead() {
    NavLock lk(gNavMu);
    return PlayerDead();
}

bool OnRopeNow() { return OnRopeLive(); }

bool TickGetOffRope(DWORD now) {
    NavLock lk(gNavMu);
    if (PlayerDead()) {
        if (gOffRope.active) {
            (void)ports::attack::StopNav();
            gOffRope = {};
        }
        return false;
    }
    ports::teleport::FlightState st{};
    if (!OnRopeLive(&st)) {
        if (gOffRope.active) {
            (void)ports::attack::StopNav();
            LogNav("human_offrope done x=%.0f y=%.0f onFh=%d after=%ums", st.x, st.y, st.onFh ? 1 : 0,
                   (unsigned)(now - gOffRope.startMs));
            gOffRope = {};
        }
        return false;
    }
    if (!gOffRope.active) {
        gOffRope = {};
        gOffRope.active = true;
        gOffRope.startMs = now;
        // 往正下方有台的那一侧跳；两侧都有就朝绳底台宽的一侧。
        ports::foothold_path::RopeInfo rope{};
        const bool found = ports::foothold_path::FindRopeAt(st.x, st.y, 12, &rope);
        int dir = found && rope.dnFh ? ClimbOffWalkDir(rope.dnFh, rope.x, st.x) : 1;
        const uint32_t belowA = FirstFhBelow(st.x + dir * kDismountProbeDx, st.y, 900);
        const uint32_t belowB = FirstFhBelow(st.x - dir * kDismountProbeDx, st.y, 900);
        if (!belowA && belowB) dir = -dir;
        gOffRope.dir = dir;
        LogNav("human_offrope begin x=%.0f y=%.0f ma=%d rope=%d dir=%d below=%u/%u", st.x, st.y, st.ma,
               found ? rope.x : 0, dir, (unsigned)belowA, (unsigned)belowB);
    }
    (void)ports::attack::ReleaseVertical();
    (void)ports::attack::HoldWalk(gOffRope.dir);
    if (!gOffRope.jumpMs || now - gOffRope.jumpMs >= kHopOffGapMs) {
        gOffRope.jumpMs = now;
        (void)ports::attack::PulseJump(kJumpPulseMs);
    }
    return true;
}

RestState TickRest(DWORD now, float px, float py) {
    NavLock lk(gNavMu);
    if (PlayerDead()) {
        // 死了没得休息：松键、清上下文，让调用方走 Dead 态。
        TickOut dummy{};
        DriveDead(now, px, py, dummy);
        return RestState::Failed;
    }
    if (!gRest.active) RestBegin(now, px, py);
    if (!gRest.ropeOk) return RestState::Failed;
    const auto& rope = gRest.rope;
    const float ropeX = static_cast<float>(rope.x);
    const uint32_t pfh = ports::foothold::PeekCurFhId();

    Sense s = Perceive(now, px, py, ropeX, gRest.dnY, pfh, /*travelPortal=*/true, rope.dnFh);
    if (s.onRope && std::fabs(s.x - ropeX) <= 16.f) {
        gMotor.Stop();
        gMotor.ReleaseJump();
        const bool there = s.y >= gRest.hangY - 6.f;
        gMotor.Up(there ? 0 : 1);
        if (!gRest.logMs || now - gRest.logMs > 2000) {
            gRest.logMs = now;
            LogNav("human_rest %s y=%.0f hangY=%.0f ma=%d", there ? "hanging" : "climbing", s.y,
                   gRest.hangY, s.ma);
        }
        return there ? RestState::Hanging : RestState::Going;
    }

    // 站到绳底那层、离绳 ≤48px，而状态机还没在管这根绳：直接合成一段上绳动作
    //（别让规划挑别的路）。from 用脚下真实 FH，免得 NeedReplan 以「换台」为由推翻。
    const bool handling = g.planOk && (g.mode == Mode::Grab || g.mode == Mode::Climb) &&
                          g.act.wx == rope.x;
    if (s.grounded && !handling && std::fabs(s.x - ropeX) <= 48.f) {
        float fy = 0.f;
        const bool onBottomLayer =
            s.cur == rope.dnFh ||
            (ports::foothold_path::FhYAt(s.cur, ropeX, &fy) && std::fabs(fy - gRest.dnY) <= 24.f);
        if (onBottomLayer) {
            FirstAction act{};
            act.ok = true;
            act.kind = EdgeKind::ClimbUp;
            act.fromFh = s.cur;
            act.toFh = rope.upFh;
            act.wx = rope.x;
            act.wy = rope.yBot;
            act.hops = 1;
            g.planOk = true;
            g.planMs = now;
            g.planTgtFh = s.tfh;
            StartAction(act, s, /*allowSame=*/false);
            LogNav("human_rest mount rope x=%d from=%u", rope.x, (unsigned)s.cur);
        }
    }

    // AbsPos：更大 Y = 更高。已经掉到绳底下一层就别再 Tick 对着绳 X 走
    //（BUILD198：fall_landed y=-1785，dnY=-1725，hops=0 左右抖）。
    if (s.grounded && !s.onRope && s.y + kRestWrongLayerPx < gRest.dnY) {
        LogNav("human_rest fail why=below_rope y=%.0f dnY=%.0f cur=%u tfh=%u", s.y, gRest.dnY,
               (unsigned)s.cur, (unsigned)s.tfh);
        gRest.ropeOk = false;
        return RestState::Failed;
    }

    // 其它：按普通导航走到绳底台的绳 X（travelPortal 口径：不等怪、不侧步、不顿）。
    // 目标台强制 dnFh，别让 SnapStandAt 吸到下一层。
    const TickOut o = Tick(now, px, py, ropeX, gRest.dnY, pfh, /*travelPortal=*/true, rope.dnFh);
    if (o.result == Result::Unreachable || o.result == Result::Timeout) {
        LogNav("human_rest fail why=%s sm=%s", o.why ? o.why : "?", o.state ? o.state : "?");
        gRest.ropeOk = false;
        return RestState::Failed;
    }
    if (s.grounded && !s.onRope && o.hops == 0 && std::fabs(s.y - gRest.dnY) > kRestWrongLayerPx) {
        LogNav("human_rest fail why=wrong_layer y=%.0f dnY=%.0f hops=0 cur=%u tfh=%u", s.y, gRest.dnY,
               (unsigned)s.cur, (unsigned)s.tfh);
        gRest.ropeOk = false;
        return RestState::Failed;
    }
    const bool climbing = g.mode == Mode::Grab || g.mode == Mode::Climb || g.mode == Mode::Drop;
    if (!s.onRope && !climbing && o.hops == 0 && now - gRest.startMs >= kRestStuckMs) {
        LogNav("human_rest fail why=stuck age=%ums sm=%s x=%.0f y=%.0f",
               (unsigned)(now - gRest.startMs), o.state ? o.state : "?", s.x, s.y);
        gRest.ropeOk = false;
        return RestState::Failed;
    }
    if (!gRest.logMs || now - gRest.logMs > 2000) {
        gRest.logMs = now;
        LogNav("human_rest going sm=%s x=%.0f y=%.0f ropeX=%d dnFh=%u cur=%u tfh=%u hops=%d",
               o.state ? o.state : "?", s.x, s.y, rope.x, (unsigned)rope.dnFh, (unsigned)s.cur,
               (unsigned)s.tfh, o.hops);
    }
    return RestState::Going;
}

void EndRest() {
    NavLock lk(gNavMu);
    if (gRest.active) LogNav("human_rest end after=%ums", (unsigned)(GetTickCount() - gRest.startMs));
    gRest = {};
    Reset();
    (void)ports::attack::ReleaseVertical();
}

void ReleaseKeys() { (void)ports::attack::StopNav(); }

const char* StateName() {
    NavLock lk(gNavMu);
    return ModeName(g.mode);
}

bool AirborneNav() {
    NavLock lk(gNavMu);
    if (g.mode == Mode::Dead) return false;  // 尸体 onFh=0 cur=0，别当悬空
    ports::teleport::FlightState st{};
    const bool stOk = ports::teleport::QueryFlightState(st) && st.ok;
    const bool onFh = stOk && st.onFh;
    const uint32_t cur = ports::foothold::PeekCurFhId();
    if (!g.planOk && g.mode == Mode::Idle) {
        // 没动作也要认「挂在绳上」：赶路 / 战斗刚开始时人若在绳上，调用方得让 Tick 跑接管，
        // 而不是当成普通悬空松键干等（BIN 14:28 挂绳 40s）。
        return stOk && !onFh && IsRopeMa(st.ma);
    }
    if (g.mode == Mode::Drop) return true;
    return !onFh || !cur;
}

bool ClimbGrabBusy() {
    NavLock lk(gNavMu);
    if (!g.planOk || g.mode == Mode::Dead) return false;
    switch (g.mode) {
        case Mode::Climb:
        case Mode::HopOff:
            return true;
        case Mode::Grab:
            if (g.sub != Sub::Approach) return true;
            break;
        default:
            break;
    }
    return AirborneNav();
}

bool VerticalBusy() {
    NavLock lk(gNavMu);
    if (!g.planOk || g.mode == Mode::Dead) return false;
    if (g.mode == Mode::Drop) return true;
    return ClimbGrabBusy();
}

bool StandingNearClimb(float px) {
    NavLock lk(gNavMu);
    if (!g.planOk || g.mode != Mode::Grab || g.sub != Sub::Approach) return false;
    if (AirborneNav()) return false;
    return std::fabs(px - static_cast<float>(g.act.wx)) <= 110.f;
}

TickOut Tick(DWORD now, float px, float py, float lockX, float lockY, uint32_t playerFhHint,
             bool travelPortal, uint32_t targetFhHint) {
    NavLock lk(gNavMu);
    TickOut out{};
    g.actionChanged = false;
    g.fail = Result::Continue;
    g.failWhy = "";
    gMotor.keyFail = false;

    // 死亡最先判：死了不规划、不按键，也不能回 Unreachable 让调用方 ban 目标
    //（upload 2026-09-09 10:34：hp=0 后 30s 里 Acquire→MoveTo→超时 转圈、对尸体按跳）。
    if (PlayerDead()) {
        DriveDead(now, px, py, out);
        return out;
    }
    if (g.mode == Mode::Dead) {
        LogNav("human_dead revived hp=%d/%d x=%.0f y=%.0f", gDeadHp, gDeadMhp, px, py);
        ResetUnlocked();
    }

    if (!ports::foothold_path::EnsureGraph()) {
        // 图没起来：退化成朝目标直走。
        out.phase = Phase::WalkToMob;
        out.state = "NoGraph";
        gMotor.Up(0);
        const float dx = lockX - px;
        if (std::isfinite(dx) && std::fabs(dx) >= 1.f) {
            gMotor.Walk(now, Sign(dx));
            out.progress = Progress::Moving;
        } else {
            gMotor.Stop();
        }
        if (gMotor.keyFail) {
            out.result = Result::KeyFail;
            out.why = "key";
        }
        return out;
    }

    Sense s = Perceive(now, px, py, lockX, lockY, playerFhHint, travelPortal, targetFhHint);
    if (!s.tOk && !travelPortal) {
        out.result = Result::Unreachable;
        out.why = "no_mob_fh";
        out.state = ModeName(g.mode);
        return out;
    }
    if (!s.pfh) {
        out.result = Result::Unreachable;
        out.why = "no_player_fh";
        out.state = ModeName(g.mode);
        return out;
    }

    // 挂在绳上而状态机没在管这根绳：接管（合成爬绳动作），别按脚下猜的台去走路。
    if (s.onRope) {
        const bool handled = g.planOk && (g.mode == Mode::Climb || g.mode == Mode::HopOff ||
                                          g.mode == Mode::Grab);
        if (!handled) ResumeOnRope(s);
    }

    // 没规划却在空中（刚换目标时正在坠落 / 跳着）：先等落地，别拿同 Y 带猜到的远台去规划
    //（SnapStandAt 能贴到 500px 外的台，BIN 14:28 pfh=68 时人在 x=381）。绳上另有接管。
    if (!g.planOk && !s.grounded && !s.onRope) {
        gMotor.Stop();
        out.phase = Phase::WalkToMob;
        out.progress = Progress::Recovering;
        out.state = ModeName(g.mode);
        return out;
    }

    // 被击退：走路不会 ≥220。刚换目标还没规划时也要认（BIN 18:32:51 顶飞中起跳往反方向飞）。
    if (s.grounded && std::fabs(s.vx) >= kKnockVx &&
        (g.mode == Mode::WalkTo || g.mode == Mode::Idle || g.mode == Mode::Recover)) {
        if (g.mode != Mode::Recover) {
            g.recoverReturn = g.mode;
            g.planOk = false;
            ++gStats.knockbacks;
            EnterMode(Mode::Recover, Sub::None, now, "knock", s);
        }
        DriveRecover(s);
        LogSense(s);
        out.phase = PhaseOf(s);
        out.progress = Progress::Recovering;
        out.state = ModeName(g.mode);
        return out;
    }

    if (NeedReplan(s)) {
        if (!Replan(s, out)) {
            out.state = ModeName(g.mode);
            gMotor.Stop();
            return out;
        }
    }

    NoteAirSample(s);  // 任何态腾空都记：起跳有没有真离地 / 带出横速（失手计数去混淆用）

    // 动作超时
    if (g.planOk && g.actStartMs) {
        const DWORD limit = ActionTimeout(s);
        const DWORD age = now - g.actStartMs;
        if (limit && age >= limit) {
            ++gStats.actionTimeout;
            LogNav("human_nav timeout kind=%s st=%s age=%ums wx=%d from=%u to=%u cur=%u",
                   KindName(g.act.kind), ModeName(g.mode), (unsigned)age, (int)g.act.wx,
                   (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (unsigned)s.cur);
            out.result = Result::Timeout;
            out.why = "timeout";
            out.kind = g.act.kind;
            out.hops = g.act.hops;
            out.wx = g.act.wx;
            out.fromFh = g.act.fromFh;
            out.toFh = g.act.toFh;
            out.state = ModeName(g.mode);
            return out;
        }
    }

    switch (g.mode) {
        case Mode::WalkTo:
            DriveWalk(s, out);
            break;
        case Mode::Jump:
            DriveJump(s);
            break;
        case Mode::Grab:
            DriveGrab(s, out);
            break;
        case Mode::Climb:
            DriveClimb(s, out);
            break;
        case Mode::HopOff:
            DriveHopOff(s);
            break;
        case Mode::Drop:
            DriveDrop(s);
            break;
        case Mode::Recover:
            DriveRecover(s);
            break;
        case Mode::Idle:
        default:
            gMotor.Stop();
            gMotor.Up(0);
            g.prog = Progress::Idle;
            break;
    }

    out.kind = g.act.kind;
    out.hops = g.planOk ? g.act.hops : 0;
    out.wx = g.act.wx;
    out.fromFh = g.act.fromFh;
    out.toFh = g.act.toFh;
    out.actionBudgetMs = (g.planOk && g.act.hops > 0) ? ActionTimeout(s) : 0u;
    out.actionChanged = g.actionChanged;
    out.phase = PhaseOf(s);
    out.progress = g.prog;
    out.state = ModeName(g.mode);
    if (g.fail != Result::Continue) {
        out.result = g.fail;
        out.why = g.failWhy;
        LogNav("human_nav fail why=%s kind=%s st=%s hops=%d wx=%d from=%u to=%u cur=%u", g.failWhy,
               KindName(g.act.kind), ModeName(g.mode), g.act.hops, (int)g.act.wx,
               (unsigned)g.act.fromFh, (unsigned)g.act.toFh, (unsigned)s.cur);
    } else if (gMotor.keyFail) {
        out.result = Result::KeyFail;
        out.why = "key";
    }
    LogSense(s);
    return out;
}

Stats GetStats() {
    NavLock lock(gNavMu);
    Stats st = gStats;
    st.deadEdges = static_cast<uint32_t>(ports::foothold_path::DeadEdgeCount());
    return st;
}

}  // namespace x::features::simple_combat::human_nav
