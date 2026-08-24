#pragma once

// Classic TWMS：实验 TAB 一次打开游戏内 UICheat IMGUI GM 台。
// 主泵调官方 CreateInstance + UIWindow.Open + SetActive；
// Unity 自己每帧调 OnGUI()。禁止在 worker / 泵上直调 OnGUI。
// 韩文 caption：仅在打开时 Rel5 拦 GUIContent.Temp（GA .text E9+近桩，steal 9）。
// 不开 overlay 不 patch。Shutdown 还原。

namespace x::features::ui_cheat_overlay {

void Init();
void Shutdown();

// IPC 脉冲：开短线程 → InvokeAndWait 主泵跑一次。忙则拒绝。
void RequestOpen();

}  // namespace x::features::ui_cheat_overlay
