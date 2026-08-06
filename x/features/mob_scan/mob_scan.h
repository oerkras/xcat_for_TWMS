#pragma once
// mob_scan — P1 活怪扫描 worker：周期 Collect MobPool 并打 mobscan 日志

#include <Windows.h>

#include <cstdint>

namespace x {
namespace features {
namespace mob_scan {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

// 打怪开时的扫描周期（ms）；闲置仍用 worker 内固定 idle 间隔。
void SetCombatIntervalMs(uint32_t ms);
uint32_t GetCombatIntervalMs();

}  // namespace mob_scan
}  // namespace features
}  // namespace x
