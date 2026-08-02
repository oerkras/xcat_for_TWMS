#include "xcat_worlds_cache.h"

#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace xcat {
namespace {

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

}  // namespace

bool WriteWorldsCache(const char* binDir, const WorldsCacheEntry* entries, uint32_t count) {
    if (!binDir || !binDir[0] || !entries) return false;
    if (!EnsureStateDir(binDir)) return false;
    if (count > kWorldsCacheMax) count = static_cast<uint32_t>(kWorldsCacheMax);

    const std::string path = UserConfigIniPath(binDir);
    const uint64_t tick = GetTickCount64();
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "worlds", "version", kWorldsCacheIniVersion);
        IniSetU32(ini, "worlds", "count", count);
        IniSetU64(ini, "worlds", "writeTickMs", tick);
        (void)IniEraseIndexedKeysAbove(ini, "worlds", "w", count);
        for (uint32_t i = 0; i < count; ++i) {
            char keyId[32]{};
            char keyName[32]{};
            snprintf(keyId, sizeof(keyId), "w%u.id", i + 1);
            snprintf(keyName, sizeof(keyName), "w%u.name", i + 1);
            IniSetU32(ini, "worlds", keyId, static_cast<uint32_t>(entries[i].worldId));
            char name[kWorldsCacheNameCap]{};
            strncpy_s(name, entries[i].name, _TRUNCATE);
            IniSetString(ini, "worlds", keyName, name);
        }
    });
}

bool ReadWorldsCache(const char* binDir, WorldsCacheEntry* outEntries, uint32_t maxOut, uint32_t* outCount,
                     uint64_t* outWriteTickMs) {
    if (outCount) *outCount = 0;
    if (outWriteTickMs) *outWriteTickMs = 0;
    if (!binDir || !binDir[0] || !outEntries || maxOut == 0) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    uint32_t count = 0;
    if (!IniGetU32(ini, "worlds", "count", count) || count == 0) return false;
    if (count > maxOut) count = maxOut;
    if (count > kWorldsCacheMax) count = static_cast<uint32_t>(kWorldsCacheMax);

    uint64_t tick = 0;
    IniGetU64(ini, "worlds", "writeTickMs", tick);
    if (outWriteTickMs) *outWriteTickMs = tick;

    uint32_t got = 0;
    for (uint32_t i = 0; i < count; ++i) {
        char keyId[32]{};
        char keyName[32]{};
        snprintf(keyId, sizeof(keyId), "w%u.id", i + 1);
        snprintf(keyName, sizeof(keyName), "w%u.name", i + 1);
        uint32_t id = 0;
        if (!IniGetU32(ini, "worlds", keyId, id)) continue;
        WorldsCacheEntry& e = outEntries[got];
        e = WorldsCacheEntry{};
        e.worldId = static_cast<int32_t>(id);
        std::string name;
        if (IniGetString(ini, "worlds", keyName, name)) {
            // 热写竞态偶发把下一键粘进来：`雪吉拉w2.id=2` → 面板显示「雪吉?w2.id=2」。
            const size_t glue = name.find(".id=");
            if (glue != std::string::npos) {
                size_t cut = glue;
                while (cut > 0 && std::isdigit(static_cast<unsigned char>(name[cut - 1]))) --cut;
                if (cut > 0 && (name[cut - 1] == 'w' || name[cut - 1] == 'W')) {
                    name.resize(cut - 1);
                    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                } else {
                    name.clear();
                }
            } else if (name.find('=') != std::string::npos) {
                name.clear();
            }
            if (!name.empty()) strncpy_s(e.name, name.c_str(), _TRUNCATE);
        }
        ++got;
    }
    if (outCount) *outCount = got;
    return got > 0;
}

}  // namespace xcat
