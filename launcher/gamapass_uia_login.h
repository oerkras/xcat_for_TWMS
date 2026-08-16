#pragma once

// Gama Pass：Chromium + Windows UI Automation 点选换票（正道）。
// ★ 默认日常目录、无调试口。账密助手留下的 device_id 不劫持自动登录（助手仍用独立罐）。
// ★ 红线：不 clearCookie / 不 refresh / 不改 OAuth prompt / 不把独立罐写回日常。
// ★ 助手登录完成不会自动换票；由用户再点 GAMA PASS自动登录（走日常浏览器）。
// ★ 自动登录开始前：不结束日常浏览器；靠 --new-window + 启动前 hwnd 快照只附着本轮登录窗。
// ★ 若单例把 Galaxy 开进已有窗：等新窗超时后兜底附着登录流标题窗，且不 WM_CLOSE 该窗。
// ★ accounts/error、点选超时、登录页连不上（刷新仍失败）：关本轮「自开」登录窗后干净重开 Galaxy 最多 10 次（仍不杀浏览器主进程）。

#include "http_beanfun_login.h"

namespace msc::launcher {

HttpLoginResult HttpGamaPassUiaLoginToOtt(HttpLoginLogFn log = nullptr, int timeoutMs = 240000);

}  // namespace msc::launcher
