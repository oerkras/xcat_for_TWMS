#include "ops_window.h"
#include "resource.h"

#include "xcat_imgui_cjk_font.h"
#include "xcat_imgui_theme.h"

#include "../common/process_util.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

OpsWindow* g_opsWindow = nullptr;
ImGuiStyle g_styleBase{};
bool g_hasStyleBase = false;
bool g_pendingThemeCommit = false;
bool g_frameHadInput = true;
std::string g_opsThemeBinDir;

void ApplyDpiStyle(float scale);

std::string ResolveOpsThemeBinDir() {
    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    // OPS 主题独立落盘：bin_ops\state\user.ini，不读写客户端 bin\XCat_data。
    return xcat::WideToUtf8(xcat::ParentDirWithSlash(std::wstring(exe, exe + n)));
}

void OpsRecaptureStyleBase() {
    g_styleBase = ImGui::GetStyle();
    g_hasStyleBase = true;
}

void OpsThemeCommit(OpsWindow& app) {
    xcat::ui::UiTheme_Apply();
    OpsRecaptureStyleBase();
    ApplyDpiStyle(app.dpiScale);
    xcat::ui::UiTheme_RefreshDwm(app.hwnd);
}

void OpsThemeRequestCommit() { g_pendingThemeCommit = true; }

bool CreateRenderTarget(OpsWindow& app) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    const HRESULT hr = app.device->CreateRenderTargetView(backBuffer, nullptr, &app.renderTarget);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

void CleanupRenderTarget(OpsWindow& app) {
    if (app.renderTarget) {
        app.renderTarget->Release();
        app.renderTarget = nullptr;
    }
}

void CleanupDevice(OpsWindow& app) {
    CleanupRenderTarget(app);
    if (app.swapChain) {
        app.swapChain->Release();
        app.swapChain = nullptr;
    }
    if (app.context) {
        app.context->Release();
        app.context = nullptr;
    }
    if (app.device) {
        app.device->Release();
        app.device = nullptr;
    }
}

bool TryCreateDevice(OpsWindow& app, HWND hwnd, D3D_DRIVER_TYPE driverType,
                     D3D_FEATURE_LEVEL& selectedLevel, HRESULT& failureHr) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT width = std::max<UINT>(1u, static_cast<UINT>(client.right - client.left));
    const UINT height = std::max<UINT>(1u, static_cast<UINT>(client.bottom - client.top));

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    failureHr = D3D11CreateDeviceAndSwapChain(
        nullptr, driverType, nullptr, 0, levels, _countof(levels), D3D11_SDK_VERSION, &sd,
        &app.swapChain, &app.device, &selectedLevel, &app.context);
    if (FAILED(failureHr)) {
        CleanupDevice(app);
        return false;
    }
    if (!CreateRenderTarget(app)) {
        failureHr = E_FAIL;
        CleanupDevice(app);
        return false;
    }

    constexpr float probeColor[4] = {0.04f, 0.045f, 0.06f, 1.0f};
    app.context->OMSetRenderTargets(1, &app.renderTarget, nullptr);
    app.context->ClearRenderTargetView(app.renderTarget, probeColor);
    failureHr = app.swapChain->Present(0, 0);
    if (FAILED(failureHr)) {
        CleanupDevice(app);
        return false;
    }
    return true;
}

bool CreateDevice(OpsWindow& app, HWND hwnd) {
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hardwareHr = E_FAIL;
    if (TryCreateDevice(app, hwnd, D3D_DRIVER_TYPE_HARDWARE, featureLevel, hardwareHr)) {
        return true;
    }

    OutputDebugStringW(L"XCat TWMS Ops: hardware D3D11 failed; falling back to WARP.\n");
    HRESULT warpHr = E_FAIL;
    return TryCreateDevice(app, hwnd, D3D_DRIVER_TYPE_WARP, featureLevel, warpHr);
}

void ApplyDpiStyle(float scale) {
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.f) scale = 3.f;
    if (!g_hasStyleBase) {
        g_styleBase = ImGui::GetStyle();
        g_hasStyleBase = true;
    }
    ImGui::GetStyle() = g_styleBase;
    ImGui::GetStyle().ScaleAllSizes(scale);
}

void ResizeOuterToDesign(OpsWindow& app) {
    if (!app.hwnd) return;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(app.hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE));
    const int clientW = static_cast<int>(app.designW * app.dpiScale + 0.5f);
    const int clientH = static_cast<int>(app.designH * app.dpiScale + 0.5f);
    RECT rc{0, 0, clientW, clientH};
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    SetWindowPos(app.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CenterWindowOnWorkArea(HWND hwnd) {
    if (!hwnd) return;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return;
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& work = mi.rcWork;
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK OpsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    if (!g_opsWindow) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return OpsWindow_WndProc(*g_opsWindow, hwnd, msg, wParam, lParam);
}

}  // namespace

