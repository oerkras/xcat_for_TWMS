#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "ops_panel.h"
#include "ops_window.h"

namespace {

HANDLE g_singleInstance = nullptr;

bool AcquireSingleInstance() {
    g_singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\XCatTwmsOpsConsole.SingleInstance");
    if (!g_singleInstance) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_singleInstance);
        g_singleInstance = nullptr;
        return false;
    }
    return true;
}

void ReleaseSingleInstance() {
    if (g_singleInstance) {
        ReleaseMutex(g_singleInstance);
        CloseHandle(g_singleInstance);
        g_singleInstance = nullptr;
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    if (!AcquireSingleInstance()) {
        MessageBoxW(nullptr, L"XCat TWMS 运维控制台已在运行。", L"XCat TWMS Ops", MB_ICONINFORMATION);
        return 0;
    }

    OpsWindow window{};
    if (!OpsWindow_Create(window, inst, 1080, 760)) {
        MessageBoxW(nullptr, L"Failed to create ops window / D3D11 device.", L"XCat TWMS Ops", MB_ICONERROR);
        ReleaseSingleInstance();
        return 1;
    }
    OpsWindow_Show(window);

    xcat::ops::OpsState state{};
    xcat::ops::OpsState_Init(state);

    const float clear[4] = {0.04f, 0.045f, 0.06f, 1.f};
    // 运维台 UI 不需要高刷：Present(1) 在最小化/遮挡时常立刻返回，会空转吃满 GPU。
    constexpr ULONGLONG kOpsFrameBusyMs = 33;   // 交互中 ~30 FPS
    constexpr ULONGLONG kOpsFrameIdleMs = 100;  // 干盯连接表 ~10 FPS（数据本身 5s 才变）
    bool lastFrameHadInput = true;
    while (window.running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) window.running = false;
            lastFrameHadInput = true;
        }
        if (!window.running) break;

        if (OpsWindow_IsMinimized(window)) {
            // 不 Present，避免 DXGI 在最小化时空转吃 GPU；但仍 Tick，利润监控继续采样。
            xcat::ops::OpsState_Tick(state);
            Sleep(200);
            continue;
        }

        const ULONGLONG frameStart = GetTickCount64();
        xcat::ops::OpsState_Tick(state);
        OpsWindow_BeginFrame(window, clear);
        xcat::ops::OpsPanel_Draw(state);
        OpsWindow_EndFrame(window);
        lastFrameHadInput = OpsWindow_FrameHadInput();

        const ULONGLONG budget = lastFrameHadInput ? kOpsFrameBusyMs : kOpsFrameIdleMs;
        const ULONGLONG elapsed = GetTickCount64() - frameStart;
        if (elapsed < budget) {
            Sleep(static_cast<DWORD>(budget - elapsed));
        }
    }

    xcat::ops::OpsState_Shutdown(state);
    OpsWindow_Destroy(window);
    ReleaseSingleInstance();
    return 0;
}
