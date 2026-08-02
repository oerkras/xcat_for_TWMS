#pragma once
// simple_combat — Classic TWMS 站桩自动打怪（状态机重设计）
//
// Idle → Acquire → [MoveTo→Settling] → Aim → Firing → Recover → …
// 瞬移与出刀互斥；F5 / 面板启停。

#include <Windows.h>
#include <cstdint>

namespace x::features::simple_combat {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();

void SetAttackIntervalMs(uint32_t ms);
// 状态机 worker 心跳 ms（面板「TICK值」）；下限 5。
void SetTickIntervalMs(uint32_t ms);
void SetSmartInterval(bool on);
void SetClusterPriority(bool on);
bool IsClusterPriority();
void SetTeleportEnabled(bool on);
void SetLiveStepEnabled(bool on);
bool IsLiveStepEnabled();
void SetTeleportParams(uint32_t minDx, uint32_t standOff, uint32_t cooldownMs);
void SetExternalPause(bool on);
// 可重叠的短暂停战（buffs / timed_keys）：成对 Acquire/Release，勿与绝对 Set 混用语义。
void AcquireExternalPause();
void ReleaseExternalPause();
void ResetForMapChange();

// F11 / 测试贴怪：fill+Doing → 随机活怪身边（长距绝对落点）。
void RequestNativeTeleportToRandomMob();
void RequestTeleportToRandomMob();

// 面板「原生CALL」：短距 ~140px fill+Doing。
void RequestNativeTeleportCall();

// 踢号压测：随机贴怪 fill+Doing，间隔由慢→快直到断线；见 combat.log DONE。
void RequestTeleportKickStress();
// 细扫档：50→0ms，步进 5ms，每级 12 跳。
void RequestTeleportKickStressFine();
// 钉地板：30→10ms，步进 5ms，每级 12 跳（不到 5/0，验证 10ms 是否可过）。
void RequestTeleportKickStressFine10();
// 原地短跳：同台 ±120px 来回，CD 扫档对齐 fine0-50（排除远距因素）。
void RequestTeleportKickStressLocal();
void StopTeleportKickStress();
bool IsTeleportKickStressActive();

void Tick(DWORD nowMs);

}  // namespace x::features::simple_combat
