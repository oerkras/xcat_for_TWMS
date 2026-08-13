#pragma once

// Gama Pass：日常 Chromium + Windows UI Automation 点选换票（正道）。
// ★ 不建 GamaPassCdpProfile、不开 remote-debugging-port；与日常同一 Cookie 罐。
// ★ 红线：不 clearCookie / 不 refresh / 不改 OAuth prompt / 不写回其它目录。
// ★ 自动登录开始前：不结束日常浏览器；靠 --new-window + 启动前 hwnd 快照只附着本轮登录窗。
// ★ 若单例把 Galaxy 开进已有窗：等新窗超时后兜底附着登录流标题窗，且不 WM_CLOSE 该窗。
// ★ accounts/error 或点选超时：关本轮「自开」登录窗后干净重开 Galaxy 最多 3 次（仍不杀浏览器主进程）。

#include "http_beanfun_login.h"

namespace msc::launcher {

HttpLoginResult HttpGamaPassUiaLoginToOtt(HttpLoginLogFn log = nullptr, int timeoutMs = 240000);

}  // namespace msc::launcher
