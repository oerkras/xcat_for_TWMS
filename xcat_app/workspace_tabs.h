#pragma once

// TWMS 工作区各 TAB 设计稿（对齐枫星布局；控件可点预览，契约/注入未接）

#include <string>

struct AppWindow;

namespace xcat::app {

struct LaunchUiState;

enum class WorkspaceTab : int {
    Home = 0,  // 冷启默认页
    HangupSchedule,
    MultiSkill,
    Relogin,
    TimedKeys,
    Buffs,
    Travel,
    Beta,
    Launch,  // 紧挨调试左侧
    Debug,
    Count,
};

inline constexpr const char* kWorkspaceTabLabels[] = {
    "首页", "挂机时段", "技能多发", "遇人策略", "定时按键",
    "BUFF", "超级赶路", "实验", "启动", "调试",
};
static_assert(sizeof(kWorkspaceTabLabels) / sizeof(kWorkspaceTabLabels[0]) ==
                  static_cast<int>(WorkspaceTab::Count),
              "tab label count mismatch");

void DrawWorkspaceTabContent(AppWindow& app, LaunchUiState& ui, int tabIndex);

// Bump [core] manualRejoinSeq. requireInjected：无游戏进程则拒绝写盘。
// 成功返回 true；失败时 outErr 可填短因（可选）。
bool TriggerManualRejoin(const std::string& prefsBinDir, bool requireInjected,
                         std::string* outErr = nullptr);

}  // namespace xcat::app
