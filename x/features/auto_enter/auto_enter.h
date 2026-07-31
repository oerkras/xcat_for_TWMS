#pragma once
#include <Windows.h>
#include <cstdint>

namespace x {
namespace features {
namespace auto_enter {

// Login auto-enter: world (panel) → least-populous channel → char slot → confirm.
// UI-semantic calls only; MethodInfo pump on Canvas.SendWillRenderCanvases (no GA .text).
void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetDesired(bool on, int32_t worldId, const char* worldName, uint32_t charSlot);
bool IsDesired();

}  // namespace auto_enter
}  // namespace features
}  // namespace x
