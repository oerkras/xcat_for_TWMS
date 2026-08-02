#include "xcat_auto_supply.h"

#include "xcat_config_ini.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace xcat {
namespace {

constexpr uint32_t kAutoSupplyIniVersion = 3u;
constexpr uint32_t kAutoSupplyIniVersionMin = 1u;
constexpr uint32_t kAutoSupplyStatusIniVersion = 1u;

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%sstate", binDir);
    CreateDirectoryA(dir, nullptr);
    return true;
}

const char* SellModeToIni(uint32_t mode) {
    return mode == kAutoSupplySellDenylist ? "denylist" : "allowlist";
}

uint32_t SellModeFromIni(const char* s) {
    if (s && _stricmp(s, "denylist") == 0) return kAutoSupplySellDenylist;
    return kAutoSupplySellAllowlist;
}

void NormalizeConfig(AutoSupplyConfig& cfg) {
    cfg.magic = kAutoSupplyMagic;
    cfg.version = kAutoSupplyVersion;
    cfg.enabled = cfg.enabled ? 1u : 0u;
    // 产品硬约束：不给用户关
    cfg.pauseCombat = 1u;
    cfg.returnAfterSupply = 1u;
    cfg.openShopMode = kAutoSupplyOpenInteractNpc;
    cfg.autoBuyEnabled = 0u;  // 旧 buyRules 自动买药已废弃
    cfg.autoSellOnBagFullEnabled = cfg.autoSellOnBagFullEnabled ? 1u : 0u;
    if (cfg.sellFreeSlotsAtOrBelow < 0) cfg.sellFreeSlotsAtOrBelow = 0;
    if (cfg.sellFreeSlotsAtOrBelow > 300) cfg.sellFreeSlotsAtOrBelow = 300;
    if (cfg.buyRuleCount > static_cast<uint32_t>(kAutoSupplyMaxBuyRules))
        cfg.buyRuleCount = static_cast<uint32_t>(kAutoSupplyMaxBuyRules);
    if (cfg.sellRuleCount > static_cast<uint32_t>(kAutoSupplyMaxSellRules))
        cfg.sellRuleCount = static_cast<uint32_t>(kAutoSupplyMaxSellRules);
    cfg.refillHpEnabled = cfg.refillHpEnabled ? 1u : 0u;
    cfg.refillMpEnabled = cfg.refillMpEnabled ? 1u : 0u;
    cfg.refillCustomEnabled = cfg.refillCustomEnabled ? 1u : 0u;
    cfg.refillFeedEnabled = cfg.refillFeedEnabled ? 1u : 0u;
    cfg.rechargeStarsEnabled = cfg.rechargeStarsEnabled ? 1u : 0u;
    if (cfg.refillHpBuyTo < 0) cfg.refillHpBuyTo = 0;
    if (cfg.refillHpBuyTo > 9999) cfg.refillHpBuyTo = 9999;
    if (cfg.refillMpBuyTo < 0) cfg.refillMpBuyTo = 0;
    if (cfg.refillMpBuyTo > 9999) cfg.refillMpBuyTo = 9999;
    if (cfg.refillCustomBuyTo < 0) cfg.refillCustomBuyTo = 0;
    if (cfg.refillCustomBuyTo > 9999) cfg.refillCustomBuyTo = 9999;
    if (cfg.refillFeedBuyTo < 0) cfg.refillFeedBuyTo = 0;
    if (cfg.refillFeedBuyTo > 9999) cfg.refillFeedBuyTo = 9999;
    // 空名回填默认展示名；内置名强制经典 CODE（避免 catalog 误匹配）。
    if (!cfg.refillHpName[0]) {
        strncpy_s(cfg.refillHpName, kAutoSupplyDefaultRefillHpName, _TRUNCATE);
    }
    if (!cfg.refillMpName[0]) {
        strncpy_s(cfg.refillMpName, kAutoSupplyDefaultRefillMpName, _TRUNCATE);
    }
    // 旧默认「黑輪(碟子)」→「藍色藥水」（仅旧默认名+旧 CODE 对；手填黑輪仍可经 builtin 解析）。
    if (strcmp(cfg.refillMpName, "黑輪(碟子)") == 0 &&
        (!cfg.refillMpCode[0] || strcmp(cfg.refillMpCode, "2022022") == 0)) {
        strncpy_s(cfg.refillMpName, kAutoSupplyDefaultRefillMpName, _TRUNCATE);
        strncpy_s(cfg.refillMpCode, kAutoSupplyDefaultRefillMpCode, _TRUNCATE);
    }
    if (!cfg.refillCustomName[0]) {
        // 仅回填展示名；不抬 buyTo，避免「空名/买到 0」的旧惰性配置被悄悄激活。
        strncpy_s(cfg.refillCustomName, kAutoSupplyDefaultRefillCustomName, _TRUNCATE);
    }
    // 饲料名/CODE 执行侧固定，读写一律收敛到主选，避免旧 ini 改名造成 UI/行为漂移。
    strncpy_s(cfg.refillFeedName, kAutoSupplyDefaultRefillFeedName, _TRUNCATE);
    strncpy_s(cfg.refillFeedCode, kAutoSupplyDefaultRefillFeedCode, _TRUNCATE);
    if (const char* hpCode = AutoSupplyBuiltinRefillCodeForName(cfg.refillHpName)) {
        strncpy_s(cfg.refillHpCode, hpCode, _TRUNCATE);
    }
    if (const char* mpCode = AutoSupplyBuiltinRefillCodeForName(cfg.refillMpName)) {
        strncpy_s(cfg.refillMpCode, mpCode, _TRUNCATE);
    }
    if (const char* customCode = AutoSupplyBuiltinRefillCodeForName(cfg.refillCustomName)) {
        strncpy_s(cfg.refillCustomCode, customCode, _TRUNCATE);
    }
}

