#pragma once

#include <string>
#include <vector>

namespace xcat::ops {

std::vector<std::string> ReadLogTail(const char* path, size_t maxLines, size_t readBytes = 65536);

}  // namespace xcat::ops
