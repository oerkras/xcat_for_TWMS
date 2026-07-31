#pragma once

// TWMS launcher 壳：标题栏（含 LED）+ 状态条 + 工作区 Tab + 主帧

#include "app_dpi.h"
#include "app_event_log.h"
#include "app_notify.h"
#include "app_sound.h"
#include "app_theme.h"
#include "app_window.h"
#include "runtime_leds.h"

#include "xcat_imgui_theme.h"
#include "xcat_log.h"
#include "xcat_version.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace xcat::app::ui {

inline float Gap() { return AppDpi_Px(6.f); }
inline float Pad() { return AppDpi_Px(10.f); }
inline float BtnH() { return AppDpi_Px(28.f); }

inline AppThemeMode ThemeModeCycleNext(AppThemeMode cur) {
    return cur == AppThemeMode::Dark ? AppThemeMode::Light : AppThemeMode::Dark;
}

inline float TopBarThemeToggleWidth(float titleH) {
    const char* label = AppTheme_IsLight() ? "暗夜" : "白天";
    const ImVec2 ts = ImGui::CalcTextSize(label);
    return (std::max)(titleH, ts.x + ImGui::GetStyle().FramePadding.x * 2.f + AppDpi_Px(8.f));
}

inline void DrawTopBarThemeToggle(const std::string& prefsBinDir, float titleH) {
    const char* label = AppTheme_IsLight() ? "暗夜##theme" : "白天##theme";
    const float w = TopBarThemeToggleWidth(titleH);
    if (ImGui::Button(label, ImVec2(w, titleH))) {
        const AppThemeMode next = ThemeModeCycleNext(AppTheme_Mode());
        bool ok = true;
        if (!prefsBinDir.empty()) {
            ok = AppTheme_SetModePersisted(prefsBinDir.c_str(), next);
            if (!ok) {
                xcat::log::Warn("App", "theme save failed: %s",
                                AppTheme_LastSaveError() ? AppTheme_LastSaveError() : "?");
                notify::PushLocal(/*Danger*/ 3, "theme-save", "主题保存失败",
                                  AppTheme_LastSaveError() ? AppTheme_LastSaveError()
                                                           : "无法写入 user.ini",
                                  5000);
            } else {
                notify::PushLocal(/*Success*/ 1, "theme-save", "主题已保存",
                                  next == AppThemeMode::Light ? "白天模式" : "暗夜模式", 2800);
            }
        } else {
            AppTheme_SetMode(next);
        }
        sound::UiToggle();
        HWND hwnd = nullptr;
        if (ImGuiViewport* vp = ImGui::GetMainViewport()) {
            hwnd = static_cast<HWND>(vp->PlatformHandleRaw);
            if (!hwnd) hwnd = static_cast<HWND>(vp->PlatformHandle);
        }
        AppTheme_RequestCommit(hwnd);
        (void)ok;
    }
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("点击切换：暗夜 / 白天");
}