void NormalizeStatus(AutoSupplyStatus& status) {
    status.magic = kAutoSupplyMagic;
    status.version = kAutoSupplyVersion;
}

bool ReadAutoSupplyBinLegacy(const char* binDir, AutoSupplyConfig& out) {
    AutoSupplySetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    const std::string path = AutoSupplyPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    AutoSupplyConfig disk{};
    const size_t n = fread(&disk, 1, sizeof(AutoSupplyConfig), f);
    fclose(f);
    if (n != sizeof(AutoSupplyConfig)) return false;
    if (disk.magic != kAutoSupplyMagic || disk.version != kAutoSupplyVersion) return false;

    out = disk;
    NormalizeConfig(out);
    return true;
}

bool ReadAutoSupplyIni(const char* binDir, AutoSupplyConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    uint32_t version = 0;
    if (!IniGetU32(ini, "auto_supply", "version", version) || version < kAutoSupplyIniVersionMin ||
        version > kAutoSupplyIniVersion)
        return false;

    AutoSupplySetDefaults(out);
    bool enabled = false;
    if (IniGetBool(ini, "auto_supply", "enabled", enabled)) out.enabled = enabled ? 1u : 0u;

    uint32_t u32 = 0;
    if (IniGetU32(ini, "auto_supply", "manualSeq", u32)) out.manualSeq = u32;
    if (IniGetU32(ini, "auto_supply", "manualKind", u32)) out.manualKind = u32;

    std::string shopNpcCode;
    if (IniGetString(ini, "auto_supply", "shopNpcCode", shopNpcCode))
        strncpy_s(out.shopNpcCode, shopNpcCode.c_str(), _TRUNCATE);

    std::string shopMapName;
    if (IniGetString(ini, "auto_supply", "shopMapName", shopMapName))
        strncpy_s(out.shopMapName, shopMapName.c_str(), _TRUNCATE);

    std::string returnMapName;
    if (IniGetString(ini, "auto_supply", "returnMapName", returnMapName))
        strncpy_s(out.returnMapName, returnMapName.c_str(), _TRUNCATE);

    bool pauseCombat = out.pauseCombat != 0;
    if (IniGetBool(ini, "auto_supply", "pauseCombat", pauseCombat))
        out.pauseCombat = pauseCombat ? 1u : 0u;

    bool returnAfterSupply = out.returnAfterSupply != 0;
    if (IniGetBool(ini, "auto_supply", "returnAfterSupply", returnAfterSupply))
        out.returnAfterSupply = returnAfterSupply ? 1u : 0u;

    if (IniGetU32(ini, "auto_supply", "openShopMode", u32)) out.openShopMode = u32;

    bool autoBuyEnabled = out.autoBuyEnabled != 0;
    if (IniGetBool(ini, "auto_supply", "autoBuyEnabled", autoBuyEnabled))
        out.autoBuyEnabled = autoBuyEnabled ? 1u : 0u;

    bool autoSellOnBagFullEnabled = out.autoSellOnBagFullEnabled != 0;
    if (IniGetBool(ini, "auto_supply", "autoSellOnBagFullEnabled", autoSellOnBagFullEnabled))
        out.autoSellOnBagFullEnabled = autoSellOnBagFullEnabled ? 1u : 0u;

    // 优先新键；旧键同字段兼容（语义已改为装备件数阈值）
    if (IniGetU32(ini, "auto_supply", "sellEquipTriggerCount", u32) ||
        IniGetU32(ini, "auto_supply", "sellFreeSlotsAtOrBelow", u32))
        out.sellFreeSlotsAtOrBelow = static_cast<int32_t>(u32);

    bool refillHp = false;
    if (IniGetBool(ini, "auto_supply", "refillHpEnabled", refillHp))
        out.refillHpEnabled = refillHp ? 1u : 0u;
    bool refillMp = false;
    if (IniGetBool(ini, "auto_supply", "refillMpEnabled", refillMp))
        out.refillMpEnabled = refillMp ? 1u : 0u;
    bool refillCustom = false;
    const bool hadCustomEnabledKey =
        IniGetBool(ini, "auto_supply", "refillCustomEnabled", refillCustom);
    if (hadCustomEnabledKey) out.refillCustomEnabled = refillCustom ? 1u : 0u;
    bool refillFeed = false;
    if (IniGetBool(ini, "auto_supply", "refillFeedEnabled", refillFeed))
        out.refillFeedEnabled = refillFeed ? 1u : 0u;
    std::string refillHpName;
    if (IniGetString(ini, "auto_supply", "refillHpName", refillHpName))
        strncpy_s(out.refillHpName, refillHpName.c_str(), _TRUNCATE);
    std::string refillMpName;
    if (IniGetString(ini, "auto_supply", "refillMpName", refillMpName))
        strncpy_s(out.refillMpName, refillMpName.c_str(), _TRUNCATE);
    std::string refillCustomName;
    const bool hadCustomNameKey =
        IniGetString(ini, "auto_supply", "refillCustomName", refillCustomName);
    if (hadCustomNameKey) strncpy_s(out.refillCustomName, refillCustomName.c_str(), _TRUNCATE);
    std::string refillFeedName;
    if (IniGetString(ini, "auto_supply", "refillFeedName", refillFeedName))
        strncpy_s(out.refillFeedName, refillFeedName.c_str(), _TRUNCATE);
    std::string refillHpCode;
    if (IniGetString(ini, "auto_supply", "refillHpCode", refillHpCode))
        strncpy_s(out.refillHpCode, refillHpCode.c_str(), _TRUNCATE);
    std::string refillMpCode;
    if (IniGetString(ini, "auto_supply", "refillMpCode", refillMpCode))
        strncpy_s(out.refillMpCode, refillMpCode.c_str(), _TRUNCATE);
    std::string refillCustomCode;
    const bool hadCustomCodeKey =
        IniGetString(ini, "auto_supply", "refillCustomCode", refillCustomCode);
    if (hadCustomCodeKey) strncpy_s(out.refillCustomCode, refillCustomCode.c_str(), _TRUNCATE);
    std::string refillFeedCode;
    if (IniGetString(ini, "auto_supply", "refillFeedCode", refillFeedCode))
        strncpy_s(out.refillFeedCode, refillFeedCode.c_str(), _TRUNCATE);
    if (IniGetU32(ini, "auto_supply", "refillHpBuyTo", u32))
        out.refillHpBuyTo = static_cast<int32_t>(u32);
    if (IniGetU32(ini, "auto_supply", "refillMpBuyTo", u32))
        out.refillMpBuyTo = static_cast<int32_t>(u32);
    const bool hadCustomBuyToKey = IniGetU32(ini, "auto_supply", "refillCustomBuyTo", u32);
    if (hadCustomBuyToKey) out.refillCustomBuyTo = static_cast<int32_t>(u32);
    // 旧盘已有 custom 字段但缺 buyTo：旧默认是 0。勿被 SetDefaults(100) 抬高导致突然开买。
    // 全新无任何 custom 键：保留 SetDefaults 的 100 + 回家卷軸展示（开关仍默认关）。
    if (!hadCustomBuyToKey &&
        (hadCustomEnabledKey || hadCustomNameKey || hadCustomCodeKey)) {
        out.refillCustomBuyTo = 0;
    }
    if (IniGetU32(ini, "auto_supply", "refillFeedBuyTo", u32))
        out.refillFeedBuyTo = static_cast<int32_t>(u32);
    bool rechargeStars = false;
    if (IniGetBool(ini, "auto_supply", "rechargeStarsEnabled", rechargeStars))
        out.rechargeStarsEnabled = rechargeStars ? 1u : 0u;

    if (IniGetU32(ini, "auto_supply", "buyRuleCount", u32)) {
        out.buyRuleCount = u32 > static_cast<uint32_t>(kAutoSupplyMaxBuyRules)
                               ? static_cast<uint32_t>(kAutoSupplyMaxBuyRules)
                               : u32;
    }
    for (uint32_t i = 0; i < out.buyRuleCount; ++i) {
        char prefix[32]{};
        snprintf(prefix, sizeof(prefix), "buy.%u.", i + 1);
        AutoSupplyBuyRule& rule = out.buyRules[i];

        bool ruleEnabled = false;
        if (IniGetBool(ini, "auto_supply", (std::string(prefix) + "enabled").c_str(), ruleEnabled))
            rule.enabled = ruleEnabled ? 1u : 0u;

        std::string itemCode;
        if (IniGetString(ini, "auto_supply", (std::string(prefix) + "itemCode").c_str(), itemCode))
            strncpy_s(rule.itemCode, itemCode.c_str(), _TRUNCATE);

        int32_t triggerBelow = rule.triggerBelow;
        if (IniGetU32(ini, "auto_supply", (std::string(prefix) + "triggerBelow").c_str(), u32))
            triggerBelow = static_cast<int32_t>(u32);
        rule.triggerBelow = triggerBelow;

        int32_t buyTo = rule.buyTo;
        if (IniGetU32(ini, "auto_supply", (std::string(prefix) + "buyTo").c_str(), u32))
            buyTo = static_cast<int32_t>(u32);
        rule.buyTo = buyTo;
    }

    if (IniGetU32(ini, "auto_supply", "sellRuleCount", u32)) {
        out.sellRuleCount = u32 > static_cast<uint32_t>(kAutoSupplyMaxSellRules)
                                ? static_cast<uint32_t>(kAutoSupplyMaxSellRules)
                                : u32;
    }
    for (uint32_t i = 0; i < out.sellRuleCount; ++i) {
        char prefix[32]{};
        snprintf(prefix, sizeof(prefix), "sell.%u.", i + 1);
        AutoSupplySellRule& rule = out.sellRules[i];

        bool ruleEnabled = false;
        if (IniGetBool(ini, "auto_supply", (std::string(prefix) + "enabled").c_str(), ruleEnabled))
            rule.enabled = ruleEnabled ? 1u : 0u;

        if (IniGetU32(ini, "auto_supply", (std::string(prefix) + "bagMask").c_str(), u32))
            rule.bagMask = u32;

        std::string mode;
        if (IniGetString(ini, "auto_supply", (std::string(prefix) + "mode").c_str(), mode))
            rule.mode = SellModeFromIni(mode.c_str());

        std::string itemCode;
        if (IniGetString(ini, "auto_supply", (std::string(prefix) + "itemCode").c_str(), itemCode))
            strncpy_s(rule.itemCode, itemCode.c_str(), _TRUNCATE);

        std::string nameKey;
        if (IniGetString(ini, "auto_supply", (std::string(prefix) + "nameKey").c_str(), nameKey))
            strncpy_s(rule.nameKey, nameKey.c_str(), _TRUNCATE);
    }

    if (outWriteTick) IniGetU64(ini, "auto_supply", "writeTickMs", *outWriteTick);
    NormalizeConfig(out);
    return true;
}

