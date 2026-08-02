#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <atomic>

inline constexpr UINT WM_XCAT_GRACEFUL_EXIT_DONE = WM_USER + 0x7C41;

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
    HWND webHost = nullptr;  // 静默 WebView2 宿主（隐藏，不占 ImGui 布局）
};

// 创建隐藏的 WebView2 父控件（不显示在 UI；验证码场景可另开调试显示）
void AppWindow_CreateSilentWebHost(AppWindow& app);

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