inline void DrawLauncherLedStrip(const RuntimeLeds& leds, float width, float height) {
    const bool values[5] = {leds.ipc, leds.gameContext, leds.localPlayer, leds.mapOk,
                            leds.quizCache};
    const char* labels[5] = {"IPC", "GameContext", "LocalPlayer", "Map", "Cache"};
    const char* tips[5] = {
        "IPC / WebView：会话就绪（注入后改为 payload IPC）",
        "GameContext：检测到 Maplestory_Classic.exe",
        "LocalPlayer：待注入后点亮",
        "Map：待注入后点亮",
        "Cache：测谎缓存位置（待接入）",
    };

    ImGui::InvisibleButton("##launcher_led_strip", ImVec2(width, height));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const float step = AppDpi_Px(16.f);
    const float r = AppDpi_Px(4.2f);
    const float totalW = step * 4.f;
    const float firstX = (p0.x + p1.x - totalW) * 0.5f;
    const float y = (p0.y + p1.y) * 0.5f;

    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        for (int i = 0; i < 5; ++i) {
            const ImVec2 c(firstX + step * static_cast<float>(i), y);
            const bool on = values[i];
            const ImU32 fill =
                on ? IM_COL32(64, 220, 98, 255)
                   : ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const ImVec4 glowOff = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            const ImU32 glow =
                on ? IM_COL32(64, 220, 98, 80)
                   : IM_COL32(static_cast<int>(glowOff.x * 255.f),
                              static_cast<int>(glowOff.y * 255.f),
                              static_cast<int>(glowOff.z * 255.f), 40);
            dl->AddCircleFilled(c, r + AppDpi_Px(3.f), glow, 16);
            dl->AddCircleFilled(c, r, fill, 16);
            const ImVec4 ring = ImGui::GetStyleColorVec4(ImGuiCol_Border);
            dl->AddCircle(c, r + AppDpi_Px(0.5f),
                          IM_COL32(static_cast<int>(ring.x * 255.f),
                                   static_cast<int>(ring.y * 255.f),
                                   static_cast<int>(ring.z * 255.f), on ? 120 : 55),
                          16);
        }
    }

    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int nearest = 0;
        float best = 1.0e9f;
        for (int i = 0; i < 5; ++i) {
            const float x = firstX + step * static_cast<float>(i);
            const float d = (mouse.x - x) * (mouse.x - x) + (mouse.y - y) * (mouse.y - y);
            if (d < best) {
                best = d;
                nearest = i;
            }
        }
        ImGui::SetTooltip("%s\n%s：%s", tips[nearest], labels[nearest],
                          values[nearest] ? "亮" : "灭");
    }
}

