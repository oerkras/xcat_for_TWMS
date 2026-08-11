#pragma once
// teleport_port — Classic TWMS 位移端口
//
// 产品路径（统一）：Impact（F5 贴怪 / F6 飞）—— NockBack / SetImpactNext / ImpactHop /
// ImpactImpulseToward / ImpactSetVelocity。Attr=2。
//
// fill+Doing（TeleportNativeSkillCall / TryDoingTeleport）已失效：入口硬拒，禁止再接入。
// 禁止再引入：Attr=4 旁路 / SyncRelPosOnly settle / RestoreWalkable / Register 技能包 /
// 钉 Transform / VisualLeash。

#include <cstdint>

namespace x::features::ports::teleport {

bool EnsureBound();

// 进图物理就绪（Impact / 旧 native 共用探测）。
bool IsPhysicsReadyForNative();

// fill+Doing 已禁用：一律 false（保留符号防旧调用链链接）。
bool TeleportNativeSkillCall();
bool TeleportNativeSkillCall(float landX, float landY, uint32_t plantFhId, bool snapStand = true);
void SetNativeCooldownMs(uint32_t ms);
// 从「现在」起强制自冷 ms（写入 lastOk=now）。补给开趟冷却窗用。
void ForceNativeCooldownMs(uint32_t ms);
void ClearNativeSelfCd();

// 距下次可 native 的剩余 ms；fill+Doing 已废，调用方勿再依赖此门开瞬移。
uint32_t NativeCooldownRemainingMs();

// 最近一次 native ok 后的收态窗（历史互斥；fill+Doing 停用后通常恒 false）。
bool IsPostTeleportQuiet(uint32_t quietMs = 220);

// 必须已在主线程泵上。拆 CurFh/LastFh + 清 InputX/Ap.V/Rp.V（land/fh 仅日志上下文）。
// 禁止补种台面 / 写 RelPos 弧长（旧 replant=1 = AbsPos 重算 = 体感瞬移，已删除该路径）。
bool StabilizeFootholdMainThread(float landX, float landY, uint32_t fhId);

// 必须已在主线程。只清 InputX + 零速度，不拆 CurFh。
bool ClearMotionLatchMainThread();

// 调试 TAB 冲击位移探针（无热键）。A=NockBack；B=SetImpactNext。
bool FireImpactNockBackTest(int dir, int vx, int vy);
bool FireImpactSetNextTest(double vx, double vy);

// P0 近距冲击 hop：|Δx|→vx 线性表 + NockBack；引擎自然出 Attr=2。
struct ImpactHopOpts {
    bool force = false;  // true=无视无敌（仅调试）
    int vy = 0;          // ≤0 用默认 kImpactHopVyDefault
};
bool ImpactHopDeltaX(int deltaX, ImpactHopOpts opts = {});

// F6 Impact 飞 / F5 贴怪：朝世界坐标推一段冲量。
// NockBack=官方击退 helper；SetImpactNext=合并写入。均要求无敌（除非 force）。
enum class ImpactRoute : unsigned {
    NockBack = 0,
    SetImpactNext = 1,
};
struct ImpactTowardOpts {
    bool force = false;
    float maxSegPx = 160.f;   // 单段目标位移上限（adaptive 时作远距天花板）
    float minSegPx = 12.f;    // 更近则跳过
    float maxSpeed = 0.f;     // ≤0 用 hop 默认 800；adaptive 时作远距天花板
    float speedScale = 0.f;   // ≤0 用 hop 表 *4；adaptive 时作远距天花板
    float maxAbsVy = 0.f;     // >0 时额外夹 |vy|（F5 空中贴怪防竖直甩出图）
    float leadSec = 0.f;      // >0 时按误差/光标速度动态超前（飞路径当前关闭）
    bool adaptive = false;    // 动态 drive/scale（飞路径当前关闭，代码保留）
    bool quietLog = false;    // true=跟飞安静：toward ~5Hz 抽样、不跟采 Attr
};
bool ImpactImpulseToward(float worldX, float worldY, ImpactRoute route,
                         ImpactTowardOpts opts = {});

// 飞控状态快照：VecCtrl.Ap 的位置 + 速度。闭环飞控（PD 阻尼/重力前馈）必须读速度，
// 否则只能按位置误差开环点射，重力累积的落速无从抵消 → 空中越推越偏。
// 直接 SEH 读（与 ImpactImpulseToward 同款），可在 worker 线程调用。
struct FlightState {
    bool ok = false;
    float x = 0.f, y = 0.f;    // Ap
    float vx = 0.f, vy = 0.f;  // Ap.V（y 向下为正）
    bool onFh = false;         // CurFh 非空 = 仍挂台
};
bool QueryFlightState(FlightState& out);

// 直给速度（px/s，y 向下为正），不走「位置误差 → 速度」映射，也不套 kImpactHopVxMin
// 的每轴 80 地板 —— 那个地板让近距无法做小修正，只能大踹，是空中限幅震荡的根因。
struct ImpactVelOpts {
    bool force = false;      // true=无视无敌（仅调试）
    float maxAbsVx = 900.f;  // 硬幅值上限（防写飞）
    float maxAbsVy = 420.f;
    float minAbs = 6.f;      // 两轴幅值都低于此则跳过（省 pump job）
    bool quietLog = true;
};
bool ImpactSetVelocity(float vx, float vy, ImpactRoute route, ImpactVelOpts opts = {});

}  // namespace x::features::ports::teleport
