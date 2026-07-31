#pragma once

#include "launch_panel.h"
#include "runtime_leds.h"

namespace xcat::app {

// 标题栏下方状态条：服名 / 版本 / WebView·阶段 / 启动 / 检查更新
void DrawLauncherStatusBar(LaunchUiState& ui, const RuntimeLeds& leds);

}  // namespace xcat::app
