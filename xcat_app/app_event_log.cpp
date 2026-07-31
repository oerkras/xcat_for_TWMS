#include "app_event_log.h"

#include "app_dpi.h"
#include "app_sound.h"
#include "app_theme.h"

#include "imgui.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace xcat::app::eventlog {
namespace {

constexpr size_t kMaxEvents = 200;

std::mutex g_mtx;
std::deque<Event> g_events;  // 旧 -> 新
std::string g_binDir;
bool g_initialized = false;
uint32_t g_unseen = 0;
bool g_windowOpen = false;
int g_levelFilter = 0xF;  // 位掩码：bit0 Info / bit1 Success / bit2 Warning / bit3 Danger

Level ClampLevel(uint32_t kind) {
    return kind > 3u ? Level::Info : static_cast<Level>(kind);
}

std::string EventsPath() {
    if (g_binDir.empty()) return {};
    return g_binDir + "state\\events.bin";
}

uint64_t NowFileTime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string FormatTimestamp(uint64_t filetime) {
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(filetime & 0xFFFFFFFFull);
    ft.dwHighDateTime = static_cast<DWORD>(filetime >> 32);
    FILETIME local{};
    SYSTEMTIME st{};
    if (FileTimeToLocalFileTime(&ft, &local) && FileTimeToSystemTime(&local, &st)) {
        char buf[32]{};
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }
    return "--";
}

ImU32 ColorForLevel(Level level, float alpha = 1.f) {
    const int a = static_cast<int>(std::clamp(alpha, 0.f, 1.f) * 255.f);
    switch (level) {
    case Level::Success: return IM_COL32(64, 220, 128, a);
    case Level::Warning: return IM_COL32(255, 188, 64, a);
    case Level::Danger: return IM_COL32(255, 92, 86, a);
    case Level::Info:
    default: return IM_COL32(98, 178, 255, a);
    }
}

const char* LabelForLevel(Level level) {
    switch (level) {
    case Level::Success: return "成功";
    case Level::Warning: return "警告";
    case Level::Danger: return "危险";
    case Level::Info:
    default: return "信息";
    }
}

}  // namespace

void SetStoragePath(const std::string& binDir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (binDir.empty()) return;
    if (g_initialized) return;
    g_binDir = binDir;
    g_events.clear();
    g_unseen = 0;
    const std::string oldPath = EventsPath();
    if (!oldPath.empty()) DeleteFileA(oldPath.c_str());
    g_initialized = true;
}

void Record(uint32_t kind, const std::string& key, const std::string& title,
            const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Event ev{};
    ev.level = ClampLevel(kind);
    ev.filetime = NowFileTime();
    ev.timestamp = FormatTimestamp(ev.filetime);
    ev.key = key;
    ev.title = title;
    ev.body = body;
    g_events.emplace_back(std::move(ev));
    while (g_events.size() > kMaxEvents) g_events.pop_front();
    if (g_unseen < 0xFFFFFFFFu) ++g_unseen;
}

void Snapshot(std::vector<Event>& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    out.assign(g_events.rbegin(), g_events.rend());
}

size_t Count() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_events.size();
}

uint32_t UnseenCount() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_unseen;
}

void MarkAllSeen() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_unseen = 0;
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_events.clear();
    g_unseen = 0;
}

bool WindowOpen() { return g_windowOpen; }

void SetWindowOpen(bool open) {
    if (open && !g_windowOpen) MarkAllSeen();
    g_windowOpen = open;
}

void ToggleWindow() { SetWindowOpen(!g_windowOpen); }

