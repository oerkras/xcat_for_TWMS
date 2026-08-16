#pragma once

#include <cstdint>
#include <string>

namespace xcat {

// 经典版宠物吸物：user.ini [pet_loot]。对照枫星 autopickup.fullMapEnabled 语义。
// 正式吸物只走宠扩盒（Pet.TryPickUpDrop → ByPet → Pet.Send）。角色全图假位姿已移除。
constexpr uint32_t kPetLootMagic = 0x5843504Cu;  // 'XCPL'
constexpr uint32_t kPetLootVersion = 6u;  // v6：+nativeVacEnabled（变态宠吸：常驻扩盒）
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

// 高价值优先：装备栏 / 消耗栏（卷軸）有空位时打断出刀先吸
constexpr uint32_t kPetLootHighValueIntervalMs = 100u;  // 紧急窗调度间隔顶

// 默认调度 200ms × 5（可改；勿长期压到 50 / 过高 burst 拖死 MainPump）
constexpr uint32_t kPetLootIntervalDefaultMs = 200u;
constexpr uint32_t kPetLootCharVacIntervalMs = kPetLootIntervalDefaultMs;  // 语义别名
constexpr uint32_t kPetLootIntervalMinMs = 50u;
constexpr uint32_t kPetLootIntervalMaxMs = 2000u;
// 每 Tick 连调官方吸物次数：默认 5；硬顶压低——BIN 4000 盒 × burst 连打 → 拾取洪泛静默掐线
constexpr uint32_t kPetLootBurstDefault = 5u;
constexpr uint32_t kPetLootBurstMin = 1u;
constexpr uint32_t kPetLootBurstHardCap = 8u;
// 宠吸有效真空：人物/宠附近矩形（宽×高，默认 1000×1000）；用户自调。
// Max 只挡面板拖条 / ini 垃圾，不是「服端能吸」顶。过大空 Send / 掐线自负。
constexpr float kPetLootVacuumWDefault = 1000.f;
constexpr float kPetLootVacuumHDefault = 1000.f;
constexpr float kPetLootVacuumMin = 50.f;
// 兼容旧名：现与 Near 默认同值（勿再当全图尺寸用）
constexpr float kPetLootMapVacuumW = kPetLootVacuumWDefault;
constexpr float kPetLootMapVacuumH = kPetLootVacuumHDefault;
constexpr float kPetLootVacuumMax = 20000.f;
// 人物直吸与宠吸同一盒顶，不再另钳 1500。
constexpr float kPetLootCharVacWMax = kPetLootVacuumMax;
constexpr float kPetLootCharVacHMax = kPetLootVacuumMax;
// 盒内 Ready 超过此数：禁大盒 ByPet（一拍可连发 N 个 Pet.Send），改按件 Pool.Send
constexpr int kPetLootByPetReadyMax = 6;
// 人物直吸半宽/半高：与 vacuumW/H 共用全盒语义（半盒 = 全盒/2）
constexpr float kPetLootCharHalfWDefault = kPetLootVacuumWDefault * 0.5f;
constexpr float kPetLootCharHalfHDefault = kPetLootVacuumHDefault * 0.5f;
constexpr float kPetLootCharHalfMax = kPetLootCharVacWMax * 0.5f;
// 人物直吸用户面开关：false=保留实现但禁止启用（ini 强制关、ImGui 置灰）。改 true 可上架。
constexpr bool kPetLootCharVacUserEnabled = true;
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
    uint32_t enabled = 0;           // 宠吸（人物附近真空，尺寸见 vacuumW/H）
    uint32_t footEnabled = 0;       // 角色脚边（与宠吸互斥；默认关）
    uint32_t nativeVacEnabled = 0;  // 变态宠吸：常驻扩 ByPet .rdata；与其它吸物互斥
    uint32_t mapVacuumEnabled = 0;  // 与 enabled 同开；保留键兼容旧 ini（不再表示全图）
    uint32_t intervalMs = kPetLootIntervalDefaultMs;
    uint32_t burstPerTick = kPetLootBurstDefault;  // 每拍连吸次数（自设；硬顶见 HardCap）
    float vacuumW = kPetLootVacuumWDefault;  // 宠吸/人物直吸共用全盒宽（可调；默认 1000）
    float vacuumH = kPetLootVacuumHDefault;  // 宠吸/人物直吸共用全盒高（可调；默认 1000）
    uint32_t charVacEnabled = 0;  // 人物直吸（与宠吸/脚边互斥）
    float charHalfW = kPetLootCharHalfWDefault;  // = vacuumW/2（Normalize 同步）
    float charHalfH = kPetLootCharHalfHDefault;
    uint32_t filterFlags = kPetLootFilterDefault;
    uint32_t skipFilterEnabled = 1;
    uint32_t highValuePriority = 1;  // 装备/卷軸优先；对应栏满则跳过该件
    uint32_t scrollDropNotify = 1;   // 新卷軸落地 → notify + 明亮叮咚（装备不提醒）
    uint32_t dropSnapLand = 0;       // 飞行中 Pt1=PickPt、EndPara=3、LastTry=0（默认关）
    uint32_t dropAccelFall = 0;      // 加大 LastTry / +0x90，抛物仍在只是更快（默认关）
    uint32_t skipRuleCount = 0;
    PetLootSkipRule skipRules[kPetLootMaxSkipRules]{};
    uint64_t writeTickMs = 0;
};

void PetLootSetDefaults(PetLootConfig& out);
void PetLootNormalize(PetLootConfig& cfg);
uint32_t PetLootClampIntervalMs(uint32_t ms);
uint32_t PetLootClampBurstPerTick(uint32_t n);
void PetLootEffectiveVacuum(const PetLootConfig& cfg, float& outW, float& outH);
// 人物直吸半盒：与 vacuumW/H 共用（半宽高 = 全盒/2）
void PetLootEffectiveCharHalf(const PetLootConfig& cfg, float& outHalfW, float& outHalfH);

bool ReadPetLoot(const char* binDir, PetLootConfig& out);
bool WritePetLoot(const char* binDir, const PetLootConfig& cfg);

}  // namespace xcat

