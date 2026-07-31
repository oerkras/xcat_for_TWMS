#pragma once

// TWMS 工作区各 TAB 设计稿（对齐枫星布局；控件可点预览，契约/注入未接）

struct AppWindow;

namespace xcat::app {

struct LaunchUiState;

enum class WorkspaceTab : int {
    Launch = 0,
    Home,
    HangupSchedule,
    MultiSkill,
    Relogin,
    TimedKeys,
    Buffs,
    Travel,
    Beta,
    Debug,
    Count,
};

inline constexpr const char* kWorkspaceTabLabels[] = {
    "启动", "首页", "挂机时段", "技能多发", "遇人策略",
    "定时按键", "BUFF", "超级赶路", "实验", "调试",
};
static_assert(sizeof(kWorkspaceTabLabels) / sizeof(kWorkspaceTabLabels[0]) ==
                  static_cast<int>(WorkspaceTab::Count),
              "tab label count mismatch");

void DrawWorkspaceTabContent(AppWindow& app, LaunchUiState& ui, int tabIndex);

}  // namespace xcat::app
