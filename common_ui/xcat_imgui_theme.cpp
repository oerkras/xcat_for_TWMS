#include "xcat_imgui_theme.h"

#include "../common/process_util.h"
#include "../common/xcat_config_ini.h"
#include "../common/xcat_log.h"

#include <d3d11.h>
#include <dwmapi.h>

#include <cmath>
#include <filesystem>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")


#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

namespace xcat::ui {
namespace {

ThemePreference g_pref = ThemePreference::Light;
UiPalette g_palette{};
std::string g_lastSaveError;

void ApplyGeometry(ImGuiStyle& s, bool lightELang) {
    s.Alpha = 1.f;
    s.WindowRounding = 0.f;
    // 白天：Win7 金属控件轻微圆角；暗夜保持现有圆角。
    if (lightELang) {
        s.ChildRounding = 2.f;
        s.FrameRounding = 2.f;
        s.PopupRounding = 2.f;
        s.ScrollbarRounding = 2.f;
        s.GrabRounding = 2.f;
    } else {
        s.ChildRounding = 6.f;
        s.FrameRounding = 5.f;
        s.PopupRounding = 6.f;
        s.ScrollbarRounding = 5.f;
        s.GrabRounding = 5.f;
    }
    s.WindowBorderSize = 0.f;
    s.ChildBorderSize = 1.f;
    s.FrameBorderSize = 1.f;
    s.PopupBorderSize = 1.f;
    s.ItemSpacing = ImVec2(8.f, 5.f);
    s.ItemInnerSpacing = ImVec2(6.f, 4.f);
    s.FramePadding = ImVec2(6.f, 4.f);
    s.WindowPadding = ImVec2(10.f, 8.f);
    // 设计像素；DPI 由 AppDpi_ApplyStyle → ScaleAllSizes 统一放大。
    // 2026-07-09 (c603985d) 主题重构曾漏掉此项，回落到 ImGui 默认 14 显得过宽。
    s.ScrollbarSize = 10.f;
    s.GrabMinSize = 10.f;
}

void FillDarkPalette(UiPalette& p) {
    p.titleBarTop = ImVec4(8 / 255.f, 10 / 255.f, 16 / 255.f, 1.f);
    p.titleBarBottom = ImVec4(22 / 255.f, 26 / 255.f, 36 / 255.f, 1.f);
    p.titleBarLineTop = ImVec4(72 / 255.f, 120 / 255.f, 190 / 255.f, 50 / 255.f);
    p.titleBarLineBottom = ImVec4(52 / 255.f, 88 / 255.f, 140 / 255.f, 210 / 255.f);
    p.brandText = ImVec4(118 / 255.f, 186 / 255.f, 255 / 255.f, 1.f);
    p.hintText = ImVec4(130 / 255.f, 138 / 255.f, 152 / 255.f, 1.f);
    p.mutedText = ImVec4(0.78f, 0.82f, 0.88f, 1.f);

    p.tabStripBg = ImVec4(0.055f, 0.060f, 0.075f, 1.f);
    p.tabStripBorder = ImVec4(0.20f, 0.28f, 0.40f, 0.70f);
    p.tabActive = ImVec4(0.20f, 0.42f, 0.78f, 1.f);
    p.tabActiveHovered = ImVec4(0.26f, 0.50f, 0.88f, 1.f);
    p.tabActiveActive = ImVec4(0.16f, 0.34f, 0.66f, 1.f);
    p.tabActiveText = ImVec4(0.96f, 0.98f, 1.f, 1.f);
    p.tabActiveBorder = ImVec4(0.42f, 0.62f, 0.92f, 0.85f);
    p.tabInactive = ImVec4(0.09f, 0.10f, 0.13f, 1.f);
    p.tabInactiveHovered = ImVec4(0.14f, 0.17f, 0.22f, 1.f);
    p.tabInactiveActive = ImVec4(0.18f, 0.28f, 0.44f, 1.f);
    p.tabInactiveText = ImVec4(0.66f, 0.70f, 0.78f, 1.f);
    p.tabInactiveBorder = ImVec4(0.18f, 0.24f, 0.34f, 0.55f);

    p.captionBtnHovered = ImVec4(0.22f, 0.28f, 0.38f, 0.95f);
    p.captionBtnActive = ImVec4(0.30f, 0.38f, 0.50f, 1.f);
    p.captionBtnText = ImVec4(0.82f, 0.86f, 0.92f, 1.f);

    p.statusStripBg = ImVec4(0.065f, 0.072f, 0.092f, 1.f);
    p.statusStripBorder = ImVec4(0.22f, 0.30f, 0.44f, 0.72f);

    p.toastBg = ImVec4(20 / 255.f, 22 / 255.f, 28 / 255.f, 226 / 255.f);
    p.toastBorder = ImVec4(1.f, 1.f, 1.f, 42 / 255.f);
    p.toastTitle = ImVec4(245 / 255.f, 247 / 255.f, 255 / 255.f, 245 / 255.f);
    p.toastBody = ImVec4(190 / 255.f, 198 / 255.f, 214 / 255.f, 220 / 255.f);
    p.toastShadow = ImVec4(0.f, 0.f, 0.f, 70 / 255.f);

    p.dangerHover = ImVec4(0.55f, 0.16f, 0.18f, 0.95f);
    p.dangerActive = ImVec4(0.72f, 0.20f, 0.24f, 1.f);

    p.borderR = 10;
    p.borderG = 11;
    p.borderB = 14;
    p.immersiveDarkMode = true;
}

// Windows 经典对比阶梯（白天）：
//   窗体底 #B8B8B8 → 凹槽/条 #A8A8A8 → 凸起卡片/按钮 #D4D4D4
//   编辑框纯白 → 选中经典蓝 #0054A6 → 正文纯黑 / 弱文案 #5A5A5A
// 标题栏同系中灰（不要深蓝、不要近白）。
struct ELangClassic {
    static ImVec4 Form() { return {184 / 255.f, 184 / 255.f, 184 / 255.f, 1.f}; }       // 窗体底
    static ImVec4 FormDark() { return {168 / 255.f, 168 / 255.f, 168 / 255.f, 1.f}; }    // 凹槽/表头
    static ImVec4 FormDeep() { return {148 / 255.f, 148 / 255.f, 148 / 255.f, 1.f}; }    // 未选中 Tab
    static ImVec4 Card() { return {212 / 255.f, 212 / 255.f, 212 / 255.f, 1.f}; }        // 凸起卡片
    static ImVec4 Btn() { return {220 / 255.f, 220 / 255.f, 220 / 255.f, 1.f}; }         // 凸起按钮
    static ImVec4 BtnHot() { return {196 / 255.f, 214 / 255.f, 232 / 255.f, 1.f}; }
    static ImVec4 BtnDown() { return {156 / 255.f, 172 / 255.f, 188 / 255.f, 1.f}; }
    static ImVec4 Edit() { return {1.f, 1.f, 1.f, 1.f}; }
    static ImVec4 EditFocus() { return {1.f, 1.f, 1.f, 1.f}; }
    static ImVec4 Shadow() { return {64 / 255.f, 64 / 255.f, 64 / 255.f, 1.f}; }         // 高对比暗边
    static ImVec4 HiliteEdge() { return {240 / 255.f, 240 / 255.f, 240 / 255.f, 1.f}; }  // 斜角亮边
    static ImVec4 CaptionTop() { return {200 / 255.f, 200 / 255.f, 200 / 255.f, 1.f}; }
    static ImVec4 CaptionBottom() { return {172 / 255.f, 172 / 255.f, 172 / 255.f, 1.f}; }
    static ImVec4 Hilite() { return {0 / 255.f, 84 / 255.f, 166 / 255.f, 1.f}; }
    static ImVec4 Text() { return {0.f, 0.f, 0.f, 1.f}; }
    static ImVec4 TextDisabled() { return {90 / 255.f, 90 / 255.f, 90 / 255.f, 1.f}; }   // 在银灰上仍可读
    static ImVec4 CaptionText() { return {0.f, 0.f, 0.f, 1.f}; }
};

void FillLightPalette(UiPalette& p) {
    const ImVec4 form = ELangClassic::Form();
    const ImVec4 formDark = ELangClassic::FormDark();
    const ImVec4 formDeep = ELangClassic::FormDeep();
    const ImVec4 card = ELangClassic::Card();
    const ImVec4 btn = ELangClassic::Btn();
    const ImVec4 shadow = ELangClassic::Shadow();
    const ImVec4 text = ELangClassic::Text();

    // 标题栏：略亮于窗体，仍非近白/深蓝。
    p.titleBarTop = ELangClassic::CaptionTop();
    p.titleBarBottom = ELangClassic::CaptionBottom();
    p.titleBarLineTop = ImVec4(1.f, 1.f, 1.f, 0.40f);
    p.titleBarLineBottom = ImVec4(0.f, 0.f, 0.f, 0.28f);
    p.brandText = ELangClassic::CaptionText();
    p.hintText = ImVec4(0.22f, 0.22f, 0.22f, 1.f);
    p.mutedText = ImVec4(0.18f, 0.18f, 0.18f, 1.f);

    // Tab 条：底=窗体；选中=凸起亮面；未选中=明显更深。
    p.tabStripBg = form;
    p.tabStripBorder = shadow;
    p.tabActive = card;
    p.tabActiveHovered = ELangClassic::BtnHot();
    p.tabActiveActive = formDark;
    p.tabActiveText = text;
    p.tabActiveBorder = ImVec4(48 / 255.f, 48 / 255.f, 48 / 255.f, 1.f);
    p.tabInactive = formDeep;
    p.tabInactiveHovered = formDark;
    p.tabInactiveActive = btn;
    p.tabInactiveText = text;
    p.tabInactiveBorder = ImVec4(shadow.x, shadow.y, shadow.z, 0.90f);

    p.captionBtnHovered = ImVec4(0.f, 0.f, 0.f, 0.10f);
    p.captionBtnActive = ImVec4(0.f, 0.f, 0.f, 0.18f);
    p.captionBtnText = ELangClassic::CaptionText();

    p.statusStripBg = formDark;
    p.statusStripBorder = ImVec4(shadow.x, shadow.y, shadow.z, 0.90f);

    p.toastBg = ImVec4(card.x, card.y, card.z, 0.97f);
    p.toastBorder = ImVec4(shadow.x, shadow.y, shadow.z, 0.85f);
    p.toastTitle = ImVec4(0.f, 0.f, 0.f, 0.96f);
    p.toastBody = ImVec4(0.16f, 0.16f, 0.16f, 0.92f);
    p.toastShadow = ImVec4(0.f, 0.f, 0.f, 0.28f);

    p.dangerHover = ImVec4(0.72f, 0.18f, 0.16f, 0.95f);
    p.dangerActive = ImVec4(0.62f, 0.12f, 0.10f, 1.f);

    p.borderR = 64;
    p.borderG = 64;
    p.borderB = 64;
    p.immersiveDarkMode = false;
}

void ApplyDarkColors(ImGuiStyle& s) {
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.038f, 0.042f, 0.055f, 1.f);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.082f, 0.102f, 1.f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(0.090f, 0.098f, 0.120f, 0.98f);
    s.Colors[ImGuiCol_Border] = ImVec4(0.24f, 0.32f, 0.46f, 0.72f);
    s.Colors[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.35f);
    s.Colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.30f, 0.42f, 0.70f);
    s.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.96f, 1.f);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.52f, 0.62f, 1.f);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.16f, 0.22f, 1.f);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.19f, 0.27f, 1.f);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(0.46f, 0.70f, 1.f, 1.f);
    s.Colors[ImGuiCol_SliderGrab] = ImVec4(0.34f, 0.54f, 0.82f, 1.f);
    s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.42f, 0.62f, 0.92f, 1.f);
    s.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.32f, 0.52f, 0.62f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.38f, 0.60f, 0.78f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.42f, 0.66f, 0.90f);
    s.Colors[ImGuiCol_Button] = ImVec4(0.11f, 0.13f, 0.18f, 1.f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.22f, 0.32f, 1.f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.32f, 0.48f, 1.f);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.90f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.30f, 0.42f, 0.90f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.38f, 0.54f, 1.f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.34f, 0.48f, 0.68f, 1.f);
    s.Colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.06f, 0.08f, 1.f);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.09f, 0.12f, 1.f);
}

