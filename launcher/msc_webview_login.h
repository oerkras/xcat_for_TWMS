#pragma once

// WebView2 / HTTP 登录换票会话 — 嵌入 xcat_app（同进程）

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <functional>
#include <string>

namespace msc::weblogin {

using LogCallback = std::function<void(const std::wstring& line)>;

enum class AuthStrategy {
    HttpFirst = 0,   // 默认：HTTP 优先，失败且有 Runtime 则回退 WebView
    HttpOnly = 1,    // 仅 HTTP（无 WebView2 的虚拟机）
    WebViewOnly = 2, // 仅 WebView（旧行为）
};

// 遇验证码/双验时的 UI 策略（写入 bin/captcha_ui.txt）
enum class CaptchaUiMode {
    PopupOnCaptcha = 0,  // 默认：有 Runtime 则弹互动登录窗；否则开浏览器
    BrowserOnly = 1,     // 永不弹窗，只开浏览器（可仍静默 WebView 回退）
    SilentFallback = 2,  // 永不弹窗、不开浏览器；能回退则静默 WebView
};

// msgHwnd：收 WM_TIMER / WM_APP 的窗口（通常为 xcat 主窗）
// webHostHwnd：WebView2 父控件（子 STATIC）
bool Init(HWND msgHwnd, HWND webHostHwnd, LogCallback onLog);
bool IsReady();   // WebView 环境是否就绪（HTTP 策略不依赖）
bool IsBusy();
bool CanStartOneClick();  // 当前策略下是否允许点一键

AuthStrategy GetAuthStrategy();
void SetAuthStrategy(AuthStrategy s);
const char* AuthStrategyLabel(AuthStrategy s);

CaptchaUiMode GetCaptchaUiMode();
void SetCaptchaUiMode(CaptchaUiMode m);
const char* CaptchaUiModeLabel(CaptchaUiMode m);

// Evergreen Runtime 是否已安装
bool IsRuntimeInstalled();
bool PromptRuntimeInstall(HWND owner = nullptr);

// accountLine：邮箱-密码-…（连续任意个 '-' 分隔，只取前两项）
bool StartOneClick(const std::wstring& accountLine, std::wstring& err);

bool TryParseAccountLine(const std::wstring& accountLine, std::wstring& err);

void OnTimer(UINT_PTR timerId);
void OnResize();
void OnFlushLogs();  // WM_APP+1
void OnIdle();       // WM_APP+2
void OnStartWebViewLogin();  // WM_APP+3；wParam=1 弹出可交互登录窗
void OnStartWebViewLoginEx(WPARAM showInteractive);
void Shutdown();

std::wstring LoadSavedAccountLine();
void SaveAccountLine(const std::wstring& line);

constexpr UINT kMsgFlushLogs = WM_APP + 1;
constexpr UINT kMsgIdle = WM_APP + 2;
constexpr UINT kMsgStartWebViewLogin = WM_APP + 3;  // wParam=1：显示可交互登录窗（验证码）
constexpr UINT_PTR kBusyTimerId = 42;
constexpr UINT_PTR kAutoLoginTimerId = 43;
constexpr UINT_PTR kHttpFallbackWaitTimerId = 44;
constexpr UINT_PTR kHttpBusyTimerId = 45;
constexpr UINT kHttpFallbackWaitMs = 90000;  // 等 WebView 初始化再回退
constexpr UINT kHttpBusyTimeoutMs = 300000;  // HTTP 换票看门狗约 5 分钟

// 日志/状态栏稳定标记：UI 据此弹通知与改 status
inline constexpr wchar_t kNeedWebVerifyTag[] = L"[提示][需网页验证]";
inline constexpr wchar_t kHttpBusyTag[] = L"[状态][HTTP换票中]";
inline constexpr wchar_t kHttpTicketOkTag[] = L"[状态][HTTP换票成功]";
inline constexpr wchar_t kHttpTimeoutTag[] = L"[状态][HTTP超时]";

}  // namespace msc::weblogin
