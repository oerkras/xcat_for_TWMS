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

void StatusLed(bool on, const char* label) {
    const ImVec4 color = on ? ImVec4(0.25f, 0.85f, 0.40f, 1.f) : ImVec4(0.75f, 0.25f, 0.25f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(on ? "●" : "○");
    ImGui::PopStyleColor();
    ImGui::SameLine();
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
        return ImVec4(1.f, 0.45f, 0.42f, 1.f);
    }
    if (line.find(" WARN ") != std::string::npos || line.find("Warning") != std::string::npos) {
        return ImVec4(1.f, 0.78f, 0.35f, 1.f);
    }
    if (line.find(" INFO ") != std::string::npos) {
        return ImVec4(0.72f, 0.82f, 0.92f, 1.f);
    }
    return ImVec4(0.78f, 0.80f, 0.84f, 1.f);
}

int JsonIntField(const std::string& obj, const char* key, int fallback = 0) {
    const std::string raw = FindJsonNumber(obj, key);
    if (raw.empty() || raw == "?") return fallback;
    return static_cast<int>(std::strtol(raw.c_str(), nullptr, 10));
}

bool ParseClientsPayload(const std::string& body, OpsState& st) {
    st.clients.clear();
    st.clientsCount = JsonIntField(body, "count", 0);
    st.clientsTracked = JsonIntField(body, "tracked", 0);
    st.clientsGeoProvider = FindJsonString(body, "geoProvider");
    const int activeSec = JsonIntField(body, "activeSec", st.clientsActiveSec);
    if (activeSec > 0) st.clientsActiveSec = activeSec;

    const size_t arrKey = body.find("\"clients\"");
    if (arrKey == std::string::npos) return false;
    size_t i = body.find('[', arrKey);
    if (i == std::string::npos) return false;
    ++i;
    while (i < body.size()) {
        while (i < body.size() &&
               (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n' || body[i] == ',')) {
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
        OpsState::ConnectedClient row;
        row.ip = FindJsonString(obj, "ip");
        if (row.ip.empty()) continue;
        row.geo = FindJsonString(obj, "geo");
        row.machine = FindJsonString(obj, "machine");
        row.device = FindJsonString(obj, "device");
        row.appVersion = FindJsonString(obj, "appVersion");
        row.lastKind = FindJsonString(obj, "lastKind");
        row.lastSeenAt = FindJsonString(obj, "lastSeenAt");
        row.idleSec = JsonIntField(obj, "idleSec", 0);
        row.hits = JsonIntField(obj, "hits", 0);
        row.lastStatus = JsonIntField(obj, "lastStatus", 0);
        row.sameIpOnline = JsonIntField(obj, "sameIpOnline", 1);
        row.knownOnIp = JsonIntField(obj, "knownOnIp", 0);
        row.identified = obj.find("\"identified\":true") != std::string::npos;
        if (!row.identified && !row.machine.empty() && !row.device.empty()) row.identified = true;
        st.clients.push_back(std::move(row));
    }
    if (st.clientsCount <= 0) st.clientsCount = static_cast<int>(st.clients.size());
    return true;
}

void RefreshClients(OpsState& st, bool force) {
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

    const std::wstring path =
        L"/twms/admin/clients?activeSec=" + std::to_wstring(st.clientsActiveSec);
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
            if (id == 1) RefreshClients(st, true);
        }
        if (selected) ImGui::PopStyleColor();
    };
    tabBtn("服务与日志##maintab", 0);
    ImGui::SameLine();
    tabBtn("连接列表##maintab", 1);
}

