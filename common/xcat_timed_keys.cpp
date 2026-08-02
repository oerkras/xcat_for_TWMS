#include "xcat_timed_keys.h"

#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>

#include <cstdio>
#include <iterator>

namespace xcat {
namespace {

constexpr uint32_t kTimedKeysIniVersion = 1u;

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

void NormalizeSlots(TimedKeysConfig& cfg) {
    for (size_t i = 0; i < kTimedKeySlotCount; ++i) {
        cfg.slots[i].intervalMs = TimedKeysClampIntervalMs(cfg.slots[i].intervalMs);
    }
    TimedKeysNormalizeMasterEnabled(cfg);
}

bool ReadTimedKeysIni(const char* binDir, TimedKeysConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    uint32_t version = 0;
    if (!IniGetU32(ini, "timed_keys", "version", version) || version != kTimedKeysIniVersion)
        return false;

    TimedKeysSetDefaults(out);
    bool master = false;
    if (IniGetBool(ini, "timed_keys", "masterEnabled", master)) out.masterEnabled = master ? 1u : 0u;

    for (size_t i = 0; i < kTimedKeySlotCount; ++i) {
        char prefix[32]{};
        snprintf(prefix, sizeof(prefix), "slot.%zu.", i + 1);
        TimedKeySlotConfig& slot = out.slots[i];

        std::string key = std::string(prefix) + "vk";
        IniGetU32(ini, "timed_keys", key.c_str(), slot.vk);
        key = std::string(prefix) + "enabled";
        bool enabled = false;
        if (IniGetBool(ini, "timed_keys", key.c_str(), enabled)) slot.enabled = enabled ? 1u : 0u;
        key = std::string(prefix) + "intervalMs";
        IniGetU32(ini, "timed_keys", key.c_str(), slot.intervalMs);
    }

    NormalizeSlots(out);
    if (outWriteTick) IniGetU64(ini, "timed_keys", "writeTickMs", *outWriteTick);
    return true;
}

bool WriteTimedKeysIni(const char* binDir, const TimedKeysConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;

    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "timed_keys", "version", kTimedKeysIniVersion);
        IniSetU64(ini, "timed_keys", "writeTickMs", writeTickMs);
        IniSetBool(ini, "timed_keys", "masterEnabled", cfg.masterEnabled != 0);

        IniEraseKeysWithPrefix(ini, "timed_keys", "slot.");
        for (size_t i = 0; i < kTimedKeySlotCount; ++i) {
            const TimedKeySlotConfig& slot = cfg.slots[i];
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "slot.%zu.", i + 1);
            IniSetU32(ini, "timed_keys", (std::string(prefix) + "vk").c_str(), slot.vk);
            IniSetBool(ini, "timed_keys", (std::string(prefix) + "enabled").c_str(),
                       slot.enabled != 0);
            IniSetU32(ini, "timed_keys", (std::string(prefix) + "intervalMs").c_str(),
                      slot.intervalMs);
        }
    });
}

}  // namespace

bool TimedKeysAnySlotEnabled(const TimedKeysConfig& cfg) {
    for (size_t i = 0; i < kTimedKeySlotCount; ++i) {
        if (cfg.slots[i].enabled != 0) return true;
    }
    return false;
}

void TimedKeysNormalizeMasterEnabled(TimedKeysConfig& cfg) {
    cfg.masterEnabled = TimedKeysAnySlotEnabled(cfg) ? 1u : 0u;
}

uint32_t TimedKeysClampIntervalMs(uint32_t intervalMs) {
    const uint32_t minMs = kTimedKeysMinIntervalSec * 1000u;
    const uint32_t maxMs = kTimedKeysMaxIntervalSec * 1000u;
    if (intervalMs < minMs) return minMs;
    if (intervalMs > maxMs) return maxMs;
    return intervalMs;
}

