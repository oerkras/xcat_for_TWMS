#include "launch_panel.h"
#include "app_dpi.h"
#include "app_event_log.h"
#include "app_notify.h"
#include "app_sound.h"
#include "app_window.h"
#include "attach_inject.h"
#include "hangup_schedule.h"
#include "imgui_shell.h"
#include "runtime_leds.h"
#include "status_bar.h"
#include "update_client.h"
#include "workspace_tabs.h"

#include "msc_webview_login.h"
#include "process_util.h"
#include "xcat_log.h"
#include "xcat_version.h"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

namespace xcat::app {
namespace {

std::mutex gLogUiMu;
LaunchUiState* gLogUi = nullptr;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void TrimLogTail(std::string& s, size_t maxChars = 48 * 1024) {
    if (s.size() <= maxChars) return;
    const size_t cut = s.size() - maxChars;
    size_t nl = s.find('\n', cut);
    if (nl == std::string::npos) nl = cut;
    else ++nl;
    s.erase(0, nl);
}

void SoftWrapAccountBuffer(char* buf, size_t cap) {
    if (!buf || cap < 8) return;
    std::string s = buf;
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();

    std::string out;
    out.reserve(s.size() + 32);
    constexpr size_t kSoftWrap = 42;
    size_t i = 0;
    size_t col = 0;
    while (i < s.size()) {
        // 连续 '-' 视为分隔：原样写出后换行，便于阅读（解析时会剥掉换行）
        if (s[i] == '-') {
            while (i < s.size() && s[i] == '-') {
                out.push_back(s[i++]);
            }
            out.push_back('\n');
            col = 0;
            continue;
        }
        out.push_back(s[i++]);
        ++col;
        if (col >= kSoftWrap) {
            out.push_back('\n');
            col = 0;
        }
    }
    if (out.size() >= cap) out.resize(cap - 1);
    memcpy(buf, out.data(), out.size());
    buf[out.size()] = '\0';
}

void PollMilestoneSounds(const RuntimeLeds& leds) {
    static bool s_ipcPlayed = false;
    static bool s_gcPlayed = false;

    if (leds.ipc && !s_ipcPlayed) {
        s_ipcPlayed = true;
        sound::IpcReady();
    }
    if (!leds.ipc) s_ipcPlayed = false;

    if (leds.gameContext && !s_gcPlayed) {
        s_gcPlayed = true;
        sound::GameContextReady();
    }
    if (!leds.gameContext) s_gcPlayed = false;
}

void MaybeLaunchFeedbackFromLog(const std::wstring& line) {
    if (line.find(L"[OK] 一键启动并注入完成") != std::wstring::npos) {
        sound::LaunchOk();
        notify::PushLocal(/*Success*/ 1, "launch-ok", "启动成功", "一键登录并注入完成。", 4200);
        return;
    }
    if (line.find(L"[OK] 附着注入完成") != std::wstring::npos) {
        sound::LaunchOk();
        notify::PushLocal(/*Success*/ 1, "attach-ok", "注入成功", "已附着注入到手动启动的游戏。",
                          4200);
        if (gLogUi) gLogUi->status = attach_inject::StatusBrief();
        return;
    }
    if (line.find(L"[FAIL] 附着注入未完成") != std::wstring::npos ||
        line.find(L"[FAIL] 立即注入：") != std::wstring::npos) {
        sound::LaunchFail();
        notify::PushLocal(/*Warning*/ 2, "attach-fail", "注入失败", "附着注入未完成，详见启动日志。",
                          6000);
        return;
    }
    if (line.find(msc::weblogin::kHttpBusyTag) != std::wstring::npos) {
        if (gLogUi) gLogUi->status = "HTTP 登录换票中…";
        return;
    }
    if (line.find(msc::weblogin::kHttpTicketOkTag) != std::wstring::npos) {
        if (gLogUi) gLogUi->status = "HTTP 换票成功，正在开游戏…";
        return;
    }
    if (line.find(msc::weblogin::kHttpTimeoutTag) != std::wstring::npos) {
        if (gLogUi) gLogUi->status = "HTTP 换票超时（约5分钟），请重试或检查网络";
        sound::UiError();
        notify::PushLocal(/*Warning*/ 2, "launch-http-timeout", "HTTP 换票超时",
                          "约5分钟仍未完成换票，请检查网络后重试。", 7000);
        return;
    }
    // 验证码 / 二次验证：状态栏 + 通知，引导自行网页登录一次
    if (line.find(msc::weblogin::kNeedWebVerifyTag) != std::wstring::npos ||
        line.find(L"[CaptchaRequired]") != std::wstring::npos ||
        line.find(L"[DualVerifyRequired]") != std::wstring::npos) {
        if (gLogUi) {
            gLogUi->status =
                "需要网页验证：请在浏览器或弹出的登录窗完成验证码后，再点一键启动";
        }
        notify::PushLocal(
            /*Warning*/ 2, "launch-captcha", "需要网页验证",
            "请在浏览器或弹出的登录窗完成登录后，再点「一键启动」。",
            9000);
        sound::UiError();
        return;
    }
    if (line.find(L"[FAIL] 换票失败") != std::wstring::npos ||
        line.find(L"[FAIL] 启动失败") != std::wstring::npos ||
        line.find(L"[FAIL] 注入未完成") != std::wstring::npos ||
        line.find(L"[FAIL] HTTP 登录失败") != std::wstring::npos ||
        line.find(L"[FAIL] 登录失败") != std::wstring::npos) {
        sound::LaunchFail();
        const std::string body = xcat::WideToUtf8(line);
        // CDP 防呆：浏览器已开无调试口 — 用 Warning + 明确标题（气泡通知）
        const bool browserBusy =
            line.find(L"未开启调试口") != std::wstring::npos ||
            (line.find(L"调试口") != std::wstring::npos &&
             (line.find(L"关闭") != std::wstring::npos ||
              line.find(L"占用") != std::wstring::npos ||
              line.find(L"超时") != std::wstring::npos));
        if (gLogUi) {
            gLogUi->status = browserBusy ? "请先关闭已打开的浏览器后再一键启动"
                                         : (line.find(L"[FAIL] HTTP 登录失败") != std::wstring::npos
                                                ? "HTTP 登录失败（详见日志）；遇验证码请先网页登录一次"
                                                : "登录/换票失败（详见通知与日志）");
        }
        if (browserBusy) {
            notify::PushLocal(/*Warning*/ 2, "launch-browser-busy", "请先关闭浏览器",
                              body.c_str(), 9000);
        } else {
            notify::PushLocal(/*Danger*/ 3, "launch-fail", "启动失败", body.c_str(), 6500);
        }
        return;
    }
}

void DrawKillButton(AppWindow& app, LaunchUiState& ui) {
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.5f));
    if (app.exiting) {
        ImGui::BeginDisabled();
        ImGui::Button("正在退出…", ImVec2(-1.f, 0.f));
        ImGui::EndDisabled();
        return;
    }
    if (UpdateNeedsVisibleUi()) {
        ImGui::BeginDisabled();
        ImGui::Button("更新进行中，请稍候…", ImVec2(-1.f, 0.f));
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("客户端正在检查/下载/安装更新，完成后窗口会自动关闭或恢复。");
        return;
    }

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float usable = (std::max)(2.f, rowW - gap);
    const float exitW = (std::max)(1.f, usable * 0.80f);
    const float relaunchW = (std::max)(1.f, usable - exitW);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.14f, 0.16f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.18f, 0.22f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.22f, 0.26f, 1.f));
    if (ImGui::Button("退出XCAT和游戏", ImVec2(exitW, 0.f))) {
        app.exiting = true;
        app.exitStartedTick = GetTickCount64();
        xcat::log::Info("App", "graceful exit: requested");
        sound::UiShutdownAsync(app.hwnd, WM_XCAT_GRACEFUL_EXIT_DONE);
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(
        "结束游戏进程并关闭本程序。\n"
        "标题栏 × 只关启动器窗口，不杀游戏。");

    ImGui::SameLine(0.f, gap);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.38f, 0.58f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.48f, 0.72f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.32f, 0.50f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.97f, 1.f, 1.f));
    if (ImGui::Button("重启游戏", ImVec2(relaunchW, 0.f))) {
        if (hangup_schedule::RequestManualCleanRelaunch(ui)) {
            sound::UiClick();
            xcat::log::Info("App", "manual clean relaunch requested");
        } else {
            sound::UiError();
        }
    }
    ImGui::PopStyleColor(4);
    ImGui::SetItemTooltip(
        "结束游戏进程，等退净后自动一键冷启。\n"
        "本程序保持运行；非挂机时段不可用。");
}

