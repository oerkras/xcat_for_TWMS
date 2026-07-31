#pragma once

#include "imgui.h"

#include <Windows.h>

#include <cstdint>
#include <string>

namespace xcat::ui {

// 用户偏好（可持久化）。System 仅兼容旧 ini，Load 时按当前解析结果归一为 Dark/Light。
enum class ThemePreference : uint8_t { Dark = 0, Light = 1, System = 2 };
enum class ThemeResolved : uint8_t { Dark = 0, Light = 1 };

struct UiPalette {
    ImVec4 titleBarTop{};
    ImVec4 titleBarBottom{};
    ImVec4 titleBarLineTop{};
    ImVec4 titleBarLineBottom{};
    ImVec4 brandText{};
    ImVec4 hintText{};
    ImVec4 mutedText{};

    ImVec4 tabStripBg{};
    ImVec4 tabStripBorder{};
    ImVec4 tabActive{};
    ImVec4 tabActiveHovered{};
    ImVec4 tabActiveActive{};
    ImVec4 tabActiveText{};
    ImVec4 tabActiveBorder{};
    ImVec4 tabInactive{};
    ImVec4 tabInactiveHovered{};
    ImVec4 tabInactiveActive{};
    ImVec4 tabInactiveText{};
    ImVec4 tabInactiveBorder{};

    ImVec4 captionBtnHovered{};
    ImVec4 captionBtnActive{};
    ImVec4 captionBtnText{};

    ImVec4 statusStripBg{};
    ImVec4 statusStripBorder{};

    ImVec4 toastBg{};
    ImVec4 toastBorder{};
    ImVec4 toastTitle{};
    ImVec4 toastBody{};
    ImVec4 toastShadow{};

    ImVec4 dangerHover{};
    ImVec4 dangerActive{};

    int borderR = 10;
    int borderG = 11;
    int borderB = 14;
    bool immersiveDarkMode = true;
};

ThemePreference UiTheme_Preference();
ThemeResolved UiTheme_Resolved();
void UiTheme_SetPreference(ThemePreference pref);

// 写 ImGui style + 填充 palette（需已有 ImGui context）。
void UiTheme_Apply();
const UiPalette& UiTheme_Palette();

bool UiTheme_IsSystemLight();
const char* UiTheme_PreferenceToString(ThemePreference pref);
ThemePreference UiTheme_ParsePreference(const std::string& value);

// user.ini [ui] theme=dark|light（旧值 system 在 Load 时迁移并写回）。
bool UiTheme_Load(const char* binDir);
bool UiTheme_Save(const char* binDir);
bool UiTheme_SetPreferencePersisted(const char* binDir, ThemePreference pref);

std::string UiTheme_IniPath(const char* binDir);
const char* UiTheme_LastSaveError();
void UiTheme_RefreshDwm(HWND hwnd);

// Win7 客户区金属质感（不含标题栏）：渐变底 + 上沿高光 + 双线斜角。
enum class Win7FaceKind : uint8_t {
    Panel = 0,
    Button,
    ButtonHot,
    ButtonPressed,
    Tab,
    TabHot,
    TabActive,
};

void UiTheme_DrawWin7Face(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax, Win7FaceKind kind,
                          float rounding = 2.f);
void UiTheme_DrawWin7Bevel(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax,
                           bool inset = false);
void UiTheme_DrawWin7Separator(ImDrawList* dl, float x0, float x1, float y);

// 纹理粗糙度：程序性拉丝 + 预烘焙颗粒贴图平铺（启动时一次生成 64×64，每帧不再扫像素）。
enum class RoughKind : uint8_t { Matte = 0, Brushed, Grain };
void UiTheme_DrawRoughness(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax,
                           RoughKind kind = RoughKind::Matte, float strength = 1.f);

// DX11：从预烘焙 CPU 瓦片上传 SRV；Destroy 在 ImGui/DX shutdown 之前调用。
bool UiTheme_CreateGrainTextureDX11(void* d3d11Device);
void UiTheme_DestroyGrainTextureDX11();

}  // namespace xcat::ui
