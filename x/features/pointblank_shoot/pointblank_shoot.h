#pragma once
// Classic TWMS — 不挥弓（弓/弩贴身仍射箭；主动技贴身不改近战）。
//
// 三层 abs-hook（武器 45/46 + 弓系职业兜底）：
// 1) CED7E0 → false
// 2) TryDoingShootAttack → isMortalBlow=1
// 3) TryDoingMeleeAttack → 改道 Shoot，禁止挥弓落点
//
// 见 Dumps/runtime/ARCHER_SHOOT_VS_BONK_GATE_20260809.md

namespace x::features::pointblank_shoot {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
bool IsActive();

}  // namespace x::features::pointblank_shoot
