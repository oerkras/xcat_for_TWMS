#include "app_font.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include "../common_ui/xcat_imgui_cjk_font.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>

void AppFont_Load(float dpiScale) {
    if (dpiScale < 0.75f) dpiScale = 0.75f;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    xcat::ui::PrepareCjkFontAtlas(io.Fonts, dpiScale);

    // 整数像素字号，避免非整数 DPI 缩放导致字体发糊
    const float fontSize =
        (16.0f * dpiScale < 12.f) ? 12.f : std::floorf(16.0f * dpiScale + 0.5f);

    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.0f;
    cfg.FontNo = 0;  // TTC：Regular face

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);

    // 字体候选对齐对照仓枫星：微软雅黑优先（观感一致）；缺字形再由 MergeUiSymbolFallback 补。
    static const char* kCandidates[] = {
        "%s\\Fonts\\msyh.ttc",
        "%s\\Fonts\\msyhbd.ttc",
        "%s\\Fonts\\simhei.ttf",
        "%s\\Fonts\\simsun.ttc",
        "%s\\Fonts\\mingliu.ttc",
        "%s\\Fonts\\msjh.ttc",
        "%s\\Fonts\\msjhbd.ttc",
    };

    // 繁中全集 + 杂项/数学符号（无韩文，见 xcat::ui::CjkGuiGlyphRanges）。
    const ImWchar* glyphRanges = xcat::ui::CjkGuiGlyphRanges(io.Fonts);

    bool loaded = false;
    char fontPath[MAX_PATH]{};
    for (const char* fmt : kCandidates) {
        std::snprintf(fontPath, sizeof(fontPath), fmt, winDir);
        if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES) continue;
        if (io.Fonts->AddFontFromFileTTF(fontPath, fontSize, &cfg, glyphRanges)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        // ProggyClean 无中文：加载失败时整句都会变成 '?'。
        io.Fonts->AddFontDefault(&cfg);
    } else {
        // 正黑等 CJK 常缺 ≈≤≥；叠一层 Segoe/Arial 符号补齐。
        (void)xcat::ui::MergeUiSymbolFallbackFont(io.Fonts, fontSize);
    }

    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}