void ApplyLightColors(ImGuiStyle& s) {
    // 对比阶梯：窗体中灰 / 凹槽更深 / 按钮卡片更亮 / 编辑框白 / 选中蓝。
    const ImVec4 form = ELangClassic::Form();
    const ImVec4 formDark = ELangClassic::FormDark();
    const ImVec4 formDeep = ELangClassic::FormDeep();
    const ImVec4 card = ELangClassic::Card();
    const ImVec4 btn = ELangClassic::Btn();
    const ImVec4 btnHot = ELangClassic::BtnHot();
    const ImVec4 btnDown = ELangClassic::BtnDown();
    const ImVec4 edit = ELangClassic::Edit();
    const ImVec4 editFocus = ELangClassic::EditFocus();
    const ImVec4 shadow = ELangClassic::Shadow();
    const ImVec4 hilite = ELangClassic::Hilite();
    const ImVec4 text = ELangClassic::Text();
    const ImVec4 textDis = ELangClassic::TextDisabled();
    const ImVec4 hilite28(hilite.x, hilite.y, hilite.z, 0.28f);
    const ImVec4 hilite42(hilite.x, hilite.y, hilite.z, 0.42f);
    const ImVec4 hilite58(hilite.x, hilite.y, hilite.z, 0.58f);

    s.Colors[ImGuiCol_Text] = text;
    s.Colors[ImGuiCol_TextDisabled] = textDis;
    s.Colors[ImGuiCol_WindowBg] = form;
    s.Colors[ImGuiCol_ChildBg] = formDark;
    s.Colors[ImGuiCol_PopupBg] = ImVec4(card.x, card.y, card.z, 0.98f);
    s.Colors[ImGuiCol_Border] = shadow;
    s.Colors[ImGuiCol_BorderShadow] = ImVec4(1.f, 1.f, 1.f, 0.35f);  // 经典亮边暗示
    s.Colors[ImGuiCol_FrameBg] = edit;
    s.Colors[ImGuiCol_FrameBgHovered] = editFocus;
    s.Colors[ImGuiCol_FrameBgActive] = editFocus;
    s.Colors[ImGuiCol_TitleBg] = formDark;
    s.Colors[ImGuiCol_TitleBgActive] = ELangClassic::CaptionTop();
    s.Colors[ImGuiCol_TitleBgCollapsed] = formDark;
    s.Colors[ImGuiCol_MenuBarBg] = formDark;
    s.Colors[ImGuiCol_ScrollbarBg] = formDeep;
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(132 / 255.f, 132 / 255.f, 132 / 255.f, 1.f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(108 / 255.f, 108 / 255.f, 108 / 255.f, 1.f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(72 / 255.f, 72 / 255.f, 72 / 255.f, 1.f);
    s.Colors[ImGuiCol_CheckMark] = hilite;
    s.Colors[ImGuiCol_SliderGrab] = hilite;
    s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0 / 255.f, 60 / 255.f, 128 / 255.f, 1.f);
    s.Colors[ImGuiCol_Button] = btn;
    s.Colors[ImGuiCol_ButtonHovered] = btnHot;
    s.Colors[ImGuiCol_ButtonActive] = btnDown;
    s.Colors[ImGuiCol_Header] = hilite28;
    s.Colors[ImGuiCol_HeaderHovered] = hilite42;
    s.Colors[ImGuiCol_HeaderActive] = hilite58;
    s.Colors[ImGuiCol_Separator] = ImVec4(shadow.x, shadow.y, shadow.z, 0.85f);
    s.Colors[ImGuiCol_SeparatorHovered] = hilite42;
    s.Colors[ImGuiCol_SeparatorActive] = hilite58;
    s.Colors[ImGuiCol_ResizeGrip] = ImVec4(shadow.x, shadow.y, shadow.z, 0.40f);
    s.Colors[ImGuiCol_ResizeGripHovered] = hilite42;
    s.Colors[ImGuiCol_ResizeGripActive] = hilite58;
    s.Colors[ImGuiCol_TabHovered] = btnHot;
    s.Colors[ImGuiCol_Tab] = formDeep;
    s.Colors[ImGuiCol_TabSelected] = card;
    s.Colors[ImGuiCol_TabSelectedOverline] = hilite;
    s.Colors[ImGuiCol_TabDimmed] = formDeep;
    s.Colors[ImGuiCol_TabDimmedSelected] = formDark;
    s.Colors[ImGuiCol_TabDimmedSelectedOverline] = shadow;
    s.Colors[ImGuiCol_PlotLines] = ImVec4(0.12f, 0.12f, 0.12f, 1.f);
    s.Colors[ImGuiCol_PlotLinesHovered] = hilite;
    s.Colors[ImGuiCol_PlotHistogram] = hilite;
    s.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0 / 255.f, 60 / 255.f, 128 / 255.f, 1.f);
    s.Colors[ImGuiCol_TableHeaderBg] = formDark;
    s.Colors[ImGuiCol_TableBorderStrong] = shadow;
    s.Colors[ImGuiCol_TableBorderLight] = ImVec4(128 / 255.f, 128 / 255.f, 128 / 255.f, 0.95f);
    s.Colors[ImGuiCol_TableRowBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.f, 0.f, 0.f, 0.08f);  // 斑马纹更清晰
    s.Colors[ImGuiCol_TextLink] = hilite;
    s.Colors[ImGuiCol_TextSelectedBg] = hilite42;
    s.Colors[ImGuiCol_DragDropTarget] = ImVec4(hilite.x, hilite.y, hilite.z, 0.90f);
    s.Colors[ImGuiCol_NavCursor] = hilite;
    s.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.f, 1.f, 1.f, 0.35f);
    s.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.35f);
    s.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.40f);
}

}  // namespace

