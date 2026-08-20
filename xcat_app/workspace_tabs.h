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
    MobGather,  // 第三排首：吸怪 快攻（原首页卡迁出）
    AutoSell,   // 第三排：一键卖装 + 自动回城卖/补给（原首页卡片）
    AutoStat,   // 第三排：自动加点（属性 AP）
    AutoSkill,  // 第三排：自动加技能点（独立 FEATURE，不和 AP 混页）
    CharBoot,   // 第三排末：一键起号
    Count,
};

inline constexpr const char* kWorkspaceTabLabels[] = {
    "首页", "挂机时段", "技能多发", "遇人策略", "定时按键",
    "BUFF", "超级赶路", "实验", "启动", "调试",
    "吸怪 快攻", "自动卖装", "自动加点", "自动加技能点", "一键起号",
};
static_assert(sizeof(kWorkspaceTabLabels) / sizeof(kWorkspaceTabLabels[0]) ==
                  static_cast<int>(WorkspaceTab::Count),
              "tab label count mismatch");

void DrawWorkspaceTabContent(AppWindow& app, LaunchUiState& ui, int tabIndex);

// 部分 TAB 默认不出现在条上；调试页「指令」解锁后写入 HKCU，重启仍可见。
// 「卸载」清掉本机记录，TAB 立刻隐藏。不写 user.ini。
bool WorkspaceTabIsVisible(int tabIndex);
bool WorkspaceGatherTabUnlocked();

// Bump [core] manualRejoinSeq. requireInjected：无游戏进程则拒绝写盘。
// 成功返回 true；失败时 outErr 可填短因（可选）。
bool TriggerManualRejoin(const std::string& prefsBinDir, bool requireInjected,
                         std::string* outErr = nullptr);

}  // namespace xcat::app
