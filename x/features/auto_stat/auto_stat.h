#pragma once

#include "xcat_auto_stat.h"

#include <Windows.h>

namespace x::features::auto_stat {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void ApplyConfig(const xcat::AutoStatConfig& cfg);

}  // namespace x::features::auto_stat
