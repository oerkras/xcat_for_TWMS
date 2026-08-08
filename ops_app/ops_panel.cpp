#include "ops_panel.h"

#include "log_tail.h"
#include "ops_health.h"
#include "ops_window.h"

#include "xcat_imgui_theme.h"

#include "../common/process_util.h"

#include "imgui.h"

#include <Shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace xcat::ops {
namespace {

constexpr ULONGLONG kWatchdogGraceMs = 8000;
constexpr ULONGLONG kWatchdogMaxBackoffMs = 120000;
constexpr ULONGLONG kHealthyResetMs = 60000;
constexpr ULONGLONG kLogRotateBytes = 32ull * 1024ull * 1024ull;

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::string JoinPathUtf8(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

void EnsureDirs(const std::wstring& repo) {
    CreateDirectoryW(JoinPath(repo, L"artifacts").c_str(), nullptr);
    CreateDirectoryW(JoinPath(repo, L"artifacts\\ops_logs").c_str(), nullptr);
    CreateDirectoryW(JoinPath(repo, L"user_log_uploads").c_str(), nullptr);
    CreateDirectoryW(JoinPath(repo, L"artifacts\\travel_samples").c_str(), nullptr);
    CreateDirectoryW(JoinPath(repo, L"artifacts\\release").c_str(), nullptr);
}

std::wstring OpsLogTwms(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\twms.log");
}
std::wstring OpsLogPublish(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\publish.log");
}
std::wstring OpsLogHelper(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\helper.log");
}
std::wstring OpsLogAccess(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\twms_access.jsonl");
}

// 运维台语义色：按主题切换——暗夜用亮色字，白天用深色字，保证对比度。
bool OpsIsLight() {
    return xcat::ui::UiTheme_Resolved() == xcat::ui::ThemeResolved::Light;
}

namespace OpsTone {
inline ImVec4 Ok() {
    return OpsIsLight() ? ImVec4(0.05f, 0.42f, 0.20f, 1.f) : ImVec4(0.42f, 0.78f, 0.52f, 1.f);
}
inline ImVec4 Info() {
    return OpsIsLight() ? ImVec4(0.08f, 0.36f, 0.68f, 1.f) : ImVec4(0.45f, 0.70f, 0.95f, 1.f);
}
inline ImVec4 Warn() {
    return OpsIsLight() ? ImVec4(0.62f, 0.36f, 0.04f, 1.f) : ImVec4(0.95f, 0.72f, 0.35f, 1.f);
}
inline ImVec4 Danger() {
    return OpsIsLight() ? ImVec4(0.68f, 0.14f, 0.12f, 1.f) : ImVec4(0.95f, 0.48f, 0.42f, 1.f);
}
inline ImVec4 DangerSoft() {
    return OpsIsLight() ? ImVec4(0.72f, 0.22f, 0.12f, 1.f) : ImVec4(0.92f, 0.55f, 0.42f, 1.f);
}
inline ImVec4 Token() {
    return OpsIsLight() ? ImVec4(0.06f, 0.40f, 0.58f, 1.f) : ImVec4(0.55f, 0.82f, 0.95f, 1.f);
}
inline ImVec4 Muted() {
    return OpsIsLight() ? ImVec4(0.38f, 0.38f, 0.40f, 1.f) : ImVec4(0.62f, 0.64f, 0.68f, 1.f);
}
inline ImVec4 Busy() {
    return OpsIsLight() ? ImVec4(0.55f, 0.38f, 0.05f, 1.f) : ImVec4(0.92f, 0.78f, 0.40f, 1.f);
}
inline ImVec4 Body() {
    return OpsIsLight() ? ImVec4(0.18f, 0.18f, 0.20f, 1.f) : ImVec4(0.78f, 0.80f, 0.84f, 1.f);
}
}  // namespace OpsTone

void StatusLed(bool on, const char* label) {
    const ImVec4 color = on ? OpsTone::Ok() : OpsTone::Danger();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(on ? "●" : "○");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4.f);
    ImGui::TextUnformatted(label);
}

void SetStatus(OpsState& st, std::string msg) {
    std::lock_guard<std::mutex> lock(st.statusMu);
    st.statusMessage = std::move(msg);
}

std::string GetStatus(OpsState& st) {
    std::lock_guard<std::mutex> lock(st.statusMu);
    return st.statusMessage;
}

bool TwmsRunning(OpsState& st) {
    std::lock_guard<std::mutex> lock(st.procMu);
    return st.twms.IsRunning();
}

bool PublishRunning(OpsState& st) {
    std::lock_guard<std::mutex> lock(st.procMu);
    return st.publish.IsRunning();
}

DWORD TwmsPid(OpsState& st) {
    std::lock_guard<std::mutex> lock(st.procMu);
    return st.twms.Pid();
}

DWORD PublishPid(OpsState& st) {
    std::lock_guard<std::mutex> lock(st.procMu);
    return st.publish.Pid();
}

void RotateLogIfHuge(const std::wstring& logPath) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) return;
    ULARGE_INTEGER size{};
    size.HighPart = fad.nFileSizeHigh;
    size.LowPart = fad.nFileSizeLow;
    if (size.QuadPart < kLogRotateBytes) return;
    const std::wstring bak = logPath + L".1";
    DeleteFileW(bak.c_str());
    MoveFileW(logPath.c_str(), bak.c_str());
}

void TruncateLog(const std::wstring& logPath) {
    HANDLE h = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void OpenFolder(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CopyText(const char* text) {
    if (!text || !*text) return;
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t len = strlen(text) + 1;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!mem) {
        CloseClipboard();
        return;
    }
    void* dst = GlobalLock(mem);
    if (dst) {
        memcpy(dst, text, len);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
}

std::string FormatBytes(unsigned long long n) {
    char buf[64]{};
    if (n >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f GB", n / (1024.0 * 1024.0 * 1024.0));
    } else if (n >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    } else if (n >= 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", n);
    }
    return buf;
}

std::string FindJsonNumber(const std::string& body, const char* key, size_t from = 0) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t p = body.find(needle, from);
    if (p == std::string::npos) return "?";
    size_t i = p + needle.size();
    while (i < body.size() && body[i] == ' ') ++i;
    size_t j = i;
    while (j < body.size() &&
           (isdigit(static_cast<unsigned char>(body[j])) || body[j] == '-' || body[j] == '.')) {
        ++j;
    }
    return body.substr(i, j - i);
}

std::string FindJsonString(const std::string& body, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    size_t i = p + needle.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != '"') return "";
    ++i;
    size_t j = i;
    while (j < body.size() && body[j] != '"') ++j;
    return body.substr(i, j - i);
}

struct ReleaseInfo {
    std::string version;
    uint32_t buildId = 0;
    std::string zipName;
    std::string sha256;
};

std::wstring ReleasePath(const OpsState& st, const wchar_t* name) {
    return JoinPath(st.repoRoot, std::wstring(L"artifacts\\release\\") + name);
}

bool ReadTextFile(const std::wstring& path, std::string& text) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    text = stream.str();
    return !file.bad();
}

