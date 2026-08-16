#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>

#include "app_chrome.h"
#include "lie_ai_pump.h"

#include "app_notify.h"
#include "app_sound.h"
#include "app_theme.h"
#include "app_window.h"
#include "attach_inject.h"
#include "gate_activation_ui.h"
#include "hangup_schedule.h"
#include "launch_panel.h"
#include "log_upload.h"
#include "net_path_probe.h"
#include "single_instance.h"
#include "update_client.h"
#include "xcat_start_gate.h"

#include "msc_webview_login.h"
#include "process_util.h"
#include "xcat_log.h"
#include "xcat_payload_control.h"
#include "xcat_version.h"

#include <objbase.h>

#include <string>

namespace {

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::string ExeDirUtf8() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return xcat::WideToUtf8(xcat::ParentDirWithSlash(path));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // 须在单实例锁之前：未提权进程若先占锁，UAC 重开的管理员实例会起不来。
    // 模式对照仓枫星 xcat_app（清单 requireAdministrator + runas 兜底）。
    if (!IsProcessElevated()) {
        wchar_t exePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH) || !exePath[0]) {
            MessageBoxW(nullptr, L"请用管理员模式启动", L"XCat TWMS",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return 1;
        }
        std::wstring params;
        for (int i = 1; i < __argc; ++i) {
            if (i > 1) params.push_back(L' ');
            const std::wstring a = __wargv[i] ? __wargv[i] : L"";
            const bool needQuote = a.find_first_of(L" \t\"") != std::wstring::npos;
            if (needQuote) {
                params.push_back(L'"');
                for (wchar_t ch : a) {
                    if (ch == L'"') params += L"\\\"";
                    else params.push_back(ch);
                }
                params.push_back(L'"');
            } else {
                params += a;
            }
        }
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.lpParameters = params.empty() ? nullptr : params.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            return 0;  // 提权子进程已拉起
        }
        MessageBoxW(nullptr,
                    L"需要管理员权限才能稳定注入与启动。\n"
                    L"请在 UAC 对话框点「是」，或右键 xcat.exe「以管理员身份运行」。",
                    L"XCat TWMS", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return 1;
    }

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
    // 飞行武装不持久化：每次启动 launcher 清会话态。出刀自组已落盘 user.ini，不清。
    xcat::ClearFlyArmedSession(prefsBin.c_str());
    xcat::ClearMapAttackSession(prefsBin.c_str());
    xcat::ClearAttackRpcFireSeq(prefsBin.c_str());
    xcat::ClearAttackRpcResetSeq(prefsBin.c_str());
    xcat::ClearAttackRpcStopSeq(prefsBin.c_str());
    xcat::ClearMobGatherFireSeq(prefsBin.c_str());
    xcat::ClearMobGatherDyRampSeq(prefsBin.c_str());
    // gate/1 启动激活门：纯本地校验个人签名 TOKEN（断网也拦），首次激活后按本机 deviceId 免输。
    // 这里只做「无 UI 缓存检查」；无有效激活时不退出，延后到窗口建好后用 ImGui 白天样式弹激活框
    // （见 AppTheme_Commit 之后）——避免再起一套 D3D11/字体，观感与主界面一致。
    const std::string gateDeviceId = xcat::app::ResolveClientHostIdentity(prefsBin).deviceId;
    const bool gateActivated = xcat::gate::HasValidActivation(prefsBin, gateDeviceId);
    xcat::app::AppTheme_Load(prefsBin.c_str());
    xcat::app::LoadAutoReceiveUpdates(prefsBin);
    xcat::app::notify::LoadNotifyPrefs(prefsBin);
    (void)xcat::app::ConsumeUpdateFailedNotify(prefsBin);

    xcat::app::sound::Init(prefsBin.c_str());

    AppWindow app{};
    // ImGui 全占客户区；高度对齐对照仓枫星 kLauncherOnlyDesignH。
    if (!AppWindow_Create(app, hi, xcat::app::kLauncherOnlyDesignW,
                          xcat::app::kLauncherOnlyDesignH)) {
        xcat::log::Error("App", "AppWindow_Create failed");
        CoUninitialize();
        xcat::app::ReleaseXcatSingleInstance();
        return 2;
    }
    xcat::app::SetAccessGateUiHwnd(app.hwnd);
    app.launchTickMs = GetTickCount64();

    xcat::app::AppTheme_Commit(app.hwnd);

    // gate/1 交互激活：无有效缓存时，在真正进入面板前用 ImGui 白天样式弹激活框；
    // 用户取消/关窗 → 视同拒启退出（退出码与旧 Win32 门一致 kStartGateExitCode）。
    if (!gateActivated) {
        if (!xcat::app::RunGateActivation(app, prefsBin, gateDeviceId)) {
            xcat::log::Warn("App", "exiting at startup: gate/1 cancelled");
            AppWindow_Destroy(app);
            xcat::log::Shutdown();
            CoUninitialize();
            xcat::app::ReleaseXcatSingleInstance();
            return xcat::gate::kStartGateExitCode;
        }
    }

    // gate/2 / gate/3 在线门放在 gate/1 之后：保证「本地激活门绝对最先」——无有效 TOKEN
    // 连在线门都不碰。此时窗口已建好，退出前先销毁窗口再清理。
    // 被封粘性：进 UI 前先探活——解禁后必须能清粘性；服不可达才继续本地拦。
    // 探活在运维不可达时会串行走完整回退（直连→代理→AliDNS→按 IP），耗时可达数十秒；
    // 放后台线程跑并泵「正在检查授权…」帧，避免主线程被 WinHTTP 阻塞导致窗口白屏假死。
    // 慢路径会把窗口临时缩成居中小对话框；这里先记住启动器原尺寸，放行进主面板前恢复。
    RECT gateSavedRect{};
    GetWindowRect(app.hwnd, &gateSavedRect);
    const bool gate2Deny = xcat::app::RunStartupGateWithModal(app, "正在检查设备授权", [&] {
        return xcat::app::EnforceStickyDeviceAccessOnStartup(xcat::app::kDefaultUpdateServiceUrl,
                                                             prefsBin);
    });
    if (gate2Deny) {
        const auto kind = xcat::app::ConsumeAccessGateExitRequest();
        xcat::log::Warn("App", "exiting at startup: gate/2 code=%d",
                        xcat::app::AccessGateExitCode(kind));
        AppWindow_Destroy(app);
        xcat::log::Shutdown();
        CoUninitialize();
        xcat::app::ReleaseXcatSingleInstance();
        return xcat::app::AccessGateExitCode(kind) ? xcat::app::AccessGateExitCode(kind) : 2;
    }
    // 在线租约：无有效租约则同步探运维 access；掐网且租约失效则无法启动。
    const bool gate3Deny = xcat::app::RunStartupGateWithModal(app, "正在检查在线授权", [&] {
        return xcat::app::EnforceOnlineLeaseGateOnStartup(xcat::app::kDefaultUpdateServiceUrl,
                                                          prefsBin);
    });
    if (gate3Deny) {
        const auto kind = xcat::app::ConsumeAccessGateExitRequest();
        const int code = xcat::app::AccessGateExitCode(kind);
        xcat::log::Warn("App", "exiting at startup: gate/%d code=%d",
                        kind == xcat::app::AccessGateExitKind::AccessDeny ? 2 : 3,
                        code ? code : 3);
        AppWindow_Destroy(app);
        xcat::log::Shutdown();
        CoUninitialize();
        xcat::app::ReleaseXcatSingleInstance();
        return code ? code : 3;
    }

    // 本地卡要让服务端拿出 uid，但只在本轮已经同步探到「放行却没人」且 proof 头确实发出时才弹框。
    // 租约仍有效、服不可达、或派生凭证没带上：不清缓存（否则台账现卡也会被误杀）。
    // 刚重贴完才 quick 探一次核对新卡，仍不走完整回退。
    constexpr const char* kGateUnrecognizedHint =
        "服务端无法识别此卡，请粘贴管理员发放的现卡。";
    bool afterReactivate = false;
    for (;;) {
        if (!xcat::gate::HasValidActivation(prefsBin, gateDeviceId)) break;
        bool unrecognized = false;
        if (afterReactivate) {
            (void)xcat::app::RunStartupGateWithModal(app, "正在检查授权", [&] {
                unrecognized = xcat::app::StartupCardUnrecognizedByServer(
                    xcat::app::kDefaultUpdateServiceUrl, prefsBin, true);
                return false;
            });
        } else {
            unrecognized = xcat::app::StartupCardUnrecognizedByServer(
                xcat::app::kDefaultUpdateServiceUrl, prefsBin, false);
        }
        if (!unrecognized) break;
        xcat::gate::InvalidateActivationCache(prefsBin);
        xcat::app::ClearStartupAccessProbe();
        if (!xcat::app::RunGateActivation(app, prefsBin, gateDeviceId, kGateUnrecognizedHint)) {
            xcat::log::Warn("App", "exiting at startup: gate/1 re-activate cancelled");
            AppWindow_Destroy(app);
            xcat::log::Shutdown();
            CoUninitialize();
            xcat::app::ReleaseXcatSingleInstance();
            return xcat::gate::kStartGateExitCode;
        }
        afterReactivate = true;
    }

    // 放行进主面板：若在线门慢路径把窗口缩成了对话框，恢复启动器原尺寸/位置。
    SetWindowPos(app.hwnd, nullptr, gateSavedRect.left, gateSavedRect.top,
                 gateSavedRect.right - gateSavedRect.left, gateSavedRect.bottom - gateSavedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    xcat::app::LaunchUiState ui{};
    ui.prefsBinDir = prefsBin;
    xcat::app::LaunchPanel_LoadAccount(ui);

    // attach_inject 日志进启动面板；启动模式落盘到 XCat_data/state（更新保留）。
    xcat::app::attach_inject::Init(&xcat::app::LaunchPanel_OnWebLog, prefsBin);

    // 冷启：手动自动监视；GAMA PASS / gamania (HK) 准备窗后自动换票启动。
    ui.pendingAutoLaunch = true;
    if (xcat::app::attach_inject::IsAttachWatchMode(
            xcat::app::attach_inject::GetLaunchMode())) {
        ui.status = "启动模式：手动启动并注入 — 就绪后自动开始监视";
        xcat::log::Info("App", "pending auto-watch (AttachWatch)");
    } else if (xcat::app::attach_inject::GetLaunchMode() ==
               xcat::app::attach_inject::LaunchMode::OneClickLogin) {
        if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
            // weblogin 尚未 Init；稍后 Init 会读盘。此处只设状态。
        }
        xcat::app::LaunchPanel_ArmStrategyPrep(ui, 6000);
        ui.status = "gamania (HK)：约 6 秒后自动启动（可再点按钮取消）";
        xcat::log::Info("App", "pending auto gamania(HK) launch (defer 6s)");
    } else {
        msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
        xcat::app::LaunchPanel_ArmGamaPassAutoLaunch(ui);
        xcat::log::Info("App", "pending auto GamaPass launch (defer %us)",
                        xcat::app::kGamaPassAutoPrepSec);
    }

    if (!msc::weblogin::Init(app.hwnd, &xcat::app::LaunchPanel_OnWebLog)) {
        xcat::log::Error("App", "msc::weblogin::Init failed");
        ui.status = "登录会话初始化失败";
        ui.pendingAutoLaunch = false;
    } else {
        xcat::log::Info("App", "login session ready (GamaPass CDP / HTTP; no WebView2)");
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
                    // 换票中/开游戏中等进行态：会话闲下后必须收口，否则 GAMA PASS 成功后
                    // 「换票成功，正在开游戏/注入…」会一直挂在状态栏。
                    const bool inFlightHint =
                        ui.status.find("已开始") != std::string::npos ||
                        ui.status.find("进行中") != std::string::npos ||
                        ui.status.find("正在开游戏") != std::string::npos ||
                        ui.status.find("自动登录中") != std::string::npos ||
                        ui.status.find("登录换票中") != std::string::npos ||
                        ui.status.find("正在自动登录") != std::string::npos ||
                        ui.status.find("正在按时段拉起") != std::string::npos;
                    if (inFlightHint) {
                        ui.status = msc::weblogin::IsBusy() ? "正在登录/换票中…" : "空闲";
                    }
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
        if (const auto kind = xcat::app::ConsumeAccessGateExitRequest();
            kind != xcat::app::AccessGateExitKind::None) {
            const int code = xcat::app::AccessGateExitCode(kind);
            xcat::log::Warn("App", "exiting: gate/%d code=%d",
                            kind == xcat::app::AccessGateExitKind::AccessDeny ? 2 : 3, code);
            ExitProcess(static_cast<UINT>(code));
        }
        xcat::app::UpdateForcePollTick(xcat::app::kDefaultUpdateServiceUrl, prefsBin);
        // 放在最小化 continue 之前：挂机时面板通常是最小化的，而那正是要取证的时段。
        xcat::app::net_path::Tick();

        if (AppWindow_IsMinimized(app)) {
            // F9 最小化跳过 Present；仍 Poll 以便卷軸叮咚等可播。
            // 不自动 ShowWindow：避免桌面露出面板导致误触；气泡等用户自行还原再画。
            xcat::app::notify::Poll(prefsBin);
            Sleep(50);
            continue;
        }

        const ULONGLONG frameStart = GetTickCount64();
        AppWindow_BeginFrame(app, clearColor);
        xcat::app::DrawMainShell(app, ui);
        AppWindow_EndFrame(app);

        // 仅限启动器 ImGui：Present(1) 在遮挡时常立刻返回，软限约 30FPS，不碰游戏/payload。
        constexpr ULONGLONG kUiFrameBudgetMs = 33;
        const ULONGLONG elapsed = GetTickCount64() - frameStart;
        if (elapsed < kUiFrameBudgetMs) {
            Sleep(static_cast<DWORD>(kUiFrameBudgetMs - elapsed));
        }

        if (!shown) {
            AppWindow_Show(app);
            shown = true;
        }
    }

    msc::weblogin::Shutdown();
    xcat::app::attach_inject::Shutdown();
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
