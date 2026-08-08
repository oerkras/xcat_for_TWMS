#pragma once

// Gama Pass：日常 Chromium + Windows UI Automation 点选换票（正道）。
// ★ 不建 GamaPassCdpProfile、不开 remote-debugging-port；与日常同一 Cookie 罐。
// ★ 红线：不 clearCookie / 不 refresh / 不改 OAuth prompt / 不写回其它目录。
// ★ 自动登录开始前：结束同安装已开浏览器主进程（用户授权；不清 Cookie），确保只拉起本轮登录窗。
// ★ accounts/error 或点选超时：关本轮登录窗后干净重开 Galaxy 最多 3 次。

#include "http_beanfun_login.h"

namespace msc::launcher {

HttpLoginResult HttpGamaPassUiaLoginToOtt(HttpLoginLogFn log = nullptr, int timeoutMs = 240000);

}  // namespace msc::launcher
