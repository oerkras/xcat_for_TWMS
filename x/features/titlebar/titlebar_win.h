#pragma once

#include <Windows.h>

#include <string>

namespace x::features::titlebar::win {

HWND FindGameWindow();
// 仅 UnityWndClass（无大窗 fallback）——冷启 GC 门闩用。
HWND FindUnityWndClass();
// 贴到当前显示器工作区左上（margin≈12px）。由调用方保证只贴有限次，避免抢用户拖拽。
bool PositionGameTopLeft(HWND hwnd);
void SetTitleSafe(HWND hwnd, const wchar_t* text, UINT timeoutMs);
std::wstring Utf8ToWide(const char* text);

}  // namespace x::features::titlebar::win