void DrawWindow(float dpiScale) {
    if (!g_windowOpen) return;

    MarkAllSeen();

    const float s = dpiScale > 0.f ? dpiScale : AppDpi_Scale();
    ImGui::SetNextWindowSize(ImVec2(520.f * s, 560.f * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.f * s, 260.f * s),
                                        ImVec2(FLT_MAX, FLT_MAX));

    bool open = g_windowOpen;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(AppDpi_Px(12.f), AppDpi_Px(10.f)));
    if (!ImGui::Begin("事件记录##xcat_event_log", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        ImGui::PopStyleVar();
        g_windowOpen = open;
        return;
    }

    std::vector<Event> events;
    Snapshot(events);

    ImGui::TextDisabled("共 %d 条（最新在前，最多保留 %d 条）",
                        static_cast<int>(events.size()), static_cast<int>(kMaxEvents));

    // 级别过滤 chips
    struct Chip {
        const char* label;
        int bit;
        Level level;
    };
    static const Chip kChips[] = {
        {"信息", 1 << 0, Level::Info},
        {"成功", 1 << 1, Level::Success},
        {"警告", 1 << 2, Level::Warning},
        {"危险", 1 << 3, Level::Danger},
    };

    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine(0.f, AppDpi_Px(4.f));
        const bool on = (g_levelFilter & kChips[i].bit) != 0;
        const ImU32 col = ColorForLevel(kChips[i].level, on ? 1.f : 0.35f);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImColor(col).Value
                                                  : ImVec4(0.12f, 0.13f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImColor(ColorForLevel(kChips[i].level, 0.7f)).Value);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImColor(col).Value);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)
                                                : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        char id[32]{};
        snprintf(id, sizeof(id), "%s##evt_flt_%d", kChips[i].label, i);
        if (ImGui::SmallButton(id)) g_levelFilter ^= kChips[i].bit;
        ImGui::PopStyleColor(4);
    }

    ImGui::SameLine();
    const float clearW = AppDpi_Px(56.f);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > clearW + AppDpi_Px(4.f)) ImGui::Dummy(ImVec2(avail - clearW, 0.f));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.32f, 0.12f, 0.14f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f, 0.16f, 0.18f, 1.f));
    if (ImGui::SmallButton("清空##evt_clear")) {
        Clear();
        events.clear();
        sound::UiToggle();
    }
    ImGui::PopStyleColor(2);

    ImGui::Separator();

    ImGui::BeginChild("##evt_list", ImVec2(0.f, 0.f), ImGuiChildFlags_None);

    int shown = 0;
    for (const Event& ev : events) {
        const int bit = 1 << static_cast<int>(ev.level);
        if ((g_levelFilter & bit) == 0) continue;
        ++shown;

        const ImU32 accent = ColorForLevel(ev.level);
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();

        ImGui::PushID(static_cast<int>(reinterpret_cast<uintptr_t>(&ev) & 0x7FFFFFFF) ^ shown);

        // 左侧彩色竖条
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float barW = AppDpi_Px(3.f);
        const float indent = barW + AppDpi_Px(8.f);
        ImGui::Indent(indent);

        // 标题行：[级别] 标题
        ImGui::TextColored(ImColor(accent).Value, "[%s]", LabelForLevel(ev.level));
        ImGui::SameLine(0.f, AppDpi_Px(6.f));
        ImGui::TextUnformatted(ev.title.empty() ? "(无标题)" : ev.title.c_str());

        // 时间行
        ImGui::TextDisabled("%s", ev.timestamp.empty() ? "--" : ev.timestamp.c_str());

        // 详情
        if (!ev.body.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, xcat::app::AppTheme_Palette().mutedText);
            ImGui::TextWrapped("%s", ev.body.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Unindent(indent);

        const ImVec2 rowEnd = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(rowStart.x, rowStart.y),
                          ImVec2(rowStart.x + barW, rowEnd.y - AppDpi_Px(2.f)), accent,
                          AppDpi_Px(1.5f));

        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.f, AppDpi_Px(4.f)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, AppDpi_Px(4.f)));
    }

    if (shown == 0) {
        ImGui::Dummy(ImVec2(0.f, AppDpi_Px(20.f)));
        ImGui::TextDisabled(events.empty() ? "暂无事件记录。" : "当前过滤条件下没有事件。");
    }

    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar();
    g_windowOpen = open;
}

}  // namespace xcat::app::eventlog