bool WriteAutoSupplyIni(const char* binDir, const AutoSupplyConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;

    AutoSupplyConfig normalized = cfg;
    NormalizeConfig(normalized);

    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "auto_supply", "version", kAutoSupplyIniVersion);
        IniSetU64(ini, "auto_supply", "writeTickMs", writeTickMs);
        IniSetBool(ini, "auto_supply", "enabled", normalized.enabled != 0);
        IniSetU32(ini, "auto_supply", "manualSeq", normalized.manualSeq);
        IniSetU32(ini, "auto_supply", "manualKind", normalized.manualKind);
        IniSetString(ini, "auto_supply", "shopNpcCode", normalized.shopNpcCode);
        IniSetString(ini, "auto_supply", "shopMapName", normalized.shopMapName);
        IniSetString(ini, "auto_supply", "returnMapName", normalized.returnMapName);
        IniSetBool(ini, "auto_supply", "pauseCombat", normalized.pauseCombat != 0);
        IniSetBool(ini, "auto_supply", "returnAfterSupply", normalized.returnAfterSupply != 0);
        IniSetU32(ini, "auto_supply", "openShopMode", normalized.openShopMode);
        IniSetBool(ini, "auto_supply", "autoBuyEnabled", normalized.autoBuyEnabled != 0);
        IniSetBool(ini, "auto_supply", "autoSellOnBagFullEnabled", normalized.autoSellOnBagFullEnabled != 0);
        IniSetU32(ini, "auto_supply", "sellEquipTriggerCount",
                  static_cast<uint32_t>(normalized.sellFreeSlotsAtOrBelow));
        IniSetU32(ini, "auto_supply", "sellFreeSlotsAtOrBelow",
                  static_cast<uint32_t>(normalized.sellFreeSlotsAtOrBelow));
        IniSetBool(ini, "auto_supply", "refillHpEnabled", normalized.refillHpEnabled != 0);
        IniSetBool(ini, "auto_supply", "refillMpEnabled", normalized.refillMpEnabled != 0);
        IniSetBool(ini, "auto_supply", "refillCustomEnabled", normalized.refillCustomEnabled != 0);
        IniSetBool(ini, "auto_supply", "refillFeedEnabled", normalized.refillFeedEnabled != 0);
        IniSetString(ini, "auto_supply", "refillHpName", normalized.refillHpName);
        IniSetString(ini, "auto_supply", "refillMpName", normalized.refillMpName);
        IniSetString(ini, "auto_supply", "refillCustomName", normalized.refillCustomName);
        IniSetString(ini, "auto_supply", "refillFeedName", normalized.refillFeedName);
        IniSetString(ini, "auto_supply", "refillHpCode", normalized.refillHpCode);
        IniSetString(ini, "auto_supply", "refillMpCode", normalized.refillMpCode);
        IniSetString(ini, "auto_supply", "refillCustomCode", normalized.refillCustomCode);
        IniSetString(ini, "auto_supply", "refillFeedCode", normalized.refillFeedCode);
        IniSetU32(ini, "auto_supply", "refillHpBuyTo",
                  static_cast<uint32_t>(normalized.refillHpBuyTo));
        IniSetU32(ini, "auto_supply", "refillMpBuyTo",
                  static_cast<uint32_t>(normalized.refillMpBuyTo));
        IniSetU32(ini, "auto_supply", "refillCustomBuyTo",
                  static_cast<uint32_t>(normalized.refillCustomBuyTo));
        IniSetU32(ini, "auto_supply", "refillFeedBuyTo",
                  static_cast<uint32_t>(normalized.refillFeedBuyTo));
        IniSetBool(ini, "auto_supply", "rechargeStarsEnabled",
                   normalized.rechargeStarsEnabled != 0);

        const uint32_t buyCount = normalized.buyRuleCount;
        // 先清 buy.N.* / sell.N.*，再按当前 count 重写，避免收缩后残留孤儿 key。
        // 前缀带点：不会误删 buyRuleCount / sellEquipTriggerCount 等标量。
        IniEraseKeysWithPrefix(ini, "auto_supply", "buy.");
        IniEraseKeysWithPrefix(ini, "auto_supply", "sell.");
        IniSetU32(ini, "auto_supply", "buyRuleCount", buyCount);
        for (uint32_t i = 0; i < buyCount; ++i) {
            const AutoSupplyBuyRule& rule = normalized.buyRules[i];
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "buy.%u.", i + 1);
            IniSetBool(ini, "auto_supply", (std::string(prefix) + "enabled").c_str(), rule.enabled != 0);
            IniSetString(ini, "auto_supply", (std::string(prefix) + "itemCode").c_str(), rule.itemCode);
            IniSetU32(ini, "auto_supply", (std::string(prefix) + "triggerBelow").c_str(),
                      static_cast<uint32_t>(rule.triggerBelow));
            IniSetU32(ini, "auto_supply", (std::string(prefix) + "buyTo").c_str(),
                      static_cast<uint32_t>(rule.buyTo));
        }

        const uint32_t sellCount = normalized.sellRuleCount;
        IniSetU32(ini, "auto_supply", "sellRuleCount", sellCount);
        for (uint32_t i = 0; i < sellCount; ++i) {
            const AutoSupplySellRule& rule = normalized.sellRules[i];
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "sell.%u.", i + 1);
            IniSetBool(ini, "auto_supply", (std::string(prefix) + "enabled").c_str(), rule.enabled != 0);
            IniSetU32(ini, "auto_supply", (std::string(prefix) + "bagMask").c_str(), rule.bagMask);
            IniSetString(ini, "auto_supply", (std::string(prefix) + "mode").c_str(), SellModeToIni(rule.mode));
            IniSetString(ini, "auto_supply", (std::string(prefix) + "itemCode").c_str(), rule.itemCode);
            IniSetString(ini, "auto_supply", (std::string(prefix) + "nameKey").c_str(), rule.nameKey);
        }

    });
}

