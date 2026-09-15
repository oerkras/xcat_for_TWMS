#pragma once

#include "launch_panel.h"
#include "runtime_leds.h"

#include <cstdint>
#include <string>

namespace xcat::app {

// 签卡 uid 水印文案（「UID xxx」；无卡为「UID -」）。约 4s 刷新，给状态条截图追溯。
const char* LauncherCachedGateUidLabel(const std::string& prefsBinDir);

// 标题栏下方状态条：固定 5 行（版本/UID | 运行/阶段/PID | 守护/在线 | 主动软重连 | 状态/更新）
void DrawLauncherStatusBar(LaunchUiState& ui, const RuntimeLeds& leds, uint64_t launchTickMs);

}  // namespace xcat::app
