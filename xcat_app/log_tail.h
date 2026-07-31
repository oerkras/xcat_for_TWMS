#pragma once

#include <string>
#include <vector>

namespace xcat::app {

// 读取日志文件末尾若干行（文件不存在则返回空）
std::vector<std::string> ReadLogTail(const char* path, size_t maxLines, size_t readBytes = 65536);

}  // namespace xcat::app
