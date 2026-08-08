#pragma once

#include <Windows.h>

#include <cstdint>

namespace x::features::auto_lie::anti_macro_follower {

void Init();
void SetEnabled(bool enabled);
void SetRegionOverlayEnabled(bool enabled);
bool IsRegionOverlayPref();  // 用户勾选偏好（模拟结束后恢复叠层用）
bool TryCopyPublishedPanelRect(RECT& out);  // 活计划青框 AABB → 桌面 RECT
void RefreshAutoLieHardPauseFromOutside();  // 模拟线程结束后重算硬闸
void Tick(DWORD now);
void Stop();
void Shutdown();

bool IsFollowing();
bool IsUiVisible();
bool IsRegionOverlayEnabled();
void SetQuizWorldPaused(bool paused); // 硬闸 = quiz|following|(ui&&!answerDone)|sim

}  // namespace x::features::auto_lie::anti_macro_follower
