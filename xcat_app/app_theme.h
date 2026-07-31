#pragma once

#include "../common_ui/xcat_imgui_theme.h"

#include <Windows.h>

#include <string>

namespace xcat::app {

using AppThemeMode = xcat::ui::ThemePreference;
using AppPalette = xcat::ui::UiPalette;

inline AppThemeMode AppTheme_Mode() { return xcat::ui::UiTheme_Preference(); }
inline void AppTheme_SetMode(AppThemeMode mode) { xcat::ui::UiTheme_SetPreference(mode); }
inline bool AppTheme_IsLight() {
    return xcat::ui::UiTheme_Resolved() == xcat::ui::ThemeResolved::Light;
}

void AppTheme_Apply();
inline const AppPalette& AppTheme_Palette() { return xcat::ui::UiTheme_Palette(); }

bool AppTheme_Load(const char* binDir);
bool AppTheme_Save(const char* binDir);
bool AppTheme_SetModePersisted(const char* binDir, AppThemeMode mode);

void AppTheme_Commit(HWND hwnd);
void AppTheme_RequestCommit(HWND hwnd);
void AppTheme_PumpPending();

// 系统主题变化时调用（WM_SETTINGCHANGE）；仅 preference=system 时重 Apply。
void AppTheme_OnSystemThemeMaybeChanged(HWND hwnd);

std::string AppTheme_IniPath(const char* binDir);
const char* AppTheme_LastSaveError();

}  // namespace xcat::app