bool ReadAutoSupplyStatusBinLegacy(const char* binDir, AutoSupplyStatus& out) {
    AutoSupplyStatusSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    const std::string path = AutoSupplyStatusPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    AutoSupplyStatus disk{};
    const size_t n = fread(&disk, 1, sizeof(AutoSupplyStatus), f);
    fclose(f);
    if (n != sizeof(AutoSupplyStatus)) return false;
    if (disk.magic != kAutoSupplyMagic || disk.version != kAutoSupplyVersion) return false;

    out = disk;
    NormalizeStatus(out);
    return true;
}

bool ReadAutoSupplyStatusIni(const char* binDir, AutoSupplyStatus& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    uint32_t version = 0;
    if (!IniGetU32(ini, "auto_supply_status", "version", version) ||
        version != kAutoSupplyStatusIniVersion)
        return false;

    AutoSupplyStatusSetDefaults(out);
    uint32_t u32 = 0;
    if (IniGetU32(ini, "auto_supply_status", "state", u32)) out.state = u32;
    if (IniGetU32(ini, "auto_supply_status", "lastManualSeq", u32)) out.lastManualSeq = u32;
    if (IniGetU32(ini, "auto_supply_status", "lastResult", u32)) out.lastResult = u32;

    std::string shopNpcCode;
    if (IniGetString(ini, "auto_supply_status", "shopNpcCode", shopNpcCode))
        strncpy_s(out.shopNpcCode, shopNpcCode.c_str(), _TRUNCATE);

    std::string message;
    if (IniGetString(ini, "auto_supply_status", "message", message))
        strncpy_s(out.message, message.c_str(), _TRUNCATE);

    std::string lastFarm;
    if (IniGetString(ini, "auto_supply_status", "lastFarmMapName", lastFarm))
        strncpy_s(out.lastFarmMapName, lastFarm.c_str(), _TRUNCATE);

    if (IniGetU32(ini, "auto_supply_status", "pendingReturnFarm", u32))
        out.pendingReturnFarm = u32 ? 1u : 0u;

    if (outWriteTick) IniGetU64(ini, "auto_supply_status", "writeTickMs", *outWriteTick);
    NormalizeStatus(out);
    return true;
}

