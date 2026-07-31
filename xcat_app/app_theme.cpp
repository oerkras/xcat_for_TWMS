#include "app_theme.h"

#include "app_dpi.h"

namespace xcat::app {
namespace {

bool g_pendingCommit = false;
HWND g_pendingHwnd = nullptr;

}  // namespace

void AppTheme_Apply() { xcat::ui::UiTheme_Apply(); }

bool AppTheme_Load(const char* binDir) { return xcat::ui::UiTheme_Load(binDir); }

bool AppTheme_Save(const char* binDir) { return xcat::ui::UiTheme_Save(binDir); }

bool AppTheme_SetModePersisted(const char* binDir, AppThemeMode mode) {
    return xcat::ui::UiTheme_SetPreferencePersisted(binDir, mode);
}

void AppTheme_Commit(HWND hwnd) {
    AppTheme_Apply();
    AppDpi_RecaptureStyleBase();
    AppDpi_ApplyStyle(AppDpi_Scale());
    xcat::ui::UiTheme_RefreshDwm(hwnd);
}

void AppTheme_RequestCommit(HWND hwnd) {
    g_pendingHwnd = hwnd;
    g_pendingCommit = true;
}

void AppTheme_PumpPending() {
    if (!g_pendingCommit) return;
    g_pendingCommit = false;
    HWND hwnd = g_pendingHwnd;
    g_pendingHwnd = nullptr;
    AppTheme_Commit(hwnd);
}

void AppTheme_OnSystemThemeMaybeChanged(HWND hwnd) {
    if (AppTheme_Mode() != AppThemeMode::System) return;
    AppTheme_RequestCommit(hwnd);
}

std::string AppTheme_IniPath(const char* binDir) { return xcat::ui::UiTheme_IniPath(binDir); }

const char* AppTheme_LastSaveError() { return xcat::ui::UiTheme_LastSaveError(); }

}  // namespace xcat::app
