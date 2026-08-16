#pragma once
// Classic TWMS — soft ConnectLoginServer try-connect probe (default OFF).
// After disconnect, call SceneLogin's no-arg connect starter (starts ConnectLogin
// coroutine → NM.ConnectServer). On Connected: CloseDialog (not OnClickYes/Ok) on
// UIUtilDialog(s), RequestRestart auto_enter, hold until play-ready **and**（图内）curFh≠0
//（悬空不 RESULT；满 kStandReadyWaitMs 降级成功）, or timeout → fail →
// guardian clean relaunch). Recoverable fails retry up to kSoftCycleMax soft cycles;
// Done+!inMap may RequestRestart up to kDoneNoPlayMaxRestarts per cycle;
// Done+inMap wall timeout → degrade success (no ConnectLogin soft cycle).
// While observing, publishes softLoginHold via PayloadStatus：守护推迟踢线硬杀。
// 无经验/心跳停滞已满 noExpSec，或 hold 墙钟已满 noExpSec，守护仍干净重拉。
// 进程已死始终放行。softLoginResult=2 立即放行。
//
// 守护契约细节（见 docs/features/soft_login/模块设计.md §2.4）：
// - Status SHM 500ms 一拍 + 守护认 result 0→1 及 success 后短窗 → hold 至少按住
//   kMinHoldMs(2s)；land_quiet 期内 KickSniff 不得 bump disconnectSeq。否则观察窗
//   在两拍之间消失，守护看到「seq 涨了且 hold=0」就杀进程（BIN 17:29 / 01:00:25）。
// - 真断线 why 下 MapScene 残留不判 already_in_map 成功；但 playReady+挂台+Connected
//   稳住 kInMapRecoverConfirmMs(8s) 认「游戏自己恢复」，避免十分钟空 hold 后误杀。
// - 窗口内连续失败满额则暂停接管（熔断），把干净重拉交回守护，防止 hold 无限架空守护。
//
// SceneLogin 进图后可能暂为空（sl_null）：hold 内重试 ~20s，并接受游戏自连 Connecting；
// 勿在首次 sl_null 立刻放 hold（否则守护会杀进程重拉）。

namespace x::features::soft_login_probe {

void Init();
void Shutdown();
void SetEnabled(bool on);

void StartWorker();
void StopWorker();

bool IsArmed();
bool IsHoldActive();
// soft 成功进图后的落地静默（与 softLoginHold 解耦：Finish 已放 hold，守护可 absorb）。
bool IsLandQuiet();
// hold 或 land quiet：打怪/旋翼停；Invuln：hold 停写，land quiet 允许急钉。
bool IsGameplayQuiet();
// soft 成功后墙钟内禁止 F5 空中 fhBan/旋翼（ce6797：quiet 结束立刻 BAN ON → 高速 Impact → 再软断）。
// 比 land quiet 更长；不影响 hold 期 SafeLand / 测谎落台。
bool IsPostSoftAirCombatBlocked();
unsigned ResultCode();

void RequestAttempt(const char* why);

// 图内 CloseSession（安全踢等）常无 Disconnected 边沿，且 lost_session 被 inMap 门吞掉。
// 先粘住；worker 见 !inMap 或大厅 UI ready 后再 RequestAttempt（避免图内误 hold）。
void RequestDeferredAttempt(const char* why);

// 粘性武装已挂、尚未正式 attempt（图内主动拆会话后等回大厅）。
bool IsDeferredPending();
// worker 正在跑 ConnectLogin / 重进。
bool IsAttemptBusy();
// hold / 落地静默 / 粘性 / attempt：赶路当空档，禁止 wait_wm_field。
bool IsReconnectInFlight();

// 图内主动拆会话：先粘性武装，再泵上 CloseSession。不 SetHold（图内 hold 会闪成强制软重连）。
// 仅 Field+playReady；商城/拍卖/InterStage 拒绝。首页「软重连试连」未开则拒绝（禁止裸拆）。
bool RequestProactiveReconnect(const char* why);

// 调试 / 手动：安全关断线弹窗（CloseDialog + SetActive，绝不点確認/Yes）。
// 不依赖软重连是否武装；须在已注入且泵可用时调用。
void RequestManualDismiss();

}  // namespace x::features::soft_login_probe
