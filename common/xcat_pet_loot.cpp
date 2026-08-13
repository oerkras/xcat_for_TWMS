#include "xcat_pet_loot.h"
#include "process_util.h"
#include "xcat_config_ini.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace xcat {
namespace {
constexpr uint32_t kPetLootIniVersion = 1u;
bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

void ClampVacuum(PetLootConfig& cfg) {
    if (!(cfg.vacuumW >= kPetLootVacuumMin)) cfg.vacuumW = kPetLootVacuumWDefault;
    if (!(cfg.vacuumH >= kPetLootVacuumMin)) cfg.vacuumH = kPetLootVacuumHDefault;
    // 超顶钳到 Max（旧 ini 4000 一次压回安全顶，勿整段打回默认丢用户意图）
    if (cfg.vacuumW > kPetLootVacuumMax) cfg.vacuumW = kPetLootVacuumMax;
    if (cfg.vacuumH > kPetLootVacuumMax) cfg.vacuumH = kPetLootVacuumMax;
    // 人物直吸与宠吸共用 vacuum*；半盒镜像写入以便旧读路径/ini 一致
    cfg.charHalfW = cfg.vacuumW * 0.5f;
    cfg.charHalfH = cfg.vacuumH * 0.5f;
}

// 经典版默认黑名单：箭矢 / 彈丸（名称含匹配，展开图鉴）
void ApplyDefaultSkipRules(PetLootConfig& cfg) {
    static constexpr const char* kDefaults[] = {"箭矢", "彈丸"};
    cfg.skipRuleCount = 0;
    for (const char* key : kDefaults) {
        if (cfg.skipRuleCount >= static_cast<uint32_t>(kPetLootMaxSkipRules)) break;
        PetLootSkipRule& r = cfg.skipRules[cfg.skipRuleCount++];
        r = {};
        r.enabled = 1;
        strncpy_s(r.nameKey, key, _TRUNCATE);
    }
}

bool ReadPetLootIni(const char* binDir, PetLootConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;
    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;
    uint32_t version = 0;
    if (!IniGetU32(ini, "pet_loot", "version", version) || version != kPetLootIniVersion)
        return false;
    PetLootSetDefaults(out);
    bool b = false;
    if (IniGetBool(ini, "pet_loot", "enabled", b)) out.enabled = b ? 1u : 0u;
    if (IniGetBool(ini, "pet_loot", "footEnabled", b)) out.footEnabled = b ? 1u : 0u;
    if (IniGetBool(ini, "pet_loot", "mapVacuumEnabled", b)) out.mapVacuumEnabled = b ? 1u : 0u;
    IniGetU32(ini, "pet_loot", "intervalMs", out.intervalMs);
    IniGetU32(ini, "pet_loot", "burstPerTick", out.burstPerTick);
    IniGetFloat(ini, "pet_loot", "vacuumW", out.vacuumW);
    IniGetFloat(ini, "pet_loot", "vacuumH", out.vacuumH);
    if (IniGetBool(ini, "pet_loot", "charVacEnabled", b)) out.charVacEnabled = b ? 1u : 0u;
    IniGetFloat(ini, "pet_loot", "charHalfW", out.charHalfW);
    IniGetFloat(ini, "pet_loot", "charHalfH", out.charHalfH);
    IniGetU32(ini, "pet_loot", "filterFlags", out.filterFlags);
    if (IniGetBool(ini, "pet_loot", "skipFilterEnabled", b)) out.skipFilterEnabled = b ? 1u : 0u;
    if (IniGetBool(ini, "pet_loot", "highValuePriority", b)) out.highValuePriority = b ? 1u : 0u;
    if (IniGetBool(ini, "pet_loot", "scrollDropNotify", b)) out.scrollDropNotify = b ? 1u : 0u;
    uint32_t skipCount = 0;
    const bool hadSkip = IniGetU32(ini, "pet_loot", "skipCount", skipCount);
    if (hadSkip) {
        out.skipRuleCount = skipCount > static_cast<uint32_t>(kPetLootMaxSkipRules)
                                ? static_cast<uint32_t>(kPetLootMaxSkipRules)
                                : skipCount;
        for (uint32_t i = 0; i < out.skipRuleCount; ++i) {
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "skip.%u.", i + 1);
            PetLootSkipRule& r = out.skipRules[i];
            r = {};
            r.enabled = 1;
            bool en = true;
            if (IniGetBool(ini, "pet_loot", (std::string(prefix) + "enabled").c_str(), en))
                r.enabled = en ? 1u : 0u;
            IniGetU32(ini, "pet_loot", (std::string(prefix) + "itemId").c_str(), r.itemId);
            std::string name;
            if (IniGetString(ini, "pet_loot", (std::string(prefix) + "nameKey").c_str(), name)) {
                strncpy_s(r.nameKey, name.c_str(), _TRUNCATE);
                if (r.itemId == 0 && !name.empty()) {
                    char* end = nullptr;
                    const unsigned long v = strtoul(name.c_str(), &end, 10);
                    if (end && *end == '\0' && v > 0 && v < 0x7FFFFFFFul) r.itemId = (uint32_t)v;
                }
            }
        }
        // skipCount=0：尊重用户清空；缺键时保留 SetDefaults 的「箭矢/彈丸」
    }
    PetLootNormalize(out);
    if (outWriteTick) IniGetU64(ini, "pet_loot", "writeTickMs", *outWriteTick);
    return true;
}

