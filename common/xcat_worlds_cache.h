#pragma once

#include <cstdint>
#include <cstddef>

namespace xcat {

constexpr uint32_t kWorldsCacheIniVersion = 1u;
constexpr size_t kWorldsCacheMax = 32;
constexpr size_t kWorldsCacheNameCap = 64;

struct WorldsCacheEntry {
    int32_t worldId = 0;
    char name[kWorldsCacheNameCap]{};
};

// user.ini [worlds]：登录扫到的分区列表（payload 写入，面板只读展示/点选）。
// keys: version, count, writeTickMs, w{N}.id, w{N}.name （N 从 1）
bool WriteWorldsCache(const char* binDir, const WorldsCacheEntry* entries, uint32_t count);
bool ReadWorldsCache(const char* binDir, WorldsCacheEntry* outEntries, uint32_t maxOut, uint32_t* outCount,
                     uint64_t* outWriteTickMs);

}  // namespace xcat
