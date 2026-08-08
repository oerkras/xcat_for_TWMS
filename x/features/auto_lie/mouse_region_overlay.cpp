#include "mouse_region_overlay.h"

#include "../titlebar/titlebar_win.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace x::features::auto_lie::mouse_region_overlay {
namespace {

constexpr const wchar_t* kClassName = L"XCatLieMouseRegionOverlay";
constexpr int kPad = 48;
constexpr int kHudW = 520;
constexpr int kHudH = 72;
constexpr DWORD kPaintIntervalMs = 33;
constexpr int kMaxOverlayW = 1600;
constexpr int kMaxOverlayH = 1200;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_paintStop{false};
SRWLOCK g_snapLock = SRWLOCK_INIT;
Snapshot g_snap{};

HWND g_hwnd = nullptr;
HANDLE g_paintThread = nullptr;
bool g_classRegistered = false;

void DrawCross(HDC hdc, long x, long y, long arm, HPEN pen) {
    const HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, x - arm, y, nullptr);
    LineTo(hdc, x + arm + 1, y);
    MoveToEx(hdc, x, y - arm, nullptr);
    LineTo(hdc, x, y + arm + 1);
    SelectObject(hdc, old);
}

void DrawDiamond(HDC hdc, long x, long y, long r, HPEN pen) {
    const HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    const HBRUSH oldBrush =
        static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    POINT pts[5] = {
        {x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}, {x, y - r},
    };
    Polyline(hdc, pts, 5);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, old);
}

bool EnsureClass() {
    if (g_classRegistered) return true;
    HMODULE mod = x::runtime::GetImageModule();
    if (!mod) mod = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = mod;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        x::runtime::LogW("AutoLieOverlay", "RegisterClassEx failed err=%lu", GetLastError());
        return false;
    }
    g_classRegistered = true;
    return true;
}

void DestroyOverlayWindow() {
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_HIDE);
        DestroyWindow(g_hwnd);
    }
    g_hwnd = nullptr;
}