bool UiTheme_IsSystemLight() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG rc =
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value),
                         &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && value != 0;
}

const char* UiTheme_PreferenceToString(ThemePreference pref) {
    switch (pref) {
    case ThemePreference::Light:
        return "light";
    case ThemePreference::System:
        return "system";
    case ThemePreference::Dark:
    default:
        return "dark";
    }
}

ThemePreference UiTheme_ParsePreference(const std::string& value) {
    if (value == "light" || value == "Light" || value == "LIGHT" || value == "day" || value == "1") {
        return ThemePreference::Light;
    }
    if (value == "system" || value == "System" || value == "SYSTEM" || value == "auto") {
        return ThemePreference::System;
    }
    return ThemePreference::Dark;
}

ThemePreference UiTheme_Preference() { return g_pref; }

ThemeResolved UiTheme_Resolved() {
    if (g_pref == ThemePreference::Light) return ThemeResolved::Light;
    if (g_pref == ThemePreference::System) {
        return UiTheme_IsSystemLight() ? ThemeResolved::Light : ThemeResolved::Dark;
    }
    return ThemeResolved::Dark;
}

void UiTheme_SetPreference(ThemePreference pref) { g_pref = pref; }

void UiTheme_Apply() {
    const ThemeResolved resolved = UiTheme_Resolved();
    if (resolved == ThemeResolved::Light) {
        ImGui::StyleColorsLight();
        FillLightPalette(g_palette);
    } else {
        ImGui::StyleColorsDark();
        FillDarkPalette(g_palette);
    }
    ImGuiStyle& s = ImGui::GetStyle();
    const bool light = resolved == ThemeResolved::Light;
    ApplyGeometry(s, light);
    if (light) {
        ApplyLightColors(s);
    } else {
        ApplyDarkColors(s);
    }
}