bool WriteAutoSupplyStatusIni(const char* binDir, const AutoSupplyStatus& status, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;

    AutoSupplyStatus normalized = status;
    NormalizeStatus(normalized);

    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "auto_supply_status", "version", kAutoSupplyStatusIniVersion);
        IniSetU64(ini, "auto_supply_status", "writeTickMs", writeTickMs);
        IniSetU32(ini, "auto_supply_status", "state", normalized.state);
        IniSetU32(ini, "auto_supply_status", "lastManualSeq", normalized.lastManualSeq);
        IniSetU32(ini, "auto_supply_status", "lastResult", normalized.lastResult);
        IniSetString(ini, "auto_supply_status", "shopNpcCode", normalized.shopNpcCode);
        IniSetString(ini, "auto_supply_status", "message", normalized.message);
        IniSetString(ini, "auto_supply_status", "lastFarmMapName", normalized.lastFarmMapName);
        IniSetU32(ini, "auto_supply_status", "pendingReturnFarm",
                  normalized.pendingReturnFarm != 0 ? 1u : 0u);

    });
}

}  // namespace

const char* AutoSupplyBuiltinRefillCodeForName(const char* nameZh) {
    if (!nameZh || !nameZh[0]) return nullptr;
    // 仅默认红/蓝/自定义/饲料：同名冲突时强制经典 CODE；其余走离线 catalog 精确中文名。
    if (strcmp(nameZh, kAutoSupplyDefaultRefillHpName) == 0) return kAutoSupplyDefaultRefillHpCode;
    if (strcmp(nameZh, kAutoSupplyDefaultRefillMpName) == 0) return kAutoSupplyDefaultRefillMpCode;
    // 旧默认补蓝名（神社料理）：仍可解析，避免手填/旧文案失配
    if (strcmp(nameZh, "黑輪(碟子)") == 0) return "2022022";
    if (strcmp(nameZh, kAutoSupplyDefaultRefillCustomName) == 0)
        return kAutoSupplyDefaultRefillCustomCode;
    // 简体别名：与 catalog「回家卷軸」同 CODE
    if (strcmp(nameZh, "回家卷轴") == 0) return kAutoSupplyDefaultRefillCustomCode;
    if (strcmp(nameZh, kAutoSupplyDefaultRefillFeedName) == 0) return kAutoSupplyDefaultRefillFeedCode;
    if (strcmp(nameZh, kAutoSupplyDefaultRefillFeedAltName) == 0)
        return kAutoSupplyDefaultRefillFeedAltCode;
    return nullptr;
}

