#pragma once

#include <cstdint>

namespace xcat {

// 经典版自动加技能点：user.ini [auto_skill]。独立 section，不进 [core] / PayloadControl。
// enabled 是总闸（默认关）。job1Enabled/job2Enabled 表示该书已配好、纳入计划；
// 总闸关时 worker 不加，列表和职业选择保留。点数池按 ExtendSp JobLevel 0/1/2 分。
// 已 2/3/4 转仍可加 1/2 转剩余点。初心者未转职不发包。不加 3 转书。
constexpr uint32_t kAutoSkillMagic = 0x58534B4Cu;  // 'XSKL'
constexpr uint32_t kAutoSkillVersion = 1u;
constexpr uint32_t kAutoSkillDefaultEnabled = 0;
constexpr uint32_t kAutoSkillOrderMax = 24u;
constexpr int32_t kAutoSkillTargetMax = 60;

struct AutoSkillConfig {
    uint32_t magic = kAutoSkillMagic;
    uint32_t version = kAutoSkillVersion;
    // 总闸。Normalize 不再把它收成两本 OR；关总闸可保留分本配置。
    uint32_t enabled = kAutoSkillDefaultEnabled;
    uint32_t job1Enabled = 0;
    uint32_t job2Enabled = 0;
    int32_t job1 = 0;
    int32_t job2 = 0;
    uint32_t job1Count = 0;
    uint32_t job2Count = 0;
    int32_t job1Order[kAutoSkillOrderMax]{};
    int32_t job2Order[kAutoSkillOrderMax]{};
    // 与对应 Order 对齐。0 = 跟游戏满级；1..kAutoSkillTargetMax = 加到该级停。
    int32_t job1Target[kAutoSkillOrderMax]{};
    int32_t job2Target[kAutoSkillOrderMax]{};
    uint64_t writeTickMs = 0;
};

void AutoSkillSetDefaults(AutoSkillConfig& out);
void AutoSkillNormalize(AutoSkillConfig& cfg);
bool AutoSkillJob1Configured(const AutoSkillConfig& cfg);
bool AutoSkillJob2Configured(const AutoSkillConfig& cfg);
// 总闸开，且至少一本书已开且已配好。不含「人物当前转职」。
bool AutoSkillReady(const AutoSkillConfig& cfg);

bool ReadAutoSkill(const char* binDir, AutoSkillConfig& out);
bool WriteAutoSkill(const char* binDir, const AutoSkillConfig& cfg);
// lockTimeoutMs=0：锁被占立刻失败。启动器 ImGui 线程必须走这条，禁止 8 秒干等。
bool WriteAutoSkill(const char* binDir, const AutoSkillConfig& cfg, uint32_t lockTimeoutMs);

// 冒险家五职（战士/法师/弓手/盗贼/海盗）。双刀 430–434 不是冒险家，返回 0。
int AutoSkillJobFamily(int job);
bool AutoSkillIsExplorerJob1(int job);
bool AutoSkillIsExplorerJob2(int job);
// 冒险家五职且已离开初心者（含 3/4 转）。双刀返回 false。
bool AutoSkillIsExplorerAdvancement(int job);
// 人物已走上这条 2 转分支：job2 本身，或其后的 3/4 转（410 与 411/412）。
bool AutoSkillSameJob2Branch(int charJob, int job2);
// 该 2 转所属的 1 转（410 → 400）。对不上返回 0。
int AutoSkillJob1OfJob2(int job2);
// WZ 技能书 ID：sid<1000000 → 0（冒險之技）；否则 sid/10000（与 SkillRoot.SkillRootID 同口径）。
int AutoSkillBookJobOfSkill(int skillId);
// ExtendSp 的 JobLevel：新手 0；1 转 1；2 转 2；其它 0。
int AutoSkillJobLevel(int job);
void AutoSkillListJob1(int* out, uint32_t* count);
void AutoSkillListJob2(int job1, int* out, uint32_t* count);

const char* AutoSkillJobLabel(const char* binDir, int job);
const char* AutoSkillSkillName(const char* binDir, int skillId);
int AutoSkillMaxObserved(const char* binDir, int skillId);
// 启动器进 Tab 时预解析离线表，避免选职业那一帧才读 TSV。
void AutoSkillWarmCatalog(const char* binDir);
void AutoSkillWarmCatalogAsync(const char* binDir);

// 按该职业 WZ 技能书填默认顺序：须在 skill_meta 中、非 invisible/psd、无内部名。
// targets 可空；非空时与 ids 对齐，默认 0 = 跟游戏满级。
bool AutoSkillFillDefaultOrder(const char* binDir, int job, int* ids, uint32_t* count,
                               int* targets = nullptr);

}  // namespace xcat
