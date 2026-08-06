#include "log_upload_ui.h"

#include "app_dpi.h"
#include "app_sound.h"
#include "imgui_shell.h"
#include "log_upload.h"
#include "update_client.h"

#include "process_util.h"

#include "imgui.h"

#include <Windows.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace xcat::app {
namespace {

struct LogUploadUiState {
    bool loaded = false;
    char note[kMaxUploadNoteUtf8Bytes + 1]{};
    LogUploadMode mode = LogUploadMode::Light;
    std::string exeBinDir;
    std::string clearedNoteForUploadId;
};

LogUploadUiState g_ui;

std::string ExeBinDirUtf8() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return xcat::WideToUtf8(xcat::ParentDirWithSlash(path));
}

int LogUploadNoteCallback(ImGuiInputTextCallbackData* data) {
    if (!data || data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;
    const std::string normalized =
        NormalizeUploadNote(std::string_view(data->Buf, static_cast<size_t>(data->BufTextLen)));
    if (normalized.size() == static_cast<size_t>(data->BufTextLen) &&
        normalized == std::string_view(data->Buf, static_cast<size_t>(data->BufTextLen))) {
        return 0;
    }
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, normalized.c_str());
    return 0;
}

void PersistLogUploadUiPrefs() {
    LogUploadPrefs prefs{};
    // 上报/更新口写死内置默认，面板不再暴露可编辑域名。
    prefs.url = kDefaultUpdateServiceUrl;
    prefs.mode = g_ui.mode;
    SaveLogUploadPrefs(g_ui.exeBinDir, prefs);
}

void EnsureLogUploadUiLoaded(const std::string& prefsBinDir) {
    if (g_ui.loaded) return;
    g_ui.loaded = true;
    g_ui.exeBinDir = ExeBinDirUtf8();

    LogUploadPrefs defaults{};
    defaults.url = kDefaultUpdateServiceUrl;
    defaults.mode = LogUploadMode::Light;
    const LogUploadPrefs prefs = LoadLogUploadPrefs(g_ui.exeBinDir, defaults);
    g_ui.mode = prefs.mode == LogUploadMode::Full ? LogUploadMode::Full : LogUploadMode::Light;
    (void)prefsBinDir;
}

LogUploadRequest MakeLogUploadRequest(const std::string& prefsBinDir) {
    EnsureLogUploadUiLoaded(prefsBinDir);
    LogUploadRequest req{};
    req.url = kDefaultUpdateServiceUrl;
    req.note = NormalizeUploadNote(g_ui.note);
    req.mode = g_ui.mode;
    req.profileId = "twms";
    req.exeBinDir = g_ui.exeBinDir;
    req.payloadBinDir = prefsBinDir.empty()
                            ? xcat::JoinBinPath(req.exeBinDir.c_str(), "XCat_data")
                            : prefsBinDir;
    return req;
}

}  // namespace

void DrawLogUploadPanel(const std::string& prefsBinDir) {
    EnsureLogUploadUiLoaded(prefsBinDir);
    const LogUploadSnapshot snap = GetLogUploadSnapshot();
    const bool uploading = snap.phase == LogUploadPhase::Uploading;

    if (snap.phase == LogUploadPhase::Succeeded && !snap.uploadId.empty() &&
        g_ui.clearedNoteForUploadId != snap.uploadId) {
        g_ui.note[0] = '\0';
        g_ui.clearedNoteForUploadId = snap.uploadId;
    }

    ImGui::PushID("log_upload_twms");

    if (uploading) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##log_upload_note", "备注（可选，说明 BUG 原因）", g_ui.note,
                             sizeof(g_ui.note), ImGuiInputTextFlags_CallbackEdit,
                             LogUploadNoteCallback);
    {
        char tip[160]{};
        snprintf(tip, sizeof(tip),
                 "可留空。填写后写入服务端 catalog.jsonl 的 note，最多 %zu 字。",
                 kMaxUploadNoteCodePoints);
        ImGui::SetItemTooltip("%s", tip);
    }

    {
        const char* modeItems[] = {"轻量上传", "全量上传"};
        int modeIdx = g_ui.mode == LogUploadMode::Full ? 1 : 0;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("##log_upload_mode", &modeIdx, modeItems, 2)) {
            g_ui.mode = modeIdx == 1 ? LogUploadMode::Full : LogUploadMode::Light;
            PersistLogUploadUiPrefs();
            sound::UiClick();
        }
        ImGui::SetItemTooltip(
            "两种模式都含全部日志频道（launcher/inject/x.jsonl + combat/foothold 等）。\n"
            "轻量：每频道最近 %zu 卷（默认，省流量）。\n"
            "全量：每频道最多 %zu 卷（深排障）。\n"
            "有测谎包则附带。",
            kLogUploadBackupsLight, kLogUploadBackupsFull);
    }
    if (uploading) ImGui::EndDisabled();

    const LogUploadRequest req = MakeLogUploadRequest(prefsBinDir);
    const bool configured = LogUploadConfigured(req);
    if (!configured || uploading) ImGui::BeginDisabled();
    const char* label = uploading ? "上传中..." : "上传日志";
    if (ImGui::Button(label, ImVec2(-1.f, ui::BtnH()))) {
        if (StartLogUpload(req)) sound::UiClick();
    }
    if (!configured || uploading) ImGui::EndDisabled();

    if (!configured) {
        ImGui::TextDisabled("上报服务未就绪");
    } else if (!snap.message.empty()) {
        ImGui::TextWrapped("%s", snap.message.c_str());
    } else {
        ImGui::TextDisabled("默认轻量；深排障再切全量。");
    }

    const std::vector<LogUploadHistoryEntry> history = GetLogUploadHistory();
    if (history.empty()) {
        ImGui::TextDisabled("日志上传：0 次");
    } else {
        const LogUploadHistoryEntry& last = history.back();
        ImGui::TextDisabled("日志上传：%d 次，最近 %s", static_cast<int>(history.size()),
                            last.timeText.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("最近日志上传记录");
            ImGui::Separator();
            const int total = static_cast<int>(history.size());
            const int begin = total > 8 ? total - 8 : 0;
            for (int i = total - 1; i >= begin; --i) {
                const LogUploadHistoryEntry& e = history[static_cast<size_t>(i)];
                ImGui::TextWrapped("%s  HTTP %u  文件 %u", e.timeText.c_str(), e.httpStatus,
                                   e.files);
                if (!e.uploadId.empty()) ImGui::TextDisabled("  %s", e.uploadId.c_str());
            }
            ImGui::EndTooltip();
        }
    }

    ImGui::PopID();
}

}  // namespace xcat::app
