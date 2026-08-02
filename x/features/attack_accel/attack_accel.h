#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace attack_accel {

// 攻击加速：启用后
//   1) 清忙锁 LocalUser+0x118=-1（跳过动作等待）
//   2) 写 SecondaryStat+0x1BC=140（Prepare 攻速上限；+0x1C4 远期 expire）
// 出刀频率看面板「间隔」/ simpleCombatAttackIntervalMs（默认 50，下限 5）。
// 非「技能无 CD」。禁止 GA .text hook。
void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();

}  // namespace attack_accel
}  // namespace features
}  // namespace x
