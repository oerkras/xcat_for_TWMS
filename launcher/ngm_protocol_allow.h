#pragma once

// 官网 Main 用 ngm:// 拉 NGM 时，Chrome/Edge 会弹「要打开 Nexon Game Manager 吗？」。
// 独立罐：启动前写入 Preferences.protocol_handler（Chrome 关闭时才能写）。
// 弹窗仍出现：UIA 勾选「始终允许」再点打开。不写日常 User Data，不清 Cookie。

#include <functional>
#include <string>

namespace msc::launcher {

using NgmProtocolLogFn = std::function<void(const std::wstring& line)>;

void SeedNgmProtocolAllowlist(const std::wstring& userDataDir, const NgmProtocolLogFn& log);
bool TryAcceptNgmProtocolDialog(const NgmProtocolLogFn& log);

}  // namespace msc::launcher
