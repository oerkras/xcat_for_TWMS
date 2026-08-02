#pragma once

#include "imgui.h"

#include <Windows.h>

#include <cstdio>

namespace xcat::ui {

// 经典版 / TWMS 面板：繁中全集 + Ext-A + 常用 UI / 数学符号。
// 勿用 ChineseSimplifiedCommon —— 分区名等为繁体，缺字会显示 '?'。
// 故意不含韩文：ChineseFull+Hangul 在高 DPI 下极易 atlas 打包失败，随机丢字（如「拉」→'?'）。
//
// 注意：ImFontConfig::GlyphRanges 指针须在字体存活期内保持有效；此处用静态缓冲，
// 每次重建内容但不在 Build 之后再 clear。
inline const ImWchar* CjkGuiGlyphRanges(ImFontAtlas* fonts) {
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder b;
    b.AddRanges(fonts->GetGlyphRangesChineseFull());

    // ChineseFull 止于 U+9FAF；补齐统一表意文字剩余段 + Extension A（偶发繁体/旧字形）。
    static const ImWchar kCjkExtra[] = {
        0x3400, 0x4DBF,  // CJK Ext-A
        0x9FB0, 0x9FFF,  // Unified Ideographs residual
        0,
    };
    b.AddRanges(kCjkExtra);

    static const ImWchar kLatin1[] = {0x00A0, 0x00FF, 0};
    b.AddRanges(kLatin1);
    static const ImWchar kNumberForms[] = {0x2150, 0x218F, 0};
    b.AddRanges(kNumberForms);
    static const ImWchar kPunctuation[] = {0x2010, 0x206F, 0};
    b.AddRanges(kPunctuation);
    static const ImWchar kArrows[] = {0x2190, 0x21FF, 0};
    b.AddRanges(kArrows);
    // ≈ ≤ ≥ ≠ ± 等：UI 文案常见，不在 ChineseFull 内，缺了就会显示 '?'
    static const ImWchar kMathOps[] = {0x2200, 0x22FF, 0};
    b.AddRanges(kMathOps);
    static const ImWchar kBoxDrawing[] = {0x2500, 0x257F, 0};
    b.AddRanges(kBoxDrawing);
    static const ImWchar kGeometric[] = {0x25A0, 0x25FF, 0};
    b.AddRanges(kGeometric);
    static const ImWchar kSymbols[] = {0x2600, 0x26FF, 0};
    b.AddRanges(kSymbols);

    ranges.resize(0);
    b.BuildRanges(&ranges);
    return ranges.Data;
}

// 西文补丁图集：正黑/细明体常缺 ≈≤≥ 等数学符；MergeMode 叠进主字体兜底。
inline const ImWchar* UiSymbolFallbackRanges() {
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder b;
    static const ImWchar kLatin1[] = {0x00A0, 0x00FF, 0};
    b.AddRanges(kLatin1);
    static const ImWchar kPunctuation[] = {0x2010, 0x206F, 0};
    b.AddRanges(kPunctuation);
    static const ImWchar kArrows[] = {0x2190, 0x21FF, 0};
    b.AddRanges(kArrows);
    static const ImWchar kMathOps[] = {0x2200, 0x22FF, 0};
    b.AddRanges(kMathOps);
    static const ImWchar kBoxDrawing[] = {0x2500, 0x257F, 0};
    b.AddRanges(kBoxDrawing);
    static const ImWchar kGeometric[] = {0x25A0, 0x25FF, 0};
    b.AddRanges(kGeometric);
    static const ImWchar kSymbols[] = {0x2600, 0x26FF, 0};
    b.AddRanges(kSymbols);
    // 显式钉死面板常用符，避免 builder/字体子集边界漏字（码点不用 u8 串，防源文件编码坑）。
    static const ImWchar kPinned[] = {
        0x00B7,  // ·
        0x00B1,  // ±
        0x00D7,  // ×
        0x00F7,  // ÷
        0x2013,  // –
        0x2014,  // —
        0x2026,  // …
        0x2190,  // ←
        0x2191,  // ↑
        0x2192,  // →
        0x2193,  // ↓
        0x2248,  // ≈
        0x2260,  // ≠
        0x2264,  // ≤
        0x2265,  // ≥
        0,
    };
    for (const ImWchar* p = kPinned; *p; ++p) b.AddChar(*p);
    ranges.resize(0);
    b.BuildRanges(&ranges);
    return ranges.Data;
}

// 在已加载的主 CJK 字体上 Merge 一款带数学符号的西文 TTF。
inline bool MergeUiSymbolFallbackFont(ImFontAtlas* fonts, float fontSize) {
    if (!fonts || fontSize < 1.f) return false;

    ImFontConfig cfg{};
    cfg.MergeMode = true;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.0f;
    cfg.GlyphMinAdvanceX = 0.f;

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);
    // Segoe / Arial / Calibri 覆盖 Mathematical Operators；任选其一即可。
    static const char* kCandidates[] = {
        "%s\\Fonts\\segoeui.ttf",
        "%s\\Fonts\\arial.ttf",
        "%s\\Fonts\\calibri.ttf",
        "%s\\Fonts\\tahoma.ttf",
    };

    const ImWchar* ranges = UiSymbolFallbackRanges();
    char fontPath[MAX_PATH]{};
    for (const char* fmt : kCandidates) {
        std::snprintf(fontPath, sizeof(fontPath), fmt, winDir);
        if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES) continue;
        if (fonts->AddFontFromFileTTF(fontPath, fontSize, &cfg, ranges)) return true;
    }
    return false;
}

// 大字号 / 高 DPI 下 CJK atlas 仍可能挤爆；宽纹理优先，避免丢字变 '?'。
inline void PrepareCjkFontAtlas(ImFontAtlas* fonts, float dpiScale) {
    if (!fonts) return;
    if (dpiScale < 0.75f) dpiScale = 0.75f;
    fonts->TexDesiredWidth = (dpiScale >= 1.5f) ? 8192 : 4096;
    fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
}

}  // namespace xcat::ui
