#pragma once

#include <cstdint>

namespace x::features::multi_skill {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetConfig(bool enabled, uint32_t gapMs, bool safeStagger);
bool IsEnabled();
// 可选：多发技能优先 SendSkillUseRequest（默认关）。
void SetSendUseRequest(bool on);

// 供 simple_combat.Fire 与面板「测试一波」调用（转发 ports::multi_skill::TryCast）。
bool TryCast(char* out, int outSz);
bool IsBurstBusy();
bool CancelPendingBurstForRetarget();

}  // namespace x::features::multi_skill
