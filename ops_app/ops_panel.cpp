#include "ops_panel.h"

#include "log_tail.h"
#include "ops_health.h"
#include "ops_window.h"

#include "xcat_imgui_theme.h"

#include "../common/process_util.h"
#include "../common/xcat_map_names.h"
#include "../common/xcat_item_catalog.h"

#include "imgui.h"

#include <Shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xcat::ops {
namespace {

constexpr ULONGLONG kWatchdogGraceMs = 8000;
constexpr ULONGLONG kWatchdogMaxBackoffMs = 120000;
constexpr ULONGLONG kHealthyResetMs = 60000;
constexpr ULONGLONG kLogRotateBytes = 32ull * 1024ull * 1024ull;
constexpr ULONGLONG kMesoDashKeepMs = 7ull * 24ull * 60ull * 60ull * 1000ull;
constexpr ULONGLONG kMesoEventKeepMs = 30ull * 24ull * 60ull * 60ull * 1000ull;
// 探活采样约 5s；相邻点超过此时长视为关服/API 断连，折线断开不连斜线。
constexpr ULONGLONG kMesoDashGapMs = 30ull * 1000ull;

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
std::wstring OpsLogMesoDash(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\meso_dash.jsonl");
}
std::wstring OpsLogMesoEvents(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\meso_events.jsonl");
}
std::wstring OpsLogMesoUnits(const std::wstring& repo) {
    return JoinPath(repo, L"artifacts\\ops_logs\\meso_units.json");
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
    // 剪贴板文本必须用 CF_UNICODETEXT(UTF-16)：CF_TEXT 会被按系统 ANSI(本机 GBK)解读，
    // UTF-8 中文塞进去到别处粘贴就成乱码（如「永不过期」变「姘镐笉杩囨湡」）。
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wlen <= 0) return;
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen) * sizeof(wchar_t));
    if (!mem) {
        CloseClipboard();
        return;
    }
    void* dst = GlobalLock(mem);
    if (dst) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, static_cast<wchar_t*>(dst), wlen);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
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
    if (p == std::string::npos) return "";
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

// 取 "key":{...} 对象正文（含花括号），供嵌套字段再 FindJsonString。
std::string FindJsonObjectSlice(const std::string& body, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    size_t i = p + needle.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != '{') return "";
    int depth = 0;
    const size_t start = i;
    for (; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return body.substr(start, i - start + 1);
        } else if (c == '"') {
            ++i;
            while (i < body.size()) {
                if (body[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (body[i] == '"') break;
                ++i;
            }
        }
    }
    return "";
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

void MesoDashLoadFile(OpsState& st);

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
    MesoDashLoadFile(st);

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
        SetStatus(st, "已全体推送强制更新 v" + latest.version + " build " +
                          std::to_string(latest.buildId) +
                          "（所有客户端轮询都会更新；单机请用连接表「推更」）");
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
void RefreshUpdateChannels(OpsState& st, bool force);

void OpsState_Tick(OpsState& st) {
    if (st.autoStartPending) {
        st.autoStartPending = false;
        Ops_RequestStartAll(st);
    }

    if (st.shuttingDown.load()) return;

    // 利润折线按墙钟采样，不能绑在「当前 Tab / 窗口是否在画」。
    // 最小化时 main 仍会 Tick；忙于启停服务时也继续采，避免切走页面就断线。
    if (TwmsRunning(st)) {
        RefreshClients(st, false);
    } else if (!st.clients.empty() || st.clientsCount != 0) {
        st.clients.clear();
        st.clientsCount = 0;
        st.clientsTracked = 0;
    }

    if (st.busy.load()) return;

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
        if (TwmsRunning(st)) RefreshUpdateChannels(st, false);
    }

    if (TwmsRunning(st)) {
        const auto h = HttpGet(L"127.0.0.1", 18789, L"/twms/health", 400);
        st.twmsHealthOk = h.ok;
        if (h.ok) {
            st.twmsHealthText = "HTTP " + std::to_string(h.status);
            st.serverVersionText = FindJsonString(h.body, "version");
            const std::string up = FindJsonNumber(h.body, "uptimeSec");
            st.twmsUptimeText = up.empty() ? "" : ("uptime " + up + "s");
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
                                   (bytes.empty() ? "-"
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

// 与启动器同口径：repo/bin/XCat_data/dataservice/map_names.tsv
std::string OpsMapNamesBinDir(const OpsState& st) {
    return st.repoRootUtf8 + "\\bin\\XCat_data";
}

// 离线表优先；未命中再回退客户端上报名；再无则「圖{id}」。
std::string ResolveClientMapLabel(const OpsState& st, uint32_t mapId,
                                  const std::string& clientReported) {
    if (mapId == 0) return clientReported;
    const auto& pack = xcat::GetSharedMapNames(OpsMapNamesBinDir(st).c_str());
    const auto it = pack.labelById.find(static_cast<int>(mapId));
    if (it != pack.labelById.end() && !it->second.empty()) return it->second;
    if (!clientReported.empty()) return clientReported;
    return xcat::MapNamesLabelById(pack, static_cast<int>(mapId));
}

std::string ClientMapLabel(const OpsState& st, const OpsState::ConnectedClient& c) {
    return ResolveClientMapLabel(st, c.mapId, c.mapName);
}

// 单元格用短名（「街道·地图」取地图侧），省列宽。
std::string MapLabelShort(const std::string& label) {
    static const std::string kSep = "·";
    const size_t p = label.rfind(kSep);
    if (p != std::string::npos && p + kSep.size() < label.size()) {
        return label.substr(p + kSep.size());
    }
    return label;
}

void FormatMapChannelCell(const OpsState& st, const OpsState::ConnectedClient& c, char* out,
                          size_t outN) {
    if (!out || outN == 0) return;
    out[0] = '\0';
    if (c.mapId == 0 && c.channelId <= 0) return;
    const std::string label = ClientMapLabel(st, c);
    const std::string shortN = MapLabelShort(label);
    if (!shortN.empty() && c.mapId > 0 && c.channelId > 0) {
        std::snprintf(out, outN, "%s·%d", shortN.c_str(), c.channelId);
    } else if (!shortN.empty() && c.mapId > 0) {
        std::snprintf(out, outN, "%s", shortN.c_str());
    } else if (c.mapId > 0 && c.channelId > 0) {
        std::snprintf(out, outN, "%u·%d", c.mapId, c.channelId);
    } else if (c.mapId > 0) {
        std::snprintf(out, outN, "%u", c.mapId);
    } else {
        std::snprintf(out, outN, "ch.%d", c.channelId);
    }
}

const char* ChannelOrDash(int ch, char* buf, size_t n) {
    if (ch > 0 && buf && n > 0) {
        std::snprintf(buf, n, "%d", ch);
        return buf;
    }
    return "-";
}

void CopyClientSummary(const OpsState& st, const OpsState::ConnectedClient& c) {
    const std::string mapLabel = ClientMapLabel(st, c);
    char chBuf[16]{};
    char buf[1024]{};
    std::snprintf(buf, sizeof(buf),
                  "%s\t%s\t%s\t%s\t%s\t%s\t%s\tLv.%d\t%s\t%s\t%s\tmap=%u\t%s\tch=%s\tgate=%s\tidle=%ds",
                  c.ip.c_str(), c.machine.c_str(), c.mac.c_str(), c.token.c_str(),
                  c.deviceId.c_str(), c.appVersion.c_str(),
                  c.charName.empty() ? "-" : c.charName.c_str(), c.charLevel,
                  c.charJobName.empty() ? "-" : c.charJobName.c_str(),
                  c.charMeso.empty() ? "-" : c.charMeso.c_str(),
                  c.hasWealthScrolls ? (c.wealthScrolls.empty() ? "-" : c.wealthScrolls.c_str())
                                     : "-",
                  c.mapId,
                  mapLabel.empty() ? "-" : mapLabel.c_str(), ChannelOrDash(c.channelId, chBuf, sizeof(chBuf)),
                  c.gate.empty() ? "-" : c.gate.c_str(), c.idleSec);
    CopyText(buf);
}

void AppendClientSummaryLine(std::string& out, const OpsState& st, const OpsState::ConnectedClient& c) {
    const std::string mapLabel = ClientMapLabel(st, c);
    char chBuf[16]{};
    char buf[1024]{};
    std::snprintf(buf, sizeof(buf), "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%u\t%s\t%s\t%s\t%d\n",
                  c.ip.c_str(), c.machine.c_str(), c.mac.c_str(), c.token.c_str(),
                  c.deviceId.c_str(), c.appVersion.c_str(),
                  c.charName.empty() ? "-" : c.charName.c_str(), c.charLevel,
                  c.charJobName.empty() ? "-" : c.charJobName.c_str(),
                  c.charMeso.empty() ? "-" : c.charMeso.c_str(),
                  c.hasWealthScrolls ? (c.wealthScrolls.empty() ? "-" : c.wealthScrolls.c_str())
                                     : "-",
                  c.mapId,
                  mapLabel.empty() ? "-" : mapLabel.c_str(), ChannelOrDash(c.channelId, chBuf, sizeof(chBuf)),
                  c.gate.empty() ? "-" : c.gate.c_str(), c.idleSec);
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

// 组内背包金累加（十进制串 → ull；忽略空/非数字/负数；溢出夹到 ULLONG_MAX）。
bool TryParseMesoUll(const std::string& mesoRaw, unsigned long long& outAbs, bool& outNeg) {
    outAbs = 0;
    outNeg = false;
    if (mesoRaw.empty()) return false;
    std::string digits;
    digits.reserve(mesoRaw.size());
    for (size_t i = 0; i < mesoRaw.size(); ++i) {
        const char c = mesoRaw[i];
        if (i == 0 && c == '-') {
            outNeg = true;
            continue;
        }
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    if (digits.empty() || digits.size() > 18) return false;
    char* end = nullptr;
    outAbs = std::strtoull(digits.c_str(), &end, 10);
    return end && *end == '\0';
}

std::string SumGroupMeso(const OpsState& st, const std::vector<size_t>& members, int* outCounted) {
    if (outCounted) *outCounted = 0;
    unsigned long long sum = 0;
    int counted = 0;
    for (size_t mi : members) {
        unsigned long long v = 0;
        bool neg = false;
        if (!TryParseMesoUll(st.clients[mi].charMeso, v, neg) || neg) continue;
        if (sum > (std::numeric_limits<unsigned long long>::max)() - v)
            sum = (std::numeric_limits<unsigned long long>::max)();
        else
            sum += v;
        ++counted;
    }
    if (outCounted) *outCounted = counted;
    if (counted <= 0) return {};
    return std::to_string(sum);
}

ULONGLONG MesoDashWallMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    constexpr ULONGLONG kUnixEpochFt = 116444736000000000ull;
    if (u.QuadPart < kUnixEpochFt) return 0;
    return (u.QuadPart - kUnixEpochFt) / 10000ull;
}

void MesoDashPrune(std::deque<OpsState::MesoDashPoint>& pts, ULONGLONG cutMs) {
    while (!pts.empty() && pts.front().wallMs < cutMs) pts.pop_front();
}

void MesoDashPush(std::deque<OpsState::MesoDashPoint>& pts, ULONGLONG nowMs,
                  unsigned long long meso) {
    if (!pts.empty() && pts.back().wallMs == nowMs) {
        pts.back().meso = meso;
        return;
    }
    pts.push_back(OpsState::MesoDashPoint{nowMs, meso});
}

void MesoDashAppendFile(OpsState& st, ULONGLONG nowMs, unsigned long long total);
void MesoDashLoadFile(OpsState& st);
void MesoDashClearFile(OpsState& st);
void MesoEventsAppend(OpsState& st, const OpsState::MesoEvent& ev);
void MesoEventsLoad(OpsState& st);
void MesoUnitsSave(OpsState& st);
void MesoUnitsLoad(OpsState& st);
void MesoMergeUidTokenAliases(OpsState& st);

std::string MesoUnitKey(const std::string& token, const std::string& charName,
                        const std::string& deviceId) {
    std::string k = token;
    k.push_back('\x1f');
    if (!charName.empty())
        k += charName;
    else {
        k.push_back('#');
        k += deviceId.empty() ? "?" : deviceId;
    }
    return k;
}

// 利润监控分组键：已签卡用 uid（同人多机合并）；未激活才退回旧调试 TOKEN。
std::string MesoPersonId(const std::string& uid, const std::string& token) {
    if (!uid.empty()) return uid;
    return token;
}

using WealthQtyMap = std::unordered_map<int, unsigned long long>;

void ParseWealthScrolls(const std::string& s, WealthQtyMap& out) {
    out.clear();
    if (s.empty() || s == "-") return;
    size_t i = 0;
    while (i < s.size()) {
        const size_t comma = s.find(',', i);
        const std::string part =
            s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        const size_t colon = part.find(':');
        if (colon != std::string::npos && colon > 0) {
            const long id = std::strtol(part.c_str(), nullptr, 10);
            const unsigned long long qty = std::strtoull(part.c_str() + colon + 1, nullptr, 10);
            if (id > 0 && qty > 0) out[static_cast<int>(id)] += qty;
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
}

std::string ScrollItemLabel(const OpsState& st, int itemId) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "%d", itemId);
    const char* name = xcat::ItemCatalogLookupName(
        xcat::GetSharedItemCatalog(OpsMapNamesBinDir(st).c_str()), code);
    if (name && name[0]) return name;
    return code;
}

std::string FormatWealthScrollsHuman(const OpsState& st, const std::string& raw) {
    WealthQtyMap m;
    ParseWealthScrolls(raw, m);
    if (m.empty()) return "无";
    std::vector<std::pair<int, unsigned long long>> items(m.begin(), m.end());
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    int n = 0;
    for (const auto& it : items) {
        if (n) out += "，";
        out += ScrollItemLabel(st, it.first);
        out += " ×";
        out += std::to_string(it.second);
        if (++n >= 8) {
            if (static_cast<int>(items.size()) > n) out += "…";
            break;
        }
    }
    return out;
}

bool MesoKindScroll(const std::string& kind) {
    return kind.size() >= 7 && kind.compare(0, 7, "scroll_") == 0;
}

bool MesoKindAlert(const std::string& kind) {
    return kind == "outflow" || kind == "token_xfer" || kind == "reconnect_drop" ||
           kind == "scroll_outflow" || kind == "scroll_xfer" || kind == "scroll_reconnect";
}

bool MesoKindAbnormal(const std::string& kind) {
    return MesoKindAlert(kind) || kind == "char_move" || kind == "scroll_move";
}

int MesoCountEvents(const OpsState& st, ULONGLONG sinceMs, int mode) {
    int n = 0;
    for (const auto& e : st.mesoEvents) {
        if (e.wallMs < sinceMs) continue;
        if (mode == 0 && MesoKindAlert(e.kind)) ++n;
        else if (mode == 1 && MesoKindAbnormal(e.kind)) ++n;
        else if (mode == 2 && (e.kind == "inflow" || e.kind == "scroll_inflow")) ++n;
    }
    return n;
}

bool MesoAmtClose(unsigned long long a, unsigned long long b) {
    const unsigned long long lo = (std::min)(a, b);
    const unsigned long long hi = (std::max)(a, b);
    if (hi == 0) return true;
    if (hi - lo <= 50000ull) return true;
    return lo * 5ull >= hi * 4ull;
}

OpsState::MesoDashSeries* MesoDashFindSeries(OpsState& st, const std::string& token) {
    for (auto& s : st.mesoDashSeries) {
        if (s.token == token) return &s;
    }
    return nullptr;
}

void MesoDashMergePoints(std::deque<OpsState::MesoDashPoint>& dst,
                         const std::deque<OpsState::MesoDashPoint>& src) {
    if (src.empty()) return;
    if (dst.empty()) {
        dst = src;
        return;
    }
    std::deque<OpsState::MesoDashPoint> out;
    size_t i = 0;
    size_t j = 0;
    while (i < dst.size() || j < src.size()) {
        if (i < dst.size() && (j >= src.size() || dst[i].wallMs < src[j].wallMs)) {
            out.push_back(dst[i++]);
        } else if (j < src.size() && (i >= dst.size() || src[j].wallMs < dst[i].wallMs)) {
            out.push_back(src[j++]);
        } else {
            out.push_back(dst[i]);
            ++i;
            ++j;
        }
    }
    dst.swap(out);
}

void MesoCollectTokenUidAliases(const OpsState& st,
                                std::unordered_map<std::string, std::string>& tokenToUid) {
    std::unordered_set<std::string> amb;
    auto consider = [&](const std::string& token, const std::string& uid) {
        if (token.empty() || uid.empty() || token == uid) return;
        if (amb.count(token)) return;
        const auto it = tokenToUid.find(token);
        if (it == tokenToUid.end())
            tokenToUid.emplace(token, uid);
        else if (it->second != uid) {
            tokenToUid.erase(it);
            amb.insert(token);
        }
    };
    for (const auto& c : st.clients) consider(c.token, c.uid);
    for (const auto& u : st.mesoUnits) consider(u.token, u.uid);
}

void MesoMergeUidTokenAliases(OpsState& st) {
    std::unordered_map<std::string, std::string> tokenToUid;
    MesoCollectTokenUidAliases(st, tokenToUid);

    for (auto& u : st.mesoUnits) {
        if (!u.uid.empty()) {
            u.key = MesoUnitKey(u.uid, u.charName, u.deviceId);
            continue;
        }
        const auto it = tokenToUid.find(u.token);
        if (it == tokenToUid.end()) continue;
        u.uid = it->second;
        u.key = MesoUnitKey(u.uid, u.charName, u.deviceId);
    }

    {
        std::vector<OpsState::MesoUnit> kept;
        std::unordered_map<std::string, size_t> idx;
        kept.reserve(st.mesoUnits.size());
        for (auto& u : st.mesoUnits) {
            if (u.key.empty()) continue;
            const auto it = idx.find(u.key);
            if (it == idx.end()) {
                idx.emplace(u.key, kept.size());
                kept.push_back(std::move(u));
                continue;
            }
            OpsState::MesoUnit& a = kept[it->second];
            const bool preferNew = u.lastSeenMs > a.lastSeenMs ||
                                   (u.lastSeenMs == a.lastSeenMs && a.uid.empty() && !u.uid.empty());
            OpsState::MesoUnit win = preferNew ? std::move(u) : std::move(a);
            OpsState::MesoUnit& other = preferNew ? a : u;
            if (win.uid.empty()) win.uid = other.uid;
            if (win.token.empty()) win.token = other.token;
            if (win.machine.empty()) win.machine = other.machine;
            if (!win.scrollsSampled && other.scrollsSampled) {
                win.lastScrolls = other.lastScrolls;
                win.scrollsSampled = true;
            }
            win.sampled = win.sampled || other.sampled;
            win.online = win.online || other.online;
            a = std::move(win);
        }
        st.mesoUnits.swap(kept);
    }

    if (tokenToUid.empty()) return;

    std::vector<size_t> drop;
    for (size_t i = 0; i < st.mesoDashSeries.size(); ++i) {
        auto& s = st.mesoDashSeries[i];
        const auto it = tokenToUid.find(s.token);
        if (it == tokenToUid.end()) continue;
        const std::string& uid = it->second;
        if (s.token == uid) {
            s.uid = uid;
            continue;
        }
        int dest = -1;
        for (size_t j = 0; j < st.mesoDashSeries.size(); ++j) {
            if (j == i) continue;
            if (st.mesoDashSeries[j].token == uid) {
                dest = static_cast<int>(j);
                break;
            }
        }
        if (dest < 0) {
            s.token = uid;
            s.uid = uid;
            continue;
        }
        auto& d = st.mesoDashSeries[static_cast<size_t>(dest)];
        MesoDashMergePoints(d.points, s.points);
        d.uid = uid;
        d.visible = d.visible || s.visible;
        if (s.lastAlertMs > d.lastAlertMs) d.lastAlertMs = s.lastAlertMs;
        if (!d.points.empty()) d.lastMeso = d.points.back().meso;
        drop.push_back(i);
    }
    for (size_t n = drop.size(); n > 0; --n) {
        st.mesoDashSeries.erase(st.mesoDashSeries.begin() +
                                static_cast<std::ptrdiff_t>(drop[n - 1]));
    }
    for (auto& e : st.mesoEvents) {
        const auto it = tokenToUid.find(e.token);
        if (it != tokenToUid.end()) e.token = it->second;
        const auto jt = tokenToUid.find(e.peerToken);
        if (jt != tokenToUid.end()) e.peerToken = jt->second;
    }
}

void MesoMarkSeriesAlert(OpsState& st, const std::string& token, ULONGLONG nowMs) {
    auto* s = MesoDashFindSeries(st, token);
    if (!s) {
        OpsState::MesoDashSeries neu;
        neu.token = token;
        neu.visible = true;
        st.mesoDashSeries.push_back(std::move(neu));
        s = &st.mesoDashSeries.back();
    }
    s->lastAlertMs = nowMs;
}

void SampleMesoDash(OpsState& st) {
    MesoMergeUidTokenAliases(st);
    const ULONGLONG nowMs = MesoDashWallMs();
    const ULONGLONG cutMs = nowMs > kMesoDashKeepMs ? nowMs - kMesoDashKeepMs : 0;
    const unsigned long long alertMin = st.mesoAlertMin > 0 ? st.mesoAlertMin : 100000ull;

    struct LiveUnit {
        std::string key;
        std::string personId;
        std::string token;
        std::string uid;
        std::string charName;
        std::string deviceId;
        std::string machine;
        unsigned long long meso = 0;
        int sessions = 0;
        bool hasWealth = false;
        std::string wealth;
    };
    std::unordered_map<std::string, LiveUnit> live;
    int noToken = 0;
    for (const auto& c : st.clients) {
        const std::string personId = MesoPersonId(c.uid, c.token);
        if (personId.empty()) {
            ++noToken;
            continue;
        }
        const std::string key = MesoUnitKey(personId, c.charName, c.deviceId);
        LiveUnit& u = live[key];
        if (u.key.empty()) {
            u.key = key;
            u.personId = personId;
            u.token = c.token;
            u.uid = c.uid;
            u.charName = c.charName;
            u.deviceId = c.deviceId;
            u.machine = c.machine;
        } else {
            if (u.uid.empty() && !c.uid.empty()) u.uid = c.uid;
            if (u.token.empty() && !c.token.empty()) u.token = c.token;
        }
        ++u.sessions;
        unsigned long long v = 0;
        bool neg = false;
        if (TryParseMesoUll(c.charMeso, v, neg) && !neg) {
            if (u.meso > (std::numeric_limits<unsigned long long>::max)() - v)
                u.meso = (std::numeric_limits<unsigned long long>::max)();
            else
                u.meso += v;
        }
        if (c.hasWealthScrolls) {
            u.hasWealth = true;
            u.wealth = c.wealthScrolls;
        }
    }
    st.mesoDashNoToken = noToken;

    struct Delta {
        std::string key;
        std::string token;
        std::string charName;
        unsigned long long before = 0;
        unsigned long long after = 0;
        unsigned long long mag = 0;
        bool down = false;
        bool reconnect = false;
        bool used = false;
    };
    std::vector<Delta> deltas;
    struct ScrollDelta {
        std::string token;
        std::string charName;
        int itemId = 0;
        unsigned long long before = 0;
        unsigned long long after = 0;
        unsigned long long mag = 0;
        bool down = false;
        bool reconnect = false;
        bool used = false;
    };
    std::vector<ScrollDelta> scrollDeltas;

    for (auto& kv : live) {
        LiveUnit& lu = kv.second;
        OpsState::MesoUnit* pu = nullptr;
        for (auto& x : st.mesoUnits) {
            if (x.key == lu.key) {
                pu = &x;
                break;
            }
        }
        if (!pu && !lu.uid.empty()) {
            for (auto& x : st.mesoUnits) {
                if (!lu.deviceId.empty() && x.deviceId == lu.deviceId &&
                    x.charName == lu.charName &&
                    (x.uid.empty() || x.uid == lu.uid)) {
                    pu = &x;
                    break;
                }
                if (!lu.token.empty() && x.uid.empty() && x.token == lu.token &&
                    x.charName == lu.charName) {
                    pu = &x;
                    break;
                }
            }
        }
        if (!pu) {
            OpsState::MesoUnit neu;
            neu.key = lu.key;
            neu.token = lu.token;
            neu.uid = lu.uid;
            neu.charName = lu.charName;
            neu.deviceId = lu.deviceId;
            neu.machine = lu.machine;
            st.mesoUnits.push_back(std::move(neu));
            pu = &st.mesoUnits.back();
        }
        const bool wasOnline = pu->online;
        const bool had = pu->sampled;
        const unsigned long long before = pu->lastMeso;
        pu->key = lu.key;
        pu->token = lu.token;
        pu->uid = lu.uid.empty() ? pu->uid : lu.uid;
        pu->charName = lu.charName;
        pu->deviceId = lu.deviceId;
        if (!lu.machine.empty()) pu->machine = lu.machine;
        pu->online = true;
        pu->lastSeenMs = nowMs;
        if (!had) {
            pu->lastMeso = lu.meso;
            pu->sampled = true;
            if (lu.hasWealth) {
                pu->lastScrolls = lu.wealth;
                pu->scrollsSampled = true;
            }
            continue;
        }
        const bool reconnect = !wasOnline;
        unsigned long long mag = 0;
        if (lu.meso >= before)
            mag = lu.meso - before;
        else
            mag = before - lu.meso;
        if (mag >= alertMin) {
            Delta d;
            d.key = lu.key;
            d.token = lu.personId;
            d.charName = lu.charName;
            d.before = before;
            d.after = lu.meso;
            d.mag = mag;
            d.down = lu.meso < before;
            d.reconnect = reconnect;
            deltas.push_back(std::move(d));
        }
        pu->lastMeso = lu.meso;
        pu->sampled = true;

        if (lu.hasWealth) {
            const bool hadSc = pu->scrollsSampled;
            const std::string beforeSc = pu->lastScrolls;
            pu->lastScrolls = lu.wealth;
            pu->scrollsSampled = true;
            if (hadSc) {
                WealthQtyMap beforeM, afterM;
                ParseWealthScrolls(beforeSc, beforeM);
                ParseWealthScrolls(lu.wealth, afterM);
                std::set<int> ids;
                for (const auto& x : beforeM) ids.insert(x.first);
                for (const auto& x : afterM) ids.insert(x.first);
                auto qtyOf = [](const WealthQtyMap& m, int id) -> unsigned long long {
                    const auto it = m.find(id);
                    return it == m.end() ? 0ull : it->second;
                };
                for (int id : ids) {
                    const unsigned long long b = qtyOf(beforeM, id);
                    const unsigned long long a = qtyOf(afterM, id);
                    if (a == b) continue;
                    ScrollDelta sd;
                    sd.token = lu.personId;
                    sd.charName = lu.charName;
                    sd.itemId = id;
                    sd.before = b;
                    sd.after = a;
                    sd.mag = a > b ? a - b : b - a;
                    sd.down = a < b;
                    sd.reconnect = reconnect;
                    scrollDeltas.push_back(std::move(sd));
                }
            }
        }
    }

    for (auto& u : st.mesoUnits) {
        if (live.find(u.key) == live.end()) u.online = false;
    }

    auto emit = [&](OpsState::MesoEvent ev) {
        ev.wallMs = nowMs;
        if (ev.mag == 0) {
            if (ev.after >= ev.before)
                ev.mag = ev.after - ev.before;
            else
                ev.mag = ev.before - ev.after;
        }
        MesoMarkSeriesAlert(st, ev.token, nowMs);
        if (!ev.peerToken.empty()) MesoMarkSeriesAlert(st, ev.peerToken, nowMs);
        st.mesoEvents.push_back(ev);
        MesoEventsAppend(st, ev);
    };

    for (size_t i = 0; i < deltas.size(); ++i) {
        if (!deltas[i].down || deltas[i].used) continue;
        for (size_t j = 0; j < deltas.size(); ++j) {
            if (i == j || deltas[j].used || deltas[j].down) continue;
            if (deltas[i].token != deltas[j].token) continue;
            if (!MesoAmtClose(deltas[i].mag, deltas[j].mag)) continue;
            OpsState::MesoEvent ev;
            ev.kind = "char_move";
            ev.token = deltas[i].token;
            ev.charName = deltas[i].charName;
            ev.peerToken = deltas[j].token;
            ev.peerChar = deltas[j].charName;
            ev.before = deltas[i].before;
            ev.after = deltas[i].after;
            ev.mag = deltas[i].mag;
            ev.note = "同号搬仓 " + deltas[i].charName + " → " + deltas[j].charName;
            emit(ev);
            deltas[i].used = true;
            deltas[j].used = true;
            break;
        }
    }
    for (size_t i = 0; i < deltas.size(); ++i) {
        if (!deltas[i].down || deltas[i].used) continue;
        for (size_t j = 0; j < deltas.size(); ++j) {
            if (i == j || deltas[j].used || deltas[j].down) continue;
            if (deltas[i].token == deltas[j].token) continue;
            if (!MesoAmtClose(deltas[i].mag, deltas[j].mag)) continue;
            OpsState::MesoEvent ev;
            ev.kind = "token_xfer";
            ev.token = deltas[i].token;
            ev.charName = deltas[i].charName;
            ev.peerToken = deltas[j].token;
            ev.peerChar = deltas[j].charName;
            ev.before = deltas[i].before;
            ev.after = deltas[i].after;
            ev.mag = deltas[i].mag;
            ev.note = "跨号 " + deltas[i].token + " → " + deltas[j].token;
            emit(ev);
            deltas[i].used = true;
            deltas[j].used = true;
            break;
        }
    }

    int nOut = 0;
    std::string outHint;
    for (auto& d : deltas) {
        if (d.used) continue;
        OpsState::MesoEvent ev;
        ev.token = d.token;
        ev.charName = d.charName;
        ev.before = d.before;
        ev.after = d.after;
        ev.mag = d.mag;
        if (d.down) {
            ev.kind = d.reconnect ? "reconnect_drop" : "outflow";
            ev.note = d.reconnect ? "离线后回来骤降，无对端进账" : "下降且无对端进账";
            ++nOut;
            if (outHint.empty()) outHint = d.token;
        } else {
            ev.kind = "inflow";
            ev.note = "进账";
        }
        emit(ev);
        d.used = true;
    }

    for (size_t i = 0; i < scrollDeltas.size(); ++i) {
        if (!scrollDeltas[i].down || scrollDeltas[i].used) continue;
        for (size_t j = 0; j < scrollDeltas.size(); ++j) {
            if (i == j || scrollDeltas[j].used || scrollDeltas[j].down) continue;
            if (scrollDeltas[i].itemId != scrollDeltas[j].itemId) continue;
            if (scrollDeltas[i].mag != scrollDeltas[j].mag) continue;
            if (scrollDeltas[i].token != scrollDeltas[j].token) continue;
            OpsState::MesoEvent ev;
            ev.kind = "scroll_move";
            ev.token = scrollDeltas[i].token;
            ev.charName = scrollDeltas[i].charName;
            ev.peerToken = scrollDeltas[j].token;
            ev.peerChar = scrollDeltas[j].charName;
            ev.before = scrollDeltas[i].before;
            ev.after = scrollDeltas[i].after;
            ev.mag = scrollDeltas[i].mag;
            ev.note = ScrollItemLabel(st, scrollDeltas[i].itemId) + " 同号 " +
                      scrollDeltas[i].charName + " → " + scrollDeltas[j].charName;
            emit(ev);
            scrollDeltas[i].used = true;
            scrollDeltas[j].used = true;
            break;
        }
    }
    for (size_t i = 0; i < scrollDeltas.size(); ++i) {
        if (!scrollDeltas[i].down || scrollDeltas[i].used) continue;
        for (size_t j = 0; j < scrollDeltas.size(); ++j) {
            if (i == j || scrollDeltas[j].used || scrollDeltas[j].down) continue;
            if (scrollDeltas[i].itemId != scrollDeltas[j].itemId) continue;
            if (scrollDeltas[i].mag != scrollDeltas[j].mag) continue;
            if (scrollDeltas[i].token == scrollDeltas[j].token) continue;
            OpsState::MesoEvent ev;
            ev.kind = "scroll_xfer";
            ev.token = scrollDeltas[i].token;
            ev.charName = scrollDeltas[i].charName;
            ev.peerToken = scrollDeltas[j].token;
            ev.peerChar = scrollDeltas[j].charName;
            ev.before = scrollDeltas[i].before;
            ev.after = scrollDeltas[i].after;
            ev.mag = scrollDeltas[i].mag;
            ev.note = ScrollItemLabel(st, scrollDeltas[i].itemId) + " 跨号 " +
                      scrollDeltas[i].token + " → " + scrollDeltas[j].token;
            emit(ev);
            scrollDeltas[i].used = true;
            scrollDeltas[j].used = true;
            break;
        }
    }

    int nScrollOut = 0;
    std::string scrollHint;
    for (auto& d : scrollDeltas) {
        if (d.used) continue;
        OpsState::MesoEvent ev;
        ev.token = d.token;
        ev.charName = d.charName;
        ev.before = d.before;
        ev.after = d.after;
        ev.mag = d.mag;
        const std::string item = ScrollItemLabel(st, d.itemId);
        if (d.down) {
            ev.kind = d.reconnect ? "scroll_reconnect" : "scroll_outflow";
            ev.note = d.reconnect ? (item + " 离线后骤降，无对端进账")
                                  : (item + " 减少且无对端进账");
            ++nScrollOut;
            if (scrollHint.empty()) scrollHint = d.token;
        } else {
            ev.kind = "scroll_inflow";
            ev.note = item + " 增加";
        }
        emit(ev);
        d.used = true;
    }

    if (nOut > 0 || nScrollOut > 0) {
        std::string msg;
        if (nOut > 0)
            msg += "金币外转 " + std::to_string(nOut) + " 笔 · " + outHint;
        if (nScrollOut > 0) {
            if (!msg.empty()) msg += "；";
            msg += "卷轴外转 " + std::to_string(nScrollOut) + " 笔 · " + scrollHint;
        }
        SetStatus(st, msg + "（利润监控流水已记）");
    }

    while (!st.mesoEvents.empty() &&
           (st.mesoEvents.size() > 4000 ||
            (st.mesoEvents.front().wallMs + kMesoEventKeepMs < nowMs))) {
        st.mesoEvents.pop_front();
    }

    for (auto& s : st.mesoDashSeries) {
        s.online = false;
        s.sessions = 0;
    }

    auto addMeso = [](unsigned long long& acc, unsigned long long v) {
        if (acc > (std::numeric_limits<unsigned long long>::max)() - v)
            acc = (std::numeric_limits<unsigned long long>::max)();
        else
            acc += v;
    };

    // 折线用角色底账上次探活值（含当前不在线）。更新服务关服/重启时名单会空一截，
    // 不能按「此刻还在 clients 里的人」加总，否则会整图悬崖再弹回来。
    std::unordered_map<std::string, std::pair<unsigned long long, int>> byTok;
    std::unordered_map<std::string, std::set<std::string>> tokChars;
    std::unordered_map<std::string, int> tokLiveSessions;
    std::unordered_map<std::string, std::string> tokUid;
    unsigned long long total = 0;
    for (const auto& u : st.mesoUnits) {
        if (!u.sampled) continue;
        const std::string pid = MesoPersonId(u.uid, u.token);
        if (pid.empty()) continue;
        auto& acc = byTok[pid];
        addMeso(acc.first, u.lastMeso);
        addMeso(total, u.lastMeso);
        if (!u.charName.empty()) tokChars[pid].insert(u.charName);
        if (!u.uid.empty()) tokUid[pid] = u.uid;
    }
    for (const auto& kv : live) tokLiveSessions[kv.second.personId] += kv.second.sessions;

    for (auto& kv : byTok) {
        OpsState::MesoDashSeries* ser = MesoDashFindSeries(st, kv.first);
        if (!ser) {
            OpsState::MesoDashSeries neu;
            neu.token = kv.first;
            neu.visible = true;
            st.mesoDashSeries.push_back(std::move(neu));
            ser = &st.mesoDashSeries.back();
        }
        ser->uid = tokUid[kv.first];
        ser->sessions = tokLiveSessions[kv.first];
        ser->online = ser->sessions > 0;
        ser->lastMeso = kv.second.first;
        ser->chars.clear();
        int n = 0;
        for (const auto& name : tokChars[kv.first]) {
            if (n > 0) ser->chars += "/";
            ser->chars += name;
            if (++n >= 3) {
                if (static_cast<int>(tokChars[kv.first].size()) > n) ser->chars += "…";
                break;
            }
        }
        MesoDashPush(ser->points, nowMs, ser->lastMeso);
        MesoDashPrune(ser->points, cutMs);
    }
    for (auto& s : st.mesoDashSeries) {
        if (byTok.find(s.token) == byTok.end()) {
            s.lastMeso = 0;
            s.online = false;
            s.sessions = 0;
            s.chars.clear();
        }
    }

    MesoDashPush(st.mesoDashTotal, nowMs, total);
    MesoDashPrune(st.mesoDashTotal, cutMs);

    st.mesoDashSeries.erase(
        std::remove_if(st.mesoDashSeries.begin(), st.mesoDashSeries.end(),
                       [&](const OpsState::MesoDashSeries& s) {
                           return !s.online && s.points.empty();
                       }),
        st.mesoDashSeries.end());
    for (auto& s : st.mesoDashSeries) MesoDashPrune(s.points, cutMs);

    st.mesoUnits.erase(std::remove_if(st.mesoUnits.begin(), st.mesoUnits.end(),
                                      [&](const OpsState::MesoUnit& u) {
                                          return !u.online && u.lastSeenMs < cutMs;
                                      }),
                       st.mesoUnits.end());

    MesoDashAppendFile(st, nowMs, total);
    MesoUnitsSave(st);
}

// 客户端上报 "0.1.149 build 149"；latest.json 的 version 是 "0.1.149"。
// 只认 "build N"，避免把 0.1.149 的尾数当 build。
uint32_t ParseAppVersionBuildId(const std::string& ver) {
    for (size_t i = 0; i + 5 <= ver.size(); ++i) {
        auto low = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        if (low(ver[i]) != 'b' || low(ver[i + 1]) != 'u' || low(ver[i + 2]) != 'i' ||
            low(ver[i + 3]) != 'l' || low(ver[i + 4]) != 'd')
            continue;
        size_t j = i + 5;
        while (j < ver.size() && (ver[j] == ' ' || ver[j] == '\t')) ++j;
        if (j >= ver.size() || ver[j] < '0' || ver[j] > '9') continue;
        char* end = nullptr;
        const unsigned long v = std::strtoul(ver.c_str() + j, &end, 10);
        if (end != ver.c_str() + j && v > 0 && v <= UINT32_MAX) return static_cast<uint32_t>(v);
    }
    return 0;
}

bool ClientVersionIsCurrent(const OpsState& st, const std::string& ver) {
    if (ver.empty() || st.latestClientVersionText.empty()) return true;
    const uint32_t bid = ParseAppVersionBuildId(ver);
    if (bid > 0 && st.latestClientBuildId > 0) return bid >= st.latestClientBuildId;
    if (ver == st.latestClientVersionText) return true;
    return ver.find(st.latestClientVersionText) != std::string::npos;
}

ImVec4 AppVersionColor(const OpsState& st, const std::string& ver) {
    if (ver.empty()) return OpsTone::Muted();
    if (st.latestClientVersionText.empty() && st.latestClientBuildId == 0)
        return ImVec4(0.78f, 0.80f, 0.84f, 1.f);
    if (ClientVersionIsCurrent(st, ver)) return OpsTone::Ok();
    return OpsTone::Warn();
}

int JsonIntField(const std::string& obj, const char* key, int fallback = 0) {
    const std::string raw = FindJsonNumber(obj, key);
    if (raw.empty()) return fallback;
    return static_cast<int>(std::strtol(raw.c_str(), nullptr, 10));
}

long long JsonInt64Field(const std::string& obj, const char* key, long long fallback = 0) {
    const std::string raw = FindJsonNumber(obj, key);
    if (raw.empty()) return fallback;
    return std::strtoll(raw.c_str(), nullptr, 10);
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
    st.clientsStrictToken = body.find("\"strictToken\":true") != std::string::npos;

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
        row.uid = FindJsonString(obj, "uid");
        row.gateExp = JsonInt64Field(obj, "gateExp", 0);
        row.appVersion = FindJsonString(obj, "appVersion");
        row.charName = FindJsonString(obj, "charName");
        row.charJobName = FindJsonString(obj, "charJobName");
        row.charMeso = FindJsonString(obj, "charMeso");
        row.wealthScrolls = FindJsonString(obj, "wealthScrolls");
        row.hasWealthScrolls = obj.find("\"hasWealthScrolls\":true") != std::string::npos;
        row.charLevel = JsonIntField(obj, "charLevel", 0);
        row.charJob = JsonIntField(obj, "charJob", 0);
        row.mapId = static_cast<uint32_t>(JsonIntField(obj, "mapId", 0));
        row.mapName = FindJsonString(obj, "mapName");
        row.channelId = JsonIntField(obj, "channelId", 0);
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
        {
            const std::string lf = FindJsonObjectSlice(obj, "logFetch");
            if (!lf.empty()) {
                row.logFetchId = FindJsonString(lf, "id");
                row.logFetchMode = FindJsonString(lf, "mode");
                row.logFetchStatus = FindJsonString(lf, "status");
            }
            const std::string ft = FindJsonObjectSlice(obj, "forceTarget");
            if (!ft.empty()) {
                row.forceTargetId = FindJsonString(ft, "id");
                row.forceTargetStatus = FindJsonString(ft, "status");
                row.forceTargetBuildId =
                    static_cast<uint32_t>(JsonIntField(ft, "buildId", 0));
            }
        }
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

bool ParseUpdateChannelsPayload(const std::string& body, OpsState& st) {
    st.updatePackages.clear();
    st.updateChannelGroups.clear();
    st.updateChannelTokens.clear();
    st.updateChannelsConfigured = body.find("\"configured\":true") != std::string::npos;
    st.allowedClientBuildId = static_cast<uint32_t>(JsonIntField(body, "defaultBuildId", 0));
    st.allowedClientVersionText.clear();
    st.lastBuiltClientBuildId = 0;
    st.lastBuiltClientVersionText.clear();

    auto forEachObj = [](const std::string& text, const char* key, auto&& onObj) {
        const size_t arrKey = text.find(std::string("\"") + key + "\"");
        if (arrKey == std::string::npos) return;
        size_t i = text.find('[', arrKey);
        if (i == std::string::npos) return;
        ++i;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                       text[i] == '\n' || text[i] == ',')) {
                ++i;
            }
            if (i >= text.size() || text[i] == ']') break;
            if (text[i] != '{') break;
            const size_t start = i;
            int depth = 0;
            bool inStr = false;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (inStr) {
                    if (c == '\\' && i + 1 < text.size()) {
                        ++i;
                        continue;
                    }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') inStr = true;
                else if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        onObj(text.substr(start, i - start));
                        break;
                    }
                }
            }
        }
    };

    const std::string defObj = FindJsonObjectSlice(body, "default");
    if (!defObj.empty()) {
        st.allowedClientBuildId =
            static_cast<uint32_t>(JsonIntField(defObj, "buildId", (int)st.allowedClientBuildId));
        st.allowedClientVersionText = FindJsonString(defObj, "version");
    }
    const std::string lastObj = FindJsonObjectSlice(body, "lastBuilt");
    if (!lastObj.empty()) {
        st.lastBuiltClientBuildId = static_cast<uint32_t>(JsonIntField(lastObj, "buildId", 0));
        st.lastBuiltClientVersionText = FindJsonString(lastObj, "version");
    }

    forEachObj(body, "packages", [&](const std::string& obj) {
        OpsState::UpdatePackageRow row;
        row.buildId = static_cast<uint32_t>(JsonIntField(obj, "buildId", 0));
        row.version = FindJsonString(obj, "version");
        row.zipName = FindJsonString(obj, "zipName");
        if (row.buildId > 0) st.updatePackages.push_back(std::move(row));
    });
    forEachObj(body, "groups", [&](const std::string& obj) {
        OpsState::UpdateChannelOverride row;
        row.uid = FindJsonString(obj, "uid");
        row.buildId = static_cast<uint32_t>(JsonIntField(obj, "buildId", 0));
        row.version = FindJsonString(obj, "version");
        if (!row.uid.empty() && row.buildId > 0) st.updateChannelGroups.push_back(std::move(row));
    });
    forEachObj(body, "tokens", [&](const std::string& obj) {
        OpsState::UpdateChannelOverride row;
        row.token = FindJsonString(obj, "token");
        row.buildId = static_cast<uint32_t>(JsonIntField(obj, "buildId", 0));
        row.version = FindJsonString(obj, "version");
        if (!row.token.empty() && row.buildId > 0) st.updateChannelTokens.push_back(std::move(row));
    });

    st.updateChannelCombo = -1;
    for (int i = 0; i < (int)st.updatePackages.size(); ++i) {
        if (st.updatePackages[(size_t)i].buildId == st.allowedClientBuildId) {
            st.updateChannelCombo = i;
            break;
        }
    }
    return true;
}

void RefreshUpdateChannels(OpsState& st, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastUpdateChannelsFetchMs != 0 && now - st.lastUpdateChannelsFetchMs < 4000) {
        return;
    }
    st.lastUpdateChannelsFetchMs = now;
    if (!TwmsRunning(st)) {
        st.updateChannelsError = "TWMS API 未运行";
        return;
    }
    const auto r = HttpGet(L"127.0.0.1", 18789, L"/twms/admin/update-channels", 2000, 512 * 1024);
    if (!r.ok) {
        st.updatePackages.clear();
        if (r.status == 404)
            st.updateChannelsError = "接口不存在：请重启 TWMS 更新服务（需含 update-channels）";
        else
            st.updateChannelsError = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        return;
    }
    if (!ParseUpdateChannelsPayload(r.body, st)) {
        st.updateChannelsError = "解析 update-channels 失败";
        return;
    }
    st.updateChannelsError.clear();
}

