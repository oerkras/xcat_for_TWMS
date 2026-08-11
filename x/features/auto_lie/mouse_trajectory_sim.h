#pragma once

#include <cstdint>

#include <Windows.h>

namespace x::features::auto_lie::mouse_trajectory_sim {

// 内置 UV 轨迹按原生时序重放：165 ready + 330 pts @33Hz。
// 实战同款：由 LieFrameTick（泵线程）ClipCursor(1×1)+SetCursorPos；worker 只喂目标点。
// 无真实题目时：按当前游戏客户区缩放 dump 面板位置。
void Init();
void RequestStart(uint32_t seq);
// 请求停止（真题抢占）：置 stop、让出泵槽；线程自行 finish。
void RequestStop(const char* why = nullptr);
bool IsRunning();
void Shutdown();

// 面板四角（桌面坐标）；0=BL 1=BR 2=TR 3=TL，与 anti_macro_port::ResolvePanelGeometry 同序。
// 真面板未必轴对齐，AABB 会把倾斜抹平，故映射走四角而非 RECT。
struct PanelQuad {
    POINT c[4]{};
};

enum class PanelSource {
    Synth = 0,   // 按当前客户区比例缩放 fixture 里的面板常量
    Replay = 1,  // 回放本机真题证据（state\lie_events\mouse_*\meta.txt）里的仿射四角
    Live = 2,    // 当前活 NonFinite 计划的真面板
};
const char* PanelSourceTag(PanelSource src);

// 解析测试面板：优先级 Live > Replay > Synth。
bool ResolveTestPanelQuad(PanelQuad& out, PanelSource* src = nullptr);
// AABB 壳（overlay 等只要外接矩形的调用方）；live 仅在 Live 档为真。
bool ResolveTestPanel(RECT& out, bool* live = nullptr);

// 泵线程脉冲（由 anti_macro_follower::LieFramePulse 调用）：若模拟在跑则夹光标并返回 true。
bool PulseCursorOnPump();
// 泵上强制松锁（模拟结束 Invoke）。
void ReleaseCursorOnPump();

}  // namespace x::features::auto_lie::mouse_trajectory_sim