bool ParseReleaseInfo(const std::string& body, ReleaseInfo& out) {
    out = {};
    out.version = FindJsonString(body, "version");
    out.zipName = FindJsonString(body, "zipName");
    out.sha256 = FindJsonString(body, "sha256");
    const std::string build = FindJsonNumber(body, "buildId");
    try {
        const unsigned long long parsed = std::stoull(build);
        if (parsed == 0 || parsed > UINT32_MAX) return false;
        out.buildId = static_cast<uint32_t>(parsed);
    } catch (...) {
        return false;
    }
    const std::filesystem::path zipPath(xcat::Utf8ToWide(out.zipName));
    if (out.version.empty() || out.zipName.empty() || zipPath.filename().wstring() != zipPath.wstring() ||
        zipPath.extension() != L".zip" || out.sha256.size() != 64) {
        return false;
    }
    if (!std::all_of(out.version.begin(), out.version.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '.' || c == '-' || c == '_';
        })) {
        return false;
    }
    return std::all_of(out.sha256.begin(), out.sha256.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool LoadReleaseInfo(const OpsState& st, const wchar_t* name, ReleaseInfo& out) {
    std::string body;
    return ReadTextFile(ReleasePath(st, name), body) && ParseReleaseInfo(body, out);
}

void RefreshReleaseInfo(OpsState& st) {
    ReleaseInfo latest{};
    if (LoadReleaseInfo(st, L"latest.json", latest)) {
        st.latestClientVersionText = latest.version;
        st.latestClientBuildId = latest.buildId;
    } else {
        st.latestClientVersionText.clear();
        st.latestClientBuildId = 0;
    }

    ReleaseInfo forced{};
    if (LoadReleaseInfo(st, L"force-update.json", forced)) {
        st.forcedClientVersionText = forced.version;
        st.forcedClientBuildId = forced.buildId;
    } else {
        st.forcedClientVersionText.clear();
        st.forcedClientBuildId = 0;
    }
}

bool WriteForceUpdate(const OpsState& st, const ReleaseInfo& release, std::string& err) {
    const std::wstring zipPath = ReleasePath(st, xcat::Utf8ToWide(release.zipName).c_str());
    std::error_code existsError;
    if (!std::filesystem::is_regular_file(zipPath, existsError) || existsError) {
        err = "最新发布包不存在，无法推送";
        return false;
    }

    SYSTEMTIME now{};
    GetSystemTime(&now);
    char issuedAt[32]{};
    std::snprintf(issuedAt, sizeof(issuedAt), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::string body =
        "{\n"
        "  \"version\": \"" + release.version + "\",\n"
        "  \"buildId\": " + std::to_string(release.buildId) + ",\n"
        "  \"zipName\": \"" + release.zipName + "\",\n"
        "  \"sha256\": \"" + release.sha256 + "\",\n"
        "  \"issuedAt\": \"" + issuedAt + "\"\n"
        "}\n";

    const std::wstring target = ReleasePath(st, L"force-update.json");
    const std::wstring temp = target + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        file.flush();
        if (!file) {
            err = "写入强制更新标记失败";
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        err = "替换强制更新标记失败";
        return false;
    }
    return true;
}

bool ClearForceUpdate(const OpsState& st, std::string& err) {
    const std::wstring target = ReleasePath(st, L"force-update.json");
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "当前没有强制更新标记";
        return false;
    }
    if (!DeleteFileW(target.c_str())) {
        err = "删除强制更新标记失败";
        return false;
    }
    return true;
}

void RefreshDiskFree(OpsState& st) {
    ULARGE_INTEGER freeBytes{}, totalBytes{};
    if (!GetDiskFreeSpaceExW(st.repoRoot.c_str(), &freeBytes, &totalBytes, nullptr)) {
        st.diskFreeText.clear();
        return;
    }
    st.diskFreeText = "磁盘剩余 " + FormatBytes(freeBytes.QuadPart) + " / " + FormatBytes(totalBytes.QuadPart);
}

bool StopTwmsHardLocked(OpsState& st) {
    st.twms.Stop();
    KillListenersOnPort(18789);
    st.twmsHealthOk = false;
    st.twmsReadyOk = false;
    return true;
}

void StopTwmsGraceful(OpsState& st) {
    const bool running = TwmsRunning(st);
    if (running) {
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/shutdown", "{}", 1500);
        for (int i = 0; i < 40; ++i) {
            if (!TwmsRunning(st)) break;
            Sleep(200);
        }
    }
    std::lock_guard<std::mutex> lock(st.procMu);
    StopTwmsHardLocked(st);
}

bool StartTwmsLocked(OpsState& st, std::string& err) {
    if (!st.hasNode) {
        err = "无法启动：缺少 node.exe";
        return false;
    }
    const std::wstring script = JoinPath(st.repoRoot, L"scripts\\twms-update-server.mjs");
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "缺少 scripts\\twms-update-server.mjs";
        return false;
    }

    StopTwmsHardLocked(st);
    EnsureDirs(st.repoRoot);
    const std::wstring logPath = OpsLogTwms(st.repoRoot);
    RotateLogIfHuge(logPath);

    // 对齐对照仓（枫星）：启动前放行入站端口，避免防火墙开档后外网摸不到。
    {
        const std::wstring fwScript = JoinPath(st.repoRoot, L"publish_site\\ensure-firewall.ps1");
        if (GetFileAttributesW(fwScript.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string fwErr;
            RunPowerShellFile(fwScript, L"", st.repoRoot, 20000, fwErr);
        }
    }

    const std::wstring args =
        L"scripts\\twms-update-server.mjs "
        L"--host 0.0.0.0 --port 18789 --base-path /twms "
        L"--release-root artifacts\\release "
        L"--out user_log_uploads "
        L"--accept-profile twms "
        L"--access-log artifacts\\ops_logs\\twms_access.jsonl";

    if (!st.twms.Start(st.nodePath, args, st.repoRoot, logPath, err)) return false;
    st.twmsWanted = true;
    st.twmsDownSinceMs = 0;
    return true;
}

bool StopPublishLocked(OpsState& st) {
    st.publish.Stop();
    KillListenersOnPort(52080);
    st.publishProbeOk = false;
    return true;
}

bool StartPublishOnly(OpsState& st, std::string& err) {
    if (st.shuttingDown.load()) {
        err = "正在退出";
        return false;
    }
    if (!st.hasPython) {
        err = "无法启动：缺少 python.exe";
        return false;
    }
    const std::wstring publishDir = JoinPath(st.repoRoot, L"publish_site");
    const std::wstring serverPy = JoinPath(publishDir, L"server.py");
    if (GetFileAttributesW(serverPy.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "缺少 publish_site\\server.py";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(st.procMu);
        StopPublishLocked(st);
    }

    const std::wstring stopScript = JoinPath(st.repoRoot, L"publish_site\\stop-server.ps1");
    if (GetFileAttributesW(stopScript.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RunPowerShellFile(stopScript, L"", st.repoRoot, 15000, err);
    }
    if (st.shuttingDown.load()) {
        err = "正在退出";
        return false;
    }

    {
        const std::wstring fwScript = JoinPath(st.repoRoot, L"publish_site\\ensure-firewall.ps1");
        if (GetFileAttributesW(fwScript.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string fwErr;
            RunPowerShellFile(fwScript, L"", st.repoRoot, 20000, fwErr);
        }
    }
    if (st.shuttingDown.load()) {
        err = "正在退出";
        return false;
    }

    // cwd=publish_site so relative downloads/update paths resolve.
    const std::wstring args = L"server.py --host 0.0.0.0 --port 52080 --root .";
    const std::wstring logPath = OpsLogPublish(st.repoRoot);
    RotateLogIfHuge(logPath);

    EnsureDirs(st.repoRoot);
    std::lock_guard<std::mutex> lock(st.procMu);
    if (!st.publish.Start(st.pythonPath, args, publishDir, logPath, err)) {
        err = "启动发布站失败: " + err;
        return false;
    }
    st.publishWanted = true;
    st.publishDownSinceMs = 0;
    return true;
}

bool SyncPublishPackages(OpsState& st, std::string& err) {
    const std::wstring syncScript = JoinPath(st.repoRoot, L"publish_site\\publish-latest-from-repo.ps1");
    if (GetFileAttributesW(syncScript.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "缺少 publish-latest-from-repo.ps1";
        return false;
    }
    SetStatus(st, "正在同步发布包…");
    if (!RunPowerShellFile(syncScript, L"-Repo \"" + st.repoRoot + L"\"", st.repoRoot, 60000, err)) {
        err = "发布站同步失败: " + err;
        return false;
    }
    return true;
}

template <typename Fn>
void RunAsync(OpsState& st, Fn&& fn) {
    if (st.shuttingDown.load()) return;
    if (st.busy.exchange(true)) {
        SetStatus(st, "已有启动/停止任务进行中…");
        return;
    }
    if (st.worker.joinable()) st.worker.join();
    st.worker = std::thread([&st, fn = std::forward<Fn>(fn)]() mutable {
        fn();
        st.busy.store(false);
    });
}

}  // namespace

void OpsState_Init(OpsState& st) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    st.repoRoot = p.parent_path().parent_path().wstring();
    st.repoRootUtf8 = xcat::WideToUtf8(st.repoRoot);

    st.hasNode = FindOnPath(L"node", st.nodePath);
    st.hasPython = FindOnPath(L"python", st.pythonPath);
    EnsureDirs(st.repoRoot);
    RefreshDiskFree(st);

    if (!st.hasNode) SetStatus(st, "未找到 node.exe（请安装 Node.js 并加入 PATH）");
    else if (!st.hasPython) SetStatus(st, "未找到 python.exe（请安装 Python 并加入 PATH）");
    else SetStatus(st, "正在自动启动服务…");
    st.autoStartPending = st.hasNode || st.hasPython;
}

void OpsState_Shutdown(OpsState& st) {
    st.autoStartPending = false;
    st.twmsWanted = false;
    st.publishWanted = false;
    st.shuttingDown.store(true);
    SetStatus(st, "正在停止服务并退出…");
    if (st.worker.joinable()) st.worker.join();
    st.busy.store(false);

    StopTwmsGraceful(st);
    std::lock_guard<std::mutex> lock(st.procMu);
    StopPublishLocked(st);
}

void Ops_RequestStartTwms(OpsState& st) {
    RunAsync(st, [&st]() {
        SetStatus(st, "正在启动 TWMS 更新 API…");
        std::string err;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(st.procMu);
            ok = StartTwmsLocked(st, err);
        }
        if (ok) SetStatus(st, "TWMS 更新服务已启动 PID=" + std::to_string(TwmsPid(st)));
        else SetStatus(st, err);
    });
}

void Ops_RequestStopTwms(OpsState& st) {
    RunAsync(st, [&st]() {
        st.twmsWanted = false;
        SetStatus(st, "正在停止 TWMS 更新 API…");
        StopTwmsGraceful(st);
        SetStatus(st, "TWMS 更新服务已停止");
    });
}

void Ops_RequestRestartTwms(OpsState& st) {
    RunAsync(st, [&st]() {
        SetStatus(st, "正在重启 TWMS 更新 API…");
        StopTwmsGraceful(st);
        std::string err;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(st.procMu);
            ok = StartTwmsLocked(st, err);
        }
        if (ok) SetStatus(st, "TWMS 更新服务已启动 PID=" + std::to_string(TwmsPid(st)));
        else SetStatus(st, err);
    });
}

void Ops_RequestStartPublish(OpsState& st) {
    RunAsync(st, [&st]() {
        SetStatus(st, "正在启动发布站…");
        std::string err;
        const bool ok = StartPublishOnly(st, err);
        if (!ok) {
            SetStatus(st, err);
            return;
        }
        SetStatus(st, "发布站已启动，正在后台同步发布包…");
        std::string syncErr;
        if (SyncPublishPackages(st, syncErr)) {
            SetStatus(st, "发布站已启动 PID=" + std::to_string(PublishPid(st)) + "（已同步）");
        } else {
            SetStatus(st, "发布站已启动，但同步失败：" + syncErr);
        }
    });
}

void Ops_RequestStopPublish(OpsState& st) {
    RunAsync(st, [&st]() {
        std::lock_guard<std::mutex> lock(st.procMu);
        st.publishWanted = false;
        StopPublishLocked(st);
        SetStatus(st, "发布站已停止");
    });
}

void Ops_RequestRestartPublish(OpsState& st) {
    Ops_RequestStartPublish(st);
}

void Ops_RequestSyncPublish(OpsState& st) {
    RunAsync(st, [&st]() {
        std::string err;
        if (SyncPublishPackages(st, err)) SetStatus(st, "发布包同步完成");
        else SetStatus(st, err);
    });
}

void Ops_RequestForceClientUpdate(OpsState& st) {
    RunAsync(st, [&st]() {
        ReleaseInfo latest{};
        if (!LoadReleaseInfo(st, L"latest.json", latest)) {
            SetStatus(st, "推送失败：最新发布 manifest 无效或不存在");
            return;
        }
        std::string err;
        if (!WriteForceUpdate(st, latest, err)) {
            SetStatus(st, "推送失败：" + err);
            return;
        }
        SetStatus(st, "已推送强制更新 v" + latest.version + " build " +
                          std::to_string(latest.buildId) + "（客户端将在下次轮询时下载安装）");
    });
}

void Ops_RequestClearForceClientUpdate(OpsState& st) {
    RunAsync(st, [&st]() {
        std::string err;
        if (!ClearForceUpdate(st, err)) {
            SetStatus(st, err);
            return;
        }
        st.forcedClientVersionText.clear();
        st.forcedClientBuildId = 0;
        SetStatus(st, "已取消强制更新标记");
    });
}

void Ops_RequestStartAll(OpsState& st) {
    RunAsync(st, [&st]() {
        SetStatus(st, "正在自动启动服务…");
        std::string twmsErr;
        std::string publishErr;
        bool a = false;
        bool p = false;
        if (st.hasNode) {
            std::lock_guard<std::mutex> lock(st.procMu);
            a = StartTwmsLocked(st, twmsErr);
        }
        if (!st.shuttingDown.load() && st.hasPython) {
            SetStatus(st, a ? "TWMS API 已启动，正在启动发布站…" : "正在启动发布站…");
            p = StartPublishOnly(st, publishErr);
        }
        if (a && p) {
            SetStatus(st, "服务已启动，正在后台同步发布包…");
            std::string syncErr;
            if (SyncPublishPackages(st, syncErr)) {
                SetStatus(st, "全部服务已启动（发布包已同步）");
            } else {
                SetStatus(st, "服务已启动，同步失败：" + syncErr);
            }
        } else if (a) {
            SetStatus(st, "仅 TWMS API 启动成功：" + publishErr);
        } else if (p) {
            SetStatus(st, "仅发布站启动成功：" + twmsErr);
        } else {
            SetStatus(st, "启动失败：" + (twmsErr.empty() ? publishErr : twmsErr));
        }
    });
}

void Ops_RequestStopAll(OpsState& st) {
    RunAsync(st, [&st]() {
        st.twmsWanted = false;
        st.publishWanted = false;
        SetStatus(st, "正在停止全部服务…");
        StopTwmsGraceful(st);
        {
            std::lock_guard<std::mutex> lock(st.procMu);
            StopPublishLocked(st);
        }
        SetStatus(st, "全部服务已停止");
    });
}

void RefreshClients(OpsState& st, bool force, bool refreshGeo = false);

void OpsState_Tick(OpsState& st) {
    if (st.autoStartPending) {
        st.autoStartPending = false;
        Ops_RequestStartAll(st);
    }

    if (st.busy.load() || st.shuttingDown.load()) return;

    const ULONGLONG now = GetTickCount64();

    if (st.autoRestart) {
        if (st.twmsWanted && st.hasNode && !TwmsRunning(st)) {
            if (st.twmsDownSinceMs == 0) st.twmsDownSinceMs = now;
            else if (now - st.twmsDownSinceMs >= st.twmsBackoffMs && !st.busy.load()) {
                st.twmsRestartCount += 1;
                SetStatus(st, "检测到 TWMS API 退出，正在自动拉起…");
                Ops_RequestStartTwms(st);
                st.twmsDownSinceMs = now;
                st.twmsBackoffMs = (std::min)(st.twmsBackoffMs * 2, kWatchdogMaxBackoffMs);
            }
        } else if (TwmsRunning(st) && st.twmsHealthOk) {
            if (st.twmsHealthySinceMs == 0) st.twmsHealthySinceMs = now;
            else if (now - st.twmsHealthySinceMs >= kHealthyResetMs) {
                st.twmsBackoffMs = kWatchdogGraceMs;
            }
            st.twmsDownSinceMs = 0;
        } else {
            st.twmsHealthySinceMs = 0;
            if (TwmsRunning(st)) st.twmsDownSinceMs = 0;
        }

        if (st.publishWanted && st.hasPython && !PublishRunning(st)) {
            if (st.publishDownSinceMs == 0) st.publishDownSinceMs = now;
            else if (now - st.publishDownSinceMs >= st.publishBackoffMs && !st.busy.load()) {
                st.publishRestartCount += 1;
                SetStatus(st, "检测到发布站退出，正在自动拉起…");
                Ops_RequestStartPublish(st);
                st.publishDownSinceMs = now;
                st.publishBackoffMs = (std::min)(st.publishBackoffMs * 2, kWatchdogMaxBackoffMs);
            }
        } else if (PublishRunning(st) && st.publishProbeOk) {
            if (st.publishHealthySinceMs == 0) st.publishHealthySinceMs = now;
            else if (now - st.publishHealthySinceMs >= kHealthyResetMs) {
                st.publishBackoffMs = kWatchdogGraceMs;
            }
            st.publishDownSinceMs = 0;
        } else {
            st.publishHealthySinceMs = 0;
            if (PublishRunning(st)) st.publishDownSinceMs = 0;
        }
    }

    if (st.lastProbeMs != 0 && now - st.lastProbeMs < 1000) return;
    st.lastProbeMs = now;

    if (st.lastDiskMs == 0 || now - st.lastDiskMs >= 30000) {
        st.lastDiskMs = now;
        RefreshDiskFree(st);
    }
    if (st.lastReleaseReadMs == 0 || now - st.lastReleaseReadMs >= 5000) {
        st.lastReleaseReadMs = now;
        RefreshReleaseInfo(st);
    }

    if (TwmsRunning(st)) {
        const auto h = HttpGet(L"127.0.0.1", 18789, L"/twms/health", 400);
        st.twmsHealthOk = h.ok;
        if (h.ok) {
            st.twmsHealthText = "HTTP " + std::to_string(h.status);
            st.serverVersionText = FindJsonString(h.body, "version");
            const std::string up = FindJsonNumber(h.body, "uptimeSec");
            st.twmsUptimeText = up == "?" ? "" : ("uptime " + up + "s");
            const size_t upPos = h.body.find("\"uploads\"");
            const bool uploadStub =
                upPos != std::string::npos &&
                (h.body.find("\"stub\":true", upPos) != std::string::npos ||
                 h.body.find("\"stub\": true", upPos) != std::string::npos);
            if (uploadStub) {
                st.twmsStatsText = "日志上传：未启用（API stub 501）";
            } else if (upPos != std::string::npos) {
                auto nested = [&](const char* key) {
                    return FindJsonNumber(h.body, key, upPos);
                };
                const std::string bytes = nested("bytesReceived");
                st.twmsStatsText = "upload ok=" + nested("ok") + " reject=" + nested("rejected") +
                                   " fail=" + nested("failed") + " inflight=" + nested("inFlight") +
                                   " bytes=" +
                                   (bytes == "?" ? "?"
                                                 : FormatBytes(_strtoui64(bytes.c_str(), nullptr, 10)));
            } else {
                st.twmsStatsText.clear();
            }
            const size_t reqPos = h.body.find("\"requests\"");
            auto reqNum = [&](const char* key) {
                return FindJsonNumber(h.body, key, reqPos == std::string::npos ? 0 : reqPos);
            };
            const size_t kindPos = h.body.find("\"byKind\"");
            auto kindNum = [&](const char* key) {
                return FindJsonNumber(h.body, key, kindPos == std::string::npos ? 0 : kindPos);
            };
            st.twmsRequestText = "req total=" + reqNum("total") +
                                   "  upd=" + kindNum("update") + " up=" + kindNum("upload") +
                                   " 404=" + kindNum("notFound");
        } else {
            st.twmsHealthText = h.error.empty() ? ("HTTP " + std::to_string(h.status)) : h.error;
            st.twmsStatsText.clear();
            st.twmsRequestText.clear();
            st.twmsUptimeText.clear();
        }

        const auto r = HttpGet(L"127.0.0.1", 18789, L"/twms/ready", 400);
        st.twmsReadyOk = r.ok;
    } else {
        st.twmsHealthOk = false;
        st.twmsReadyOk = false;
        st.twmsHealthText = "未运行";
        st.twmsStatsText.clear();
        st.twmsRequestText.clear();
        st.twmsUptimeText.clear();
        st.serverVersionText.clear();
    }

    if (PublishRunning(st)) {
        const auto h = HttpGet(L"127.0.0.1", 52080, L"/health", 400);
        st.publishProbeOk = h.ok;
        st.publishProbeText = h.ok ? ("HTTP " + std::to_string(h.status))
                                   : (h.error.empty() ? ("HTTP " + std::to_string(h.status)) : h.error);
    } else {
        st.publishProbeOk = false;
        st.publishProbeText = "未运行";
    }

    // 服务页 Tab 角标 / 健康条需要在线数；RefreshClients 内部 5s 节流
    if (TwmsRunning(st)) {
        RefreshClients(st, false);
    } else if (!st.clients.empty() || st.clientsCount != 0) {
        st.clients.clear();
        st.clientsCount = 0;
        st.clientsTracked = 0;
    }
}

void RefreshLogViewer(OpsState& st, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastLogReadMs != 0 && now - st.lastLogReadMs < 800) return;

    std::wstring wpath;
    const char* label = "TWMS";
    if (st.logTab == 1) {
        wpath = OpsLogPublish(st.repoRoot);
        label = "Publish";
    } else if (st.logTab == 2) {
        wpath = OpsLogHelper(st.repoRoot);
        label = "Helper";
    } else if (st.logTab == 3) {
        wpath = OpsLogAccess(st.repoRoot);
        label = "Access";
    } else {
        wpath = OpsLogTwms(st.repoRoot);
        label = "TWMS";
    }
    st.logPathUtf8 = xcat::WideToUtf8(wpath);

    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(std::filesystem::path(wpath), ec);
    if (!ec) size = static_cast<std::uint64_t>(fsize);
    const auto ftime = std::filesystem::last_write_time(std::filesystem::path(wpath), ec);
    if (!ec) {
        mtime = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
    }
    // 文件未变则不重读，避免整表替换导致滚动条回弹闪烁
    if (!force && size == st.logFileSize && mtime == st.logFileMtime && st.lastLogReadMs != 0) {
        st.lastLogReadMs = now;
        return;
    }

    st.lastLogReadMs = now;
    st.logFileSize = size;
    st.logFileMtime = mtime;
    auto lines = ReadLogTail(st.logPathUtf8.c_str(), 300, 192 * 1024);
    const bool grew = lines.size() > st.logLines.size() ||
                      (size > 0 && !lines.empty() &&
                       (st.logLines.empty() || lines.back() != st.logLines.back()));
    st.logLines = std::move(lines);
    if (st.logLines.empty()) {
        st.logEmptyHint = std::string("(暂无 ") + label + " 日志)  " + st.logPathUtf8;
        st.logScrollToBottom = false;
    } else {
        st.logEmptyHint.clear();
        if (st.logAutoScroll && (force || grew)) st.logScrollToBottom = true;
    }
}

