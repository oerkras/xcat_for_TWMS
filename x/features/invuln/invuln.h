#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace invuln {

// Data-plane invuln (v2.6.5): +0x298 i-frame; anti-blink = frame tick + 8ms backup.
// Soft tick gate disabled. Bind SSOT = WM.MyUser@+0x28, FindAll fallback.
// Field hashes remounted 2026-08-06 (User/SS class+field); offs fallback 0x298/0x2A8/….
// Rebind: WM path unthrottled when unbound; FindAll MapScene-only（InterStage 禁扫，防拖黑屏）。
// Optional read-only +0x228 probe: XCAT_INVULN_PROBE=1.
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
