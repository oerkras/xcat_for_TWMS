#include "status_bar.h"

#include "app_dpi.h"
#include "app_sound.h"
#include "app_theme.h"
#include "attach_inject.h"
#include "hangup_schedule.h"
#include "imgui_shell.h"
#include "launch_panel.h"
#include "update_client.h"

#include "msc_webview_login.h"
#include "xcat_log.h"
#include "xcat_payload_status.h"
#include "xcat_version.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace xcat::app {
namespace {

constexpr int kStatusRows = 4;

void FormatDurationHms(uint64_t sec, char* out, size_t outN) {
    if (!out || outN == 0) return;
    const uint64_t h = sec / 3600u;
    const uint64_t m = (sec % 3600u) / 60u;
    const uint64_t s = sec % 60u;
    if (h > 0)
        snprintf(out, outN, "%llu:%02llu:%02llu", static_cast<unsigned long long>(h),
                 static_cast<unsigned long long>(m), static_cast<unsigned long long>(s));
    else
        snprintf(out, outN, "%llu:%02llu", static_cast<unsigned long long>(m),
                 static_cast<unsigned long long>(s));
}

const char* SessionPhase(const RuntimeLeds& leds) {
    if (msc::weblogin::IsBusy()) {
        if (attach_inject::GetLaunchMode() == attach_inject::LaunchMode::GamaPassAuto ||
            msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto)
            return "GAMA PASS登录中";
        return "换票中";
    }
    if (hangup_schedule::IsCleanRelaunchInFlight()) return "干净重拉中";
    if (leds.gameContext) return "游戏运行中";
    if (leds.ipc) return "空闲";
    return "初始化";
}

void DrawUpdateProgressMini() {
    if (!UpdateShouldDrawProgressUi()) return;
    const UpdateSnapshot snap = GetUpdateSnapshot();
    float frac = -1.f;
    if (snap.phase == UpdatePhase::UpToDate) {
        frac = 1.f;
    } else if (snap.phase == UpdatePhase::Failed) {
        frac = 0.f;
    } else if (snap.progress >= 0.f && snap.progress <= 1.f) {
        frac = snap.progress;
    } else {
        frac = -1.0f * static_cast<float>(ImGui::GetTime());
    }
    ImGui::ProgressBar(frac, ImVec2(AppDpi_Px(140.f), AppDpi_Px(6.f)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !snap.message.empty()) {
        ImGui::SetTooltip("%s", snap.message.c_str());
    }
}

void DrawCompactWatchdogLine() {
    const hangup_schedule::Snapshot snap = hangup_schedule::GetSnapshot();
    if (!snap.watchdogOn) {
        ImGui::TextDisabled("守护：关闭");
        return;
    }
    const std::string timer = hangup_schedule::FormatWatchdogTimerText(snap);
    if (!snap.scheduleActive) {
        ImGui::TextDisabled("守护：%s", timer.c_str());
        return;
    }
    if (snap.watchdogMode == hangup_schedule::WatchdogUiMode::Backoff &&
        snap.backoffRemainingSec > 0) {
        ImGui::Text("守护：%s %s", hangup_schedule::WatchdogUiModeLabel(snap.watchdogMode),
                    timer.c_str());
        return;
    }
    if (snap.watchdogMode == hangup_schedule::WatchdogUiMode::Recovering) {
        ImGui::Text("守护：%s %s", hangup_schedule::WatchdogUiModeLabel(snap.watchdogMode),
                    timer.c_str());
        return;
    }
    ImGui::Text("守护：%s %s", hangup_schedule::WatchdogUiModeLabel(snap.watchdogMode),
                timer.c_str());
}

void DrawCcuText(const LaunchUiState& ui) {
    char ccu[64]{};
    bool show = false;
    if (!ui.prefsBinDir.empty()) {
        xcat::PayloadStatus st{};
        if (xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) &&
            xcat::PayloadStatusFresh(st, GetTickCount64(), 120000ull) &&
            st.worldChannelOnline >= 0) {
            snprintf(ccu, sizeof(ccu), "实时在线人数 %lld人", st.worldChannelOnline);
            show = true;
        }
    }
    if (show) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.22f, 1.0f), "%s", ccu);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.22f, 1.0f), "实时在线人数 --");
    }
}

void BeginStatusRow(const ImVec2& origin, float rowH, int row) {
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowH * static_cast<float>(row)));
}

// 昼夜都清晰的提示蓝：白天深蓝、黑夜亮蓝（对齐 brandText）。
ImVec4 StatusHintBlue() {
    if (AppTheme_IsLight()) return ImVec4(0.00f, 0.33f, 0.65f, 1.0f);  // ~#0054A6
    return AppTheme_Palette().brandText;                                 // ~#76BAFF
}

