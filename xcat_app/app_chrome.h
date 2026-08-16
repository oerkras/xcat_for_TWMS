#pragma once

namespace xcat::app {

constexpr float kPanelDesignW = 448.f;
constexpr float kPanelDesignH = 700.f;

// launcher-only：仅启动/注入/状态灯，配置在游戏内 overlay（F9）。
// 高度需容纳：三行 TAB、首页多卡片、技能多发列表与底部退出；列表 TAB 用 fill 卡片吃满剩余高度。
constexpr float kLauncherOnlyDesignW = 380.f;
constexpr float kLauncherOnlyDesignH = 1016.f;

}  // namespace xcat::app
