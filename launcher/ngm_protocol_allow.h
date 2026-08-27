#pragma once

// 官网 Main 用 ngm:// 拉 NGM 时，Chrome/Edge 会弹「要打开 Nexon Game Manager 吗？」。
// 只靠 UIA 勾选「始终允许」再点打开。不写 Preferences、不写日常 User Data、不清 Cookie。
// 启动前塞残缺 Preferences 会被 Chrome 当未正常退出丢掉，还会弹出崩溃报告窗。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <functional>
#include <string>

namespace msc::launcher {

using NgmProtocolLogFn = std::function<void(const std::wstring& line)>;

// 限制只点本轮登录浏览器，避免扫到日常 Chrome/Edge 乱点「打开」。
struct NgmProtocolAllowOpts {
    int debugPort = 0;                 // cmdline 含 --remote-debugging-port=N
    const wchar_t* cmdNeedle = nullptr;  // cmdline 含此串（账密罐 GpDeviceLoginProfile）
    HWND hwnd = nullptr;               // UIA 登录窗；同进程的协议气泡也会扫
};

bool TryAcceptNgmProtocolDialog(const NgmProtocolLogFn& log,
                                const NgmProtocolAllowOpts& opts = {});

}  // namespace msc::launcher