const UiPalette& UiTheme_Palette() { return g_palette; }

std::string UiTheme_IniPath(const char* binDir) {
    if (!binDir || !binDir[0]) return {};
    return xcat::UserConfigIniPath(binDir);
}

const char* UiTheme_LastSaveError() { return g_lastSaveError.c_str(); }

bool UiTheme_Load(const char* binDir) {
    g_lastSaveError.clear();
    if (!binDir || !binDir[0]) {
        g_pref = ThemePreference::Light;
        xcat::log::Info("UiTheme", "load skip: empty binDir → light");
        return true;
    }
    xcat::IniStore ini{};
    const std::string path = UiTheme_IniPath(binDir);
    if (!xcat::LoadIniFile(path.c_str(), ini)) {
        g_pref = ThemePreference::Light;
        xcat::log::Info("UiTheme", "load miss path=%s → light", path.c_str());
        return true;
    }
    std::string value;
    if (!xcat::IniGetString(ini, "ui", "theme", value) || value.empty()) {
        g_pref = ThemePreference::Light;
        xcat::log::Info("UiTheme", "load no [ui].theme path=%s → light", path.c_str());
        return true;
    }
    g_pref = UiTheme_ParsePreference(value);
    // UI 已去掉「跟随系统」：旧 theme=system 按当前解析结果固化为 dark/light 并尽量写回。
    if (g_pref == ThemePreference::System) {
        const ThemeResolved resolved = UiTheme_Resolved();
        g_pref = (resolved == ThemeResolved::Light) ? ThemePreference::Light : ThemePreference::Dark;
        if (UiTheme_Save(binDir)) {
            xcat::log::Info("UiTheme", "migrate system → %s path=%s",
                            UiTheme_PreferenceToString(g_pref), path.c_str());
        } else {
            xcat::log::Warn("UiTheme",
                            "migrate system → %s in-memory only; save fail path=%s err=%s",
                            UiTheme_PreferenceToString(g_pref), path.c_str(),
                            g_lastSaveError.c_str());
        }
        return true;
    }
    xcat::log::Info("UiTheme", "load ok theme=%s resolved=%s path=%s",
                    UiTheme_PreferenceToString(g_pref),
                    UiTheme_Resolved() == ThemeResolved::Light ? "light" : "dark", path.c_str());
    return true;
}