void AutoSupplySetDefaults(AutoSupplyConfig& out) {
    out = {};
    out.magic = kAutoSupplyMagic;
    out.version = kAutoSupplyVersion;
    out.enabled = 0;
    out.manualKind = kAutoSupplyManualNone;
    out.pauseCombat = 1;
    out.returnAfterSupply = 1;
    out.openShopMode = kAutoSupplyOpenInteractNpc;
    out.autoBuyEnabled = 0;                 // 自动买药已废弃
    out.autoSellOnBagFullEnabled = 0;       // 装备达阈值卖装，默认关
    out.sellFreeSlotsAtOrBelow = 0;
    out.buyRuleCount = 0;
    out.refillHpEnabled = 0;
    out.refillMpEnabled = 0;
    out.refillCustomEnabled = 0;
    out.refillFeedEnabled = 1;
    out.refillFeedBuyTo = 100;
    out.refillMpBuyTo = kAutoSupplyDefaultRefillMpBuyTo;
    out.refillCustomBuyTo = kAutoSupplyDefaultRefillCustomBuyTo;
    out.rechargeStarsEnabled = 0;
    strncpy_s(out.refillHpName, kAutoSupplyDefaultRefillHpName, _TRUNCATE);
    strncpy_s(out.refillMpName, kAutoSupplyDefaultRefillMpName, _TRUNCATE);
    strncpy_s(out.refillCustomName, kAutoSupplyDefaultRefillCustomName, _TRUNCATE);
    strncpy_s(out.refillFeedName, kAutoSupplyDefaultRefillFeedName, _TRUNCATE);
    strncpy_s(out.refillHpCode, kAutoSupplyDefaultRefillHpCode, _TRUNCATE);
    strncpy_s(out.refillMpCode, kAutoSupplyDefaultRefillMpCode, _TRUNCATE);
    strncpy_s(out.refillCustomCode, kAutoSupplyDefaultRefillCustomCode, _TRUNCATE);
    strncpy_s(out.refillFeedCode, kAutoSupplyDefaultRefillFeedCode, _TRUNCATE);
}

