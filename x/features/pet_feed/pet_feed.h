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

}  // namespace pet_feed
}  // namespace features
}  // namespace x
