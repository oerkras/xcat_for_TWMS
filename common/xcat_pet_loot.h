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

// 默认调度 200ms × 1（可改；打怪同开时勿长期压到 50 / 高 burst）
constexpr uint32_t kPetLootIntervalDefaultMs = 200u;
constexpr uint32_t kPetLootCharVacIntervalMs = kPetLootIntervalDefaultMs;  // 语义别名
constexpr uint32_t kPetLootIntervalMinMs = 50u;
constexpr uint32_t kPetLootIntervalMaxMs = 2000u;
// 每 Tick 连调官方吸物次数（1–5）
constexpr uint32_t kPetLootBurstDefault = 1u;
constexpr uint32_t kPetLootBurstMin = 1u;
constexpr uint32_t kPetLootBurstMax = 5u;
constexpr float kPetLootVacuumWDefault = 300.f;
constexpr float kPetLootVacuumHDefault = 200.f;
// 近图宠吸预设（对照枫星 fullMap 语义：扩宠盒，非手组包）
constexpr float kPetLootMapVacuumW = 3200.f;
constexpr float kPetLootMapVacuumH = 2400.f;
constexpr float kPetLootVacuumMax = 4000.f;
// 角色脚边拾取范围（半宽/半高，世界单位）；全盒默认 200×160
constexpr float kPetLootFootHalfWDefault = 100.f;
constexpr float kPetLootFootHalfHDefault = 80.f;
constexpr float kPetLootFootHalfMax = 2000.f;
// 人物直吸：半宽/半高；全盒默认 1500×1500（Normalize 钉死）
constexpr float kPetLootCharHalfWDefault = 750.f;
constexpr float kPetLootCharHalfHDefault = 750.f;
constexpr float kPetLootCharHalfMax = 2000.f;
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
    uint32_t enabled = 0;           // 宠吸（固定 3200×2400 .rdata 真空）
    uint32_t footEnabled = 0;       // 角色脚边（与宠吸互斥；默认关）
    uint32_t mapVacuumEnabled = 0;  // 与 enabled 同开；保留键兼容旧 ini
    uint32_t intervalMs = kPetLootIntervalDefaultMs;
    uint32_t burstPerTick = kPetLootBurstDefault;  // 每拍连吸次数 1–5
    float vacuumW = kPetLootVacuumWDefault;  // 占位；宠开时 EffectiveVacuum 覆盖
    float vacuumH = kPetLootVacuumHDefault;
    float footHalfW = kPetLootFootHalfWDefault;
    float footHalfH = kPetLootFootHalfHDefault;
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
// 人物直吸半盒：Normalize 已钉 1500×1500 全盒（半宽高 750）
void PetLootEffectiveCharHalf(const PetLootConfig& cfg, float& outHalfW, float& outHalfH);

bool ReadPetLoot(const char* binDir, PetLootConfig& out);
bool WritePetLoot(const char* binDir, const PetLootConfig& cfg);

}  // namespace xcat

