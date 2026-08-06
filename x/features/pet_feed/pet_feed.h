#pragma once
// pet_feed — Classic TWMS 自动召唤宠物（喂食交官方）

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
// 自动召唤开、场上无宠、且仍在尝试时：打怪应先等（防开打进警戒后召唤被拒）。
// 永久不可召（无宠/无粮）或超时后返回 false，避免卡死挂机。
bool ShouldHoldCombatForSummon();

}  // namespace pet_feed
}  // namespace features
}  // namespace x
