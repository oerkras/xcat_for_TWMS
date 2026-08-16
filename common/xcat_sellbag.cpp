#include "xcat_sellbag.h"

#include "xcat_config_ini.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>

namespace xcat {
namespace {

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%sstate", binDir);
    CreateDirectoryA(dir, nullptr);
    return true;
}

const char* TargetMaskToIni(uint32_t mask) {
    (void)mask;
    return "all";  // 不卖名单全栏共用；保留符号以免旧调用断链
}

uint32_t TargetMaskFromIni(const char* s) {
    (void)s;
    return kSellbagBagAll;
}

bool ReadSellbagBinLegacy(const char* binDir, SellbagConfig& out) {
    SellbagSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    const std::string path = SellbagPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    SellbagConfig disk{};
    const size_t n = fread(&disk, 1, sizeof(SellbagConfig), f);
    fclose(f);
    const size_t v1Size = offsetof(SellbagConfig, manualSeq);
    if (n < v1Size) return false;
    if (disk.magic != kSellbagMagic) return false;
    if (disk.version != 1u && disk.version != kSellbagVersion) return false;

    out = disk;
    out.version = kSellbagVersion;
    if (n < sizeof(SellbagConfig)) {
        out.manualSeq = 0;
        out.manualMask = 0;
    }
    if (out.keepRuleCount > static_cast<uint32_t>(kSellbagMaxKeepRules))
        out.keepRuleCount = kSellbagMaxKeepRules;
    out.manualMask &= kSellbagBagAll;
    return true;
}

}  // namespace

void SellbagSetDefaults(SellbagConfig& out) {
    out = {};
    out.magic = kSellbagMagic;
    out.version = kSellbagVersion;
    // 默认保留「礦」「玻璃鞋」（艾溫任務）；用户可清空或改关键词。
    out.keepRuleCount = static_cast<uint32_t>(kSellbagDefaultKeepNameKeyCount);
    for (int i = 0; i < kSellbagDefaultKeepNameKeyCount; ++i) {
        out.keepRules[i].enabled = 1;
        out.keepRules[i].targetMask = kSellbagBagAll;
        strncpy_s(out.keepRules[i].nameKey, kSellbagDefaultKeepNameKeys[i], _TRUNCATE);
    }
}

std::string SellbagRelPath() { return "state\\sellbag.bin"; }

std::string SellbagPath(const char* binDir) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%s%s", binDir ? binDir : "", SellbagRelPath().c_str());
    return path;
}

bool ReadSellbagKeepRulesIni(const char* binDir, SellbagConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    uint32_t count = 0;
    if (!IniGetU32(ini, "sellbag", "keepCount", count)) return false;

    SellbagSetDefaults(out);
    out.keepRuleCount = count > static_cast<uint32_t>(kSellbagMaxKeepRules)
                            ? static_cast<uint32_t>(kSellbagMaxKeepRules)
                            : count;

    for (uint32_t i = 0; i < out.keepRuleCount; ++i) {
        char prefix[32]{};
        snprintf(prefix, sizeof(prefix), "keep.%u.", i + 1);
        SellbagKeepRule& r = out.keepRules[i];

        bool enabled = false;
        std::string key = std::string(prefix) + "enabled";
        if (IniGetBool(ini, "sellbag", key.c_str(), enabled)) r.enabled = enabled ? 1u : 0u;

        // 产品：不卖名单全栏共用（装备+其他）。可读历史 keep.N.target，写入侧仍落 all。
        r.targetMask = kSellbagBagAll;

        key = std::string(prefix) + "name";
        std::string name;
        if (IniGetString(ini, "sellbag", key.c_str(), name))
            strncpy_s(r.nameKey, name.c_str(), _TRUNCATE);
    }

    if (outWriteTick) IniGetU64(ini, "sellbag", "writeTickMs", *outWriteTick);

    uint32_t manualSeq = 0;
    uint32_t manualMask = 0;
    uint32_t abortSeq = 0;
    if (IniGetU32(ini, "sellbag", "manualSeq", manualSeq)) out.manualSeq = manualSeq;
    if (IniGetU32(ini, "sellbag", "manualMask", manualMask)) out.manualMask = manualMask & kSellbagBagAll;
    if (IniGetU32(ini, "sellbag", "abortSeq", abortSeq)) out.abortSeq = abortSeq;
    return true;
}

