#pragma once

#include "imgui.h"

namespace xcat::ui {

// 游戏内 overlay / launcher 共用：繁中全集 + Ext-A + 韩文 + 常用 UI / 数学符号。
// 勿用 ChineseSimplifiedCommon —— 创世/枫星 GUI 与物品名为繁体，缺字会显示 '?'。
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

    static const ImWchar kHangul[] = {0x1100, 0x11FF, 0x3130, 0x318F, 0xAC00, 0xD7AF, 0};
    b.AddRanges(kHangul);
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

// 大字号 / 高 DPI 下 ChineseFull+Hangul atlas 极易超默认纹理宽，打包失败会丢字变 '?'。
inline void PrepareCjkFontAtlas(ImFontAtlas* fonts, float dpiScale) {
    if (!fonts) return;
    if (dpiScale < 0.75f) dpiScale = 0.75f;
    // 宽纹理优先：避免 stb 打包在窄图上把大量 CJK 字模挤掉。
    fonts->TexDesiredWidth = (dpiScale >= 1.5f) ? 8192 : 4096;
    fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
}

}  // namespace xcat::ui