ImVec4 LogLineColor(const std::string& line) {
    if (line.find(" ERROR ") != std::string::npos || line.find(" FATAL ") != std::string::npos ||
        line.find("Error:") != std::string::npos || line.find("FAILED") != std::string::npos) {
        return OpsTone::Danger();
    }
    if (line.find(" WARN ") != std::string::npos || line.find("Warning") != std::string::npos) {
        return OpsTone::Warn();
    }
    if (line.find(" INFO ") != std::string::npos) {
        return OpsTone::Info();
    }
    return OpsTone::Body();
}

bool LogLineIsError(const std::string& line) {
    return line.find(" ERROR ") != std::string::npos || line.find(" FATAL ") != std::string::npos ||
           line.find("Error:") != std::string::npos || line.find("FAILED") != std::string::npos;
}

bool LogLineIsWarn(const std::string& line) {
    return line.find(" WARN ") != std::string::npos || line.find("Warning") != std::string::npos;
}

ImVec4 IdleSecColor(int idleSec) {
    if (idleSec <= 20) return OpsTone::Ok();
    if (idleSec <= 70) return OpsTone::Muted();
    if (idleSec <= 120) return OpsTone::Warn();
    return OpsTone::Danger();
}

void FormatIdleSec(int idleSec, char* buf, size_t bufSize) {
    if (idleSec < 0) idleSec = 0;
    if (idleSec < 120) {
        std::snprintf(buf, bufSize, "%ds", idleSec);
    } else if (idleSec < 3600) {
        std::snprintf(buf, bufSize, "%dm", (idleSec + 30) / 60);
    } else {
        std::snprintf(buf, bufSize, "%dh", (idleSec + 1800) / 3600);
    }
}

void CopyClientSummary(const OpsState::ConnectedClient& c) {
    char buf[512]{};
    std::snprintf(buf, sizeof(buf),
                  "%s\t%s\t%s\t%s\t%s\t%s\t%s\tLv.%d\t%s\t%s\tgate=%s\tidle=%ds",
                  c.ip.c_str(), c.machine.c_str(), c.mac.c_str(), c.token.c_str(),
                  c.deviceId.c_str(), c.appVersion.c_str(),
                  c.charName.empty() ? "-" : c.charName.c_str(), c.charLevel,
                  c.charJobName.empty() ? "-" : c.charJobName.c_str(),
                  c.charMeso.empty() ? "-" : c.charMeso.c_str(),
                  c.gate.empty() ? "?" : c.gate.c_str(), c.idleSec);
    CopyText(buf);
}

void AppendClientSummaryLine(std::string& out, const OpsState::ConnectedClient& c) {
    char buf[512]{};
    std::snprintf(buf, sizeof(buf), "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%d\n",
                  c.ip.c_str(), c.machine.c_str(), c.mac.c_str(), c.token.c_str(),
                  c.deviceId.c_str(), c.appVersion.c_str(),
                  c.charName.empty() ? "-" : c.charName.c_str(), c.charLevel,
                  c.charJobName.empty() ? "-" : c.charJobName.c_str(),
                  c.charMeso.empty() ? "-" : c.charMeso.c_str(),
                  c.gate.empty() ? "?" : c.gate.c_str(), c.idleSec);
    out += buf;
}

void FormatMesoDisplay(const std::string& mesoRaw, char* out, size_t outN) {
    if (!out || outN == 0) return;
    out[0] = '\0';
    if (mesoRaw.empty()) {
        std::snprintf(out, outN, "—");
        return;
    }
    // 万/亿单位（与标题栏金币紧凑显示一致）：<1万原样，≥1万用万，≥1亿用亿。
    std::string digits;
    digits.reserve(mesoRaw.size());
    bool neg = false;
    for (size_t i = 0; i < mesoRaw.size(); ++i) {
        const char c = mesoRaw[i];
        if (i == 0 && c == '-') {
            neg = true;
            continue;
        }
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    if (digits.empty()) {
        std::snprintf(out, outN, "%s", mesoRaw.c_str());
        return;
    }
    // 超长（>18 位）不进 long long，退回截断原串。
    if (digits.size() > 18) {
        std::snprintf(out, outN, "%s%s…", neg ? "-" : "", digits.substr(0, 12).c_str());
        return;
    }
    char* end = nullptr;
    const unsigned long long absVal = std::strtoull(digits.c_str(), &end, 10);
    if (!end || *end != '\0') {
        std::snprintf(out, outN, "%s", mesoRaw.c_str());
        return;
    }
    const char* sign = neg ? "-" : "";
    if (absVal < 10000ull) {
        std::snprintf(out, outN, "%s%llu", sign, absVal);
        return;
    }
    if (absVal >= 100000000ull) {
        const double scaled = static_cast<double>(absVal) / 100000000.0;
        if (scaled >= 100.0)
            std::snprintf(out, outN, "%s%.0f亿", sign, scaled);
        else if (scaled >= 10.0)
            std::snprintf(out, outN, "%s%.1f亿", sign, scaled);
        else
            std::snprintf(out, outN, "%s%.2f亿", sign, scaled);
        return;
    }
    const double scaled = static_cast<double>(absVal) / 10000.0;
    if (scaled >= 100.0)
        std::snprintf(out, outN, "%s%.0f万", sign, scaled);
    else if (scaled >= 10.0)
        std::snprintf(out, outN, "%s%.1f万", sign, scaled);
    else
        std::snprintf(out, outN, "%s%.2f万", sign, scaled);
}

ImVec4 AppVersionColor(const OpsState& st, const std::string& ver) {
    if (ver.empty()) return OpsTone::Muted();
    if (!st.latestClientVersionText.empty() && ver == st.latestClientVersionText)
        return OpsTone::Ok();
    if (!st.latestClientVersionText.empty()) return OpsTone::Warn();
    return ImVec4(0.78f, 0.80f, 0.84f, 1.f);
}

int JsonIntField(const std::string& obj, const char* key, int fallback = 0) {
    const std::string raw = FindJsonNumber(obj, key);
    if (raw.empty() || raw == "?") return fallback;
    return static_cast<int>(std::strtol(raw.c_str(), nullptr, 10));
}

bool ParseClientsPayload(const std::string& body, OpsState& st) {
    st.clients.clear();
    st.recentDenies.clear();
    st.ipAlerts.clear();
    st.ipAlertCount = JsonIntField(body, "ipMultiDeviceAlertCount", 0);
    st.clientsCount = JsonIntField(body, "count", 0);
    st.clientsTracked = JsonIntField(body, "tracked", 0);
    st.clientsGeoProvider = FindJsonString(body, "geoProvider");
    const int activeSec = JsonIntField(body, "activeSec", st.clientsActiveSec);
    if (activeSec > 0) st.clientsActiveSec = activeSec;
    {
        const std::string modeHdr = FindJsonString(body, "accessMode");
        if (modeHdr == "allow" || modeHdr == "deny") st.accessMode = modeHdr;
    }

    auto parseObjArray = [&](const char* key, auto&& onObj) {
        const size_t arrKey = body.find(std::string("\"") + key + "\"");
        if (arrKey == std::string::npos) return;
        size_t i = body.find('[', arrKey);
        if (i == std::string::npos) return;
        ++i;
        while (i < body.size()) {
            while (i < body.size() &&
                   (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n' ||
                    body[i] == ',')) {
                ++i;
            }
            if (i >= body.size() || body[i] == ']') break;
            if (body[i] != '{') break;
            const size_t start = i;
            int depth = 0;
            bool inStr = false;
            for (; i < body.size(); ++i) {
                const char c = body[i];
                if (inStr) {
                    if (c == '\\' && i + 1 < body.size()) {
                        ++i;
                        continue;
                    }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') {
                    inStr = true;
                    continue;
                }
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
            if (depth != 0) break;
            onObj(body.substr(start, i - start));
        }
    };

    parseObjArray("recentDenies", [&](const std::string& obj) {
        OpsState::AccessDenyHit hit;
        hit.at = FindJsonString(obj, "at");
        hit.ip = FindJsonString(obj, "ip");
        hit.machine = FindJsonString(obj, "machine");
        hit.deviceId = FindJsonString(obj, "deviceId");
        hit.mac = FindJsonString(obj, "mac");
        hit.token = FindJsonString(obj, "token");
        hit.reason = FindJsonString(obj, "reason");
        hit.match = FindJsonString(obj, "match");
        hit.mode = FindJsonString(obj, "mode");
        if (hit.at.empty() && hit.ip.empty()) return;
        st.recentDenies.push_back(std::move(hit));
    });

    parseObjArray("ipMultiDeviceAlerts", [&](const std::string& obj) {
        OpsState::IpMultiDeviceAlert alert;
        alert.ip = FindJsonString(obj, "ip");
        alert.geo = FindJsonString(obj, "geo");
        alert.deviceCount = JsonIntField(obj, "deviceCount", 0);
        // 从 devices 数组拼摘要（简易：截一段原文里的 machine/deviceId）
        std::string summary;
        size_t pos = 0;
        int n = 0;
        while (n < 4) {
            const size_t m = obj.find("\"machine\"", pos);
            if (m == std::string::npos) break;
            const std::string piece = obj.substr(m, (std::min)(size_t(180), obj.size() - m));
            const std::string machine = FindJsonString(piece, "machine");
            const std::string deviceId = FindJsonString(piece, "deviceId");
            if (!summary.empty()) summary += " · ";
            if (!machine.empty()) summary += machine;
            if (!deviceId.empty()) {
                if (!machine.empty()) summary += "/";
                summary += deviceId.size() > 8 ? deviceId.substr(0, 8) : deviceId;
            }
            ++n;
            pos = m + 8;
        }
        if (alert.deviceCount > n && n > 0) summary += " …";
        alert.summary = summary;
        if (alert.ip.empty()) return;
        st.ipAlerts.push_back(std::move(alert));
    });
    if (st.ipAlertCount <= 0) st.ipAlertCount = static_cast<int>(st.ipAlerts.size());

    parseObjArray("clients", [&](const std::string& obj) {
        OpsState::ConnectedClient row;
        row.ip = FindJsonString(obj, "ip");
        if (row.ip.empty()) return;
        row.geo = FindJsonString(obj, "geo");
        row.geoStatus = FindJsonString(obj, "geoStatus");
        row.machine = FindJsonString(obj, "machine");
        row.deviceId = FindJsonString(obj, "deviceId");
        row.device = FindJsonString(obj, "device");
        row.mac = FindJsonString(obj, "mac");
        row.token = FindJsonString(obj, "token");
        row.appVersion = FindJsonString(obj, "appVersion");
        row.charName = FindJsonString(obj, "charName");
        row.charJobName = FindJsonString(obj, "charJobName");
        row.charMeso = FindJsonString(obj, "charMeso");
        row.charLevel = JsonIntField(obj, "charLevel", 0);
        row.charJob = JsonIntField(obj, "charJob", 0);
        row.lastKind = FindJsonString(obj, "lastKind");
        row.lastSeenAt = FindJsonString(obj, "lastSeenAt");
        row.lastDenyAt = FindJsonString(obj, "lastDenyAt");
        row.lastDenyReason = FindJsonString(obj, "lastDenyReason");
        row.lastDenyMatch = FindJsonString(obj, "lastDenyMatch");
        row.lastAllowAt = FindJsonString(obj, "lastAllowAt");
        row.gate = FindJsonString(obj, "gate");
        row.idleSec = JsonIntField(obj, "idleSec", 0);
        row.hits = JsonIntField(obj, "hits", 0);
        row.lastStatus = JsonIntField(obj, "lastStatus", 0);
        row.sameIpOnline = JsonIntField(obj, "sameIpOnline", 1);
        row.knownOnIp = JsonIntField(obj, "knownOnIp", 0);
        row.leaseRemainSec = JsonIntField(obj, "leaseRemainSec", 0);
        row.leaseTtlHours = JsonIntField(obj, "leaseTtlHours", 64);
        row.identified = obj.find("\"identified\":true") != std::string::npos;
        row.banned = obj.find("\"banned\":true") != std::string::npos;
        row.allowed = obj.find("\"allowed\":true") != std::string::npos;
        if (!row.identified && !row.machine.empty() && !row.device.empty()) row.identified = true;
        st.clients.push_back(std::move(row));
    });

    if (st.clientsCount <= 0) st.clientsCount = static_cast<int>(st.clients.size());
    return true;
}

std::string JsonEscapeLocal(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            continue;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool ParseBansPayload(const std::string& body, OpsState& st) {
    st.bans.clear();
    st.allows.clear();
    st.bansCount = JsonIntField(body, "banCount", JsonIntField(body, "count", 0));
    st.allowsCount = JsonIntField(body, "allowCount", 0);
    st.accessMode = FindJsonString(body, "mode");
    if (st.accessMode != "allow") st.accessMode = "deny";

    auto parseList = [&](const char* key, std::vector<OpsState::BannedDevice>& out) {
        const size_t arrKey = body.find(std::string("\"") + key + "\"");
        if (arrKey == std::string::npos) return;
        size_t i = body.find('[', arrKey);
        if (i == std::string::npos) return;
        ++i;
        while (i < body.size()) {
            while (i < body.size() &&
                   (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n' ||
                    body[i] == ',')) {
                ++i;
            }
            if (i >= body.size() || body[i] == ']') break;
            if (body[i] != '{') break;
            const size_t start = i;
            int depth = 0;
            bool inStr = false;
            for (; i < body.size(); ++i) {
                const char c = body[i];
                if (inStr) {
                    if (c == '\\' && i + 1 < body.size()) {
                        ++i;
                        continue;
                    }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') {
                    inStr = true;
                    continue;
                }
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
            if (depth != 0) break;
            const std::string obj = body.substr(start, i - start);
            OpsState::BannedDevice row;
            row.key = FindJsonString(obj, "key");
            row.machine = FindJsonString(obj, "machine");
            row.deviceId = FindJsonString(obj, "deviceId");
            row.mac = FindJsonString(obj, "mac");
            row.token = FindJsonString(obj, "token");
            row.device = FindJsonString(obj, "device");
            row.reason = FindJsonString(obj, "reason");
            row.bannedAt = FindJsonString(obj, "at");
            if (row.bannedAt.empty()) row.bannedAt = FindJsonString(obj, "bannedAt");
            if (row.key.empty() && row.machine.empty() && row.deviceId.empty() && row.mac.empty() &&
                row.token.empty()) {
                continue;
            }
            out.push_back(std::move(row));
        }
    };
    parseList("bans", st.bans);
    parseList("allows", st.allows);
    if (st.bansCount <= 0) st.bansCount = static_cast<int>(st.bans.size());
    if (st.allowsCount <= 0) st.allowsCount = static_cast<int>(st.allows.size());
    return body.find("\"ok\":true") != std::string::npos || !st.bans.empty() ||
           !st.allows.empty() || body.find("\"mode\"") != std::string::npos;
}

void RefreshBans(OpsState& st, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastBansFetchMs != 0 && now - st.lastBansFetchMs < 5000) return;
    st.lastBansFetchMs = now;

    if (!TwmsRunning(st)) {
        st.bans.clear();
        st.bansCount = 0;
        st.bansError = "TWMS API 未运行";
        return;
    }

    const auto r = HttpGet(L"127.0.0.1", 18789, L"/twms/admin/bans", 1500, 256 * 1024);
    if (!r.ok) {
        st.bans.clear();
        st.bansCount = 0;
        if (r.status == 404) {
            st.bansError = "接口不存在：请重启 TWMS 更新服务以加载封禁能力";
        } else if (!r.error.empty()) {
            st.bansError = r.error;
        } else {
            st.bansError = "HTTP " + std::to_string(r.status);
        }
        return;
    }
    if (!ParseBansPayload(r.body, st)) {
        st.bansError = "解析 bans 响应失败";
        return;
    }
    st.bansError.clear();
}

bool PostBanAction(OpsState& st, const char* action, const std::string& machine,
                   const std::string& deviceId, const std::string& reason, const std::string& key,
                   std::string& err, const std::string& mac = {}, const std::string& mode = {},
                   const std::string& token = {}) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    std::string body = std::string("{\"action\":\"") + action + "\"";
    if (!key.empty()) body += ",\"key\":\"" + JsonEscapeLocal(key) + "\"";
    if (!machine.empty()) body += ",\"machine\":\"" + JsonEscapeLocal(machine) + "\"";
    if (!deviceId.empty()) body += ",\"deviceId\":\"" + JsonEscapeLocal(deviceId) + "\"";
    if (!mac.empty()) body += ",\"mac\":\"" + JsonEscapeLocal(mac) + "\"";
    if (!token.empty()) body += ",\"token\":\"" + JsonEscapeLocal(token) + "\"";
    if (!reason.empty()) body += ",\"reason\":\"" + JsonEscapeLocal(reason) + "\"";
    if (!mode.empty()) body += ",\"mode\":\"" + JsonEscapeLocal(mode) + "\"";
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/access", body.c_str(), 2500, 64 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "访问策略操作失败";
        return false;
    }
    ParseBansPayload(r.body, st);
    return true;
}

void RefreshClients(OpsState& st, bool force, bool refreshGeo) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastClientsFetchMs != 0 && now - st.lastClientsFetchMs < 5000) return;
    st.lastClientsFetchMs = now;

    if (!TwmsRunning(st)) {
        st.clients.clear();
        st.clientsCount = 0;
        st.clientsTracked = 0;
        st.clientsError = "TWMS API 未运行";
        return;
    }

    std::wstring path =
        L"/twms/admin/clients?activeSec=" + std::to_wstring(st.clientsActiveSec);
    if (refreshGeo) path += L"&refreshGeo=1";
    const auto r = HttpGet(L"127.0.0.1", 18789, path.c_str(), 1500, 256 * 1024);
    if (!r.ok) {
        st.clients.clear();
        st.clientsCount = 0;
        st.clientsTracked = 0;
        if (r.status == 404) {
            st.clientsError = "接口不存在：请重启 TWMS 更新服务以加载新版 twms-update-server";
        } else if (!r.error.empty()) {
            st.clientsError = r.error;
        } else {
            st.clientsError = "HTTP " + std::to_string(r.status);
        }
        return;
    }
    if (!ParseClientsPayload(r.body, st)) {
        st.clientsError = "解析 clients 响应失败";
        return;
    }
    st.clientsError.clear();
}

