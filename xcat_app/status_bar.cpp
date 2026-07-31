#include "status_bar.h"

#include "app_dpi.h"
#include "app_theme.h"
#include "imgui_shell.h"
#include "update_client.h"

#include "msc_webview_login.h"
#include "xcat_version.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace xcat::app {
namespace {


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
    ImGui::ProgressBar(frac, ImVec2(-1.f, AppDpi_Px(4.f)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !snap.message.empty()) {
        ImGui::SetTooltip("%s", snap.message.c_str());
    }
}

}  // namespace

void DrawLauncherStatusBar(LaunchUiState& ui, const RuntimeLeds& leds) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, AppTheme_Palette().statusStripBg);
    ImGui::PushStyleColor(ImGuiCol_Border, AppTheme_Palette().statusStripBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Pad() * 0.7f, ui::Gap() * 0.55f));
    const bool stripOpen =
        ImGui::BeginChild("##launcher_status_strip", ImVec2(0.f, 0.f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

    if (stripOpen) {
        ImGui::TextUnformatted("新楓之谷：經典版");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("%s", xcat::kXcatVersionString);
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.f, ui::Gap());

        if (leds.ipc && leds.webReadyTickMs) {
            char dur[24]{};
            FormatDurationHms((GetTickCount64() - leds.webReadyTickMs) / 1000u, dur, sizeof(dur));
            ImGui::TextDisabled("WebView %s", dur);
        } else {
            ImGui::TextDisabled("WebView --");
        }

        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextUnformatted(SessionPhase(leds));

        if (leds.gamePid) {
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::TextDisabled("PID %lu", leds.gamePid);
        }

        const bool busy = msc::weblogin::IsBusy();
        if (!busy && leds.ipc) {
            ImGui::SameLine(0.f, ui::Gap());
            if (ImGui::SmallButton("启动")) {
                LaunchPanel_StartOneClick(ui);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("一键登录 / 换票 / 开游戏 / Classic 注入");
        }

        const bool updateBusy = UpdateNeedsVisibleUi();
        ImGui::SameLine(0.f, ui::Gap());
        if (updateBusy) ImGui::BeginDisabled();
        if (ImGui::SmallButton("检查更新")) {
            (void)StartUpdateCheck(kDefaultUpdateServiceUrl);
        }
        if (updateBusy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (updateBusy) {
                ImGui::SetTooltip("更新进行中，请等待当前流程完成。");
            } else {
                ImGui::SetTooltip("检查 %s/update/latest.json；有新版本则自动下载并安装。",
                                   kDefaultUpdateServiceUrl);
            }
        }

        const UpdateSnapshot snap = GetUpdateSnapshot();
        if (snap.latestBuildId > 0) {
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::TextDisabled("最新 build %u", snap.latestBuildId);
        }

        if (!ui.status.empty() && (busy || ui.status.find("失败") != std::string::npos)) {
            ImGui::TextDisabled("%s", ui.status.c_str());
        }

        if (UpdateShouldDrawProgressUi()) {
            DrawUpdateProgressMini();
            if (!snap.message.empty()) {
                ImGui::TextDisabled("%s", snap.message.c_str());
            }
        }

        ImGui::TextDisabled("守护 --");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.22f, 1.0f), "实时在线人数 --");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.5f));
}

}  // namespace xcat::app