bool WriteSellbagKeepRulesIni(const char* binDir, const SellbagConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;

    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        const uint32_t count =
            cfg.keepRuleCount > static_cast<uint32_t>(kSellbagMaxKeepRules)
                ? static_cast<uint32_t>(kSellbagMaxKeepRules)
                : cfg.keepRuleCount;
        // 先清 keep.N.*，再按当前 count 重写，避免收缩后残留孤儿 key。
        IniEraseKeysWithPrefix(ini, "sellbag", "keep.");
        IniSetU32(ini, "sellbag", "keepCount", count);
        IniSetU64(ini, "sellbag", "writeTickMs", writeTickMs);
        IniSetU32(ini, "sellbag", "manualSeq", cfg.manualSeq);
        IniSetU32(ini, "sellbag", "manualMask", cfg.manualMask & kSellbagBagAll);
        IniSetU32(ini, "sellbag", "abortSeq", cfg.abortSeq);

        for (uint32_t i = 0; i < count; ++i) {
            const SellbagKeepRule& r = cfg.keepRules[i];
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "keep.%u.", i + 1);

            IniSetBool(ini, "sellbag", (std::string(prefix) + "enabled").c_str(), r.enabled != 0);
            // 不卖名单不按栏拆分；强制 all，清掉历史 equip/etc。
            IniSetString(ini, "sellbag", (std::string(prefix) + "target").c_str(), "all");
            IniSetString(ini, "sellbag", (std::string(prefix) + "name").c_str(), r.nameKey);
        }
    });
}

bool ReadSellbag(const char* binDir, SellbagConfig& out) {
    SellbagSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    uint64_t iniTick = 0;
    SellbagConfig iniCfg{};
    const bool iniOk = ReadSellbagKeepRulesIni(binDir, iniCfg, &iniTick);

    SellbagConfig bin{};
    const bool binOk = ReadSellbagBinLegacy(binDir, bin);

    if (iniOk && binOk) {
        if (iniTick >= bin.writeTickMs) {
            out = iniCfg;
            out.writeTickMs = iniTick;
        } else {
            out = bin;
            WriteSellbagKeepRulesIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
        }
    } else if (iniOk) {
        out = iniCfg;
        out.writeTickMs = iniTick;
    } else if (binOk) {
        out = bin;
        WriteSellbagKeepRulesIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
    } else {
        return false;
    }

    // 读盘兜底：历史 keep.N.target=equip/etc 一律抬成全栏共用。
    for (uint32_t i = 0; i < out.keepRuleCount && i < static_cast<uint32_t>(kSellbagMaxKeepRules);
         ++i) {
        out.keepRules[i].targetMask = kSellbagBagAll;
    }
    return true;
}

bool WriteSellbag(const char* binDir, const SellbagConfig& cfg) {
    if (!binDir || !binDir[0]) return false;

    SellbagConfig normalized = cfg;
    normalized.magic = kSellbagMagic;
    normalized.version = kSellbagVersion;
    if (normalized.keepRuleCount > static_cast<uint32_t>(kSellbagMaxKeepRules))
        normalized.keepRuleCount = static_cast<uint32_t>(kSellbagMaxKeepRules);
    normalized.manualMask &= kSellbagBagAll;
    // 不卖名单：全栏共用，忽略规则上的分栏掩码。
    for (uint32_t i = 0; i < normalized.keepRuleCount; ++i)
        normalized.keepRules[i].targetMask = kSellbagBagAll;

    // 同毫秒连续写会让 payload 因 writeTickMs 未变而忽略配置；强制单调递增。
    static uint64_t s_lastTick = 0;
    uint64_t tick = normalized.writeTickMs ? normalized.writeTickMs : GetTickCount64();
    if (tick <= s_lastTick) tick = s_lastTick + 1;
    s_lastTick = tick;
    normalized.writeTickMs = tick;

    return WriteSellbagKeepRulesIni(binDir, normalized, tick);
}

}  // namespace xcat
