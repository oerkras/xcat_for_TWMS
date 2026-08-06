#pragma once
// final_attack_force — 普攻必出终极一击（Final Attack）
//
// 两层：
// 1) 数据面：SkillLevelData.Prop(+0x84)→100（掷骰必过）
// 2) 强制注册：写 UserLocal.FinalAttack@+0x3A4 并主线程调 TryDoingFinalAttack
//    （日志已证 Prop 写成功仍无效时，卡在空 FinalAttack 列表/武器门）
// 禁止 GameAssembly .text / E9。
// 默认关；面板 / user.ini [core] finalAttackForce / 环境变量 XCAT_FINAL_ATTACK_FORCE=1。

namespace x {
namespace features {
namespace final_attack_force {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();

}  // namespace final_attack_force
}  // namespace features
}  // namespace x
