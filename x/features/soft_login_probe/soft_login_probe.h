#pragma once
// Classic TWMS — soft ConnectLoginServer try-connect probe (default OFF).
// After disconnect, call SceneLogin's no-arg connect starter (starts ConnectLogin
// coroutine → NM.ConnectServer). On Connected: CloseDialog (not OnClickYes/Ok) on
// UIUtilDialog(s), RequestRestart auto_enter, hold until play-ready (or timeout → fail →
// guardian clean relaunch).
// While observing, publishes softLoginHold via PayloadStatus so hangup 守护推迟踢线干净重拉.
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
unsigned ResultCode();

void RequestAttempt(const char* why);

// 调试 / 手动：安全关断线弹窗（CloseDialog + SetActive，绝不点確認/Yes）。
// 不依赖软重连是否武装；须在已注入且泵可用时调用。
void RequestManualDismiss();

}  // namespace x::features::soft_login_probe
