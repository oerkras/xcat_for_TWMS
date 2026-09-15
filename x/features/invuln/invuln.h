#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace invuln {

// Data-plane invuln (v2.6.8): +0x2A0 i-frame; anti-blink = frame tick + 8ms backup @+0x2B0.
// Soft tick gate disabled. Bind SSOT = WM.MyUser@+0x28, FindAll fallback.
// Field hashes remounted 2026-09-10（hit int@0x2A0 / layer uint@0x2B0；勿再用 0x270 指针槽）.
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
// F5 滑翔 ≤1.00X：运行时否决写字段。不改 gDesired / ini；倍率升高后若勾着会恢复。
void SetWalkGlideVeto(bool on);
bool IsWalkGlideVeto();
// 勾着无敌但被 1.00X 否决：Combat / Travel / Gather 旋翼仍可发 Impact（含地上起飞）。
// F6 手动飞另走 Owner::Fly force，不依赖本闸。用户自己关掉无敌时 F5 仍走原 invuln_off。
bool HeliOverrideInvulnGate();

}  // namespace invuln
}  // namespace features
}  // namespace x
