#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace fly {

void Init();
void Shutdown();
// Spawns / stops the 60Hz worker. Safe to call from DllMain: StartWorker only creates a
// thread, StopWorker only signals it (never joins — that would deadlock the loader).
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();
bool IsEnabled();
float GetSpeed();
void SetSpeed(float v);
void TickRealtime();
bool PollFlyHotkey();
void ToggleFly();
void ForceRebind();
void PreferCameraBind();

}  // namespace fly
}  // namespace features
}  // namespace x
