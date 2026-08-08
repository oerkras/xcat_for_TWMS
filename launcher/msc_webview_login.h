#pragma once

// 经典版登录换票会话 — 嵌入 xcat_app（GAMA PASS CDP / HTTP Beanfun；无 WebView2）

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <functional>
#include <string>

namespace msc::weblogin {

using LogCallback = std::function<void(const std::wstring& line)>;

enum class AuthStrategy {
    HttpFirst = 0,    // HK：HTTP 换票（磁盘兼容名；与 HttpOnly 行为相同）
    HttpOnly = 1,     // 历史值：读盘归一为 HttpFirst
    // 2 = 历史 webview_only：读盘归一为 HttpFirst
    GamaPassAuto = 3, // 日常浏览器 Windows UI Automation 点选换票（无需账密）
};

inline bool AuthStrategyNeedsAccountCreds(AuthStrategy s) {
    return s != AuthStrategy::GamaPassAuto;
}

// 遇验证码/双验时的 UI 策略（写入 bin/captcha_ui.txt）
enum class CaptchaUiMode {
    OpenBrowser = 0,     // 默认：开系统浏览器（磁盘兼容易名 popup_on_captcha / browser_only）
    NoBrowser = 1,       // 不开浏览器，仅提示（磁盘兼容易名 silent）
    // 历史别名（与上同值语义，供旧代码/读盘）
    PopupOnCaptcha = OpenBrowser,
    BrowserOnly = OpenBrowser,
    SilentFallback = NoBrowser,
};

bool Init(HWND msgHwnd, LogCallback onLog);
bool IsBusy();
bool CanStartOneClick();

AuthStrategy GetAuthStrategy();
void SetAuthStrategy(AuthStrategy s);
const char* AuthStrategyLabel(AuthStrategy s);

CaptchaUiMode GetCaptchaUiMode();
void SetCaptchaUiMode(CaptchaUiMode m);
const char* CaptchaUiModeLabel(CaptchaUiMode m);

// GAMA PASS：SelectGameAccount 页 1-based 昵称槽（默认 1；写入 bin/gamapass_nick_slot.txt）
int GetGamaPassNickSlot();
void SetGamaPassNickSlot(int slot1Based);

// GAMA PASS：select-account 页 1-based 账号槽（默认 1；写入 bin/gamapass_account_slot.txt）
int GetGamaPassAccountSlot();
void SetGamaPassAccountSlot(int slot1Based);

bool StartOneClick(const std::wstring& accountLine, std::wstring& err);

bool TryParseAccountLine(const std::wstring& accountLine, std::wstring& err);

void OnTimer(UINT_PTR timerId);
void OnFlushLogs();  // WM_APP+1
void OnIdle();       // WM_APP+2
void Shutdown();

std::wstring LoadSavedAccountLine();
void SaveAccountLine(const std::wstring& line);

constexpr UINT kMsgFlushLogs = WM_APP + 1;
constexpr UINT kMsgIdle = WM_APP + 2;
constexpr UINT_PTR kHttpBusyTimerId = 45;
constexpr UINT kHttpBusyTimeoutMs = 300000;  // HTTP / GamaPass 换票看门狗约 5 分钟

inline constexpr wchar_t kNeedWebVerifyTag[] = L"[提示][需网页验证]";
inline constexpr wchar_t kHttpBusyTag[] = L"[状态][HTTP换票中]";
inline constexpr wchar_t kHttpTicketOkTag[] = L"[状态][HTTP换票成功]";
inline constexpr wchar_t kHttpTimeoutTag[] = L"[状态][HTTP超时]";

}  // namespace msc::weblogin
