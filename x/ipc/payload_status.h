#pragma once
// TWMS payload → launcher：精简 PayloadStatus 心跳（CCU + 顶栏 5 灯）。

namespace x::ipc {

void PayloadStatus_Start();
void PayloadStatus_Stop();
void PayloadStatus_Publish();

}  // namespace x::ipc
