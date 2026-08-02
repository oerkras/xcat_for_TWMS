#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "titlebar_win.h"

namespace x::features::titlebar::win {
namespace {

struct FindCtx {
    DWORD pid = 0;
    HWND unity = nullptr;
    HWND fallback = nullptr;
};

BOOL CALLBACK EnumCb(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindCtx*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd)) return TRUE;

    char className[64]{};
    GetClassNameA(hwnd, className, sizeof(className));
    if (_stricmp(className, "UnityWndClass") == 0) {
        ctx->unity = hwnd;
        return FALSE;
    }
    if (!ctx->fallback) {
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        if ((rect.right - rect.left) > 200 && (rect.bottom - rect.top) > 200) {
            ctx->fallback = hwnd;
        }
    }
    return TRUE;
}

}  // namespace

HWND FindGameWindow() {
    FindCtx ctx{GetCurrentProcessId(), nullptr, nullptr};
    EnumWindows(EnumCb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.unity ? ctx.unity : ctx.fallback;
}

HWND FindUnityWndClass() {
    FindCtx ctx{GetCurrentProcessId(), nullptr, nullptr};
    EnumWindows(EnumCb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.unity;  // 不要 fallback：冷启 splash 大窗 ≠ Unity 就绪
}

void SetTitleSafe(HWND hwnd, const wchar_t* text, UINT timeoutMs) {
    if (!hwnd || !IsWindow(hwnd) || !text) return;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text),
                        SMTO_ABORTIFHUNG | SMTO_NORMAL, timeoutMs, &result);
}

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    return result;
}

}  // namespace x::features::titlebar::win