ImVec4 StatusAlertBlue() {
    // 失败/拦截：同系略沉，仍保持蓝调可读（非暖黄/橙）。
    if (AppTheme_IsLight()) return ImVec4(0.00f, 0.28f, 0.58f, 1.0f);
    return ImVec4(0.55f, 0.78f, 1.0f, 1.0f);
}

bool StatusLooksActionable(const std::string& s) {
    if (s.empty()) return false;
    // 结构化前缀优先，少靠零散单字（避免无关「跳过」误亮）。
    static const char* kPrefixes[] = {
        "GAMA PASS", "HTTP ", "正在", "已开始", "已取消", "挂机", "守护", "监视",
        "请先关闭", "需要网页", "登录会话", "非挂机", "清理 NGM", "干净重拉",
        "启动失败", "登录/换票失败", "HTTP 登录失败", "注入失败", "自动监视启动失败",
    };
    for (const char* p : kPrefixes) {
        if (s.rfind(p, 0) == 0 || s.find(p) != std::string::npos) return true;
    }
    return s.find("失败") != std::string::npos;
}

void DrawStatusEllipsis(const ImVec4& col, const std::string& text) {
    const float maxW = ImGui::GetContentRegionAvail().x;
    if (maxW <= 8.f || ImGui::CalcTextSize(text.c_str()).x <= maxW) {
        ImGui::TextColored(col, "%s", text.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !text.empty())
            ImGui::SetTooltip("%s", text.c_str());
        return;
    }
    // 按显示宽度收成「…」；UTF-8 按字节回退到字符边界。
    std::string shown = text;
    const char* ell = "\xE2\x80\xA6";  // UTF-8 …
    while (shown.size() > 1) {
        size_t cut = shown.size();
        while (cut > 0 && (static_cast<unsigned char>(shown[cut - 1]) & 0xC0) == 0x80) --cut;
        if (cut == 0) break;
        shown.resize(cut - 1);
        const std::string trial = shown + ell;
        if (ImGui::CalcTextSize(trial.c_str()).x <= maxW) {
            ImGui::TextColored(col, "%s", trial.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", text.c_str());
            return;
        }
    }
    ImGui::TextColored(col, "%s", ell);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text.c_str());
}

}  // namespace

