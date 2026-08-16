#include "app_window.h"
#include "app_dpi.h"
#include "app_font.h"
#include "app_theme.h"
#include "resource.h"
#include "update_client.h"

#include "../common/xcat_log.h"
#include "../common/process_util.h"

#include "xcat_imgui_win32_ime.h"
#include "xcat_imgui_theme.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <tchar.h>

#include <algorithm>
#include <string>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

constexpr DWORD kLauncherFrameStyle = WS_OVERLAPPED | WS_THICKFRAME;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr int kHotkeyToggleGui = 1;
constexpr int kHotkeyManualRejoin = 2;

void ApplyTopmostZOrder(AppWindow& app) {
    if (!app.hwnd) return;
    SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool CreateRenderTarget(AppWindow& app) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    const HRESULT hr = app.device->CreateRenderTargetView(backBuffer, nullptr, &app.renderTarget);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

void CleanupRenderTarget(AppWindow& app) {
    if (app.renderTarget) {
        app.renderTarget->Release();
        app.renderTarget = nullptr;
    }
}

void CleanupDevice(AppWindow& app) {
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

bool TryCreateDevice(AppWindow& app, HWND hwnd, D3D_DRIVER_TYPE driverType,
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

    // Some virtual display drivers create a D3D11 device successfully but fail on the first
    // real Present. Probe before showing the window so we can transparently fall back to WARP.
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

bool CreateDevice(AppWindow& app, HWND hwnd) {
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hardwareHr = E_FAIL;
    if (TryCreateDevice(app, hwnd, D3D_DRIVER_TYPE_HARDWARE, featureLevel, hardwareHr)) {
        xcat::log::Info("App", "D3D11 renderer=hardware featureLevel=0x%04X",
                        static_cast<unsigned>(featureLevel));
        return true;
    }

    xcat::log::Warn("App", "D3D11 hardware renderer unavailable hr=0x%08lX，切换 WARP",
                    static_cast<unsigned long>(hardwareHr));
    HRESULT warpHr = E_FAIL;
    if (TryCreateDevice(app, hwnd, D3D_DRIVER_TYPE_WARP, featureLevel, warpHr)) {
        xcat::log::Info("App", "D3D11 renderer=WARP featureLevel=0x%04X",
                        static_cast<unsigned>(featureLevel));
        return true;
    }
    xcat::log::Error("App", "D3D11 WARP renderer failed hr=0x%08lX",
                     static_cast<unsigned long>(warpHr));
    return false;
}

void GetFrameBorderMetrics(int& borderLR, int& borderTB) {
    const int padding = GetSystemMetrics(SM_CXPADDEDBORDER);
    borderLR = GetSystemMetrics(SM_CXFRAME) + padding;
    borderTB = GetSystemMetrics(SM_CYFRAME) + padding;
}

void EnableBorderlessFrame(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~WS_CAPTION;
    style |= kLauncherFrameStyle;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    const xcat::app::AppPalette& p = xcat::app::AppTheme_Palette();
    BOOL dark = p.immersiveDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const int corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    // 与 ImGui WindowBg 对齐，避免 Win11 在客户区外缘渲染错色 DWM 边框。
    const COLORREF borderColor = RGB(p.borderR, p.borderG, p.borderB);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
}

void RefreshWindowFrame(HWND hwnd) {
    const xcat::app::AppPalette& p = xcat::app::AppTheme_Palette();
    BOOL dark = p.immersiveDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF borderColor = RGB(p.borderR, p.borderG, p.borderB);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void PositionWindowTopRight(HWND hwnd, int clientW, int clientH, float dpiScale) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT rc{0, 0, clientW, clientH};
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int outerW = rc.right - rc.left;
    int outerH = rc.bottom - rc.top;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int margin = static_cast<int>(12.f * dpiScale + 0.5f);
    const int maxOuterH =
        (std::max)(1, static_cast<int>((work.bottom - work.top) - margin * 2));
    if (outerH > maxOuterH) outerH = maxOuterH;
    const int x = work.right - outerW - margin;
    const int y = work.top + margin;
    SetWindowPos(hwnd, nullptr, x, y, outerW, outerH, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool RectContainsPoint(const RECT& rect, POINT pt) {
    return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top && pt.y < rect.bottom;
}

LRESULT HandleNcCalcSize(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    if (!wParam) return DefWindowProcW(hwnd, WM_NCCALCSIZE, wParam, lParam);
    if (!IsZoomed(hwnd)) return 0;

    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
    int borderLR = 0;
    int borderTB = 0;
    GetFrameBorderMetrics(borderLR, borderTB);

    RECT& r = params->rgrc[0];
    r.left += borderLR;
    r.right -= borderLR;
    r.top += borderTB;
    r.bottom -= borderTB;
    return 0;
}

LRESULT HandleNcHitTest(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &pt);

    RECT client{};
    GetClientRect(hwnd, &client);
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return HTCLIENT;

    int borderLR = 0;
    int borderTB = 0;
    GetFrameBorderMetrics(borderLR, borderTB);

    RECT topEdge = client;
    topEdge.bottom = client.top + borderTB;

    RECT topLeft = client;
    topLeft.right = topLeft.left + borderLR;
    topLeft.left -= borderLR;
    topLeft.bottom = topLeft.top + borderTB;

    RECT topRight = client;
    topRight.left = topRight.right - borderLR;
    topRight.right += borderLR;
    topRight.bottom = topRight.top + borderTB;

    if (RectContainsPoint(topLeft, pt)) return HTTOPLEFT;
    if (RectContainsPoint(topRight, pt)) return HTTOPRIGHT;
    if (RectContainsPoint(topEdge, pt)) return HTTOP;

    RECT leftEdge = client;
    leftEdge.right = client.left + borderLR;

    RECT rightEdge = client;
    rightEdge.left = client.right - borderLR;

    if (RectContainsPoint(leftEdge, pt)) return HTLEFT;
    if (RectContainsPoint(rightEdge, pt)) return HTRIGHT;

    RECT bottomEdge = client;
    bottomEdge.top = client.bottom - borderTB;

    RECT bottomLeft = client;
    bottomLeft.right = bottomLeft.left + borderLR;
    bottomLeft.left -= borderLR;
    bottomLeft.top = bottomLeft.bottom - borderTB;

    RECT bottomRight = client;
    bottomRight.left = bottomRight.right - borderLR;
    bottomRight.right += borderLR;
    bottomRight.top = bottomRight.bottom - borderTB;

    if (RectContainsPoint(bottomLeft, pt)) return HTBOTTOMLEFT;
    if (RectContainsPoint(bottomRight, pt)) return HTBOTTOMRIGHT;
    if (RectContainsPoint(bottomEdge, pt)) return HTBOTTOM;

    return HTCLIENT;
}

}  // namespace

bool AppWindow_Create(AppWindow& app, HINSTANCE inst, float designW, float designH) {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) return AppWindow_WndProc(*self, hwnd, msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = L"XCatAppWindow";
    RegisterClassExW(&wc);

    const int initW = static_cast<int>(designW + 0.5f);
    const int initH = static_cast<int>(designH + 0.5f);

    app.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"XCat", kLauncherFrameStyle,
                               CW_USEDEFAULT, CW_USEDEFAULT, initW, initH, nullptr, nullptr, inst,
                               nullptr);
    if (!app.hwnd) return false;

    SetWindowLongPtrW(app.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    EnableBorderlessFrame(app.hwnd);

    if (!RegisterHotKey(app.hwnd, kHotkeyToggleGui, MOD_NOREPEAT, VK_F9)) {
        xcat::log::Warn("App", "RegisterHotKey F9 失败（可能已被占用），最小化热键不可用");
    }
    if (!RegisterHotKey(app.hwnd, kHotkeyManualRejoin, MOD_NOREPEAT, VK_F10)) {
        xcat::log::Warn("App", "RegisterHotKey F10 失败（可能已被占用），随机换频热键不可用");
    }

    app.dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(app.hwnd);
    const int scaledW = static_cast<int>(designW * app.dpiScale + 0.5f);
    const int scaledH = static_cast<int>(designH * app.dpiScale + 0.5f);
    PositionWindowTopRight(app.hwnd, scaledW, scaledH, app.dpiScale);
    RefreshWindowFrame(app.hwnd);

    ApplyTopmostZOrder(app);

    if (!CreateDevice(app, app.hwnd)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath;
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    const size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir.resize(slash + 1);
    iniPath = xcat::WideToUtf8(exeDir) + "xcat_imgui.ini";
    io.IniFilename = iniPath.c_str();

    xcat::app::AppTheme_Apply();
    AppDpi_Init(app.hwnd);
    app.dpiScale = AppDpi_Scale();
    RefreshWindowFrame(app.hwnd);

    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(app.device, app.context);
    xcat::ui::UiTheme_CreateGrainTextureDX11(app.device);
    AppFont_Load(app.dpiScale);
    AppWindow_UpdateChromeMetrics(app);
    return true;
}

void AppWindow_Show(AppWindow& app) {
    if (!app.hwnd || IsWindowVisible(app.hwnd)) return;
    ShowWindow(app.hwnd, SW_SHOWNA);
    UpdateWindow(app.hwnd);
    ApplyTopmostZOrder(app);
}

void AppWindow_Destroy(AppWindow& app) {
    if (app.hwnd) {
        UnregisterHotKey(app.hwnd, kHotkeyToggleGui);
        UnregisterHotKey(app.hwnd, kHotkeyManualRejoin);
    }

    xcat::ui::UiTheme_DestroyGrainTextureDX11();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDevice(app);
    if (app.hwnd && IsWindow(app.hwnd)) DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    UnregisterClassW(L"XCatAppWindow", GetModuleHandleW(nullptr));
}

void AppWindow_GetClearColor(float out[4]) {
    if (!out) return;
    const ImVec4& c = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    out[0] = c.x;
    out[1] = c.y;
    out[2] = c.z;
    out[3] = 1.0f;
}

void AppWindow_BeginFrame(AppWindow& app, const float clearColor[4]) {
    xcat::app::AppTheme_PumpPending();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    AppWindow_UpdateChromeMetrics(app);

    // 主题可能刚在 PumpPending 里改了 WindowBg；调用方传入的 clearColor 可能是上一帧的。
    float clear[4] = {clearColor[0], clearColor[1], clearColor[2], clearColor[3]};
    AppWindow_GetClearColor(clear);

    app.context->OMSetRenderTargets(1, &app.renderTarget, nullptr);
    app.context->ClearRenderTargetView(app.renderTarget, clear);
}

void AppWindow_UpdateChromeMetrics(AppWindow& app) {
    if (!ImGui::GetCurrentContext()) return;
    const float titleH = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;
    app.titleBarHeightPx = static_cast<int>(titleH + 0.5f);
}

void AppWindow_DragFromLastItem(AppWindow& app) {
    if (!app.hwnd) return;

    // 顶栏可有多段 drag item（品牌区 / 中间空白）。必须按 ItemID 归属，
    // 否则后一段的 DragFromLastItem 会把前一段正在进行的拖拽清掉。
    struct DragState {
        bool active = false;
        ImGuiID ownerId = 0;
        POINT mouseStart{};
        POINT winStart{};
    };
    static DragState drag;

    const ImGuiID id = ImGui::GetItemID();
    if (id == 0) return;

    if (ImGui::IsItemActivated()) {
        if (!GetCursorPos(&drag.mouseStart)) return;
        RECT rect{};
        if (!GetWindowRect(app.hwnd, &rect)) return;
        drag.winStart.x = rect.left;
        drag.winStart.y = rect.top;
        drag.ownerId = id;
        drag.active = true;
    }

    if (!drag.active || drag.ownerId != id) return;

    if (!ImGui::IsItemActive() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        drag.active = false;
        drag.ownerId = 0;
        return;
    }

    POINT mouse{};
    if (!GetCursorPos(&mouse)) return;
    const int x = drag.winStart.x + (mouse.x - drag.mouseStart.x);
    const int y = drag.winStart.y + (mouse.y - drag.mouseStart.y);
    SetWindowPos(app.hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 启动器不要 Tooltip 浮层（挡控件）。调用点可留着当注释；这里统一不画。
static void SuppressImGuiTooltips() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return;
    for (ImGuiWindow* w : ctx->Windows) {
        if (w && (w->Flags & ImGuiWindowFlags_Tooltip)) w->Hidden = true;
    }
}

void AppWindow_EndFrame(AppWindow& app) {
    SuppressImGuiTooltips();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = app.swapChain->Present(1, 0);
    if (FAILED(hr)) {
        const HRESULT reason = app.device ? app.device->GetDeviceRemovedReason() : E_FAIL;
        xcat::log::Error("App", "D3D11 Present failed hr=0x%08lX removedReason=0x%08lX",
                         static_cast<unsigned long>(hr), static_cast<unsigned long>(reason));
        app.running = false;
    }
}

void AppWindow_HandleResize(AppWindow& app, UINT width, UINT height) {
    if (!app.device || width == 0 || height == 0) return;
    CleanupRenderTarget(app);
    const HRESULT hr = app.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr) || !CreateRenderTarget(app)) {
        xcat::log::Error("App", "D3D11 resize failed hr=0x%08lX size=%ux%u",
                         static_cast<unsigned long>(hr), width, height);
        app.running = false;
    }
}

void AppWindow_ToggleMinimized(AppWindow& app) {
    if (!app.hwnd) return;
    if (IsIconic(app.hwnd)) {
        ShowWindow(app.hwnd, SW_RESTORE);
        SetForegroundWindow(app.hwnd);
        ApplyTopmostZOrder(app);
        xcat::log::Info("App", "窗口还原 (F9)");
        return;
    }
    AppWindow_Minimize(app);
}

void AppWindow_Minimize(AppWindow& app) {
    if (!app.hwnd) return;
    ShowWindow(app.hwnd, SW_MINIMIZE);
    xcat::log::Info("App", "窗口最小化到任务栏");
}

void AppWindow_Restore(AppWindow& app) {
    if (!app.hwnd) return;
    if (IsIconic(app.hwnd)) {
        ShowWindow(app.hwnd, SW_RESTORE);
        xcat::log::Info("App", "窗口还原（更新进度可见）");
    }
    SetForegroundWindow(app.hwnd);
    ApplyTopmostZOrder(app);
}

bool AppWindow_IsMinimized(const AppWindow& app) {
    return app.hwnd && IsIconic(app.hwnd);
}

void AppWindow_RefreshDpi(AppWindow& app, bool reloadFont) {
    AppDpi_Refresh(app.hwnd, reloadFont);
    app.dpiScale = AppDpi_Scale();
    AppWindow_UpdateChromeMetrics(app);
}

LRESULT AppWindow_WndProc(AppWindow& app, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        return HandleNcCalcSize(hwnd, wParam, lParam);

    case WM_NCHITTEST:
        return HandleNcHitTest(hwnd, wParam, lParam);

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }

    if (ImGui::GetCurrentContext()) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
        LRESULT imeResult = 0;
        if (xcat::ui::HandleOfficialWin32ImeMessage(hwnd, msg, wParam, lParam, imeResult))
            return imeResult;
    }

    switch (msg) {
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // style base 已在主题 Commit 时按当前配色抓取；此处只重缩放 + 字体。
        AppWindow_RefreshDpi(app, true);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lParam &&
            (_wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0 ||
             _wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"AppsUseLightTheme") == 0)) {
            xcat::app::AppTheme_OnSystemThemeMaybeChanged(hwnd);
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            AppWindow_HandleResize(app, LOWORD(lParam), HIWORD(lParam));
        if (wParam == SIZE_RESTORED)
            ApplyTopmostZOrder(app);
        return 0;

    case WM_TIMER:
        // 登录会话定时器：由 main 注册的转发钩子处理；此处不吞消息
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;

    case WM_HOTKEY:
        if (wParam == kHotkeyToggleGui) {
            AppWindow_ToggleMinimized(app);
            return 0;
        }
        if (wParam == kHotkeyManualRejoin) {
            app.hotkeyF10.store(true);
            return 0;
        }
        break;

    case WM_CLOSE: {
        // 对齐 Artale：普通关窗默认只关 GUI、不杀游戏。
        // 例外：本会话 AccessDeny / 门禁 pending / 本机粘性拒绝 → 一并杀经典版，防注入留存。
        std::string prefsBin;
        {
            wchar_t mod[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, mod, MAX_PATH) > 0) {
                prefsBin = xcat::WideToUtf8(xcat::ParentDirWithSlash(mod)) + "XCat_data";
            }
        }
        if (xcat::app::ShouldKillGameOnLauncherClose(prefsBin)) {
            xcat::log::Warn("App", "WM_CLOSE access-gate → stop game");
            xcat::app::StopGameForAccessGateExit();
        }
        app.running = false;
        return 0;
    }

    case WM_XCAT_GRACEFUL_EXIT_DONE:
        app.pendingExitAfterSound.store(true);
        return 0;

    case WM_XCAT_ACCESS_GATE: {
        // 工作线程 PostMessage：主线程弹窗后硬退，避免在 ImGui 帧内/worker 里 MessageBox。
        const int code =
            xcat::app::HandleAccessGateUiMessage(static_cast<unsigned long long>(wParam));
        ExitProcess(static_cast<UINT>(code > 0 ? code : 2));
        return 0;
    }

    case WM_DESTROY:
        app.running = false;
        if (app.hwnd == hwnd) app.hwnd = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
