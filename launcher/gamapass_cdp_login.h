#pragma once

// Gama Pass：在用户默认 Chromium 浏览器里 CDP 点选换票
// ★ 红线：绝不清除浏览器 Cookie / Local Storage / refreshToken；
//   绝不调用 /v1/refresh/token；绝不 Navigate 强制 prompt=login 冲刷 SSO。
// 失败策略：
//   accounts/error → 置 accountsOauthError，外层关调试浏览器后干净重开 1 次（新 Galaxy OTT；
//     非同标签 soft-retry；识别靠标志位而非 Fail 文案）；
//   完整登录页、选昵称 ack 失败 → 立刻停、不重开登录页；
//   Main 上 init/过期 OTT 等官网拉起：未见到 NGM 时可宽限后「最后一次」stale-ott-retry
//   重开 Galaxy（见实现；已见 NGM 则不再 Navigate，防 SPGA0001）。

#include "chromium_cdp.h"
#include "http_beanfun_login.h"

#include <functional>

namespace msc::launcher {

using GpCdpOnLoginPageFn = std::function<void(msc::cdp::Session& cdp, HttpLoginLogFn log)>;

// SelectGameAccount 页：1-based 昵称槽（跳过「建立遊戲暱稱」后的第 N 个可选项）。
// 默认 1；范围 1..16；写入程序目录 gamapass_nick_slot.txt。
int GetGamaPassNickSlot();
void SetGamaPassNickSlot(int slot1Based);

// select-account 页：1-based 账号卡槽（自上而下第 N 张，跳过「使用其他帳號」）。
// 默认 1；范围 1..16；写入程序目录 gamapass_account_slot.txt。
int GetGamaPassAccountSlot();
void SetGamaPassAccountSlot(int slot1Based);

// 打开/附着 Chrome|Edge → Galaxy → 点 Gama Pass → 选号 → 选昵称 → 收 OTT 换票
HttpLoginResult HttpGamaPassCdpLoginToOtt(HttpLoginLogFn log = nullptr, int timeoutMs = 240000);

// 已 EnsureBrowser 的会话上走同一套点选换票（账密独立罐 19223 复用，勿再 Ensure 日常罐）。
// debugPort：换票成功后 CloseRemoteBrowser 用的调试口；失败不关窗。
// onLoginPage：落到完整 /login 时由账密助手自动填表；日常 UIA/CDP 不传。
HttpLoginResult HttpGamaPassCdpLoginToOttOnConnected(
    msc::cdp::Session& cdp, HttpLoginLogFn log = nullptr, int timeoutMs = 240000,
    int debugPort = msc::cdp::kDefaultRemoteDebugPort, GpCdpOnLoginPageFn onLoginPage = nullptr);

// 无 CDP / JS 点不到时：在独立罐窗口上 UIA 点「回到 GAMA PLAY」。
// debugPort>0 时优先匹配 --remote-debugging-port=N；cmdNeedle 匹配 cmdline（账密罐用 GpDeviceLoginProfile）。
bool TryUiaClickBackToGamaPlay(int debugPort, const wchar_t* cmdNeedle, HttpLoginLogFn log);

}  // namespace msc::launcher