void DrawLauncherStatusBar(LaunchUiState& ui, const RuntimeLeds& leds, uint64_t launchTickMs) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, AppTheme_Palette().statusStripBg);
    ImGui::PushStyleColor(ImGuiCol_Border, AppTheme_Palette().statusStripBorder);
    // 加大内边距，避免贴边拥挤。
    const ImVec2 pad(ui::Pad() * 0.85f, ui::Gap() * 0.75f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Gap(), ui::Gap() * 0.35f));

    const float rowH = ImGui::GetFrameHeightWithSpacing();
    const float stripH = pad.y * 2.f + rowH * static_cast<float>(kStatusRows);

    const bool stripOpen =
        ImGui::BeginChild("##launcher_status_strip", ImVec2(0.f, stripH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (stripOpen) {
        // 0 = 不换行。右侧剩余宽度变窄时，禁止把 "PID 12345" 拆成竖条。
        ImGui::PushTextWrapPos(0.f);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const bool busy = msc::weblogin::IsBusy() || attach_inject::IsInjectBusy();
        const UpdateSnapshot snap = GetUpdateSnapshot();
        const bool attachMode =
            attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode());
        const unsigned strategyPrepLeft = LaunchPanel_StrategyPrepLeftSec(ui);
        const bool autoPending = ui.pendingAutoLaunch;
        const bool canStart =
            attachMode
                ? (!busy &&
                   (attach_inject::IsWatching() || strategyPrepLeft == 0 || autoPending))
                : (!busy && (strategyPrepLeft == 0 || autoPending));

        // —— 1/4 产品 + 版本（脱敏：不写服名/游戏名）——
        BeginStatusRow(origin, rowH, 0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("XCat");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("%s", xcat::kXcatVersionString);
        if (snap.latestBuildId > 0) {
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::TextDisabled("|");
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::TextDisabled("最新 build %u", snap.latestBuildId);
        }

        // —— 2/4 运行时间 + 阶段 + PID + 启动 ——
        BeginStatusRow(origin, rowH, 1);
        ImGui::AlignTextToFramePadding();
        {
            const uint64_t now = GetTickCount64();
            const uint64_t start = launchTickMs ? launchTickMs : now;
            char dur[24]{};
            FormatDurationHms((now >= start ? now - start : 0u) / 1000u, dur, sizeof(dur));
            ImGui::TextDisabled("XCat运行时间 %s", dur);
        }
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextUnformatted(SessionPhase(leds));
        if (leds.gamePid) {
            ImGui::SameLine(0.f, ui::Gap());
            // 预格式化成单段字符串，避免窄缝里按空格拆行。
            char pidBuf[32]{};
            snprintf(pidBuf, sizeof(pidBuf), "PID %lu", leds.gamePid);
            ImGui::TextDisabled("%s", pidBuf);
        }
        ImGui::SameLine(0.f, ui::Gap());
        if (!canStart) ImGui::BeginDisabled();
        const char* startLabel =
            autoPending ? "取消"
                        : (attachMode ? (attach_inject::IsWatching() ? "监视中" : "监视")
                                      : "启动");
        if (ImGui::SmallButton(startLabel)) {
            if (autoPending) {
                sound::UiClick();
                LaunchPanel_CancelPendingAutoLaunch(ui);
                ui.status = attachMode ? "已取消自动监视 — 需要时再点「监视」"
                                       : (attach_inject::GetLaunchMode() ==
                                                  attach_inject::LaunchMode::OneClickLogin
                                              ? "已取消自动启动 — 需要时再点「启动」"
                                              : "已取消自动换票 — 需要时再点「启动」");
                xcat::log::Info("App", "user cancelled pending auto-launch (status bar)");
            } else if (attachMode) {
                if (!attach_inject::IsWatching()) {
                    if (attach_inject::StartWatch()) {
                        ui.pendingAutoLaunch = false;
                        ui.autoLaunchNotBeforeMs = 0;
                        ui.status = "监视中：等待游戏进程…";
                        hangup_schedule::NoteLaunchStarted(0);
                    }
                }
            } else {
                if (attach_inject::GetLaunchMode() == attach_inject::LaunchMode::GamaPassAuto) {
                    msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
                } else if (msc::weblogin::GetAuthStrategy() ==
                           msc::weblogin::AuthStrategy::GamaPassAuto) {
                    msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
                }
                LaunchPanel_StartOneClick(ui);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (autoPending) {
                ImGui::SetTooltip("取消即将开始的自动%s",
                                  attachMode ? "监视" : "换票/启动");
            } else if (strategyPrepLeft > 0 &&
                       !(attachMode && attach_inject::IsWatching())) {
                ImGui::SetTooltip("启动策略刚改过：约 %u 秒后可启动（防误触）",
                                  strategyPrepLeft);
            } else {
                ImGui::SetTooltip(attachMode ? "手动开游戏后自动检测并注入"
                                             : "一键换票 / 开游戏 / Classic 注入");
            }
        }
        if (!canStart) ImGui::EndDisabled();

        // —— 3/4 守护 + 在线 ——
        BeginStatusRow(origin, rowH, 2);
        ImGui::AlignTextToFramePadding();
        DrawCompactWatchdogLine();
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.f, ui::Gap());
        DrawCcuText(ui);

        // —— 4/4 状态 / 更新 ——
        // 用户默认停在首页 TAB，倒计时/自动登录/干净重拉必须在顶部可见。
        BeginStatusRow(origin, rowH, 3);
        ImGui::AlignTextToFramePadding();
        const hangup_schedule::Snapshot hs = hangup_schedule::GetSnapshot();
        const bool relaunching = hangup_schedule::IsCleanRelaunchInFlight();
        const bool hangupStarting =
            hs.hangupOn && hs.mode == hangup_schedule::UiMode::Starting;
        const bool watchdogRecovering =
            hs.watchdogOn && hs.watchdogMode == hangup_schedule::WatchdogUiMode::Recovering;
        const bool showLoginHint =
            !ui.status.empty() &&
            (busy || autoPending || relaunching || hangupStarting || watchdogRecovering ||
             StatusLooksActionable(ui.status));
        const bool showUpdate = UpdateShouldDrawProgressUi();
        // 自动登录提示优先于更新条：用户默认在首页，换票进度更紧急。
        if (showLoginHint) {
            const bool inFlight = busy || autoPending || relaunching || hangupStarting ||
                                  watchdogRecovering;
            DrawStatusEllipsis(inFlight ? StatusHintBlue() : StatusAlertBlue(), ui.status);
        } else if (showUpdate) {
            DrawUpdateProgressMini();
            if (!snap.message.empty()) {
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::TextDisabled("%s", snap.message.c_str());
            }
        } else {
            ImGui::TextDisabled(" ");
        }

        ImGui::PopTextWrapPos();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.55f));
}

}  // namespace xcat::app
