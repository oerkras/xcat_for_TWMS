#pragma once
// mob_gather — 吸怪 feature 壳（首页：oneshot + 周期，默认关，落盘 user.ini）

namespace x::features::mob_gather {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

}  // namespace x::features::mob_gather
