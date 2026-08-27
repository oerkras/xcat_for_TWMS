#pragma once

#include <Windows.h>

#include <string>

#include "imgui_log_sanitize.h"
#include "workspace_tabs.h"

struct AppWindow;

namespace xcat::app {

struct LaunchUiState {
    char accountLine[2048]{};
    char gpLoginLine[2048]{};      // 粘贴缓冲；解析成功后改成 * 占位，不回填密文
    char gpDisplayAccount[256]{};  // 仅邮箱，给输入框下方展示
    unsigned gpPasteEpoch = 0;     // 提交后换 ID，丢掉 ImGui 内部编辑态
    int gpLoginBrowserKind = 0;  // 0 自动(Chrome++>Chrome>Edge) / 1 Chrome++ / 2 Chrome / 3 Edge
    std::string status;
    std::string logTail;
    std::string prefsBinDir;
    DWORD lastLogRefreshMs = 0;
    int activeTab = static_cast<int>(WorkspaceTab::Home);
    bool pendingAutoLaunch = false;  // 冷启/切模式后自动：AttachWatch；GAMA PASS / 账密直登 / HK
    // 到点前禁止自动/手动启动：切启动模式/取票策略、或 GAMA PASS 冷启准备窗（0=不延迟）
    DWORD autoLaunchNotBeforeMs = 0;
    // 「删除浏览器账号数据」二次确认截止；0=未武装
    DWORD gpClearConfirmUntilMs = 0;
};

// GAMA PASS 冷启/切模式/再次点击后的自动换票准备窗（防误触）
inline constexpr DWORD kGamaPassAutoPrepMs = 5000;
inline constexpr unsigned kGamaPassAutoPrepSec = kGamaPassAutoPrepMs / 1000u;
inline constexpr DWORD kGpClearConfirmMs = 5000;
inline constexpr DWORD kGpClearConfirmDebounceMs = 400;

void LaunchPanel_LoadAccount(LaunchUiState& ui);
void LaunchPanel_SaveAccount(LaunchUiState& ui);
void LaunchPanel_FormatAccountForUi(LaunchUiState& ui);  // 连续 '-' 处分行，便于换行显示
void LaunchPanel_AppendLog(LaunchUiState& ui, const std::wstring& line);
void LaunchPanel_OnWebLog(const std::wstring& line);
bool LaunchPanel_AccountLooksValid(const LaunchUiState& ui, std::wstring* errOut = nullptr);
// 切启动策略后的防误触准备窗（默认 7s）；阻塞自动拉起与一键启动。
void LaunchPanel_ArmStrategyPrep(LaunchUiState& ui, DWORD ms = 7000);
// 剩余秒数；到期则清零。0 = 可启动。
unsigned LaunchPanel_StrategyPrepLeftSec(LaunchUiState& ui);
// 取消冷启/切模式留下的自动启动（准备窗一并清掉）。返回是否确实取消了待办。
bool LaunchPanel_CancelPendingAutoLaunch(LaunchUiState& ui);
// 进入 GAMA PASS 自动换票读秒（冷启/切模式/取消后再点同一入口）。
void LaunchPanel_ArmGamaPassAutoLaunch(LaunchUiState& ui);
// 进入 GAMA PASS 账密直登读秒（冷启/切模式/取消后再点同一入口）。
void LaunchPanel_ArmGamaPassDirectLaunch(LaunchUiState& ui, bool fromUserClick = false);
// 粘贴行解析成功则落盘、输入框改成 * 占位、只留下邮箱展示。reportError=false 时解析失败不改 status（仍在打字）。
bool LaunchPanel_TryCommitGamaPassDirectPaste(LaunchUiState& ui, bool reportError);
bool LaunchPanel_GpLoginLineIsMask(const LaunchUiState& ui);
bool LaunchPanel_StartOneClick(LaunchUiState& ui, bool honorStrategyPrep = true);
bool LaunchPanel_StartGamaPassDirect(LaunchUiState& ui);
// 取消进行中的账密直登或 GAMA PASS 自动登录。不关日常浏览器、不杀游戏。
bool LaunchPanel_CancelInFlightGpLogin(LaunchUiState& ui);
// 清独立罐 + 已保存账密 + 界面账号；进行中拒绝。不碰日常浏览器。须先二次确认。
bool LaunchPanel_ClearGamaPassDirectProfile(LaunchUiState& ui);
void LaunchPanel_ArmGpClearConfirm(LaunchUiState& ui);
void LaunchPanel_CancelGpClearConfirm(LaunchUiState& ui);
// 剩余秒数；到期则解除武装。0 = 未在确认。
unsigned LaunchPanel_GpClearConfirmLeftSec(LaunchUiState& ui);
// 已过防误触去抖，可以真正删除。
bool LaunchPanel_GpClearConfirmReady(LaunchUiState& ui);
void LaunchPanel_TryAutoLaunchWhenReady(LaunchUiState& ui);
void DrawMainShell(AppWindow& app, LaunchUiState& ui);

// 退出收尾；最小化时主循环也要调。返回 true 表示本帧已结束退出。
bool PollGracefulExit(AppWindow& app, LaunchUiState& ui);

}  // namespace xcat::app
