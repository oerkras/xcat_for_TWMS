#pragma once

#include <string>
#include <string_view>

namespace xcat::app {

// 仅上屏：剥 IP / 已知域名 / 登录 URL 与票字段。落盘日志不要走这里。
std::string SanitizeImGuiLogLine(std::string_view raw);

}  // namespace xcat::app
