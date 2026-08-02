#pragma once

#include "xcat_timed_keys.h"

namespace x::features::timed_keys {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void ApplyConfig(const xcat::TimedKeysConfig& cfg);
bool IsMasterEnabled();

}  // namespace x::features::timed_keys
