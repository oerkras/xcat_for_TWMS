#pragma once

#include "launch_panel.h"
#include "runtime_leds.h"

#include <cstdint>

namespace xcat::app {

// 标题栏下方状态条：固定 4 行（服名 | 运行/阶段/PID | 守护/在线 | 状态/更新）
void DrawLauncherStatusBar(LaunchUiState& ui, const RuntimeLeds& leds, uint64_t launchTickMs);

}  // namespace xcat::app