void OpsWindow_LoadFonts(OpsWindow& app) {
    float dpiScale = app.dpiScale;
    if (dpiScale < 0.75f) dpiScale = 0.75f;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    xcat::ui::PrepareCjkFontAtlas(io.Fonts, dpiScale);

    // Integer pixel size avoids blurry glyphs under fractional DPI.
    const float fontSize =
        (16.0f * dpiScale < 12.f) ? 12.f : std::floorf(16.0f * dpiScale + 0.5f);

    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.0f;
    cfg.FontNo = 0;

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);
    static const char* kCandidates[] = {
        "%s\\Fonts\\msyh.ttc",
        "%s\\Fonts\\msyhbd.ttc",
        "%s\\Fonts\\simhei.ttf",
        "%s\\Fonts\\simsun.ttc",
        "%s\\Fonts\\mingliu.ttc",
        "%s\\Fonts\\msjh.ttc",
        "%s\\Fonts\\msjhbd.ttc",
    };
    const ImWchar* ranges = xcat::ui::CjkGuiGlyphRanges(io.Fonts);
    bool loaded = false;
    char fontPath[MAX_PATH]{};
    for (const char* fmt : kCandidates) {
        std::snprintf(fontPath, sizeof(fontPath), fmt, winDir);
        if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES) continue;
        if (io.Fonts->AddFontFromFileTTF(fontPath, fontSize, &cfg, ranges)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) io.Fonts->AddFontDefault(&cfg);
    else (void)xcat::ui::MergeUiSymbolFallbackFont(io.Fonts, fontSize);

    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}

void OpsWindow_RefreshDpi(OpsWindow& app, bool reloadFont) {
    const float newScale = ImGui_ImplWin32_GetDpiScaleForHwnd(app.hwnd);
    if (newScale == app.dpiScale && !reloadFont) return;
    app.dpiScale = newScale;
    if (app.dpiScale < 0.75f) app.dpiScale = 0.75f;
    ApplyDpiStyle(app.dpiScale);
    if (reloadFont) OpsWindow_LoadFonts(app);
}

bool OpsWindow_Create(OpsWindow& app, HINSTANCE inst, int designW, int designH) {
    g_opsWindow = &app;
    app.designW = designW;
    app.designH = designH;

    // Critical: without this, Windows bitmap-scales the window and fonts look blurry.
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = OpsWndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_OPS_APP_ICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        inst, MAKEINTRESOURCEW(IDI_OPS_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"XCatTwmsOpsWindow";
    RegisterClassExW(&wc);

    // Create at design size first; real DPI is known only after HWND exists.
    app.hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, wc.lpszClassName, L"XCat TWMS Ops (:18789)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, designW, designH,
        nullptr, nullptr, inst, nullptr);
    if (!app.hwnd) return false;

    app.dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(app.hwnd);
    if (app.dpiScale < 0.75f) app.dpiScale = 0.75f;
    ResizeOuterToDesign(app);
    CenterWindowOnWorkArea(app.hwnd);

    if (!CreateDevice(app, app.hwnd)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    g_opsThemeBinDir = ResolveOpsThemeBinDir();
    xcat::ui::UiTheme_Load(g_opsThemeBinDir.c_str());
    g_hasStyleBase = false;
    OpsThemeCommit(app);

    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(app.device, app.context);
    xcat::ui::UiTheme_CreateGrainTextureDX11(app.device);
    OpsWindow_LoadFonts(app);
    return true;
}

void OpsWindow_Destroy(OpsWindow& app) {
    xcat::ui::UiTheme_DestroyGrainTextureDX11();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDevice(app);
    if (app.hwnd) {
        DestroyWindow(app.hwnd);
        app.hwnd = nullptr;
    }
    g_opsWindow = nullptr;
    g_hasStyleBase = false;
}

void OpsWindow_Show(OpsWindow& app) {
    ShowWindow(app.hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(app.hwnd);
}

bool OpsWindow_IsMinimized(const OpsWindow& app) {
    return app.hwnd && IsIconic(app.hwnd);
}

void OpsWindow_BeginFrame(OpsWindow& app, const float clearColor[4]) {
    if (g_pendingThemeCommit) {
        g_pendingThemeCommit = false;
        OpsThemeCommit(app);
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    float clear[4] = {clearColor[0], clearColor[1], clearColor[2], clearColor[3]};
    const ImVec4& bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    clear[0] = bg.x;
    clear[1] = bg.y;
    clear[2] = bg.z;
    clear[3] = 1.f;

    app.context->OMSetRenderTargets(1, &app.renderTarget, nullptr);
    app.context->ClearRenderTargetView(app.renderTarget, clear);
}

void OpsWindow_EndFrame(OpsWindow& app) {
    ImGuiIO& io = ImGui::GetIO();
    g_frameHadInput = io.WantTextInput || ImGui::IsAnyItemActive() || io.MouseWheel != 0.f ||
                      io.MouseWheelH != 0.f || io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f ||
                      io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = app.swapChain->Present(1, 0);
    if (FAILED(hr)) {
        OutputDebugStringW(L"XCat TWMS Ops: D3D11 Present failed.\n");
        app.running = false;
    }
}

bool OpsWindow_FrameHadInput() {
    return g_frameHadInput;
}

void OpsWindow_HandleResize(OpsWindow& app, UINT width, UINT height) {
    if (!app.swapChain || width == 0 || height == 0) return;
    CleanupRenderTarget(app);
    const HRESULT hr = app.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr) || !CreateRenderTarget(app)) {
        OutputDebugStringW(L"XCat TWMS Ops: D3D11 resize failed.\n");
        app.running = false;
    }
}

LRESULT OpsWindow_WndProc(OpsWindow& app, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        OpsWindow_RefreshDpi(app, true);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam &&
            (_wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0 ||
             _wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"AppsUseLightTheme") == 0)) {
            if (xcat::ui::UiTheme_Preference() == xcat::ui::ThemePreference::System) {
                OpsThemeRequestCommit();
            }
        }
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            OpsWindow_HandleResize(app, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_DESTROY:
        app.running = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 供 ops_panel 调用：改主题并请求下一帧 Commit。
namespace xcat::ops {
const char* OpsThemeBinDir() { return g_opsThemeBinDir.c_str(); }
void OpsTheme_RequestCommit() { OpsThemeRequestCommit(); }
}  // namespace xcat::ops
