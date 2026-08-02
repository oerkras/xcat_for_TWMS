#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "app_chrome.h"
#include "lie_ai_pump.h"

#include "app_notify.h"
#include "app_sound.h"
#include "app_theme.h"
#include "app_window.h"
#include "hangup_schedule.h"
#include "launch_panel.h"
#include "single_instance.h"
#include "update_client.h"

#include "msc_webview_login.h"
#include "process_util.h"
#include "xcat_log.h"
#include "xcat_payload_control.h"
#include "xcat_version.h"

#include <objbase.h>

#include <string>

namespace {


std::string ExeDirUtf8() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return xcat::WideToUtf8(xcat::ParentDirWithSlash(path));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    if (!xcat::app::AcquireXcatSingleInstance(3000)) {
        MessageBoxW(nullptr, L"XCat 已在运行。", L"XCat TWMS", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::string binDir = ExeDirUtf8();
    xcat::CreateDirectoryUtf8(binDir + "logs");
    xcat::CreateDirectoryUtf8(binDir + "XCat_data\\state");
    xcat::CreateDirectoryUtf8(binDir + "XCat_data\\logs");

    xcat::log::Options logOpts{};
    logOpts.component = xcat::log::Component::Launcher;
    logOpts.filePath = xcat::log::paths::LauncherLog(binDir.c_str());
    logOpts.debugOutput = true;
    xcat::log::Configure(logOpts);
    xcat::log::RegisterAuxFile(xcat::log::Component::Inject,
                               xcat::log::paths::InjectLog(binDir.c_str()));
    xcat::log::BeginSession("xcat_twms");
    xcat::log::Info("App", "start %s %s launcher=%s", xcat::kXcatProductName,
                    xcat::kXcatVersionString, logOpts.filePath.c_str());

    const std::string prefsBin = binDir + "XCat_data";
    // 飞行武装不持久化：每次启动 launcher 清会话态（mode/CD 仍在 user.ini）。
    xcat::ClearFlyArmedSession(prefsBin.c_str());
    xcat::app::AppTheme_Load(prefsBin.c_str());
    xcat::app::LoadAutoReceiveUpdates(prefsBin);
    xcat::app::notify::LoadNotifyPrefs(prefsBin);
    (void)xcat::app::ConsumeUpdateFailedNotify(prefsBin);

    xcat::app::sound::Init();

    AppWindow app{};
    // ImGui 全占客户区；高度对齐对照仓枫星 kLauncherOnlyDesignH（WebView 静默不占布局）。
    if (!AppWindow_Create(app, hi, xcat::app::kLauncherOnlyDesignW,
                          xcat::app::kLauncherOnlyDesignH)) {
        xcat::log::Error("App", "AppWindow_Create failed");
        CoUninitialize();
        xcat::app::ReleaseXcatSingleInstance();
        return 2;
    }
    app.launchTickMs = GetTickCount64();

    AppWindow_CreateSilentWebHost(app);

    xcat::app::AppTheme_Commit(app.hwnd);

    xcat::app::LaunchUiState ui{};
    ui.prefsBinDir = prefsBin;
    xcat::app::LaunchPanel_LoadAccount(ui);
    if (xcat::app::LaunchPanel_AccountLooksValid(ui, nullptr)) {
        ui.pendingAutoLaunch = true;
        ui.status = "已加载账号，WebView 就绪后将自动启动…";
        xcat::log::Info("App", "pending auto-launch (saved account valid)");
    } else {
        ui.pendingAutoLaunch = false;
        if (ui.accountLine[0]) {
            ui.status = "账号串无法识别，请修正后再启动（不会自动开游戏）";
            xcat::log::Warn("App", "saved account invalid, auto-launch disabled");
        } else {
            ui.status = "未填写账号串，等待手动粘贴后启动";
            xcat::log::Info("App", "no account, auto-launch disabled");
        }
    }

    if (!app.webHost ||
        !msc::weblogin::Init(app.hwnd, app.webHost, &xcat::app::LaunchPanel_OnWebLog)) {
        xcat::log::Error("App", "msc::weblogin::Init failed");
        ui.status = "WebView 登录模块初始化失败";
        ui.pendingAutoLaunch = false;
    } else if (!msc::weblogin::IsRuntimeInstalled()) {
        if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::WebViewOnly) {
            ui.status = "缺少 WebView2 Runtime：请按提示下载安装，完成后重启本程序";
            ui.pendingAutoLaunch = false;
            xcat::log::Warn("App", "WebView2 Runtime missing (WebViewOnly)");
        } else {
            ui.status = "无 WebView2 Runtime：将走 HTTP 取票（遇验证码需改策略或装 Runtime）";
            xcat::log::Info("App", "WebView2 Runtime missing; HTTP auth strategy available");
        }
        msc::weblogin::OnResize();
    } else {
        msc::weblogin::OnResize();
        xcat::log::Info("App", "WebView login session silent (hidden)");
    }

    float clearColor[4]{};
    AppWindow_GetClearColor(clearColor);

    bool shown = false;
    while (app.running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.hwnd == app.hwnd) {
                if (msg.message == WM_TIMER) {
                    msc::weblogin::OnTimer(static_cast<UINT_PTR>(msg.wParam));
                } else if (msg.message == msc::weblogin::kMsgFlushLogs) {
                    msc::weblogin::OnFlushLogs();
                } else if (msg.message == msc::weblogin::kMsgIdle) {
                    msc::weblogin::OnIdle();
                    if (ui.status.find("已开始") != std::string::npos ||
                        ui.status.find("进行中") != std::string::npos) {
                        ui.status = msc::weblogin::IsBusy() ? "正在登录/换票中…" : "空闲";
                    }
                } else if (msg.message == msc::weblogin::kMsgStartWebViewLogin) {
                    msc::weblogin::OnStartWebViewLoginEx(msg.wParam);
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) app.running = false;
        }
        if (!app.running) break;

        xcat::app::AppTheme_PumpPending();
        xcat::app::LaunchPanel_TryAutoLaunchWhenReady(ui);
        xcat::app::hangup_schedule::Tick(ui, app.exiting);
        xcat::app::LieAiPump_Tick(prefsBin);

        if (xcat::app::PollGracefulExit(app, ui)) break;

        if (xcat::app::TryStartAutoInstall(binDir)) {
            xcat::log::Info("App", "auto-install update started");
        }
        if (xcat::app::ConsumeUpdateProcessExitRequest()) {
            xcat::log::Info("App", "exiting for update apply");
            ExitProcess(0);
        }
        xcat::app::UpdateForcePollTick(xcat::app::kDefaultUpdateServiceUrl, prefsBin);

        if (AppWindow_IsMinimized(app)) {
            Sleep(16);
            continue;
        }

        AppWindow_BeginFrame(app, clearColor);
        xcat::app::DrawMainShell(app, ui);
        AppWindow_EndFrame(app);

        if (!shown) {
            AppWindow_Show(app);
            shown = true;
        }
    }

    msc::weblogin::Shutdown();
    xcat::app::LieAiPump_Shutdown();
    AppWindow_Destroy(app);
    xcat::app::notify::Reset();
    xcat::app::sound::Shutdown();
    xcat::log::SetLineCallback(nullptr);
    xcat::log::Shutdown();
    CoUninitialize();
    xcat::app::ReleaseXcatSingleInstance();
    return 0;
}
