#pragma once
// pet_loot — Classic TWMS 宠物吸物（扩盒 + 官方 TryPickUpDrop）

#include "xcat_pet_loot.h"

#include <Windows.h>

namespace x {
namespace features {
namespace pet_loot {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void ApplyConfig(const xcat::PetLootConfig& cfg);

}  // namespace pet_loot
}  // namespace features
}  // namespace x