void DrawMainTabButtons(OpsState& st) {
    auto tabBtn = [&](const char* label, int id) {
        const bool selected = (st.mainTab == id);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label) && !selected) {
            st.mainTab = id;
            if (id == 1) {
                RefreshClients(st, true);
                RefreshBans(st, true);
            }
        }
        if (selected) ImGui::PopStyleColor();
    };
    tabBtn("服务与日志##maintab", 0);
    ImGui::SameLine();
    {
        char accessTab[80]{};
        const bool allowMode = st.accessMode == "allow";
        if (allowMode && st.clientsCount > 0)
            std::snprintf(accessTab, sizeof(accessTab), "连接与访问 (%d)·仅白##maintab",
                          st.clientsCount);
        else if (allowMode)
            std::snprintf(accessTab, sizeof(accessTab), "连接与访问 ·仅白##maintab");
        else if (st.clientsCount > 0)
            std::snprintf(accessTab, sizeof(accessTab), "连接与访问 (%d)##maintab", st.clientsCount);
        else
            std::snprintf(accessTab, sizeof(accessTab), "连接与访问##maintab");
        if (allowMode) {
            ImGui::PushStyleColor(ImGuiCol_Text, OpsTone::Warn());
            tabBtn(accessTab, 1);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("当前访问策略：仅白名单（止血中）");
        } else {
            tabBtn(accessTab, 1);
        }
    }
}

void SetOrToggleClientsFilter(OpsState& st, const char* key) {
    if (!key || key[0] == '\0') {
        st.clientsGateFilter[0] = '\0';
        return;
    }
    if (std::strcmp(st.clientsGateFilter, key) == 0) {
        st.clientsGateFilter[0] = '\0';
    } else {
        std::snprintf(st.clientsGateFilter, sizeof(st.clientsGateFilter), "%s", key);
    }
}

bool ClientsFilterIs(const OpsState& st, const char* key) {
    return key && key[0] && std::strcmp(st.clientsGateFilter, key) == 0;
}

void FilterChipButton(OpsState& st, const char* label, const char* filterKey, ImVec4 color) {
    const bool on = ClientsFilterIs(st, filterKey);
    const bool light = OpsIsLight();
    if (on) {
        // 白天：淡色底 + 深色字；暗夜：半透明色底 + 浅色字
        const float a = light ? 0.28f : 0.38f;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, a));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, a + 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              light ? color : ImVec4(0.96f, 0.97f, 0.98f, 1.f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
    }
    if (ImGui::SmallButton(label)) SetOrToggleClientsFilter(st, filterKey);
    if (on) ImGui::PopStyleColor(3);
    else ImGui::PopStyleColor(1);
}

// 按钮底：暗夜/白天各一套，保证白字可读。
ImVec4 BtnDanger() {
    return OpsIsLight() ? ImVec4(0.72f, 0.28f, 0.24f, 1.f) : ImVec4(0.62f, 0.26f, 0.24f, 1.f);
}
ImVec4 BtnDangerHov() {
    return OpsIsLight() ? ImVec4(0.80f, 0.34f, 0.28f, 1.f) : ImVec4(0.72f, 0.32f, 0.28f, 1.f);
}
ImVec4 BtnSafe() {
    return OpsIsLight() ? ImVec4(0.22f, 0.52f, 0.34f, 1.f) : ImVec4(0.22f, 0.48f, 0.34f, 1.f);
}
ImVec4 BtnSafeHov() {
    return OpsIsLight() ? ImVec4(0.26f, 0.58f, 0.38f, 1.f) : ImVec4(0.28f, 0.56f, 0.40f, 1.f);
}
ImVec4 BtnNeutral() {
    return OpsIsLight() ? ImVec4(0.48f, 0.50f, 0.54f, 1.f) : ImVec4(0.36f, 0.38f, 0.42f, 1.f);
}
ImVec4 BtnNeutralHov() {
    return OpsIsLight() ? ImVec4(0.40f, 0.42f, 0.46f, 1.f) : ImVec4(0.44f, 0.46f, 0.50f, 1.f);
}
ImVec4 BtnText() { return ImVec4(0.98f, 0.98f, 0.98f, 1.f); }

void PushActionBtn(ImVec4 bg, ImVec4 hov) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hov);
    ImGui::PushStyleColor(ImGuiCol_Text, BtnText());
}
void PopActionBtn() { ImGui::PopStyleColor(4); }

bool DangerSmallButton(const char* label) {
    PushActionBtn(BtnDanger(), BtnDangerHov());
    const bool hit = ImGui::SmallButton(label);
    PopActionBtn();
    return hit;
}
bool SafeSmallButton(const char* label) {
    PushActionBtn(BtnSafe(), BtnSafeHov());
    const bool hit = ImGui::SmallButton(label);
    PopActionBtn();
    return hit;
}
bool NeutralSmallButton(const char* label) {
    PushActionBtn(BtnNeutral(), BtnNeutralHov());
    const bool hit = ImGui::SmallButton(label);
    PopActionBtn();
    return hit;
}
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    PushActionBtn(BtnDanger(), BtnDangerHov());
    const bool hit = ImGui::Button(label, size);
    PopActionBtn();
    return hit;
}
bool SafeButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    PushActionBtn(BtnSafe(), BtnSafeHov());
    const bool hit = ImGui::Button(label, size);
    PopActionBtn();
    return hit;
}

void FillBanFormFromClient(OpsState& st, const OpsState::ConnectedClient& c) {
    std::snprintf(st.banMachineInput, sizeof(st.banMachineInput), "%s", c.machine.c_str());
    std::snprintf(st.banDeviceIdInput, sizeof(st.banDeviceIdInput), "%s", c.deviceId.c_str());
    std::snprintf(st.banMacInput, sizeof(st.banMacInput), "%s", c.mac.c_str());
    // TOKEN 故意不带：防共享 TOKEN 误伤全队；需要时手动填。
    st.banPassInput[0] = '\0';
    if (st.banReasonInput[0] == '\0') {
        std::snprintf(st.banReasonInput, sizeof(st.banReasonInput), "ops fill");
    }
}

void FillAllowFormFromClient(OpsState& st, const OpsState::ConnectedClient& c) {
    std::snprintf(st.allowMachineInput, sizeof(st.allowMachineInput), "%s", c.machine.c_str());
    std::snprintf(st.allowDeviceIdInput, sizeof(st.allowDeviceIdInput), "%s", c.deviceId.c_str());
    std::snprintf(st.allowMacInput, sizeof(st.allowMacInput), "%s", c.mac.c_str());
    st.allowPassInput[0] = '\0';
    if (st.allowReasonInput[0] == '\0') {
        std::snprintf(st.allowReasonInput, sizeof(st.allowReasonInput), "ops fill");
    }
}

void DrawAccessFormField(const char* label, const char* id, char* buf, size_t bufSize,
                         const char* hint) {
    // 单行：固定标签宽 + 输入，上下对齐更整齐。
    constexpr float kLabelW = 56.f;
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorPosX(x0 + kLabelW);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint(id, hint, buf, bufSize);
}

bool BeginAccessFormGrid(const char* id, int cols) {
    const ImGuiTableFlags tf =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoBordersInBody;
    if (!ImGui::BeginTable(id, cols, tf)) return false;
    for (int i = 0; i < cols; ++i) {
        char cid[8]{};
        std::snprintf(cid, sizeof(cid), "c%d", i);
        ImGui::TableSetupColumn(cid, ImGuiTableColumnFlags_WidthStretch, 1.0f);
    }
    return true;
}

void DrawModeBadge(bool allowMode) {
    const ImVec4 bg = OpsIsLight()
                          ? (allowMode ? ImVec4(0.78f, 0.48f, 0.18f, 1.f) : ImVec4(0.24f, 0.55f, 0.36f, 1.f))
                          : (allowMode ? ImVec4(0.68f, 0.40f, 0.18f, 1.f) : ImVec4(0.20f, 0.50f, 0.32f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, BtnText());
    ImGui::SmallButton(allowMode ? "仅白名单##mode_badge" : "黑名单止血##mode_badge");
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(allowMode ? "仅白名单可继续使用；黑名单仍优先拦截"
                                    : "日常模式：只拦黑名单，未登记也可使用");
    }
}

bool ContainsIgnoreCase(const std::string& hay, const char* needle) {
    if (!needle || needle[0] == '\0') return true;
    auto lower = [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    };
    const size_t nlen = std::strlen(needle);
    if (nlen == 0) return true;
    if (hay.size() < nlen) return false;
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < nlen; ++j) {
            if (lower(static_cast<unsigned char>(hay[i + j])) !=
                lower(static_cast<unsigned char>(needle[j])))
                break;
        }
        if (j == nlen) return true;
    }
    return false;
}

bool ClientMatchesFilter(const OpsState& st, const OpsState::ConnectedClient& c, const char* filter) {
    // 门禁 chip（与文本筛选独立，二者 AND）
    if (st.clientsGateFilter[0] != '\0') {
        if (std::strcmp(st.clientsGateFilter, "__stale__") == 0) {
            if (st.latestClientVersionText.empty() || c.appVersion.empty() ||
                c.appVersion == st.latestClientVersionText)
                return false;
        } else if (c.gate != st.clientsGateFilter) {
            return false;
        }
    }
    if (!filter || filter[0] == '\0') return true;
    return ContainsIgnoreCase(c.ip, filter) || ContainsIgnoreCase(c.machine, filter) ||
           ContainsIgnoreCase(c.mac, filter) || ContainsIgnoreCase(c.token, filter) ||
           ContainsIgnoreCase(c.device, filter) || ContainsIgnoreCase(c.deviceId, filter) ||
           ContainsIgnoreCase(c.geo, filter) || ContainsIgnoreCase(c.gate, filter) ||
           ContainsIgnoreCase(c.appVersion, filter) || ContainsIgnoreCase(c.charName, filter) ||
           ContainsIgnoreCase(c.charJobName, filter);
}

const char* GateFilterLabel(const char* key) {
    if (!key || !key[0]) return "";
    if (std::strcmp(key, "probe_ok") == 0) return "探活OK";
    if (std::strcmp(key, "lease") == 0) return "租约中";
    if (std::strcmp(key, "policy_deny") == 0) return "策略将拒";
    if (std::strcmp(key, "denied") == 0) return "已拒绝";
    if (std::strcmp(key, "__stale__") == 0) return "版本落后";
    return key;
}