bool PostUpdateChannelDefault(OpsState& st, uint32_t buildId, std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    if (buildId == 0) {
        err = "未选版本";
        return false;
    }
    const std::string body = std::string("{\"action\":\"set-default\",\"buildId\":") +
                             std::to_string(buildId) + "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/update-channels", body.c_str(), 2500, 256 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "设置对外允许版本失败";
        return false;
    }
    ParseUpdateChannelsPayload(r.body, st);
    return true;
}

bool PostUpdateChannelGroup(OpsState& st, const char* uid, const char* token, uint32_t buildId,
                            bool clear, std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    std::string body = "{\"action\":\"";
    body += clear ? "clear-group" : "set-group";
    body += "\"";
    if (uid && uid[0]) body += ",\"uid\":\"" + JsonEscapeLocal(uid) + "\"";
    if (token && token[0]) body += ",\"token\":\"" + JsonEscapeLocal(token) + "\"";
    if (!clear) body += ",\"buildId\":" + std::to_string(buildId);
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/update-channels", body.c_str(), 2500, 256 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "设置分组允许版本失败";
        return false;
    }
    ParseUpdateChannelsPayload(r.body, st);
    return true;
}

uint32_t AllowedBuildForPerson(const OpsState& st, const std::string& personKey) {
    if (personKey.size() >= 2 && personKey[1] == '\x1f') {
        const char* who = personKey.c_str() + 2;
        if (personKey[0] == 'u') {
            for (const auto& g : st.updateChannelGroups) {
                if (g.uid == who) return g.buildId;
            }
        } else if (personKey[0] == 't') {
            for (const auto& g : st.updateChannelTokens) {
                if (g.token == who) return g.buildId;
            }
        }
    }
    return st.allowedClientBuildId;
}

unsigned long long MesoDashParseUll(const std::string& raw) {
    if (raw.empty()) return 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') return 0;
    return v;
}

OpsState::MesoDashSeries* MesoDashFindOrAdd(OpsState& st, const std::string& token) {
    for (auto& s : st.mesoDashSeries) {
        if (s.token == token) return &s;
    }
    OpsState::MesoDashSeries neu;
    neu.token = token;
    neu.visible = true;
    st.mesoDashSeries.push_back(std::move(neu));
    return &st.mesoDashSeries.back();
}

void MesoDashParseSeriesArray(OpsState& st, const std::string& line, ULONGLONG t) {
    const size_t arrKey = line.find("\"s\":");
    if (arrKey == std::string::npos) return;
    size_t i = line.find('[', arrKey);
    if (i == std::string::npos) return;
    ++i;
    while (i < line.size()) {
        while (i < line.size() &&
               (line[i] == ' ' || line[i] == '\t' || line[i] == ',' || line[i] == '\r'))
            ++i;
        if (i >= line.size() || line[i] == ']') break;
        if (line[i] != '{') break;
        int depth = 0;
        const size_t start = i;
        for (; i < line.size(); ++i) {
            if (line[i] == '{') ++depth;
            else if (line[i] == '}') {
                --depth;
                if (depth == 0) {
                    ++i;
                    break;
                }
            } else if (line[i] == '"') {
                ++i;
                while (i < line.size() && line[i] != '"') {
                    if (line[i] == '\\' && i + 1 < line.size()) i += 2;
                    else ++i;
                }
            }
        }
        const std::string obj = line.substr(start, i - start);
        const std::string token = FindJsonString(obj, "k");
        if (token.empty()) continue;
        const unsigned long long meso = MesoDashParseUll(FindJsonNumber(obj, "m"));
        auto* ser = MesoDashFindOrAdd(st, token);
        const std::string uid = FindJsonString(obj, "i");
        if (!uid.empty()) ser->uid = uid;
        MesoDashPush(ser->points, t, meso);
        ser->lastMeso = meso;
    }
}

void MesoDashRewriteFile(OpsState& st, ULONGLONG cutMs) {
    if (st.repoRoot.empty()) return;
    const std::wstring path = OpsLogMesoDash(st.repoRoot);
    const std::wstring tmp = path + L".tmp";
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return;
    std::ofstream out(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    if (!out) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '{') continue;
        const ULONGLONG t = MesoDashParseUll(FindJsonNumber(line, "t"));
        if (t < cutMs) continue;
        out << line << '\n';
    }
    out.close();
    in.close();
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
    if (ec) {
        std::filesystem::remove(std::filesystem::path(path), ec);
        std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
    }
}

void MesoDashLoadFile(OpsState& st) {
    if (st.repoRoot.empty()) return;
    const std::wstring path = OpsLogMesoDash(st.repoRoot);
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return;
    const ULONGLONG nowMs = MesoDashWallMs();
    const ULONGLONG cutMs = nowMs > kMesoDashKeepMs ? nowMs - kMesoDashKeepMs : 0;
    bool needCompact = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '{') continue;
        const ULONGLONG t = MesoDashParseUll(FindJsonNumber(line, "t"));
        if (t == 0) continue;
        if (t < cutMs) {
            needCompact = true;
            continue;
        }
        const unsigned long long tot = MesoDashParseUll(FindJsonNumber(line, "tot"));
        MesoDashPush(st.mesoDashTotal, t, tot);
        MesoDashParseSeriesArray(st, line, t);
    }
    in.close();
    MesoDashPrune(st.mesoDashTotal, cutMs);
    for (auto& s : st.mesoDashSeries) {
        MesoDashPrune(s.points, cutMs);
        if (!s.points.empty()) s.lastMeso = s.points.back().meso;
        s.online = false;
    }
    if (needCompact) MesoDashRewriteFile(st, cutMs);
    MesoUnitsLoad(st);
    MesoEventsLoad(st);
    MesoMergeUidTokenAliases(st);
}

void MesoDashAppendFile(OpsState& st, ULONGLONG nowMs, unsigned long long total) {
    if (st.repoRoot.empty()) return;
    EnsureDirs(st.repoRoot);
    const std::wstring path = OpsLogMesoDash(st.repoRoot);
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::app);
    if (!f) return;
    std::string line = "{\"t\":" + std::to_string(nowMs) + ",\"tot\":" + std::to_string(total) +
                       ",\"s\":[";
    bool first = true;
    for (const auto& s : st.mesoDashSeries) {
        if (!s.online) continue;
        if (!first) line += ',';
        first = false;
        line += "{\"k\":\"";
        line += JsonEscapeLocal(s.token);
        line += "\",\"i\":\"";
        line += JsonEscapeLocal(s.uid);
        line += "\",\"m\":";
        line += std::to_string(s.lastMeso);
        line += '}';
    }
    line += "]}\n";
    f << line;
}

void MesoDashClearFile(OpsState& st) {
    if (st.repoRoot.empty()) return;
    std::ofstream f(std::filesystem::path(OpsLogMesoDash(st.repoRoot)),
                    std::ios::binary | std::ios::trunc);
}

void MesoEventsAppend(OpsState& st, const OpsState::MesoEvent& ev) {
    if (st.repoRoot.empty()) return;
    EnsureDirs(st.repoRoot);
    std::ofstream f(std::filesystem::path(OpsLogMesoEvents(st.repoRoot)),
                    std::ios::binary | std::ios::app);
    if (!f) return;
    f << "{\"t\":" << ev.wallMs << ",\"kind\":\"" << JsonEscapeLocal(ev.kind) << "\",\"k\":\""
      << JsonEscapeLocal(ev.token) << "\",\"c\":\"" << JsonEscapeLocal(ev.charName)
      << "\",\"pk\":\"" << JsonEscapeLocal(ev.peerToken) << "\",\"pc\":\""
      << JsonEscapeLocal(ev.peerChar) << "\",\"b\":" << ev.before << ",\"a\":" << ev.after
      << ",\"m\":" << ev.mag << ",\"n\":\"" << JsonEscapeLocal(ev.note) << "\"}\n";
}

void MesoEventsLoad(OpsState& st) {
    if (st.repoRoot.empty()) return;
    const std::wstring path = OpsLogMesoEvents(st.repoRoot);
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return;
    const ULONGLONG nowMs = MesoDashWallMs();
    const ULONGLONG cutMs = nowMs > kMesoEventKeepMs ? nowMs - kMesoEventKeepMs : 0;
    bool needCompact = false;
    std::string line;
    std::deque<OpsState::MesoEvent> kept;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '{') continue;
        const ULONGLONG t = MesoDashParseUll(FindJsonNumber(line, "t"));
        if (t == 0) continue;
        if (t < cutMs) {
            needCompact = true;
            continue;
        }
        OpsState::MesoEvent ev;
        ev.wallMs = t;
        ev.kind = FindJsonString(line, "kind");
        ev.token = FindJsonString(line, "k");
        ev.charName = FindJsonString(line, "c");
        ev.peerToken = FindJsonString(line, "pk");
        ev.peerChar = FindJsonString(line, "pc");
        ev.before = MesoDashParseUll(FindJsonNumber(line, "b"));
        ev.after = MesoDashParseUll(FindJsonNumber(line, "a"));
        ev.mag = MesoDashParseUll(FindJsonNumber(line, "m"));
        ev.note = FindJsonString(line, "n");
        if (ev.kind.empty() || ev.token.empty()) continue;
        kept.push_back(std::move(ev));
        if (MesoKindAlert(kept.back().kind)) MesoMarkSeriesAlert(st, kept.back().token, t);
    }
    in.close();
    st.mesoEvents = std::move(kept);
    if (needCompact) {
        const std::wstring tmp = path + L".tmp";
        std::ofstream out(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (out) {
            for (const auto& ev : st.mesoEvents) {
                out << "{\"t\":" << ev.wallMs << ",\"kind\":\"" << JsonEscapeLocal(ev.kind)
                    << "\",\"k\":\"" << JsonEscapeLocal(ev.token) << "\",\"c\":\""
                    << JsonEscapeLocal(ev.charName) << "\",\"pk\":\""
                    << JsonEscapeLocal(ev.peerToken) << "\",\"pc\":\""
                    << JsonEscapeLocal(ev.peerChar) << "\",\"b\":" << ev.before
                    << ",\"a\":" << ev.after << ",\"m\":" << ev.mag << ",\"n\":\""
                    << JsonEscapeLocal(ev.note) << "\"}\n";
            }
            out.close();
            std::error_code ec;
            std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
            if (ec) {
                std::filesystem::remove(std::filesystem::path(path), ec);
                std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
            }
        }
    }
}