void AutoSupplyStatusSetDefaults(AutoSupplyStatus& out) {
    out = {};
    out.magic = kAutoSupplyMagic;
    out.version = kAutoSupplyVersion;
    out.state = kAutoSupplyStateIdle;
}

std::string AutoSupplyRelPath() { return "state\\auto_supply.bin"; }
std::string AutoSupplyStatusRelPath() { return "state\\auto_supply_status.bin"; }

std::string AutoSupplyPath(const char* binDir) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%s%s", binDir ? binDir : "", AutoSupplyRelPath().c_str());
    return path;
}

std::string AutoSupplyStatusPath(const char* binDir) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%s%s", binDir ? binDir : "", AutoSupplyStatusRelPath().c_str());
    return path;
}

bool ClearLegacyCoreAutoSellKeys(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [](IniStore& ini) {
        IniEraseKeysWithPrefix(ini, "core", "autoSell");
    });
}

bool ReadAutoSupply(const char* binDir, AutoSupplyConfig& out) {
    AutoSupplySetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    uint64_t iniTick = 0;
    AutoSupplyConfig iniCfg{};
    const bool iniOk = ReadAutoSupplyIni(binDir, iniCfg, &iniTick);

    AutoSupplyConfig bin{};
    const bool binOk = ReadAutoSupplyBinLegacy(binDir, bin);

    if (iniOk && binOk) {
        if (iniTick >= bin.writeTickMs) {
            out = iniCfg;
            out.writeTickMs = iniTick;
        } else {
            out = bin;
            WriteAutoSupplyIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
        }
    } else if (iniOk) {
        out = iniCfg;
        out.writeTickMs = iniTick;
    } else if (binOk) {
        out = bin;
        WriteAutoSupplyIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
    } else {
        // 经典版迁移：旧 core.autoSell / autoSellShopMap → [auto_supply]
        IniStore coreIni{};
        const std::string path = UserConfigIniPath(binDir);
        if (!LoadIniFile(path.c_str(), coreIni)) return false;
        bool legacyOn = false;
        std::string shopMap;
        const bool hadSell = IniGetBool(coreIni, "core", "autoSell", legacyOn);
        const bool hadMap = IniGetString(coreIni, "core", "autoSellShopMap", shopMap);
        if (!hadSell && !hadMap) return false;
        AutoSupplySetDefaults(out);
        out.autoSellOnBagFullEnabled = legacyOn ? 1u : 0u;
        out.enabled = out.autoSellOnBagFullEnabled;
        if (hadMap && !shopMap.empty())
            strncpy_s(out.shopMapName, shopMap.c_str(), _TRUNCATE);
        out.writeTickMs = GetTickCount64();
        WriteAutoSupply(binDir, out);
        ClearLegacyCoreAutoSellKeys(binDir);
        return true;
    }
    return true;
}