void FinishGracefulExit(AppWindow& app, LaunchUiState& /*ui*/, const char* reason) {
    xcat::log::Info("App", "graceful exit: finish (%s)", reason ? reason : "?");
    if (UpdateNeedsVisibleUi()) {
        xcat::log::Warn("App", "graceful exit deferred: update in progress");
        app.exiting = false;
        app.exitStartedTick = 0;
        return;
    }
    const unsigned n = xcat::KillProcessesByExeName(L"Maplestory_Classic.exe");
    xcat::log::Info("App", "graceful exit: killed Maplestory_Classic.exe x%u", n);
    msc::weblogin::Shutdown();
    app.exiting = false;
    app.exitStartedTick = 0;
    app.running = false;
}

}  // namespace

void LaunchPanel_LoadAccount(LaunchUiState& ui) {
    const std::wstring line = msc::weblogin::LoadSavedAccountLine();
    if (line.empty()) return;
    const std::string utf8 = xcat::WideToUtf8(line);
    if (utf8.size() >= sizeof(ui.accountLine)) {
        memcpy(ui.accountLine, utf8.data(), sizeof(ui.accountLine) - 1);
        ui.accountLine[sizeof(ui.accountLine) - 1] = '\0';
    } else {
        memcpy(ui.accountLine, utf8.data(), utf8.size());
        ui.accountLine[utf8.size()] = '\0';
    }
    SoftWrapAccountBuffer(ui.accountLine, sizeof(ui.accountLine));
}

