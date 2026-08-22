#pragma once
// pet_feed — Classic TWMS 自动召唤宠物（喂食交官方）
// 用户入口：kPetSummonUserEnabled=false（置灰 + 不启 worker + 不发 ActivatePet）；代码保留。

#include <Windows.h>

namespace x {
namespace features {
namespace pet_feed {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
void SetRequireFood(bool on);

bool IsDesired();
// 自动召唤开、场上无宠、且仍在尝试时：走路/旋翼打怪应先等（防开打进警戒后召唤被拒）。
// 瞬移找怪由 simple_combat 跳过此门；召唤仍在后台跑。
// armBudget=false：只探询、不起 20s 让路钟（瞬移跳过时用）。
// 永久不可召（无宠/无粮）或超时后返回 false，避免卡死挂机。
bool ShouldHoldCombatForSummon(bool armBudget = true);

}  // namespace pet_feed
}  // namespace features
}  // namespace x