inline void DrawLauncherTopBar(AppWindow& app, const RuntimeLeds& leds,
                               const std::string& prefsBinDir) {
    const float titleH = AppDpi_Px(28.f);
    const float iconBtnW = AppDpi_Px(26.f);
    const float btnGap = AppDpi_Px(2.f);
    const float pad = Gap();
    const float brandW = AppDpi_Px(56.f);
    const float themeBtnW = TopBarThemeToggleWidth(titleH);
    const float leftChromeW = brandW + themeBtnW + btnGap;
    constexpr const char* kEventsLabel = "历史事件";
    const ImVec2 eventsText = ImGui::CalcTextSize(kEventsLabel);
    const float eventsBtnW =
        (std::max)(iconBtnW, eventsText.x + ImGui::GetStyle().FramePadding.x * 2.f);
    const float muteBtnW = notify::TopBarMuteToggleWidth(titleH);
    const float ledStripW = AppDpi_Px(88.f) + pad;
    // 右侧优先级：- /关闭 > 静音 > 历史事件 > LED
    const float captionMin = iconBtnW * 2.f + btnGap;
    const float captionWithMute = captionMin + muteBtnW + btnGap;
    const float captionWithEvents = captionWithMute + eventsBtnW + btnGap;
    const float captionFull = captionWithEvents + ledStripW;
    const float fullW = ImGui::GetContentRegionAvail().x;
    const auto fits = [&](float captionW) { return fullW >= captionW + pad + leftChromeW; };
    const bool showLeds = fits(captionFull);
    const bool showEvents = showLeds || fits(captionWithEvents);
    const bool showMute = showEvents || fits(captionWithMute);
    const float captionW = showLeds         ? captionFull
                           : showEvents     ? captionWithEvents
                           : showMute       ? captionWithMute
                                            : captionMin;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        const AppPalette& p = AppTheme_Palette();
        const ImU32 top = ImGui::ColorConvertFloat4ToU32(p.titleBarTop);
        const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(p.titleBarBottom);
        dl->AddRectFilledMultiColor(origin, ImVec2(origin.x + fullW, origin.y + titleH), top, top,
                                    bottom, bottom);
        dl->AddLine(ImVec2(origin.x, origin.y + 1.f), ImVec2(origin.x + fullW, origin.y + 1.f),
                    ImGui::ColorConvertFloat4ToU32(p.titleBarLineTop));
        dl->AddLine(ImVec2(origin.x, origin.y + titleH),
                    ImVec2(origin.x + fullW, origin.y + titleH),
                    ImGui::ColorConvertFloat4ToU32(p.titleBarLineBottom));
        char brand[64]{};
        snprintf(brand, sizeof(brand), "XCat %s", xcat::kXcatVersionString);
        dl->AddText(ImVec2(origin.x + pad, origin.y + AppDpi_Px(6.f)),
                    ImGui::ColorConvertFloat4ToU32(p.brandText), brand);
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##brand_drag", ImVec2(brandW, titleH));
    AppWindow_DragFromLastItem(app);

    ImGui::SameLine(0.f, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, AppDpi_Px(4.f));
    {
        const AppPalette& p = AppTheme_Palette();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.captionBtnHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.captionBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Text, p.hintText);
    }
    DrawTopBarThemeToggle(prefsBinDir, titleH);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::SameLine(0.f, btnGap);
    const float midDragW = (std::max)(0.f, fullW - captionW - pad - leftChromeW);
    ImGui::InvisibleButton("##title_drag", ImVec2(midDragW, titleH));
    AppWindow_DragFromLastItem(app);

    ImGui::SameLine(0.f, 0.f);
    if (showLeds) {
        DrawLauncherLedStrip(leds, AppDpi_Px(72.f), titleH);
        ImGui::SameLine(0.f, pad);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, AppDpi_Px(4.f));
    {
        const AppPalette& p = AppTheme_Palette();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.captionBtnHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.captionBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Text, p.captionBtnText);
    }
    if (showMute) {
        notify::DrawTopBarMuteToggle(prefsBinDir, titleH);
        ImGui::SameLine(0.f, btnGap);
    }
    if (showEvents) {
        if (ImGui::Button("历史事件##launcher_events", ImVec2(eventsBtnW, titleH))) {
            eventlog::ToggleWindow();
            sound::UiClick();
        }
        {
            const uint32_t unseen = eventlog::UnseenCount();
            if (unseen > 0) {
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                const float r = AppDpi_Px(6.f);
                const ImVec2 c(rmax.x - r, rmin.y + r);
                if (ImDrawList* bl = ImGui::GetWindowDrawList()) {
                    bl->AddCircleFilled(c, r, IM_COL32(240, 72, 66, 255), 12);
                    char cnt[8]{};
                    snprintf(cnt, sizeof(cnt), "%u", unseen > 99 ? 99u : unseen);
                    const ImVec2 ts = ImGui::CalcTextSize(cnt);
                    bl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                                IM_COL32(255, 255, 255, 255), cnt);
                }
            }
        }
        ImGui::SetItemTooltip("历史事件：查看本次运行期间发生了什么");
        ImGui::SameLine(0.f, btnGap);
    }
    if (ImGui::Button("-##min", ImVec2(iconBtnW, titleH))) AppWindow_Minimize(app);
    ImGui::SameLine(0.f, btnGap);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AppTheme_Palette().dangerHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AppTheme_Palette().dangerActive);
    if (ImGui::Button("x##close", ImVec2(iconBtnW, titleH))) {
        if (app.hwnd) PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    }
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar();

    app.titleBarHeightPx = static_cast<int>(titleH + 0.5f);
    app.captionButtonsWidthPx = static_cast<int>(captionW + pad + 0.5f);
    ImGui::Dummy(ImVec2(0.f, AppDpi_Px(4.f)));
}

inline int SplitTabRows(int count) {
    if (count <= 1) return count;
    return (count + 1) / 2;
}

