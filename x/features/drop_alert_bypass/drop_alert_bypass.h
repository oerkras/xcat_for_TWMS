#pragma once

// Classic TWMS：战斗中可丢物 + 抑制客户端警戒（预期行为）。
// 主路径：改写 IsAlertMode 专用 threshold global（imm+global→INT_MAX），装一次即可；
// 出刀刷 +0x118 无空窗。关功能 restore。次路径：CanPerformAction MethodInfo。
// No GameAssembly .text patch / 不占 HWBP。Server Drop authority unchanged.
// 对照：枫星 drop_alert_bypass（Lua IsAlert）；本仓不搬 Lua。

#include <Windows.h>

namespace x::features::drop_alert_bypass {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

void Tick(DWORD now);

}  // namespace x::features::drop_alert_bypass
