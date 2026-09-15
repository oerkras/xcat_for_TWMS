#pragma once

#include <Windows.h>

#include <cstdint>

namespace x::features::auto_lie::anti_macro_follower {

void Init();
void SetEnabled(bool enabled);
void SetRegionOverlayEnabled(bool enabled);
bool IsRegionOverlayPref();  // 用户勾选偏好（模拟结束后恢复叠层用）
bool TryCopyPublishedPanelRect(RECT& out);  // 活计划青框 AABB → 桌面 RECT
// 活计划真面板四角（BL,BR,TR,TL）；仅在计划带仿射四角时成立，不做 AABB 退化。
bool TryCopyPublishedPanelCorners(POINT out4[4]);
void RefreshAutoLieHardPauseFromOutside();  // 模拟线程结束后重算硬闸
void Tick(DWORD now);
void Stop();
void Shutdown();

bool IsFollowing();
bool IsUiVisible();
bool IsRegionOverlayEnabled();
// 同图 InterStage 卸 Field 时测谎单例会暂时 Instantiated=false。按住计划/硬闸期间为真，
// 供 stuck_lobby / IsQuizActive 识别：这不是关题，禁止当成大厅去拆会话。
bool IsHoldingFieldTransit();
void SetQuizWorldPaused(bool paused); // 硬闸 = quiz|following|(ui&&!answerDone)|sim

}  // namespace x::features::auto_lie::anti_macro_follower
