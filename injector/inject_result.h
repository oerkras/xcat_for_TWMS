#pragma once

#include <cstdint>
#include <string>

namespace xcat {

struct InjectResult {
    bool ok = false;
    std::string message;
    uintptr_t baseAddress = 0;
    uint32_t imageSize = 0;
};

}  // namespace xcat