void MesoUnitsSave(OpsState& st) {
    if (st.repoRoot.empty()) return;
    EnsureDirs(st.repoRoot);
    std::ofstream f(std::filesystem::path(OpsLogMesoUnits(st.repoRoot)),
                    std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\"t\":" << MesoDashWallMs() << ",\"u\":[";
    bool first = true;
    for (const auto& u : st.mesoUnits) {
        if (!u.sampled) continue;
        if (!first) f << ',';
        first = false;
        f << "{\"k\":\"" << JsonEscapeLocal(u.token) << "\",\"i\":\""
          << JsonEscapeLocal(u.uid) << "\",\"c\":\""
          << JsonEscapeLocal(u.charName) << "\",\"d\":\"" << JsonEscapeLocal(u.deviceId)
          << "\",\"h\":\"" << JsonEscapeLocal(u.machine) << "\",\"m\":" << u.lastMeso
          << ",\"w\":\"" << JsonEscapeLocal(u.lastScrolls) << "\",\"ws\":"
          << (u.scrollsSampled ? 1 : 0) << ",\"s\":" << u.lastSeenMs << '}';
    }
    f << "]}\n";
}

void MesoUnitsLoad(OpsState& st) {
    if (st.repoRoot.empty()) return;
    std::ifstream in(std::filesystem::path(OpsLogMesoUnits(st.repoRoot)), std::ios::binary);
    if (!in) return;
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (body.empty()) return;
    const ULONGLONG nowMs = MesoDashWallMs();
    const ULONGLONG cutMs = nowMs > kMesoDashKeepMs ? nowMs - kMesoDashKeepMs : 0;
    size_t i = body.find("\"u\":");
    if (i == std::string::npos) return;
    i = body.find('[', i);
    if (i == std::string::npos) return;
    ++i;
    while (i < body.size()) {
        while (i < body.size() &&
               (body[i] == ' ' || body[i] == '\t' || body[i] == ',' || body[i] == '\r' ||
                body[i] == '\n'))
            ++i;
        if (i >= body.size() || body[i] == ']') break;
        if (body[i] != '{') break;
        int depth = 0;
        const size_t start = i;
        for (; i < body.size(); ++i) {
            if (body[i] == '{') ++depth;
            else if (body[i] == '}') {
                --depth;
                if (depth == 0) {
                    ++i;
                    break;
                }
            } else if (body[i] == '"') {
                ++i;
                while (i < body.size() && body[i] != '"') {
                    if (body[i] == '\\' && i + 1 < body.size()) i += 2;
                    else ++i;
                }
            }
        }
        const std::string obj = body.substr(start, i - start);
        OpsState::MesoUnit u;
        u.token = FindJsonString(obj, "k");
        u.uid = FindJsonString(obj, "i");
        u.charName = FindJsonString(obj, "c");
        u.deviceId = FindJsonString(obj, "d");
        u.machine = FindJsonString(obj, "h");
        u.lastMeso = MesoDashParseUll(FindJsonNumber(obj, "m"));
        u.lastScrolls = FindJsonString(obj, "w");
        u.scrollsSampled = JsonIntField(obj, "ws", u.lastScrolls.empty() ? 0 : 1) != 0;
        u.lastSeenMs = MesoDashParseUll(FindJsonNumber(obj, "s"));
        const std::string pid = MesoPersonId(u.uid, u.token);
        if (pid.empty()) continue;
        if (u.lastSeenMs != 0 && u.lastSeenMs < cutMs) continue;
        u.key = MesoUnitKey(pid, u.charName, u.deviceId);
        u.sampled = true;
        u.online = false;
        bool exists = false;
        for (const auto& x : st.mesoUnits) {
            if (x.key == u.key) {
                exists = true;
                break;
            }
        }
        if (!exists) st.mesoUnits.push_back(std::move(u));
    }
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

    // 卡级吊销名单：只取每条的 jti，够用来给台账打「卡已废」。
    // 用带右引号的键名精确定位，免得撞上同前缀的 "revokedJtiCount"。
    st.revokedJti.clear();
    {
        const size_t arrKey = body.find("\"revokedJti\"");
        if (arrKey != std::string::npos) {
            const size_t lb = body.find('[', arrKey);
            const size_t rb = (lb == std::string::npos) ? std::string::npos : body.find(']', lb);
            if (lb != std::string::npos && rb != std::string::npos) {
                const std::string arr = body.substr(lb, rb - lb + 1);
                size_t p = 0;
                while ((p = arr.find("\"jti\"", p)) != std::string::npos) {
                    const size_t q1 = arr.find('"', arr.find(':', p) + 1);
                    if (q1 == std::string::npos) break;
                    const size_t q2 = arr.find('"', q1 + 1);
                    if (q2 == std::string::npos) break;
                    const std::string jti = arr.substr(q1 + 1, q2 - q1 - 1);
                    if (!jti.empty()) st.revokedJti.insert(jti);
                    p = q2 + 1;
                }
            }
        }
    }

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
                   const std::string& token = {}, const std::string& uid = {}) {
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
    if (!uid.empty()) body += ",\"uid\":\"" + JsonEscapeLocal(uid) + "\"";
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

// 严格模式开关（/twms/admin/access action=setstrict）：无有效签名 TOKEN 直接拒。
bool PostSetStrictToken(OpsState& st, bool strict, std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    const std::string body =
        std::string("{\"action\":\"setstrict\",\"strict\":") + (strict ? "true" : "false") + "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/access", body.c_str(), 2500, 64 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "严格模式切换失败";
        return false;
    }
    st.clientsStrictToken = r.body.find("\"strictToken\":true") != std::string::npos;
    return true;
}

bool ParseQuotaPayload(const std::string& body, OpsState& st) {
    st.quotaUsers.clear();
    st.quotaEnabled = body.find("\"enabled\":true") != std::string::npos;
    st.quotaDefaultMax = JsonIntField(body, "defaultMax", 0);
    st.quotaAgingDays = JsonIntField(body, "agingDays", 0);
    st.quotaPathText = FindJsonString(body, "path");

    // 从 text 的 "key":[ {..},{..} ] 逐个切出对象（按花括号深度，兼容嵌套 devices 数组）。
    auto forEachObj = [](const std::string& text, const char* key, auto&& onObj) {
        const size_t arrKey = text.find(std::string("\"") + key + "\"");
        if (arrKey == std::string::npos) return;
        size_t i = text.find('[', arrKey);
        if (i == std::string::npos) return;
        ++i;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                       text[i] == '\n' || text[i] == ',')) {
                ++i;
            }
            if (i >= text.size() || text[i] == ']') break;
            if (text[i] != '{') break;
            const size_t start = i;
            int depth = 0;
            bool inStr = false;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (inStr) {
                    if (c == '\\' && i + 1 < text.size()) {
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
                if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
            if (depth != 0) break;
            onObj(text.substr(start, i - start));
        }
    };

    forEachObj(body, "users", [&](const std::string& uobj) {
        OpsState::QuotaUserRow u;
        u.uid = FindJsonString(uobj, "uid");
        if (u.uid.empty()) return;
        u.max = JsonIntField(uobj, "max", 0);
        u.effectiveMax = JsonIntField(uobj, "effectiveMax", 0);
        u.used = JsonIntField(uobj, "used", 0);
        std::snprintf(u.maxInput, sizeof(u.maxInput), "%d", u.max);
        forEachObj(uobj, "devices", [&](const std::string& dobj) {
            OpsState::QuotaDeviceRow d;
            d.deviceId = FindJsonString(dobj, "deviceId");
            d.lastSeen = FindJsonString(dobj, "lastSeen");
            d.idleSec = JsonInt64Field(dobj, "idleSec", -1);
            if (!d.deviceId.empty()) u.devices.push_back(std::move(d));
        });
        // 最久没见的排最上：清僵尸名额时省得翻。
        std::stable_sort(u.devices.begin(), u.devices.end(),
                         [](const OpsState::QuotaDeviceRow& a, const OpsState::QuotaDeviceRow& b) {
                             return a.idleSec > b.idleSec;
                         });
        st.quotaUsers.push_back(std::move(u));
    });
    return body.find("\"ok\":true") != std::string::npos ||
           body.find("\"enabled\"") != std::string::npos;
}

void RefreshQuota(OpsState& st, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastQuotaFetchMs != 0 && now - st.lastQuotaFetchMs < 5000) return;
    st.lastQuotaFetchMs = now;

    if (!TwmsRunning(st)) {
        st.quotaUsers.clear();
        st.quotaError = "TWMS API 未运行";
        return;
    }
    const auto r = HttpGet(L"127.0.0.1", 18789, L"/twms/admin/quota", 1500, 512 * 1024);
    if (!r.ok) {
        st.quotaUsers.clear();
        if (r.status == 404) {
            st.quotaError = "接口不存在：请重启 TWMS 更新服务以加载配额能力";
        } else if (!r.error.empty()) {
            st.quotaError = r.error;
        } else {
            st.quotaError = "HTTP " + std::to_string(r.status);
        }
        return;
    }
    if (!ParseQuotaPayload(r.body, st)) {
        st.quotaError = "解析 quota 响应失败";
        return;
    }
    st.quotaError.clear();
}

bool PostQuotaAction(OpsState& st, const std::string& body, std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/quota", body.c_str(), 2500, 512 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "配额操作失败";
        return false;
    }
    ParseQuotaPayload(r.body, st);
    return true;
}

bool PostQuotaSetMax(OpsState& st, const std::string& uid, int maxVal, std::string& err) {
    if (uid.empty()) {
        err = "需要 uid";
        return false;
    }
    if (maxVal < 0) maxVal = 0;
    const std::string body = "{\"action\":\"setMax\",\"uid\":\"" + JsonEscapeLocal(uid) +
                             "\",\"max\":" + std::to_string(maxVal) + "}";
    return PostQuotaAction(st, body, err);
}

bool PostQuotaRemoveDevice(OpsState& st, const std::string& uid, const std::string& deviceId,
                           std::string& err) {
    const std::string body = "{\"action\":\"removeDevice\",\"uid\":\"" + JsonEscapeLocal(uid) +
                             "\",\"deviceId\":\"" + JsonEscapeLocal(deviceId) + "\"}";
    return PostQuotaAction(st, body, err);
}

// 批量释放闲置设备；uid 空=全部用户。服务端只删 idle>=days 的，坏时间戳不动。
bool PostQuotaReleaseIdle(OpsState& st, const std::string& uid, int days, std::string& err) {
    if (days <= 0) {
        err = "天数需大于 0";
        return false;
    }
    std::string body = "{\"action\":\"releaseIdle\",\"days\":" + std::to_string(days);
    if (!uid.empty()) body += ",\"uid\":\"" + JsonEscapeLocal(uid) + "\"";
    body += "}";
    return PostQuotaAction(st, body, err);
}

// 设备闲置文案：idleSec<0 表示台账时间戳坏了，别当成「刚见过」。
std::string QuotaIdleText(long long idleSec) {
    if (idleSec < 0) return "时间未知";
    if (idleSec < 3600) return std::to_string(idleSec / 60) + " 分钟前";
    if (idleSec < 86400) return std::to_string(idleSec / 3600) + " 小时前";
    return std::to_string(idleSec / 86400) + " 天前";
}

// 闲置越久越可能是「重装系统换了 deviceId」的僵尸名额：>=阈值标红，>=1/4 阈值标黄。
ImVec4 QuotaIdleColor(long long idleSec, int idleDaysThreshold) {
    if (idleSec < 0) return OpsTone::Warn();
    const long long days = idleSec / 86400;
    const long long th = idleDaysThreshold > 0 ? idleDaysThreshold : 30;
    if (days >= th) return OpsTone::Danger();
    if (days >= th / 4) return OpsTone::Warn();
    return OpsTone::Muted();
}

// 卡到期文案：exp<=0 永久；过期标红文案交给调用方，这里只给 "剩 N 天"/"已过期"/"永久"。
std::string GateExpRemainText(long long exp) {
    if (exp <= 0) return "永久";
    const long long now = static_cast<long long>(::time(nullptr));
    const long long left = exp - now;
    if (left <= 0) return "已过期";
    const long long days = left / 86400;
    if (days >= 1) return "剩 " + std::to_string(days) + " 天";
    const long long hours = left / 3600;
    return "剩 " + std::to_string(hours > 0 ? hours : 1) + " 小时";
}

// 运维台统一北京时间（GMT+8），不跟 Windows 时区走。内部时间戳仍是 UTC epoch。
bool BeijingTm(time_t utcSec, std::tm& out) {
    constexpr time_t kEightHours = 8 * 3600;
    if (utcSec > 0 && utcSec > (std::numeric_limits<time_t>::max)() - kEightHours)
        utcSec = (std::numeric_limits<time_t>::max)();
    else
        utcSec += kEightHours;
    return gmtime_s(&out, &utcSec) == 0;
}

std::string GateExpAbsText(long long exp) {
    if (exp <= 0) return "永不过期";
    const time_t t = static_cast<time_t>(exp);
    struct tm tmv{};
    if (!BeijingTm(t, tmv)) return "—";
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// 签卡：请求服务端用本机离线私钥签一张启动 TOKEN；成功把 token/信息写入 st，失败填 err。
bool PostGateSign(OpsState& st, const std::string& uid, int days, const std::string& note,
                  std::string& err) {
    if (uid.empty()) {
        err = "需要 uid";
        return false;
    }
    if (days < 0) days = 0;
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    std::string body = "{\"uid\":\"" + JsonEscapeLocal(uid) + "\",\"days\":" + std::to_string(days);
    if (!note.empty()) body += ",\"note\":\"" + JsonEscapeLocal(note) + "\"";
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/gate-sign", body.c_str(), 2500, 64 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务以加载签卡能力";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "签发失败";
        return false;
    }
    const std::string token = FindJsonString(r.body, "token");
    if (token.empty()) {
        err = "服务端未返回 token";
        return false;
    }
    long long exp = 0;
    if (const size_t p = r.body.find("\"exp\":"); p != std::string::npos) {
        exp = std::strtoll(r.body.c_str() + p + 6, nullptr, 10);
    }
    std::string expText;
    if (exp <= 0) {
        expText = "永不过期";
    } else {
        const time_t t = static_cast<time_t>(exp);
        struct tm tmv{};
        if (!BeijingTm(t, tmv)) {
            expText = "—";
        } else {
            char buf[32]{};
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
            expText = buf;
        }
    }
    st.gateSignToken = token;
    st.gateSignInfo = "uid=" + uid + " · 到期 " + expText;
    st.gateSignError.clear();
    return true;
}

void ParseGateCardsPayload(const std::string& body, OpsState& st) {
    st.gateCards.clear();
    const size_t arrKey = body.find("\"cards\"");
    if (arrKey == std::string::npos) return;
    size_t i = body.find('[', arrKey);
    if (i == std::string::npos) return;
    ++i;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' ||
                                   body[i] == '\n' || body[i] == ','))
            ++i;
        if (i >= body.size() || body[i] == ']') break;
        if (body[i] != '{') break;
        int depth = 0;
        const size_t start = i;
        for (; i < body.size(); ++i) {
            if (body[i] == '{') ++depth;
            else if (body[i] == '}') {
                --depth;
                if (depth == 0) {
                    ++i;
                    break;
                }
            }
        }
        const std::string obj = body.substr(start, i - start);
        OpsState::GateCardRow row;
        row.id = FindJsonString(obj, "id");
        row.jti = FindJsonString(obj, "jti");
        row.uid = FindJsonString(obj, "uid");
        row.iss = JsonInt64Field(obj, "iss", 0);
        row.exp = JsonInt64Field(obj, "exp", 0);
        row.days = JsonIntField(obj, "days", 0);
        row.note = FindJsonString(obj, "note");
        row.by = FindJsonString(obj, "by");
        row.at = FindJsonString(obj, "at");
        row.token = FindJsonString(obj, "token");
        if (!row.uid.empty()) st.gateCards.push_back(std::move(row));
    }
    // 交叉 access bans：uid 封禁键 = "uid:<uid>" → 标注已吊销
    for (auto& c : st.gateCards) {
        const std::string key = "uid:" + c.uid;
        c.banned = false;
        for (const auto& b : st.bans) {
            if (b.key == key) {
                c.banned = true;
                break;
            }
        }
        // 只认 jti 字段：老台账行的 id 是随机生成的、并没签进卡里，拿它去废是废不掉的，
        // 所以那些行不给「废卡」入口，只能按 uid 封整个人。
        c.cardRevoked = !c.jti.empty() && st.revokedJti.count(c.jti) > 0;
    }
    // 标注被取代的旧卡：服务端已按签发倒序返回（listGateCards 有 rows.reverse()），
    // 故同 uid 首次出现即最新一张，其后各行都是历史卡。
    {
        std::set<std::string> seenUid;
        for (auto& c : st.gateCards) c.superseded = !seenUid.insert(c.uid).second;
    }
}

void RefreshGateCards(OpsState& st, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastGateCardsFetchMs != 0 && now - st.lastGateCardsFetchMs < 4000) return;
    st.lastGateCardsFetchMs = now;
    if (!TwmsRunning(st)) {
        st.gateCards.clear();
        st.gateCardsError = "TWMS API 未运行";
        return;
    }
    RefreshBans(st, false);  // 交叉标注“已吊销”需要 bans
    const auto r = HttpGet(L"127.0.0.1", 18789, L"/twms/admin/cards", 1500, 1024 * 1024);
    if (!r.ok) {
        st.gateCards.clear();
        if (r.status == 404)
            st.gateCardsError = "接口不存在：请重启 TWMS 更新服务以加载台账";
        else
            st.gateCardsError = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        return;
    }
    ParseGateCardsPayload(r.body, st);
    st.gateCardsError.clear();
}

void ParseClientHistoryPayload(const std::string& body, OpsState& st) {
    st.clientHistory.clear();
    st.clientHistoryTotal = JsonIntField(body, "total", 0);
    const size_t arrKey = body.find("\"clients\"");
    if (arrKey == std::string::npos) return;
    size_t i = body.find('[', arrKey);
    if (i == std::string::npos) return;
    ++i;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' ||
                                   body[i] == '\n' || body[i] == ','))
            ++i;
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
        OpsState::ClientHistoryRow row;
        row.ip = FindJsonString(obj, "ip");
        row.machine = FindJsonString(obj, "machine");
        row.deviceId = FindJsonString(obj, "deviceId");
        row.uid = FindJsonString(obj, "uid");
        row.appVersion = FindJsonString(obj, "appVersion");
        row.charName = FindJsonString(obj, "charName");
        row.lastSeenAt = FindJsonString(obj, "lastSeenAt");
        row.lastAllowAt = FindJsonString(obj, "lastAllowAt");
        row.lastDenyReason = FindJsonString(obj, "lastDenyReason");
        row.lastDenyMatch = FindJsonString(obj, "lastDenyMatch");
        row.lastSeenSec = JsonInt64Field(obj, "lastSeenSec", 0);
        row.leaseRemainSec = JsonInt64Field(obj, "leaseRemainSec", 0);
        row.online = obj.find("\"online\":true") != std::string::npos;
        st.clientHistory.push_back(std::move(row));
    }
}

void RefreshClientHistory(OpsState& st, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && st.lastClientHistoryFetchMs != 0 && now - st.lastClientHistoryFetchMs < 10000)
        return;
    st.lastClientHistoryFetchMs = now;
    if (!TwmsRunning(st)) {
        st.clientHistory.clear();
        st.clientHistoryError = "TWMS API 未运行";
        return;
    }
    const std::wstring q = L"/twms/admin/client-history?days=" +
                           std::to_wstring(st.clientHistoryDays) + L"&limit=500";
    const auto r = HttpGet(L"127.0.0.1", 18789, q.c_str(), 2000, 1024 * 1024);
    if (!r.ok) {
        st.clientHistory.clear();
        if (r.status == 404)
            st.clientHistoryError = "接口不存在：请重启 TWMS 更新服务以加载历史台账";
        else
            st.clientHistoryError =
                !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        return;
    }
    ParseClientHistoryPayload(r.body, st);
    st.clientHistoryError.clear();
}

// 租约剩余的人话。客户端每 64h 必须成功探活一次续约，否则租约过期时若正赶上服务没开，
// 会被 gate/3 硬拒（1h 宽限是整机一次性的，老客户端早已用掉），表现为「莫名启动不了」。
std::string LeaseRemainText(long long sec) {
    if (sec <= 0) return "已过期";
    if (sec < 3600) return std::to_string(sec / 60) + " 分";
    const long long h = sec / 3600;
    if (h < 48) return std::to_string(h) + " 小时";
    return std::to_string(h / 24) + " 天 " + std::to_string(h % 24) + " 小时";
}

// 封人 = 对其 uid 加 uid 封禁：该 uid 名下所有卡一起拦（离线激活拦不住，联网探活即拦）。
bool PostRevokeUid(OpsState& st, const std::string& uid, std::string& err) {
    return PostBanAction(st, "ban", {}, {}, "ops 吊销卡", {}, err, {}, {}, {}, uid);
}
bool PostUnrevokeUid(OpsState& st, const std::string& uid, std::string& err) {
    return PostBanAction(st, "unban", {}, {}, {}, {}, err, {}, {}, {}, uid);
}

// 废单张卡 = 按卡号 jti 吊销：同 uid 的其他卡不受影响（续签后可只废泄露的那张）。
bool PostCardJtiAction(OpsState& st, const char* action, const std::string& jti,
                       const std::string& uid, const std::string& reason, std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    std::string body = std::string("{\"action\":\"") + action + "\",\"jti\":\"" +
                       JsonEscapeLocal(jti) + "\"";
    if (!uid.empty()) body += ",\"uid\":\"" + JsonEscapeLocal(uid) + "\"";
    if (!reason.empty()) body += ",\"reason\":\"" + JsonEscapeLocal(reason) + "\"";
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/access", body.c_str(), 2500, 256 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务以加载卡级吊销";
        if (r.status == 400) err = "服务端不认这个卡号（老版本服务端？请重启更新服务）";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = "服务端拒绝：" + r.body.substr(0, 160);
        return false;
    }
    return true;
}

bool PostLogFetch(OpsState& st, const OpsState::ConnectedClient& c, const char* mode,
                  std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    if (!c.identified || (c.deviceId.empty() && c.mac.empty())) {
        err = "需要 deviceId 或 MAC";
        return false;
    }
    const char* m = (mode && std::strcmp(mode, "full") == 0) ? "full" : "light";
    std::string note = std::string("ops-fetch ") + m;
    if (!c.machine.empty()) note += " " + c.machine;
    if (!c.charName.empty()) note += " " + c.charName;
    std::string body = "{\"action\":\"enqueue\",\"mode\":\"";
    body += m;
    body += "\",\"note\":\"" + JsonEscapeLocal(note) + "\"";
    if (!c.machine.empty()) body += ",\"machine\":\"" + JsonEscapeLocal(c.machine) + "\"";
    if (!c.deviceId.empty()) body += ",\"deviceId\":\"" + JsonEscapeLocal(c.deviceId) + "\"";
    if (!c.mac.empty()) body += ",\"mac\":\"" + JsonEscapeLocal(c.mac) + "\"";
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/log-fetch", body.c_str(), 2500, 64 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务（需含 log-fetch）";
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "拉取请求失败";
        return false;
    }
    return true;
}

bool PostForceTarget(OpsState& st, const OpsState::ConnectedClient& c, std::string& err,
                     std::string* tip = nullptr) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    if (!c.identified || (c.deviceId.empty() && c.mac.empty())) {
        err = "需要 deviceId 或 MAC";
        return false;
    }
    if (st.forcedClientBuildId > 0) {
        err = "请先取消全体强制更新（否则其他在线设备仍会一起更新）";
        return false;
    }
    if (st.latestClientBuildId == 0 || st.latestClientVersionText.empty()) {
        err = "无最新发布包（latest.json）";
        return false;
    }
    {
        ReleaseInfo latest{};
        if (!LoadReleaseInfo(st, L"latest.json", latest) || latest.zipName.empty()) {
            err = "latest.json 无效";
            return false;
        }
        const std::wstring zipPath = ReleasePath(st, xcat::Utf8ToWide(latest.zipName).c_str());
        std::error_code existsError;
        if (!std::filesystem::is_regular_file(zipPath, existsError) || existsError) {
            err = "最新发布 zip 不存在：" + latest.zipName;
            return false;
        }
    }
    if (tip && !c.appVersion.empty() &&
        c.appVersion.find(st.latestClientVersionText) != std::string::npos) {
        *tip = "该设备版本字符串已含最新号（若 build 已达标则服务端会立刻清任务）";
    }
    std::string note = "ops-force-target";
    if (!c.machine.empty()) note += " " + c.machine;
    if (!c.charName.empty()) note += " " + c.charName;
    std::string body = "{\"action\":\"enqueue\"";
    body += ",\"note\":\"" + JsonEscapeLocal(note) + "\"";
    if (!c.machine.empty()) body += ",\"machine\":\"" + JsonEscapeLocal(c.machine) + "\"";
    if (!c.deviceId.empty()) body += ",\"deviceId\":\"" + JsonEscapeLocal(c.deviceId) + "\"";
    if (!c.mac.empty()) body += ",\"mac\":\"" + JsonEscapeLocal(c.mac) + "\"";
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/force-target", body.c_str(), 2500, 64 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        if (r.status == 404) err = "接口不存在：请重启 TWMS 更新服务（需含 force-target）";
        if (r.status == 409 || r.body.find("global_force_active") != std::string::npos) {
            err = "请先取消全体强制更新（服务端仍有 force-update.json）";
        }
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "指定推送失败";
        if (r.body.find("global_force_active") != std::string::npos) {
            err = "请先取消全体强制更新（服务端仍有 force-update.json）";
        }
        return false;
    }
    return true;
}

bool PostForceTargetCancel(OpsState& st, const OpsState::ConnectedClient& c, std::string& err) {
    if (!TwmsRunning(st)) {
        err = "TWMS API 未运行";
        return false;
    }
    std::string body = "{\"action\":\"cancel\"";
    if (!c.forceTargetId.empty()) {
        body += ",\"id\":\"" + JsonEscapeLocal(c.forceTargetId) + "\"";
    } else {
        if (!c.machine.empty()) body += ",\"machine\":\"" + JsonEscapeLocal(c.machine) + "\"";
        if (!c.deviceId.empty()) body += ",\"deviceId\":\"" + JsonEscapeLocal(c.deviceId) + "\"";
        if (!c.mac.empty()) body += ",\"mac\":\"" + JsonEscapeLocal(c.mac) + "\"";
    }
    body += "}";
    const auto r =
        HttpPost(L"127.0.0.1", 18789, L"/twms/admin/force-target", body.c_str(), 2500, 64 * 1024);
    if (!r.ok) {
        err = !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        return false;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        err = FindJsonString(r.body, "error");
        if (err.empty()) err = "取消指定推送失败";
        return false;
    }
    return true;
}

