#pragma once

// TWMS 启动/重挂锚点 MISS 灯：payload 写、launcher 调试 TAB 读。
// 独立 SHM（不挤 PayloadStatus 顶栏 5 灯），换版可单独升 version。

#include <cstdint>

namespace xcat {

constexpr uint32_t kAnchorLampsMagic = 0x5843414Cu;  // 'XCAL'
constexpr uint32_t kAnchorLampsVersion = 3u;
// v3：20 槽已被固定 roster(19) + SkipPrep 占满，新灯（DropFields）无处可放 → 扩到 24
constexpr size_t kAnchorLampMax = 24;
constexpr size_t kAnchorLampIdLen = 20;
constexpr size_t kAnchorLampDetailLen = 28;

// 0=未知(灰) 1=OK(绿) 2=降级(黄，如 shape 兜底/部分 MI) 3=MISS(红)
enum class AnchorLampCode : uint8_t {
    Unknown = 0,
    Ok = 1,
    Degraded = 2,
    Miss = 3,
};

#pragma pack(push, 1)
struct AnchorLampEntry {
    char id[kAnchorLampIdLen]{};
    char detail[kAnchorLampDetailLen]{};
    uint8_t code = 0;
    uint8_t _pad[3]{};
};

struct AnchorLampsStatus {
    uint32_t magic = kAnchorLampsMagic;
    uint32_t version = kAnchorLampsVersion;
    uint64_t writeTickMs = 0;
    uint32_t count = 0;
    AnchorLampEntry entries[kAnchorLampMax]{};
};
#pragma pack(pop)

void AnchorLampsSetDefaults(AnchorLampsStatus& out);

bool ReadAnchorLamps(const char* binDir, AnchorLampsStatus& out);
bool WriteAnchorLamps(const char* binDir, const AnchorLampsStatus& status);

bool AnchorLampsFresh(const AnchorLampsStatus& st, uint64_t nowMs, uint64_t maxAgeMs = 5000);

}  // namespace xcat
