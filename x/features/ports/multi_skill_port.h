#pragma once

#include <cstdint>

namespace x::features::ports::multi_skill {

// Classic TWMS 技能多发端口。
// 对照枫星 multi_skill_port 语义（清单/gap/busy）；不搬 Lua Immediate。
// 技能默认 CastSkill(DoActive)；可选 SendSkillUseRequest。
// 多发普攻：TryFirePrimaryForMultiSkill；间隔地板=实测 ActionBusy 周期（不低于官方出刀节奏）。
// 技能：排程与到期前跳过 CoolTimeOver/本地 CD 仍剩余者（只发不在 CD 的）。
// 首版不做连招扩族 / 职业 DPS 优化。

void SetConfig(bool enabled, uint32_t gapMs, bool safeStagger);
// 可选：技能优先 SendSkillUseRequest（默认关；失败回退 DoActive）。不影响普攻。
void SetSendUseRequest(bool on);
bool GetSendUseRequest();
bool IsEnabled();
bool GetSafeStagger();
uint32_t GetGapMs();

bool HasSelection();
void ReloadSelectionNow();

void GetLastBurst(uint32_t& scheduled, uint32_t& skip, uint32_t& spanMs, uint32_t& selCount,
                  uint32_t& castCount, unsigned long long& tickMs);

bool IsBurstBusy();
bool CancelPendingBurstForRetarget();

// 排程一波：首技 due=now，后续按 effective gap。busy 中返回 false（reason=busy）。
// out 可空；成功写入 ok / busy / no_select / disabled。
bool TryCast(char* out, int outSz);

// worker 每帧调用：到期项走 skill_port::CastSkill（或可选 SendUse）。
void Tick();

void Init();
void Shutdown();

}  // namespace x::features::ports::multi_skill