void DrawClientsPanel(OpsState& st) {
    if (st.clientsAutoRefresh) RefreshClients(st, false);

    ImGui::TextUnformatted("当前连接");
    ImGui::SameLine();
    ImGui::TextDisabled("  近 %ds 有请求视为在线（客户端约 60s 轮询 force.json）", st.clientsActiveSec);
    ImGui::SameLine();
    ImGui::Checkbox("自动刷新", &st.clientsAutoRefresh);
    ImGui::SameLine();
    if (ImGui::Button("刷新##clients")) RefreshClients(st, true);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    if (ImGui::InputInt("窗口秒##clients", &st.clientsActiveSec)) {
        if (st.clientsActiveSec < 30) st.clientsActiveSec = 30;
        if (st.clientsActiveSec > 3600) st.clientsActiveSec = 3600;
    }

    if (!st.clientsError.empty()) {
        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f), "%s", st.clientsError.c_str());
    } else {
    ImGui::TextDisabled(
        "在线 %d / 跟踪 %d · 归属 %s · 同公网IP按「计算机/设备」分行（需客户端探活带头）",
        st.clientsCount, st.clientsTracked,
        st.clientsGeoProvider.empty() ? "ip9.com.cn" : st.clientsGeoProvider.c_str());
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("clients_table", 8, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("归属地", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("设备", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("最近活动", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableSetupColumn("空闲", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("请求", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableHeadersRow();

        if (st.clients.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", st.clientsError.empty() ? "(暂无在线客户端)" : "(无数据)");
        } else {
            for (size_t idx = 0; idx < st.clients.size(); ++idx) {
                const auto& c = st.clients[idx];
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char ipLabel[96]{};
                if (c.sameIpOnline > 1) {
                    std::snprintf(ipLabel, sizeof(ipLabel), "%s ·×%d", c.ip.c_str(), c.sameIpOnline);
                } else {
                    std::snprintf(ipLabel, sizeof(ipLabel), "%s", c.ip.c_str());
                }
                ImGui::Selectable(ipLabel, false);
                if (ImGui::IsItemHovered()) {
                    if (c.sameIpOnline > 1) {
                        ImGui::SetTooltip("同公网 IP 当前在线 %d 台（NAT/公司宽带）", c.sameIpOnline);
                    } else if (!c.identified && c.knownOnIp > 1) {
                        ImGui::SetTooltip("旧客户端未报身份；该 IP 历史上传见过 %d 台设备", c.knownOnIp);
                    }
                }
                if (ImGui::BeginPopupContextItem("ip")) {
                    if (ImGui::MenuItem("复制 IP")) CopyText(c.ip.c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                if (c.geo.empty()) ImGui::TextDisabled("—");
                else ImGui::TextUnformatted(c.geo.c_str());
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
                ImGui::TextUnformatted((!c.identified || c.device.empty()) ? "—" : c.device.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(c.appVersion.empty() ? "—" : c.appVersion.c_str());
                ImGui::TableSetColumnIndex(5);
                if (!c.lastSeenAt.empty()) {
                    ImGui::Text("%s", c.lastSeenAt.c_str());
                    ImGui::SameLine(0, 6.f);
                    ImGui::TextDisabled("%s", c.lastKind.c_str());
                } else {
                    ImGui::TextDisabled("—");
                }
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%ds", c.idleSec);
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%d", c.hits);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void OpsPanel_Draw(OpsState& st) {
    if (st.mainTab == 0) RefreshLogViewer(st, false);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("XCat TWMS Ops", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Header ──
    ImGui::TextUnformatted("XCat TWMS 运维控制台");
    ImGui::SameLine();
    ImGui::TextDisabled("  台服经典版 · 本仓 bin_ops");
    if (!st.diskFreeText.empty()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(st.diskFreeText.c_str()).x - 24.f);
        ImGui::TextDisabled("%s", st.diskFreeText.c_str());
    }
    ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f),
                       "关闭本窗口会停止 API(:18789) 与发布站(:52080)；请勿与枫星 ops 混用。");

    {
        static const char* kThemeLabels[] = {"暗夜", "白天"};
        int idx = (xcat::ui::UiTheme_Preference() == xcat::ui::ThemePreference::Light) ? 1 : 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.f);
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

    // ── Global toolbar ──
    const bool busy = st.busy.load();
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("全部启动")) Ops_RequestStartAll(st);
    ImGui::SameLine();
    if (ImGui::Button("全部停止")) Ops_RequestStopAll(st);
    ImGui::SameLine();
    ImGui::Checkbox("崩溃自动拉起", &st.autoRestart);
    if (busy) ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, xcat::ui::UiTheme_Palette().statusStripBg);
    ImGui::BeginChild("status_bar", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 10.f), true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextWrapped("%s", GetStatus(st).c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    DrawMainTabButtons(st);
    ImGui::Separator();
    ImGui::Spacing();

    if (st.mainTab == 1) {
        DrawClientsPanel(st);
        ImGui::End();
        return;
    }

    // ── Feature area: two columns, no scrollable child cards ──
    if (busy) ImGui::BeginDisabled();

    const bool twmsOk = TwmsRunning(st) && st.twmsHealthOk && st.twmsReadyOk;
    const bool publishOk = PublishRunning(st) && st.publishProbeOk;

    if (ImGui::BeginTable("svc_table", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoPadOuterX,
                          ImVec2(0, 0))) {
        ImGui::TableNextColumn();
        {
            StatusLed(twmsOk, "TWMS API  :18789");
            if (TwmsRunning(st)) {
                ImGui::Text("运行中  PID %lu", static_cast<unsigned long>(TwmsPid(st)));
            } else {
                ImGui::TextUnformatted("已停止");
            }
            ImGui::TextDisabled("v%s  %s  %s%s",
                                st.serverVersionText.empty() ? "-" : st.serverVersionText.c_str(),
                                st.twmsUptimeText.c_str(), st.twmsHealthText.c_str(),
                                st.twmsReadyOk ? " · ready" : (TwmsRunning(st) ? " · not ready" : ""));
            if (!st.twmsStatsText.empty()) ImGui::TextDisabled("%s", st.twmsStatsText.c_str());
            if (!st.twmsRequestText.empty()) ImGui::TextDisabled("%s", st.twmsRequestText.c_str());
            if (st.twmsRestartCount > 0) {
                ImGui::TextDisabled("自动拉起 %d · backoff %llus", st.twmsRestartCount,
                                    static_cast<unsigned long long>(st.twmsBackoffMs / 1000));
            }
            if (!st.hasNode) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "缺少 node.exe");

            if (ImGui::Button("启动##a")) Ops_RequestStartTwms(st);
            ImGui::SameLine();
            if (ImGui::Button("停止##a")) Ops_RequestStopTwms(st);
            ImGui::SameLine();
            if (ImGui::Button("重启##a")) Ops_RequestRestartTwms(st);
            if (ImGui::Button("上传目录##a")) OpenFolder(JoinPath(st.repoRoot, L"user_log_uploads"));
            ImGui::SameLine();
            if (ImGui::Button("日志索引##a")) {
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
            if (ImGui::Button("复制 Health##a")) CopyText("http://xcat.work:18789/twms/health");
            ImGui::TextDisabled("客户端更新口：http://xcat.work:18789/twms");
        }

        ImGui::TableNextColumn();
        {
            StatusLed(publishOk, "发布站  :52080");
            if (PublishRunning(st)) {
                ImGui::Text("运行中  PID %lu", static_cast<unsigned long>(PublishPid(st)));
            } else {
                ImGui::TextUnformatted("已停止");
            }
            ImGui::TextDisabled("%s", st.publishProbeText.c_str());
            ImGui::TextDisabled("http://xcat.work:52080/");
            if (st.latestClientBuildId > 0) {
                ImGui::TextDisabled("最新客户端 v%s  build %u", st.latestClientVersionText.c_str(),
                                    st.latestClientBuildId);
            } else {
                ImGui::TextDisabled("最新客户端版本：未找到有效发布 manifest");
            }
            if (st.forcedClientBuildId > 0) {
                ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f), "强制更新已发布：v%s build %u",
                                   st.forcedClientVersionText.c_str(), st.forcedClientBuildId);
            }
            if (st.publishRestartCount > 0) {
                ImGui::TextDisabled("自动拉起 %d · backoff %llus", st.publishRestartCount,
                                    static_cast<unsigned long long>(st.publishBackoffMs / 1000));
            }
            if (!st.hasPython) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "缺少 python.exe");

            if (ImGui::Button("启动##p")) Ops_RequestStartPublish(st);
            ImGui::SameLine();
            if (ImGui::Button("停止##p")) Ops_RequestStopPublish(st);
            ImGui::SameLine();
            if (ImGui::Button("重启##p")) Ops_RequestRestartPublish(st);
            if (ImGui::Button("同步##p")) Ops_RequestSyncPublish(st);
            ImGui::SameLine();
            if (st.latestClientBuildId == 0) ImGui::BeginDisabled();
            if (ImGui::Button("一键推送更新##p")) Ops_RequestForceClientUpdate(st);
            if (st.latestClientBuildId == 0) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("下载目录##p")) OpenFolder(JoinPath(st.repoRoot, L"publish_site\\downloads"));
            ImGui::SameLine();
            if (ImGui::Button("复制 URL##p")) CopyText("http://xcat.work:52080/");
        }

        ImGui::EndTable();
    }

    if (busy) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();

    // ── Log panel (dedicated) ──
    ImGui::TextUnformatted("运维日志");
    ImGui::SameLine();
    ImGui::TextDisabled("  artifacts/ops_logs/");
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &st.logAutoScroll);
    if (st.logAutoScroll && ImGui::IsItemDeactivatedAfterEdit()) st.logScrollToBottom = true;
    ImGui::SameLine();
    if (ImGui::Button("刷新##log")) RefreshLogViewer(st, true);
    ImGui::SameLine();
    if (ImGui::Button("清空当前##log")) {
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
    if (ImGui::Button("打开日志目录##log")) {
        OpenFolder(JoinPath(st.repoRoot, L"artifacts\\ops_logs"));
    }

    // 用按钮切换日志源，避免 TabBar 溢出下拉与外部 logTab 状态互相抢选导致闪烁回弹
    {
        auto logSrcBtn = [&](const char* label, int id) {
            const bool selected = (st.logTab == id);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label) && !selected) {
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
    }

    ImGui::TextDisabled("%s", st.logPathUtf8.c_str());
    ImGui::BeginChild("log_view", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (st.logLines.empty()) {
        ImGui::TextDisabled("%s", st.logEmptyHint.c_str());
    } else {
        // 禁止自动换行：变高行使 ListClipper 失真并导致滚动回弹；横滑看长行即可
        ImGui::PushTextWrapPos(-1.0f);
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(st.logLines.size()), lineH);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& line = st.logLines[static_cast<size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, LogLineColor(line));
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopTextWrapPos();

        // 用户上翻时暂停吸底；回到底部或勾选自动滚动后由新内容再吸一次
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
