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
    if (!(cfg.vacuumW > 1.f) || cfg.vacuumW > kPetLootVacuumMax) cfg.vacuumW = kPetLootVacuumWDefault;
    if (!(cfg.vacuumH > 1.f) || cfg.vacuumH > kPetLootVacuumMax) cfg.vacuumH = kPetLootVacuumHDefault;
    if (!(cfg.footHalfW > 1.f) || cfg.footHalfW > kPetLootFootHalfMax)
        cfg.footHalfW = kPetLootFootHalfWDefault;
    if (!(cfg.footHalfH > 1.f) || cfg.footHalfH > kPetLootFootHalfMax)
        cfg.footHalfH = kPetLootFootHalfHDefault;
    if (!(cfg.charHalfW > 1.f) || cfg.charHalfW > kPetLootCharHalfMax)
        cfg.charHalfW = kPetLootCharHalfWDefault;
    if (!(cfg.charHalfH > 1.f) || cfg.charHalfH > kPetLootCharHalfMax)
        cfg.charHalfH = kPetLootCharHalfHDefault;
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
    IniGetFloat(ini, "pet_loot", "footHalfW", out.footHalfW);
    IniGetFloat(ini, "pet_loot", "footHalfH", out.footHalfH);
    if (IniGetBool(ini, "pet_loot", "charVacEnabled", b)) out.charVacEnabled = b ? 1u : 0u;
    IniGetFloat(ini, "pet_loot", "charHalfW", out.charHalfW);
    IniGetFloat(ini, "pet_loot", "charHalfH", out.charHalfH);
    IniGetU32(ini, "pet_loot", "filterFlags", out.filterFlags);
    if (IniGetBool(ini, "pet_loot", "skipFilterEnabled", b)) out.skipFilterEnabled = b ? 1u : 0u;
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
        IniSetFloat(ini, "pet_loot", "footHalfW", cfg.footHalfW);
        IniSetFloat(ini, "pet_loot", "footHalfH", cfg.footHalfH);
        IniSetBool(ini, "pet_loot", "charVacEnabled", cfg.charVacEnabled != 0);
        IniSetFloat(ini, "pet_loot", "charHalfW", cfg.charHalfW);
        IniSetFloat(ini, "pet_loot", "charHalfH", cfg.charHalfH);
        IniSetU32(ini, "pet_loot", "filterFlags", cfg.filterFlags);
        IniSetBool(ini, "pet_loot", "skipFilterEnabled", cfg.skipFilterEnabled != 0);
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
    if (n > kPetLootBurstMax) return kPetLootBurstMax;
    return n;
}

void PetLootEffectiveVacuum(const PetLootConfig& cfg, float& outW, float& outH) {
    // 宠吸唯一尺寸 = 原近图真空；enabled 即用 MapVacuum（兼容旧 mapVacuumEnabled）
    if (cfg.enabled || cfg.mapVacuumEnabled) {
        outW = kPetLootMapVacuumW;
        outH = kPetLootMapVacuumH;
        return;
    }
    outW = cfg.vacuumW;
    outH = cfg.vacuumH;
}

void PetLootEffectiveCharHalf(const PetLootConfig& cfg, float& outHalfW, float& outHalfH) {
    outHalfW = cfg.charHalfW;
    outHalfH = cfg.charHalfH;
}

void PetLootNormalize(PetLootConfig& cfg) {
    cfg.magic = kPetLootMagic;
    cfg.version = kPetLootVersion;
    cfg.enabled = cfg.enabled ? 1u : 0u;
    cfg.footEnabled = cfg.footEnabled ? 1u : 0u;
    cfg.mapVacuumEnabled = cfg.mapVacuumEnabled ? 1u : 0u;
    cfg.charVacEnabled = cfg.charVacEnabled ? 1u : 0u;
    // 三种吸物模式互斥：宠吸 > 人物直吸 > 脚边（宠吸开时脚边空成功会刷 LastTry 锁死宠吸）
    if (cfg.enabled) {
        cfg.charVacEnabled = 0;
        cfg.footEnabled = 0;
    } else if (cfg.charVacEnabled) {
        cfg.footEnabled = 0;
    }
    // 人物直吸全盒钉死 1500×1500（冲掉旧 400×320 / 3200×2400）
    cfg.charHalfW = kPetLootCharHalfWDefault;
    cfg.charHalfH = kPetLootCharHalfHDefault;
    cfg.intervalMs = PetLootClampIntervalMs(cfg.intervalMs);
    cfg.burstPerTick = PetLootClampBurstPerTick(cfg.burstPerTick);
    ClampVacuum(cfg);
    if (cfg.filterFlags == 0) cfg.filterFlags = kPetLootFilterDefault;
    cfg.skipFilterEnabled = cfg.skipFilterEnabled ? 1u : 0u;
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
    out.footHalfW = kPetLootFootHalfWDefault;
    out.footHalfH = kPetLootFootHalfHDefault;
    out.charHalfW = kPetLootCharHalfWDefault;
    out.charHalfH = kPetLootCharHalfHDefault;
    out.filterFlags = kPetLootFilterDefault;
    out.skipFilterEnabled = 1;
}

bool ReadPetLoot(const char* binDir, PetLootConfig& out) {
    PetLootSetDefaults(out);
    uint64_t tick = 0;
    if (!ReadPetLootIni(binDir, out, &tick)) return false;
    out.writeTickMs = tick;
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