bool WritePetLootIni(const char* binDir, const PetLootConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;
    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "pet_loot", "version", kPetLootIniVersion);
        IniSetU64(ini, "pet_loot", "writeTickMs", writeTickMs);
        IniSetBool(ini, "pet_loot", "enabled", cfg.enabled != 0);
        IniSetBool(ini, "pet_loot", "footEnabled", cfg.footEnabled != 0);
        IniSetBool(ini, "pet_loot", "mapVacuumEnabled", cfg.mapVacuumEnabled != 0);
        IniSetU32(ini, "pet_loot", "intervalMs", cfg.intervalMs);
        IniSetU32(ini, "pet_loot", "burstPerTick", cfg.burstPerTick);
        IniSetFloat(ini, "pet_loot", "vacuumW", cfg.vacuumW);
        IniSetFloat(ini, "pet_loot", "vacuumH", cfg.vacuumH);
        // 脚下已改原生盒：不再读写自定义半宽；擦掉旧键避免误导
        IniEraseKey(ini, "pet_loot", "footHalfW");
        IniEraseKey(ini, "pet_loot", "footHalfH");
        IniSetBool(ini, "pet_loot", "charVacEnabled", cfg.charVacEnabled != 0);
        IniSetFloat(ini, "pet_loot", "charHalfW", cfg.charHalfW);
        IniSetFloat(ini, "pet_loot", "charHalfH", cfg.charHalfH);
        IniSetU32(ini, "pet_loot", "filterFlags", cfg.filterFlags);
        IniSetBool(ini, "pet_loot", "skipFilterEnabled", cfg.skipFilterEnabled != 0);
        IniSetBool(ini, "pet_loot", "highValuePriority", cfg.highValuePriority != 0);
        IniSetBool(ini, "pet_loot", "scrollDropNotify", cfg.scrollDropNotify != 0);
        // 清理已删除的角色全图吸键（charDrop*）
        IniEraseKeysWithPrefix(ini, "pet_loot", "charDrop");
        IniEraseKeysWithPrefix(ini, "pet_loot", "skip.");
        const uint32_t n = (std::min)(cfg.skipRuleCount, static_cast<uint32_t>(kPetLootMaxSkipRules));
        IniSetU32(ini, "pet_loot", "skipCount", n);
        for (uint32_t i = 0; i < n; ++i) {
            const PetLootSkipRule& r = cfg.skipRules[i];
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "skip.%u.", i + 1);
            IniSetBool(ini, "pet_loot", (std::string(prefix) + "enabled").c_str(), r.enabled != 0);
            IniSetU32(ini, "pet_loot", (std::string(prefix) + "itemId").c_str(), r.itemId);
            IniSetString(ini, "pet_loot", (std::string(prefix) + "nameKey").c_str(), r.nameKey);
        }
    });
}

}  // namespace

uint32_t PetLootClampIntervalMs(uint32_t ms) {
    if (ms < kPetLootIntervalMinMs) return kPetLootIntervalMinMs;
    if (ms > kPetLootIntervalMaxMs) return kPetLootIntervalMaxMs;
    return ms;
}

uint32_t PetLootClampBurstPerTick(uint32_t n) {
    if (n < kPetLootBurstMin) return kPetLootBurstMin;
    if (n > kPetLootBurstHardCap) return kPetLootBurstHardCap;
    return n;
}

void PetLootEffectiveVacuum(const PetLootConfig& cfg, float& outW, float& outH) {
    outW = cfg.vacuumW >= kPetLootVacuumMin ? cfg.vacuumW : kPetLootVacuumWDefault;
    outH = cfg.vacuumH >= kPetLootVacuumMin ? cfg.vacuumH : kPetLootVacuumHDefault;
    if (outW > kPetLootVacuumMax) outW = kPetLootVacuumMax;
    if (outH > kPetLootVacuumMax) outH = kPetLootVacuumMax;
}

