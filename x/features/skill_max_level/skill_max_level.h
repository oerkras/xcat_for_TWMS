#pragma once
// skill_max_level — 已学技能按满级生效（客户端）
//
// A 主路径：写 CharacterData.SkillRecord / SkillRecordEx 等级 → GetMaxLevel
// B fallback：MethodInfo 钩 UserLocal.GetSkillLevel，orig≥1 时抬到满级
//            （抗同步打回 / 字典写失败；日志 src=dict|hook|dict+hook）
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

}  // namespace skill_max_level
}  // namespace features
}  // namespace x
