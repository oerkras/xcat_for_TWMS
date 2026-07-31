#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace invuln {

// Data-plane invuln: latch LocalUser+0x298 (hit i-frame) + anti-blink @+0x2A8.
// SecondaryStat dual-write is non-gating. No GA .text hooks (GRAP).
void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();
bool IsEnabled();
void Toggle();

}  // namespace invuln
}  // namespace features
}  // namespace x