void RefreshForceTargetQueue(OpsState& st) {
    st.forceTargetQueue.clear();
    st.forceTargetQueueError.clear();
    if (!TwmsRunning(st)) return;
    const auto r = HttpGet(L"127.0.0.1", 18789, L"/twms/admin/force-target", 1500, 256 * 1024);
    if (!r.ok) {
        if (r.status == 404) {
            st.forceTargetQueueError = "需重启更新服务（force-target）";
        } else {
            st.forceTargetQueueError =
                !r.error.empty() ? r.error : ("HTTP " + std::to_string(r.status));
        }
        return;
    }
    if (r.body.find("\"ok\":true") == std::string::npos) {
        st.forceTargetQueueError = FindJsonString(r.body, "error");
        if (st.forceTargetQueueError.empty()) st.forceTargetQueueError = "force-target 解析失败";
        return;
    }
    const size_t arrKey = r.body.find("\"pending\"");
    if (arrKey == std::string::npos) return;
    size_t i = r.body.find('[', arrKey);
    if (i == std::string::npos) return;
    ++i;
    while (i < r.body.size()) {
        while (i < r.body.size() &&
               (r.body[i] == ' ' || r.body[i] == '\t' || r.body[i] == '\r' || r.body[i] == '\n' ||
                r.body[i] == ',')) {
            ++i;
        }
        if (i >= r.body.size() || r.body[i] == ']') break;
        if (r.body[i] != '{') break;
        const size_t start = i;
        int depth = 0;
        bool inStr = false;
        for (; i < r.body.size(); ++i) {
            const char c = r.body[i];
            if (inStr) {
                if (c == '\\' && i + 1 < r.body.size()) {
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
        const std::string obj = r.body.substr(start, i - start);
        OpsState::ForceTargetPending row;
        row.id = FindJsonString(obj, "id");
        row.status = FindJsonString(obj, "status");
        row.machine = FindJsonString(obj, "machine");
        row.deviceId = FindJsonString(obj, "deviceId");
        row.mac = FindJsonString(obj, "mac");
        row.note = FindJsonString(obj, "note");
        row.at = FindJsonString(obj, "at");
        row.buildId = static_cast<uint32_t>(JsonIntField(obj, "buildId", 0));
        if (!row.id.empty()) st.forceTargetQueue.push_back(std::move(row));
    }
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
        st.forceTargetQueue.clear();
        return;
    }

    std::wstring path =
        L"/twms/admin/clients?activeSec=" + std::to_wstring(st.clientsActiveSec);
    if (refreshGeo) path += L"&refreshGeo=1";
    // 旧版服务端会把数百条同 IP 告警塞进同一响应；2MB 兜底防 UTF-8 截断变 '?'。
    const auto r = HttpGet(L"127.0.0.1", 18789, path.c_str(), 2500, 2 * 1024 * 1024);
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
    SampleMesoDash(st);
    RefreshForceTargetQueue(st);
}

void DrawMainTabButtons(OpsState& st) {
    auto tabBtn = [&](const char* label, int id) {
        const bool selected = (st.mainTab == id);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label) && !selected) {
            st.mainTab = id;
            if (id == 1 || id == 2) {
                RefreshClients(st, true);
                if (id == 1) RefreshBans(st, true);
            }
            if (id == 3) RefreshQuota(st, true);
            if (id == 4) RefreshGateCards(st, true);
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
    ImGui::SameLine();
    {
        const ULONGLONG dayCut =
            MesoDashWallMs() > 86400000ull ? MesoDashWallMs() - 86400000ull : 0;
        const int nAlert = MesoCountEvents(st, dayCut, 0);
        char mesoTab[80]{};
        if (nAlert > 0)
            std::snprintf(mesoTab, sizeof(mesoTab), "利润监控 (%d)##maintab", nAlert);
        else
            std::snprintf(mesoTab, sizeof(mesoTab), "利润监控##maintab");
        if (nAlert > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, OpsTone::Danger());
            tabBtn(mesoTab, 2);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("近 24h 外转/跨号/重连骤降 %d 笔\n背包金=利润，流水落盘 30 天", nAlert);
        } else {
            tabBtn(mesoTab, 2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("监控用户背包金变化（利润）\n防偷偷转移 · 流水 30 天");
        }
    }
    ImGui::SameLine();
    {
        char qtab[64]{};
        const int nq = static_cast<int>(st.quotaUsers.size());
        if (nq > 0)
            std::snprintf(qtab, sizeof(qtab), "台数配额 (%d)##maintab", nq);
        else
            std::snprintf(qtab, sizeof(qtab), "台数配额##maintab");
        tabBtn(qtab, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("按 uid（个人签名 TOKEN）限制可激活的设备台数");
    }
    ImGui::SameLine();
    {
        tabBtn("签卡##maintab", 4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("给成员签发 gate/1 启动 TOKEN（本机私钥离线签）");
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

// uid 比对用：服务端 normalizeUid 会折大小写并去空白，台账/配额两侧的 uid 可能大小写不一致。
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
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
                ClientVersionIsCurrent(st, c.appVersion))
                return false;
        } else if (c.gate != st.clientsGateFilter) {
            return false;
        }
    }
    if (!filter || filter[0] == '\0') return true;
    const std::string mapLabel = ClientMapLabel(st, c);
    return ContainsIgnoreCase(c.ip, filter) || ContainsIgnoreCase(c.machine, filter) ||
           ContainsIgnoreCase(c.mac, filter) || ContainsIgnoreCase(c.token, filter) ||
           ContainsIgnoreCase(c.uid, filter) ||
           ContainsIgnoreCase(c.device, filter) || ContainsIgnoreCase(c.deviceId, filter) ||
           ContainsIgnoreCase(c.geo, filter) || ContainsIgnoreCase(c.gate, filter) ||
           ContainsIgnoreCase(c.appVersion, filter) || ContainsIgnoreCase(c.charName, filter) ||
           ContainsIgnoreCase(c.charJobName, filter) || ContainsIgnoreCase(c.mapName, filter) ||
           ContainsIgnoreCase(mapLabel, filter) ||
           (c.mapId > 0 && ContainsIgnoreCase(std::to_string(c.mapId), filter)) ||
           (c.channelId > 0 && ContainsIgnoreCase(std::to_string(c.channelId), filter));
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

// 客户端表列 UserID（点击列头排序）
enum ClientSortCol : ImGuiID {
    kCliIp = 1,
    kCliGeo,
    kCliMachine,
    kCliChar,
    kCliLevel,
    kCliJob,
    kCliMeso,
    kCliMapCh,
    kCliMac,
    kCliToken,
    kCliDevice,
    kCliVer,
    kCliLastSeen,
    kCliIdle,
    kCliGate,
    kCliHits,
    kCliAction,
};

int CmpStrField(const std::string& a, const std::string& b) {
    return a.compare(b);
}

int CmpIntField(int a, int b) {
    return (a > b) - (a < b);
}

// 背包金是十进制字符串，按数值比（长度优先，避免大数溢出）
int CmpMesoField(const std::string& a, const std::string& b) {
    auto digits = [](const std::string& s) -> std::string {
        size_t i = 0;
        while (i < s.size() && (s[i] == '0' || s[i] == '+' || s[i] == ' ')) ++i;
        if (i >= s.size()) return "0";
        return s.substr(i);
    };
    const std::string na = digits(a);
    const std::string nb = digits(b);
    if (na.size() != nb.size()) return CmpIntField(static_cast<int>(na.size()), static_cast<int>(nb.size()));
    return na.compare(nb);
}

int CompareConnectedClient(const OpsState::ConnectedClient& a, const OpsState::ConnectedClient& b,
                           ImGuiID col) {
    switch (col) {
        case kCliIp:
            return CmpStrField(a.ip, b.ip);
        case kCliGeo:
            return CmpStrField(a.geo, b.geo);
        case kCliMachine:
            return CmpStrField(a.machine, b.machine);
        case kCliChar:
            return CmpStrField(a.charName, b.charName);
        case kCliLevel:
            return CmpIntField(a.charLevel, b.charLevel);
        case kCliJob:
            return CmpStrField(a.charJobName, b.charJobName);
        case kCliMeso:
            return CmpMesoField(a.charMeso, b.charMeso);
        case kCliMapCh:
            if (int d = CmpIntField(static_cast<int>(a.mapId), static_cast<int>(b.mapId))) return d;
            return CmpIntField(a.channelId, b.channelId);
        case kCliMac:
            return CmpStrField(a.mac, b.mac);
        case kCliToken:
            return CmpStrField(a.token, b.token);
        case kCliDevice:
            return CmpStrField(a.device, b.device);
        case kCliVer:
            return CmpStrField(a.appVersion, b.appVersion);
        case kCliLastSeen:
            return CmpStrField(a.lastSeenAt, b.lastSeenAt);
        case kCliIdle:
            return CmpIntField(a.idleSec, b.idleSec);
        case kCliGate:
            return CmpStrField(a.gate, b.gate);
        case kCliHits:
            return CmpIntField(a.hits, b.hits);
        default:
            return 0;
    }
}

// 人的主键：签卡 uid（不可伪造）> 能对上的旧 TOKEN 并入该 uid > 其余旧 TOKEN > 无标识不合并。
std::string ClientResolvedUid(const OpsState::ConnectedClient& c,
                              const std::unordered_map<std::string, std::string>& tokenToUid) {
    if (!c.uid.empty()) return c.uid;
    if (c.token.empty()) return {};
    const auto it = tokenToUid.find(c.token);
    if (it != tokenToUid.end()) return it->second;
    return {};
}

std::string ClientPersonKey(const OpsState::ConnectedClient& c, size_t idx,
                            const std::unordered_map<std::string, std::string>& tokenToUid) {
    const std::string uid = ClientResolvedUid(c, tokenToUid);
    if (!uid.empty()) return std::string("u\x1f") + uid;
    if (!c.token.empty()) return std::string("t\x1f") + c.token;
    return std::string("\x01solo:") + std::to_string(idx);
}

int ClientPersonRank(const OpsState::ConnectedClient& c,
                     const std::unordered_map<std::string, std::string>& tokenToUid) {
    if (!ClientResolvedUid(c, tokenToUid).empty()) return 0;
    if (!c.token.empty()) return 1;
    return 2;
}

int CmpPersonPrimary(const OpsState::ConnectedClient& a, const OpsState::ConnectedClient& b,
                     const std::unordered_map<std::string, std::string>& tokenToUid) {
    const int ra = ClientPersonRank(a, tokenToUid);
    const int rb = ClientPersonRank(b, tokenToUid);
    if (ra != rb) return ra - rb;
    if (ra == 0) return ClientResolvedUid(a, tokenToUid).compare(ClientResolvedUid(b, tokenToUid));
    if (ra == 1) return a.token.compare(b.token);
    return 0;
}

bool ClientPersonKeyIsUid(const std::string& key) {
    return key.size() >= 2 && key[0] == 'u' && key[1] == '\x1f';
}

const char* ClientPersonKeyDisplay(const std::string& key) {
    if (key.size() >= 2 && key[1] == '\x1f') return key.c_str() + 2;
    return key.c_str();
}

// 旧调试 TOKEN 比较（连接表排序兜底；利润监控分组已改走 MesoPersonId）。
int CmpTokenPrimary(const std::string& a, const std::string& b) {
    const bool ae = a.empty();
    const bool be = b.empty();
    if (ae != be) return ae ? 1 : -1;
    return a.compare(b);
}

template <typename GetItem>
void StableSortByTableSpecs(std::vector<size_t>& order, const ImGuiTableSortSpecs* specs,
                            GetItem&& getItem) {
    if (!specs || specs->SpecsCount <= 0 || order.size() < 2) return;
    std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
        for (int n = 0; n < specs->SpecsCount; ++n) {
            const ImGuiTableColumnSortSpecs& s = specs->Specs[n];
            const int delta = getItem(ia, ib, s.ColumnUserID);
            if (delta != 0) {
                return (s.SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0) : (delta > 0);
            }
        }
        return ia < ib;
    });
}

// 连接表专用：先按人（uid > 旧 TOKEN），再列头（或空闲），保证同人行聚在一起。
void SortClientIndices(OpsState& st, std::vector<size_t>& order, const ImGuiTableSortSpecs* specs,
                       bool idleFirst,
                       const std::unordered_map<std::string, std::string>& tokenToUid) {
    if (order.size() < 2) return;
    const bool haveColSort = specs && specs->SpecsCount > 0;
    std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
        const int td = CmpPersonPrimary(st.clients[ia], st.clients[ib], tokenToUid);
        if (td != 0) return td < 0;
        if (haveColSort) {
            for (int n = 0; n < specs->SpecsCount; ++n) {
                const ImGuiTableColumnSortSpecs& s = specs->Specs[n];
                if (s.ColumnUserID == kCliToken) continue;  // 已用主键
                const int delta =
                    CompareConnectedClient(st.clients[ia], st.clients[ib], s.ColumnUserID);
                if (delta != 0) {
                    return (s.SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0)
                                                                             : (delta > 0);
                }
            }
        } else if (idleFirst) {
            const int iaIdle = st.clients[ia].idleSec;
            const int ibIdle = st.clients[ib].idleSec;
            if (iaIdle != ibIdle) return iaIdle < ibIdle;
        }
        return ia < ib;
    });
}