void LaunchPanel_FormatAccountForUi(LaunchUiState& ui) {
    SoftWrapAccountBuffer(ui.accountLine, sizeof(ui.accountLine));
}

void LaunchPanel_SaveAccount(LaunchUiState& ui) {
    msc::weblogin::SaveAccountLine(Utf8ToWide(ui.accountLine));
    xcat::log::Info("App", "account config saved");
}

void LaunchPanel_AppendLog(LaunchUiState& ui, const std::wstring& line) {
    const std::string utf8 = xcat::WideToUtf8(line);
    if (!ui.logTail.empty() && ui.logTail.back() != '\n') ui.logTail.push_back('\n');
    ui.logTail += utf8;
    if (!utf8.empty() && utf8.back() != '\n') ui.logTail.push_back('\n');
    TrimLogTail(ui.logTail);
    MaybeLaunchFeedbackFromLog(line);
}

void LaunchPanel_OnWebLog(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(gLogUiMu);
    if (gLogUi) LaunchPanel_AppendLog(*gLogUi, line);
}

bool LaunchPanel_AccountLooksValid(const LaunchUiState& ui, std::wstring* errOut) {
    std::wstring err;
    const bool ok = msc::weblogin::TryParseAccountLine(Utf8ToWide(ui.accountLine), err);
    if (errOut) *errOut = err;
    return ok;
}

void LaunchPanel_ArmStrategyPrep(LaunchUiState& ui, DWORD ms) {
    if (ms == 0) {
        ui.autoLaunchNotBeforeMs = 0;
        return;
    }
    ui.autoLaunchNotBeforeMs = GetTickCount() + ms;
}

unsigned LaunchPanel_StrategyPrepLeftSec(LaunchUiState& ui) {
    if (ui.autoLaunchNotBeforeMs == 0) return 0;
    const DWORD now = GetTickCount();
    if (now >= ui.autoLaunchNotBeforeMs) {
        ui.autoLaunchNotBeforeMs = 0;
        return 0;
    }
    return (ui.autoLaunchNotBeforeMs - now + 999u) / 1000u;
}

bool LaunchPanel_CancelPendingAutoLaunch(LaunchUiState& ui) {
    if (!ui.pendingAutoLaunch && ui.autoLaunchNotBeforeMs == 0) return false;
    ui.pendingAutoLaunch = false;
    ui.autoLaunchNotBeforeMs = 0;
    return true;
}

