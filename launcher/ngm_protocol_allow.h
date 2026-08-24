#pragma once

// 官网 Main 用 ngm:// 拉 NGM 时，Chrome/Edge 会弹「要打开 Nexon Game Manager 吗？」。
// 只靠 UIA 勾选「始终允许」再点打开。不写 Preferences、不写日常 User Data、不清 Cookie。
// 启动前塞残缺 Preferences 会被 Chrome 当未正常退出丢掉，还会弹出崩溃报告窗。

#include <functional>
#include <string>

namespace msc::launcher {

using NgmProtocolLogFn = std::function<void(const std::wstring& line)>;

bool TryAcceptNgmProtocolDialog(const NgmProtocolLogFn& log);

}  // namespace msc::launcher
