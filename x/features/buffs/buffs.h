#pragma once

#include "xcat_buffs.h"

namespace x::features::buffs {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void ApplyConfig(const xcat::BuffsConfig& cfg);
bool IsMasterEnabled();

// ExternalPause / 施法闸：simple_combat 与出刀门同步查询
bool IsCastingBusy();

}  // namespace x::features::buffs