bool LaunchPanel_StartOneClick(LaunchUiState& ui, bool honorStrategyPrep) {
    if (honorStrategyPrep) {
        if (const unsigned prepLeft = LaunchPanel_StrategyPrepLeftSec(ui)) {
            ui.status = "启动策略刚改过：约 " + std::to_string(prepLeft) +
                        " 秒后可启动（防误触）";
            xcat::log::Info("App", "one-click blocked: strategy prep %us", prepLeft);
            return false;
        }
    }
    if (!hangup_schedule::AllowsLaunch(ui.prefsBinDir)) {
        ui.status = "挂机时段：当前为非挂机小时，已跳过启动";
        xcat::log::Info("App", "one-click blocked: hangup schedule off-hour");
        LaunchPanel_AppendLog(ui, L"[Hangup] 非挂机时段，跳过启动");
        return false;
    }
    // 按启动模式对齐取票策略：GAMA PASS / gamania (HK)；不把 HK 钉死成 GamaPass。
    const auto mode = attach_inject::GetLaunchMode();
    if (mode == attach_inject::LaunchMode::GamaPassAuto) {
        msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
    } else if (mode == attach_inject::LaunchMode::OneClickLogin) {
        if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
            msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
        }
    }
    const bool needCreds = msc::weblogin::AuthStrategyNeedsAccountCreds(
        msc::weblogin::GetAuthStrategy());
    if (needCreds) {
        LaunchPanel_SaveAccount(ui);
        std::wstring parseErr;
        if (!LaunchPanel_AccountLooksValid(ui, &parseErr)) {
            ui.status = parseErr.empty() ? "账号串为空或无法识别，未启动"
                                         : xcat::WideToUtf8(parseErr);
            xcat::log::Warn("App", "one-click skipped: %s", ui.status.c_str());
            sound::UiError();
            notify::PushLocal(/*Warning*/ 2, "launch-account", "账号无效", ui.status.c_str(),
                              4500);
            return false;
        }
    }
    if (!msc::weblogin::CanStartOneClick()) {
        ui.status = "登录会话未就绪";
        sound::UiError();
        return false;
    }
    if (msc::weblogin::IsBusy()) {
        ui.status = "正在登录/换票中…";
        return false;
    }
    sound::UiClick();
    std::wstring err;
    // GAMA PASS：传空账号串，避免触发账密路径
    const std::wstring accountArg =
        needCreds ? Utf8ToWide(ui.accountLine) : std::wstring{};
    if (!msc::weblogin::StartOneClick(accountArg, err)) {
        ui.status = err.empty() ? "启动失败" : xcat::WideToUtf8(err);
        sound::UiError();
        return false;
    }
    ui.status = needCreds ? "已开始一键登录/换票/开游戏/注入"
                          : "已开始 GAMA PASS（浏览器会话）换票/开游戏/注入";
    // 任意入口成功启动后，取消切策略留下的自动待办，避免准备窗到期再打一发。
    ui.pendingAutoLaunch = false;
    ui.autoLaunchNotBeforeMs = 0;
    xcat::log::Ok("App", needCreds ? "one-click launch+inject started"
                                   : "GamaPass session-only launch+inject started");
    hangup_schedule::NoteLaunchStarted(0);
    return true;
}

void LaunchPanel_TryAutoLaunchWhenReady(LaunchUiState& ui) {
    if (!ui.pendingAutoLaunch) return;

    if (const unsigned prepLeft = LaunchPanel_StrategyPrepLeftSec(ui)) {
        if (attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode())) {
            ui.status = "手动模式：约 " + std::to_string(prepLeft) +
                        " 秒后自动开始监视（可再点按钮取消）";
        } else {
            ui.status = "GAMA PASS：约 " + std::to_string(prepLeft) +
                        " 秒后自动换票（可再点按钮取消）";
        }
        return;
    }

    if (attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode())) {
        // 附着模式不依赖 WebView / 账号；也不受挂机时段拦（用户已手动开游戏语义）。
        ui.pendingAutoLaunch = false;
        if (attach_inject::IsWatching()) {
            ui.status = attach_inject::StatusBrief();
            return;
        }
        if (attach_inject::StartWatch()) {
            ui.status = "监视中：等待游戏进程…（请手动开游戏）";
            xcat::log::Info("App", "auto-watch started (AttachWatch)");
            hangup_schedule::NoteLaunchStarted(0);
        } else {
            ui.status = "自动监视启动失败";
            xcat::log::Warn("App", "auto-watch StartWatch failed");
        }
        return;
    }

    // 一键类模式：GAMA PASS 自动换票；gamania (HK) 等用户点（冷启不自动填账密跑）。
    if (attach_inject::GetLaunchMode() == attach_inject::LaunchMode::GamaPassAuto) {
        msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
    } else if (attach_inject::GetLaunchMode() == attach_inject::LaunchMode::OneClickLogin) {
        if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
            msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
        }
        ui.pendingAutoLaunch = false;
        ui.status = "gamania (HK)：请粘贴账密后点「一键启动游戏」";
        return;
    }
    if (!msc::weblogin::CanStartOneClick()) return;
    if (msc::weblogin::IsBusy()) {
        // 挂机/守护等已在换票中：吃掉 pending，防止结束后再自动打第二发。
        ui.pendingAutoLaunch = false;
        ui.autoLaunchNotBeforeMs = 0;
        return;
    }

    if (!hangup_schedule::AllowsLaunch(ui.prefsBinDir)) {
        ui.pendingAutoLaunch = false;
        ui.status = "非挂机时段，已跳过冷启自动启动（时段到后由挂机调度拉起）";
        xcat::log::Info("App", "auto-launch skipped: hangup schedule off-hour");
        LaunchPanel_AppendLog(ui, L"[Hangup] 非挂机时段，跳过冷启自动启动");
        return;
    }

    ui.pendingAutoLaunch = false;
    if (LaunchPanel_StartOneClick(ui)) {
        xcat::log::Info("App", "auto GamaPass launch started");
    } else {
        xcat::log::Warn("App", "auto GamaPass launch failed: %s", ui.status.c_str());
    }
}

