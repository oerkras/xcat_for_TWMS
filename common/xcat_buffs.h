#pragma once

#include <cstdint>
#include <string>

namespace xcat {

// BUFF 续航：优先 user.ini [buffs]；旧 buffs.bin 只读 migrate。
// ReadBuffs：无 [buffs]/bin 时返回 false（out 仍为 defaults），便于 LKG/注入治愈。
// runtime 快照：payload 写 Local\XCatBuffsRuntime_v2_<hash> SHM；launcher 优先读 SHM，旧 buffs_runtime.bin 只读回退。
// 产品：经典版 TWMS（Maplestory_Classic.exe）· 不是枫星。
// 产品边界：仅技能 BUFF 续航（药水归 autopot）。每槽 code=数值 skillId + 续航策略。
// 经典版事实源：在身读 User._listAffectedSkillEntry；辅证 SecondaryStat r/t；学级 UserLocal.GetSkillLevel；
// 施放 DoActiveSkillPrepare → SendSkillUseRequest。契约结构对齐对照仓，实现面独立。

constexpr uint32_t kBuffsMagic    = 0x46465542u;  // 'BUFF'
constexpr uint32_t kBuffsVersion  = 2u;
constexpr size_t   kBuffSlotCount = 16u;

constexpr uint32_t kBuffsRuntimeMagic   = 0x46554252u;  // 'RBUF'
constexpr uint32_t kBuffsRuntimeVersion = 2u;
constexpr size_t   kBuffsRuntimeMaxSkills = 128u;

// 运行时 BUFF 管理器已接管旧「定时重放」入口。
constexpr bool kBuffsFeatureSuspended = false;

enum BuffSlotKind : uint32_t {
    kBuffKindSkill = 0,  // 技能 BUFF：CharacterSkillManager:TryUseOnClient(skillId)
    kBuffKindItem  = 1,  // 遗留枚举：读到即禁用，不再施放（请用 autopot）
};

enum BuffRenewStrategy : uint32_t {
    kBuffRenewByPresence = 0,  // BUFF 不存在/即将过期且 CD 可用时补
    kBuffRenewByCooldown = 1,  // CD 好且 BUFF 不在身上时补
    kBuffRenewByInterval = 2,  // 旧行为兜底：按 intervalSec 到点重放
};

#pragma pack(push, 1)
struct BuffSlotConfig {
    uint32_t enabled     = 0;            // 0/1
    uint32_t kind        = kBuffKindSkill;
    char     code[64]    = {};           // 数值 skillId 字符串（如 "1101006"）
    uint32_t intervalSec = 180;          // 重放间隔（秒）
    uint32_t strategy    = kBuffRenewByPresence;
};

struct BuffsConfig {
    uint32_t       magic         = kBuffsMagic;
    uint32_t       version       = kBuffsVersion;
    uint32_t       masterEnabled = 0;
    BuffSlotConfig slots[kBuffSlotCount]{};
    uint64_t       writeTickMs   = 0;
    // launcher「刷新」递增；payload 见新值则强制 heavy 重扫技能书并写 runtime 快照
    uint32_t       refreshSeq    = 0;
};

struct BuffsRuntimeSkill {
    char     code[64] = {};
    char     name[128] = {};
    char     typeAbbr[16] = {};
    uint32_t inJob = 1;
    uint32_t learned = 0;
    uint32_t show = 1;
    uint32_t active = 0;
    uint32_t canCast = 0;
    float    remainBuffSec = 0.0f;
    float    durationSec = 0.0f;
    float    remainCooldownSec = 0.0f;
    float    cooldownSec = 0.0f;
};

struct BuffsRuntimeSnapshot {
    uint32_t magic = kBuffsRuntimeMagic;
    uint32_t version = kBuffsRuntimeVersion;
    uint32_t count = 0;
    uint32_t ready = 0;
    uint64_t writeTickMs = 0;
    char     status[96] = {};
    BuffsRuntimeSkill skills[kBuffsRuntimeMaxSkills]{};
    // 最近一次完成的 UI「刷新」seq（heavy 成功后回写；launcher 用它确认列表已更新）
    uint32_t refreshAckSeq = 0;
};
#pragma pack(pop)

void BuffsSetDefaults(BuffsConfig& out);
bool BuffsAnySlotEnabled(const BuffsConfig& cfg);
// 保留 API：master 与 slot 启用相互独立，不再把「有启用槽」强制写成 master=1。
void BuffsNormalizeMasterEnabled(BuffsConfig& cfg);

std::string BuffsRelPath();
std::string BuffsPath(const char* binDir);

bool ReadBuffs(const char* binDir, BuffsConfig& out);
bool WriteBuffs(const char* binDir, const BuffsConfig& cfg);

// 磁盘 LKG（state\buffs.lkg）：user.ini [buffs] 瞬时 miss / 冷启动无进程缓存时，
// 给 launcher heal 用；不替代 ReadBuffs 权威源。
std::string BuffsLkgRelPath();
std::string BuffsLkgPath(const char* binDir);
bool ReadBuffsLkgFile(const char* binDir, BuffsConfig& out);
bool WriteBuffsLkgFile(const char* binDir, const BuffsConfig& cfg);

void BuffsRuntimeSnapshotSetDefaults(BuffsRuntimeSnapshot& out);

std::string BuffsRuntimeSnapshotRelPath();
std::string BuffsRuntimeSnapshotPath(const char* binDir);

bool ReadBuffsRuntimeSnapshot(const char* binDir, BuffsRuntimeSnapshot& out);
bool WriteBuffsRuntimeSnapshot(const char* binDir, const BuffsRuntimeSnapshot& snapshot);

// BUFF 列表显示名：offline-first（skill_names.tsv）→ 可信传入名 → 已知补表 → code。
bool BuffNameLooksLikeCode(const char* code, const char* name);
// 返回静态缺口补表命中的显示名；未命中返回 nullptr。
const char* BuffKnownDisplayName(const char* code);
// 写入 out：dataservice/skill_names.tsv → 可信 name → 已知补表 → name/code。
// payloadBinDir = XCat_data/（可空；空则跳过离线表）。
void BuffSkillDisplayLabel(const char* code, const char* name, char* out, size_t outSz,
                           const char* payloadBinDir = nullptr);

}  // namespace xcat
