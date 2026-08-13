#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

#include "workspace_tabs.h"

struct AppWindow;

namespace xcat::app {

struct LaunchUiState {
    char accountLine[2048]{};
    std::string status;
    std::string logTail;
    std::string prefsBinDir;
    DWORD lastLogRefreshMs = 0;
    int activeTab = static_cast<int>(WorkspaceTab::Home);
    bool pendingAutoLaunch = false;  // 冷启/切模式后自动：AttachWatch 监视；GAMA PASS / HK 换票启动
    // 到点前禁止自动/手动启动：切启动模式/取票策略、或 GAMA PASS 冷启准备窗（0=不延迟）
    DWORD autoLaunchNotBeforeMs = 0;
};

// GAMA PASS 冷启/切模式自动换票准备窗（防误触；用户反馈 7s 过长）
inline constexpr DWORD kGamaPassAutoPrepMs = 3000;

void LaunchPanel_LoadAccount(LaunchUiState& ui);
void LaunchPanel_SaveAccount(LaunchUiState& ui);
void LaunchPanel_FormatAccountForUi(LaunchUiState& ui);  // 连续 '-' 处分行，便于换行显示
// 仅上屏：剥 IP / 域名 / 登录 URL；落盘 JSONL 与 launcher.log 仍是原文。
std::string SanitizeImGuiLogLine(std::string_view raw);
void LaunchPanel_AppendLog(LaunchUiState& ui, const std::wstring& line);
void LaunchPanel_OnWebLog(const std::wstring& line);
bool LaunchPanel_AccountLooksValid(const LaunchUiState& ui, std::wstring* errOut = nullptr);
// 切启动策略后的防误触准备窗（默认 7s）；阻塞自动拉起与一键启动。
void LaunchPanel_ArmStrategyPrep(LaunchUiState& ui, DWORD ms = 7000);
// 剩余秒数；到期则清零。0 = 可启动。
unsigned LaunchPanel_StrategyPrepLeftSec(LaunchUiState& ui);
// 取消冷启/切模式留下的自动启动（准备窗一并清掉）。返回是否确实取消了待办。
bool LaunchPanel_CancelPendingAutoLaunch(LaunchUiState& ui);
bool LaunchPanel_StartOneClick(LaunchUiState& ui, bool honorStrategyPrep = true);
void LaunchPanel_TryAutoLaunchWhenReady(LaunchUiState& ui);
void DrawMainShell(AppWindow& app, LaunchUiState& ui);

// 退出收尾；最小化时主循环也要调。返回 true 表示本帧已结束退出。
bool PollGracefulExit(AppWindow& app, LaunchUiState& ui);

}  // namespace xcat::app
