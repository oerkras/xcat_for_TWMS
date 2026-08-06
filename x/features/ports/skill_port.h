#pragma once
// skill_port — Classic TWMS skill presence / learn / cast (BUFF manager).
// 真源：docs/features/buffs/P0a_锚点复核.md
// 禁止 INLINE HOOK / SendInput。

#include <cstdint>

namespace x::features::ports::skill {

constexpr int kMaxActiveSkills = 64;
constexpr int kMaxLearnedSkills = 256;

struct ActiveSkill {
    int skillId = 0;
    int startTime = 0;
};

struct SkillInfoLite {
    int skillId = 0;
    int level = 0;
    bool learned = false;
    bool active = false;
    float remainBuffSec = 0.f;
    float remainCooldownSec = 0.f;
    float cooldownSec = 0.f;
    char name[128]{};
    char code[32]{};
};

void Init();
void Shutdown();
bool EnsureBound();
bool Ready();

// 只读：MyUser 在身 AffectedSkill 列表。
int ListActiveSkills(ActiveSkill* out, int cap);

bool IsSkillActive(int skillId, float* outRemainSec = nullptr);
// UserLocal.PreparingSkill.SkillID≠0（meta 防漂，fallback @0x398）：DoActive / 手搓技能警戒态（禁瞬移）。
bool IsPreparingSkill(int* outSkillId = nullptr);
int GetSkillLevel(int skillId);
// 技能冷却剩余秒：优先有效 CoolTimeOver；否则用本地倒数（仅 verify ok 后 ConfirmLocalCooldown）。
// 无 CD / 未命中返回 0。
float GetSkillCooldownRemainSec(int skillId);
// 表内「再使用冷却」总时长（秒）；未知返回 0。供 UI cooldownSec。
float GetSkillCooldownDurationSec(int skillId);
// 确认效果在身后记本地 CD（勿在 CastSkill 报 ok 时调用——假 ok 会种出假 CD）。
void ConfirmLocalCooldown(int skillId);

// 游戏逻辑钟 WorldManager.GetUpdateTime = (int)(_updateTime*1000)，取不到返回 0。
// SecondaryStat 的 tXxx_ 到期字段一律以此为基；写 GetTickCount 会让 CheckByTime 判错。
int GetGameUpdateTimeMs();

// 解析显示名：offline-first（dataservice/skill_names.tsv）→ Il2Cpp SkillEntry.Name → skillId 字符串。
bool ResolveSkillName(int skillId, char* out, int outSz);

// SkillInfo.GetSkill → SkillEntry*（未学也可能有表项；失败 null）。
void* GetSkillEntry(int skillId);

// heavy：SkillRecord(+Ex) 优先；条目过少时回退扫 SkillInfo._dictionarySkill + GetSkillLevel。
int ListLearnedSkills(SkillInfoLite* out, int cap);

// 主线程出刀：优先 UserLocal.DoActiveSkill(skillId)（对标枫星 UseOnClientImmediate）；
// 失败再回退 DoActiveSkillPrepare（才需要 SkillInfo）。默认不直调 SendSkillUseRequest。
// outReason：ok_do_active / ok_prepare / prepare_false / do_false_* / no_level / …
// 带 _mi0 后缀表示 MethodInfo 未解析到（调用仍可能成功，便于对照）。
// 注意：不再因 SkillInfo 未绑定而提前返回 no_si（主路径不依赖 SI）。
bool CastSkill(int skillId, bool* notReady = nullptr, char* outReason = nullptr, int reasonSz = 0);

// 可选：优先直调 SendSkillUseRequest(SkillEntry,…)；失败仍回退 CastSkill 主路径。
// 供多发面板开关；BUFF 等原调用方不受影响。
bool CastSkillPreferSendUse(int skillId, bool* notReady = nullptr, char* outReason = nullptr,
                            int reasonSz = 0);
}  // namespace x::features::ports::skill