bool UiTheme_Save(const char* binDir) {
    g_lastSaveError.clear();
    if (!binDir || !binDir[0]) {
        g_lastSaveError = "binDir 为空";
        xcat::log::Warn("UiTheme", "save fail: empty binDir");
        return false;
    }
    const std::string path = UiTheme_IniPath(binDir);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(xcat::Utf8ToWide(path)).parent_path(), ec);
    if (ec) {
        g_lastSaveError = "无法创建 state 目录";
        xcat::log::Warn("UiTheme", "save fail: mkdir ec=%d path=%s", ec.value(), path.c_str());
        return false;
    }
    const bool ok = xcat::UpdateIniFile(path.c_str(), [&](xcat::IniStore& ini) {
        xcat::IniSetU32(ini, "meta", "version", static_cast<uint32_t>(xcat::kUserConfigIniVersion));
        xcat::IniSetU32(ini, "ui", "version", 2u);
        xcat::IniSetString(ini, "ui", "theme", UiTheme_PreferenceToString(g_pref));
        xcat::IniSetU64(ini, "ui", "writeTickMs", GetTickCount64());
    });
    if (ok) {
        xcat::log::Info("UiTheme", "save ok theme=%s path=%s", UiTheme_PreferenceToString(g_pref),
                        path.c_str());
    } else {
        g_lastSaveError = "UpdateIniFile 失败（锁或读写）";
        xcat::log::Warn("UiTheme", "save fail path=%s theme=%s", path.c_str(),
                        UiTheme_PreferenceToString(g_pref));
    }
    return ok;
}

bool UiTheme_SetPreferencePersisted(const char* binDir, ThemePreference pref) {
    const ThemePreference prev = g_pref;
    g_pref = pref;
    if (!UiTheme_Save(binDir)) {
        g_pref = prev;
        return false;
    }
    return true;
}

