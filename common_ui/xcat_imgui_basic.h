#pragma once

#include "imgui.h"

#include "../common_ui/xcat_imgui_theme.h"

#include <algorithm>

namespace xcat::ui {

inline float Gap() { return 5.f; }
inline float Pad() { return 10.f; }

struct StyleColorGuard {
    int count = 0;
    explicit StyleColorGuard(int n) : count(n) {}
    ~StyleColorGuard() {
        if (count > 0) ImGui::PopStyleColor(count);
    }
    StyleColorGuard(const StyleColorGuard&) = delete;
    StyleColorGuard& operator=(const StyleColorGuard&) = delete;
};

struct StyleVarGuard {
    int count = 0;
    explicit StyleVarGuard(int n) : count(n) {}
    ~StyleVarGuard() {
        if (count > 0) ImGui::PopStyleVar(count);
    }
    StyleVarGuard(const StyleVarGuard&) = delete;
    StyleVarGuard& operator=(const StyleVarGuard&) = delete;
};

inline bool DragIntClamped(const char* label, int* v, int v_min, int v_max,
                           const char* format = "%d", float speed = 1.f) {
    ImGui::DragInt(label, v, speed, v_min, v_max, format, ImGuiSliderFlags_AlwaysClamp);
    *v = std::clamp(*v, v_min, v_max);
    return ImGui::IsItemDeactivatedAfterEdit();
}

// 拖动过程中每帧触发（松手才触发请用 DragIntClamped）。
inline bool DragIntClampedLive(const char* label, int* v, int v_min, int v_max,
                               const char* format = "%d", float speed = 1.f) {
    ImGui::DragInt(label, v, speed, v_min, v_max, format, ImGuiSliderFlags_AlwaysClamp);
    *v = std::clamp(*v, v_min, v_max);
    return ImGui::IsItemEdited();
}

inline bool DragFloatClamped(const char* label, float* v, float v_min, float v_max,
                             const char* format = "%.3f", float speed = 0.01f) {
    ImGui::DragFloat(label, v, speed, v_min, v_max, format, ImGuiSliderFlags_AlwaysClamp);
    *v = std::clamp(*v, v_min, v_max);
    return ImGui::IsItemDeactivatedAfterEdit();
}

inline bool DragFloatClampedLive(const char* label, float* v, float v_min, float v_max,
                                 const char* format = "%.3f", float speed = 0.01f) {
    ImGui::DragFloat(label, v, speed, v_min, v_max, format, ImGuiSliderFlags_AlwaysClamp);
    *v = std::clamp(*v, v_min, v_max);
    return ImGui::IsItemEdited();
}

struct OptionFrameStyleGuard {
    OptionFrameStyleGuard() {
        // 颜色跟当前主题；只强制边框几何，避免白天模式残留暗色框。
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    }
    ~OptionFrameStyleGuard() { ImGui::PopStyleVar(2); }
    OptionFrameStyleGuard(const OptionFrameStyleGuard&) = delete;
    OptionFrameStyleGuard& operator=(const OptionFrameStyleGuard&) = delete;
};

inline bool OptionCheckbox(const char* label, bool* value) {
    OptionFrameStyleGuard style;
    return ImGui::Checkbox(label, value);
}

inline bool OptionRadioButton(const char* label, int* value, int vButton) {
    OptionFrameStyleGuard style;
    return ImGui::RadioButton(label, value, vButton);
}

inline void SectionHeader(const char* label) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dl) {
        // 白天用低对比灰条，避免金属高光条抢视线。
        const ImU32 bar =
            (UiTheme_Resolved() == ThemeResolved::Light)
                ? IM_COL32(0, 84, 166, 230)
                : ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        dl->AddRectFilled(start, ImVec2(start.x + 3.f, start.y + h), bar);
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x + 11.f, start.y));
    ImGui::TextUnformatted(label);
    ImGui::Dummy(ImVec2(0.f, Gap() * 0.5f));
}

inline void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered() && desc && desc[0]) ImGui::SetTooltip("%s", desc);
}

inline bool BeginFields(const char* id, float labelFrac = 0.38f) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.f, 2.f));
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, labelFrac);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.f - labelFrac);
    return true;
}

inline void EndFields() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

struct FieldsGuard {
    bool active = false;
    FieldsGuard(const char* id, float labelFrac = 0.38f) : active(BeginFields(id, labelFrac)) {}
    ~FieldsGuard() {
        if (active) EndFields();
    }
    explicit operator bool() const { return active; }
    FieldsGuard(const FieldsGuard&) = delete;
    FieldsGuard& operator=(const FieldsGuard&) = delete;
};

inline void FieldLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

struct CardGuard {
    StyleVarGuard vars{2};
    int extraColors = 0;
    bool opened = false;
    bool textured = false;
    bool light = false;
    // fillRemaining=true：占满父级剩余高度（列表类 TAB 用 RemainingY 铺满，避免 AutoResizeY 把表高锁死）。
    CardGuard(const char* id, const char* title = nullptr, bool fillRemaining = false) {
        light = UiTheme_Resolved() == ThemeResolved::Light;
        textured = true;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, light ? 2.f : 5.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Pad(), 7.f));
        // 透明底：最终尺寸在背景层铺色+粗糙度，避免盖住文字。
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        if (light) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(64 / 255.f, 64 / 255.f, 64 / 255.f, 0.95f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.32f, 0.46f, 0.72f));
        }
        extraColors = 2;
        const ImGuiChildFlags childFlags =
            fillRemaining ? ImGuiChildFlags_Borders
                          : (ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::BeginChild(id, ImVec2(0.f, 0.f), childFlags);
        opened = true;
        if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
        }
        if (extraColors) {
            ImGui::PopStyleColor(extraColors);
            extraColors = 0;
        }
        if (title && title[0]) SectionHeader(title);
    }
    ~CardGuard() {
        if (opened) {
            if (textured) {
                if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
                    dl->ChannelsSetCurrent(0);
                    const ImVec2 a = ImGui::GetWindowPos();
                    const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
                    if (light) {
                        // 白天：银灰纵向渐变（对齐 Win7 Panel 凸起面）+ 拉丝。
                        dl->AddRectFilledMultiColor(a, b, IM_COL32(228, 228, 228, 255),
                                                    IM_COL32(228, 228, 228, 255),
                                                    IM_COL32(196, 196, 196, 255),
                                                    IM_COL32(196, 196, 196, 255));
                        UiTheme_DrawRoughness(dl, a, b, RoughKind::Brushed, 0.75f);
                    } else {
                        // 暗夜卡片：深蓝灰底；只用轻拉丝，避免白点颗粒在深底上像「灰尘」。
                        dl->AddRectFilledMultiColor(a, b, IM_COL32(28, 32, 42, 255),
                                                    IM_COL32(28, 32, 42, 255),
                                                    IM_COL32(18, 22, 30, 255),
                                                    IM_COL32(18, 22, 30, 255));
                        UiTheme_DrawRoughness(dl, a, b, RoughKind::Brushed, 0.45f);
                    }
                    UiTheme_DrawWin7Bevel(dl, a, b, false);
                    dl->ChannelsMerge();
                }
            }
            ImGui::EndChild();
        }
    }
    CardGuard(const CardGuard&) = delete;
    CardGuard& operator=(const CardGuard&) = delete;
};

}  // namespace xcat::ui
