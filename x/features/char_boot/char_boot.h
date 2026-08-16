#pragma once

namespace x::features::char_boot {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

// state != Idle（含 Arm / Done 那 2 秒）。补给开趟、世界地图 Spot 用这个互斥。
bool IsBusy();

}  // namespace x::features::char_boot
