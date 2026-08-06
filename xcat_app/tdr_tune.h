#pragma once

// 本机 TDR（Timeout Detection and Recovery）调优：改 HKLM GraphicsDrivers。
// 与游戏 core 无关；写完必须重启系统/虚拟机才生效。XCat 已 requireAdministrator。

#include <cstdint>
#include <string>

namespace xcat::app::tdr {

inline constexpr uint32_t kRecommendedDelaySec = 8;

struct Snapshot {
    // 键不存在时 present=false，effectiveSec 用系统默认（Delay=2 / DdiDelay=5）。
    bool delayPresent = false;
    uint32_t delaySec = 2;
    bool ddiPresent = false;
    uint32_t ddiSec = 5;
    bool readable = false;
    std::string err;
};

Snapshot Read();

// 写入 TdrDelay / TdrDdiDelay = recommendedSec（默认 8）。成功后仍需重启。
bool ApplyRecommended(uint32_t recommendedSec, std::string* errOut);

// 删除两键，恢复系统默认。成功后仍需重启。
bool RestoreDefaults(std::string* errOut);

}  // namespace xcat::app::tdr
