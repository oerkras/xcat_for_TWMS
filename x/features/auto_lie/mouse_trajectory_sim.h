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

// 解析测试面板桌面矩形：优先活 NonFinite 计划角点；否则客户区合成。
bool ResolveTestPanel(RECT& out, bool* live = nullptr);

// 泵线程脉冲（由 anti_macro_follower::LieFramePulse 调用）：若模拟在跑则夹光标并返回 true。
bool PulseCursorOnPump();
// 泵上强制松锁（模拟结束 Invoke）。
void ReleaseCursorOnPump();

}  // namespace x::features::auto_lie::mouse_trajectory_sim
