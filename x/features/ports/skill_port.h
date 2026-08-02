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
int GetSkillLevel(int skillId);
// CharacterData.SkillCooltime 剩余秒（无 CD / 未命中返回 0）。
float GetSkillCooldownRemainSec(int skillId);

// 解析显示名：offline-first（dataservice/skill_names.tsv）→ Il2Cpp SkillEntry.Name → skillId 字符串。
bool ResolveSkillName(int skillId, char* out, int outSz);

// SkillInfo.GetSkill → SkillEntry*（未学也可能有表项；失败 null）。
void* GetSkillEntry(int skillId);

// heavy：SkillRecord(+Ex) 优先；条目过少时回退扫 SkillInfo._dictionarySkill + GetSkillLevel。
int ListLearnedSkills(SkillInfoLite* out, int cap);

// 主线程出刀：优先 UserLocal.DoActiveSkill(skillId)（对标枫星 UseOnClientImmediate）；
// 失败再回退 DoActiveSkillPrepare（才需要 SkillInfo）。不直调 SendSkillUseRequest。
// outReason：ok_do_active / ok_prepare / prepare_false / do_false_* / no_level / …
// 带 _mi0 后缀表示 MethodInfo 未解析到（调用仍可能成功，便于对照）。
// 注意：不再因 SkillInfo 未绑定而提前返回 no_si（主路径不依赖 SI）。
bool CastSkill(int skillId, bool* notReady = nullptr, char* outReason = nullptr, int reasonSz = 0);
}  // namespace x::features::ports::skill