int CompareBannedDevice(const OpsState::BannedDevice& a, const OpsState::BannedDevice& b, ImGuiID col) {
    switch (col) {
        case 1:
            return CmpStrField(a.key, b.key);
        case 2:
            return CmpStrField(a.machine, b.machine);
        case 3:
            return CmpStrField(a.mac, b.mac);
        case 4:
            return CmpStrField(a.token, b.token);
        case 5:
            return CmpStrField(a.reason, b.reason);
        default:
            return 0;
    }
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
        if (!c.appVersion.empty() && !ClientVersionIsCurrent(st, c.appVersion))
            ++nStaleVer;
    }
    const int alertN =
        st.ipAlertCount > 0 ? st.ipAlertCount : static_cast<int>(st.ipAlerts.size());

    std::unordered_map<std::string, std::string> tokenToUid;
    MesoCollectTokenUidAliases(st, tokenToUid);

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
    // ── 严格模式：无有效签名 TOKEN 直接拒（默认关，兼容老客户端） ──
    ImGui::SameLine(0, 12.f);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8.f);
    {
        bool strict = st.clientsStrictToken;
        if (ImGui::Checkbox("严格模式##strict_token", &strict)) {
            if (strict) {
                // 开启方向不可逆伤老客户端：严格模式拒绝走的是正式 Denied，客户端会写本地隐蔽粘性
                // 并清租约；关回来也得让对方再探活成功一次才解得掉。先摆在线里没 uid 的台数再确认。
                ImGui::OpenPopup("confirm_strict_token");
            } else {
                std::string err;
                if (PostSetStrictToken(st, false, err)) {
                    SetStatus(st, "已关闭严格模式：允许无 TOKEN 的老客户端（被写过粘性的需再探活一次才恢复）");
                    RefreshClients(st, true);
                } else {
                    SetStatus(st, err);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "开：只有带有效签名 TOKEN（gate/1 激活）的客户端才放行，改硬件码也蹭不进。\n"
                "关：不检查 TOKEN，老客户端（无签名 TOKEN）也能连——仅黑/白名单拦截。\n"
                "默认关；开启前确认在用客户端都已用签名 TOKEN 激活，否则会被挡在门外。");
        }
        if (ImGui::BeginPopupModal("confirm_strict_token", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::vector<const OpsState::ConnectedClient*> noUidList;
            for (const auto& c : st.clients)
                if (c.uid.empty()) noUidList.push_back(&c);
            const int noUid = static_cast<int>(noUidList.size());
            ImGui::TextUnformatted("确认开启「严格模式」？");
            ImGui::Spacing();
            if (noUid > 0) {
                ImGui::TextColored(OpsTone::Danger(),
                                   "当前在线 %d 台没有签名 TOKEN（老版本或未激活）：", noUid);
                ImGui::BulletText("下一次探活即被拒并退出");
                ImGui::BulletText("客户端会写本地封禁粘性、清掉在线租约");
                ImGui::BulletText("关回严格模式后，它们还需联网探活成功一次才解得掉粘性");
                ImGui::Spacing();
                // 光给数字判断不了该不该开：列出具体是谁，认得出就是自己人还没换卡。
                const int kShow = 8;
                for (int i = 0; i < noUid && i < kShow; ++i) {
                    const auto* c = noUidList[static_cast<size_t>(i)];
                    std::string line = c->ip.empty() ? std::string("(无 IP)") : c->ip;
                    if (!c->machine.empty()) line += "  " + c->machine;
                    if (!c->charName.empty()) line += "  " + c->charName;
                    if (!c->appVersion.empty()) line += "  v" + c->appVersion;
                    ImGui::TextDisabled("    %s", line.c_str());
                }
                if (noUid > kShow) ImGui::TextDisabled("    …另有 %d 台", noUid - kShow);
            } else {
                ImGui::TextUnformatted("当前在线客户端都带签名 TOKEN，开启不影响它们。");
                ImGui::TextDisabled("离线的老版本客户端仍会在下次上线时被拒。");
            }
            ImGui::Spacing();
            ImGui::TextDisabled("在线口径：最近 %d 秒内探活过的客户端；离线机器数不进来。",
                                st.clientsActiveSec);
            ImGui::Spacing();
            if (DangerButton("确认开启##strict_yes", ImVec2(120, 0))) {
                std::string err;
                if (PostSetStrictToken(st, true, err)) {
                    SetStatus(st, "已开启严格模式：无有效签名 TOKEN 一律拒");
                    RefreshClients(st, true);
                } else {
                    SetStatus(st, err);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消##strict_no", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
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
    ImGui::InputTextWithHint("##clients_filter", "筛选 IP/机名/角色/地图/频道/MAC/TOKEN…",
                             st.clientsFilter,
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
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "未点列头时：同人组内空闲秒数小的排前。\n"
            "任意排序均以「人」为最高优先级（签卡 uid 优先，能对上的旧 TOKEN 并入）");
    ImGui::SameLine(0, 6.f);
    ImGui::Checkbox("同人折叠", &st.clientsGroupByToken);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "有签卡按 uid 合并；同一旧 TOKEN 能对上该 uid 的未签卡会话也并入\n"
            "对不上的仍按旧 TOKEN 一组；无身份不合并\n点 [+] 展开；右键组操作");
    if (st.clientsGroupByToken) {
        ImGui::SameLine(0, 4.f);
        if (ImGui::SmallButton("全展##tok_expand_all")) {
            for (size_t i = 0; i < st.clients.size(); ++i) {
                const auto& c = st.clients[i];
                if (c.uid.empty() && c.token.empty()) continue;
                st.clientsGroupExpanded.insert(ClientPersonKey(c, i, tokenToUid));
            }
        }
        ImGui::SameLine(0, 2.f);
        if (ImGui::SmallButton("全折##tok_collapse_all")) st.clientsGroupExpanded.clear();
    }
    ImGui::SameLine(0, 8.f);
    ImGui::Checkbox("同IP折叠", &st.clientsGroupByIp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "开「同人」时：展开后同 IP 会话再折叠一层\n"
            "关「同人」时：顶层直接按公网 IP 折叠");
    if (st.clientsGroupByIp) {
        ImGui::SameLine(0, 4.f);
        if (ImGui::SmallButton("全展##ip_expand_all")) {
            if (st.clientsGroupByToken) {
                for (size_t i = 0; i < st.clients.size(); ++i) {
                    const auto& c = st.clients[i];
                    if (c.ip.empty() || (c.uid.empty() && c.token.empty())) continue;
                    st.clientsIpExpanded.insert(ClientPersonKey(c, i, tokenToUid) + "\x1f" + c.ip);
                }
            } else {
                for (const auto& c : st.clients) {
                    if (!c.ip.empty()) st.clientsIpExpanded.insert(c.ip);
                }
            }
        }
        ImGui::SameLine(0, 2.f);
        if (ImGui::SmallButton("全折##ip_collapse_all")) st.clientsIpExpanded.clear();
    }
    ImGui::SameLine(0, 6.f);
    if (ImGui::SmallButton("复制可见##clients")) {
        std::string out =
            "ip\tmachine\tmac\ttoken\tdeviceId\tver\tchar\tlevel\tjob\tmeso\tmapId\tmap\tch\tgate\tidle\n";
        int n = 0;
        for (const auto& c : st.clients) {
            if (!ClientMatchesFilter(st, c, st.clientsFilter)) continue;
            AppendClientSummaryLine(out, st, c);
            ++n;
        }
        CopyText(out.c_str());
        SetStatus(st, "已复制可见客户端 " + std::to_string(n) + " 行");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("复制当前筛选结果为 TSV（打平各台，含折叠组内成员）");

    int filterMatch = 0;
    const bool anyFilter = st.clientsFilter[0] != '\0' || st.clientsGateFilter[0] != '\0';
    if (anyFilter) {
        for (const auto& c : st.clients) {
            if (ClientMatchesFilter(st, c, st.clientsFilter)) ++filterMatch;
        }
        ImGui::SameLine(0, 8.f);
        ImGui::TextDisabled("%d/%d", filterMatch, st.clientsCount);
        if (st.clientsGateFilter[0] != '\0' && st.clientsFilter[0] != '\0') {
            ImGui::SameLine(0, 6.f);
            ImGui::TextDisabled("（门禁 chip ∧ 文本）");
        } else if (st.clientsGateFilter[0] != '\0') {
            ImGui::SameLine(0, 6.f);
            ImGui::TextDisabled("（门禁 chip）");
        }
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SortTristate;
    // 工具条收紧后主表再抬一点。
    const float clientsH = (std::max)(260.f, ImGui::GetContentRegionAvail().y * 0.58f);
    if (ImGui::BeginTable("clients_table", 17, flags, ImVec2(0, clientsH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 100.f, kCliIp);
        ImGui::TableSetupColumn("归属地", ImGuiTableColumnFlags_WidthStretch, 1.2f, kCliGeo);
        ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 0.9f, kCliMachine);
        ImGui::TableSetupColumn("角色", ImGuiTableColumnFlags_WidthStretch, 1.0f, kCliChar);
        ImGui::TableSetupColumn("等级", ImGuiTableColumnFlags_WidthFixed, 44.f, kCliLevel);
        ImGui::TableSetupColumn("职业", ImGuiTableColumnFlags_WidthFixed, 88.f, kCliJob);
        ImGui::TableSetupColumn("背包金", ImGuiTableColumnFlags_WidthFixed, 100.f, kCliMeso);
        ImGui::TableSetupColumn("图/频", ImGuiTableColumnFlags_WidthFixed, 140.f, kCliMapCh);
        ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 110.f, kCliMac);
        ImGui::TableSetupColumn("TOKEN",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort,
                                88.f, kCliToken);
        ImGui::TableSetupColumn("设备", ImGuiTableColumnFlags_WidthStretch, 1.0f, kCliDevice);
        ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthStretch, 0.85f, kCliVer);
        ImGui::TableSetupColumn("最近活动", ImGuiTableColumnFlags_WidthFixed, 120.f, kCliLastSeen);
        ImGui::TableSetupColumn("空闲", ImGuiTableColumnFlags_WidthFixed, 42.f, kCliIdle);
        ImGui::TableSetupColumn("门禁", ImGuiTableColumnFlags_WidthFixed, 128.f, kCliGate);
        ImGui::TableSetupColumn("请求", ImGuiTableColumnFlags_WidthFixed, 40.f, kCliHits);
        ImGui::TableSetupColumn("操作",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 200.f,
                                kCliAction);
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

            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            const bool haveColSort = sortSpecs && sortSpecs->SpecsCount > 0;
            auto cmpClientIdx = [&](size_t ia, size_t ib, ImGuiID col) {
                return CompareConnectedClient(st.clients[ia], st.clients[ib], col);
            };
            auto sortMembers = [&](std::vector<size_t>& v) {
                SortClientIndices(st, v, haveColSort ? sortSpecs : nullptr, st.clientsSortIdleFirst,
                                  tokenToUid);
            };

            struct ClientGroup {
                std::string key;  // 人：uid（含并入的旧 TOKEN）或旧 TOKEN；无身份用 solo
                std::vector<size_t> members;
            };
            std::vector<ClientGroup> groups;
            groups.reserve(order.size());

            auto groupKeyOf = [&](size_t idx) -> std::string {
                return ClientPersonKey(st.clients[idx], idx, tokenToUid);
            };

            if (st.clientsGroupByToken) {
                // 按人分桶（签卡 uid 优先；未激活仍按旧 TOKEN；都没有则各成一组）
                std::map<std::string, std::vector<size_t>> byKey;
                for (size_t idx : order) byKey[groupKeyOf(idx)].push_back(idx);
                std::vector<std::string> keys;
                keys.reserve(byKey.size());
                for (auto& kv : byKey) {
                    sortMembers(kv.second);
                    keys.push_back(kv.first);
                }
                // 组序：有 uid 的人排前，其次旧 TOKEN，无标识垫后
                std::stable_sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) {
                    const auto& ma = byKey[a];
                    const auto& mb = byKey[b];
                    if (ma.empty() || mb.empty()) return a < b;
                    const int td = CmpPersonPrimary(st.clients[ma.front()], st.clients[mb.front()],
                                                    tokenToUid);
                    if (td != 0) return td < 0;
                    if (haveColSort) {
                        for (int n = 0; n < sortSpecs->SpecsCount; ++n) {
                            const ImGuiTableColumnSortSpecs& s = sortSpecs->Specs[n];
                            if (s.ColumnUserID == kCliToken) continue;
                            int delta = 0;
                            if (s.ColumnUserID == kCliIp) {
                                delta = CmpStrField(st.clients[ma.front()].ip, st.clients[mb.front()].ip);
                            } else {
                                delta = CompareConnectedClient(st.clients[ma.front()],
                                                               st.clients[mb.front()], s.ColumnUserID);
                            }
                            if (delta != 0) {
                                return (s.SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0)
                                                                                         : (delta > 0);
                            }
                        }
                    } else if (st.clientsSortIdleFirst) {
                        const int ia = st.clients[ma.front()].idleSec;
                        const int ib = st.clients[mb.front()].idleSec;
                        if (ia != ib) return ia < ib;
                    }
                    return a < b;
                });
                for (const auto& key : keys) {
                    groups.push_back(ClientGroup{key, std::move(byKey[key])});
                }
            } else if (st.clientsGroupByIp) {
                std::map<std::string, std::vector<size_t>> byIp;
                for (size_t idx : order) {
                    const std::string& ip = st.clients[idx].ip;
                    byIp[ip.empty() ? (std::string("\x01solo:") + std::to_string(idx)) : ip]
                        .push_back(idx);
                }
                std::vector<std::string> ips;
                ips.reserve(byIp.size());
                for (auto& kv : byIp) {
                    sortMembers(kv.second);
                    ips.push_back(kv.first);
                }
                std::stable_sort(ips.begin(), ips.end(), [&](const std::string& a, const std::string& b) {
                    const auto& ma = byIp[a];
                    const auto& mb = byIp[b];
                    if (ma.empty() || mb.empty()) return a < b;
                    const int td = CmpPersonPrimary(st.clients[ma.front()], st.clients[mb.front()],
                                                    tokenToUid);
                    if (td != 0) return td < 0;
                    if (haveColSort) {
                        for (int n = 0; n < sortSpecs->SpecsCount; ++n) {
                            const ImGuiTableColumnSortSpecs& s = sortSpecs->Specs[n];
                            if (s.ColumnUserID == kCliToken) continue;
                            int delta = 0;
                            if (s.ColumnUserID == kCliIp) {
                                delta = CmpStrField(a, b);
                            } else {
                                delta = CompareConnectedClient(st.clients[ma.front()],
                                                               st.clients[mb.front()], s.ColumnUserID);
                            }
                            if (delta != 0) {
                                return (s.SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0)
                                                                                         : (delta > 0);
                            }
                        }
                    } else if (st.clientsSortIdleFirst) {
                        const int ia = st.clients[ma.front()].idleSec;
                        const int ib = st.clients[mb.front()].idleSec;
                        if (ia != ib) return ia < ib;
                    }
                    return a < b;
                });
                for (const auto& ip : ips) {
                    groups.push_back(ClientGroup{ip, std::move(byIp[ip])});
                }
            } else {
                sortMembers(order);
                for (size_t idx : order) {
                    groups.push_back(ClientGroup{groupKeyOf(idx), {idx}});
                }
            }

            int shown = 0;
            for (const auto& g : groups) {
                const bool tokenGroup = st.clientsGroupByToken && !g.key.empty() && g.key[0] != '\x01';
                const bool ipOuterGroup = !st.clientsGroupByToken && st.clientsGroupByIp &&
                                          !g.key.empty() && g.key[0] != '\x01' &&
                                          g.members.size() >= 2;
                const bool multi =
                    (tokenGroup && g.members.size() >= 2) || ipOuterGroup;
                const bool expanded =
                    !multi ||
                    (tokenGroup
                         ? st.clientsGroupExpanded.find(g.key) != st.clientsGroupExpanded.end()
                         : st.clientsIpExpanded.find(g.key) != st.clientsIpExpanded.end());

                if (multi) {
                    const auto& head = st.clients[g.members.front()];
                    ImGui::PushID(g.key.c_str());
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
                    std::string ipSummary;
                    if (tokenGroup) {
                        int nShow = 0;
                        for (size_t mi : g.members) {
                            const std::string& ip = st.clients[mi].ip;
                            if (ip.empty()) continue;
                            if (ipSummary.find(ip) != std::string::npos) continue;
                            if (nShow > 0) ipSummary += " / ";
                            ipSummary += ip;
                            if (++nShow >= 2) {
                                if (g.members.size() > 2) ipSummary += " …";
                                break;
                            }
                        }
                        if (ipSummary.empty()) ipSummary = "—";
                    } else {
                        ipSummary = g.key;
                    }
                    char grpLabel[192]{};
                    std::snprintf(grpLabel, sizeof(grpLabel), "%s %s ·%zu台", open ? "[-]" : "[+]",
                                  ipSummary.c_str(), g.members.size());
                    if (tokenGroup) {
                        const uint32_t allowB = AllowedBuildForPerson(st, g.key);
                        if (allowB > 0 && allowB != st.allowedClientBuildId) {
                            std::snprintf(grpLabel, sizeof(grpLabel), "%s %s ·%zu台 ·允许#%u",
                                          open ? "[-]" : "[+]", ipSummary.c_str(), g.members.size(),
                                          allowB);
                        }
                    }
                    auto& expandSet = tokenGroup ? st.clientsGroupExpanded : st.clientsIpExpanded;
                    if (ImGui::Selectable(grpLabel, false,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowOverlap)) {
                        if (open)
                            expandSet.erase(g.key);
                        else
                            expandSet.insert(g.key);
                    }
                    if (ImGui::BeginPopupContextItem("outer_grp_ops")) {
                        if (!open && ImGui::MenuItem("展开本组")) expandSet.insert(g.key);
                        if (open && ImGui::MenuItem("折叠本组")) expandSet.erase(g.key);
                        ImGui::Separator();
                        auto forEachIdentified = [&](auto&& fn) {
                            int n = 0;
                            for (size_t mi : g.members) {
                                const auto& m = st.clients[mi];
                                if (!m.identified || (m.deviceId.empty() && m.mac.empty())) continue;
                                fn(m);
                                ++n;
                            }
                            return n;
                        };
                        if (ImGui::MenuItem("本组拉取轻量日志")) {
                            int ok = 0;
                            std::string lastErr;
                            forEachIdentified([&](const OpsState::ConnectedClient& m) {
                                std::string err;
                                if (PostLogFetch(st, m, "light", err))
                                    ++ok;
                                else
                                    lastErr = err;
                            });
                            if (ok)
                                SetStatus(st, "本组已请求轻量日志 " + std::to_string(ok) + " 台");
                            else
                                SetStatus(st, lastErr.empty() ? "本组无可识别设备" : lastErr);
                            if (ok) RefreshClients(st, true);
                        }
                        if (ImGui::MenuItem("本组拉取全量日志")) {
                            int ok = 0;
                            std::string lastErr;
                            forEachIdentified([&](const OpsState::ConnectedClient& m) {
                                std::string err;
                                if (PostLogFetch(st, m, "full", err))
                                    ++ok;
                                else
                                    lastErr = err;
                            });
                            if (ok)
                                SetStatus(st, "本组已请求全量日志 " + std::to_string(ok) + " 台");
                            else
                                SetStatus(st, lastErr.empty() ? "本组无可识别设备" : lastErr);
                            if (ok) RefreshClients(st, true);
                        }
                        if (tokenGroup && ImGui::BeginMenu("允许更新到")) {
                            const uint32_t cur = AllowedBuildForPerson(st, g.key);
                            const bool isUid = ClientPersonKeyIsUid(g.key);
                            const char* who = ClientPersonKeyDisplay(g.key);
                            const bool followDefault =
                                st.allowedClientBuildId == 0 || cur == st.allowedClientBuildId;
                            if (ImGui::MenuItem("跟随默认", nullptr, followDefault)) {
                                std::string err;
                                if (PostUpdateChannelGroup(st, isUid ? who : nullptr,
                                                           isUid ? nullptr : who, 0, true, err))
                                    SetStatus(st, std::string("已让 ") + who + " 跟随默认允许版本");
                                else
                                    SetStatus(st, err);
                            }
                            for (const auto& pkg : st.updatePackages) {
                                char lab[80]{};
                                std::snprintf(lab, sizeof(lab), "v%s #%u", pkg.version.c_str(),
                                              pkg.buildId);
                                if (ImGui::MenuItem(lab, nullptr, cur == pkg.buildId)) {
                                    std::string err;
                                    if (PostUpdateChannelGroup(st, isUid ? who : nullptr,
                                                               isUid ? nullptr : who, pkg.buildId,
                                                               false, err))
                                        SetStatus(st, std::string(who) + " 允许更新到 #" +
                                                          std::to_string(pkg.buildId));
                                    else
                                        SetStatus(st, err);
                                }
                            }
                            if (st.updatePackages.empty())
                                ImGui::TextDisabled("无可用包（重启更新服务）");
                            ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem("本组指定推更", nullptr, false,
                                            st.latestClientBuildId > 0 &&
                                                st.forcedClientBuildId == 0)) {
                            int ok = 0;
                            std::string lastErr;
                            forEachIdentified([&](const OpsState::ConnectedClient& m) {
                                std::string err;
                                if (PostForceTarget(st, m, err, nullptr))
                                    ++ok;
                                else
                                    lastErr = err;
                            });
                            if (ok)
                                SetStatus(st, "本组已排队指定推送 " + std::to_string(ok) + " 台");
                            else
                                SetStatus(st, lastErr.empty() ? "本组无可识别设备" : lastErr);
                            if (ok) RefreshClients(st, true);
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        if (tokenGroup) {
                            const char* who = ClientPersonKeyDisplay(g.key);
                            if (ClientPersonKeyIsUid(g.key)) {
                                int nNoUid = 0;
                                for (size_t mi : g.members)
                                    if (st.clients[mi].uid.empty()) ++nNoUid;
                                if (nNoUid > 0) {
                                    ImGui::SetTooltip(
                                        "同人 uid「%s」·%zu 会话（签卡验签）\n"
                                        "其中 %d 台未上报 uid，已按同 TOKEN 并入\n"
                                        "展开后可再按 IP 折叠\n归属：%s",
                                        who, g.members.size(), nNoUid,
                                        head.geo.empty() ? "—" : head.geo.c_str());
                                } else {
                                    ImGui::SetTooltip(
                                        "同人 uid「%s」·%zu 会话（签卡验签）\n展开后可再按 IP 折叠\n归属：%s",
                                        who, g.members.size(),
                                        head.geo.empty() ? "—" : head.geo.c_str());
                                }
                            } else {
                                ImGui::SetTooltip(
                                    "同旧 TOKEN「%s」·%zu 会话（尚未签卡/未上报 uid）\n归属：%s",
                                    who, g.members.size(),
                                    head.geo.empty() ? "—" : head.geo.c_str());
                            }
                        } else {
                            ImGui::SetTooltip(
                                "同公网 IP「%s」·%zu 台\n归属：%s", g.key.c_str(),
                                g.members.size(), head.geo.empty() ? "—" : head.geo.c_str());
                        }
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
                            if (!tag || !tag[0]) tag = "—";
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

                    // 折叠头：汇总关键列，避免整行只剩「…」看不见内容。
                    auto joinUnique = [&](auto&& pick, int maxN) -> std::string {
                        std::string out;
                        int n = 0;
                        for (size_t mi : g.members) {
                            const std::string v = pick(st.clients[mi]);
                            if (v.empty()) continue;
                            if (out.find(v) != std::string::npos) continue;
                            if (n > 0) out += " / ";
                            out += v;
                            if (++n >= maxN) {
                                if (static_cast<int>(g.members.size()) > n) out += " …";
                                break;
                            }
                        }
                        return out;
                    };

                    ImGui::TableSetColumnIndex(4);
                    {
                        int maxLv = 0;
                        bool any = false;
                        for (size_t mi : g.members) {
                            if (st.clients[mi].charLevel > 0) {
                                any = true;
                                maxLv = (std::max)(maxLv, st.clients[mi].charLevel);
                            }
                        }
                        if (!any) ImGui::TextDisabled("—");
                        else ImGui::Text("≤%d", maxLv);
                    }
                    ImGui::TableSetColumnIndex(5);
                    {
                        const std::string jobs = joinUnique(
                            [](const OpsState::ConnectedClient& m) { return m.charJobName; }, 2);
                        if (jobs.empty()) ImGui::TextDisabled("—");
                        else ImGui::TextUnformatted(jobs.c_str());
                    }
                    for (int col = 6; col <= 8; ++col) {
                        ImGui::TableSetColumnIndex(col);
                        if (col == 6) {
                            int counted = 0;
                            const std::string sumRaw = SumGroupMeso(st, g.members, &counted);
                            if (sumRaw.empty()) {
                                ImGui::TextDisabled("—");
                            } else {
                                char mesoBuf[48]{};
                                FormatMesoDisplay(sumRaw, mesoBuf, sizeof(mesoBuf));
                                ImGui::TextUnformatted(mesoBuf);
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("组内累计 %s\n%d/%zu 台上报", sumRaw.c_str(),
                                                      counted, g.members.size());
                                }
                            }
                        } else if (col == 7) {
                            std::string maps;
                            int nShow = 0;
                            for (size_t mi : g.members) {
                                const auto& m = st.clients[mi];
                                if (m.mapId == 0 && m.channelId <= 0) continue;
                                char cell[96]{};
                                FormatMapChannelCell(st, m, cell, sizeof(cell));
                                if (!cell[0]) continue;
                                if (nShow > 0) maps += " / ";
                                maps += cell;
                                if (++nShow >= 2) {
                                    if (static_cast<int>(g.members.size()) > nShow) maps += " …";
                                    break;
                                }
                            }
                            if (maps.empty()) ImGui::TextDisabled("—");
                            else ImGui::TextUnformatted(maps.c_str());
                        } else {
                            const std::string macs = joinUnique(
                                [](const OpsState::ConnectedClient& m) { return m.mac; }, 2);
                            if (macs.empty()) ImGui::TextDisabled("—");
                            else ImGui::TextUnformatted(macs.c_str());
                        }
                    }
                    ImGui::TableSetColumnIndex(9);
                    {
                        if (tokenGroup) {
                            const char* who = ClientPersonKeyDisplay(g.key);
                            if (ClientPersonKeyIsUid(g.key)) {
                                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "uid:%s", who);
                                bool mergedTok = false;
                                for (size_t mi : g.members) {
                                    if (st.clients[mi].uid.empty()) {
                                        mergedTok = true;
                                        break;
                                    }
                                }
                                if (mergedTok) {
                                    ImGui::SameLine(0, 4.f);
                                    ImGui::TextDisabled("含旧");
                                }
                            } else
                                ImGui::TextColored(OpsTone::Warn(), "%s", who);
                        } else {
                            const std::string tokens = joinUnique(
                                [](const OpsState::ConnectedClient& m) { return m.token; }, 3);
                            if (tokens.empty()) ImGui::TextDisabled("—");
                            else ImGui::TextUnformatted(tokens.c_str());
                        }
                    }
                    ImGui::TableSetColumnIndex(10);
                    {
                        const std::string devices = joinUnique(
                            [](const OpsState::ConnectedClient& m) {
                                return !m.device.empty() ? m.device : m.deviceId;
                            },
                            2);
                        if (devices.empty()) ImGui::TextDisabled(open ? "" : "—");
                        else ImGui::TextUnformatted(devices.c_str());
                    }
                    ImGui::TableSetColumnIndex(11);
                    {
                        const std::string vers = joinUnique(
                            [](const OpsState::ConnectedClient& m) { return m.appVersion; }, 2);
                        if (vers.empty()) ImGui::TextDisabled("—");
                        else ImGui::TextUnformatted(vers.c_str());
                    }
                    ImGui::TableSetColumnIndex(12);
                    {
                        size_t best = g.members.front();
                        for (size_t mi : g.members) {
                            if (st.clients[mi].idleSec < st.clients[best].idleSec) best = mi;
                        }
                        const auto& act = st.clients[best];
                        if (act.lastSeenAt.empty()) {
                            ImGui::TextDisabled("—");
                        } else {
                            ImGui::Text("%s", act.lastSeenAt.c_str());
                            if (!act.lastKind.empty()) {
                                ImGui::SameLine(0, 6.f);
                                ImGui::TextDisabled("%s", act.lastKind.c_str());
                            }
                        }
                    }
                    ImGui::TableSetColumnIndex(13);
                    {
                        int bestIdle = head.idleSec;
                        for (size_t mi : g.members)
                            bestIdle = (std::min)(bestIdle, st.clients[mi].idleSec);
                        char idleBuf[16]{};
                        FormatIdleSec(bestIdle, idleBuf, sizeof(idleBuf));
                        ImGui::TextColored(IdleSecColor(bestIdle), "%s", idleBuf);
                    }
                    ImGui::TableSetColumnIndex(14);
                    {
                        const std::string gates = joinUnique(
                            [](const OpsState::ConnectedClient& m) { return m.gate; }, 2);
                        if (gates.empty()) ImGui::TextDisabled("—");
                        else if (gates == "probe_ok")
                            ImGui::TextColored(OpsTone::Ok(), "探活OK");
                        else if (gates == "lease")
                            ImGui::TextColored(OpsTone::Info(), "租约");
                        else
                            ImGui::TextUnformatted(gates.c_str());
                    }
                    ImGui::TableSetColumnIndex(15);
                    {
                        int hits = 0;
                        for (size_t mi : g.members) hits += st.clients[mi].hits;
                        ImGui::TextDisabled("%d", hits);
                    }
                    ImGui::TableSetColumnIndex(16);
                    if (open)
                        ImGui::TextDisabled("");
                    else
                        ImGui::TextDisabled("点开展开");
                    ImGui::PopID();
                    ++shown;
                    if (!expanded) continue;
                }

                // 成员渲染：TOKEN 展开后可再按 IP 嵌套折叠
                struct IpBucket {
                    std::string ip;
                    std::vector<size_t> members;
                };
                std::vector<IpBucket> ipBuckets;
                if (st.clientsGroupByIp && (tokenGroup || !multi)) {
                    std::map<std::string, std::vector<size_t>> byIp;
                    for (size_t mi : g.members) {
                        const std::string& ip = st.clients[mi].ip;
                        byIp[ip.empty() ? (std::string("\x01solo:") + std::to_string(mi)) : ip]
                            .push_back(mi);
                    }
                    for (auto& kv : byIp) {
                        sortMembers(kv.second);
                        ipBuckets.push_back(IpBucket{kv.first, std::move(kv.second)});
                    }
                    std::stable_sort(ipBuckets.begin(), ipBuckets.end(),
                                     [](const IpBucket& a, const IpBucket& b) {
                                         const bool ae = a.ip.empty() || a.ip[0] == '\x01';
                                         const bool be = b.ip.empty() || b.ip[0] == '\x01';
                                         if (ae != be) return !ae && be;
                                         return a.ip < b.ip;
                                     });
                } else {
                    ipBuckets.push_back(IpBucket{"", g.members});
                }

                for (const auto& ib : ipBuckets) {
                    const bool ipKeyOk = !ib.ip.empty() && ib.ip[0] != '\x01';
                    const bool nestIp = st.clientsGroupByIp && tokenGroup && multi && ipKeyOk &&
                                        ib.members.size() >= 2;
                    // 顶层已是 IP 组时不再套一层
                    const bool ipSubMulti = nestIp;
                    const std::string ipExpandKey =
                        tokenGroup ? (g.key + "\x1f" + ib.ip) : ib.ip;
                    const bool ipOpen =
                        !ipSubMulti ||
                        st.clientsIpExpanded.find(ipExpandKey) != st.clientsIpExpanded.end();

                    if (ipSubMulti) {
                        ImGui::PushID(ipExpandKey.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        char ipLab[160]{};
                        std::snprintf(ipLab, sizeof(ipLab), "  %s %s ·%zu台",
                                      ipOpen ? "[-]" : "[+]", ib.ip.c_str(), ib.members.size());
                        if (ImGui::Selectable(ipLab, false,
                                              ImGuiSelectableFlags_SpanAllColumns |
                                                  ImGuiSelectableFlags_AllowOverlap)) {
                            if (ipOpen)
                                st.clientsIpExpanded.erase(ipExpandKey);
                            else
                                st.clientsIpExpanded.insert(ipExpandKey);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("同 TOKEN 下同 IP「%s」·%zu 会话", ib.ip.c_str(),
                                              ib.members.size());
                        }
                        const auto& ihead = st.clients[ib.members.front()];
                        ImGui::TableSetColumnIndex(1);
                        if (ihead.geo.empty()) ImGui::TextDisabled("—");
                        else ImGui::TextUnformatted(ihead.geo.c_str());
                        for (int col = 2; col <= 8; ++col) {
                            ImGui::TableSetColumnIndex(col);
                            ImGui::TextDisabled(ipOpen ? "" : "—");
                        }
                        ImGui::TableSetColumnIndex(9);
                        {
                            const char* who = ClientPersonKeyDisplay(g.key);
                            if (ClientPersonKeyIsUid(g.key))
                                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "uid:%s", who);
                            else
                                ImGui::TextColored(OpsTone::Warn(), "%s", who);
                        }
                        for (int col = 10; col <= 15; ++col) {
                            ImGui::TableSetColumnIndex(col);
                            if (col == 13) {
                                int bestIdle = ihead.idleSec;
                                for (size_t mi : ib.members)
                                    bestIdle = (std::min)(bestIdle, st.clients[mi].idleSec);
                                char idleBuf[16]{};
                                FormatIdleSec(bestIdle, idleBuf, sizeof(idleBuf));
                                ImGui::TextColored(IdleSecColor(bestIdle), "%s", idleBuf);
                            } else if (col == 15) {
                                int hits = 0;
                                for (size_t mi : ib.members) hits += st.clients[mi].hits;
                                ImGui::TextDisabled("%d", hits);
                            } else {
                                ImGui::TextDisabled(ipOpen ? "" : "—");
                            }
                        }
                        ImGui::TableSetColumnIndex(16);
                        ImGui::TextDisabled(ipOpen ? "" : "点开展开");
                        ImGui::PopID();
                        ++shown;
                        if (!ipOpen) continue;
                    }

                for (size_t memberPos = 0; memberPos < ib.members.size(); ++memberPos) {
                const size_t idx = ib.members[memberPos];
                const auto& c = st.clients[idx];
                const bool childRow = multi || ipSubMulti;
                ++shown;
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char ipLabel[96]{};
                if (multi && ipSubMulti) {
                    std::snprintf(ipLabel, sizeof(ipLabel), "    +-");
                } else if (childRow) {
                    // ASCII：避免 └ 缺字变 '?'
                    std::snprintf(ipLabel, sizeof(ipLabel), "  +-");
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
                    if ((c.mapId > 0 || c.channelId > 0) && ImGui::MenuItem("复制地图/频道")) {
                        const std::string label = ClientMapLabel(st, c);
                        char chBuf[16]{};
                        char buf[192]{};
                        std::snprintf(buf, sizeof(buf), "map=%u ch=%s %s", c.mapId,
                                      ChannelOrDash(c.channelId, chBuf, sizeof(chBuf)),
                                      label.c_str());
                        CopyText(buf);
                    }
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
                        CopyClientSummary(st, c);
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
                        ImGui::Separator();
                        if (ImGui::MenuItem("拉取轻量日志")) {
                            std::string err;
                            if (PostLogFetch(st, c, "light", err)) {
                                SetStatus(st, "已请求轻量日志（约 15s 内客户端探活上报）");
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (ImGui::MenuItem("拉取全量日志")) {
                            std::string err;
                            if (PostLogFetch(st, c, "full", err)) {
                                SetStatus(st, "已请求全量日志（约 15s 内客户端探活上报）");
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        ImGui::Separator();
                        if (st.latestClientBuildId == 0 || st.forcedClientBuildId > 0) {
                            ImGui::BeginDisabled();
                            ImGui::MenuItem("推送更新到此设备");
                            ImGui::EndDisabled();
                        } else if (ImGui::MenuItem("推送更新到此设备")) {
                            std::string err;
                            std::string tip;
                            if (PostForceTarget(st, c, err, &tip)) {
                                std::string msg =
                                    "已排队指定推送 build " +
                                    std::to_string(st.latestClientBuildId) +
                                    "（仅此设备；重启更新服务会丢队列）";
                                if (!tip.empty()) msg += "；" + tip;
                                SetStatus(st, msg);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (!c.forceTargetId.empty() && ImGui::MenuItem("取消此设备推送")) {
                            std::string err;
                            if (PostForceTargetCancel(st, c, err)) {
                                SetStatus(st, "已取消指定推送");
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                if (ipHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    CopyClientSummary(st, c);
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
                        std::string tip = "角色 ";
                        tip += c.charName;
                        tip += "\n等级 ";
                        tip += std::to_string(c.charLevel);
                        tip += " · 职业 ";
                        tip += c.charJobName.empty() ? "—" : c.charJobName;
                        tip += " (id=";
                        tip += std::to_string(c.charJob);
                        tip += ")\n背包金 ";
                        tip += mesoTip;
                        tip += "（精确 ";
                        tip += c.charMeso.empty() ? "—" : c.charMeso;
                        tip += "）";
                        if (c.hasWealthScrolls) {
                            tip += "\n卷轴 ";
                            tip += FormatWealthScrollsHuman(st, c.wealthScrolls);
                        }
                        ImGui::SetTooltip("%s", tip.c_str());
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
                {
                    if (c.mapId == 0 && c.channelId <= 0) {
                        ImGui::TextDisabled("—");
                    } else {
                        char cell[96]{};
                        FormatMapChannelCell(st, c, cell, sizeof(cell));
                        ImGui::TextUnformatted(cell[0] ? cell : "—");
                        if (ImGui::IsItemHovered()) {
                            const std::string label = ClientMapLabel(st, c);
                            if (c.channelId > 0) {
                                ImGui::SetTooltip("地图 %s\nid=%u\n频道 ch.%d",
                                                  label.empty() ? "—" : label.c_str(), c.mapId,
                                                  c.channelId);
                            } else {
                                ImGui::SetTooltip("地图 %s\nid=%u\n频道 —",
                                                  label.empty() ? "—" : label.c_str(), c.mapId);
                            }
                        }
                    }
                }
                ImGui::TableSetColumnIndex(8);
                ImGui::TextUnformatted((!c.identified || c.mac.empty()) ? "—" : c.mac.c_str());
                ImGui::TableSetColumnIndex(9);
                if (!c.identified || c.token.empty()) {
                    ImGui::TextDisabled("—");
                } else {
                    ImGui::TextColored(OpsTone::Token(), "%s", c.token.c_str());
                }
                if (!c.uid.empty()) {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "uid:%s", c.uid.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "签名 TOKEN 的 uid（服务端验签，不可伪造）\n按此封禁：改硬件码也拦得住");
                    }
                    const std::string remain = GateExpRemainText(c.gateExp);
                    ImGui::SameLine(0, 6.f);
                    if (c.gateExp > 0 && remain == "已过期")
                        ImGui::TextColored(OpsTone::Danger(), "(%s)", remain.c_str());
                    else
                        ImGui::TextDisabled("(%s)", remain.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("卡到期：%s", GateExpAbsText(c.gateExp).c_str());
                }
                ImGui::TableSetColumnIndex(10);
                ImGui::TextUnformatted((!c.identified || c.device.empty()) ? "—" : c.device.c_str());
                ImGui::TableSetColumnIndex(11);
                if (c.appVersion.empty()) {
                    ImGui::TextDisabled("—");
                } else {
                    ImGui::TextColored(AppVersionColor(st, c.appVersion), "%s", c.appVersion.c_str());
                    if (ImGui::IsItemHovered() && !st.latestClientVersionText.empty() &&
                        !ClientVersionIsCurrent(st, c.appVersion)) {
                        ImGui::SetTooltip("最新发布 v%s #%u · 此客户端偏旧",
                                          st.latestClientVersionText.c_str(),
                                          st.latestClientBuildId);
                    }
                }
                ImGui::TableSetColumnIndex(12);
                if (!c.lastSeenAt.empty()) {
                    ImGui::Text("%s", c.lastSeenAt.c_str());
                    ImGui::SameLine(0, 6.f);
                    ImGui::TextDisabled("%s", c.lastKind.c_str());
                } else {
                    ImGui::TextDisabled("—");
                }
                ImGui::TableSetColumnIndex(13);
                char idleBuf[16]{};
                FormatIdleSec(c.idleSec, idleBuf, sizeof(idleBuf));
                ImGui::TextColored(IdleSecColor(c.idleSec), "%s", idleBuf);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("空闲 %d 秒", c.idleSec);
                ImGui::TableSetColumnIndex(14);
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
                            c.gate.empty() ? "—" : c.gate.c_str(),
                            c.lastAllowAt.empty() ? "—" : c.lastAllowAt.c_str(), c.leaseRemainSec,
                            c.leaseTtlHours > 0 ? c.leaseTtlHours : 64);
                    }
                }
                ImGui::TableSetColumnIndex(15);
                ImGui::Text("%d", c.hits);
                ImGui::TableSetColumnIndex(16);
                if (c.identified && (!c.deviceId.empty() || !c.mac.empty())) {
                    const bool forcePending = !c.forceTargetId.empty() &&
                                              (c.forceTargetStatus == "queued" ||
                                               c.forceTargetStatus == "offered");
                    if (forcePending) {
                        ImGui::TextColored(OpsTone::Warn(), "推#%u",
                                           c.forceTargetBuildId > 0 ? c.forceTargetBuildId
                                                                    : st.latestClientBuildId);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "指定推送排队中 build %u\n状态 %s\n点右键可取消；仅此设备\n"
                                "队列在更新服务内存中，重启服务会丢",
                                c.forceTargetBuildId, c.forceTargetStatus.c_str());
                        }
                        if (ImGui::BeginPopupContextItem("force_tgt")) {
                            if (ImGui::MenuItem("取消此设备推送")) {
                                std::string err;
                                if (PostForceTargetCancel(st, c, err)) {
                                    SetStatus(st, "已取消指定推送");
                                    RefreshClients(st, true);
                                } else {
                                    SetStatus(st, err);
                                }
                            }
                            ImGui::EndPopup();
                        }
                    } else {
                        const bool blockByGlobal = st.forcedClientBuildId > 0;
                        if (st.latestClientBuildId == 0 || blockByGlobal) ImGui::BeginDisabled();
                        if (SafeSmallButton("推更")) {
                            std::string err;
                            std::string tip;
                            if (PostForceTarget(st, c, err, &tip)) {
                                std::string msg =
                                    "已排队指定推送（仅此设备；重启更新服务会丢队列）";
                                if (!tip.empty()) msg += "；" + tip;
                                SetStatus(st, msg);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (st.latestClientBuildId == 0 || blockByGlobal) ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            if (blockByGlobal) {
                                ImGui::SetTooltip(
                                    "请先取消全体强制更新（强制#%u），否则其他设备仍会一起更新",
                                    st.forcedClientBuildId);
                            } else {
                                ImGui::SetTooltip(
                                    "只推这一台到最新包 build %u\n不会写全体 force-update.json\n"
                                    "队列在更新服务内存中，重启服务会丢",
                                    st.latestClientBuildId);
                            }
                        }
                    }
                    ImGui::SameLine(0, 4.f);
                    const bool fetchPending = !c.logFetchId.empty() &&
                                              (c.logFetchStatus == "queued" ||
                                               c.logFetchStatus == "offered" ||
                                               c.logFetchStatus == "acked");
                    if (fetchPending) {
                        const char* label = "轻量…";
                        if (c.logFetchStatus == "acked")
                            label = c.logFetchMode == "full" ? "上传全…" : "上传中…";
                        else if (c.logFetchMode == "full")
                            label = "全量…";
                        ImGui::TextDisabled("%s", label);
                        if (ImGui::IsItemHovered()) {
                            const char* stHint = "排队中";
                            if (c.logFetchStatus == "offered") stHint = "已下发，等客户端开传";
                            if (c.logFetchStatus == "acked") stHint = "客户端已开传（上传中）";
                            ImGui::SetTooltip(
                                "mode=%s\n状态 %s（%s）\nid %s\n"
                                "完成需新客户端 Done；旧端约 1 分钟后队列自清",
                                c.logFetchMode.c_str(), c.logFetchStatus.c_str(), stHint,
                                c.logFetchId.c_str());
                        }
                    } else {
                        if (SafeSmallButton("轻志")) {
                            std::string err;
                            if (PostLogFetch(st, c, "light", err)) {
                                SetStatus(st, "已请求轻量日志（约 15s 内探活上报）");
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("FETCH 轻量：各频道最近约 10 卷");
                        ImGui::SameLine(0, 2.f);
                        if (SafeSmallButton("全志")) {
                            std::string err;
                            if (PostLogFetch(st, c, "full", err)) {
                                SetStatus(st, "已请求全量日志（约 15s 内探活上报）");
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("FETCH 全量：各频道最多约 360 卷");
                    }
                    ImGui::SameLine(0, 4.f);
                    if (c.banned) {
                        if (NeutralSmallButton("解禁")) {
                            std::string err;
                            if (PostBanAction(st, "unban", c.machine, c.deviceId, {}, {}, err,
                                              c.mac, {}, {}, c.uid)) {
                                SetStatus(st, "已解禁 " + (!c.uid.empty()
                                                              ? ("uid " + c.uid)
                                                              : (c.device.empty() ? c.machine
                                                                                  : c.device)));
                                RefreshBans(st, true);
                                RefreshClients(st, true);
                            } else {
                                SetStatus(st, err);
                            }
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(!c.uid.empty()
                                                  ? "按签名 uid 解禁（本人所有设备一起解）"
                                                  : "按硬件解禁");
                        }
                    } else if (DangerSmallButton("禁止")) {
                        std::string err;
                        if (PostBanAction(st, "ban", c.machine, c.deviceId, "ops ban", {}, err,
                                          c.mac, {}, {}, c.uid)) {
                            SetStatus(st, "已禁止 " + (!c.uid.empty()
                                                          ? ("uid " + c.uid)
                                                          : (c.device.empty() ? c.machine
                                                                              : c.device)));
                            RefreshBans(st, true);
                            RefreshClients(st, true);
                        } else {
                            SetStatus(st, err);
                        }
                    }
                    if (ImGui::IsItemHovered() && !c.banned) {
                        ImGui::SetTooltip(!c.uid.empty()
                                              ? "按签名 uid 封禁：改硬件码/换 MAC/清目录都拦得住"
                                              : "此端无签名 TOKEN，只能按硬件封（可被改码绕过）");
                    }
                    ImGui::SameLine();
                    if (c.allowed) {
                        if (NeutralSmallButton("移出")) {
                            std::string err;
                            if (PostBanAction(st, "unallow", c.machine, c.deviceId, {}, {}, err,
                                              c.mac, {}, {}, c.uid)) {
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
                                          c.mac, {}, {}, c.uid)) {
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
                }  // members in ip bucket
                }  // ip buckets
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
                                          ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate,
                                      ImVec2(0, denyH))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 130.f, 1);
                    ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 110.f, 2);
                    ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 1.0f, 3);
                    ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 100.f, 4);
                    ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 88.f, 5);
                    ImGui::TableSetupColumn("匹配", ImGuiTableColumnFlags_WidthFixed, 80.f, 6);
                    ImGui::TableSetupColumn("原因", ImGuiTableColumnFlags_WidthStretch, 1.3f, 7);
                    ImGui::TableHeadersRow();
                    std::vector<size_t> denyOrder(st.recentDenies.size());
                    for (size_t i = 0; i < denyOrder.size(); ++i) denyOrder[i] = i;
                    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                        StableSortByTableSpecs(denyOrder, specs, [&](size_t ia, size_t ib, ImGuiID col) {
                            const auto& a = st.recentDenies[ia];
                            const auto& b = st.recentDenies[ib];
                            switch (col) {
                                case 1:
                                    return CmpStrField(a.at, b.at);
                                case 2:
                                    return CmpStrField(a.ip, b.ip);
                                case 3:
                                    return CmpStrField(a.machine, b.machine);
                                case 4:
                                    return CmpStrField(a.mac, b.mac);
                                case 5:
                                    return CmpStrField(a.token, b.token);
                                case 6:
                                    return CmpStrField(a.match, b.match);
                                case 7:
                                    return CmpStrField(a.reason, b.reason);
                                default:
                                    return 0;
                            }
                        });
                    }
                    for (size_t oi = 0; oi < denyOrder.size(); ++oi) {
                        const size_t i = denyOrder[oi];
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

    // ── 历史客户端 / 租约预警：在线表只有「此刻在线」，掉租约的人恰恰是「没上线」的那批 ──
    {
        int riskN = 0;
        for (const auto& h : st.clientHistory) {
            if (!h.online && h.leaseRemainSec < 12 * 3600) ++riskN;
        }
        char histHdr[80]{};
        if (riskN > 0)
            std::snprintf(histHdr, sizeof(histHdr), "历史客户端 · 租约预警 %d##client_hist", riskN);
        else
            std::snprintf(histHdr, sizeof(histHdr), "历史客户端##client_hist");
        ImGui::SetNextItemOpen(riskN > 0, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(histHdr)) {
            if (st.lastClientHistoryFetchMs == 0) RefreshClientHistory(st, true);
            ImGui::TextDisabled(
                "落盘台账（跨重启保留）。客户端每 64h 需成功探活一次续租约；"
                "租约过期时若正赶上服务没开，会被拒启且无宽限可用。");
            if (ImGui::SmallButton("刷新##hist")) RefreshClientHistory(st, true);
            ImGui::SameLine(0, 12.f);
            ImGui::Checkbox("只看快掉租约的##hist_risk", &st.clientHistoryOnlyRisk);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("离线且租约剩余 <12 小时的：这些人下次启动最可能被拒。");
            ImGui::SameLine(0, 12.f);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::InputInt("天##hist_days", &st.clientHistoryDays)) {
                if (st.clientHistoryDays < 1) st.clientHistoryDays = 1;
                if (st.clientHistoryDays > 365) st.clientHistoryDays = 365;
                RefreshClientHistory(st, true);
            }
            ImGui::SameLine(0, 12.f);
            ImGui::TextDisabled("共 %d 条", st.clientHistoryTotal);
            if (!st.clientHistoryError.empty()) {
                ImGui::SameLine(0, 12.f);
                ImGui::TextColored(OpsTone::Danger(), "%s", st.clientHistoryError.c_str());
            }

            const float histH = (std::min)(180.f, belowH * 0.38f);
            if (ImGui::BeginTable("client_hist", 7,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, histH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("uid", ImGuiTableColumnFlags_WidthFixed, 100.f);
                ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("角色", ImGuiTableColumnFlags_WidthFixed, 90.f);
                ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthFixed, 70.f);
                ImGui::TableSetupColumn("最后见到", ImGuiTableColumnFlags_WidthFixed, 130.f);
                ImGui::TableSetupColumn("租约剩余", ImGuiTableColumnFlags_WidthFixed, 100.f);
                ImGui::TableSetupColumn("最后拒绝", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();

                int shown = 0;
                for (size_t hi = 0; hi < st.clientHistory.size(); ++hi) {
                    const auto& h = st.clientHistory[hi];
                    const bool risk = !h.online && h.leaseRemainSec < 12 * 3600;
                    if (st.clientHistoryOnlyRisk && !risk) continue;
                    ++shown;
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(hi));

                    ImGui::TableSetColumnIndex(0);
                    if (h.uid.empty())
                        ImGui::TextDisabled("—");
                    else if (h.online)
                        ImGui::TextColored(OpsTone::Ok(), "%s", h.uid.c_str());
                    else
                        ImGui::TextUnformatted(h.uid.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\ndeviceId=%s\nIP=%s",
                                          h.online ? "此刻在线" : "当前离线",
                                          h.deviceId.empty() ? "—" : h.deviceId.c_str(),
                                          h.ip.empty() ? "—" : h.ip.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(h.machine.empty() ? "—" : h.machine.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(h.charName.empty() ? "—" : h.charName.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("%s", h.appVersion.empty() ? "—" : h.appVersion.c_str());

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextDisabled("%s", h.lastSeenAt.empty()
                                                  ? "—"
                                                  : h.lastSeenAt.substr(0, 16).c_str());

                    ImGui::TableSetColumnIndex(5);
                    const std::string lease = LeaseRemainText(h.leaseRemainSec);
                    if (h.online)
                        ImGui::TextColored(OpsTone::Ok(), "%s", lease.c_str());
                    else if (h.leaseRemainSec <= 0)
                        ImGui::TextColored(OpsTone::Danger(), "%s", lease.c_str());
                    else if (risk)
                        ImGui::TextColored(OpsTone::Warn(), "%s", lease.c_str());
                    else
                        ImGui::TextUnformatted(lease.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("按最后一次放行 + 64h 估算。\n最后放行：%s",
                                          h.lastAllowAt.empty() ? "（无记录）"
                                                                : h.lastAllowAt.c_str());

                    ImGui::TableSetColumnIndex(6);
                    if (h.lastDenyMatch.empty() && h.lastDenyReason.empty()) {
                        ImGui::TextDisabled("—");
                    } else {
                        ImGui::TextColored(OpsTone::Danger(), "%s %s", h.lastDenyMatch.c_str(),
                                           h.lastDenyReason.c_str());
                    }
                    ImGui::PopID();
                }
                if (shown == 0) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled(st.clientHistoryOnlyRisk
                                            ? "（没有快掉租约的客户端）"
                                            : "（暂无历史；重启更新服务后开始积累）");
                }
                ImGui::EndTable();
            }
        }
    }

    if (!st.forceTargetQueue.empty() || !st.forceTargetQueueError.empty()) {
        char ftHdr[64]{};
        std::snprintf(ftHdr, sizeof(ftHdr), "指定推送排队 (%zu)##force_tgt_q",
                      st.forceTargetQueue.size());
        if (ImGui::CollapsingHeader(ftHdr)) {
            ImGui::TextDisabled("内存队列；重启更新服务会丢。全局强制存在时禁止新排队。");
            if (!st.forceTargetQueueError.empty()) {
                ImGui::TextColored(OpsTone::Warn(), "%s", st.forceTargetQueueError.c_str());
            }
            const float qH = (std::min)(120.f, belowH * 0.22f);
            if (ImGui::BeginTable("force_tgt_queue", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, qH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("目标", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("build", ImGuiTableColumnFlags_WidthFixed, 56.f);
                ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 72.f);
                ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 130.f);
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 56.f);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < st.forceTargetQueue.size(); ++i) {
                    const auto& j = st.forceTargetQueue[i];
                    ImGui::PushID(static_cast<int>(i) + 51000);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    std::string target = j.machine;
                    if (target.empty()) target = j.deviceId;
                    if (target.empty()) target = j.mac;
                    if (target.empty()) target = j.id;
                    ImGui::TextUnformatted(target.c_str());
                    if (ImGui::IsItemHovered() && !j.note.empty()) {
                        ImGui::SetTooltip("%s\nid=%s", j.note.c_str(), j.id.c_str());
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", j.buildId);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(j.status.empty() ? "—" : j.status.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(j.at.empty() ? "—" : j.at.c_str());
                    ImGui::TableSetColumnIndex(4);
                    if (SafeSmallButton("取消")) {
                        OpsState::ConnectedClient tmp;
                        tmp.forceTargetId = j.id;
                        std::string err;
                        if (PostForceTargetCancel(st, tmp, err)) {
                            SetStatus(st, "已取消指定推送 " + j.id);
                            RefreshClients(st, true);
                        } else {
                            SetStatus(st, err);
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    if (alertN > 0 || !st.ipAlerts.empty()) {
        char alertHdr[80]{};
        const int listedN = static_cast<int>(st.ipAlerts.size());
        if (alertN > listedN && listedN > 0) {
            std::snprintf(alertHdr, sizeof(alertHdr), "同 IP 多设备告警 (%d，列%d)##ipalerts_v2",
                          alertN, listedN);
        } else {
            std::snprintf(alertHdr, sizeof(alertHdr), "同 IP 多设备告警 (%d)##ipalerts_v2", alertN);
        }
        // 状态条已有计数：默认折叠；点状态条「同IP告警」可强制展开。
        if (st.forceOpenIpAlerts) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            st.forceOpenIpAlerts = false;
        } else {
            ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        }
        if (ImGui::CollapsingHeader(alertHdr)) {
            if (alertN > listedN && listedN > 0) {
                ImGui::TextDisabled(
                    "历史见过的 deviceId/MAC 数≥2（NAT/多机）。表内仅列前 %d / 共 %d（防响应过大）",
                    listedN, alertN);
            } else {
                ImGui::TextDisabled("历史见过的 deviceId/MAC 数≥2，可能是 NAT 或多机泄露");
            }
            const float alertH = (std::min)(130.f, belowH * 0.28f);
            if (ImGui::BeginTable("ip_alerts", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate,
                                  ImVec2(0, alertH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 120.f, 1);
                ImGui::TableSetupColumn("归属地", ImGuiTableColumnFlags_WidthStretch, 1.6f, 2);
                ImGui::TableSetupColumn("设备数", ImGuiTableColumnFlags_WidthFixed, 56.f, 3);
                ImGui::TableSetupColumn("摘要", ImGuiTableColumnFlags_WidthStretch, 2.2f, 4);
                ImGui::TableHeadersRow();
                std::vector<size_t> alertOrder(st.ipAlerts.size());
                for (size_t i = 0; i < alertOrder.size(); ++i) alertOrder[i] = i;
                if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                    StableSortByTableSpecs(alertOrder, specs, [&](size_t ia, size_t ib, ImGuiID col) {
                        const auto& a = st.ipAlerts[ia];
                        const auto& b = st.ipAlerts[ib];
                        switch (col) {
                            case 1:
                                return CmpStrField(a.ip, b.ip);
                            case 2:
                                return CmpStrField(a.geo, b.geo);
                            case 3:
                                return CmpIntField(a.deviceCount, b.deviceCount);
                            case 4:
                                return CmpStrField(a.summary, b.summary);
                            default:
                                return 0;
                        }
                    });
                }
                for (size_t oi = 0; oi < alertOrder.size(); ++oi) {
                    const size_t i = alertOrder[oi];
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
            const ImGuiTableFlags listFlags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
            const float listH = (std::max)(80.f, ImGui::GetContentRegionAvail().y);
            if (ImGui::BeginTable("bans_table", 6, listFlags, ImVec2(0, listH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthStretch, 1.2f, 1);
                ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 0.9f, 2);
                ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 96.f, 3);
                ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 72.f, 4);
                ImGui::TableSetupColumn("原因", ImGuiTableColumnFlags_WidthStretch, 0.9f, 5);
                ImGui::TableSetupColumn("操作",
                                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                        52.f, 6);
                ImGui::TableHeadersRow();
                if (st.bans.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("(无封禁 · 可从在线表/拒绝记录右键填入)");
                } else {
                    std::vector<size_t> banOrder(st.bans.size());
                    for (size_t i = 0; i < banOrder.size(); ++i) banOrder[i] = i;
                    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                        StableSortByTableSpecs(banOrder, specs, [&](size_t ia, size_t ib, ImGuiID col) {
                            return CompareBannedDevice(st.bans[ia], st.bans[ib], col);
                        });
                    }
                    for (size_t oi = 0; oi < banOrder.size(); ++oi) {
                        const size_t idx = banOrder[oi];
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
            const ImGuiTableFlags listFlags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
            const float listH = (std::max)(80.f, ImGui::GetContentRegionAvail().y);
            if (ImGui::BeginTable("allows_table", 6, listFlags, ImVec2(0, listH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthStretch, 1.2f, 1);
                ImGui::TableSetupColumn("计算机", ImGuiTableColumnFlags_WidthStretch, 0.9f, 2);
                ImGui::TableSetupColumn("MAC", ImGuiTableColumnFlags_WidthFixed, 96.f, 3);
                ImGui::TableSetupColumn("TOKEN", ImGuiTableColumnFlags_WidthFixed, 72.f, 4);
                ImGui::TableSetupColumn("备注", ImGuiTableColumnFlags_WidthStretch, 0.9f, 5);
                ImGui::TableSetupColumn("操作",
                                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                        52.f, 6);
                ImGui::TableHeadersRow();
                if (st.allows.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("(无白名单 · 可从在线表右键填入)");
                } else {
                    std::vector<size_t> allowOrder(st.allows.size());
                    for (size_t i = 0; i < allowOrder.size(); ++i) allowOrder[i] = i;
                    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                        StableSortByTableSpecs(allowOrder, specs, [&](size_t ia, size_t ib, ImGuiID col) {
                            return CompareBannedDevice(st.allows[ia], st.allows[ib], col);
                        });
                    }
                    for (size_t oi = 0; oi < allowOrder.size(); ++oi) {
                        const size_t idx = allowOrder[oi];
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

void FormatMesoUll(unsigned long long v, char* out, size_t n) {
    FormatMesoDisplay(std::to_string(v), out, n);
}

void FormatMesoDelta(unsigned long long first, unsigned long long last, char* out, size_t n) {
    if (last == first) {
        std::snprintf(out, n, "持平");
        return;
    }
    const bool up = last > first;
    const unsigned long long mag = up ? (last - first) : (first - last);
    char magBuf[48]{};
    FormatMesoUll(mag, magBuf, sizeof(magBuf));
    std::snprintf(out, n, "%s%s", up ? "+" : "-", magBuf);
}

const char* MesoKindLabel(const std::string& kind) {
    if (kind == "outflow") return "外转嫌疑";
    if (kind == "token_xfer") return "跨号转移";
    if (kind == "char_move") return "同号搬仓";
    if (kind == "reconnect_drop") return "重连骤降";
    if (kind == "inflow") return "进账";
    if (kind == "scroll_outflow") return "卷轴外转";
    if (kind == "scroll_xfer") return "卷轴跨号";
    if (kind == "scroll_move") return "卷轴搬仓";
    if (kind == "scroll_reconnect") return "卷轴骤降";
    if (kind == "scroll_inflow") return "卷轴进账";
    return kind.c_str();
}

void FormatWallHms(ULONGLONG wallMs, char* buf, size_t n) {
    const time_t sec = static_cast<time_t>(wallMs / 1000ull);
    std::tm t{};
    if (!BeijingTm(sec, t)) {
        std::snprintf(buf, n, "--:--:--");
        return;
    }
    std::strftime(buf, n, "%H:%M:%S", &t);
}

void FormatWallTick(ULONGLONG wallMs, int windowMin, char* buf, size_t n) {
    const time_t sec = static_cast<time_t>(wallMs / 1000ull);
    std::tm t{};
    if (!BeijingTm(sec, t)) {
        std::snprintf(buf, n, "--:--");
        return;
    }
    if (windowMin >= 1440)
        std::strftime(buf, n, "%m-%d %H:%M", &t);
    else if (windowMin >= 180)
        std::strftime(buf, n, "%H:%M", &t);
    else
        std::strftime(buf, n, "%H:%M:%S", &t);
}

ImU32 MesoSeriesColor(const std::string& token, bool online, float alpha = 1.f) {
    uint32_t h = 2166136261u;
    for (unsigned char c : token) {
        h ^= c;
        h *= 16777619u;
    }
    const float hue = (static_cast<int>(h % 330) + 15) / 360.f;
    const float sat = online ? 0.70f : 0.22f;
    const float val = OpsIsLight() ? (online ? 0.62f : 0.48f) : (online ? 0.92f : 0.55f);
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB(hue, sat, val, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

ImU32 MesoTotalColor() {
    const ImVec4 c = OpsTone::Warn();
    return ImGui::ColorConvertFloat4ToU32(c);
}

double MesoNiceStep(double range, int ticks) {
    if (range <= 0.0) return 1.0;
    if (ticks < 2) ticks = 2;
    const double raw = range / static_cast<double>(ticks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / mag;
    if (n < 1.5) return mag;
    if (n < 3.5) return 2.0 * mag;
    if (n < 7.5) return 5.0 * mag;
    return 10.0 * mag;
}

bool MesoPointInWindow(const OpsState::MesoDashPoint& p, ULONGLONG t0, ULONGLONG t1) {
    return p.wallMs >= t0 && p.wallMs <= t1;
}

unsigned long long MesoAtOrBefore(const std::deque<OpsState::MesoDashPoint>& pts, ULONGLONG t,
                                  bool* ok) {
    if (ok) *ok = false;
    unsigned long long v = 0;
    for (const auto& p : pts) {
        if (p.wallMs > t) break;
        v = p.meso;
        if (ok) *ok = true;
    }
    return v;
}

bool MesoSampleGap(ULONGLONG earlier, ULONGLONG later) {
    return later > earlier && (later - earlier) > kMesoDashGapMs;
}

void MesoCollectGaps(const std::deque<OpsState::MesoDashPoint>& pts, ULONGLONG t0, ULONGLONG t1,
                     std::vector<std::pair<ULONGLONG, ULONGLONG>>& gaps) {
    gaps.clear();
    const OpsState::MesoDashPoint* prev = nullptr;
    for (const auto& p : pts) {
        if (p.wallMs > t1) break;
        if (prev && MesoSampleGap(prev->wallMs, p.wallMs)) {
            const ULONGLONG a = (std::max)(prev->wallMs, t0);
            const ULONGLONG b = (std::min)(p.wallMs, t1);
            if (b > a) gaps.push_back({a, b});
        }
        prev = &p;
    }
    if (prev && MesoSampleGap(prev->wallMs, t1)) {
        const ULONGLONG a = (std::max)(prev->wallMs, t0);
        if (t1 > a) gaps.push_back({a, t1});
    }
}

bool MesoTimeInGaps(ULONGLONG t, const std::vector<std::pair<ULONGLONG, ULONGLONG>>& gaps) {
    for (const auto& g : gaps) {
        if (t > g.first && t < g.second) return true;
    }
    return false;
}

void MesoDragSplitNS(const char* id, float* topH, float totalH, float minTop, float minBot) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##ns", ImVec2(-FLT_MIN, 8.f));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float midY = (a.y + b.y) * 0.5f;
    dl->AddRectFilled(ImVec2(a.x + 24.f, midY - 1.f), ImVec2(b.x - 24.f, midY + 1.f),
                      ImGui::GetColorU32(hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator));
    if (ImGui::IsItemActive()) *topH += ImGui::GetIO().MouseDelta.y;
    const float maxTop = totalH - minBot;
    if (*topH < minTop) *topH = minTop;
    if (*topH > maxTop) *topH = maxTop;
    ImGui::PopID();
}

void MesoDragSplitEW(const char* id, float* rightW, float rowW, float rowH, float minLeft,
                     float minRight) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##ew", ImVec2(8.f, rowH));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float midX = (a.x + b.x) * 0.5f;
    dl->AddRectFilled(ImVec2(midX - 1.f, a.y + 16.f), ImVec2(midX + 1.f, b.y - 16.f),
                      ImGui::GetColorU32(hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator));
    if (ImGui::IsItemActive()) *rightW -= ImGui::GetIO().MouseDelta.x;
    const float maxRight = rowW - minLeft;
    if (*rightW < minRight) *rightW = minRight;
    if (*rightW > maxRight) *rightW = maxRight;
    ImGui::PopID();
}

void DrawMesoKpiCard(const char* title, const char* value, const char* sub, ImVec4 valueCol,
                     float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, OpsIsLight()
                                                ? ImVec4(1.f, 1.f, 1.f, 1.f)
                                                : ImVec4(0.11f, 0.12f, 0.15f, 1.f));
    ImGui::BeginChild(title, ImVec2(width, 92.f), true, ImGuiWindowFlags_NoScrollbar);
    const char* hash = std::strstr(title, "##");
    if (hash && hash != title) {
        ImGui::TextDisabled("%.*s", static_cast<int>(hash - title), title);
    } else {
        ImGui::TextDisabled("%s", title);
    }
    ImGui::SetWindowFontScale(1.35f);
    ImGui::PushStyleColor(ImGuiCol_Text, valueCol);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.f);
    if (sub && sub[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + width - 16.f);
        ImGui::TextDisabled("%s", sub);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawMesoSparkline(const std::deque<OpsState::MesoDashPoint>& pts, ULONGLONG t0, ULONGLONG t1,
                       ImU32 col, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                      ImGui::GetColorU32(OpsIsLight() ? ImVec4(0.94f, 0.95f, 0.97f, 1.f)
                                                      : ImVec4(0.08f, 0.09f, 0.11f, 1.f)),
                      2.f);
    if (pts.size() < 2 || t1 <= t0) return;
    unsigned long long mn = (std::numeric_limits<unsigned long long>::max)();
    unsigned long long mx = 0;
    int n = 0;
    for (const auto& pt : pts) {
        if (!MesoPointInWindow(pt, t0, t1)) continue;
        mn = (std::min)(mn, pt.meso);
        mx = (std::max)(mx, pt.meso);
        ++n;
    }
    if (n < 2) return;
    if (mx <= mn) mx = mn + 1;
    const int stride = n > 80 ? (n + 79) / 80 : 1;
    ImVec2 prev{};
    bool have = false;
    int seen = 0;
    const OpsState::MesoDashPoint* lastSrc = nullptr;
    for (const auto& pt : pts) {
        if (!MesoPointInWindow(pt, t0, t1)) continue;
        if (lastSrc && MesoSampleGap(lastSrc->wallMs, pt.wallMs)) have = false;
        lastSrc = &pt;
        const bool take = (seen % stride == 0) || (seen + 1 == n);
        ++seen;
        if (!take) continue;
        const float x = p.x + 1.f +
                        static_cast<float>(static_cast<double>(pt.wallMs - t0) /
                                           static_cast<double>(t1 - t0)) *
                            (size.x - 2.f);
        const float y = p.y + size.y - 2.f -
                        static_cast<float>(pt.meso - mn) / static_cast<float>(mx - mn) * (size.y - 4.f);
        const ImVec2 cur(x, y);
        if (have) dl->AddLine(prev, cur, col, 1.4f);
        prev = cur;
        have = true;
    }
}

void DrawMesoPlot(OpsState& st, const std::vector<size_t>& visIdx, ULONGLONG t0, ULONGLONG t1,
                  ImVec2 size) {
    ImGui::InvisibleButton("meso_plot_hit", size);
    const ImVec2 r0 = ImGui::GetItemRectMin();
    const ImVec2 r1 = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 bg = ImGui::GetColorU32(OpsIsLight() ? ImVec4(0.97f, 0.98f, 0.99f, 1.f)
                                                     : ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    const ImU32 grid = ImGui::GetColorU32(OpsIsLight() ? ImVec4(0.82f, 0.84f, 0.88f, 0.85f)
                                                       : ImVec4(1.f, 1.f, 1.f, 0.08f));
    const ImU32 axis = ImGui::GetColorU32(OpsTone::Muted());
    dl->AddRectFilled(r0, r1, bg, 6.f);
    dl->AddRect(r0, r1, ImGui::GetColorU32(ImGuiCol_Border), 6.f);

    const float padL = 78.f, padR = 14.f, padT = 14.f, padB = 30.f;
    const float x0 = r0.x + padL, x1 = r1.x - padR;
    const float y0 = r0.y + padT, y1 = r1.y - padB;
    if (x1 - x0 < 40.f || y1 - y0 < 40.f) return;

    double yMin = 0.0, yMax = 1.0;
    bool any = false;
    unsigned long long dataMin = (std::numeric_limits<unsigned long long>::max)();
    unsigned long long dataMax = 0;
    auto consider = [&](const std::deque<OpsState::MesoDashPoint>& pts) {
        for (const auto& p : pts) {
            if (!MesoPointInWindow(p, t0, t1)) continue;
            dataMin = (std::min)(dataMin, p.meso);
            dataMax = (std::max)(dataMax, p.meso);
            any = true;
        }
    };
    if (st.mesoDashShowTotal) consider(st.mesoDashTotal);
    for (size_t idx : visIdx) consider(st.mesoDashSeries[idx].points);

    if (any) {
        if (st.mesoDashYFromZero) {
            yMin = 0.0;
            yMax = static_cast<double>(dataMax);
        } else {
            yMin = static_cast<double>(dataMin);
            yMax = static_cast<double>(dataMax);
        }
        if (yMax <= yMin) yMax = yMin + 1.0;
        yMax *= 1.08;
    }

    const double step = MesoNiceStep(yMax - yMin, 5);
    const double tick0 = std::floor(yMin / step) * step;
    for (double tv = tick0; tv <= yMax + step * 0.01; tv += step) {
        if (tv < yMin - step * 0.01) continue;
        const float y = y1 - static_cast<float>((tv - yMin) / (yMax - yMin)) * (y1 - y0);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), grid, 1.f);
        char lab[48]{};
        if (tv <= 0.0)
            std::snprintf(lab, sizeof(lab), "0");
        else
            FormatMesoUll(static_cast<unsigned long long>(tv + 0.5), lab, sizeof(lab));
        dl->AddText(ImVec2(r0.x + 6.f, y - ImGui::GetTextLineHeight() * 0.5f), axis, lab);
    }

    const int stepSec = st.mesoDashWindowMin <= 10     ? 60
                        : st.mesoDashWindowMin <= 30   ? 300
                        : st.mesoDashWindowMin <= 60   ? 600
                        : st.mesoDashWindowMin <= 180  ? 1800
                        : st.mesoDashWindowMin <= 1440 ? 7200
                                                       : 43200;
    const ULONGLONG stepMs = static_cast<ULONGLONG>(stepSec) * 1000ull;
    ULONGLONG tick = ((t0 + stepMs - 1) / stepMs) * stepMs;
    for (; tick <= t1; tick += stepMs) {
        const float x = x0 + static_cast<float>(static_cast<double>(tick - t0) /
                                               static_cast<double>(t1 - t0)) *
                                 (x1 - x0);
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), grid, 1.f);
        char lab[20]{};
        FormatWallTick(tick, st.mesoDashWindowMin, lab, sizeof(lab));
        const float tw = ImGui::CalcTextSize(lab).x;
        dl->AddText(ImVec2(x - tw * 0.5f, y1 + 6.f), axis, lab);
    }

    dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), axis, 1.2f);
    dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y1), axis, 1.2f);

    auto xOf = [&](ULONGLONG t) -> float {
        if (t <= t0) return x0;
        if (t >= t1) return x1;
        return x0 + static_cast<float>(static_cast<double>(t - t0) / static_cast<double>(t1 - t0)) *
                        (x1 - x0);
    };
    auto yOf = [&](unsigned long long v) -> float {
        const double u = (static_cast<double>(v) - yMin) / (yMax - yMin);
        return y1 - static_cast<float>(u) * (y1 - y0);
    };

    const std::deque<OpsState::MesoDashPoint>* gapSrc = &st.mesoDashTotal;
    if (gapSrc->empty() && !visIdx.empty()) gapSrc = &st.mesoDashSeries[visIdx[0]].points;
    std::vector<std::pair<ULONGLONG, ULONGLONG>> gaps;
    MesoCollectGaps(*gapSrc, t0, t1, gaps);

    ImGui::PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);

    const ImU32 gapBg = ImGui::GetColorU32(OpsIsLight() ? ImVec4(0.78f, 0.80f, 0.84f, 0.40f)
                                                       : ImVec4(1.f, 1.f, 1.f, 0.055f));
    const ImU32 gapTx = ImGui::GetColorU32(OpsTone::Muted());
    for (const auto& g : gaps) {
        const float xa = xOf(g.first);
        const float xb = xOf(g.second);
        if (xb - xa < 2.f) continue;
        dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), gapBg);
        if (xb - xa >= 40.f && (g.second - g.first) >= 60000ull) {
            const char* gl = "断连";
            const ImVec2 ts = ImGui::CalcTextSize(gl);
            if (ts.x + 8.f < xb - xa)
                dl->AddText(ImVec2((xa + xb - ts.x) * 0.5f, y0 + 6.f), gapTx, gl);
        }
    }

    auto emitBroken = [&](const std::deque<OpsState::MesoDashPoint>& pts, ImU32 col, float thick,
                          bool fillArea, bool endDot) {
        int inN = 0;
        for (const auto& p : pts) {
            if (p.wallMs < t0) continue;
            if (p.wallMs > t1) break;
            ++inN;
        }
        if (inN < 1) return;
        const int stride = inN > 1600 ? (inN + 1599) / 1600 : 1;
        ImVector<ImVec2> poly;
        ImVector<ImVec2> fill;
        const OpsState::MesoDashPoint* prevP = nullptr;
        const OpsState::MesoDashPoint* lastIn = nullptr;
        int seen = 0;
        auto flush = [&]() {
            if (fillArea && poly.Size >= 2 && fill.Size >= 2) {
                fill.push_back(ImVec2(fill.back().x, y1));
                const ImU32 fillCol = ImGui::ColorConvertFloat4ToU32(
                    OpsIsLight() ? ImVec4(0.85f, 0.62f, 0.18f, 0.16f)
                                 : ImVec4(0.95f, 0.72f, 0.35f, 0.14f));
                if (fill.Size >= 3) dl->AddConcavePolyFilled(fill.Data, fill.Size, fillCol);
            }
            if (poly.Size >= 2) dl->AddPolyline(poly.Data, poly.Size, col, 0, thick);
            else if (poly.Size == 1) dl->AddCircleFilled(poly[0], 2.2f, col);
            poly.clear();
            fill.clear();
        };
        for (const auto& p : pts) {
            if (p.wallMs > t1) break;
            if (p.wallMs < t0) {
                prevP = &p;
                continue;
            }
            lastIn = &p;
            if (prevP && MesoSampleGap(prevP->wallMs, p.wallMs)) flush();
            prevP = &p;
            const bool take = poly.empty() || (seen % stride == 0);
            ++seen;
            if (!take) continue;
            const ImVec2 pt(xOf(p.wallMs), yOf(p.meso));
            if (fillArea && poly.empty()) fill.push_back(ImVec2(pt.x, y1));
            poly.push_back(pt);
            if (fillArea) fill.push_back(pt);
        }
        if (lastIn) {
            const ImVec2 pt(xOf(lastIn->wallMs), yOf(lastIn->meso));
            if (poly.empty() || poly.back().x != pt.x || poly.back().y != pt.y) {
                if (fillArea && poly.empty()) fill.push_back(ImVec2(pt.x, y1));
                poly.push_back(pt);
                if (fillArea) fill.push_back(pt);
            }
        }
        flush();
        if (endDot && lastIn)
            dl->AddCircleFilled(ImVec2(xOf(lastIn->wallMs), yOf(lastIn->meso)),
                                thick > 2.5f ? 3.6f : 2.4f, col);
    };

    if (st.mesoDashShowTotal && !st.mesoDashTotal.empty())
        emitBroken(st.mesoDashTotal, MesoTotalColor(), 2.6f, true, false);

    for (size_t n = 0; n < visIdx.size(); ++n) {
        const auto& s = st.mesoDashSeries[visIdx[n]];
        if (s.points.empty()) continue;
        const bool hi = st.mesoDashHoverSeries == static_cast<int>(visIdx[n]);
        const float thick = hi ? 3.2f : 1.8f;
        const ImU32 col = MesoSeriesColor(s.token, s.online, hi ? 1.f : 0.92f);
        emitBroken(s.points, col, thick, false, true);
    }

    ULONGLONG hoverT = 0;
    if (hovered && t1 > t0) {
        const float mx = ImGui::GetIO().MousePos.x;
        float u = (mx - x0) / (x1 - x0);
        if (u < 0.f) u = 0.f;
        if (u > 1.f) u = 1.f;
        hoverT = t0 + static_cast<ULONGLONG>(static_cast<double>(u) * static_cast<double>(t1 - t0));
        const float hx = xOf(hoverT);
        dl->AddLine(ImVec2(hx, y0), ImVec2(hx, y1),
                    ImGui::GetColorU32(OpsIsLight() ? ImVec4(0.2f, 0.2f, 0.25f, 0.45f)
                                                    : ImVec4(1.f, 1.f, 1.f, 0.28f)),
                    1.f);
    }
    ImGui::PopClipRect();

    {
        float ax = x0;
        const float ay = r0.y + 3.f;
        if (st.mesoDashShowTotal) {
            dl->AddCircleFilled(ImVec2(ax + 6.f, ay + ImGui::GetTextLineHeight() * 0.45f), 4.f,
                                MesoTotalColor());
            dl->AddText(ImVec2(ax + 14.f, ay), axis, "合计");
        }
        if (!st.mesoDashTotal.empty()) {
            const ULONGLONG last = st.mesoDashTotal.back().wallMs;
            const ULONGLONG age = t1 > last ? t1 - last : 0;
            char ageLab[48]{};
            if (age > kMesoDashGapMs)
                std::snprintf(ageLab, sizeof(ageLab), "上次采样 %llu 秒前",
                              static_cast<unsigned long long>(age / 1000ull));
            else
                std::snprintf(ageLab, sizeof(ageLab), "采样中");
            const ImVec2 ts = ImGui::CalcTextSize(ageLab);
            dl->AddText(ImVec2(r1.x - padR - ts.x, ay),
                        age > kMesoDashGapMs ? ImGui::GetColorU32(OpsTone::Warn()) : axis, ageLab);
        }
    }

    if (!any) {
        const char* empty = gaps.empty() ? "等待采样或历史为空。约 5 秒一点，已落盘 7 天。"
                                         : "断连期间无采样。关服后折线留白，底账不掉。";
        const ImVec2 ts = ImGui::CalcTextSize(empty);
        dl->AddText(ImVec2((x0 + x1 - ts.x) * 0.5f, (y0 + y1 - ts.y) * 0.5f), axis, empty);
        return;
    }

    if (hovered && hoverT != 0) {
        char tlab[24]{};
        FormatWallTick(hoverT, st.mesoDashWindowMin, tlab, sizeof(tlab));
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tlab);
        ImGui::Separator();
        if (MesoTimeInGaps(hoverT, gaps)) {
            ImGui::TextDisabled("断连（更新服务未采样）");
        } else {
            if (st.mesoDashShowTotal) {
                bool ok = false;
                const unsigned long long v = MesoAtOrBefore(st.mesoDashTotal, hoverT, &ok);
                if (ok) {
                    char buf[48]{};
                    FormatMesoUll(v, buf, sizeof(buf));
                    ImGui::TextColored(OpsTone::Warn(), "合计  %s", buf);
                }
            }
            for (size_t idx : visIdx) {
                const auto& s = st.mesoDashSeries[idx];
                bool ok = false;
                const unsigned long long v = MesoAtOrBefore(s.points, hoverT, &ok);
                if (!ok) continue;
                char buf[48]{};
                FormatMesoUll(v, buf, sizeof(buf));
                ImVec4 col = ImGui::ColorConvertU32ToFloat4(MesoSeriesColor(s.token, s.online));
                ImGui::TextColored(col, "%s  %s", s.token.c_str(), buf);
            }
        }
        ImGui::EndTooltip();
    }
}

void DrawMesoDashPanel(OpsState& st) {
    RefreshClients(st, false);

    const ULONGLONG nowMs = MesoDashWallMs();
    const int winMin = (std::max)(1, st.mesoDashWindowMin);
    const ULONGLONG winMs = static_cast<ULONGLONG>(winMin) * 60ull * 1000ull;
    const ULONGLONG t0 = nowMs > winMs ? nowMs - winMs : 0;
    const ULONGLONG t1 = nowMs;

    int nOnline = 0;
    unsigned long long bookTotal = 0;
    for (const auto& s : st.mesoDashSeries) {
        if (s.online) ++nOnline;
        if (bookTotal > (std::numeric_limits<unsigned long long>::max)() - s.lastMeso)
            bookTotal = (std::numeric_limits<unsigned long long>::max)();
        else
            bookTotal += s.lastMeso;
    }
    unsigned long long winFirst = 0, winLast = bookTotal;
    bool haveFirst = false;
    for (const auto& p : st.mesoDashTotal) {
        if (p.wallMs < t0) continue;
        if (!haveFirst) {
            winFirst = p.meso;
            haveFirst = true;
        }
        winLast = p.meso;
    }

    ImGui::TextUnformatted("利润监控");
    ImGui::SameLine();
    ImGui::TextDisabled("按人分组 · 底账含离线 · 关服断线留白 · 流水 30 天");
    ImGui::SameLine(0, 12.f);
    ImGui::TextDisabled("窗口");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.f);
    const char* winLabs[] = {"10 分钟", "30 分钟", "1 小时", "3 小时", "24 小时", "7 天"};
    const int winVals[] = {10, 30, 60, 180, 1440, 10080};
    int winIdx = 1;
    for (int i = 0; i < 6; ++i)
        if (winVals[i] == st.mesoDashWindowMin) winIdx = i;
    if (ImGui::Combo("##meso_win", &winIdx, winLabs, 6)) st.mesoDashWindowMin = winVals[winIdx];
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("折线与名单「增减」共用此时段");
    ImGui::SameLine();
    ImGui::TextDisabled("门槛");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.f);
    const char* thLabs[] = {"≥1万", "≥5万", "≥10万", "≥50万", "≥100万"};
    const unsigned long long thVals[] = {10000ull, 50000ull, 100000ull, 500000ull, 1000000ull};
    int thIdx = 2;
    for (int i = 0; i < 5; ++i)
        if (thVals[i] == st.mesoAlertMin) thIdx = i;
    if (ImGui::Combo("##meso_th", &thIdx, thLabs, 5)) st.mesoAlertMin = thVals[thIdx];
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("金币变化达到此额度才记流水（过滤打怪小额抖动）\n卷轴数量变化 ≥1 即记，不受此门槛");
    ImGui::SameLine();
    ImGui::Checkbox("合计曲线", &st.mesoDashShowTotal);
    ImGui::SameLine();
    ImGui::Checkbox("离线样本", &st.mesoDashShowOffline);
    ImGui::SameLine();
    ImGui::Checkbox("Y 从 0", &st.mesoDashYFromZero);
    ImGui::SameLine();
    if (ImGui::SmallButton("全显##meso")) {
        for (auto& s : st.mesoDashSeries) s.visible = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("全隐##meso")) {
        for (auto& s : st.mesoDashSeries) s.visible = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("清空曲线##meso")) {
        for (auto& s : st.mesoDashSeries) s.points.clear();
        st.mesoDashTotal.clear();
        MesoDashClearFile(st);
        SetStatus(st, "已清空曲线（流水与角色底账未动）");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "只清折线缓存 meso_dash.jsonl\n"
            "流水 meso_events.jsonl 与角色底账 meso_units.json 保留");

    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 8.f;
    const float cardW = (avail - gap * 3.f) / 4.f;
    char v1[48]{}, v3[48]{}, sub3[64]{};
    FormatMesoUll(bookTotal, v1, sizeof(v1));
    FormatMesoDelta(haveFirst ? winFirst : bookTotal, winLast, v3, sizeof(v3));
    std::snprintf(sub3, sizeof(sub3), "近 %d 分钟折线净变化", winMin);
    const ULONGLONG dayCut = nowMs > 86400000ull ? nowMs - 86400000ull : 0;
    const int nOut24 = MesoCountEvents(st, dayCut, 0);
    const int nAbn24 = MesoCountEvents(st, dayCut, 1);
    char v4[48]{};
    std::snprintf(v4, sizeof(v4), "%d", nOut24);
    char sub4[80]{};
    if (st.mesoDashNoToken > 0)
        std::snprintf(sub4, sizeof(sub4), "%d 台无身份未监控", st.mesoDashNoToken);
    else
        std::snprintf(sub4, sizeof(sub4), "近 24h 外转 %d · 异常 %d · 流水 %zu", nOut24, nAbn24,
                      st.mesoEvents.size());

    const ImVec4 deltaCol = (!haveFirst || winLast == winFirst) ? OpsTone::Muted()
                            : (winLast > winFirst)                ? OpsTone::Ok()
                                                                  : OpsTone::Danger();
    DrawMesoKpiCard("底账合计##kpi1", v1, "含离线角色上次探活，关服不掉", OpsTone::Warn(), cardW);
    ImGui::SameLine(0, gap);
    DrawMesoKpiCard("窗口净增##kpi2", v3, sub3, deltaCol, cardW);
    ImGui::SameLine(0, gap);
    DrawMesoKpiCard("外转嫌疑##kpi3", v4, sub4, nOut24 > 0 ? OpsTone::Danger() : OpsTone::Muted(),
                    cardW);
    ImGui::SameLine(0, gap);
    {
        char tokBuf[48]{};
        std::snprintf(tokBuf, sizeof(tokBuf), "%d / %d", nOnline,
                      static_cast<int>(st.mesoDashSeries.size()));
        DrawMesoKpiCard("在线人数##kpi4", tokBuf, "在线 / 已见过（签卡 uid 优先，否则旧 TOKEN）",
                        OpsTone::Token(), cardW);
    }

    if (!st.clientsError.empty()) {
        ImGui::TextColored(OpsTone::DangerSoft(), "%s", st.clientsError.c_str());
    }

    std::vector<size_t> listed;
    listed.reserve(st.mesoDashSeries.size());
    for (size_t i = 0; i < st.mesoDashSeries.size(); ++i) {
        const auto& s = st.mesoDashSeries[i];
        if (!st.mesoDashShowOffline && !s.online) continue;
        if (st.mesoDashFilter[0] && !ContainsIgnoreCase(s.token, st.mesoDashFilter) &&
            !ContainsIgnoreCase(s.uid, st.mesoDashFilter) &&
            !ContainsIgnoreCase(s.chars, st.mesoDashFilter))
            continue;
        listed.push_back(i);
    }
    std::stable_sort(listed.begin(), listed.end(), [&](size_t a, size_t b) {
        const auto& sa = st.mesoDashSeries[a];
        const auto& sb = st.mesoDashSeries[b];
        if (sa.online != sb.online) return sa.online && !sb.online;
        if (sa.lastMeso != sb.lastMeso) return sa.lastMeso > sb.lastMeso;
        return sa.token < sb.token;
    });
    std::vector<size_t> visIdx;
    visIdx.reserve(listed.size());
    for (size_t i : listed)
        if (st.mesoDashSeries[i].visible) visIdx.push_back(i);

    const float restH = (std::max)(1.f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("meso_body", ImVec2(0, restH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float bodyH = ImGui::GetContentRegionAvail().y;
    const float splitNs = 8.f;
    const float evHdrH = ImGui::GetFrameHeightWithSpacing() + 4.f;
    const float minPlot = 180.f;
    const float minEv = 150.f;
    if (st.mesoUiPlotH < minPlot)
        st.mesoUiPlotH = (std::max)(minPlot, bodyH - evHdrH - 200.f - splitNs);
    {
        const float maxPlot = (std::max)(minPlot, bodyH - evHdrH - minEv - splitNs);
        if (st.mesoUiPlotH > maxPlot) st.mesoUiPlotH = maxPlot;
    }
    const float plotH = st.mesoUiPlotH;

    ImGui::BeginChild("meso_plot_row", ImVec2(0, plotH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float rowH = ImGui::GetContentRegionAvail().y;
        const float rowW = ImGui::GetContentRegionAvail().x;
        const float minPlotW = 360.f;
        const float minLegendW = 420.f;
        if (st.mesoUiLegendW < minLegendW) {
            st.mesoUiLegendW = rowW * 0.40f;
            if (st.mesoUiLegendW < minLegendW) st.mesoUiLegendW = minLegendW;
            if (st.mesoUiLegendW > 720.f) st.mesoUiLegendW = 720.f;
        }
        {
            const float maxLeg = (std::max)(minLegendW, rowW - minPlotW - 8.f);
            if (st.mesoUiLegendW > maxLeg) st.mesoUiLegendW = maxLeg;
        }
        const float plotW = (std::max)(80.f, rowW - st.mesoUiLegendW - 8.f);
        ImGui::BeginChild("meso_plot_cell", ImVec2(plotW, rowH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            const ImVec2 sz = ImGui::GetContentRegionAvail();
            DrawMesoPlot(st, visIdx, t0, t1,
                         ImVec2((std::max)(40.f, sz.x), (std::max)(40.f, sz.y)));
        }
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        MesoDragSplitEW("meso_leg_split", &st.mesoUiLegendW, rowW, rowH, minPlotW, minLegendW);
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("meso_legend_cell", ImVec2(st.mesoUiLegendW, rowH), false,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 56.f);
        ImGui::InputTextWithHint("##meso_filter", "筛选身份 / 角色", st.mesoDashFilter,
                                 sizeof(st.mesoDashFilter));
        ImGui::SameLine();
        ImGui::TextDisabled("%d人", static_cast<int>(listed.size()));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.f, 6.f));
        ImGui::BeginChild("meso_legend", ImVec2(0, 0), true);
        if (listed.empty()) {
            ImGui::TextDisabled("暂无样本。有签卡 uid 或旧 TOKEN 的会话会出现在这里。");
        } else if (ImGui::BeginTable("meso_leg_tbl", 7,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_SizingStretchProp |
                                         ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("##vis", ImGuiTableColumnFlags_WidthFixed, 28.f);
            ImGui::TableSetupColumn("身份", ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableSetupColumn("角色", ImGuiTableColumnFlags_WidthStretch, 1.05f);
            ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 52.f);
            ImGui::TableSetupColumn("实时", ImGuiTableColumnFlags_WidthFixed, 100.f);
            ImGui::TableSetupColumn("增减", ImGuiTableColumnFlags_WidthFixed, 88.f);
            ImGui::TableSetupColumn("走势", ImGuiTableColumnFlags_WidthFixed, 100.f);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            {
                const char* tips[7] = {
                    "勾选后左侧折线显示此人",
                    "签卡 uid（绿）；未激活为旧 TOKEN",
                    "该名下角色，斜杠分隔",
                    "在线=当前探活到；离线保留底账与轨迹",
                    "底账金币；离线为上次探活",
                    "当前时间窗口净变化（与顶栏窗口一致）",
                    "当前时间窗口迷你折线",
                };
                for (int col = 0; col < 7; ++col) {
                    ImGui::TableSetColumnIndex(col);
                    ImGui::PushID(col);
                    ImGui::TableHeader(ImGui::TableGetColumnName(col));
                    if (tips[col] && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tips[col]);
                    ImGui::PopID();
                }
            }
            st.mesoDashHoverSeries = -1;
            for (size_t idx : listed) {
                auto& s = st.mesoDashSeries[idx];
                ImGui::PushID(s.token.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("##v", &s.visible);
                ImGui::TableSetColumnIndex(1);
                const ImU32 col = MesoSeriesColor(s.token, s.online);
                const bool hot = s.lastAlertMs >= dayCut && s.lastAlertMs != 0;
                const ImVec4 col4 = hot ? OpsTone::Danger() : ImGui::ColorConvertU32ToFloat4(col);
                {
                    ImDrawList* ldl = ImGui::GetWindowDrawList();
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float y = p.y + ImGui::GetTextLineHeight() * 0.45f;
                    ldl->AddCircleFilled(ImVec2(p.x + 5.f, y), 5.f, col);
                    ImGui::Dummy(ImVec2(14.f, ImGui::GetTextLineHeight()));
                    ImGui::SameLine(0, 4.f);
                }
                ImGui::BeginGroup();
                if (!s.uid.empty()) {
                    ImGui::TextColored(hot ? OpsTone::Danger()
                                           : ImVec4(0.45f, 0.85f, 0.55f, 1.f),
                                       "uid:%s", s.token.c_str());
                } else {
                    ImGui::TextColored(col4, "%s", s.token.c_str());
                    ImGui::SameLine(0, 4.f);
                    ImGui::TextDisabled("旧");
                }
                ImGui::EndGroup();
                if (ImGui::IsItemHovered()) {
                    st.mesoDashHoverSeries = static_cast<int>(idx);
                    ImGui::SetTooltip("%s%s\n%s\n%d 台 · %s%s\n双击只看此项", s.token.c_str(),
                                      s.uid.empty() ? "（旧 TOKEN，尚未签卡）" : "（签卡 uid）",
                                      s.chars.empty() ? "—" : s.chars.c_str(), s.sessions,
                                      s.online ? "在线" : "离线（保留轨迹）",
                                      hot ? "\n近 24h 有外转/骤降告警" : "");
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    for (size_t j = 0; j < st.mesoDashSeries.size(); ++j)
                        st.mesoDashSeries[j].visible = (j == idx);
                }
                ImGui::TableSetColumnIndex(2);
                if (s.chars.empty())
                    ImGui::TextDisabled("—");
                else
                    ImGui::TextUnformatted(s.chars.c_str());
                if (ImGui::IsItemHovered()) st.mesoDashHoverSeries = static_cast<int>(idx);
                ImGui::TableSetColumnIndex(3);
                if (s.online)
                    ImGui::TextColored(OpsTone::Ok(), "在线");
                else
                    ImGui::TextDisabled("离线");
                ImGui::TableSetColumnIndex(4);
                char mesoBuf[48]{};
                FormatMesoUll(s.lastMeso, mesoBuf, sizeof(mesoBuf));
                if (s.online)
                    ImGui::TextUnformatted(mesoBuf);
                else
                    ImGui::TextDisabled("%s", mesoBuf);
                ImGui::TableSetColumnIndex(5);
                unsigned long long f = s.lastMeso, l = s.lastMeso;
                bool hf = false;
                for (const auto& p : s.points) {
                    if (p.wallMs < t0) continue;
                    if (!hf) {
                        f = p.meso;
                        hf = true;
                    }
                    l = p.meso;
                }
                char dBuf[48]{};
                FormatMesoDelta(hf ? f : s.lastMeso, l, dBuf, sizeof(dBuf));
                const ImVec4 dcol = (!hf || l == f) ? OpsTone::Muted()
                                    : (l > f)         ? OpsTone::Ok()
                                                      : OpsTone::Danger();
                ImGui::TextColored(dcol, "%s", dBuf);
                ImGui::TableSetColumnIndex(6);
                DrawMesoSparkline(s.points, t0, t1, col, ImVec2(92.f, 28.f));
                if (ImGui::IsItemHovered()) st.mesoDashHoverSeries = static_cast<int>(idx);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    MesoDragSplitNS("meso_plot_ev_split", &st.mesoUiPlotH, bodyH, minPlot, evHdrH + minEv);

    int evMatch = 0;
    for (const auto& e : st.mesoEvents) {
        const bool ok = st.mesoEventView == 1 ||
                        (st.mesoEventView == 2 &&
                         (e.kind == "inflow" || e.kind == "scroll_inflow")) ||
                        (st.mesoEventView == 0 && MesoKindAbnormal(e.kind));
        if (!ok) continue;
        if (st.mesoDashFilter[0] && !ContainsIgnoreCase(e.token, st.mesoDashFilter) &&
            !ContainsIgnoreCase(e.charName, st.mesoDashFilter) &&
            !ContainsIgnoreCase(e.peerToken, st.mesoDashFilter))
            continue;
        ++evMatch;
    }
    ImGui::TextUnformatted("转移流水");
    ImGui::SameLine();
    ImGui::TextDisabled("%d 条", evMatch);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("落盘 artifacts\\ops_logs\\meso_events.jsonl（最多 30 天）");
    ImGui::SameLine(0, 10.f);
    if (ImGui::RadioButton("异常##ev", st.mesoEventView == 0)) st.mesoEventView = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("全部##ev", st.mesoEventView == 1)) st.mesoEventView = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("进账##ev", st.mesoEventView == 2)) st.mesoEventView = 2;
    ImGui::SameLine();
    if (ImGui::SmallButton("复制流水##ev")) {
        std::string out = "time\tkind\tid\tchar\tbefore\tafter\tdelta\tpeer\tnote\n";
        int n = 0;
        for (auto it = st.mesoEvents.rbegin(); it != st.mesoEvents.rend(); ++it) {
            const auto& e = *it;
            const bool ok = st.mesoEventView == 1 ||
                            (st.mesoEventView == 2 &&
                             (e.kind == "inflow" || e.kind == "scroll_inflow")) ||
                            (st.mesoEventView == 0 && MesoKindAbnormal(e.kind));
            if (!ok) continue;
            if (st.mesoDashFilter[0] && !ContainsIgnoreCase(e.token, st.mesoDashFilter) &&
                !ContainsIgnoreCase(e.charName, st.mesoDashFilter) &&
                !ContainsIgnoreCase(e.peerToken, st.mesoDashFilter))
                continue;
            char tlab[24]{};
            FormatWallTick(e.wallMs, 1440, tlab, sizeof(tlab));
            out += tlab;
            out += '\t';
            out += MesoKindLabel(e.kind);
            out += '\t';
            out += e.token;
            out += '\t';
            out += e.charName;
            out += '\t';
            out += std::to_string(e.before);
            out += '\t';
            out += std::to_string(e.after);
            out += '\t';
            out += (e.after >= e.before ? "+" : "-");
            out += std::to_string(e.mag);
            out += '\t';
            out += e.peerToken;
            if (!e.peerChar.empty()) {
                out += '/';
                out += e.peerChar;
            }
            out += '\t';
            out += e.note;
            out += '\n';
            ++n;
        }
        CopyText(out.c_str());
        SetStatus(st, "已复制流水 " + std::to_string(n) + " 行");
    }

    const float evH = (std::max)(90.f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("meso_ev_tbl", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, evH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 120.f);
        ImGui::TableSetupColumn("判定", ImGuiTableColumnFlags_WidthFixed, 96.f);
        ImGui::TableSetupColumn("身份", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("角色", ImGuiTableColumnFlags_WidthStretch, 0.95f);
        ImGui::TableSetupColumn("变化", ImGuiTableColumnFlags_WidthFixed, 108.f);
        ImGui::TableSetupColumn("对端", ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (auto it = st.mesoEvents.rbegin(); it != st.mesoEvents.rend(); ++it) {
            const auto& e = *it;
            const bool ok = st.mesoEventView == 1 ||
                            (st.mesoEventView == 2 &&
                             (e.kind == "inflow" || e.kind == "scroll_inflow")) ||
                            (st.mesoEventView == 0 && MesoKindAbnormal(e.kind));
            if (!ok) continue;
            if (st.mesoDashFilter[0] && !ContainsIgnoreCase(e.token, st.mesoDashFilter) &&
                !ContainsIgnoreCase(e.charName, st.mesoDashFilter) &&
                !ContainsIgnoreCase(e.peerToken, st.mesoDashFilter))
                continue;
            if (++shown > 400) break;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char tlab[24]{};
            FormatWallTick(e.wallMs, 1440, tlab, sizeof(tlab));
            ImGui::TextUnformatted(tlab);
            ImGui::TableSetColumnIndex(1);
            const ImVec4 kcol = MesoKindAlert(e.kind)
                                    ? OpsTone::Danger()
                                : (e.kind == "char_move" || e.kind == "scroll_move")
                                    ? OpsTone::Warn()
                                : (e.kind == "inflow" || e.kind == "scroll_inflow")
                                    ? OpsTone::Ok()
                                    : OpsTone::Muted();
            ImGui::TextColored(kcol, "%s", MesoKindLabel(e.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.token.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(e.charName.empty() ? "—" : e.charName.c_str());
            ImGui::TableSetColumnIndex(4);
            char dBuf[48]{};
            if (MesoKindScroll(e.kind)) {
                std::snprintf(dBuf, sizeof(dBuf), "×%s%llu",
                              e.after >= e.before ? "+" : "-", e.mag);
            } else {
                FormatMesoDelta(e.before, e.after, dBuf, sizeof(dBuf));
            }
            ImGui::TextColored(e.after >= e.before ? OpsTone::Ok() : OpsTone::Danger(), "%s", dBuf);
            if (ImGui::IsItemHovered()) {
                char bBuf[48]{}, aBuf[48]{};
                FormatMesoUll(e.before, bBuf, sizeof(bBuf));
                FormatMesoUll(e.after, aBuf, sizeof(aBuf));
                ImGui::SetTooltip("%s → %s", bBuf, aBuf);
            }
            ImGui::TableSetColumnIndex(5);
            if (e.peerToken.empty())
                ImGui::TextDisabled("—");
            else {
                ImGui::TextUnformatted(e.peerToken.c_str());
                if (!e.peerChar.empty()) {
                    ImGui::SameLine(0, 4.f);
                    ImGui::TextDisabled("%s", e.peerChar.c_str());
                }
            }
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(e.note.empty() ? "—" : e.note.c_str());
        }
        if (shown == 0) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("暂无流水。达到告警额度的涨跌会记在这里，重开 OPS 仍在。");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void DrawQuotaPanel(OpsState& st) {
    if (st.lastQuotaFetchMs == 0) RefreshQuota(st, true);

    if (ImGui::Button("刷新##quota")) RefreshQuota(st, true);
    ImGui::SameLine(0, 12.f);
    if (st.quotaEnabled)
        ImGui::TextColored(OpsTone::Ok(), "配额已启用");
    else
        ImGui::TextColored(OpsTone::Warn(), "配额未启用");
    {
        std::string sub = "默认上限 ";
        sub += st.quotaDefaultMax > 0 ? std::to_string(st.quotaDefaultMax) : std::string("不限");
        sub += " · 老化释放 ";
        sub += st.quotaAgingDays > 0 ? (std::to_string(st.quotaAgingDays) + " 天") : std::string("关");
        ImGui::SameLine(0, 12.f);
        ImGui::TextDisabled("%s", sub.c_str());
    }
    if (!st.quotaEnabled) {
        ImGui::TextColored(
            OpsTone::Warn(),
            "服务端缺公钥：跑 node scripts/xcat-gate-keygen.mjs 生成后重启 TWMS 更新服务即启用。");
    }
    if (!st.quotaError.empty()) {
        ImGui::TextColored(OpsTone::Danger(), "%s", st.quotaError.c_str());
    }

    // 过滤 + 直接给某 uid 设上限（uid 不存在会自动建档）
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputTextWithHint("##quota_filter", "筛选 uid…", st.quotaFilter, sizeof(st.quotaFilter));
    ImGui::SameLine(0, 16.f);
    ImGui::TextDisabled("设置上限：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.f);
    ImGui::InputTextWithHint("##quota_new_uid", "uid（如 张三）", st.quotaNewUidInput,
                             sizeof(st.quotaNewUidInput));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    // 空态 hint 直接写「不限」：让「清空 = 不限」这条规则在界面上自明，不用记 tooltip。
    ImGui::InputTextWithHint("##quota_new_max", "不限", st.quotaNewMaxInput,
                             sizeof(st.quotaNewMaxInput), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::Button("保存##quota_new")) {
        const std::string uid = st.quotaNewUidInput;
        if (!uid.empty()) {
            const int mv = std::atoi(st.quotaNewMaxInput);
            std::string err;
            if (PostQuotaSetMax(st, uid, mv, err)) {
                SetStatus(st, mv > 0 ? ("已设置 " + uid + " 上限=" + std::to_string(mv))
                                     : ("已设置 " + uid + " 为不限台数"));
                st.quotaNewUidInput[0] = '\0';
                // 台数恢复默认而非清空：清空的语义是「不限」，留在框里会让下一次操作误设成不限。
                std::snprintf(st.quotaNewMaxInput, sizeof(st.quotaNewMaxInput), "100");
            } else {
                SetStatus(st, "设置失败：" + err);
            }
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("留空或填 0 = 不限台数；uid 不存在会自动建档");

    // 清僵尸名额：deviceId 存在 ProgramData，重装 XCat 不换，但重装系统会换 → 旧名额白占。
    const int idleDays = std::atoi(st.quotaIdleDaysInput);
    ImGui::TextDisabled("清僵尸名额：闲置超过");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(50.f);
    ImGui::InputText("##quota_idle_days", st.quotaIdleDaysInput, sizeof(st.quotaIdleDaysInput),
                     ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::TextDisabled("天");
    ImGui::SameLine(0, 10.f);
    {
        int total = 0;
        if (idleDays > 0) {
            const long long cut = static_cast<long long>(idleDays) * 86400;
            for (const auto& u : st.quotaUsers)
                for (const auto& d : u.devices)
                    if (d.idleSec >= cut) ++total;
        }
        const bool can = idleDays > 0 && total > 0;
        if (!can) ImGui::BeginDisabled();
        const std::string label = "一键清理全部（" + std::to_string(total) + " 台）##quota_rel_all";
        if (ImGui::Button(label.c_str())) {
            st.quotaReleaseConfirmOpen = true;
            st.quotaReleaseConfirmUid.clear();
            st.quotaReleaseConfirmDays = idleDays;
            st.quotaReleaseConfirmCount = total;
        }
        if (!can) ImGui::EndDisabled();
        // AllowWhenDisabled：按钮灰着时最需要这段解释，默认的 IsItemHovered 恰好不上报。
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "释放这些设备占的名额。可逆：设备下次探活会重新登记（前提是那时没超上限）。\n"
                "重装系统会换 deviceId，旧名额不会自己消失（老化释放当前为%s）。\n"
                "%s",
                st.quotaAgingDays > 0 ? "已开" : "关",
                idleDays <= 0 ? "（当前不可用：天数需大于 0）"
                              : (total <= 0 ? "（当前不可用：没有设备闲置到这个天数）" : ""));
        }
    }

    ImGui::Separator();

    // 待执行动作：遍历中不直接改 vector（POST 会重建 st.quotaUsers）。
    bool doSet = false, doRemove = false;
    std::string pendSetUid, pendRmUid, pendRmDev;
    int pendSetMax = 0;

    const std::string filter = st.quotaFilter;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float availY = ImGui::GetContentRegionAvail().y - 4.f;
    if (ImGui::BeginTable("quota_table", 4, flags, ImVec2(0, availY > 120.f ? availY : 120.f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("用户 uid", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("已用 / 上限", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("上限", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();

        for (auto& u : st.quotaUsers) {
            if (!filter.empty() && !ContainsIgnoreCase(u.uid, filter.c_str())) continue;
            const bool over = u.effectiveMax > 0 && u.used >= u.effectiveMax;
            const bool expanded = st.quotaExpanded.count(u.uid) > 0;
            ImGui::TableNextRow();
            ImGui::PushID(u.uid.c_str());

            ImGui::TableSetColumnIndex(0);
            std::string caption = (expanded ? "v  " : ">  ") + u.uid;
            if (ImGui::Selectable(caption.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                if (expanded)
                    st.quotaExpanded.erase(u.uid);
                else
                    st.quotaExpanded.insert(u.uid);
            }

            ImGui::TableSetColumnIndex(1);
            std::string usage =
                std::to_string(u.used) + " / " +
                (u.effectiveMax > 0 ? std::to_string(u.effectiveMax) : std::string("不限"));
            if (over)
                ImGui::TextColored(OpsTone::Danger(), "%s", usage.c_str());
            else
                ImGui::TextUnformatted(usage.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##max", u.maxInput, sizeof(u.maxInput),
                             ImGuiInputTextFlags_CharsDecimal);

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("保存##row")) {
                doSet = true;
                pendSetUid = u.uid;
                pendSetMax = std::atoi(u.maxInput);
            }
            ImGui::SameLine();
            {
                // 本人的僵尸台数：本地按同一阈值算，不必再问服务端。
                int stale = 0;
                if (idleDays > 0) {
                    const long long cut = static_cast<long long>(idleDays) * 86400;
                    for (const auto& d : u.devices)
                        if (d.idleSec >= cut) ++stale;
                }
                if (stale > 0) {
                    const std::string label = "清僵尸(" + std::to_string(stale) + ")##row_rel";
                    if (ImGui::SmallButton(label.c_str())) {
                        st.quotaReleaseConfirmOpen = true;
                        st.quotaReleaseConfirmUid = u.uid;
                        st.quotaReleaseConfirmDays = idleDays;
                        st.quotaReleaseConfirmCount = stale;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("释放 %s 名下闲置超过 %d 天的 %d 台设备", u.uid.c_str(),
                                          idleDays, stale);
                } else {
                    ImGui::TextDisabled("(0=不限)");
                }
            }

            if (expanded) {
                for (const auto& d : u.devices) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const std::string shortId =
                        d.deviceId.size() > 18 ? d.deviceId.substr(0, 18) + "…" : d.deviceId;
                    ImGui::TextDisabled("      %s", shortId.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.deviceId.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(QuotaIdleColor(d.idleSec, idleDays), "%s",
                                       QuotaIdleText(d.idleSec).c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("最后一次探活：%s",
                                          d.lastSeen.empty() ? "—" : d.lastSeen.c_str());
                    }
                    ImGui::TableSetColumnIndex(3);
                    ImGui::PushID(d.deviceId.c_str());
                    if (NeutralSmallButton("释放")) {
                        doRemove = true;
                        pendRmUid = u.uid;
                        pendRmDev = d.deviceId;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("从该用户台账移除此设备（腾出一台名额）");
                    ImGui::PopID();
                }
                if (u.devices.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("      （暂无已登记设备）");
                }
            }
            ImGui::PopID();
        }
        if (st.quotaUsers.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled(st.quotaEnabled
                                    ? "暂无用户（客户端探活带签名 TOKEN 后自动出现）"
                                    : "配额未启用");
        }
        ImGui::EndTable();
    }

    if (doSet) {
        std::string err;
        if (PostQuotaSetMax(st, pendSetUid, pendSetMax, err))
            SetStatus(st, "已更新 " + pendSetUid + " 上限");
        else
            SetStatus(st, "设置失败：" + err);
    }
    if (doRemove) {
        std::string err;
        if (PostQuotaRemoveDevice(st, pendRmUid, pendRmDev, err))
            SetStatus(st, "已释放一台设备（" + pendRmUid + "）");
        else
            SetStatus(st, "释放失败：" + err);
    }

    // 批量释放确认：按钮在表格内（PushID 层级里），弹窗统一放在函数顶层开。
    if (st.quotaReleaseConfirmOpen) {
        ImGui::OpenPopup("confirm_quota_release_idle");
        st.quotaReleaseConfirmOpen = false;
    }
    if (ImGui::BeginPopupModal("confirm_quota_release_idle", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool all = st.quotaReleaseConfirmUid.empty();
        ImGui::TextUnformatted(all ? "确认清理全部用户的僵尸名额？" : "确认清理该用户的僵尸名额？");
        ImGui::Spacing();
        if (!all) ImGui::Text("用户：%s", st.quotaReleaseConfirmUid.c_str());
        ImGui::Text("释放闲置超过 %d 天的设备，共 %d 台。", st.quotaReleaseConfirmDays,
                    st.quotaReleaseConfirmCount);
        ImGui::Spacing();
        ImGui::TextDisabled("可逆：这些设备下次探活会重新登记并重新占名额。");
        ImGui::TextDisabled("若某台其实还在用，只是这段时间没开服，清了也不影响它下次上线。");
        ImGui::Spacing();
        if (SafeButton("确认释放##quota_rel_yes", ImVec2(130, 0))) {
            std::string err;
            if (PostQuotaReleaseIdle(st, st.quotaReleaseConfirmUid, st.quotaReleaseConfirmDays,
                                     err)) {
                SetStatus(st, "已释放 " + std::to_string(st.quotaReleaseConfirmCount) + " 台设备名额");
            } else {
                SetStatus(st, "释放失败：" + err);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##quota_rel_no", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DrawGateSignPanel(OpsState& st) {
    if (st.lastGateCardsFetchMs == 0) RefreshGateCards(st, true);
    // 台数回显要用 quotaUsers；只在首次进页拉一次，避免每帧阻塞式 HTTP。
    if (st.lastQuotaFetchMs == 0) RefreshQuota(st, true);

    ImGui::TextDisabled(
        "给成员签发启动 TOKEN：填 uid + 有效期 → 签发 → 复制发给他，启动 xcat.exe 时粘贴一次即激活。");
    ImGui::TextDisabled("私钥离线保管在本机 secrets\\，只由本机更新服务读取，不入库、不外发。");

    if (!TwmsRunning(st)) {
        ImGui::TextColored(OpsTone::Warn(),
                           "TWMS API 未运行：先到「服务与日志」启动更新服务，再来签卡。");
        return;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("uid（成员标识，如 张三）");
    ImGui::SetNextItemWidth(200.f);
    const bool uidEnter = ImGui::InputTextWithHint("##gate_sign_uid", "uid", st.gateSignUidInput,
                                                   sizeof(st.gateSignUidInput),
                                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 14.f);
    ImGui::TextUnformatted("有效期");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::InputTextWithHint("##gate_sign_days", "天数", st.gateSignDaysInput,
                             sizeof(st.gateSignDaysInput), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::TextDisabled("天（0=永久）");
    ImGui::SameLine(0, 14.f);
    ImGui::TextUnformatted("台数");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.f);
    ImGui::InputTextWithHint("##gate_sign_quota", "不改", st.gateSignQuotaInput,
                             sizeof(st.gateSignQuotaInput), ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "签发同时给该 uid 设台数上限，默认 100。\n"
            "留空 = 不动他现有上限（给老成员重签时清空，才不会把他已设好的上限抹掉）。\n"
            "填 0 = 明确设成不限台数。");

    // 填 uid 时先照一遍现状：他名下已有哪张有效卡、台数用了多少。
    // 少了这一步很容易给同一个人重复签发（台账堆重复行），或把上限设成他早已超掉的数。
    if (st.gateSignUidInput[0] != '\0') {
        const std::string wantUid = st.gateSignUidInput;
        const OpsState::GateCardRow* latest = nullptr;
        for (const auto& c : st.gateCards) {
            if (!EqualsIgnoreCase(c.uid, wantUid)) continue;
            latest = &c;  // 台账按签发倒序，首个命中即最新
            break;
        }
        if (latest) {
            const std::string remain = GateExpRemainText(latest->exp);
            if (latest->banned) {
                ImGui::TextColored(OpsTone::Danger(),
                                   "该 uid 已被吊销：重签新卡后仍会被联网拦截，需先在台账「解禁」。");
            } else if (remain == "已过期") {
                ImGui::TextColored(OpsTone::Warn(), "该 uid 上一张卡已过期（%s），本次签发相当于换新卡。",
                                   GateExpAbsText(latest->exp).c_str());
            } else {
                ImGui::TextColored(OpsTone::Warn(),
                                   "该 uid 已有有效卡（%s，%s）：再签发会多出一张，旧卡在到期前仍可用。",
                                   GateExpAbsText(latest->exp).c_str(), remain.c_str());
            }
        } else {
            ImGui::TextColored(OpsTone::Ok(), "新 uid：台账里还没有这个人的卡。");
        }
        const OpsState::QuotaUserRow* qu = nullptr;
        for (const auto& u : st.quotaUsers) {
            if (EqualsIgnoreCase(u.uid, wantUid)) {
                qu = &u;
                break;
            }
        }
        if (qu) {
            ImGui::SameLine(0, 10.f);
            ImGui::TextDisabled("｜台数已用 %d / %s", qu->used,
                                qu->effectiveMax > 0 ? std::to_string(qu->effectiveMax).c_str()
                                                     : "不限");
        }
    }

    ImGui::TextUnformatted("备注");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300.f);
    ImGui::InputTextWithHint("##gate_sign_note", "可选，落台账（如 部门/用途）", st.gateSignNoteInput,
                             sizeof(st.gateSignNoteInput));
    ImGui::SameLine(0, 14.f);
    const bool doSign = ImGui::Button("签发##gate_sign") || uidEnter;
    if (doSign) {
        const std::string uid = st.gateSignUidInput;
        const int days = std::atoi(st.gateSignDaysInput);
        std::string err;
        if (PostGateSign(st, uid, days, st.gateSignNoteInput, err)) {
            std::string msg = "已签发 " + st.gateSignInfo;
            if (st.gateSignQuotaInput[0] != '\0') {
                const int mv = std::atoi(st.gateSignQuotaInput);
                std::string qerr;
                if (PostQuotaSetMax(st, uid, mv, qerr))
                    msg += " · 台数上限=" + std::to_string(mv < 0 ? 0 : mv);
                else
                    msg += "（配额设置失败：" + qerr + "）";
            }
            SetStatus(st, msg);
            RefreshGateCards(st, true);
        } else {
            st.gateSignToken.clear();
            st.gateSignInfo.clear();
            st.gateSignError = err;
            SetStatus(st, "签发失败：" + err);
        }
    }

    if (!st.gateSignError.empty()) {
        ImGui::TextColored(OpsTone::Danger(), "%s", st.gateSignError.c_str());
    }

    if (!st.gateSignToken.empty()) {
        ImGui::TextColored(OpsTone::Ok(), "%s", st.gateSignInfo.c_str());
        ImGui::TextDisabled("TOKEN（整行发给成员，启动时粘贴）：");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputText("##gate_sign_token_out", st.gateSignToken.data(),
                         st.gateSignToken.size() + 1, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("复制 TOKEN##gate_sign")) {
            CopyText(st.gateSignToken.c_str());
            SetStatus(st, "TOKEN 已复制到剪贴板");
        }
        ImGui::SameLine();
        if (ImGui::Button("清空##gate_sign")) {
            st.gateSignToken.clear();
            st.gateSignInfo.clear();
            st.gateSignError.clear();
        }
    }

    // ── 签发台账 ──
    ImGui::Separator();
    ImGui::TextUnformatted("签发台账");
    ImGui::SameLine(0, 12.f);
    if (ImGui::Button("刷新##cards")) RefreshGateCards(st, true);
    ImGui::SameLine(0, 12.f);
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputTextWithHint("##cards_filter", "筛选 uid / 备注…", st.gateCardsFilter,
                             sizeof(st.gateCardsFilter));
    ImGui::SameLine(0, 12.f);
    {
        int histN = 0;
        for (const auto& c : st.gateCards)
            if (c.superseded) ++histN;
        char label[48]{};
        std::snprintf(label, sizeof(label), "显示历史卡 (%d)##cards_hist", histN);
        ImGui::Checkbox(label, &st.gateCardsShowHistory);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "默认每个 uid 只显示最新一张卡。\n"
                "续签/重签会给同一 uid 追加新卡，旧卡在到期前仍然可用；\n"
                "发卡时只该发最新那张，故把历史卡折起来，免得复制到旧卡发错人。");
    }
    if (!st.gateCardsError.empty()) {
        ImGui::SameLine(0, 12.f);
        ImGui::TextColored(OpsTone::Danger(), "%s", st.gateCardsError.c_str());
    }
    ImGui::TextDisabled(
        "废卡=只作废这一张（同 uid 的其他卡照用）；封人=封整个 uid（他所有卡一起拦）。"
        "两者都只在客户端联网探活时生效，离线激活拦不住。");

    const std::string filter = st.gateCardsFilter;
    std::string pendRevoke, pendUnrevoke;
    struct CardReq { std::string jti; std::string uid; };
    CardReq pendCardKill, pendCardRestore;
    struct RenewReq { std::string uid; int days; };
    RenewReq pendRenew{"", -1};

    // 台账是本页最后一个可见元素（EndTable 之后只有 POST 处理），吃满剩余高度别留半屏空白。
    const float cardsH = (std::max)(200.f, ImGui::GetContentRegionAvail().y - 4.f);
    if (ImGui::BeginTable("##gate_cards", 8,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0.f, cardsH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("uid", ImGuiTableColumnFlags_WidthFixed, 120.f);
        ImGui::TableSetupColumn("签发时间", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("有效期", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("备注", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("续期", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("这张卡", ImGuiTableColumnFlags_WidthFixed, 92.f);
        ImGui::TableSetupColumn("整个人", ImGuiTableColumnFlags_WidthFixed, 210.f);
        ImGui::TableHeadersRow();

        for (size_t idx = 0; idx < st.gateCards.size(); ++idx) {
            auto& c = st.gateCards[idx];
            if (c.superseded && !st.gateCardsShowHistory) continue;
            if (!filter.empty() && !ContainsIgnoreCase(c.uid, filter.c_str()) &&
                !ContainsIgnoreCase(c.note, filter.c_str()))
                continue;
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(idx));

            const std::string remain = GateExpRemainText(c.exp);
            const bool expired = (c.exp > 0 && remain == "已过期");

            ImGui::TableNextColumn();
            const std::string rowStatus =
                c.banned ? "人已封"
                         : (c.cardRevoked
                                ? "卡已废"
                                : (c.superseded ? "已被取代" : (expired ? "已过期" : "有效")));
            const std::string rowSummary = c.uid + "\t" + GateExpAbsText(c.exp) + "\t" + rowStatus +
                                           (c.note.empty() ? "" : ("\t" + c.note));
            if (ImGui::Selectable(c.uid.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    CopyText(c.uid.c_str());
                    SetStatus(st, "已复制 uid：" + c.uid);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("双击复制 uid；右键更多复制");
            if (ImGui::BeginPopupContextItem("##card_copy_ctx")) {
                if (ImGui::MenuItem("复制 uid")) {
                    CopyText(c.uid.c_str());
                    SetStatus(st, "已复制 uid：" + c.uid);
                }
                if (ImGui::MenuItem("复制整行摘要")) {
                    CopyText(rowSummary.c_str());
                    SetStatus(st, "已复制台账摘要");
                }
                if (!c.token.empty() &&
                    ImGui::MenuItem(c.superseded ? "复制 TOKEN（旧卡）" : "复制 TOKEN")) {
                    CopyText(c.token.c_str());
                    SetStatus(st, c.superseded
                                      ? ("已复制旧卡 TOKEN（uid=" + c.uid + "）：该 uid 已有更新的卡")
                                      : ("已复制 TOKEN（uid=" + c.uid + "）"));
                }
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", c.at.empty() ? "-" : c.at.substr(0, 16).c_str());

            ImGui::TableNextColumn();
            if (expired)
                ImGui::TextColored(OpsTone::Danger(), "%s", GateExpAbsText(c.exp).c_str());
            else
                ImGui::Text("%s", GateExpAbsText(c.exp).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", remain.c_str());

            ImGui::TableNextColumn();
            if (c.banned) {
                ImGui::TextColored(OpsTone::Danger(), "人已封");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("整个 uid 被封：他名下每张卡都拦，包括之后新签的。");
            } else if (c.cardRevoked) {
                ImGui::TextColored(OpsTone::Danger(), "卡已废");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("只这一张作废；该 uid 的其他卡照常能用。");
            } else if (c.superseded) {
                ImGui::TextDisabled("已被取代");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("该 uid 之后又签了新卡；这张在到期前仍能用，但不该再发给成员。");
            } else if (expired) {
                ImGui::TextColored(OpsTone::Warn(), "已过期");
            } else {
                ImGui::TextColored(OpsTone::Ok(), "有效");
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.note.empty() ? "-" : c.note.c_str());

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(56.f);
            ImGui::InputTextWithHint("##renew_days", "天", c.renewInput, sizeof(c.renewInput),
                                     ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            if (ImGui::Button("续签")) {
                pendRenew.uid = c.uid;
                pendRenew.days = std::atoi(c.renewInput);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "按天数给同一 uid 再签一张新卡。\n"
                    "旧卡在有效期内仍可用；不想让它继续用，续签后在旧卡那行点「废卡」。");

            // ── 这张卡：按卡号 jti 废/恢复，不牵连同 uid 的其他卡 ──
            ImGui::TableNextColumn();
            if (c.jti.empty()) {
                ImGui::TextDisabled("无卡号");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "这张卡签发于卡号机制上线前，没有可吊销的卡号。\n"
                        "要拦只能用右边「封人」封掉整个 uid，或让他换一张新卡后再废旧的。");
            } else if (c.cardRevoked) {
                if (ImGui::SmallButton("恢复卡")) {
                    pendCardRestore.jti = c.jti;
                    pendCardRestore.uid = c.uid;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("撤销这张卡的作废（卡号 %s）", c.jti.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, OpsTone::Danger());
                if (ImGui::SmallButton("废卡")) {
                    pendCardKill.jti = c.jti;
                    pendCardKill.uid = c.uid;
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "只作废这一张（卡号 %s）：\n"
                        "该 uid 的其他卡照常能用，适合「卡泄露了但人还要继续用」。\n"
                        "客户端联网探活时生效。",
                        c.jti.c_str());
            }

            // ── 整个人：按 uid 封禁，他名下所有卡（含之后新签的）一起拦 ──
            ImGui::TableNextColumn();
            if (c.banned) {
                if (ImGui::Button("解禁")) pendUnrevoke = c.uid;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("解开对 uid %s 的封禁", c.uid.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, OpsTone::Danger());
                if (ImGui::Button("封人")) pendRevoke = c.uid;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("封掉整个 uid %s：他名下每张卡都拦，之后新签的也拦。",
                                      c.uid.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("复制")) {
                CopyText(rowSummary.c_str());
                SetStatus(st, "已复制台账摘要（uid=" + c.uid + "）");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("复制该行摘要（uid + 有效期 + 状态 + 备注）");
            if (!c.token.empty()) {
                ImGui::SameLine();
                if (c.superseded) ImGui::BeginDisabled();
                if (ImGui::SmallButton("TOKEN")) {
                    CopyText(c.token.c_str());
                    SetStatus(st, "已复制 TOKEN（uid=" + c.uid + "），可直接发给成员");
                }
                if (c.superseded) ImGui::EndDisabled();
                // 禁用态默认不算 hovered，须显式放开，否则解释不了「为什么这个按钮点不动」。
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(c.superseded
                                          ? "该 uid 已有更新的卡，此处不给一键复制；\n"
                                            "真要发这张旧卡请用 uid 列右键菜单。"
                                          : "复制该卡完整 TOKEN，发给成员启动时粘贴激活");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!pendCardKill.jti.empty()) {
        std::string err;
        if (PostCardJtiAction(st, "revokejti", pendCardKill.jti, pendCardKill.uid, "ops 废卡", err)) {
            SetStatus(st, "已废掉这张卡（uid=" + pendCardKill.uid + " 卡号=" + pendCardKill.jti +
                              "）；该 uid 的其他卡不受影响");
            RefreshBans(st, true);
            RefreshGateCards(st, true);
        } else {
            SetStatus(st, "废卡失败：" + err);
        }
    }
    if (!pendCardRestore.jti.empty()) {
        std::string err;
        if (PostCardJtiAction(st, "unrevokejti", pendCardRestore.jti, {}, {}, err)) {
            SetStatus(st, "已恢复这张卡（卡号=" + pendCardRestore.jti + "）");
            RefreshBans(st, true);
            RefreshGateCards(st, true);
        } else {
            SetStatus(st, "恢复失败：" + err);
        }
    }
    if (!pendRevoke.empty()) {
        std::string err;
        if (PostRevokeUid(st, pendRevoke, err)) {
            SetStatus(st, "已封掉 " + pendRevoke + "（名下所有卡联网探活即拦）");
            RefreshGateCards(st, true);
        } else {
            SetStatus(st, "封人失败：" + err);
        }
    }
    if (!pendUnrevoke.empty()) {
        std::string err;
        if (PostUnrevokeUid(st, pendUnrevoke, err)) {
            SetStatus(st, "已解禁 " + pendUnrevoke);
            RefreshGateCards(st, true);
        } else {
            SetStatus(st, "解禁失败：" + err);
        }
    }
    if (!pendRenew.uid.empty()) {
        std::string err;
        if (PostGateSign(st, pendRenew.uid, pendRenew.days < 0 ? 0 : pendRenew.days, "续期", err)) {
            SetStatus(st, "已续签 " + st.gateSignInfo + "（记得把新 TOKEN 复制发给成员）");
            RefreshGateCards(st, true);
        } else {
            SetStatus(st, "续签失败：" + err);
        }
    }
}

void OpsPanel_Draw(OpsState& st) {
    if (st.mainTab == 0) RefreshLogViewer(st, false);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (st.mainTab == 2)
        hostFlags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("XCat TWMS Ops", nullptr, hostFlags);

    // ── Header（压成两行，把高度留给内容页） ──
    ImGui::TextUnformatted("XCat TWMS 运维控制台");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("台服经典版 · bin_ops\n关窗即停 API(:18789) 与发布站(:52080)");
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
            ImGui::SetTooltip("仅本运维台（bin_ops\\state\\user.ini）\n与启动器主题互不影响");
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
            if (st.lastClientHistoryFetchMs != 0) RefreshClientHistory(st, true);
            SetStatus(st, "已刷新连接列表");
        } else if (st.mainTab == 2) {
            RefreshClients(st, true);
            SetStatus(st, "已刷新利润监控采样");
        } else if (st.mainTab == 3) {
            RefreshQuota(st, true);
            SetStatus(st, "已刷新台数配额");
        } else if (st.mainTab == 4) {
            RefreshGateCards(st, true);
            SetStatus(st, "已刷新签发台账");
        } else {
            RefreshLogViewer(st, true);
            RefreshReleaseInfo(st);
            RefreshUpdateChannels(st, true);
            SetStatus(st, "已刷新日志与发布信息");
        }
    }
    ImGui::Separator();

    if (st.mainTab == 1) {
        DrawClientsPanel(st);
        ImGui::End();
        return;
    }
    if (st.mainTab == 2) {
        DrawMesoDashPanel(st);
        ImGui::End();
        return;
    }
    if (st.mainTab == 3) {
        DrawQuotaPanel(st);
        ImGui::End();
        return;
    }
    if (st.mainTab == 4) {
        DrawGateSignPanel(st);
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
    const float svcCardH = ImGui::GetTextLineHeightWithSpacing() * 10.4f;

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
                ImGui::TextDisabled("构建 v%s #%u", st.latestClientVersionText.c_str(),
                                    st.latestClientBuildId);
            } else {
                ImGui::TextDisabled("最新构建：未找到 latest.json");
            }
            if (st.allowedClientBuildId > 0) {
                ImGui::SameLine(0, 10.f);
                if (st.latestClientBuildId != 0 &&
                    st.allowedClientBuildId != st.latestClientBuildId)
                    ImGui::TextColored(OpsTone::Warn(), "允许 v%s #%u",
                                       st.allowedClientVersionText.c_str(), st.allowedClientBuildId);
                else
                    ImGui::TextDisabled("允许 v%s #%u", st.allowedClientVersionText.c_str(),
                                        st.allowedClientBuildId);
            } else if (!st.updateChannelsError.empty()) {
                ImGui::SameLine(0, 10.f);
                ImGui::TextColored(OpsTone::Warn(), "%s", st.updateChannelsError.c_str());
            } else if (TwmsRunning(st)) {
                ImGui::SameLine(0, 10.f);
                ImGui::TextColored(OpsTone::Warn(), "允许=构建（未配置通道）");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    "检查更新走「允许」版本，不是刚打的 latest.json。\n"
                    "出新包不会自动放开；分组可在连接表右键单独指定。");
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

            if (!st.updatePackages.empty()) {
                ImGui::SetNextItemWidth(210.f);
                const char* preview = "选择对外允许版本";
                char previewBuf[80]{};
                if (st.updateChannelCombo >= 0 &&
                    st.updateChannelCombo < (int)st.updatePackages.size()) {
                    const auto& p = st.updatePackages[(size_t)st.updateChannelCombo];
                    std::snprintf(previewBuf, sizeof(previewBuf), "v%s #%u", p.version.c_str(),
                                  p.buildId);
                    preview = previewBuf;
                }
                if (ImGui::BeginCombo("##allow_build", preview)) {
                    for (int i = 0; i < (int)st.updatePackages.size(); ++i) {
                        const auto& p = st.updatePackages[(size_t)i];
                        char lab[80]{};
                        std::snprintf(lab, sizeof(lab), "v%s #%u", p.version.c_str(), p.buildId);
                        const bool sel = (i == st.updateChannelCombo);
                        if (ImGui::Selectable(lab, sel)) st.updateChannelCombo = i;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                const bool canSet =
                    st.updateChannelCombo >= 0 &&
                    st.updateChannelCombo < (int)st.updatePackages.size();
                if (!canSet) ImGui::BeginDisabled();
                if (ImGui::SmallButton("设为对外允许##p")) {
                    st.updateChannelPendingBuildId =
                        st.updatePackages[(size_t)st.updateChannelCombo].buildId;
                    ImGui::OpenPopup("confirm_allow_channel");
                }
                if (!canSet) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("全员检查更新升到此版；不影响磁盘上的最新构建。");
                if (ImGui::BeginPopupModal("confirm_allow_channel", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    uint32_t bid = st.updateChannelPendingBuildId;
                    std::string ver;
                    for (const auto& p : st.updatePackages) {
                        if (p.buildId == bid) {
                            ver = p.version;
                            break;
                        }
                    }
                    ImGui::TextUnformatted("设为对外允许版本？");
                    ImGui::TextDisabled("目标：v%s  build %u", ver.c_str(), bid);
                    ImGui::TextWrapped(
                        "之后客户端检查更新默认升到这一版，刚打的包不会自动放开。"
                        "个别分组可在「连接与访问」右键单独指定。");
                    ImGui::Spacing();
                    if (SafeButton("确认##allow_yes", ImVec2(120, 0))) {
                        std::string err;
                        if (PostUpdateChannelDefault(st, bid, err))
                            SetStatus(st, "已设置对外允许 v" + ver + " #" + std::to_string(bid));
                        else
                            SetStatus(st, err);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("取消##allow_no", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }

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
            if (ImGui::SmallButton("全体推送##p")) ImGui::OpenPopup("confirm_force_update");
            if (st.latestClientBuildId == 0) ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("写入 force-update.json，所有在线客户端都会更新。\n"
                                  "只想更新一台：用连接表「推更」。");
            if (ImGui::BeginPopupModal("confirm_force_update", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextColored(OpsTone::Warn(), "确认全体强制更新？");
                ImGui::TextDisabled("目标：v%s  build %u", st.latestClientVersionText.c_str(),
                                    st.latestClientBuildId);
                ImGui::TextWrapped(
                    "会写入 force-update.json，所有轮询到的客户端都会拉新包。"
                    "若只要更新个别机器，请关闭此框，改用「连接与访问」表里的「推更」。");
                ImGui::Spacing();
                if (SafeButton("确认全体推送##force_yes", ImVec2(140, 0))) {
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