uint32_t TimedKeysIntervalSecFromMs(uint32_t intervalMs) {
    const uint32_t sec = TimedKeysClampIntervalMs(intervalMs) / 1000u;
    return sec < kTimedKeysMinIntervalSec ? kTimedKeysMinIntervalSec : sec;
}

uint32_t TimedKeysIntervalMsFromSec(uint32_t intervalSec) {
    if (intervalSec < kTimedKeysMinIntervalSec) intervalSec = kTimedKeysMinIntervalSec;
    if (intervalSec > kTimedKeysMaxIntervalSec) intervalSec = kTimedKeysMaxIntervalSec;
    return intervalSec * 1000u;
}

void TimedKeysSetDefaults(TimedKeysConfig& out) {
    out = {};
    out.magic = kTimedKeysMagic;
    out.version = kTimedKeysVersion;
    out.masterEnabled = 0;

    static const struct {
        uint32_t vk;
        uint32_t intervalMs;
    } kDefaults[] = {
        {'7', 60000},
        {'8', 60000},
        {'9', 60000},
        {'0', 60000},
        {VK_OEM_MINUS, 420000},
        {VK_OEM_PLUS, 420000},
        {'Z', 1000},
    };
    static_assert(std::size(kDefaults) == kTimedKeySlotCount, "slot defaults");

    for (size_t i = 0; i < kTimedKeySlotCount; ++i) {
        out.slots[i].vk = kDefaults[i].vk;
        out.slots[i].intervalMs = kDefaults[i].intervalMs;
        out.slots[i].enabled = 0;
    }
}

const char* TimedKeySlotLabel(size_t index) {
    static const char* kLabels[] = {"7", "8", "9", "0", "-", "=", "Z"};
    if (index >= kTimedKeySlotCount) return "?";
    return kLabels[index];
}

bool ReadTimedKeys(const char* binDir, TimedKeysConfig& out) {
    TimedKeysSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    uint64_t iniTick = 0;
    TimedKeysConfig iniCfg{};
    if (!ReadTimedKeysIni(binDir, iniCfg, &iniTick)) return false;
    out = iniCfg;
    out.writeTickMs = iniTick;
    out.magic = kTimedKeysMagic;
    out.version = kTimedKeysVersion;
    return true;
}

bool WriteTimedKeys(const char* binDir, const TimedKeysConfig& cfg) {
    if (!binDir || !binDir[0]) return false;
    if (cfg.magic != kTimedKeysMagic || cfg.version != kTimedKeysVersion) return false;

    TimedKeysConfig toWrite = cfg;
    NormalizeSlots(toWrite);

    uint64_t iniTick = 0;
    TimedKeysConfig iniProbe{};
    ReadTimedKeysIni(binDir, iniProbe, &iniTick);
    if (toWrite.writeTickMs <= iniTick) toWrite.writeTickMs = iniTick + 1u;
    if (!toWrite.writeTickMs) toWrite.writeTickMs = GetTickCount64();

    return WriteTimedKeysIni(binDir, toWrite, toWrite.writeTickMs);
}

bool WriteTimedKeysCas(const char* binDir, const TimedKeysConfig& cfg, uint64_t expectedTick,
                       bool* outConflict) {
    if (outConflict) *outConflict = false;
    if (!binDir || !binDir[0]) return false;
    if (cfg.magic != kTimedKeysMagic || cfg.version != kTimedKeysVersion) return false;

    uint64_t iniTick = 0;
    TimedKeysConfig iniProbe{};
    ReadTimedKeysIni(binDir, iniProbe, &iniTick);
    if (iniTick != expectedTick) {
        if (outConflict) *outConflict = true;
        return false;
    }

    TimedKeysConfig toWrite = cfg;
    NormalizeSlots(toWrite);
    toWrite.writeTickMs = expectedTick + 1u;
    if (!toWrite.writeTickMs) toWrite.writeTickMs = 1u;
    return WriteTimedKeysIni(binDir, toWrite, toWrite.writeTickMs);
}

}  // namespace xcat
