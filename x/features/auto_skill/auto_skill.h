#pragma once

#include "xcat_auto_skill.h"

#include <Windows.h>

namespace x::features::auto_skill {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void ApplyConfig(const xcat::AutoSkillConfig& cfg);

}  // namespace x::features::auto_skill
