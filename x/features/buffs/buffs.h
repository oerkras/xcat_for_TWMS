#pragma once

#include "xcat_buffs.h"

namespace x::features::buffs {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void ApplyConfig(const xcat::BuffsConfig& cfg);
bool IsMasterEnabled();

}  // namespace x::features::buffs
