#include "app_font.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include "../common_ui/xcat_imgui_cjk_font.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>

namespace {

ImFont* gHintFont = nullptr;

const char* kCandidates[] = {
    "%s\\Fonts\\msyh.ttc",
    "%s\\Fonts\\msyhbd.ttc",
    "%s\\Fonts\\simhei.ttf",
    "%s\\Fonts\\simsun.ttc",
    "%s\\Fonts\\mingliu.ttc",
    "%s\\Fonts\\msjh.ttc",
    "%s\\Fonts\\msjhbd.ttc",
};

ImFont* AddCjkFont(ImFontAtlas* fonts, float fontSize, const ImWchar* glyphRanges) {
    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.0f;
    cfg.FontNo = 0;

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);
    char fontPath[MAX_PATH]{};
    for (const char* fmt : kCandidates) {
        std::snprintf(fontPath, sizeof(fontPath), fmt, winDir);
        if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES) continue;
        if (ImFont* f = fonts->AddFontFromFileTTF(fontPath, fontSize, &cfg, glyphRanges)) {
            (void)xcat::ui::MergeUiSymbolFallbackFont(fonts, fontSize);
            return f;
        }
    }
    return nullptr;
}

}  // namespace

void AppFont_Load(float dpiScale) {
    gHintFont = nullptr;
    if (dpiScale < 0.75f) dpiScale = 0.75f;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    xcat::ui::PrepareCjkFontAtlas(io.Fonts, dpiScale);

    // 整数像素字号，避免非整数 DPI 缩放导致字体发糊
    const float fontSize =
        (16.0f * dpiScale < 12.f) ? 12.f : std::floorf(16.0f * dpiScale + 0.5f);
    const float hintSize = std::floorf(fontSize * 1.5f + 0.5f);

    const ImWchar* glyphRanges = xcat::ui::CjkGuiGlyphRanges(io.Fonts);

    if (!AddCjkFont(io.Fonts, fontSize, glyphRanges)) {
        ImFontConfig cfg{};
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        io.Fonts->AddFontDefault(&cfg);
    } else {
        gHintFont = AddCjkFont(io.Fonts, hintSize, glyphRanges);
    }

    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}

ImFont* AppFont_Hint() { return gHintFont; }
