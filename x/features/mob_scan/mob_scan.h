#pragma once
// mob_scan — P1 活怪扫描 worker：周期 Collect MobPool 并打 mobscan 日志

#include <Windows.h>

namespace x {
namespace features {
namespace mob_scan {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

}  // namespace mob_scan
}  // namespace features
}  // namespace x
