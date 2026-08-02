#include "status_bar.h"

#include "app_dpi.h"
#include "app_theme.h"
#include "hangup_schedule.h"
#include "imgui_shell.h"
#include "update_client.h"

#include "msc_webview_login.h"
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
    if (msc::weblogin::IsBusy()) return "换票中";
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
        const bool busy = msc::weblogin::IsBusy();
        const UpdateSnapshot snap = GetUpdateSnapshot();
        const bool canStart = !busy && leds.ipc;

        // —— 1/4 服名 + 版本 ——
        BeginStatusRow(origin, rowH, 0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("新楓之谷：經典版");
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
        if (ImGui::SmallButton("启动")) {
            LaunchPanel_StartOneClick(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("一键登录 / 换票 / 开游戏 / Classic 注入");
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
        BeginStatusRow(origin, rowH, 3);
        ImGui::AlignTextToFramePadding();
        const bool showStatus =
            !ui.status.empty() && (busy || ui.status.find("失败") != std::string::npos);
        const bool showUpdate = UpdateShouldDrawProgressUi();
        if (showUpdate) {
            DrawUpdateProgressMini();
            if (!snap.message.empty()) {
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::TextDisabled("%s", snap.message.c_str());
            }
        } else if (showStatus) {
            ImGui::TextDisabled("%s", ui.status.c_str());
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
