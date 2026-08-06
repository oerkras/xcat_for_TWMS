#pragma once
// skill_max_level — 已学技能按满级生效（客户端）
//
// A 主路径：写 CharacterData.SkillRecord / SkillRecordEx 等级 → GetMaxLevel
// B fallback：MethodInfo 钩 UserLocal.GetSkillLevel +
//            SkillInfo.GetSkillLevel / GetPureSkillLevel（orig≥1 抬满级；
//            Pure 一并抬是本功能语义，非「保留纯加点」）
// C 双保险：ports::skill::GetSkillLevel（及施法路径）经 AdjustLevelIfForced
// 关掉还原字典原等级并卸钩。服端结算以服为准。
// 默认关；面板 / user.ini [core] skillMaxLevel / XCAT_SKILL_MAX_LEVEL=1。

namespace x {
namespace features {
namespace skill_max_level {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();

// feature 开且 raw≥1 时抬到满级；否则原样返回。供 skill_port / Hook 共用。
int AdjustLevelIfForced(int skillId, int rawLevel);

}  // namespace skill_max_level
}  // namespace features
}  // namespace x