bool PollGracefulExit(AppWindow& app, LaunchUiState& ui) {
    if (!app.exiting) return false;
    if (app.pendingExitAfterSound.exchange(false)) {
        FinishGracefulExit(app, ui, "requested");
        return !app.running;
    }
    if (app.exitStartedTick && GetTickCount64() - app.exitStartedTick >= 2000) {
        FinishGracefulExit(app, ui, "timeout");
        return !app.running;
    }
    return false;
}

void DrawMainShell(AppWindow& app, LaunchUiState& ui) {
    {
        std::lock_guard<std::mutex> lock(gLogUiMu);
        gLogUi = &ui;
    }

    // 实时行：公共日志 → 面板；与 WebView 文本日志合并显示。
    static bool s_logCbArmed = false;
    if (!s_logCbArmed) {
        s_logCbArmed = true;
        xcat::log::SetLineCallback([](const char* line) {
            if (!line || !line[0]) return;
            std::lock_guard<std::mutex> lock(gLogUiMu);
            if (!gLogUi) return;
            LaunchPanel_AppendLog(*gLogUi, Utf8ToWide(line));
        });
    }

    if (PollGracefulExit(app, ui)) return;

    const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
    PollMilestoneSounds(leds);
    notify::Poll(ui.prefsBinDir);

    if (app.hotkeyF10.exchange(false)) {
        std::string err;
        if (TriggerManualRejoin(ui.prefsBinDir, /*requireInjected=*/true, &err)) {
            notify::PushLocal(/*Info*/ 0, "manual-rejoin", "随机换频已触发",
                              "F10：已下发换频命令。", 3500);
        } else {
            notify::PushLocal(/*Warning*/ 2, "manual-rejoin-fail", "随机换频失败",
                              err.empty() ? "未注入或写盘失败" : err.c_str(), 4200);
        }
    }

    ui::LauncherFrame frame(0.f);
    if (!frame.visible) return;

    ui::DrawLauncherTopBar(app, leds, ui.prefsBinDir);
    DrawLauncherStatusBar(ui, leds, app.launchTickMs);

    constexpr int kTabCount = static_cast<int>(WorkspaceTab::Count);
    if (ui.activeTab < 0 || ui.activeTab >= kTabCount)
        ui.activeTab = static_cast<int>(WorkspaceTab::Home);
    ui.activeTab = ui::DrawWorkspaceTabStrip(ui.activeTab, kWorkspaceTabLabels, kTabCount);
    ui::DrawWorkspaceContentSeparator();

    ImGui::BeginChild("##launcher_workspace", ImVec2(0.f, -ui::LauncherFooterReserve()),
                      ImGuiChildFlags_None);
    DrawWorkspaceTabContent(app, ui, ui.activeTab);
    ImGui::EndChild();

    DrawKillButton(app, ui);
    notify::Draw(app.dpiScale);
    eventlog::DrawWindow(app.dpiScale);
}

}  // namespace xcat::app