void UiTheme_RefreshDwm(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    const UiPalette& p = g_palette;
    BOOL dark = p.immersiveDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF borderColor = RGB(p.borderR, p.borderG, p.borderB);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

namespace {

ImU32 Rgba8(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

void FaceStops(Win7FaceKind kind, ImU32& top, ImU32& bottom, ImU32& border, ImU32& gloss) {
    const bool light = (UiTheme_Resolved() == ThemeResolved::Light);
    if (!light) {
        // 暗夜：深蓝灰金属面，选中更亮一档，热态偏蓝。
        switch (kind) {
        case Win7FaceKind::ButtonHot:
        case Win7FaceKind::TabHot:
            top = Rgba8(48, 72, 112);
            bottom = Rgba8(28, 44, 72);
            border = Rgba8(72, 112, 168);
            gloss = Rgba8(160, 200, 255, 35);
            break;
        case Win7FaceKind::ButtonPressed:
            top = Rgba8(24, 36, 56);
            bottom = Rgba8(16, 24, 40);
            border = Rgba8(56, 88, 136);
            gloss = Rgba8(120, 160, 220, 18);
            break;
        case Win7FaceKind::TabActive:
            top = Rgba8(52, 68, 96);
            bottom = Rgba8(32, 44, 68);
            border = Rgba8(88, 128, 184);
            gloss = Rgba8(180, 210, 255, 40);
            break;
        case Win7FaceKind::Panel:
            top = Rgba8(28, 32, 42);
            bottom = Rgba8(18, 22, 30);
            border = Rgba8(48, 60, 84);
            gloss = Rgba8(140, 160, 200, 18);
            break;
        case Win7FaceKind::Tab:
            top = Rgba8(26, 30, 40);
            bottom = Rgba8(16, 18, 26);
            border = Rgba8(40, 52, 72);
            gloss = Rgba8(120, 140, 180, 22);
            break;
        case Win7FaceKind::Button:
        default:
            top = Rgba8(40, 48, 64);
            bottom = Rgba8(24, 30, 42);
            border = Rgba8(56, 72, 100);
            gloss = Rgba8(150, 170, 210, 28);
            break;
        }
        return;
    }

    switch (kind) {
    case Win7FaceKind::ButtonHot:
    case Win7FaceKind::TabHot:
        top = Rgba8(196, 214, 232);
        bottom = Rgba8(152, 176, 200);
        border = Rgba8(40, 72, 112);
        gloss = Rgba8(255, 255, 255, 45);
        break;
    case Win7FaceKind::ButtonPressed:
        top = Rgba8(148, 164, 180);
        bottom = Rgba8(120, 136, 152);
        border = Rgba8(40, 56, 72);
        gloss = Rgba8(255, 255, 255, 20);
        break;
    case Win7FaceKind::TabActive:
        top = Rgba8(232, 232, 232);
        bottom = Rgba8(208, 208, 208);
        border = Rgba8(48, 48, 48);
        gloss = Rgba8(255, 255, 255, 40);
        break;
    case Win7FaceKind::Panel:
        top = Rgba8(216, 216, 216);
        bottom = Rgba8(200, 200, 200);
        border = Rgba8(64, 64, 64);
        gloss = Rgba8(255, 255, 255, 16);
        break;
    case Win7FaceKind::Tab:
        top = Rgba8(168, 168, 168);
        bottom = Rgba8(144, 144, 144);
        border = Rgba8(64, 64, 64);
        gloss = Rgba8(255, 255, 255, 28);
        break;
    case Win7FaceKind::Button:
    default:
        top = Rgba8(228, 228, 228);
        bottom = Rgba8(196, 196, 196);
        border = Rgba8(48, 48, 48);
        gloss = Rgba8(255, 255, 255, 40);
        break;
    }
}

}  // namespace

void UiTheme_DrawWin7Face(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax, Win7FaceKind kind,
                          float rounding) {
    if (!dl) return;
    if (rmax.x <= rmin.x || rmax.y <= rmin.y) return;

    ImU32 top{}, bottom{}, border{}, gloss{};
    FaceStops(kind, top, bottom, border, gloss);

    const float r = rounding > 0.f ? rounding : 0.f;
    dl->AddRectFilledMultiColor(rmin, rmax, top, top, bottom, bottom);
    if (r > 0.05f) {
        // MultiColor 不吃圆角：再盖一层圆角遮罩边缘用边框收住即可。
        dl->AddRect(rmin, rmax, border, r, 0, 1.0f);
    } else {
        dl->AddRect(rmin, rmax, border, 0.f, 0, 1.0f);
    }

    // 上半高光带（金属反光）。
    const float glossH = (rmax.y - rmin.y) * 0.48f;
    if (glossH > 1.f) {
        const ImVec2 g1(rmin.x + 1.f, rmin.y + 1.f);
        const ImVec2 g2(rmax.x - 1.f, rmin.y + glossH);
        dl->AddRectFilledMultiColor(g1, g2, gloss, gloss, Rgba8(255, 255, 255, 0),
                                    Rgba8(255, 255, 255, 0));
    }

    // 内沿亮边（上/左）+ 暗边（下/右）→ 轻微凸起。
    const bool lightFace = (UiTheme_Resolved() == ThemeResolved::Light);
    const ImU32 hi = lightFace ? Rgba8(255, 255, 255, 160) : Rgba8(180, 200, 240, 55);
    const ImU32 lo = lightFace ? Rgba8(80, 84, 92, 90) : Rgba8(0, 0, 0, 120);
    dl->AddLine(ImVec2(rmin.x + 1.f, rmin.y + 1.f), ImVec2(rmax.x - 2.f, rmin.y + 1.f), hi);
    dl->AddLine(ImVec2(rmin.x + 1.f, rmin.y + 1.f), ImVec2(rmin.x + 1.f, rmax.y - 2.f), hi);
    dl->AddLine(ImVec2(rmin.x + 1.f, rmax.y - 2.f), ImVec2(rmax.x - 2.f, rmax.y - 2.f), lo);
    dl->AddLine(ImVec2(rmax.x - 2.f, rmin.y + 1.f), ImVec2(rmax.x - 2.f, rmax.y - 2.f), lo);

    // Roughness：一律拉丝；Matte 颗粒在银底/深底都会像灰尘盖过金属纹。
    UiTheme_DrawRoughness(dl, rmin, rmax, RoughKind::Brushed, lightFace ? 0.80f : 0.40f);
}

void UiTheme_DrawWin7Bevel(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax, bool inset) {
    if (!dl) return;
    if (rmax.x <= rmin.x || rmax.y <= rmin.y) return;
    const bool light = (UiTheme_Resolved() == ThemeResolved::Light);
    ImU32 hi, lo;
    if (light) {
        hi = inset ? Rgba8(120, 124, 132, 140) : Rgba8(255, 255, 255, 180);
        lo = inset ? Rgba8(255, 255, 255, 160) : Rgba8(120, 124, 132, 140);
    } else {
        hi = inset ? Rgba8(0, 0, 0, 140) : Rgba8(160, 180, 220, 50);
        lo = inset ? Rgba8(140, 160, 200, 40) : Rgba8(0, 0, 0, 150);
    }
    dl->AddLine(ImVec2(rmin.x + 1.f, rmin.y + 1.f), ImVec2(rmax.x - 2.f, rmin.y + 1.f), hi);
    dl->AddLine(ImVec2(rmin.x + 1.f, rmin.y + 1.f), ImVec2(rmin.x + 1.f, rmax.y - 2.f), hi);
    dl->AddLine(ImVec2(rmin.x + 1.f, rmax.y - 2.f), ImVec2(rmax.x - 2.f, rmax.y - 2.f), lo);
    dl->AddLine(ImVec2(rmax.x - 2.f, rmin.y + 1.f), ImVec2(rmax.x - 2.f, rmax.y - 2.f), lo);
}

void UiTheme_DrawWin7Separator(ImDrawList* dl, float x0, float x1, float y) {
    if (!dl) return;
    if (UiTheme_Resolved() == ThemeResolved::Light) {
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Rgba8(64, 64, 64, 220));
        dl->AddLine(ImVec2(x0, y + 1.f), ImVec2(x1, y + 1.f), Rgba8(232, 232, 232, 200));
    } else {
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Rgba8(8, 10, 14, 220));
        dl->AddLine(ImVec2(x0, y + 1.f), ImVec2(x1, y + 1.f), Rgba8(70, 90, 130, 120));
    }
}


