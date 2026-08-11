#pragma once
// Classic TWMS — soft ConnectLoginServer try-connect probe (default OFF).
// After disconnect, call SceneLogin's no-arg connect starter (starts ConnectLogin
// coroutine → NM.ConnectServer). On Connected: CloseDialog (not OnClickYes/Ok) on
// UIUtilDialog(s), RequestRestart auto_enter, hold until play-ready **and**（图内）curFh≠0
//（悬空不 RESULT；满 kStandReadyWaitMs 降级成功）, or timeout → fail →
// guardian clean relaunch). Recoverable fails retry up to kSoftCycleMax soft cycles;
// Done+!inMap may RequestRestart up to kDoneNoPlayMaxRestarts per cycle;
// Done+inMap wall timeout → degrade success (no ConnectLogin soft cycle).
// While observing, publishes softLoginHold via PayloadStatus：守护推迟一切干净重拉
//（踢线/无经验/心跳等）；须 softLoginResult=2（完全失败）或进程已死才允许重拉。
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

// 调试 / 手动：安全关断线弹窗（CloseDialog + SetActive，绝不点確認/Yes）。
// 不依赖软重连是否武装；须在已注入且泵可用时调用。
void RequestManualDismiss();

}  // namespace x::features::soft_login_probe
