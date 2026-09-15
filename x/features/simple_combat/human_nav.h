#pragma once
// 拟人位移（经典版 / TWMS）：感知 → 规划 → 分层状态机 → 带惯性的按键执行。
//
// 三层：
//   Sense  每拍一份快照：身体（Ap/速度/挂台/MoveAction）、脚下台几何、目标站点与目标横速。
//   Plan   foothold 图第一跳（PlanFirst），**按事件重规划**（换台 / 目标换台 / 到站 / 失败 /
//          1.5s 复核），空中 / 抓绳 / 爬绳 / 下跳中一律不重规划（BIN 03:28 walk↔fall 抖）。
//   Drive  Idle / WalkTo / Jump / Grab / Climb / HopOff / Drop / Recover，
//          每态自带子步、超时、失败升级；按键经 Motor：方向最短保持 220ms（惯性）、
//          助跑跳 vs 原地跳分开、抓绳先对齐 ≤6px 再竖直跳。
//
// 键只走 unity_kbd（↑↓ / LeftAlt），禁止 SendInput、禁止直调 VecCtrl.Jump。
// AbsPos：更大 Y = 更高。
// 头顶不相连的台由构图 JumpUp 边覆盖（X 重叠 ≥8、高 6~72）；再高的没绳就当不可达。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

#include "../ports/foothold_path.h"

namespace x::features::simple_combat::human_nav {

// 对外相位（调用方据此决定进带 / 出刀 / 下跳禁带）。
enum class Phase : uint8_t {
    WalkToMob = 0,  // hops=0：朝锁怪走（进带由调用方判）
    ApproachWx,     // 走到绳子 / 下跳点 / 楼梯路点 X
    Climb,          // 贴绳 / 绳上 / 下绳
    Fall,           // 下跳 / 走崖
    Dead,           // 角色死亡（hp≤0 躺尸）：全部松键，等复活；调用方停打 / 停赶路
};

enum class Result : uint8_t {
    Continue = 0,
    Unreachable,  // 无路 / 卡死升级到顶（调用方 ban 目标 + 跳过该边）
    Timeout,      // 某动作超时 / 抓绳次数用尽
    KeyFail,
    Dead,         // 角色死亡：不是路的问题，调用方**不得** ban 目标 / 跳过边
};

// 走路进度判定：调用方据此决定「要不要因为没进展而放弃」。
enum class Progress : uint8_t {
    Idle = 0,    // 未在走（反应延迟 / 已到 / 无动作）
    Moving,      // 正常推进
    Chasing,     // 目标在远离，自己在追
    Waiting,     // 目标正朝自己走来，停步等它进带
    Blocked,     // 顶住不动，正在升级（跳 / 退一步 / 重规划）
    Recovering,  // 被击退 / 意外腾空，等落稳
};

struct TickOut {
    Result result = Result::Continue;
    Phase phase = Phase::WalkToMob;
    Progress progress = Progress::Idle;
    ports::foothold_path::EdgeKind kind = ports::foothold_path::EdgeKind::Walk;
    int hops = 0;
    int32_t wx = 0;
    uint32_t fromFh = 0;
    uint32_t toFh = 0;
    DWORD actionBudgetMs = 0;    // 当前动作自己的时限（长绳按绳长算）；调用方的单跳秒表不得比它短
    bool actionChanged = false;  // 本拍换了动作 / 到站
    const char* why = "";
    const char* state = "";  // 状态机当前态名（日志用）
};

// 所有入口内部持同一把递归锁：战斗线程与赶路 / 补给线程会交替驱动同一套状态
//（WalkStick 在 travel/auto_supply 线程跑 Tick，combat 线程同时 Reset / 查 AirborneNav）。
void Reset();
void ReleaseKeys();  // StopNav
// 角色是否已死（CharacterStat hp≤0，250ms 缓存；纯内存读）。Tick 一读到就进 Dead 态松键，
// 其余入口（休息 / 脱绳 / 忙碌判定）在死亡期间一律返回「不忙 / 失败」。复活后自动回 Idle。
bool IsDead();
bool OnRopeNow();    // 实时：悬空且 MoveAction 是绳梯（不依赖状态机）
// 挂在绳上而没人管（补给前落地 / 换图前）：方向键+跳脱离，落地后自动松键。
// 返回 true = 正在处理（调用方本拍等）；false = 不在绳上 / 已落地。
bool TickGetOffRope(DWORD now);
bool AirborneNav();  // 绳上 / 下跳 / 走路腾空：禁止 passby / 进带出刀
bool VerticalBusy();  // 腾空爬/下跳 / 抓绳流程：禁止 passby 把 ↑ 掐掉
bool ClimbGrabBusy();  // 贴绳停步 / 对齐 / 起跳 / 腾空 / 绳上：禁止 climb_clear
bool StandingNearClimb(float px);  // 走向梯子途中：先清同层会击退的怪
const char* StateName();

TickOut Tick(DWORD now, float px, float py, float lockX, float lockY, uint32_t playerFhHint,
             bool travelPortal = false, uint32_t targetFhHint = 0);

// 累计计数（进程内单调累加；调用方取差值得到一段时间的速率）。KPI 汇总用。
struct Stats {
    uint32_t actionsStarted = 0;  // 开始的非追怪动作（走一段 / 爬 / 下跳 / 极限跳）
    uint32_t hopsDone = 0;        // 干净完成的动作
    uint32_t blocked = 0;         // 顶住升级次数（human_blocked level=0）
    uint32_t relatch = 0;         // 平地补边沿
    uint32_t reverse = 0;         // 反向退一步
    uint32_t noPath = 0;          // 规划无路
    uint32_t actionTimeout = 0;   // 单动作超时
    uint32_t grabFail = 0;        // 抓绳 3 次落回
    uint32_t acrossFellBack = 0;  // 极限跳摔回原台（几何失败计数）
    uint32_t acrossFail = 0;      // 极限跳 3 次摔回 → 死边
    uint32_t loopBreak = 0;       // 两台之间来回摆被打断
    uint32_t inputFails = 0;      // 跳了却没离地 / 没横速：键没进去（不计几何失败）
    uint32_t deadEdges = 0;       // 本进程学到的死边数
    uint32_t knockbacks = 0;      // 被顶飞进 Recover
};
Stats GetStats();

// 低血无药挂绳休息：走到最近的绳，爬到中段（离底台 ≥70px）松键挂着。
// Going=在路上/在爬；Hanging=已挂好；Failed=没绳可用 / 走不到（调用方稍后再试）。
enum class RestState : uint8_t { Going = 0, Hanging, Failed };
RestState TickRest(DWORD now, float px, float py);
void EndRest();  // 结束休息：清状态（调用方自行 StopNav）

}  // namespace x::features::simple_combat::human_nav
