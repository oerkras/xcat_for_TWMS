#pragma once

#include <cstdint>
#include <string>

namespace xcat {

// 定时按键：user.ini [timed_keys]。launcher ↔ payload 磁盘契约（无 SHM 命令位）。
// 产品：经典版 TWMS；对照枫星同名契约，实现面独立。

constexpr uint32_t kTimedKeysMagic = 0x58435454u;  // 'XCTT'
constexpr uint32_t kTimedKeysVersion = 1u;
constexpr size_t kTimedKeySlotCount = 7u;

constexpr uint32_t kTimedKeysMinIntervalSec = 1u;
constexpr uint32_t kTimedKeysMaxIntervalSec = 3600u;

#pragma pack(push, 1)
struct TimedKeySlotConfig {
    uint32_t vk = 0;          // Win32 VK_*（默认 7/8/9/0/-/=/Z）
    uint32_t enabled = 0;     // 0/1
    uint32_t intervalMs = 0;  // 触发间隔（毫秒）
};

struct TimedKeysConfig {
    uint32_t magic = kTimedKeysMagic;
    uint32_t version = kTimedKeysVersion;
    uint32_t masterEnabled = 0;
    TimedKeySlotConfig slots[kTimedKeySlotCount]{};
    uint64_t writeTickMs = 0;
};
#pragma pack(pop)

void TimedKeysSetDefaults(TimedKeysConfig& out);

bool TimedKeysAnySlotEnabled(const TimedKeysConfig& cfg);
void TimedKeysNormalizeMasterEnabled(TimedKeysConfig& cfg);
uint32_t TimedKeysClampIntervalMs(uint32_t intervalMs);
uint32_t TimedKeysIntervalSecFromMs(uint32_t intervalMs);
uint32_t TimedKeysIntervalMsFromSec(uint32_t intervalSec);

const char* TimedKeySlotLabel(size_t index);

bool ReadTimedKeys(const char* binDir, TimedKeysConfig& out);
bool WriteTimedKeys(const char* binDir, const TimedKeysConfig& cfg);
// CAS：仅当磁盘 writeTickMs == expectedTick 时写入并升为 expectedTick+1。
bool WriteTimedKeysCas(const char* binDir, const TimedKeysConfig& cfg, uint64_t expectedTick,
                       bool* outConflict);

}  // namespace xcat
