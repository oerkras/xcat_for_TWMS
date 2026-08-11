#pragma once
// final_attack_force — 普攻必出终极一击（Final Attack）
//
// 两层：
// 1) 数据面：SkillLevelData.Prop(+0x84)→100（掷骰必过）
// 2) 强制注册：写 UserLocal.FinalAttack@+0x3A4 并主线程调 TryDoingFinalAttack
//    （日志已证 Prop 写成功仍无效时，卡在空 FinalAttack 列表/武器门）
// 禁止 GameAssembly .text / E9。
// 已弃用：kFinalAttackForceUserEnabled=false（不启 worker；读盘/Apply/UI 强制关）。
// 代码保留；重开把该门闩改 true。辅助接口 QueryEquippedWeaponType / EquippedWeaponIsBowFamily 仍可用。

namespace x {
namespace features {
namespace final_attack_force {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();

// 当前装备武器 MapleWeaponType（30–49）；读失败 0。弓=45、弩=46。
int QueryEquippedWeaponType();
// 弓 / 弩：多发「贴脸挥弓串行」仅对此类武器生效。
bool EquippedWeaponIsBowFamily();

}  // namespace final_attack_force
}  // namespace features
}  // namespace x
