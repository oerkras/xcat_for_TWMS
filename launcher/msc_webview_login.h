#pragma once

// WebView2 登录/换票会话 — 嵌入 xcat_app（同进程），替代独立 MscLauncher.exe

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <functional>
#include <string>

namespace msc::weblogin {

using LogCallback = std::function<void(const std::wstring& line)>;

// msgHwnd：收 WM_TIMER / WM_APP 的窗口（通常为 xcat 主窗）
// webHostHwnd：WebView2 父控件（子 STATIC）
bool Init(HWND msgHwnd, HWND webHostHwnd, LogCallback onLog);
bool IsReady();
bool IsBusy();

// accountLine：邮箱----密码----…
bool StartOneClick(const std::wstring& accountLine, std::wstring& err);

// 仅校验账号串（不启动）；空或无法识别返回 false
bool TryParseAccountLine(const std::wstring& accountLine, std::wstring& err);

void OnTimer(UINT_PTR timerId);
void OnResize();
void OnFlushLogs();  // WM_APP+1
void OnIdle();       // WM_APP+2 启动完成/失败
void Shutdown();

std::wstring LoadSavedAccountLine();
void SaveAccountLine(const std::wstring& line);

constexpr UINT kMsgFlushLogs = WM_APP + 1;
constexpr UINT kMsgIdle = WM_APP + 2;
constexpr UINT_PTR kBusyTimerId = 42;
constexpr UINT_PTR kAutoLoginTimerId = 43;

}  // namespace msc::weblogin
