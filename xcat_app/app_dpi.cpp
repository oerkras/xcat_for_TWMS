#include "app_dpi.h"

#include "app_font.h"

#include "imgui.h"
#include "imgui_impl_win32.h"

namespace {

float g_uiScale = 1.f;
ImGuiStyle g_styleBase{};
bool g_hasStyleBase = false;

}  // namespace

float AppDpi_Scale() { return g_uiScale; }

void AppDpi_SetScale(float scale) {
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.f) scale = 3.f;
    g_uiScale = scale;
}

float AppDpi_Px(float designPx) { return designPx * g_uiScale; }

void AppDpi_RecaptureStyleBase() {
    g_styleBase = ImGui::GetStyle();
    g_hasStyleBase = true;
}

void AppDpi_ApplyStyle(float scale) {
    AppDpi_SetScale(scale);
    if (!g_hasStyleBase) {
        g_styleBase = ImGui::GetStyle();
        g_hasStyleBase = true;
    }
    ImGui::GetStyle() = g_styleBase;
    ImGui::GetStyle().ScaleAllSizes(scale);
}

void AppDpi_Init(HWND hwnd) {
    AppDpi_SetScale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));
    AppDpi_ApplyStyle(g_uiScale);
}

void AppDpi_Refresh(HWND hwnd, bool reloadFont) {
    const float newScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (newScale == g_uiScale && !reloadFont) return;
    AppDpi_ApplyStyle(newScale);
    if (reloadFont) AppFont_Load(g_uiScale);
}
