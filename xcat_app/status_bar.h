#pragma once

#include "launch_panel.h"
#include "runtime_leds.h"

#include <cstdint>

namespace xcat::app {

// 标题栏下方状态条：固定 5 行（版本 | 运行/阶段/PID | 守护/在线 | 定时软重连 | 状态/更新）
void DrawLauncherStatusBar(LaunchUiState& ui, const RuntimeLeds& leds, uint64_t launchTickMs);

}  // namespace xcat::app