void PetLootEffectiveCharHalf(const PetLootConfig& cfg, float& outHalfW, float& outHalfH) {
    float w = 0.f, h = 0.f;
    PetLootEffectiveVacuum(cfg, w, h);
    // 人物直吸硬顶：与宠吸可共用 ini 大盒，但枚举/Send 不得超出服端可接受范围
    if (w > kPetLootCharVacWMax) w = kPetLootCharVacWMax;
    if (h > kPetLootCharVacHMax) h = kPetLootCharVacHMax;
    outHalfW = w * 0.5f;
    outHalfH = h * 0.5f;
}

void PetLootNormalize(PetLootConfig& cfg) {
    cfg.magic = kPetLootMagic;
    cfg.version = kPetLootVersion;
    cfg.enabled = cfg.enabled ? 1u : 0u;
    cfg.footEnabled = cfg.footEnabled ? 1u : 0u;
    cfg.mapVacuumEnabled = cfg.mapVacuumEnabled ? 1u : 0u;
    cfg.charVacEnabled = cfg.charVacEnabled ? 1u : 0u;
    // 用户面禁用：配置层掐死，避免旧 ini / 面板残留仍驱动 payload。
    if (!kPetLootCharVacUserEnabled) cfg.charVacEnabled = 0;
    // 三种吸物模式互斥：宠吸 > 人物直吸 > 脚边（脚下只触发原生口，仍与宠吸分时）
    if (cfg.enabled) {
        cfg.charVacEnabled = 0;
        cfg.footEnabled = 0;
    } else if (cfg.charVacEnabled) {
        cfg.footEnabled = 0;
    }
    // 宠吸/人物直吸共用 vacuumW/H；旧「全图」3200×2400 一次性压回默认
    if (cfg.vacuumW >= 3199.f && cfg.vacuumW <= 3201.f && cfg.vacuumH >= 2399.f &&
        cfg.vacuumH <= 2401.f) {
        cfg.vacuumW = kPetLootVacuumWDefault;
        cfg.vacuumH = kPetLootVacuumHDefault;
    }
    cfg.intervalMs = PetLootClampIntervalMs(cfg.intervalMs);
    cfg.burstPerTick = PetLootClampBurstPerTick(cfg.burstPerTick);
    ClampVacuum(cfg);  // 同步 charHalf* = vacuum*/2
    if (cfg.filterFlags == 0) cfg.filterFlags = kPetLootFilterDefault;
    cfg.skipFilterEnabled = cfg.skipFilterEnabled ? 1u : 0u;
    cfg.highValuePriority = cfg.highValuePriority ? 1u : 0u;
    cfg.scrollDropNotify = cfg.scrollDropNotify ? 1u : 0u;
    if (cfg.skipRuleCount > static_cast<uint32_t>(kPetLootMaxSkipRules))
        cfg.skipRuleCount = static_cast<uint32_t>(kPetLootMaxSkipRules);
    for (uint32_t i = 0; i < cfg.skipRuleCount; ++i) {
        PetLootSkipRule& r = cfg.skipRules[i];
        r.enabled = r.enabled ? 1u : 0u;
        r.nameKey[kPetLootNameKeyLen - 1] = '\0';
        if (r.itemId == 0 && r.nameKey[0]) {
            char* end = nullptr;
            const unsigned long v = strtoul(r.nameKey, &end, 10);
            if (end && *end == '\0' && v > 0 && v < 0x7FFFFFFFul) r.itemId = (uint32_t)v;
        }
    }
}

void PetLootSetDefaults(PetLootConfig& out) {
    out = PetLootConfig{};
    out.magic = kPetLootMagic;
    out.version = kPetLootVersion;
    out.intervalMs = kPetLootIntervalDefaultMs;
    out.burstPerTick = kPetLootBurstDefault;
    out.vacuumW = kPetLootVacuumWDefault;
    out.vacuumH = kPetLootVacuumHDefault;
    out.charHalfW = kPetLootCharHalfWDefault;
    out.charHalfH = kPetLootCharHalfHDefault;
    out.filterFlags = kPetLootFilterDefault;
    out.skipFilterEnabled = 1;
    ApplyDefaultSkipRules(out);
}

bool ReadPetLoot(const char* binDir, PetLootConfig& out) {
    PetLootSetDefaults(out);
    uint64_t tick = 0;
    if (!ReadPetLootIni(binDir, out, &tick)) return false;
    out.writeTickMs = tick;
    PetLootNormalize(out);
    return true;
}

bool WritePetLoot(const char* binDir, const PetLootConfig& cfg) {
    PetLootConfig normalized = cfg;
    PetLootNormalize(normalized);
    const uint64_t tick = cfg.writeTickMs ? cfg.writeTickMs : GetTickCount64();
    normalized.writeTickMs = tick;
    return WritePetLootIni(binDir, normalized, tick);
}

}  // namespace xcat

