#pragma once

#include <cstdint>
#include <cstddef>

namespace xcat {

// TWMS 精简控制契约：launcher ↔ payload，落盘 user.ini [core]。
constexpr uint32_t kPayloadControlMagic = 0x58435443u;  // 'XCTC'
constexpr uint32_t kPayloadControlVersion = 1u;
constexpr uint32_t kPayloadControlCoreIniVersion = 2u;
constexpr size_t kPayloadWorldNameCap = 64;

struct PayloadControl {
    uint32_t magic = kPayloadControlMagic;
    uint32_t version = kPayloadControlVersion;
    uint32_t fly = 0;
    uint32_t invuln = 0;
    uint32_t autoEnter = 0;   // 自动进游戏：分区→最少人频道→角色
    uint32_t charSlot = 1;    // 1-based 角色槽位
    int32_t worldId = 0;      // 0=未指定，改用 worldName
    char worldName[kPayloadWorldNameCap]{};
    uint64_t writeTickMs = 0;
};

void PayloadControlSetDefaults(PayloadControl& out);
bool ReadPayloadControl(const char* binDir, PayloadControl& out);
bool WritePayloadControl(const char* binDir, const PayloadControl& control);

}  // namespace xcat