namespace {

uint32_t RoughHash2(int x, int y) {
    uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
}

int RoughClampAlpha(float strength, int base) {
    if (strength < 0.f) strength = 0.f;
    if (strength > 2.f) strength = 2.f;
    int a = static_cast<int>(base * strength + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return a;
}

constexpr int kGrainTile = 64;
unsigned char g_grainRgba[kGrainTile * kGrainTile * 4]{};
bool g_grainCpuReady = false;
ID3D11ShaderResourceView* g_grainSrv = nullptr;
ImTextureID g_grainTex = 0;

void BakeGrainTileCpu() {
    if (g_grainCpuReady) return;
    // Matte 密度（step=3, thresh=7）；Grain 用更高 tint alpha。
    constexpr int kStep = 3;
    constexpr int kThresh = 7;
    for (int y = 0; y < kGrainTile; ++y) {
        for (int x = 0; x < kGrainTile; ++x) {
            unsigned char* p = &g_grainRgba[(y * kGrainTile + x) * 4];
            p[0] = p[1] = p[2] = 0;
            p[3] = 0;
            if ((x % kStep) != 0 || (y % kStep) != 0) continue;
            const uint32_t h = RoughHash2(x, y);
            if ((h % static_cast<uint32_t>(kThresh)) != 0u) continue;
            const bool lite = (h & 16u) != 0u;
            if (lite) {
                p[0] = p[1] = p[2] = 255;
                p[3] = 56;
            } else {
                p[0] = p[1] = p[2] = 30;
                p[3] = 88;
            }
        }
    }
    g_grainCpuReady = true;
}

void DrawGrainTiled(ImDrawList* dl, float x0, float y0, float x1, float y1, float strength,
                    RoughKind kind) {
    if (!g_grainTex) return;
    const float tw = static_cast<float>(kGrainTile);
    const float th = static_cast<float>(kGrainTile);
    const int baseA = (kind == RoughKind::Grain) ? 210 : 150;
    const ImU32 tint = IM_COL32(255, 255, 255, RoughClampAlpha(strength, baseA));

    const float startX = std::floor(x0 / tw) * tw;
    const float startY = std::floor(y0 / th) * th;
    for (float ty = startY; ty < y1; ty += th) {
        for (float tx = startX; tx < x1; tx += tw) {
            const float xa = (tx > x0) ? tx : x0;
            const float ya = (ty > y0) ? ty : y0;
            const float xb = (tx + tw < x1) ? (tx + tw) : x1;
            const float yb = (ty + th < y1) ? (ty + th) : y1;
            if (xb <= xa || yb <= ya) continue;
            const ImVec2 uv0((xa - tx) / tw, (ya - ty) / th);
            const ImVec2 uv1((xb - tx) / tw, (yb - ty) / th);
            dl->AddImage(g_grainTex, ImVec2(xa, ya), ImVec2(xb, yb), uv0, uv1, tint);
        }
    }
}

}  // namespace

bool UiTheme_CreateGrainTextureDX11(void* d3d11Device) {
    auto* device = static_cast<ID3D11Device*>(d3d11Device);
    if (!device) {
        xcat::log::Warn("UiTheme", "grain tex skip: no D3D11 device");
        return false;
    }
    if (g_grainSrv) {
        xcat::log::Info("UiTheme", "grain tex already ready tile=%dx%d", kGrainTile, kGrainTile);
        return true;
    }
    BakeGrainTileCpu();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(kGrainTile);
    td.Height = static_cast<UINT>(kGrainTile);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = g_grainRgba;
    init.SysMemPitch = static_cast<UINT>(kGrainTile * 4);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&td, &init, &tex)) || !tex) {
        xcat::log::Warn("UiTheme", "grain tex CreateTexture2D failed");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = device->CreateShaderResourceView(tex, &svd, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) {
        xcat::log::Warn("UiTheme", "grain tex CreateSRV failed hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    g_grainSrv = srv;
    g_grainTex = reinterpret_cast<ImTextureID>(srv);
    xcat::log::Info("UiTheme", "grain tex ready tile=%dx%d (prebake+SRV)", kGrainTile, kGrainTile);
    return true;
}

void UiTheme_DestroyGrainTextureDX11() {
    if (g_grainSrv) {
        g_grainSrv->Release();
        g_grainSrv = nullptr;
        xcat::log::Info("UiTheme", "grain tex destroyed");
    }
    g_grainTex = 0;
}

void UiTheme_DrawRoughness(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax, RoughKind kind,
                           float strength) {
    if (!dl || strength <= 0.01f) return;
    if (rmax.x - rmin.x < 4.f || rmax.y - rmin.y < 4.f) return;

    const float x0 = rmin.x + 1.f;
    const float y0 = rmin.y + 1.f;
    const float x1 = rmax.x - 1.f;
    const float y1 = rmax.y - 1.f;
    if (x1 <= x0 || y1 <= y0) return;
    // AutoResizeY 卡片若把高度反馈到几千像素，2px 一行会把 UI 线程画死（窗口未响应）。
    if (y1 - y0 > 4096.f) return;

    const bool brushed = (kind == RoughKind::Brushed || kind == RoughKind::Matte);
    const bool grain = (kind == RoughKind::Grain || kind == RoughKind::Matte);

    if (brushed) {
        const int lineA = RoughClampAlpha(strength, (kind == RoughKind::Brushed) ? 18 : 12);
        const int lineB = RoughClampAlpha(strength, (kind == RoughKind::Brushed) ? 10 : 7);
        const ImU32 dark = Rgba8(40, 40, 40, lineA);
        const ImU32 lite = Rgba8(255, 255, 255, lineB);
        for (float y = y0; y < y1; y += 2.f) {
            const int yi = static_cast<int>(y);
            const bool odd = (yi & 1) != 0;
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), odd ? dark : lite);
        }
    }

    if (grain) {
        // 有预烘焙贴图：按 64×64 瓦片 AddImage（与面积近似线性但常数很小）；
        // 无贴图时跳过 CPU 扫点，避免老机器掉帧。
        DrawGrainTiled(dl, x0, y0, x1, y1, strength, kind);
    }
}

}  // namespace xcat::ui