bool WriteAutoSupply(const char* binDir, const AutoSupplyConfig& cfg) {
    if (!binDir || !binDir[0]) return false;
    if (cfg.magic != kAutoSupplyMagic || cfg.version != kAutoSupplyVersion) return false;

    AutoSupplyConfig normalized = cfg;
    // 同毫秒连续写会让 payload 因 writeTickMs 未变而忽略 manualSeq；强制单调递增。
    static uint64_t s_lastTick = 0;
    uint64_t tick = normalized.writeTickMs ? normalized.writeTickMs : GetTickCount64();
    if (tick <= s_lastTick) tick = s_lastTick + 1;
    s_lastTick = tick;
    normalized.writeTickMs = tick;

    for (int attempt = 0; attempt < 6; ++attempt) {
        if (WriteAutoSupplyIni(binDir, normalized, tick)) {
            ClearLegacyCoreAutoSellKeys(binDir);
            return true;
        }
        Sleep(15 + attempt * 10);
    }
    return false;
}

bool ReadAutoSupplyStatus(const char* binDir, AutoSupplyStatus& out) {
    AutoSupplyStatusSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    uint64_t iniTick = 0;
    AutoSupplyStatus iniStatus{};
    const bool iniOk = ReadAutoSupplyStatusIni(binDir, iniStatus, &iniTick);

    AutoSupplyStatus bin{};
    const bool binOk = ReadAutoSupplyStatusBinLegacy(binDir, bin);

    if (iniOk && binOk) {
        if (iniTick >= bin.writeTickMs) {
            out = iniStatus;
            out.writeTickMs = iniTick;
        } else {
            out = bin;
            WriteAutoSupplyStatusIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
        }
    } else if (iniOk) {
        out = iniStatus;
        out.writeTickMs = iniTick;
    } else if (binOk) {
        out = bin;
        WriteAutoSupplyStatusIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
    } else {
        return false;
    }
    return true;
}

bool WriteAutoSupplyStatus(const char* binDir, const AutoSupplyStatus& status) {
    if (!binDir || !binDir[0]) return false;
    if (status.magic != kAutoSupplyMagic || status.version != kAutoSupplyVersion) return false;

    AutoSupplyStatus normalized = status;
    const uint64_t tick = normalized.writeTickMs ? normalized.writeTickMs : GetTickCount64();
    normalized.writeTickMs = tick;
    return WriteAutoSupplyStatusIni(binDir, normalized, tick);
}

}  // namespace xcat
