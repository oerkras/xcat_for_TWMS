#pragma once

#include <cstdint>
#include <string>

namespace xcat {

// 经典版宠物吸物：user.ini [pet_loot]。对照枫星 autopickup.fullMapEnabled 语义。
// 正式吸物只走宠扩盒（Pet.TryPickUpDrop → ByPet → Pet.Send）。角色全图假位姿已移除。
constexpr uint32_t kPetLootMagic = 0x5843504Cu;  // 'XCPL'
constexpr uint32_t kPetLootVersion = 2u;  // v2：+charVac（人物直吸）
constexpr int kPetLootMaxSkipRules = 64;
constexpr int kPetLootNameKeyLen = 64;

constexpr uint32_t kPetLootFilterMeso = 1u << 0;
constexpr uint32_t kPetLootFilterEquip = 1u << 1;
constexpr uint32_t kPetLootFilterConsume = 1u << 2;
constexpr uint32_t kPetLootFilterEtc = 1u << 3;
constexpr uint32_t kPetLootFilterInstall = 1u << 4;
constexpr uint32_t kPetLootFilterCash = 1u << 5;
constexpr uint32_t kPetLootFilterDefault =
    kPetLootFilterMeso | kPetLootFilterEquip | kPetLootFilterConsume | kPetLootFilterEtc;

// 默认调度 800ms × 1（可改；打怪同开时勿长期压到 50 / 高 burst）
constexpr uint32_t kPetLootIntervalDefaultMs = 800u;
constexpr uint32_t kPetLootCharVacIntervalMs = kPetLootIntervalDefaultMs;  // 语义别名
constexpr uint32_t kPetLootIntervalMinMs = 50u;
constexpr uint32_t kPetLootIntervalMaxMs = 2000u;
// 每 Tick 连调官方吸物次数（1–5）
constexpr uint32_t kPetLootBurstDefault = 1u;
constexpr uint32_t kPetLootBurstMin = 1u;
constexpr uint32_t kPetLootBurstMax = 5u;
// 宠吸有效真空：只扩人物/宠附近（默认 300×200）；旧全图 3200×2400 已废弃
constexpr float kPetLootVacuumWDefault = 300.f;
constexpr float kPetLootVacuumHDefault = 200.f;
// 兼容旧名：现与 Near 默认同值（勿再当全图尺寸用）
constexpr float kPetLootMapVacuumW = kPetLootVacuumWDefault;
constexpr float kPetLootMapVacuumH = kPetLootVacuumHDefault;
constexpr float kPetLootVacuumMax = 4000.f;
// 人物直吸：半宽/半高；全盒默认与宠吸同近身 300×200（半盒 150×100；Normalize 钉死）
constexpr float kPetLootCharHalfWDefault = kPetLootVacuumWDefault * 0.5f;
constexpr float kPetLootCharHalfHDefault = kPetLootVacuumHDefault * 0.5f;
constexpr float kPetLootCharHalfMax = 2000.f;
// 人物直吸用户面开关：false=保留实现但禁止启用（ini 强制关、ImGui 置灰）。改 true 可上架。
constexpr bool kPetLootCharVacUserEnabled = false;
// 与官方 Pet 金币例外占位一致（CMS MesoDummyItemIdForPetExceptionList）
constexpr int kPetLootMesoSkipId = 2147483647;

struct PetLootSkipRule {
    uint32_t enabled = 1;
    uint32_t itemId = 0;  // 非 0 优先
    char nameKey[kPetLootNameKeyLen] = {};
};

struct PetLootConfig {
    uint32_t magic = kPetLootMagic;
    uint32_t version = kPetLootVersion;
    uint32_t enabled = 0;           // 宠吸（人物附近真空，默认 300×200）
    uint32_t footEnabled = 0;       // 角色脚边（与宠吸互斥；默认关）
    uint32_t mapVacuumEnabled = 0;  // 与 enabled 同开；保留键兼容旧 ini（不再表示全图）
    uint32_t intervalMs = kPetLootIntervalDefaultMs;
    uint32_t burstPerTick = kPetLootBurstDefault;  // 每拍连吸次数 1–5
    float vacuumW = kPetLootVacuumWDefault;  // 宠吸有效尺寸；Normalize 钉默认近身盒
    float vacuumH = kPetLootVacuumHDefault;
    uint32_t charVacEnabled = 0;  // 人物直吸（与宠吸/脚边互斥）
    float charHalfW = kPetLootCharHalfWDefault;
    float charHalfH = kPetLootCharHalfHDefault;
    uint32_t filterFlags = kPetLootFilterDefault;
    uint32_t skipFilterEnabled = 1;
    uint32_t skipRuleCount = 0;
    PetLootSkipRule skipRules[kPetLootMaxSkipRules]{};
    uint64_t writeTickMs = 0;
};

void PetLootSetDefaults(PetLootConfig& out);
void PetLootNormalize(PetLootConfig& cfg);
uint32_t PetLootClampIntervalMs(uint32_t ms);
uint32_t PetLootClampBurstPerTick(uint32_t n);
void PetLootEffectiveVacuum(const PetLootConfig& cfg, float& outW, float& outH);
// 人物直吸半盒：Normalize 已钉近身 300×200 全盒（半宽高 150×100）
void PetLootEffectiveCharHalf(const PetLootConfig& cfg, float& outHalfW, float& outHalfH);

bool ReadPetLoot(const char* binDir, PetLootConfig& out);
bool WritePetLoot(const char* binDir, const PetLootConfig& cfg);

}  // namespace xcat

