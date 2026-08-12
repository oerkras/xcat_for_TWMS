// 经典版 TWMS —— 近战不挥拳（melee_veto）
//
// 贴脸时客户端会把普攻分流到近战（弓=挥弓、爪=挥拳）。本模块尝试把普攻的那一发近战
// 直接判负，让分发器 CEB600 落到兜底的射击分支。
//
// ★ 弓上已证伪：弓的普攻伤害本身就在近战体内转调射击产生，判负会让怪完全不掉血
//   （见 Dumps/runtime/ARCHER_SHOOT_VS_BONK_GATE_20260809.md §0″/§0‴）。
//   所以本模块**先测量、再决定**：观测到近战体内转调射击（nest>0）就拒绝拦截。
#pragma once

namespace x {
namespace features {
namespace melee_veto {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

// 面板「近战不挥拳」。关 = 卸钩，与原逻辑逐位相同。
void SetEnabled(bool on);
bool IsEnabled();

}  // namespace melee_veto
}  // namespace features
}  // namespace x
