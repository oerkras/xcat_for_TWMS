#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <functional>
#include <string>

namespace xcat::app::attach_inject {

enum class LaunchMode {
    AttachWatch = 0,   // 手动启动并注入（默认）
    OneClickLogin = 1, // gamania (HK)：账密 HTTP/WebView 换票 + NGM + 注入
    GamaPassAuto = 2,  // GAMA PASS自动登录：浏览器点选换票 + NGM + 注入
};

inline bool IsAttachWatchMode(LaunchMode m) { return m == LaunchMode::AttachWatch; }
inline bool IsOneClickStyleMode(LaunchMode m) { return !IsAttachWatchMode(m); }

using LogFn = std::function<void(const std::wstring& line)>;

void Init(LogFn log);
// prefsBinDir = …/XCat_data：启动模式优先落盘到 state/（更新白名单保留），并同步写安装根。
void Init(LogFn log, const std::string& prefsBinDir);
void Shutdown();

LaunchMode GetLaunchMode();
void SetLaunchMode(LaunchMode mode);
const char* LaunchModeLabel(LaunchMode mode);

bool StartWatch();
// 非阻塞：置停旗并唤醒 poll；join 在后台完成（ImGui 切模式勿再卡 1s Sleep）。
void StopWatch();
bool IsWatching();
bool IsInjectBusy();

// 对当前已运行的 Classic 立即注入一次（不依赖监视开关）。
bool InjectNow(std::wstring* errOut = nullptr);

DWORD LastHandledPid();
std::string StatusBrief();

}  // namespace xcat::app::attach_inject
