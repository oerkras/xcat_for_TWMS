#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "imgui.h"

namespace xcat::ui {

inline bool IsWin32ImeTextMessage(UINT msg) {
    return msg == WM_IME_CHAR || msg == WM_UNICHAR;
}

inline bool IsWin32ImeRouteMessage(UINT msg) {
    return msg == WM_IME_SETCONTEXT || msg == WM_IME_STARTCOMPOSITION ||
           msg == WM_IME_COMPOSITION || msg == WM_IME_ENDCOMPOSITION || msg == WM_IME_NOTIFY ||
           msg == WM_IME_CONTROL || msg == WM_IME_REQUEST || msg == WM_IME_CHAR ||
           msg == WM_UNICHAR;
}

inline UINT Win32KeyboardCodePage() {
    const HKL hkl = ::GetKeyboardLayout(0);
    const LCID lcid = MAKELCID(HIWORD(hkl), SORT_DEFAULT);
    UINT codePage = CP_ACP;
    ::GetLocaleInfoA(lcid, LOCALE_RETURN_NUMBER | LOCALE_IDEFAULTANSICODEPAGE,
                     reinterpret_cast<LPSTR>(&codePage), sizeof(codePage));
    return codePage != 0 ? codePage : CP_ACP;
}

inline bool HandleOfficialWin32ImeMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          LRESULT& outResult) {
    switch (msg) {
    case WM_IME_COMPOSITION: {
        // Backport of Dear ImGui Win32 backend IME handling after v1.91.8:
        // let Windows produce the correct WM_IME_CHAR value, especially for MBCS paths.
        const LRESULT result = ::DefWindowProcW(hwnd, msg, wp, lp);
        outResult = (lp & GCS_RESULTSTR) ? 1 : result;
        return true;
    }

    case WM_IME_CHAR:
        if (::IsWindowUnicode(hwnd) == FALSE) {
            if (::IsDBCSLeadByte(HIBYTE(wp)))
                wp = static_cast<WPARAM>(MAKEWORD(HIBYTE(wp), LOBYTE(wp)));
            wchar_t wch = 0;
            ::MultiByteToWideChar(Win32KeyboardCodePage(), MB_PRECOMPOSED,
                                  reinterpret_cast<char*>(&wp), 2, &wch, 1);
            ImGui::GetIO().AddInputCharacterUTF16(static_cast<unsigned short>(wch));
            outResult = 1;
            return true;
        }
        return false;

    case WM_UNICHAR:
        if (wp == UNICODE_NOCHAR) {
            outResult = 1;
            return true;
        }
        if (wp > 0 && wp <= 0x10FFFF) {
            ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(wp));
            outResult = 1;
            return true;
        }
        return false;

    default:
        return false;
    }
}

}  // namespace xcat::ui
