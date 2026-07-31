#pragma once

#include "xcat_version_rc.h"

#include <cstdint>

namespace xcat {

constexpr const char* kXcatVersionString = XCAT_VERSION_STRING;
constexpr uint32_t kXcatBuildId = static_cast<uint32_t>(XCAT_VER_BUILD);
constexpr const char* kXcatProductName = "XCat TWMS";

}  // namespace xcat
