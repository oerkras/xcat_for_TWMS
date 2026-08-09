#pragma once
// Classic TWMS — 不挥弓（弓/弩贴身仍射箭；主动技贴身不改近战）。
//
// 四层 abs-hook（武器 45/46 + 弓系职业兜底）——拉弦生效版：
// 1) CED7E0 → false
// 2) TryDoingShootAttack → isMortalBlow=1
// 3) TryDoingMeleeAttack → 改道 Shoot(MB=1)
// 4) FindHitMobInRect → force 时 Rect X ±120
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