void HideOverlayWindow() {
    if (g_hwnd && IsWindow(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
}

bool GameClientOrigin(POINT& out) {
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) return false;
    out = {};
    return ClientToScreen(hwnd, &out) != FALSE;
}

void ComputePaintRect(const Snapshot& snap, RECT& out) {
    if (snap.valid) {
        long minX = snap.corners[0].x;
        long minY = snap.corners[0].y;
        long maxX = minX;
        long maxY = minY;
        for (int i = 1; i < 4; ++i) {
            minX = (std::min)(minX, snap.corners[i].x);
            minY = (std::min)(minY, snap.corners[i].y);
            maxX = (std::max)(maxX, snap.corners[i].x);
            maxY = (std::max)(maxY, snap.corners[i].y);
        }
        minX = (std::min)(minX, snap.center.x);
        minY = (std::min)(minY, snap.center.y);
        maxX = (std::max)(maxX, snap.center.x);
        maxY = (std::max)(maxY, snap.center.y);
        if (snap.plannedValid) {
            minX = (std::min)(minX, snap.planned.x);
            minY = (std::min)(minY, snap.planned.y);
            maxX = (std::max)(maxX, snap.planned.x);
            maxY = (std::max)(maxY, snap.planned.y);
        }
        if (snap.cursor.x >= minX - 200 && snap.cursor.x <= maxX + 200 &&
            snap.cursor.y >= minY - 200 && snap.cursor.y <= maxY + 200) {
            minX = (std::min)(minX, snap.cursor.x);
            minY = (std::min)(minY, snap.cursor.y);
            maxX = (std::max)(maxX, snap.cursor.x);
            maxY = (std::max)(maxY, snap.cursor.y);
        }
        out.left = minX - kPad;
        out.top = minY - kPad - 40;
        out.right = maxX + kPad;
        out.bottom = maxY + kPad;
        return;
    }

    POINT origin{};
    if (GameClientOrigin(origin)) {
        out.left = origin.x + 16;
        out.top = origin.y + 16;
    } else {
        out.left = snap.cursor.x + 24;
        out.top = snap.cursor.y + 24;
    }
    out.right = out.left + kHudW;
    out.bottom = out.top + kHudH;
}

bool PushTransparentFrame(HWND hwnd, int left, int top, int width, int height) {
    if (!hwnd || width <= 0 || height <= 0) return false;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!memDc || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (memDc) DeleteDC(memDc);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        return false;
    }
    const HGDIOBJ oldBmp = SelectObject(memDc, dib);
    std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    SIZE size{width, height};
    POINT src{0, 0};
    POINT dst{left, top};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL ok =
        UpdateLayeredWindow(hwnd, screenDc, &dst, &size, memDc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return ok != FALSE;
}

bool EnsureOverlayWindow(const RECT& want) {
    if (!EnsureClass()) return false;
    const int w = want.right - want.left;
    const int h = want.bottom - want.top;
    if (w <= 0 || h <= 0 || w > kMaxOverlayW || h > kMaxOverlayH) {
        x::runtime::LogW("AutoLieOverlay", "refuse oversized overlay %dx%d", w, h);
        HideOverlayWindow();
        return false;
    }

    if (g_hwnd && IsWindow(g_hwnd)) {
        RECT cur{};
        GetWindowRect(g_hwnd, &cur);
        const int cw = cur.right - cur.left;
        const int ch = cur.bottom - cur.top;
        if (cw > w * 2 || ch > h * 2) {
            HideOverlayWindow();
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }
    }

    if (!g_hwnd || !IsWindow(g_hwnd)) {
        HMODULE createMod = x::runtime::GetImageModule();
        if (!createMod) createMod = GetModuleHandleW(nullptr);
        // 故意不挂 Unity 为 owner：最小化时 owned popup 的创建/销毁会 Sync 进挂起的
        // 游戏窗口线程，实机已出现模拟后泵永久 idle（BIN 18:12 最小化测）。
        g_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                WS_EX_NOACTIVATE,
            kClassName, L"XCatLieOverlay", WS_POPUP, want.left, want.top, w, h, nullptr,
            nullptr, createMod, nullptr);
        if (!g_hwnd) {
            x::runtime::LogW("AutoLieOverlay", "CreateWindowEx failed err=%lu", GetLastError());
            return false;
        }
        if (!PushTransparentFrame(g_hwnd, want.left, want.top, w, h)) {
            x::runtime::LogW("AutoLieOverlay", "initial transparent ULW failed");
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
            return false;
        }
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    SetWindowPos(g_hwnd, HWND_TOPMOST, want.left, want.top, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return true;
}

void PremultiplyVisiblePixels(uint32_t* px, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t c = px[i];
        if ((c & 0x00FFFFFFu) != 0) px[i] = c | 0xFF000000u;
    }
}

void DrawPolylineLocal(HDC hdc, HPEN pen, const POINT* pts, int count, long originX,
                       long originY) {
    if (!pts || count < 2) return;
    POINT local[kMaxTrailPoints]{};
    const int n = (std::min)(count, kMaxTrailPoints);
    for (int i = 0; i < n; ++i) {
        local[i].x = pts[i].x - originX;
        local[i].y = pts[i].y - originY;
    }
    const HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    Polyline(hdc, local, n);
    SelectObject(hdc, old);
}

void PaintSnapshot(Snapshot snap) {
    GetCursorPos(&snap.cursor);

    if (!snap.valid) {
        // 勾选后无题：小 HUD，提示等待 NonFinite。
        RECT want{};
        ComputePaintRect(snap, want);
        if (!EnsureOverlayWindow(want)) return;

        const int width = want.right - want.left;
        const int height = want.bottom - want.top;
        if (width <= 0 || height <= 0) return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        HBITMAP dib =
            CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!memDc || !dib || !bits) {
            if (dib) DeleteObject(dib);
            if (memDc) DeleteDC(memDc);
            if (screenDc) ReleaseDC(nullptr, screenDc);
            return;
        }
        const HGDIOBJ oldBmp = SelectObject(memDc, dib);
        std::memset(bits, 0,
                    static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

        SetBkMode(memDc, TRANSPARENT);
        HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                 L"Segoe UI");
        const HFONT oldFont = static_cast<HFONT>(
            SelectObject(memDc, font ? font : GetStockObject(DEFAULT_GUI_FONT)));

        HBRUSH bar = CreateSolidBrush(RGB(20, 24, 32));
        RECT barRc{0, 0, width, height};
        FillRect(memDc, &barRc, bar);
        DeleteObject(bar);
        SetTextColor(memDc, RGB(255, 200, 80));
        const wchar_t* waitMsg = L"题目区域显示 ON — 等待 NonFinite 面板";
        TextOutW(memDc, 12, 12, waitMsg, static_cast<int>(wcslen(waitMsg)));
        wchar_t cur[96];
        std::swprintf(cur, 96, L"cursor=(%ld,%ld)", snap.cursor.x, snap.cursor.y);
        if (snap.label[0]) {
            wchar_t wlabel[96]{};
            MultiByteToWideChar(CP_UTF8, 0, snap.label, -1, wlabel, 96);
            TextOutW(memDc, 12, 36, wlabel, static_cast<int>(wcslen(wlabel)));
        } else {
            TextOutW(memDc, 12, 40, cur, static_cast<int>(wcslen(cur)));
        }

        PremultiplyVisiblePixels(static_cast<uint32_t*>(bits),
                                 static_cast<size_t>(width) * static_cast<size_t>(height));

        SIZE size{width, height};
        POINT src{0, 0};
        POINT dst{want.left, want.top};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        if (!UpdateLayeredWindow(g_hwnd, screenDc, &dst, &size, memDc, &src, 0, &blend,
                                  ULW_ALPHA)) {
            HideOverlayWindow();
        } else {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        }

        SelectObject(memDc, oldFont);
        if (font) DeleteObject(font);
        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    RECT want{};
    ComputePaintRect(snap, want);
    if (!EnsureOverlayWindow(want)) return;

    const int width = want.right - want.left;
    const int height = want.bottom - want.top;
    if (width <= 0 || height <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!memDc || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (memDc) DeleteDC(memDc);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        return;
    }
    const HGDIOBJ oldBmp = SelectObject(memDc, dib);
    std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    auto toLocal = [&](POINT p) -> POINT {
        return POINT{p.x - want.left, p.y - want.top};
    };

    SetBkMode(memDc, TRANSPARENT);
    HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    const HFONT oldFont = static_cast<HFONT>(
        SelectObject(memDc, font ? font : GetStockObject(DEFAULT_GUI_FONT)));

    HPEN panelPen = CreatePen(PS_SOLID, 3, RGB(0, 220, 255));
    HPEN answerPen = CreatePen(PS_SOLID, 3, RGB(40, 120, 255));
    HPEN livePen = CreatePen(PS_SOLID, 3, RGB(255, 48, 48));
    HPEN centerPen = CreatePen(PS_SOLID, 2, RGB(255, 220, 0));
    HPEN plannedPen = CreatePen(PS_SOLID, 2, RGB(255, 120, 0));
    HPEN cursorPen = CreatePen(PS_SOLID, 2, RGB(255, 60, 200));

    POINT local[5]{};
    for (int i = 0; i < 4; ++i) local[i] = toLocal(snap.corners[i]);
    local[4] = local[0];
    const HPEN oldPen = static_cast<HPEN>(SelectObject(memDc, panelPen));
    Polyline(memDc, local, 5);
    SelectObject(memDc, oldPen);

    DrawPolylineLocal(memDc, answerPen, snap.answerTrail, snap.answerCount, want.left,
                      want.top);
    DrawPolylineLocal(memDc, livePen, snap.liveTrail, snap.liveCount, want.left, want.top);

    const POINT c = toLocal(snap.center);
    DrawCross(memDc, c.x, c.y, 14, centerPen);
    DrawDiamond(memDc, c.x, c.y, 8, centerPen);

    if (snap.plannedValid) {
        const POINT p = toLocal(snap.planned);
        DrawCross(memDc, p.x, p.y, 18, plannedPen);
        const HBRUSH oldBrush =
            static_cast<HBRUSH>(SelectObject(memDc, GetStockObject(NULL_BRUSH)));
        SelectObject(memDc, plannedPen);
        Ellipse(memDc, p.x - 6, p.y - 6, p.x + 7, p.y + 7);
        SelectObject(memDc, oldBrush);
    }

    const POINT cur = toLocal(snap.cursor);
    DrawCross(memDc, cur.x, cur.y, 22, cursorPen);

    SetTextColor(memDc, RGB(0, 220, 255));
    const wchar_t* legend =
        L"cyan=panel  blue=answer  red=live  orange=next  magenta=cursor";
    TextOutW(memDc, 8, 8, legend, static_cast<int>(wcslen(legend)));
    if (snap.label[0]) {
        wchar_t wlabel[96]{};
        MultiByteToWideChar(CP_UTF8, 0, snap.label, -1, wlabel, 96);
        TextOutW(memDc, 8, 28, wlabel, static_cast<int>(wcslen(wlabel)));
    }
    wchar_t coords[192];
    std::swprintf(coords, 192,
                  L"answer=%d live=%d center=(%ld,%ld) next=(%ld,%ld) cursor=(%ld,%ld)",
                  snap.answerCount, snap.liveCount, snap.center.x, snap.center.y,
                  snap.planned.x, snap.planned.y, snap.cursor.x, snap.cursor.y);
    SetTextColor(memDc, RGB(240, 240, 240));
    TextOutW(memDc, 8, 48, coords, static_cast<int>(wcslen(coords)));

    DeleteObject(panelPen);
    DeleteObject(answerPen);
    DeleteObject(livePen);
    DeleteObject(centerPen);
    DeleteObject(plannedPen);
    DeleteObject(cursorPen);

    PremultiplyVisiblePixels(static_cast<uint32_t*>(bits),
                             static_cast<size_t>(width) * static_cast<size_t>(height));

    SIZE size{width, height};
    POINT src{0, 0};
    POINT dst{want.left, want.top};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    if (!UpdateLayeredWindow(g_hwnd, screenDc, &dst, &size, memDc, &src, 0, &blend,
                             ULW_ALPHA)) {
        x::runtime::LogWThrottled(73, 3000, "AutoLieOverlay",
                               "UpdateLayeredWindow fail err=%lu size=%dx%d", GetLastError(),
                               width, height);
        HideOverlayWindow();
    } else {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }

    SelectObject(memDc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void StopPaintThread();

DWORD WINAPI PaintThread(LPVOID) {
    x::runtime::LogI("AutoLieOverlay", "paint thread start");
    while (!g_paintStop.load(std::memory_order_acquire)) {
        if (!g_enabled.load(std::memory_order_acquire)) {
            HideOverlayWindow();
            Sleep(100);
            continue;
        }

        Snapshot snap{};
        AcquireSRWLockShared(&g_snapLock);
        snap = g_snap;
        ReleaseSRWLockShared(&g_snapLock);
        PaintSnapshot(snap);
        Sleep(kPaintIntervalMs);
    }
    DestroyOverlayWindow();
    x::runtime::LogI("AutoLieOverlay", "paint thread exit");
    return 0;
}

void StopPaintThread() {
    g_paintStop.store(true, std::memory_order_release);
    if (g_paintThread) {
        const DWORD wr = WaitForSingleObject(g_paintThread, 3000);
        if (wr == WAIT_OBJECT_0) {
            CloseHandle(g_paintThread);
            g_paintThread = nullptr;
            // paint 线程退出路径已 DestroyOverlayWindow；此处勿再跨线程 Destroy。
        } else {
            // 超时仍 DestroyWindow/CloseHandle 会：① 跨线程挂死 ② 丢弃仍在跑的线程句柄。
            x::runtime::LogW("AutoLieOverlay",
                             "paint thread join timeout wr=%lu — leak handle, skip DestroyWindow",
                             wr);
        }
    }
    g_paintStop.store(false, std::memory_order_release);
}

}  // namespace

void SetEnabled(bool on) {
    const bool was = g_enabled.exchange(on, std::memory_order_acq_rel);
    if (was == on) return;
    x::runtime::LogI("AutoLieOverlay", "region overlay %s", on ? "armed" : "soft-off");
    if (!on) {
        AcquireSRWLockExclusive(&g_snapLock);
        g_snap = {};
        ReleaseSRWLockExclusive(&g_snapLock);
        // 窗口在 paint 线程创建，且该线程只有 Sleep 循环、无消息泵。
        // 从 sim/worker/follower 线程 ShowWindow/DestroyWindow 会走跨线程
        // SendMessage，paint 永远收不到 → 永久卡住（BIN 21:56 soft-off 后无
        // finished、HardPause 不放、打怪永不恢复）。关显由 paint 见 g_enabled=0 自藏。
        return;
    }
    if (!g_paintThread) {
        g_paintStop.store(false, std::memory_order_release);
        g_paintThread = CreateThread(nullptr, 0, &PaintThread, nullptr, 0, nullptr);
        if (!g_paintThread) {
            x::runtime::LogW("AutoLieOverlay", "paint thread create failed err=%lu",
                          GetLastError());
            g_enabled.store(false, std::memory_order_release);
        }
    }
}

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

void SetSnapshot(const Snapshot& snap) {
    AcquireSRWLockExclusive(&g_snapLock);
    g_snap = snap;
    ReleaseSRWLockExclusive(&g_snapLock);
}

void Tick(DWORD /*now*/) {
    // 勿在此 Hide：Tick 不在 paint 线程，跨线程 ShowWindow 同上会死锁。
    (void)0;
}

void Shutdown() {
    g_enabled.store(false, std::memory_order_release);
    StopPaintThread();
    AcquireSRWLockExclusive(&g_snapLock);
    g_snap = {};
    ReleaseSRWLockExclusive(&g_snapLock);
}

}  // namespace x::features::auto_lie::mouse_region_overlay
