#pragma once

#include <Windows.h>

#include <string>

namespace x::features::titlebar::win {

HWND FindGameWindow();
// 仅 UnityWndClass（无大窗 fallback）——冷启 GC 门闩用。
HWND FindUnityWndClass();
void SetTitleSafe(HWND hwnd, const wchar_t* text, UINT timeoutMs);
std::wstring Utf8ToWide(const char* text);

}  // namespace x::features::titlebar::win
