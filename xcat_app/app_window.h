#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <atomic>

inline constexpr UINT WM_XCAT_GRACEFUL_EXIT_DONE = WM_USER + 0x7C41;
// 门禁退出：工作线程 PostMessage → 主窗 WndProc 弹「网络错误 (2|3)」并 ExitProcess。
inline constexpr UINT WM_XCAT_ACCESS_GATE = WM_USER + 0x7C42;

struct AppWindow {
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    bool running = true;
    bool exiting = false;
    ULONGLONG exitStartedTick = 0;
    std::atomic<bool> pendingExitAfterSound{false};
    float dpiScale = 1.f;
    ULONGLONG launchTickMs = 0;   // GetTickCount64() at startup
    char  runTimeText[32]{};      // "运行 HH:MM:SS"（每帧刷新）
    std::atomic<bool> hotkeyF7{false};  // F7 全局热键置位（WndProc 收 WM_HOTKEY → main 循环消费切换超级赶路）
    std::atomic<bool> hotkeyF10{false}; // F10 随机换频（与挂机卡按钮同路径）
    int titleBarHeightPx = 32;
    int captionButtonsWidthPx = 80;
};

bool AppWindow_Create(AppWindow& app, HINSTANCE inst, float designW, float designH);
void AppWindow_Destroy(AppWindow& app);
void AppWindow_Show(AppWindow& app);
void AppWindow_BeginFrame(AppWindow& app, const float clearColor[4]);
void AppWindow_GetClearColor(float out[4]);
void AppWindow_EndFrame(AppWindow& app);
void AppWindow_UpdateChromeMetrics(AppWindow& app);
void AppWindow_DragFromLastItem(AppWindow& app);
void AppWindow_HandleResize(AppWindow& app, UINT width, UINT height);
void AppWindow_ToggleMinimized(AppWindow& app);
void AppWindow_Minimize(AppWindow& app);
void AppWindow_Restore(AppWindow& app);
bool AppWindow_IsMinimized(const AppWindow& app);
void AppWindow_RefreshDpi(AppWindow& app, bool reloadFont);
LRESULT AppWindow_WndProc(AppWindow& app, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
