#pragma once

#include <Windows.h>

#include <string>

struct AppWindow;

namespace xcat::app {

struct LaunchUiState {
    char accountLine[2048]{};
    std::string status;
    std::string logTail;
    std::string prefsBinDir;
    DWORD lastLogRefreshMs = 0;
    int activeTab = 0;
    bool pendingAutoLaunch = false;  // 启动时有有效账号串则就绪后自动一键
};

void LaunchPanel_LoadAccount(LaunchUiState& ui);
void LaunchPanel_SaveAccount(LaunchUiState& ui);
void LaunchPanel_FormatAccountForUi(LaunchUiState& ui);  // ---- 处分行，便于换行显示
void LaunchPanel_AppendLog(LaunchUiState& ui, const std::wstring& line);
void LaunchPanel_OnWebLog(const std::wstring& line);
bool LaunchPanel_AccountLooksValid(const LaunchUiState& ui, std::wstring* errOut = nullptr);
bool LaunchPanel_StartOneClick(LaunchUiState& ui);
void LaunchPanel_TryAutoLaunchWhenReady(LaunchUiState& ui);
void DrawMainShell(AppWindow& app, LaunchUiState& ui);

// 退出收尾；最小化时主循环也要调。返回 true 表示本帧已结束退出。
bool PollGracefulExit(AppWindow& app, LaunchUiState& ui);

}  // namespace xcat::app
