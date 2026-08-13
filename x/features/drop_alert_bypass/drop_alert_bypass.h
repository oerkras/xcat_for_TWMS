#pragma once

// Classic TWMS：战斗中可丢物 + 抑制客户端警戒（预期行为）。
// 主路径：清 LocalUser alert 字段；已是 0 则 1s hold，非 0 则 32ms 快清。
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
