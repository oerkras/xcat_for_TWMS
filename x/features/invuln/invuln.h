#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace invuln {

// Data-plane invuln (v2.6.3): +0x298 i-frame; anti-blink = frame tick + 8ms backup.
// Soft tick gate disabled. Optional read-only +0x228 probe: XCAT_INVULN_PROBE=1.
// Rebind 400ms + 1.5s bind grace; LU drop does not clear SecondaryStat.
// No hotkey — panel / user.ini [core] invuln / XCAT_INVULN=1 only.
// No GA .text hooks.
void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();
bool IsEnabled();

}  // namespace invuln
}  // namespace features
}  // namespace x
