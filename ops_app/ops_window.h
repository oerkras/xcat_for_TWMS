#pragma once

#include <Windows.h>
#include <d3d11.h>

struct OpsWindow {
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    bool running = true;
    float dpiScale = 1.f;
    int designW = 980;
    int designH = 720;
};

bool OpsWindow_Create(OpsWindow& app, HINSTANCE inst, int designW, int designH);
void OpsWindow_Destroy(OpsWindow& app);
void OpsWindow_Show(OpsWindow& app);
bool OpsWindow_IsMinimized(const OpsWindow& app);
void OpsWindow_BeginFrame(OpsWindow& app, const float clearColor[4]);
void OpsWindow_EndFrame(OpsWindow& app);
void OpsWindow_HandleResize(OpsWindow& app, UINT width, UINT height);
void OpsWindow_LoadFonts(OpsWindow& app);
void OpsWindow_RefreshDpi(OpsWindow& app, bool reloadFont);
LRESULT OpsWindow_WndProc(OpsWindow& app, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace xcat::ops {
const char* OpsThemeBinDir();
void OpsTheme_RequestCommit();
}  // namespace xcat::ops