ImVec4 StatusMessageColor(const std::string& msg) {
    auto has = [&](const char* s) { return ContainsIgnoreCase(msg, s); };
    if (has("失败") || has("错误") || has("error") || has("缺少") || has("须填") || has("LNK"))
        return OpsTone::Danger();
    if (has("正在") || has("同步中") || has("后台"))
        return OpsTone::Busy();
    if (has("已") || has("成功") || has("完成") || has("启动 PID"))
        return OpsTone::Ok();
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

void DrawClientsPanel(OpsState& st) {
    if (st.clientsAutoRefresh) {
        RefreshClients(st, false);
        RefreshBans(st, false);
    }

    const bool allowMode = st.accessMode == "allow";
    int nProbe = 0, nLease = 0, nPolicy = 0, nDenied = 0, nStaleVer = 0;
    for (const auto& c : st.clients) {
        if (c.gate == "probe_ok") ++nProbe;
        else if (c.gate == "lease") ++nLease;
        else if (c.gate == "policy_deny") ++nPolicy;
        else if (c.gate == "denied") ++nDenied;
        if (!st.latestClientVersionText.empty() && !c.appVersion.empty() &&
            c.appVersion != st.latestClientVersionText)
            ++nStaleVer;
    }
    const int alertN =
        st.ipAlertCount > 0 ? st.ipAlertCount : static_cast<int>(st.ipAlerts.size());

    // ── 策略条：模式徽章 + 切换 + 折叠说明 ──
    ImGui::TextUnformatted("访问策略");
    ImGui::SameLine();
    DrawModeBadge(allowMode);
    ImGui::SameLine();
    ImGui::TextDisabled("60s 探活 · 租约 64h");
    ImGui::SameLine();
    if (allowMode) {
        if (ImGui::SmallButton("切回黑名单##mode_deny")) {
            std::string err;
            if (PostBanAction(st, "setMode", {}, {}, {}, {}, err, {}, "deny")) {
                SetStatus(st, "已切回黑名单（未登记也可使用，仅封禁拦截）");
                RefreshBans(st, true);
                RefreshClients(st, true);
            } else {
                SetStatus(st, err);
            }
        }
        ImGui::SetItemTooltip("日常模式：只拦黑名单；适合平时开发与小范围发放。");
    } else {
        if (ImGui::SmallButton("一键仅白名单（止血）##mode_allow")) {
            ImGui::OpenPopup("confirm_allow_mode");
        }
        ImGui::SetItemTooltip(
            "泄露紧急锁盘：只有白名单可继续使用；黑名单仍优先拦截。\n"
            "切模式前请先把可信设备加进白名单。");
    }
    if (ImGui::BeginPopupModal("confirm_allow_mode", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("确认切换到「仅白名单」？");
        ImGui::Spacing();
        if (st.allowsCount <= 0) {
            ImGui::TextColored(OpsTone::Danger(),
                               "当前白名单为空：所有探活客户端都会被拒绝并写粘性！");
        } else {
            ImGui::Text("当前白名单 %d 台；未登记设备约 60s 内退出并写粘性。", st.allowsCount);
        }
        ImGui::Spacing();
        if (DangerButton("确认切换##allow_yes", ImVec2(120, 0))) {
            std::string err;
            if (PostBanAction(st, "setMode", {}, {}, {}, {}, err, {}, "allow")) {
                SetStatus(st, "已切仅白名单：未登记设备约 60s 内退出");
                RefreshBans(st, true);
                RefreshClients(st, true);
            } else {
                SetStatus(st, err);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##allow_no", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 12.f);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8.f);
    if (ImGui::SmallButton("使用说明##access_help_btn")) {
        ImGui::OpenPopup("access_help_popup");
    }
    if (ImGui::BeginPopup("access_help_popup")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.f);
        ImGui::TextWrapped(
            "家用按需运维：平时可关服。需要限制时开机开本台 → 禁止/切白名单 → 等在线客户端探活命中"
            "（约 1～2 分钟）后即可关服。"
            "已命中会写本机隐蔽粘性（安装目录外），解禁需再次开服让对方探活成功。"
            "成功放行后持有约 64 小时在线租约；掐服/断网超过窗口后无法继续启动或运行。"
            "「门禁」列为服务端按最近放行+64h 估算，与客户端本地租约文件可能有偏差。");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }

    // ── 一眼状态条 ──
    ImGui::PushStyleColor(ImGuiCol_ChildBg, xcat::ui::UiTheme_Palette().statusStripBg);
    ImGui::BeginChild("access_status", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 8.f), true,
                      ImGuiWindowFlags_NoScrollbar);
    if (!st.clientsError.empty()) {
        ImGui::TextColored(OpsTone::DangerSoft(), "%s", st.clientsError.c_str());
    } else {
        ImGui::Text("在线 %d", st.clientsCount);
        ImGui::SameLine(0, 10.f);
        ImGui::TextDisabled("/ 跟踪 %d", st.clientsTracked);
        ImGui::SameLine(0, 14.f);
        {
            char chip[40]{};
            std::snprintf(chip, sizeof(chip), "探活OK %d##f_probe", nProbe);
            FilterChipButton(st, chip, "probe_ok", OpsTone::Ok());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击筛选 gate=probe_ok，再点取消");
        }
        ImGui::SameLine(0, 10.f);
        {
            char chip[40]{};
            std::snprintf(chip, sizeof(chip), "租约中 %d##f_lease", nLease);
            FilterChipButton(st, chip, "lease", OpsTone::Info());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击筛选 gate=lease，再点取消");
        }
        if (nPolicy > 0) {
            ImGui::SameLine(0, 10.f);
            char chip[40]{};
            std::snprintf(chip, sizeof(chip), "策略将拒 %d##f_policy", nPolicy);
            FilterChipButton(st, chip, "policy_deny", OpsTone::Warn());
        }
        if (nDenied > 0) {
            ImGui::SameLine(0, 10.f);
            char chip[40]{};
            std::snprintf(chip, sizeof(chip), "已拒绝 %d##f_denied", nDenied);
            FilterChipButton(st, chip, "denied", OpsTone::Danger());
        }
        ImGui::SameLine(0, 14.f);
        ImGui::Text("封禁 %d", st.bansCount);
        ImGui::SameLine(0, 10.f);
        ImGui::Text("白名单 %d", st.allowsCount);
        if (nStaleVer > 0) {
            ImGui::SameLine(0, 14.f);
            char chip[40]{};
            std::snprintf(chip, sizeof(chip), "版本落后 %d##f_stale", nStaleVer);
            FilterChipButton(st, chip, "__stale__", OpsTone::Warn());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("筛选相对最新 v%s 偏旧的客户端",
                                  st.latestClientVersionText.c_str());
            }
        }
        if (alertN > 0) {
            ImGui::SameLine(0, 14.f);
            ImGui::PushStyleColor(ImGuiCol_Text, OpsTone::Warn());
            char alertBtn[48]{};
            std::snprintf(alertBtn, sizeof(alertBtn), "同IP告警 %d##open_alerts", alertN);
            if (ImGui::SmallButton(alertBtn)) st.forceOpenIpAlerts = true;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击展开下方告警明细");
        }
        ImGui::SameLine(0, 14.f);
        ImGui::TextDisabled("归属 %s",
                            st.clientsGeoProvider.empty() ? "—" : st.clientsGeoProvider.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── 当前连接（主区域优先；次要块放表后） ──
    ImGui::Separator();
    ImGui::TextUnformatted("当前连接");
    ImGui::SameLine();
    ImGui::TextDisabled("近 %ds", st.clientsActiveSec);
    ImGui::SameLine(0, 10.f);
    ImGui::Checkbox("自动刷新", &st.clientsAutoRefresh);
    ImGui::SameLine();
    if (ImGui::SmallButton("刷新##clients")) {
        RefreshClients(st, true);
        RefreshBans(st, true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("重查归属##clients")) {
        RefreshClients(st, true, true);
        SetStatus(st, "已请求重新查询 IP 归属地（多源）");
    }
    ImGui::SetItemTooltip(
        "清空服务端归属缓存并用多源（ip-api + 太平洋 + ip9）重查当前在线 IP。\n"
        "免费库不可能 100%% 精确到区县；移动/CGNAT 常显示运营商出口城市。");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.f);
    if (ImGui::InputInt("##clients_sec", &st.clientsActiveSec)) {
        if (st.clientsActiveSec < 30) st.clientsActiveSec = 30;
        if (st.clientsActiveSec > 3600) st.clientsActiveSec = 3600;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("在线判定窗口（秒）");
    ImGui::SameLine(0, 10.f);
    ImGui::SetNextItemWidth(168.f);
    ImGui::InputTextWithHint("##clients_filter", "筛选 IP/机名/角色/MAC/TOKEN…", st.clientsFilter,
                             sizeof(st.clientsFilter));
    if (st.clientsFilter[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("清除##clients_filter")) st.clientsFilter[0] = '\0';
    }
    if (st.clientsGateFilter[0] != '\0') {
        ImGui::SameLine(0, 8.f);
        char gateChip[48]{};
        std::snprintf(gateChip, sizeof(gateChip), "%s ×##clr_gate",
                      GateFilterLabel(st.clientsGateFilter));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(OpsTone::Info().x, OpsTone::Info().y, OpsTone::Info().z, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.97f, 1.f, 1.f));
        if (ImGui::SmallButton(gateChip)) st.clientsGateFilter[0] = '\0';
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击取消门禁筛选（状态条 chip 也可切换）");
    }
    ImGui::SameLine(0, 8.f);
    ImGui::Checkbox("空闲优先", &st.clientsSortIdleFirst);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("空闲秒数小的排前（刚探活靠上）");
    ImGui::SameLine(0, 6.f);
    ImGui::Checkbox("同IP折叠", &st.clientsGroupByIp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("同公网 IP 多台设备合并为一组，默认折叠；点 ▶ 展开");
    if (st.clientsGroupByIp) {
        ImGui::SameLine(0, 4.f);
        if (ImGui::SmallButton("全展##ip_expand_all")) {
            for (const auto& c : st.clients) {
                if (!c.ip.empty()) st.clientsIpExpanded.insert(c.ip);
            }
        }
        ImGui::SameLine(0, 2.f);
        if (ImGui::SmallButton("全折##ip_collapse_all")) st.clientsIpExpanded.clear();
    }
    ImGui::SameLine(0, 6.f);
    if (ImGui::SmallButton("复制可见##clients")) {
        std::string out =
            "ip\tmachine\tmac\ttoken\tdeviceId\tver\tchar\tlevel\tjob\tmeso\tgate\tidle\n";
        int n = 0;
        for (const auto& c : st.clients) {
            if (!ClientMatchesFilter(st, c, st.clientsFilter)) continue;
            AppendClientSummaryLine(out, c);
            ++n;
        }
        CopyText(out.c_str());
        SetStatus(st, "已复制可见客户端 " + std::to_string(n) + " 行");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("复制当前筛选结果为 TSV");

    int filterMatch = 0;
    const bool anyFilter = st.clientsFilter[0] != '\0' || st.clientsGateFilter[0] != '\0';
    if (anyFilter) {
        for (const auto& c : st.clients) {
            if (ClientMatchesFilter(st, c, st.clientsFilter)) ++filterMatch;
        }
        ImGui::SameLine(0, 8.f);
        ImGui::TextDisabled("%d/%d", filterMatch, st.clientsCount);
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    // 工具条收紧后主表再抬一点。
    const float clientsH = (std::max)(260.f, ImGui::GetContentRegionAvail().y * 0.58f);
    if (ImGui::BeginTable("clients_table", 16, flags, ImVec2(0, clientsH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("归属地", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("角色", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("等级", ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("职业", ImGuiTableColumnFlags_WidthFixed, 88.f);
        ImGui::TableSetupColumn("背包金", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 88.f);
        ImGui::TableSetupColumn("设备", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableSetupColumn("最近活动", ImGuiTableColumnFlags_WidthFixed, 120.f);
        ImGui::TableSetupColumn("空闲", ImGuiTableColumnFlags_WidthFixed, 42.f);
        ImGui::TableSetupColumn("门禁", ImGuiTableColumnFlags_WidthFixed, 128.f);
        ImGui::TableSetupColumn("请求", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 118.f);
        ImGui::TableHeadersRow();

        if (st.clients.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", st.clientsError.empty() ? "(暂无在线客户端)" : "(无数据)");
        } else {
            std::vector<size_t> order;
            order.reserve(st.clients.size());
            for (size_t idx = 0; idx < st.clients.size(); ++idx) {
                if (ClientMatchesFilter(st, st.clients[idx], st.clientsFilter)) order.push_back(idx);
            }
            if (st.clientsGroupByIp) {
                // 同 IP 聚在一起；组内再按空闲
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    const auto& ca = st.clients[a];
                    const auto& cb = st.clients[b];
                    if (ca.ip != cb.ip) return ca.ip < cb.ip;
                    if (st.clientsSortIdleFirst && ca.idleSec != cb.idleSec)
                        return ca.idleSec < cb.idleSec;
                    return false;
                });
            } else if (st.clientsSortIdleFirst) {
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    const int ia = st.clients[a].idleSec;
                    const int ib = st.clients[b].idleSec;
                    if (ia != ib) return ia < ib;
                    return st.clients[a].ip < st.clients[b].ip;
                });
            }

            struct IpGroup {
                std::string ip;
                std::vector<size_t> members;
            };
            std::vector<IpGroup> groups;
            groups.reserve(order.size());
            if (st.clientsGroupByIp) {
                for (size_t idx : order) {
                    const std::string& ip = st.clients[idx].ip;
                    if (groups.empty() || groups.back().ip != ip) {
                        groups.push_back(IpGroup{ip, {idx}});
                    } else {
                        groups.back().members.push_back(idx);
                    }
                }
            } else {
                for (size_t idx : order) {
                    groups.push_back(IpGroup{st.clients[idx].ip, {idx}});
                }
            }

            int shown = 0;
            for (const auto& g : groups) {
                const bool multi = st.clientsGroupByIp && g.members.size() >= 2;
                const bool expanded =
                    !multi || st.clientsIpExpanded.find(g.ip) != st.clientsIpExpanded.end();

                if (multi) {
                    const auto& head = st.clients[g.members.front()];
                    ImGui::PushID(g.ip.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool open = expanded;
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.f, 0.f, 0.f, 0.f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                         ImVec4(OpsTone::Warn().x, OpsTone::Warn().y,
                                                OpsTone::Warn().z, 0.22f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                         ImVec4(OpsTone::Warn().x, OpsTone::Warn().y,
                                                OpsTone::Warn().z, 0.35f));
                    char grpLabel[128]{};
                    std::snprintf(grpLabel, sizeof(grpLabel), "%s  %s  ·%zu台", open ? "▼" : "▶",
                                  g.ip.c_str(), g.members.size());
                    if (ImGui::Selectable(grpLabel, false)) {
                        if (open)
                            st.clientsIpExpanded.erase(g.ip);
                        else
                            st.clientsIpExpanded.insert(g.ip);
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "同公网 IP 当前筛选 %zu 台（点行折叠/展开）\n归属：%s",
                            g.members.size(), head.geo.empty() ? "—" : head.geo.c_str());
                    }

                    ImGui::TableSetColumnIndex(1);
                    if (head.geo.empty()) ImGui::TextDisabled("—");
                    else ImGui::TextUnformatted(head.geo.c_str());

                    ImGui::TableSetColumnIndex(2);
                    {
                        std::string summary;
                        int nShow = 0;
                        for (size_t mi : g.members) {
                            const auto& m = st.clients[mi];
                            const char* tag =
                                !m.machine.empty() ? m.machine.c_str()
                                : (!m.charName.empty() ? m.charName.c_str() : m.device.c_str());
                            if (!tag || !tag[0]) tag = "?";
                            if (nShow > 0) summary += " / ";
                            summary += tag;
                            if (++nShow >= 4) {
                                summary += " …";
                                break;
                            }
                        }
                        ImGui::TextColored(OpsTone::Warn(), "%s", summary.c_str());
                    }
                    ImGui::TableSetColumnIndex(3);
                    {
                        std::string chars;
                        int nShow = 0;
                        for (size_t mi : g.members) {
                            const auto& m = st.clients[mi];
                            if (m.charName.empty()) continue;
                            if (nShow > 0) chars += " / ";
                            chars += m.charName;
                            if (++nShow >= 4) {
                                chars += " …";
                                break;
                            }
                        }
                        if (chars.empty()) ImGui::TextDisabled("—");
                        else ImGui::TextUnformatted(chars.c_str());
                    }
                    for (int col = 4; col <= 15; ++col) {
                        ImGui::TableSetColumnIndex(col);
                        if (col == 12) {
                            int bestIdle = head.idleSec;
                            for (size_t mi : g.members)
                                bestIdle = (std::min)(bestIdle, st.clients[mi].idleSec);
                            char idleBuf[16]{};
                            FormatIdleSec(bestIdle, idleBuf, sizeof(idleBuf));
                            ImGui::TextColored(IdleSecColor(bestIdle), "%s", idleBuf);
                        } else if (col == 14) {
                            int hits = 0;
                            for (size_t mi : g.members) hits += st.clients[mi].hits;
                            ImGui::TextDisabled("%d", hits);
                        } else {
                            ImGui::TextDisabled(open ? "" : "…");
                        }
                    }
                    ImGui::PopID();
                    ++shown;
                    if (!expanded) continue;
                }

                for (size_t memberPos = 0; memberPos < g.members.size(); ++memberPos) {
                const size_t idx = g.members[memberPos];
                const auto& c = st.clients[idx];
                const bool childRow = multi;
                ++shown;
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char ipLabel[96]{};
                if (childRow) {
                    std::snprintf(ipLabel, sizeof(ipLabel), "  └");
                } else if (c.sameIpOnline > 1 || c.knownOnIp > 1) {
                    std::snprintf(ipLabel, sizeof(ipLabel), "%s ·×%d", c.ip.c_str(),
                                  (std::max)(c.sameIpOnline, c.knownOnIp));
                } else {
                    std::snprintf(ipLabel, sizeof(ipLabel), "%s", c.ip.c_str());
                }
                if (c.banned) ImGui::TextColored(OpsTone::Danger(), "%s", ipLabel);
                else if (!childRow && (c.knownOnIp > 1 || c.sameIpOnline > 1))
                    ImGui::TextColored(OpsTone::Warn(), "%s", ipLabel);
                else if (allowMode && !c.allowed)
                    ImGui::TextColored(OpsTone::Warn(), "%s", ipLabel);
                else if (c.allowed)
                    ImGui::TextColored(OpsTone::Ok(), "%s", ipLabel);
                else if (childRow)
                    ImGui::TextDisabled("%s", ipLabel);
                else
                    ImGui::Selectable(ipLabel, false);
                const bool ipHovered = ImGui::IsItemHovered();
                if (ipHovered) {
                    if (c.banned) ImGui::SetTooltip("此设备已在封禁清单");
                    else if (c.knownOnIp > 1)
                        ImGui::SetTooltip(
                            "同公网 IP 历史上见过 %d 台设备（deviceId/MAC）\n归属：%s", c.knownOnIp,
                            c.geo.empty() ? "—" : c.geo.c_str());
                    else if (c.sameIpOnline > 1)
                        ImGui::SetTooltip("同公网 IP 当前在线 %d 台（NAT/公司宽带）\n归属：%s",
                                          c.sameIpOnline, c.geo.empty() ? "—" : c.geo.c_str());
                    else if (allowMode && !c.allowed)
                        ImGui::SetTooltip("仅白名单模式下：此设备不在白名单，约 60s 内会退出");
                    else if (c.allowed)
                        ImGui::SetTooltip("已在白名单");
                    else if (!c.lastDenyAt.empty())
                        ImGui::SetTooltip("最近拒绝 %s · %s", c.lastDenyAt.c_str(),
                                          c.lastDenyReason.empty() ? c.lastDenyMatch.c_str()
                                                                   : c.lastDenyReason.c_str());
                    else if (!c.identified && c.knownOnIp > 1) {
                        ImGui::SetTooltip("旧客户端未报身份；该 IP 历史上传见过 %d 台设备",
                                          c.knownOnIp);
                    } else {
                        ImGui::SetTooltip("双击复制一行摘要；右键更多操作");
                    }
                }
                if (ImGui::BeginPopupContextItem("ip")) {
                    if (ImGui::MenuItem("复制 IP")) CopyText(c.ip.c_str());
                    if (c.identified && ImGui::MenuItem("复制计算机名")) CopyText(c.machine.c_str());
                    if (!c.charName.empty() && ImGui::MenuItem("复制角色名")) CopyText(c.charName.c_str());
                    if (c.identified && !c.deviceId.empty() && ImGui::MenuItem("复制 deviceId")) {
                        CopyText(c.deviceId.c_str());
                    }
                    if (c.identified && !c.mac.empty() && ImGui::MenuItem("复制 MAC")) {
                        CopyText(c.mac.c_str());
                    }
                    if (c.identified && !c.token.empty() && ImGui::MenuItem("复制 TOKEN")) {
                        CopyText(c.token.c_str());
                    }
                    if (ImGui::MenuItem("复制一行摘要")) {
                        CopyClientSummary(c);
                        SetStatus(st, "已复制客户端摘要到剪贴板");
                    }
                    if (c.identified && (!c.deviceId.empty() || !c.mac.empty())) {
                        ImGui::Separator();
                        if (ImGui::MenuItem("填入封禁表单")) {
                            FillBanFormFromClient(st, c);
                            SetStatus(st, "已填入封禁表单（未带 TOKEN）");
                        }
                        if (ImGui::MenuItem("填入白名单表单")) {
                            FillAllowFormFromClient(st, c);
                            SetStatus(st, "已填入白名单表单（未带 TOKEN）");
                        }
                    }
                    ImGui::EndPopup();
                }
                if (ipHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    CopyClientSummary(c);
                    SetStatus(st, "已复制客户端摘要（双击）");
                }
                ImGui::TableSetColumnIndex(1);
                if (c.geo.empty()) {
                    if (c.geoStatus == "pending") ImGui::TextDisabled("查询中…");
                    else ImGui::TextDisabled("—");
                } else {
                    ImGui::TextUnformatted(c.geo.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                if (!c.identified) {
                    if (c.knownOnIp > 1) {
                        ImGui::TextDisabled("未识别（历史%d台）", c.knownOnIp);
                    } else {
                        ImGui::TextDisabled("未识别");
                    }
                } else {
                    ImGui::TextUnformatted(c.machine.empty() ? "—" : c.machine.c_str());
                }
                ImGui::TableSetColumnIndex(3);
                if (c.charName.empty()) {
                    ImGui::TextDisabled("—");
                } else {
                    ImGui::TextUnformatted(c.charName.c_str());
                    if (ImGui::IsItemHovered()) {
                        char mesoTip[48]{};
                        FormatMesoDisplay(c.charMeso, mesoTip, sizeof(mesoTip));
                        ImGui::SetTooltip("角色 %s\n等级 %d · 职业 %s (id=%d)\n背包金 %s（精确 %s）",
                                          c.charName.c_str(), c.charLevel,
                                          c.charJobName.empty() ? "?" : c.charJobName.c_str(),
                                          c.charJob, mesoTip,
                                          c.charMeso.empty() ? "—" : c.charMeso.c_str());
                    }
                }
                ImGui::TableSetColumnIndex(4);
                if (c.charLevel <= 0) ImGui::TextDisabled("—");
                else ImGui::Text("%d", c.charLevel);
                ImGui::TableSetColumnIndex(5);
                if (c.charJobName.empty()) {
                    if (c.charJob != 0) ImGui::TextDisabled("%d", c.charJob);
                    else ImGui::TextDisabled("—");
                } else {
                    ImGui::TextUnformatted(c.charJobName.c_str());
                }
                ImGui::TableSetColumnIndex(6);
                {
                    char mesoBuf[48]{};
                    FormatMesoDisplay(c.charMeso, mesoBuf, sizeof(mesoBuf));
                    if (c.charMeso.empty()) ImGui::TextDisabled("—");
                    else ImGui::TextUnformatted(mesoBuf);
                }
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted((!c.identified || c.mac.empty()) ? "—" : c.mac.c_str());
                ImGui::TableSetColumnIndex(8);
                if (!c.identified || c.token.empty()) {
                    ImGui::TextDisabled("—");
                } else {
                    ImGui::TextColored(OpsTone::Token(), "%s", c.token.c_str());
                }
                ImGui::TableSetColumnIndex(9);
                ImGui::TextUnformatted((!c.identified || c.device.empty()) ? "—" : c.device.c_str());
                ImGui::TableSetColumnIndex(10);
                if (c.appVersion.empty()) {
                    ImGui::TextDisabled("—");
                } else {
                    ImGui::TextColored(AppVersionColor(st, c.appVersion), "%s", c.appVersion.c_str());
                    if (ImGui::IsItemHovered() && !st.latestClientVersionText.empty() &&
                        c.appVersion != st.latestClientVersionText) {
                        ImGui::SetTooltip("最新发布 v%s · 此客户端偏旧",
                                          st.latestClientVersionText.c_str());
                    }
                }
                ImGui::TableSetColumnIndex(11);
                if (!c.lastSeenAt.empty()) {
                    ImGui::Text("%s", c.lastSeenAt.c_str());
                    ImGui::SameLine(0, 6.f);
                    ImGui::TextDisabled("%s", c.lastKind.c_str());
                } else {
                    ImGui::TextDisabled("—");
                }
                ImGui::TableSetColumnIndex(12);
                char idleBuf[16]{};
                FormatIdleSec(c.idleSec, idleBuf, sizeof(idleBuf));
                ImGui::TextColored(IdleSecColor(c.idleSec), "%s", idleBuf);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("空闲 %d 秒", c.idleSec);
                ImGui::TableSetColumnIndex(13);
                {
                    auto formatRemain = [](int sec, char* out, size_t outN) {
                        if (sec <= 0) {
                            out[0] = '\0';
                            return;
                        }
                        const int h = sec / 3600;
                        const int m = (sec % 3600) / 60;
                        if (h >= 48)
                            std::snprintf(out, outN, "剩%dh", h);
                        else if (h > 0)
                            std::snprintf(out, outN, "剩%dh%02dm", h, m);
                        else
                            std::snprintf(out, outN, "剩%dm", m);
                    };
                    char remainBuf[24]{};
                    formatRemain(c.leaseRemainSec, remainBuf, sizeof(remainBuf));

                    ImVec4 col = OpsTone::Muted();
                    char labelBuf[64]{};
                    const char* label = "—";
                    if (c.gate == "probe_ok") {
                        if (remainBuf[0])
                            std::snprintf(labelBuf, sizeof(labelBuf), "探活OK · %s", remainBuf);
                        else
                            std::snprintf(labelBuf, sizeof(labelBuf), "探活OK");
                        label = labelBuf;
                        col = OpsTone::Ok();
                    } else if (c.gate == "lease") {
                        if (remainBuf[0])
                            std::snprintf(labelBuf, sizeof(labelBuf), "租约 · %s", remainBuf);
                        else
                            std::snprintf(labelBuf, sizeof(labelBuf), "租约中");
                        label = labelBuf;
                        col = OpsTone::Info();
                    } else if (c.gate == "policy_deny") {
                        label = "策略将拒";
                        col = OpsTone::Warn();
                    } else if (c.gate == "denied") {
                        label = "已拒绝";
                        col = OpsTone::Danger();
                    } else if (c.gate == "lease_expired") {
                        label = "租约估过期";
                        col = OpsTone::Warn();
                    } else if (c.gate == "no_allow") {
                        label = "未见放行";
                        col = ImVec4(0.78f, 0.75f, 0.45f, 1.f);
                    } else if (!c.gate.empty()) {
                        if (remainBuf[0])
                            std::snprintf(labelBuf, sizeof(labelBuf), "%s · %s", c.gate.c_str(),
                                          remainBuf);
                        else
                            std::snprintf(labelBuf, sizeof(labelBuf), "%s", c.gate.c_str());
                        label = labelBuf;
                    }
                    ImGui::TextColored(col, "%s", label);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "gate=%s\n最近放行 %s\n租约剩余估 %ds（TTL %dh）\n"
                            "服务端按 lastAllow+64h 估算，非客户端本地文件",
                            c.gate.empty() ? "?" : c.gate.c_str(),
                            c.lastAllowAt.empty() ? "—" : c.lastAllowAt.c_str(), c.leaseRemainSec,
                            c.leaseTtlHours > 0 ? c.leaseTtlHours : 64);
                    }
                }
                ImGui::TableSetColumnIndex(14);
                ImGui::Text("%d", c.hits);
                ImGui::TableSetColumnIndex(15);
                if (c.identified && (!c.deviceId.empty() || !c.mac.empty())) {
                    if (c.banned) {
                        if (NeutralSmallButton("解禁")) {
                            std::string err;
                            if (PostBanAction(st, "unban", c.machine, c.deviceId, {}, {}, err,
                                              c.mac)) {
                                SetStatus(st, "已解禁 " + (c.device.empty() ? c.machine : c.device));
                                RefreshBans(st, true);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                    } else if (DangerSmallButton("禁止")) {
                        std::string err;
                        if (PostBanAction(st, "ban", c.machine, c.deviceId, "ops ban", {}, err,
                                          c.mac)) {
                            SetStatus(st, "已禁止 " + (c.device.empty() ? c.machine : c.device));
                            RefreshBans(st, true);
                            RefreshClients(st, true);
                        } else {
                            SetStatus(st, err);
                        }
                    }
                    ImGui::SameLine();
                    if (c.allowed) {
                        if (NeutralSmallButton("移出")) {
                            std::string err;
                            if (PostBanAction(st, "unallow", c.machine, c.deviceId, {}, {}, err,
                                              c.mac)) {
                                SetStatus(st, "已移出白名单");
                                RefreshBans(st, true);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("移出白名单");
                    } else if (SafeSmallButton("加白")) {
                        std::string err;
                        if (PostBanAction(st, "allow", c.machine, c.deviceId, "ops allow", {}, err,
                                          c.mac)) {
                            SetStatus(st, "已加入白名单");
                            RefreshBans(st, true);
                            RefreshClients(st, true);
                        } else {
                            SetStatus(st, err);
                        }
                    }
                } else if (c.identified) {
                    ImGui::TextDisabled("用手动表单");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "无 deviceId/MAC。\n"
                            "共享 TOKEN 请用下方手动封禁/加白（勿从在线表带 TOKEN，防误伤全队）。");
                    }
                } else {
                    ImGui::TextDisabled("—");
                }
                ImGui::PopID();
                }  // members
            }  // groups
            if (shown == 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("(无匹配筛选)");
            }
        }
        ImGui::EndTable();
    }

    // ── 下半：次要折叠 + 封禁|白名单并排 ──
    const float belowH = (std::max)(160.f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("access_below", ImVec2(0, belowH), false);

    {
        const bool hasDenies = !st.recentDenies.empty();
        char denyHdr[64]{};
        if (hasDenies)
            std::snprintf(denyHdr, sizeof(denyHdr), "最近拒绝 (%zu)##denies",
                          st.recentDenies.size());
        else
            std::snprintf(denyHdr, sizeof(denyHdr), "最近拒绝##denies");
        ImGui::SetNextItemOpen(hasDenies, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(denyHdr)) {
            ImGui::TextDisabled("探活/上报被拦（关服前看是否命中）");
            if (!hasDenies) {
                ImGui::TextDisabled("(暂无)");
            } else {
                const float denyH = (std::min)(96.f, belowH * 0.22f);
                if (ImGui::BeginTable("recent_denies", 7,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_SizingStretchProp,
                                      ImVec2(0, denyH))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 130.f);
                    ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 110.f);
                    ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 100.f);
                    ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 88.f);
                    ImGui::TableSetupColumn("匹配", ImGuiTableColumnFlags_WidthFixed, 80.f);
                    ImGui::TableSetupColumn("原因", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < st.recentDenies.size(); ++i) {
                        const auto& d = st.recentDenies[i];
                        ImGui::PushID(static_cast<int>(i) + 30000);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(d.at.empty() ? "—" : d.at.c_str());
                        if (ImGui::BeginPopupContextItem("deny_row")) {
                            if (ImGui::MenuItem("填入封禁表单")) {
                                std::snprintf(st.banMachineInput, sizeof(st.banMachineInput), "%s",
                                              d.machine.c_str());
                                std::snprintf(st.banDeviceIdInput, sizeof(st.banDeviceIdInput), "%s",
                                              d.deviceId.c_str());
                                std::snprintf(st.banMacInput, sizeof(st.banMacInput), "%s",
                                              d.mac.c_str());
                                st.banPassInput[0] = '\0';
                                std::snprintf(st.banReasonInput, sizeof(st.banReasonInput), "%s",
                                              d.reason.empty() ? "from deny" : d.reason.c_str());
                                SetStatus(st, "已从拒绝记录填入封禁表单（未带 TOKEN）");
                            }
                            if (!d.ip.empty() && ImGui::MenuItem("复制 IP")) CopyText(d.ip.c_str());
                            if (!d.mac.empty() && ImGui::MenuItem("复制 MAC")) CopyText(d.mac.c_str());
                            ImGui::EndPopup();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("右键：填入封禁 / 复制");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(d.ip.empty() ? "—" : d.ip.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(d.machine.empty() ? "—" : d.machine.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(d.mac.empty() ? "—" : d.mac.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(d.token.empty() ? "—" : d.token.c_str());
                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextUnformatted(d.match.empty() ? "—" : d.match.c_str());
                        ImGui::TableSetColumnIndex(6);
                        ImGui::TextUnformatted(d.reason.empty() ? "—" : d.reason.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    if (alertN > 0 || !st.ipAlerts.empty()) {
        char alertHdr[64]{};
        std::snprintf(alertHdr, sizeof(alertHdr), "同 IP 多设备告警 (%d)##ipalerts_v2", alertN);
        // 状态条已有计数：默认折叠；点状态条「同IP告警」可强制展开。
        if (st.forceOpenIpAlerts) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            st.forceOpenIpAlerts = false;
        } else {
            ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        }
        if (ImGui::CollapsingHeader(alertHdr)) {
            ImGui::TextDisabled("历史见过的 deviceId/MAC 数≥2，可能是 NAT 或多机泄露");
            const float alertH = (std::min)(72.f, belowH * 0.18f);
            if (ImGui::BeginTable("ip_alerts", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, alertH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 120.f);
                ImGui::TableSetupColumn("归属地", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("设备数", ImGuiTableColumnFlags_WidthFixed, 56.f);
                ImGui::TableSetupColumn("摘要", ImGuiTableColumnFlags_WidthStretch, 2.2f);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < st.ipAlerts.size(); ++i) {
                    const auto& a = st.ipAlerts[i];
                    ImGui::PushID(static_cast<int>(i) + 40000);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(OpsTone::Warn(), "%s", a.ip.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(a.geo.empty() ? "—" : a.geo.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", a.deviceCount);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(a.summary.empty() ? "—" : a.summary.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    if (ImGui::BeginTable("ban_allow_split", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoPadOuterX,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        // ── 左：封禁 ──
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("封禁清单");
        ImGui::SameLine();
        ImGui::TextDisabled("优先拦截");
        if (!st.bansError.empty()) {
            ImGui::TextColored(OpsTone::DangerSoft(), "%s", st.bansError.c_str());
        }
        if (BeginAccessFormGrid("ban_form", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAccessFormField("计算机名", "##ban_machine", st.banMachineInput,
                                sizeof(st.banMachineInput), "可选备注");
            ImGui::TableSetColumnIndex(1);
            DrawAccessFormField("deviceId", "##ban_device", st.banDeviceIdInput,
                                sizeof(st.banDeviceIdInput), "UUID");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAccessFormField("MAC", "##ban_mac", st.banMacInput, sizeof(st.banMacInput),
                                "aa:bb:cc:…");
            ImGui::TableSetColumnIndex(1);
            DrawAccessFormField("TOKEN", "##ban_pass", st.banPassInput, sizeof(st.banPassInput),
                                "共享通行");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAccessFormField("原因", "##ban_reason", st.banReasonInput,
                                sizeof(st.banReasonInput), "可选");
            ImGui::TableSetColumnIndex(1);
            if (DangerButton("手动封禁##ban", ImVec2(-FLT_MIN, 0))) {
                std::string err;
                if (st.banDeviceIdInput[0] == '\0' && st.banMacInput[0] == '\0' &&
                    st.banPassInput[0] == '\0') {
                    SetStatus(st, "须填 deviceId / MAC / TOKEN（禁止仅计算机名）");
                } else if (PostBanAction(st, "ban", st.banMachineInput, st.banDeviceIdInput,
                                         st.banReasonInput, {}, err, st.banMacInput, {},
                                         st.banPassInput)) {
                    SetStatus(st, "已写入封禁");
                    st.banMachineInput[0] = '\0';
                    st.banDeviceIdInput[0] = '\0';
                    st.banMacInput[0] = '\0';
                    st.banPassInput[0] = '\0';
                    st.banReasonInput[0] = '\0';
                    RefreshBans(st, true);
                    RefreshClients(st, true);
                } else {
                    SetStatus(st, err);
                }
            }
            ImGui::SetItemTooltip(
                "必须填 deviceId / MAC / TOKEN。\n"
                "在线表「禁止」只封 MAC/deviceId，不带 TOKEN。");
            ImGui::EndTable();
        }
        {
            const float listH = (std::max)(80.f, ImGui::GetContentRegionAvail().y);
            if (ImGui::BeginTable("bans_table", 6, flags, ImVec2(0, listH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 96.f);
                ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 72.f);
                ImGui::TableSetupColumn("原因", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 52.f);
                ImGui::TableHeadersRow();
                if (st.bans.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("(无封禁 · 可从在线表/拒绝记录右键填入)");
                } else {
                    for (size_t idx = 0; idx < st.bans.size(); ++idx) {
                        const auto& b = st.bans[idx];
                        ImGui::PushID(static_cast<int>(idx) + 10000);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(b.key.empty() ? "—" : b.key.c_str());
                        if (ImGui::IsItemHovered() && !b.deviceId.empty()) {
                            ImGui::SetTooltip("deviceId: %s\n%s", b.deviceId.c_str(),
                                              b.bannedAt.empty() ? "" : b.bannedAt.c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(b.machine.empty() ? "—" : b.machine.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(b.mac.empty() ? "—" : b.mac.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(b.token.empty() ? "—" : b.token.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(b.reason.empty() ? "—" : b.reason.c_str());
                        ImGui::TableSetColumnIndex(5);
                        if (NeutralSmallButton("解禁")) {
                            std::string err;
                            if (PostBanAction(st, "unban", b.machine, b.deviceId, {}, b.key, err,
                                              b.mac, {}, b.token)) {
                                SetStatus(st, "已解禁 " + (b.key.empty() ? b.device : b.key));
                                RefreshBans(st, true);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }

        // ── 右：白名单 ──
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("白名单");
        ImGui::SameLine();
        ImGui::TextDisabled("仅白名单模式生效");
        if (BeginAccessFormGrid("allow_form", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAccessFormField("计算机名", "##allow_machine", st.allowMachineInput,
                                sizeof(st.allowMachineInput), "可选备注");
            ImGui::TableSetColumnIndex(1);
            DrawAccessFormField("deviceId", "##allow_device", st.allowDeviceIdInput,
                                sizeof(st.allowDeviceIdInput), "UUID");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAccessFormField("MAC", "##allow_mac", st.allowMacInput, sizeof(st.allowMacInput),
                                "aa:bb:cc:…");
            ImGui::TableSetColumnIndex(1);
            DrawAccessFormField("TOKEN", "##allow_pass", st.allowPassInput,
                                sizeof(st.allowPassInput), "共享通行");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawAccessFormField("备注", "##allow_reason", st.allowReasonInput,
                                sizeof(st.allowReasonInput), "可选");
            ImGui::TableSetColumnIndex(1);
            if (SafeButton("加入白名单##allow", ImVec2(-FLT_MIN, 0))) {
                std::string err;
                if (st.allowDeviceIdInput[0] == '\0' && st.allowMacInput[0] == '\0' &&
                    st.allowPassInput[0] == '\0') {
                    SetStatus(st, "须填 deviceId / MAC / TOKEN（禁止仅计算机名）");
                } else if (PostBanAction(st, "allow", st.allowMachineInput, st.allowDeviceIdInput,
                                         st.allowReasonInput, {}, err, st.allowMacInput, {},
                                         st.allowPassInput)) {
                    SetStatus(st, "已加入白名单");
                    st.allowMachineInput[0] = '\0';
                    st.allowDeviceIdInput[0] = '\0';
                    st.allowMacInput[0] = '\0';
                    st.allowPassInput[0] = '\0';
                    st.allowReasonInput[0] = '\0';
                    RefreshBans(st, true);
                    RefreshClients(st, true);
                } else {
                    SetStatus(st, err);
                }
            }
            ImGui::SetItemTooltip(
                "设备加白用 MAC/deviceId；共享 TOKEN 在此单独填（在线表「加白」不带 TOKEN）。");
            ImGui::EndTable();
        }
        {
            const float listH = (std::max)(80.f, ImGui::GetContentRegionAvail().y);
            if (ImGui::BeginTable("allows_table", 6, flags, ImVec2(0, listH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 96.f);
                ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 72.f);
                ImGui::TableSetupColumn("备注", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 52.f);
                ImGui::TableHeadersRow();
                if (st.allows.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("(无白名单 · 可从在线表右键填入)");
                } else {
                    for (size_t idx = 0; idx < st.allows.size(); ++idx) {
                        const auto& a = st.allows[idx];
                        ImGui::PushID(static_cast<int>(idx) + 20000);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(a.key.empty() ? "—" : a.key.c_str());
                        if (ImGui::IsItemHovered() && !a.deviceId.empty()) {
                            ImGui::SetTooltip("deviceId: %s\n%s", a.deviceId.c_str(),
                                              a.bannedAt.empty() ? "" : a.bannedAt.c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(a.machine.empty() ? "—" : a.machine.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(a.mac.empty() ? "—" : a.mac.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(a.token.empty() ? "—" : a.token.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(a.reason.empty() ? "—" : a.reason.c_str());
                        ImGui::TableSetColumnIndex(5);
                        if (NeutralSmallButton("移出")) {
                            std::string err;
                            if (PostBanAction(st, "unallow", a.machine, a.deviceId, {}, a.key, err,
                                              a.mac, {}, a.token)) {
                                SetStatus(st, "已移出白名单");
                                RefreshBans(st, true);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
}

void OpsPanel_Draw(OpsState& st) {
    if (st.mainTab == 0) RefreshLogViewer(st, false);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("XCat TWMS Ops", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Header（压成两行，把高度留给内容页） ──
    ImGui::TextUnformatted("XCat TWMS 运维控制台");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("台服经典版 · bin_ops\n关窗即停 API(:18789) 与发布站(:52080)；勿与枫星 ops 混用");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("经典版");
    {
        static const char* kThemeLabels[] = {"暗夜", "白天"};
        int idx = (xcat::ui::UiTheme_Preference() == xcat::ui::ThemePreference::Light) ? 1 : 0;
        ImGui::SameLine(0, 12.f);
        ImGui::SetNextItemWidth(72.f);
        if (ImGui::Combo("##ops_theme", &idx, kThemeLabels, IM_ARRAYSIZE(kThemeLabels))) {
            const auto want = (idx == 1) ? xcat::ui::ThemePreference::Light
                                         : xcat::ui::ThemePreference::Dark;
            if (xcat::ui::UiTheme_SetPreferencePersisted(OpsThemeBinDir(), want)) {
                OpsTheme_RequestCommit();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("与启动器共用 user.ini [ui]（若找到 bin\\XCat_data）");
        }
    }
    if (!st.diskFreeText.empty()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(st.diskFreeText.c_str()).x - 24.f);
        ImGui::TextDisabled("%s", st.diskFreeText.c_str());
    }

    // ── Global toolbar + 迷你健康 + 状态 ──
    const bool busy = st.busy.load();
    const bool twmsOkHdr = TwmsRunning(st) && st.twmsHealthOk && st.twmsReadyOk;
    const bool publishOkHdr = PublishRunning(st) && st.publishProbeOk;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("全部启动")) Ops_RequestStartAll(st);
    ImGui::SameLine();
    if (ImGui::Button("全部停止")) ImGui::OpenPopup("confirm_stop_all");
    ImGui::SameLine();
    ImGui::Checkbox("崩溃自动拉起", &st.autoRestart);
    if (busy) ImGui::EndDisabled();

    ImGui::SameLine(0, 12.f);
    StatusLed(twmsOkHdr, "API");
    ImGui::SameLine(0, 8.f);
    StatusLed(publishOkHdr, "发布");
    ImGui::SameLine(0, 12.f);
    {
        char onlineBtn[32]{};
        std::snprintf(onlineBtn, sizeof(onlineBtn), "在线 %d##goto_access", st.clientsCount);
        if (ImGui::SmallButton(onlineBtn)) {
            st.mainTab = 1;
            RefreshClients(st, true);
            RefreshBans(st, true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("打开「连接与访问」");
    }
    if (st.forcedClientBuildId > 0) {
        ImGui::SameLine(0, 10.f);
        ImGui::TextColored(OpsTone::Warn(), "强制#%u", st.forcedClientBuildId);
        ImGui::SameLine(0, 4.f);
        if (NeutralSmallButton("取消强制##hdr")) Ops_RequestClearForceClientUpdate(st);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("删除 force-update.json");
    }

    if (ImGui::BeginPopupModal("confirm_stop_all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("确认停止全部服务？");
        ImGui::TextDisabled("API 与发布站都会停；在线客户端探活会失败。");
        ImGui::Spacing();
        if (DangerButton("确认停止##stop_all_yes", ImVec2(120, 0))) {
            Ops_RequestStopAll(st);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##stop_all_no", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    {
        const std::string status = GetStatus(st);
        ImGui::SameLine(0, 14.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, xcat::ui::UiTheme_Palette().statusStripBg);
        ImGui::BeginChild("status_bar", ImVec2(-FLT_MIN, ImGui::GetFrameHeight()), true,
                          ImGuiWindowFlags_NoScrollbar);
        if (busy) {
            ImGui::TextColored(OpsTone::Busy(), "忙碌…");
            ImGui::SameLine(0, 8.f);
        }
        ImGui::TextColored(StatusMessageColor(status), "%s", status.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    DrawMainTabButtons(st);
    // F5：按当前页刷新
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (st.mainTab == 1) {
            RefreshClients(st, true);
            RefreshBans(st, true);
            SetStatus(st, "已刷新连接列表");
        } else {
            RefreshLogViewer(st, true);
            RefreshReleaseInfo(st);
            SetStatus(st, "已刷新日志与发布信息");
        }
    }
    ImGui::Separator();

    if (st.mainTab == 1) {
        DrawClientsPanel(st);
        ImGui::End();
        return;
    }

    // ── Feature area: two columns ──
    if (busy) ImGui::BeginDisabled();

    const bool twmsOk = twmsOkHdr;
    const bool publishOk = publishOkHdr;

    // 健康条：版本/强制细节（LED 已上移到顶栏）
    ImGui::PushStyleColor(ImGuiCol_ChildBg, xcat::ui::UiTheme_Palette().statusStripBg);
    ImGui::BeginChild("svc_health", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 8.f), true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled(TwmsRunning(st) ? "API 运行" : "API 停止");
    if (TwmsRunning(st)) {
        ImGui::SameLine(0, 6.f);
        ImGui::TextDisabled("PID %lu", static_cast<unsigned long>(TwmsPid(st)));
    }
    ImGui::SameLine(0, 16.f);
    ImGui::TextDisabled(PublishRunning(st) ? "发布站运行" : "发布站停止");
    if (PublishRunning(st)) {
        ImGui::SameLine(0, 6.f);
        ImGui::TextDisabled("PID %lu", static_cast<unsigned long>(PublishPid(st)));
    }
    if (st.latestClientBuildId > 0) {
        ImGui::SameLine(0, 16.f);
        ImGui::TextDisabled("最新 v%s #%u", st.latestClientVersionText.c_str(),
                            st.latestClientBuildId);
    }
    if (st.forcedClientBuildId > 0) {
        ImGui::SameLine(0, 12.f);
        ImGui::TextColored(OpsTone::Warn(), "强制 v%s #%u",
                           st.forcedClientVersionText.c_str(), st.forcedClientBuildId);
        ImGui::SameLine(0, 6.f);
        if (NeutralSmallButton("取消##force_svc")) Ops_RequestClearForceClientUpdate(st);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // 压缩服务卡片，把高度留给日志
    const float svcCardH = ImGui::GetTextLineHeightWithSpacing() * 7.6f;

    if (ImGui::BeginTable("svc_table", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoPadOuterX,
                          ImVec2(0, 0))) {
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, xcat::ui::UiTheme_Palette().statusStripBg);
        ImGui::BeginChild("twms_card", ImVec2(0, svcCardH), true);
        {
            StatusLed(twmsOk, "TWMS API  :18789");
            if (TwmsRunning(st)) {
                ImGui::SameLine(0, 12.f);
                ImGui::TextDisabled("PID %lu", static_cast<unsigned long>(TwmsPid(st)));
            } else {
                ImGui::SameLine(0, 12.f);
                ImGui::TextDisabled("已停止");
            }
            ImGui::TextDisabled("v%s · %s · %s%s",
                                st.serverVersionText.empty() ? "-" : st.serverVersionText.c_str(),
                                st.twmsUptimeText.c_str(), st.twmsHealthText.c_str(),
                                st.twmsReadyOk ? " · ready" : (TwmsRunning(st) ? " · not ready" : ""));
            if (!st.twmsStatsText.empty() || !st.twmsRequestText.empty()) {
                ImGui::TextDisabled("%s%s%s",
                                    st.twmsStatsText.c_str(),
                                    (!st.twmsStatsText.empty() && !st.twmsRequestText.empty()) ? " · "
                                                                                              : "",
                                    st.twmsRequestText.c_str());
            }
            if (st.twmsRestartCount > 0) {
                ImGui::SameLine(0, 10.f);
                ImGui::TextDisabled("拉起 %d", st.twmsRestartCount);
            }
            if (!st.hasNode) ImGui::TextColored(OpsTone::Danger(), "缺少 node.exe");

            ImGui::Spacing();
            if (ImGui::SmallButton("启动##a")) Ops_RequestStartTwms(st);
            ImGui::SameLine();
            if (ImGui::SmallButton("停止##a")) Ops_RequestStopTwms(st);
            ImGui::SameLine();
            if (ImGui::SmallButton("重启##a")) Ops_RequestRestartTwms(st);
            ImGui::SameLine(0, 12.f);
            if (ImGui::SmallButton("上传目录##a")) OpenFolder(JoinPath(st.repoRoot, L"user_log_uploads"));
            ImGui::SameLine();
            if (ImGui::SmallButton("日志索引##a")) {
                const std::wstring catalog = JoinPath(st.repoRoot, L"user_log_uploads\\catalog.jsonl");
                const std::wstring devices = JoinPath(st.repoRoot, L"user_log_uploads\\devices");
                if (GetFileAttributesW(catalog.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    OpenFolder(catalog);
                } else if (GetFileAttributesW(devices.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    OpenFolder(devices);
                } else {
                    OpenFolder(JoinPath(st.repoRoot, L"user_log_uploads"));
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("复制 Health##a")) CopyText("http://xcat.work:18789/twms/health");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, xcat::ui::UiTheme_Palette().statusStripBg);
        ImGui::BeginChild("publish_card", ImVec2(0, svcCardH), true);
        {
            StatusLed(publishOk, "发布站  :52080");
            if (PublishRunning(st)) {
                ImGui::SameLine(0, 12.f);
                ImGui::TextDisabled("PID %lu", static_cast<unsigned long>(PublishPid(st)));
            } else {
                ImGui::SameLine(0, 12.f);
                ImGui::TextDisabled("已停止");
            }
            ImGui::TextDisabled("%s", st.publishProbeText.c_str());
            if (st.latestClientBuildId > 0) {
                ImGui::TextDisabled("最新 v%s #%u", st.latestClientVersionText.c_str(),
                                    st.latestClientBuildId);
            } else {
                ImGui::TextDisabled("最新客户端：未找到有效发布 manifest");
            }
            if (st.forcedClientBuildId > 0) {
                ImGui::SameLine(0, 10.f);
                ImGui::TextColored(OpsTone::Warn(), "强制 v%s #%u",
                                   st.forcedClientVersionText.c_str(), st.forcedClientBuildId);
                ImGui::SameLine(0, 6.f);
                if (NeutralSmallButton("取消强制##p")) Ops_RequestClearForceClientUpdate(st);
            }
            if (st.publishRestartCount > 0) {
                ImGui::SameLine(0, 10.f);
                ImGui::TextDisabled("拉起 %d", st.publishRestartCount);
            }
            if (!st.hasPython) ImGui::TextColored(OpsTone::Danger(), "缺少 python.exe");

            ImGui::Spacing();
            if (ImGui::SmallButton("启动##p")) Ops_RequestStartPublish(st);
            ImGui::SameLine();
            if (ImGui::SmallButton("停止##p")) Ops_RequestStopPublish(st);
            ImGui::SameLine();
            if (ImGui::SmallButton("重启##p")) Ops_RequestRestartPublish(st);
            ImGui::SameLine(0, 12.f);
            if (ImGui::SmallButton("同步##p")) Ops_RequestSyncPublish(st);
            ImGui::SameLine();
            if (st.latestClientBuildId == 0) ImGui::BeginDisabled();
            if (ImGui::SmallButton("推送更新##p")) ImGui::OpenPopup("confirm_force_update");
            if (st.latestClientBuildId == 0) ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("confirm_force_update", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("确认推送强制更新？");
                ImGui::TextDisabled("目标：v%s  build %u", st.latestClientVersionText.c_str(),
                                    st.latestClientBuildId);
                ImGui::TextWrapped("在线客户端会按强制版本策略拉新包；请确认发布站已同步。");
                ImGui::Spacing();
                if (SafeButton("确认推送##force_yes", ImVec2(120, 0))) {
                    Ops_RequestForceClientUpdate(st);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消##force_no", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("下载目录##p"))
                OpenFolder(JoinPath(st.repoRoot, L"publish_site\\downloads"));
            ImGui::SameLine();
            if (ImGui::SmallButton("复制 URL##p")) CopyText("http://xcat.work:52080/");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::EndTable();
    }

    if (busy) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();

    // ── Log panel ──
    ImGui::TextUnformatted("运维日志");
    ImGui::SameLine();
    ImGui::TextDisabled("artifacts/ops_logs/");
    int logErr = 0, logWarn = 0;
    for (const auto& line : st.logLines) {
        if (LogLineIsError(line)) ++logErr;
        else if (LogLineIsWarn(line)) ++logWarn;
    }
    if (logErr > 0) {
        ImGui::SameLine(0, 10.f);
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.42f, 1.f), "ERR %d", logErr);
    }
    if (logWarn > 0) {
        ImGui::SameLine(0, 8.f);
        ImGui::TextColored(ImVec4(1.f, 0.78f, 0.35f, 1.f), "WARN %d", logWarn);
    }
    ImGui::SameLine(0, 12.f);
    ImGui::Checkbox("自动滚动", &st.logAutoScroll);
    if (st.logAutoScroll && ImGui::IsItemDeactivatedAfterEdit()) st.logScrollToBottom = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("刷新##log")) RefreshLogViewer(st, true);
    ImGui::SameLine();
    if (ImGui::SmallButton("跳底##log")) {
        st.logScrollToBottom = true;
        st.logAutoScroll = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("清空##log")) {
        std::wstring target;
        if (st.logTab == 1) target = OpsLogPublish(st.repoRoot);
        else if (st.logTab == 2) target = OpsLogHelper(st.repoRoot);
        else if (st.logTab == 3) target = OpsLogAccess(st.repoRoot);
        else target = OpsLogTwms(st.repoRoot);
        TruncateLog(target);
        RefreshLogViewer(st, true);
        SetStatus(st, "已清空 " + st.logPathUtf8);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("打开目录##log")) {
        OpenFolder(JoinPath(st.repoRoot, L"artifacts\\ops_logs"));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("复制ERR##log")) {
        std::string out;
        int n = 0;
        for (auto it = st.logLines.rbegin(); it != st.logLines.rend() && n < 40; ++it) {
            if (!LogLineIsError(*it)) continue;
            out += *it;
            out.push_back('\n');
            ++n;
        }
        if (out.empty()) {
            SetStatus(st, "当前日志无 ERROR 行");
        } else {
            CopyText(out.c_str());
            SetStatus(st, "已复制最近 " + std::to_string(n) + " 条 ERROR");
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("从末尾向前最多复制 40 条 ERROR 行");

    {
        auto logSrcBtn = [&](const char* label, int id) {
            const bool selected = (st.logTab == id);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(label) && !selected) {
                st.logTab = id;
                st.logFileSize = 0;
                st.logFileMtime = 0;
                RefreshLogViewer(st, true);
            }
            if (selected) ImGui::PopStyleColor();
        };
        logSrcBtn("TWMS##logsrc", 0);
        ImGui::SameLine();
        logSrcBtn("Publish##logsrc", 1);
        ImGui::SameLine();
        logSrcBtn("Helper##logsrc", 2);
        ImGui::SameLine();
        logSrcBtn("Access##logsrc", 3);
        ImGui::SameLine(0, 12.f);
        ImGui::SetNextItemWidth(140.f);
        ImGui::InputTextWithHint("##log_filter", "日志筛选…", st.logFilter, sizeof(st.logFilter));
        ImGui::SameLine();
        ImGui::Checkbox("仅ERR/WARN", &st.logErrorsOnly);
        ImGui::SameLine(0, 10.f);
        ImGui::TextDisabled("%zu 行", st.logLines.size());
        if (!st.logPathUtf8.empty()) {
            ImGui::SameLine(0, 8.f);
            ImGui::TextDisabled("%s", st.logPathUtf8.c_str());
        }
    }

    ImGui::BeginChild("log_view", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (st.logLines.empty()) {
        ImGui::TextDisabled("%s", st.logEmptyHint.c_str());
    } else {
        std::vector<const std::string*> view;
        view.reserve(st.logLines.size());
        for (const auto& line : st.logLines) {
            if (st.logErrorsOnly && !LogLineIsError(line) && !LogLineIsWarn(line)) continue;
            if (st.logFilter[0] != '\0' && !ContainsIgnoreCase(line, st.logFilter)) continue;
            view.push_back(&line);
        }
        if (view.empty()) {
            ImGui::TextDisabled("(无匹配日志行)");
        } else {
            ImGui::PushTextWrapPos(-1.0f);
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(view.size()), lineH);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& line = *view[static_cast<size_t>(i)];
                    ImGui::PushID(i);
                    ImGui::PushStyleColor(ImGuiCol_Text, LogLineColor(line));
                    ImGui::Selectable(line.c_str(), false,
                                      ImGuiSelectableFlags_AllowDoubleClick |
                                          ImGuiSelectableFlags_SpanAllColumns);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        CopyText(line.c_str());
                        SetStatus(st, "已复制日志行");
                    }
                    if (ImGui::BeginPopupContextItem("log_line")) {
                        if (ImGui::MenuItem("复制本行")) {
                            CopyText(line.c_str());
                            SetStatus(st, "已复制日志行");
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopTextWrapPos();
        }

        if (ImGui::GetIO().MouseWheel != 0.0f && ImGui::IsWindowHovered()) {
            if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 4.f) st.logScrollToBottom = false;
        }
        if (st.logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            st.logScrollToBottom = false;
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace xcat::ops