inline bool DrawWorkspaceTabButton(const char* label, int tabIndex, int activeTab, float width) {
    const bool active = tabIndex == activeTab;
    const AppPalette& p = AppTheme_Palette();
    const ImVec4 clear(0.f, 0.f, 0.f, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, clear);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, clear);
    ImGui::PushStyleColor(ImGuiCol_Text, clear);
    ImGui::PushStyleColor(ImGuiCol_Border, clear);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);

    char id[64]{};
    snprintf(id, sizeof(id), "%s##launcher_ws_%d", label, tabIndex);
    const bool clicked = ImGui::Button(id, ImVec2(width, 0.f));
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    xcat::ui::Win7FaceKind kind = xcat::ui::Win7FaceKind::Tab;
    if (held) kind = xcat::ui::Win7FaceKind::ButtonPressed;
    else if (active) kind = xcat::ui::Win7FaceKind::TabActive;
    else if (hovered) kind = xcat::ui::Win7FaceKind::TabHot;

    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        const float round = AppTheme_IsLight() ? AppDpi_Px(2.f) : AppDpi_Px(5.f);
        xcat::ui::UiTheme_DrawWin7Face(dl, rmin, rmax, kind, round);
        if (label && label[0]) {
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const ImVec2 tp((rmin.x + rmax.x - ts.x) * 0.5f, (rmin.y + rmax.y - ts.y) * 0.5f);
            dl->AddText(tp,
                        ImGui::ColorConvertFloat4ToU32(active ? p.tabActiveText : p.tabInactiveText),
                        label);
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return clicked;
}

inline int DrawWorkspaceTabStrip(int activeTab, const char* const* labels, int count) {
    if (!labels || count <= 0) return activeTab;
    const float spacing = AppDpi_Px(3.f);
    const float padX = AppDpi_Px(5.f);
    const float rowGap = AppDpi_Px(5.f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, AppTheme_Palette().tabStripBg);
    ImGui::PushStyleColor(ImGuiCol_Border, AppTheme_Palette().tabStripBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(AppDpi_Px(5.f), AppDpi_Px(5.f)));
    const bool stripOpen = ImGui::BeginChild(
        "##tab_strip", ImVec2(0.f, 0.f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    if (stripOpen) {
        const float stripW = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, rowGap));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                            AppTheme_IsLight() ? AppDpi_Px(2.f) : AppDpi_Px(5.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, AppDpi_Px(3.f)));

        const int row1Count = SplitTabRows(count);
        const int row2Count = count - row1Count;
        auto drawRow = [&](int start, int rowCount) {
            if (rowCount <= 0) return;
            const float btnW =
                (stripW - spacing * static_cast<float>(rowCount - 1)) / static_cast<float>(rowCount);
            for (int i = 0; i < rowCount; ++i) {
                if (i > 0) ImGui::SameLine(0.f, spacing);
                const int tabIndex = start + i;
                if (DrawWorkspaceTabButton(labels[tabIndex], tabIndex, activeTab, btnW))
                    activeTab = tabIndex;
            }
        };
        drawRow(0, row1Count);
        if (row2Count > 0) drawRow(row1Count, row2Count);

        ImGui::PopStyleVar(3);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return activeTab;
}

inline void DrawWorkspaceContentSeparator() {
    ImGui::Dummy(ImVec2(0.f, AppDpi_Px(4.f)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        xcat::ui::UiTheme_DrawWin7Separator(dl, p.x, p.x + w, p.y);
    }
    ImGui::Dummy(ImVec2(0.f, AppDpi_Px(8.f)));
}

inline float LauncherFooterReserve() {
    return ImGui::GetFrameHeightWithSpacing() + AppDpi_Px(18.f);
}

struct LauncherFrame {
    bool visible = false;

    // reservedBottomPx：客户区底部留给内嵌 WebView2 的高度，ImGui 只画上方区域
    explicit LauncherFrame(float reservedBottomPx = 0.f, bool* open = nullptr) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Pad(), Pad() * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Gap(), Gap() * 0.75f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(AppDpi_Px(5.f), AppDpi_Px(3.f)));

        ImVec2 sz = ImGui::GetIO().DisplaySize;
        if (reservedBottomPx > 0.f)
            sz.y = (std::max)(1.f, sz.y - reservedBottomPx);
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        visible = ImGui::Begin("XCat", open,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse);
    }

    ~LauncherFrame() {
        ImGui::End();
        ImGui::PopStyleVar(5);
    }

    LauncherFrame(const LauncherFrame&) = delete;
    LauncherFrame& operator=(const LauncherFrame&) = delete;
};

}  // namespace xcat::app::ui
