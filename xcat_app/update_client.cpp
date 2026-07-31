#include "update_client.h"

#include "app_notify.h"
#include "log_upload.h"

#include "../common/process_util.h"
#include "../common/xcat_config_ini.h"
#include "../common/xcat_log.h"
#include "../common/xcat_version.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace xcat::app {
namespace {

void NotifyUpdateFail(const char* title, const char* body) {
    if (!body || !body[0]) return;
    xcat::log::Warn("Update", "%s: %s", title ? title : "update-fail", body);
    notify::PushLocal(/*Danger*/ 3, "update-fail", title && title[0] ? title : "更新失败", body,
                      7000);
}

struct ParsedUrl {
    std::wstring host;
    std::string hostText;
    std::string scheme;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    std::wstring path;
    std::string origin;
};

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::string err;
};

struct Manifest {
    std::string version;
    uint32_t buildId = 0;
    std::string zipName;
    std::string downloadUrl;
    std::string sha256;
    uint64_t size = 0;
};

struct State {
    std::mutex mtx;
    UpdateSnapshot snapshot{};
    Manifest manifest{};
    std::string manifestUrl;
    std::string downloadUrl;
    bool autoInstallAfterDownload = false;
    bool forcePollInFlight = false;
    ULONGLONG lastForcePollMs = 0;
    // 检查瞬间完成时也要让进度区多停几秒，避免用户点完按钮「什么都没发生」。
    ULONGLONG stickyProgressUiUntilMs = 0;
};

State g_state;
std::atomic<bool> g_requestExitForUpdate{false};

std::string Win32ErrorText(DWORD err) {
    char* raw = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<char*>(&raw), 0, nullptr);
    std::string text = n && raw ? raw : "";
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::string WinHttpFailure(const char* action, DWORD err, const char* mode) {
    char buf[384]{};
    const std::string text = Win32ErrorText(err);
    snprintf(buf, sizeof(buf), "%s failed mode=%s err=%lu%s%s", action ? action : "WinHTTP",
             mode ? mode : "unknown", static_cast<unsigned long>(err), text.empty() ? "" : " ",
             text.c_str());
    return buf;
}

bool ParseUrl(const std::string& url, ParsedUrl& out) {
    out = {};
    std::string u = url;
    std::string scheme;
    if (u.rfind("https://", 0) == 0) {
        scheme = "https://";
        out.scheme = scheme;
        out.secure = true;
        out.port = INTERNET_DEFAULT_HTTPS_PORT;
        u = u.substr(8);
    } else if (u.rfind("http://", 0) == 0) {
        scheme = "http://";
        out.scheme = scheme;
        out.secure = false;
        out.port = INTERNET_DEFAULT_HTTP_PORT;
        u = u.substr(7);
    } else {
        return false;
    }
    const size_t slash = u.find('/');
    const std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    out.path = slash == std::string::npos ? L"/" : xcat::Utf8ToWide(u.substr(slash));
    const size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        out.hostText = hostPort.substr(0, colon);
        out.host = xcat::Utf8ToWide(out.hostText);
        try {
            out.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
        } catch (...) {
            return false;
        }
    } else {
        out.hostText = hostPort;
        out.host = xcat::Utf8ToWide(out.hostText);
    }
    out.origin = scheme + hostPort;
    return !out.host.empty() && !out.path.empty();
}

std::string JsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    size_t i = pos + needle.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != '"') return {};
    ++i;
    std::string out;
    while (i < json.size()) {
        const char c = json[i++];
        if (c == '"') break;
        if (c == '\\' && i < json.size()) {
            const char esc = json[i++];
            if (esc == 'n') out += '\n';
            else if (esc == 'r') out += '\r';
            else if (esc == 't') out += '\t';
            else out += esc;
        } else {
            out += c;
        }
    }
    return out;
}

uint64_t JsonUint(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    size_t i = pos + needle.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    uint64_t v = 0;
    while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) {
        v = v * 10 + static_cast<unsigned>(json[i++] - '0');
    }
    return v;
}

Manifest ParseManifest(const std::string& body) {
    Manifest m{};
    m.version = JsonString(body, "version");
    m.buildId = static_cast<uint32_t>(JsonUint(body, "buildId"));
    m.zipName = JsonString(body, "zipName");
    m.downloadUrl = JsonString(body, "downloadUrl");
    m.sha256 = JsonString(body, "sha256");
    m.size = JsonUint(body, "size");
    std::transform(m.sha256.begin(), m.sha256.end(), m.sha256.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return m;
}

bool IsIpv4Literal(const std::string& host) {
    int dots = 0;
    int digits = 0;
    for (char c : host) {
        if (c == '.') {
            if (digits == 0) return false;
            ++dots;
            digits = 0;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        ++digits;
        if (digits > 3) return false;
    }
    return dots == 3 && digits > 0;
}

bool ShouldDnsFallback(const ParsedUrl& url) {
    return !url.hostText.empty() && !IsIpv4Literal(url.hostText) &&
           url.hostText.find(':') == std::string::npos;
}

std::string UrlWithHost(const ParsedUrl& url, const std::string& host) {
    std::string out = url.scheme + host;
    const bool defaultPort =
        (!url.secure && url.port == INTERNET_DEFAULT_HTTP_PORT) ||
        (url.secure && url.port == INTERNET_DEFAULT_HTTPS_PORT);
    if (!defaultPort) out += ":" + std::to_string(url.port);
    out += xcat::WideToUtf8(url.path);
    return out;
}

HttpResult HttpGetTextOnce(const ParsedUrl& url, DWORD accessType, const char* mode,
                           const wchar_t* extraHeaders) {
    HttpResult result;
    HINTERNET ses = WinHttpOpen(L"XCat-Update/1.0", accessType,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        result.err = WinHttpFailure("WinHttpOpen", GetLastError(), mode);
        return result;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 20000);
    HINTERNET conn = WinHttpConnect(ses, url.host.c_str(), url.port, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", url.path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              url.secure ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (!conn) {
        result.err = WinHttpFailure("WinHttpConnect", GetLastError(), mode);
    } else if (!req) {
        result.err = WinHttpFailure("WinHttpOpenRequest", GetLastError(), mode);
    } else {
        if (extraHeaders && extraHeaders[0]) {
            WinHttpAddRequestHeaders(req, extraHeaders, static_cast<DWORD>(-1),
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                0)) {
            result.err = WinHttpFailure("WinHttpSendRequest", GetLastError(), mode);
        } else if (!WinHttpReceiveResponse(req, nullptr)) {
            result.err = WinHttpFailure("WinHttpReceiveResponse", GetLastError(), mode);
        } else {
            DWORD status = 0, statusSize = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);
            result.status = status;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string buf(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, buf.data(), avail, &read)) {
                    result.err = WinHttpFailure("WinHttpReadData", GetLastError(), mode);
                    break;
                }
                if (read == 0) break;
                buf.resize(read);
                result.body += buf;
                if (result.body.size() > 1024 * 1024) break;
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(ses);
    return result;
}

HttpResult HttpGetTextNoDnsFallback(const std::string& urlText, const wchar_t* extraHeaders) {
    ParsedUrl url{};
    if (!ParseUrl(urlText, url)) {
        return HttpResult{0, {}, "更新地址格式错误"};
    }
    HttpResult direct = HttpGetTextOnce(url, WINHTTP_ACCESS_TYPE_NO_PROXY, "direct", extraHeaders);
    if (direct.err.empty()) return direct;

    xcat::log::Warn("Update", "manifest request failed url=%s err=%s; retry default-proxy",
                    urlText.c_str(), direct.err.c_str());
    HttpResult result =
        HttpGetTextOnce(url, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, "default-proxy", extraHeaders);
    if (result.err.empty()) return result;

    xcat::log::Warn("Update", "manifest request failed url=%s err=%s; retry auto-proxy",
                    urlText.c_str(), result.err.c_str());
    HttpResult autoProxy =
        HttpGetTextOnce(url, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, "auto-proxy", extraHeaders);
    if (autoProxy.err.empty()) return autoProxy;

    autoProxy.err = "更新检查请求失败: 直连失败: " + direct.err + "; 默认 WinHTTP 失败: " +
                    result.err + "; 自动代理失败: " + autoProxy.err;
    return autoProxy;
}

std::string ResolveHostViaAliDns(const std::string& host) {
    if (host.empty()) return {};
    const std::string dohUrl = "https://dns.alidns.com/resolve?name=" + host + "&type=A";
    const HttpResult resp = HttpGetTextNoDnsFallback(dohUrl, nullptr);
    if (!resp.err.empty() || resp.status != 200) {
        xcat::log::Warn("Update", "alidns doh failed host=%s status=%lu err=%s", host.c_str(),
                        static_cast<unsigned long>(resp.status), resp.err.c_str());
        return {};
    }
    size_t pos = 0;
    while ((pos = resp.body.find("\"data\":", pos)) != std::string::npos) {
        pos += 7;
        while (pos < resp.body.size() &&
               std::isspace(static_cast<unsigned char>(resp.body[pos]))) {
            ++pos;
        }
        if (pos >= resp.body.size() || resp.body[pos] != '"') continue;
        ++pos;
        const size_t end = resp.body.find('"', pos);
        if (end == std::string::npos) break;
        const std::string ip = resp.body.substr(pos, end - pos);
        if (IsIpv4Literal(ip)) return ip;
        pos = end + 1;
    }
    return {};
}

HttpResult HttpGetText(const std::string& urlText, const wchar_t* extraHeaders = nullptr) {
    ParsedUrl url{};
    if (!ParseUrl(urlText, url)) {
        return HttpResult{0, {}, "更新地址格式错误"};
    }
    HttpResult result = HttpGetTextNoDnsFallback(urlText, extraHeaders);
    if (result.err.empty()) return result;
    if (!ShouldDnsFallback(url)) return result;

    const std::string ip = ResolveHostViaAliDns(url.hostText);
    if (ip.empty()) return result;
    const std::string ipUrl = UrlWithHost(url, ip);
    xcat::log::Warn("Update", "retry with AliDNS A record host=%s ip=%s url=%s",
                    url.hostText.c_str(), ip.c_str(), ipUrl.c_str());
    HttpResult byIp = HttpGetTextNoDnsFallback(ipUrl, extraHeaders);
    if (byIp.err.empty()) return byIp;
    byIp.err = result.err + "; AliDNS IP 重试失败: " + byIp.err;
    return byIp;
}

constexpr ULONGLONG kStickyProgressUiMs = 5000;

// 调用方必须已持 g_state.mtx。进度条粘性 / 默认 progress 的唯一真源。
void ApplyStickyProgressUiLocked(UpdatePhase phase) {
    if (phase == UpdatePhase::UpToDate || phase == UpdatePhase::Failed) {
        g_state.stickyProgressUiUntilMs = GetTickCount64() + kStickyProgressUiMs;
    } else {
        g_state.stickyProgressUiUntilMs = 0;
    }
}

// 调用方必须已持 g_state.mtx。切换 phase/message，并统一 progress + sticky。
// Downloading/Installing 保留字节进度（由下载/安装路径自行更新）。
void SetPhaseUiLocked(UpdatePhase phase, std::string message) {
    g_state.snapshot.phase = phase;
    g_state.snapshot.message = std::move(message);
    if (phase == UpdatePhase::Downloading || phase == UpdatePhase::Installing) {
        // 保留 downloadedBytes/totalBytes；progress 由调用方或后续进度回调写。
    } else if (phase == UpdatePhase::UpToDate) {
        g_state.snapshot.progress = 1.f;
        g_state.snapshot.downloadedBytes = 0;
        g_state.snapshot.totalBytes = 0;
    } else {
        g_state.snapshot.progress = -1.f;
        g_state.snapshot.downloadedBytes = 0;
        g_state.snapshot.totalBytes = 0;
    }
    ApplyStickyProgressUiLocked(phase);
}

void SetSnapshot(UpdatePhase phase, std::string message) {
    std::string failBody;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        SetPhaseUiLocked(phase, std::move(message));
        if (phase == UpdatePhase::Failed) {
            g_state.autoInstallAfterDownload = false;
            failBody = g_state.snapshot.message;
        }
    }
    // 进程内失败：启动器还在，直接气泡通知（PS 换包失败走 update_failed.notify）。
    if (!failBody.empty()) {
        NotifyUpdateFail("更新失败", failBody.c_str());
    }
}

void SetInstallStatus(std::string message, float progress = -1.f) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    SetPhaseUiLocked(UpdatePhase::Installing, std::move(message));
    g_state.snapshot.progress = progress;
}

void SetDownloadProgress(uint64_t got, uint64_t total) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    if (g_state.snapshot.phase != UpdatePhase::Downloading) return;
    g_state.snapshot.downloadedBytes = got;
    g_state.snapshot.totalBytes = total;
    if (total > 0) {
        float p = static_cast<float>(got) / static_cast<float>(total);
        if (p < 0.f) p = 0.f;
        if (p > 1.f) p = 1.f;
        g_state.snapshot.progress = p;
        char msg[128]{};
        const double gotMb = static_cast<double>(got) / (1024.0 * 1024.0);
        const double totalMb = static_cast<double>(total) / (1024.0 * 1024.0);
        snprintf(msg, sizeof(msg), "下载更新中… %.0f%% (%.1f / %.1f MB)",
                 static_cast<double>(p * 100.f), gotMb, totalMb);
        g_state.snapshot.message = msg;
    } else {
        g_state.snapshot.progress = -1.f;
        char msg[96]{};
        const double gotMb = static_cast<double>(got) / (1024.0 * 1024.0);
        snprintf(msg, sizeof(msg), "下载更新中… 已接收 %.1f MB", gotMb);
        g_state.snapshot.message = msg;
    }
}

std::wstring TempUpdateDir() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path dir = std::filesystem::path(temp) / L"xcat_update";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::string SanitizeFileName(std::string s) {
    for (char& c : s) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            c = '_';
        }
    }
    return s.empty() ? "xcat_update.zip" : s;
}

bool DownloadFileOnce(const ParsedUrl& url, const std::wstring& path, DWORD accessType,
                      const char* mode, uint64_t expectedSize, std::string& err) {
    HINTERNET ses = WinHttpOpen(L"XCat-Update/1.0", accessType,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        err = WinHttpFailure("WinHttpOpen", GetLastError(), mode);
        return false;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 30000);
    HINTERNET conn = WinHttpConnect(ses, url.host.c_str(), url.port, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", url.path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              url.secure ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    bool ok = false;
    if (!conn) {
        err = WinHttpFailure("WinHttpConnect", GetLastError(), mode);
    } else if (!req) {
        err = WinHttpFailure("WinHttpOpenRequest", GetLastError(), mode);
    } else if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                   0, 0)) {
        err = WinHttpFailure("WinHttpSendRequest", GetLastError(), mode);
    } else if (!WinHttpReceiveResponse(req, nullptr)) {
        err = WinHttpFailure("WinHttpReceiveResponse", GetLastError(), mode);
    } else {
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            char msg[80]{};
            snprintf(msg, sizeof(msg), "下载失败 HTTP %lu", static_cast<unsigned long>(status));
            err = msg;
        } else {
            uint64_t total = expectedSize;
            DWORD contentLen = 0;
            DWORD contentLenSize = sizeof(contentLen);
            if (WinHttpQueryHeaders(req,
                                    WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &contentLenSize,
                                    WINHTTP_NO_HEADER_INDEX) &&
                contentLen > 0) {
                total = contentLen;
            }
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                err = "创建下载文件失败";
            } else {
                ok = true;
                DWORD avail = 0;
                uint64_t got = 0;
                ULONGLONG lastUiMs = 0;
                SetDownloadProgress(0, total);
                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                    std::vector<char> buf(avail);
                    DWORD read = 0, written = 0;
                    if (!WinHttpReadData(req, buf.data(), avail, &read)) {
                        err = WinHttpFailure("WinHttpReadData", GetLastError(), mode);
                        ok = false;
                        break;
                    }
                    if (read == 0) break;
                    if (!WriteFile(file, buf.data(), read, &written, nullptr) || written != read) {
                        err = "写入下载文件失败";
                        ok = false;
                        break;
                    }
                    got += read;
                    const ULONGLONG now = GetTickCount64();
                    if (lastUiMs == 0 || now - lastUiMs >= 100 || (total > 0 && got >= total)) {
                        lastUiMs = now;
                        SetDownloadProgress(got, total);
                    }
                }
                if (ok) SetDownloadProgress(got, total > 0 ? total : got);
                CloseHandle(file);
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(ses);
    return ok;
}

bool DownloadFileNoDnsFallback(const std::string& urlText, const std::wstring& path,
                               uint64_t expectedSize, std::string& err) {
    ParsedUrl url{};
    if (!ParseUrl(urlText, url)) {
        err = "下载地址格式错误";
        return false;
    }
    if (DownloadFileOnce(url, path, WINHTTP_ACCESS_TYPE_NO_PROXY, "direct", expectedSize, err)) {
        return true;
    }
    const std::string directErr = err;
    xcat::log::Warn("Update", "download failed url=%s err=%s; retry default-proxy", urlText.c_str(),
                    directErr.c_str());
    if (DownloadFileOnce(url, path, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, "default-proxy", expectedSize,
                         err)) {
        return true;
    }
    const std::string firstErr = err;
    xcat::log::Warn("Update", "download failed url=%s err=%s; retry auto-proxy", urlText.c_str(),
                    firstErr.c_str());
    if (DownloadFileOnce(url, path, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, "auto-proxy", expectedSize,
                         err)) {
        return true;
    }
    err = "下载请求失败: 直连失败: " + directErr + "; 默认 WinHTTP 失败: " + firstErr +
          "; 自动代理失败: " + err;
    return false;
}

bool DownloadFile(const std::string& urlText, const std::wstring& path, uint64_t expectedSize,
                  std::string& err) {
    ParsedUrl url{};
    if (!ParseUrl(urlText, url)) {
        err = "下载地址格式错误";
        return false;
    }
    if (DownloadFileNoDnsFallback(urlText, path, expectedSize, err)) return true;
    if (!ShouldDnsFallback(url)) return false;

    const std::string firstErr = err;
    const std::string ip = ResolveHostViaAliDns(url.hostText);
    if (ip.empty()) return false;
    const std::string ipUrl = UrlWithHost(url, ip);
    xcat::log::Warn("Update", "retry download with AliDNS A record host=%s ip=%s url=%s",
                    url.hostText.c_str(), ip.c_str(), ipUrl.c_str());
    if (DownloadFileNoDnsFallback(ipUrl, path, expectedSize, err)) return true;
    err = firstErr + "; AliDNS IP 下载重试失败: " + err;
    return false;
}

std::string HexLower(const unsigned char* data, size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

bool Sha256File(const std::wstring& path, std::string& out, std::string& err) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = "读取下载文件失败";
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = 0;
    std::vector<unsigned char> obj;
    unsigned char digest[32]{};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                          sizeof(objLen), &cb, 0) == 0) {
        obj.resize(objLen);
        if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) == 0) {
            std::vector<unsigned char> buf(64 * 1024);
            DWORD read = 0;
            ok = true;
            while (ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) && read) {
                if (BCryptHashData(hash, buf.data(), read, 0) != 0) {
                    ok = false;
                    break;
                }
            }
            if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                out = HexLower(digest, sizeof(digest));
            } else {
                ok = false;
            }
        }
    }
    if (!ok) err = "SHA-256 校验失败";
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = suffix ? strlen(suffix) : 0;
    return n <= s.size() && s.compare(s.size() - n, n, suffix) == 0;
}

std::string ManifestUrlDirectory(const std::string& manifestUrl) {
    const size_t slash = manifestUrl.rfind('/');
    return slash == std::string::npos ? manifestUrl : manifestUrl.substr(0, slash + 1);
}

std::string UpdatePathFromServicePath(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty() || path == "/") return "/update/latest.json";
    if (EndsWith(path, "/v1/logs")) path.resize(path.size() - 8);
    if (path.empty() || path == "/") return "/update/latest.json";
    if (EndsWith(path, "/update/latest.json")) return path;
    if (EndsWith(path, "/update")) return path + "/latest.json";
    return path + "/update/latest.json";
}

std::string UpdateForcePathFromServicePath(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty() || path == "/") return "/update/force.json";
    if (EndsWith(path, "/v1/logs")) path.resize(path.size() - 8);
    if (path.empty() || path == "/") return "/update/force.json";
    if (EndsWith(path, "/update/latest.json")) path.resize(path.size() - 19);
    else if (EndsWith(path, "/update")) return path + "/force.json";
    return path + "/update/force.json";
}

bool WriteAutoReceiveUpdates(const std::string& payloadBinDir, bool enabled) {
    if (payloadBinDir.empty()) return false;
    const std::string path = xcat::UserConfigIniPath(payloadBinDir.c_str());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(xcat::Utf8ToWide(path)).parent_path(), ec);
    if (ec) return false;

    return xcat::UpdateIniFile(path.c_str(), [&](xcat::IniStore& ini) {
        xcat::IniSetU32(ini, "meta", "version", static_cast<uint32_t>(xcat::kUserConfigIniVersion));
        xcat::IniSetU32(ini, "update", "version", 1u);
        xcat::IniSetBool(ini, "update", "autoReceive", enabled);
        xcat::IniSetU64(ini, "update", "writeTickMs", GetTickCount64());
    });
}

std::string PsQuote(const std::wstring& text) {
    std::string s = xcat::WideToUtf8(text);
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

bool WriteUtf8FileBom(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    const bool ok = WriteFile(file, bom, sizeof(bom), &written, nullptr) &&
                    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok;
}

bool LaunchUpdaterScript(const std::wstring& zipPath, const std::wstring& installDir,
                         std::string& err) {
    const std::wstring scriptPath = std::filesystem::path(TempUpdateDir()) / L"apply_update.ps1";
    const DWORD pid = GetCurrentProcessId();
    std::string ps;
    ps += "$ErrorActionPreference='Stop'\r\n";
    // 绝不能 cwd=安装目录：本脚本会 Rename 整包，cwd 钉在 dest 会令目录永远「正在使用中」。
    ps += "try { Set-Location -LiteralPath $env:TEMP } catch {}\r\n";
    ps += "$log=Join-Path $env:TEMP 'xcat_update_apply.log'\r\n";
    ps += "function Write-XCatLog($m) { Add-Content -LiteralPath $log -Encoding UTF8 -Value ((Get-Date -Format o) + ' ' + $m) }\r\n";
    ps += "Write-XCatLog ('updater cwd=' + (Get-Location).Path)\r\n";
    // throwOnFail=$true：换包前门禁；=$false：失败 relaunch 时尽力清栈，不阻断拉起启动器。
    ps += "function Wait-XCatProcessGone($name, $graceSec, $forceSec, $throwOnFail=$true) {\r\n";
    ps += "  $deadline=(Get-Date).AddSeconds($graceSec)\r\n";
    ps += "  while (@(Get-Process -Name $name -ErrorAction SilentlyContinue).Count -gt 0) {\r\n";
    ps += "    if ((Get-Date) -gt $deadline) { Write-XCatLog ($name + '.exe still running; force stop'); Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; break }\r\n";
    ps += "    Start-Sleep -Milliseconds 300\r\n";
    ps += "  }\r\n";
    ps += "  $deadline=(Get-Date).AddSeconds($forceSec)\r\n";
    ps += "  while (@(Get-Process -Name $name -ErrorAction SilentlyContinue).Count -gt 0) {\r\n";
    ps += "    if ((Get-Date) -gt $deadline) {\r\n";
    ps += "      if ($throwOnFail) { throw ($name + '.exe still running after force stop') }\r\n";
    ps += "      Write-XCatLog ($name + '.exe still running after force stop; continue')\r\n";
    ps += "      return $false\r\n";
    ps += "    }\r\n";
    ps += "    Start-Sleep -Milliseconds 200\r\n";
    ps += "  }\r\n";
    ps += "  return $true\r\n";
    ps += "}\r\n";
    // TWMS：无 Nexon Worlds 栈；保留空函数兼容脚本内旧调用点。
    ps += "function Stop-XCatNexonStack($graceSec, $forceSec, $throwOnFail=$true) {\r\n";
    ps += "  Write-XCatLog 'Stop-XCatNexonStack noop (TWMS)'\r\n";
    ps += "}\r\n";
    // 清目录：只去掉 ReadOnly（勿把目录 Attributes 设成 Normal），再重试删除。
    ps += "function Clear-XCatAttrs($path) {\r\n";
    ps += "  if (-not (Test-Path -LiteralPath $path)) { return }\r\n";
    ps += "  $mask=[int][IO.FileAttributes]::ReadOnly\r\n";
    ps += "  $clear={ param($p) try { $it=Get-Item -LiteralPath $p -Force -ErrorAction Stop; $it.Attributes = ([int]$it.Attributes -band (-bnot $mask)) } catch {} }\r\n";
    ps += "  & $clear $path\r\n";
    ps += "  try { Get-ChildItem -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { & $clear $_.FullName } } catch {}\r\n";
    ps += "}\r\n";
    ps += "function Remove-XCatPathRetry($path, $attempts) {\r\n";
    ps += "  if (-not (Test-Path -LiteralPath $path)) { return $true }\r\n";
    ps += "  for ($i=1; $i -le $attempts; $i++) {\r\n";
    ps += "    Clear-XCatAttrs $path\r\n";
    ps += "    try { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop; return $true } catch { Write-XCatLog ('remove retry ' + $i + '/' + $attempts + ': ' + $path + ' :: ' + $_.Exception.Message) }\r\n";
    ps += "    Start-Sleep -Milliseconds (200 * $i)\r\n";
    ps += "  }\r\n";
    ps += "  return $false\r\n";
    ps += "}\r\n";
    // 换包失败通知：短摘要给人看；全文进日志。落盘 state + TEMP，并 Popup 兜底（不依赖新启动器）。
    ps += "function Format-XCatUpdateFailSummary($reason) {\r\n";
    ps += "  $raw=($reason | Out-String)\r\n";
    ps += "  foreach ($line in @($raw -split \"`n\")) {\r\n";
    ps += "    $l=$line.Trim()\r\n";
    ps += "    if (-not $l) { continue }\r\n";
    ps += "    if ($l -match '^(At |\\+|---)') { continue }\r\n";
    ps += "    if ($l.Length -gt 240) { $l=$l.Substring(0,237) + '...' }\r\n";
    ps += "    return $l\r\n";
    ps += "  }\r\n";
    ps += "  return '自动更新安装失败，请查看 %TEMP%\\xcat_update_apply.log'\r\n";
    ps += "}\r\n";
    ps += "function Write-XCatUpdateFailedNotify($dest, $reason) {\r\n";
    ps += "  $summary=Format-XCatUpdateFailSummary $reason\r\n";
    ps += "  $stamp=Get-Date -Format o\r\n";
    ps += "  $text=$stamp + [Environment]::NewLine + $summary + [Environment]::NewLine + '日志: %TEMP%\\xcat_update_apply.log'\r\n";
    ps += "  $tempNotify=Join-Path $env:TEMP 'xcat_update_failed.notify'\r\n";
    ps += "  try {\r\n";
    ps += "    Set-Content -LiteralPath $tempNotify -Value $text -Encoding UTF8\r\n";
    ps += "    Write-XCatLog ('update failed notify written (TEMP): ' + $tempNotify)\r\n";
    ps += "  } catch { Write-XCatLog ('update failed TEMP notify write failed: ' + ($_ | Out-String)) }\r\n";
    ps += "  if (-not $dest) { return $summary }\r\n";
    ps += "  try {\r\n";
    ps += "    $dir=Join-Path $dest 'XCat_data\\state'\r\n";
    ps += "    New-Item -ItemType Directory -Path $dir -Force | Out-Null\r\n";
    ps += "    $p=Join-Path $dir 'update_failed.notify'\r\n";
    ps += "    Set-Content -LiteralPath $p -Value $text -Encoding UTF8\r\n";
    ps += "    Write-XCatLog ('update failed notify written: ' + $p)\r\n";
    ps += "  } catch { Write-XCatLog ('update failed notify write failed: ' + ($_ | Out-String)) }\r\n";
    ps += "  return $summary\r\n";
    ps += "}\r\n";
    // 失败提示：user32 MessageBoxW（独立进程，不依赖 WScript）；未确认显示再试 msg.exe。
    ps += "function Show-XCatUpdateFailedUi($summary) {\r\n";
    ps += "  $msg='更新失败：' + $summary + [Environment]::NewLine + [Environment]::NewLine + '详情见 %TEMP%\\xcat_update_apply.log'\r\n";
    ps += "  if ($msg.Length -gt 500) { $msg=$msg.Substring(0,497) + '...' }\r\n";
    ps += "  $shown=$false\r\n";
    ps += "  try {\r\n";
    ps += "    $ui=Join-Path $env:TEMP ('xcat_update_fail_ui_' + [guid]::NewGuid().ToString('N') + '.ps1')\r\n";
    ps += "    $ready=$ui + '.ready'\r\n";
    ps += "    $uiBody=@'\r\n";
    ps += "param([Parameter(Mandatory=$true)][string]$Text)\r\n";
    ps += "$readyPath = $PSCommandPath + '.ready'\r\n";
    ps += "try {\r\n";
    ps += "  Add-Type -TypeDefinition @\"\r\n";
    ps += "using System;\r\n";
    ps += "using System.Runtime.InteropServices;\r\n";
    ps += "public static class XCatNativeMsg {\r\n";
    ps += "  [DllImport(\"user32.dll\", CharSet = CharSet.Unicode)]\r\n";
    ps += "  public static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);\r\n";
    ps += "}\r\n";
    ps += "\"@\r\n";
    ps += "  Set-Content -LiteralPath $readyPath -Value 'ready' -Encoding ASCII\r\n";
    ps += "  # 0x10=MB_ICONERROR 0x40000=MB_SETFOREGROUND 0x1000=MB_SYSTEMMODAL\r\n";
    ps += "  [void][XCatNativeMsg]::MessageBoxW([IntPtr]::Zero, $Text, 'XCat 更新失败', 0x10 -bor 0x40000 -bor 0x1000)\r\n";
    ps += "} finally {\r\n";
    ps += "  Remove-Item -LiteralPath $readyPath -Force -ErrorAction SilentlyContinue\r\n";
    ps += "  Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue\r\n";
    ps += "}\r\n";
    ps += "'@\r\n";
    // 外层 @'...'@ 不展开 $；子脚本内容里的 $Text/$readyPath 会原样写入文件。
    ps += "    Set-Content -LiteralPath $ui -Value $uiBody -Encoding UTF8\r\n";
    ps += "    Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File', $ui, '-Text', $msg) -WindowStyle Hidden | Out-Null\r\n";
    ps += "    $deadline=(Get-Date).AddSeconds(4)\r\n";
    ps += "    while ((Get-Date) -lt $deadline) {\r\n";
    ps += "      if (Test-Path -LiteralPath $ready -PathType Leaf) { $shown=$true; break }\r\n";
    ps += "      Start-Sleep -Milliseconds 100\r\n";
    ps += "    }\r\n";
    ps += "    if ($shown) { Write-XCatLog ('update failed UI MessageBox ready path=' + $ui) }\r\n";
    ps += "    else { Write-XCatLog ('update failed UI MessageBox not confirmed within timeout path=' + $ui) }\r\n";
    ps += "  } catch { Write-XCatLog ('update failed MessageBox launch failed: ' + ($_ | Out-String)) }\r\n";
    ps += "  if ($shown) { return }\r\n";
    ps += "  try {\r\n";
    ps += "    $msgExe=Join-Path $env:SystemRoot 'System32\\msg.exe'\r\n";
    ps += "    if (-not (Test-Path -LiteralPath $msgExe -PathType Leaf)) { throw 'msg.exe missing' }\r\n";
    ps += "    $short=((($msg -replace \"`r\",'') -replace \"`n\",' ').Trim())\r\n";
    ps += "    if ($short.Length -gt 220) { $short=$short.Substring(0,217) + '...' }\r\n";
    ps += "    Start-Process -FilePath $msgExe -ArgumentList @('*','/TIME:15', $short) -WindowStyle Hidden | Out-Null\r\n";
    ps += "    Write-XCatLog 'update failed UI msg.exe launched'\r\n";
    ps += "  } catch { Write-XCatLog ('update failed UI msg.exe failed: ' + ($_ | Out-String)) }\r\n";
    ps += "}\r\n";
    // 删不掉时先挪到 TEMP，避免安装目录旁留下 __xcat_old_* 邻居包。
    ps += "function Remove-XCatAsideTrash($asidePath) {\r\n";
    ps += "  if (-not $asidePath -or -not (Test-Path -LiteralPath $asidePath)) { return }\r\n";
    ps += "  if (Remove-XCatPathRetry $asidePath 4) { Write-XCatLog ('old install trash removed: ' + $asidePath); return }\r\n";
    ps += "  $trash=Join-Path $env:TEMP ('xcat_update_trash_' + [guid]::NewGuid().ToString('N'))\r\n";
    ps += "  try {\r\n";
    ps += "    Move-Item -LiteralPath $asidePath -Destination $trash -Force -ErrorAction Stop\r\n";
    ps += "    Write-XCatLog ('old install moved to TEMP trash: ' + $trash)\r\n";
    ps += "    if (Remove-XCatPathRetry $trash 3) { Write-XCatLog ('TEMP trash removed: ' + $trash) }\r\n";
    ps += "    else { Write-XCatLog ('TEMP trash left for OS cleanup: ' + $trash) }\r\n";
    ps += "  } catch { Write-XCatLog ('aside trash relocate failed: ' + $asidePath + ' :: ' + $_.Exception.Message) }\r\n";
    ps += "}\r\n";
    // 整目录改名让位；失败返回 $null（由调用方降级为逐文件原地覆盖）。
    ps += "function Move-XCatInstallAside($finalDest) {\r\n";
    ps += "  if (-not (Test-Path -LiteralPath $finalDest)) { return $null }\r\n";
    ps += "  $parent=Split-Path -Parent $finalDest\r\n";
    ps += "  $leaf=Split-Path -Leaf $finalDest\r\n";
    ps += "  $aside=$null\r\n";
    ps += "  for ($i=1; $i -le 6; $i++) {\r\n";
    ps += "    $aside=Join-Path $parent ($leaf + '.__xcat_old_' + [guid]::NewGuid().ToString('N').Substring(0,8))\r\n";
    ps += "    Clear-XCatAttrs $finalDest\r\n";
    ps += "    try { Rename-Item -LiteralPath $finalDest -NewName (Split-Path -Leaf $aside) -ErrorAction Stop; Write-XCatLog ('install moved aside -> ' + $aside); return $aside } catch { Write-XCatLog ('rename aside retry ' + $i + ': ' + $_.Exception.Message) }\r\n";
    ps += "    Start-Sleep -Milliseconds (300 * $i)\r\n";
    ps += "  }\r\n";
    ps += "  Write-XCatLog ('rename aside gave up (dir in use): ' + $finalDest)\r\n";
    ps += "  return $null\r\n";
    ps += "}\r\n";
    // rename 失败时的降级：尽量删光子项再 Copy-Item 覆盖；关键文件删不掉则硬失败。
    ps += "function Clear-XCatInstallForInPlace($finalDest) {\r\n";
    ps += "  if (-not (Test-Path -LiteralPath $finalDest)) { return }\r\n";
    ps += "  foreach ($it in @(Get-ChildItem -LiteralPath $finalDest -Force -ErrorAction SilentlyContinue)) {\r\n";
    ps += "    if (-not (Remove-XCatPathRetry $it.FullName 6)) { Write-XCatLog ('in-place clean leftover: ' + $it.FullName) }\r\n";
    ps += "  }\r\n";
    ps += "  foreach ($rel in @('xcat.exe','XCat_data\\xcat.dll','XCat_data\\state')) {\r\n";
    ps += "    $p=Join-Path $finalDest $rel\r\n";
    ps += "    if (-not (Test-Path -LiteralPath $p)) { continue }\r\n";
    ps += "    if (-not (Remove-XCatPathRetry $p 8)) {\r\n";
    ps += "      if ($rel -eq 'XCat_data\\state') { Write-XCatLog ('in-place state cleanup leftover: ' + $p); continue }\r\n";
    ps += "      throw ('in-place update blocked; locked file: ' + $p)\r\n";
    ps += "    }\r\n";
    ps += "  }\r\n";
    ps += "}\r\n";
    // 固定名桌面快捷方式；清理指向旧安装目录 / 旧包名文件夹的 .lnk
    ps += "function Update-XCatDesktopShortcut($finalDest, $oldDest) {\r\n";
    ps += "  $desktop=[Environment]::GetFolderPath('Desktop')\r\n";
    ps += "  if (-not $desktop) { Write-XCatLog 'desktop shortcut skip: no Desktop path'; return }\r\n";
    ps += "  $exe=Join-Path $finalDest 'xcat.exe'\r\n";
    ps += "  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Write-XCatLog 'desktop shortcut skip: missing xcat.exe'; return }\r\n";
    ps += "  $shell=New-Object -ComObject WScript.Shell\r\n";
    ps += "  $keepName='XCat.lnk'\r\n";
    ps += "  $keepPath=Join-Path $desktop $keepName\r\n";
    ps += "  $oldNorm=$null\r\n";
    ps += "  if ($oldDest) { try { $oldNorm=[System.IO.Path]::GetFullPath($oldDest).TrimEnd('\\').ToLowerInvariant() } catch {} }\r\n";
    ps += "  $newNorm=[System.IO.Path]::GetFullPath($finalDest).TrimEnd('\\').ToLowerInvariant()\r\n";
    ps += "  Get-ChildItem -LiteralPath $desktop -Filter '*.lnk' -Force -ErrorAction SilentlyContinue | ForEach-Object {\r\n";
    ps += "    $lnk=$_\r\n";
    ps += "    if ([string]::Equals($lnk.FullName, $keepPath, [StringComparison]::OrdinalIgnoreCase)) { return }\r\n";
    ps += "    $drop=$false\r\n";
    ps += "    if ($lnk.BaseName -match '^(?i)(xcat|xcat_for_twms_.*)$') { $drop=$true }\r\n";
    ps += "    try {\r\n";
    ps += "      $sc=$shell.CreateShortcut($lnk.FullName)\r\n";
    ps += "      $tp=$sc.TargetPath\r\n";
    ps += "      if ($tp -and ([System.IO.Path]::GetFileName($tp) -ieq 'xcat.exe')) {\r\n";
    ps += "        $td=[System.IO.Path]::GetDirectoryName($tp)\r\n";
    ps += "        if ($td) {\r\n";
    ps += "          $tdNorm=[System.IO.Path]::GetFullPath($td).TrimEnd('\\').ToLowerInvariant()\r\n";
    ps += "          if ($tdNorm -eq $newNorm) { $drop=$true }\r\n";
    ps += "          elseif ($oldNorm -and ($tdNorm -eq $oldNorm -or $tdNorm.StartsWith($oldNorm + '\\'))) { $drop=$true }\r\n";
    ps += "          elseif ((Split-Path -Leaf $td) -match '^xcat_for_twms_') { $drop=$true }\r\n";
    ps += "        }\r\n";
    ps += "      }\r\n";
    ps += "    } catch {}\r\n";
    ps += "    if ($drop) {\r\n";
    ps += "      try { Remove-Item -LiteralPath $lnk.FullName -Force -ErrorAction Stop; Write-XCatLog ('removed old shortcut: ' + $lnk.Name) } catch { Write-XCatLog ('remove shortcut failed: ' + $lnk.Name) }\r\n";
    ps += "    }\r\n";
    ps += "  }\r\n";
    ps += "  $scNew=$shell.CreateShortcut($keepPath)\r\n";
    ps += "  $scNew.TargetPath=$exe\r\n";
    ps += "  $scNew.WorkingDirectory=$finalDest\r\n";
    ps += "  $scNew.IconLocation=$exe + ',0'\r\n";
    ps += "  $scNew.Description='XCat'\r\n";
    ps += "  $scNew.Save()\r\n";
    ps += "  Write-XCatLog ('desktop shortcut ready: ' + $keepPath)\r\n";
    ps += "}\r\n";
    ps += "$pidToWait=" + std::to_string(pid) + "\r\n";
    ps += "$zipPath=" + PsQuote(zipPath) + "\r\n";
    ps += "$oldDest=" + PsQuote(installDir) + "\r\n";
    ps += "$work=Join-Path $env:TEMP ('xcat_update_apply_' + [guid]::NewGuid().ToString('N'))\r\n";
    ps += "$stage=Join-Path $env:TEMP ('xcat_update_stage_' + [guid]::NewGuid().ToString('N'))\r\n";
    ps += "$prevLogsBak=$null\r\n";
    ps += "$userPrefsBak=$null\r\n";
    ps += "$installAside=$null\r\n";
    ps += "$installSwapped=$false\r\n";
    ps += "$finalDest=$oldDest\r\n";
    ps += "$recoverExitZero=$false\r\n";
    ps += "$installCommitted=$false\r\n";
    ps += "try {\r\n";
    ps += "Write-XCatLog ('begin zip=' + $zipPath + ' oldDest=' + $oldDest)\r\n";
    ps += "while (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 200 }\r\n";
    // TWMS：仅确认启动器已退出，不强制清游戏进程。
    ps += "Wait-XCatProcessGone 'xcat' 0 8 $false\r\n";
    ps += "Write-XCatLog 'pre-update process recheck ok'\r\n";
    ps += "New-Item -ItemType Directory -Path $work -Force | Out-Null\r\n";
    ps += "Add-Type -AssemblyName System.IO.Compression.FileSystem\r\n";
    ps += "[System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $work)\r\n";
    ps += "$roots=@(Get-ChildItem -LiteralPath $work -Directory -Force)\r\n";
    ps += "$src=if ($roots.Count -eq 1) { $roots[0].FullName } else { $work }\r\n";
    ps += "if (-not (Test-Path -LiteralPath (Join-Path $src 'xcat.exe') -PathType Leaf)) { throw 'update zip missing xcat.exe' }\r\n";
    ps += "if (-not (Test-Path -LiteralPath (Join-Path $src 'XCat_data\\xcat.dll') -PathType Leaf)) { Write-XCatLog 'WARN update zip has no XCat_data\\xcat.dll (TWMS launcher-only package ok)' }\r\n";
    // 先落到 stage，校验通过后再写入安装目录；禁止「先清空 dest」。
    ps += "if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }\r\n";
    ps += "New-Item -ItemType Directory -Path $stage -Force | Out-Null\r\n";
    ps += "Copy-Item -Path (Join-Path $src '*') -Destination $stage -Recurse -Force\r\n";
    ps += "if (-not (Test-Path -LiteralPath (Join-Path $stage 'xcat.exe') -PathType Leaf)) { throw 'stage missing xcat.exe' }\r\n";
    // 包内预执行钩子：仅当「当前启动器」已含本调用时才会跑；脚本在新包 stage 里。
    // 契约见 packaging/update/pre_apply.ps1。失败则中止换包（勿半套迁移）。
    ps += "$preApply=Join-Path $stage 'XCat_data\\update\\pre_apply.ps1'\r\n";
    ps += "if (Test-Path -LiteralPath $preApply -PathType Leaf) {\r\n";
    ps += "  Write-XCatLog ('pre_apply begin path=' + $preApply)\r\n";
    ps += "  try { Unblock-File -LiteralPath $preApply -ErrorAction SilentlyContinue } catch {}\r\n";
    ps += "  $prevEap=$ErrorActionPreference\r\n";
    ps += "  $ErrorActionPreference='Stop'\r\n";
    ps += "  try {\r\n";
    ps += "    $hookOut=@(& $preApply -OldDest $oldDest -FinalDest $finalDest -Stage $stage -Work $work 2>&1)\r\n";
    ps += "    foreach ($line in $hookOut) { Write-XCatLog ('pre_apply| ' + $line) }\r\n";
    ps += "    Write-XCatLog 'pre_apply ok'\r\n";
    ps += "  } catch {\r\n";
    ps += "    throw ('pre_apply failed: ' + ($_ | Out-String))\r\n";
    ps += "  } finally { $ErrorActionPreference=$prevEap }\r\n";
    ps += "} else {\r\n";
    ps += "  Write-XCatLog 'pre_apply skipped (no XCat_data\\update\\pre_apply.ps1 in package)'\r\n";
    ps += "}\r\n";
    // 全员 ≥build106 后：白名单保留 user.ini（含 [buffs]/[core]）+ buffs.lkg/control.lkg 兜底 + 多技能勾选；
    // 其余 state（赶路学习图/测谎运行态/IPC .bin/冷启标记）仍丢弃，包内 travel_* 种子始终用新包。
    ps += "Write-XCatLog ('stage user prefs whitelist + prev logs; discard runtime state; dest=' + $finalDest)\r\n";
    // 换包会冲掉 logs；先快照到 TEMP，落新包后再写回 XCat_data\\logs\\prev。
    ps += "$prevLogsBak=Join-Path $env:TEMP ('xcat_prev_logs_' + [guid]::NewGuid().ToString('N'))\r\n";
    ps += "try {\r\n";
    ps += "  New-Item -ItemType Directory -Path $prevLogsBak -Force | Out-Null\r\n";
    ps += "  foreach ($rel in @('logs\\launcher.jsonl','logs\\launcher.log','XCat_data\\logs\\x.jsonl','XCat_data\\logs\\x.log')) {\r\n";
    ps += "    $s=Join-Path $oldDest $rel\r\n";
    ps += "    if (Test-Path -LiteralPath $s -PathType Leaf) { Copy-Item -LiteralPath $s -Destination (Join-Path $prevLogsBak (Split-Path -Leaf $rel)) -Force -ErrorAction SilentlyContinue }\r\n";
    ps += "  }\r\n";
    ps += "  if (Test-Path -LiteralPath $log -PathType Leaf) { Copy-Item -LiteralPath $log -Destination (Join-Path $prevLogsBak 'update_apply.log') -Force -ErrorAction SilentlyContinue }\r\n";
    ps += "  Write-XCatLog ('prev logs staged -> ' + $prevLogsBak)\r\n";
    ps += "} catch { Write-XCatLog ('preserve prev logs failed: ' + ($_ | Out-String)); $prevLogsBak=$null }\r\n";
    // 用户偏好白名单：在 rename/in-place 清盘前拷到 TEMP（与日志同源，旧目录尚在）。
    // 单文件失败不整单作废：已 staged 的仍会还原。
    ps += "$userPrefsBak=Join-Path $env:TEMP ('xcat_user_prefs_' + [guid]::NewGuid().ToString('N'))\r\n";
    ps += "$prefsCopied=0\r\n";
    ps += "try { New-Item -ItemType Directory -Path $userPrefsBak -Force | Out-Null } catch {\r\n";
    ps += "  Write-XCatLog ('stage user prefs mkdir failed: ' + ($_ | Out-String)); $userPrefsBak=$null\r\n";
    ps += "}\r\n";
    ps += "if ($userPrefsBak) {\r\n";
    ps += "  foreach ($leaf in @('user.ini','multiskill_select.tsv','buffs.lkg','control.lkg')) {\r\n";
    ps += "    $s=Join-Path $oldDest ('XCat_data\\state\\' + $leaf)\r\n";
    ps += "    if (-not (Test-Path -LiteralPath $s -PathType Leaf)) { continue }\r\n";
    ps += "    try {\r\n";
    ps += "      Copy-Item -LiteralPath $s -Destination (Join-Path $userPrefsBak $leaf) -Force -ErrorAction Stop\r\n";
    ps += "      $prefsCopied++\r\n";
    ps += "      Write-XCatLog ('user pref staged: ' + $leaf)\r\n";
    ps += "    } catch { Write-XCatLog ('user pref stage failed: ' + $leaf + ' :: ' + $_.Exception.Message) }\r\n";
    ps += "  }\r\n";
    ps += "  if ($prefsCopied -eq 0) {\r\n";
    ps += "    Write-XCatLog 'user prefs whitelist: nothing to stage'\r\n";
    ps += "    Remove-Item -LiteralPath $userPrefsBak -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    ps += "    $userPrefsBak=$null\r\n";
    ps += "  } else { Write-XCatLog ('user prefs staged -> ' + $userPrefsBak + ' count=' + $prefsCopied) }\r\n";
    ps += "}\r\n";
    // 优先整目录 rename 让位；桌面等场景 rename 失败则降级逐文件原地覆盖。
    // 偏好已在 TEMP；运行时 state 仍丢弃；rename 成功时失败可回滚 aside。
    ps += "$installMode='fresh'\r\n";
    ps += "if (Test-Path -LiteralPath $finalDest) {\r\n";
    ps += "  Write-XCatLog ('before rename aside cwd=' + (Get-Location).Path + ' dest=' + $finalDest)\r\n";
    ps += "  $installAside=Move-XCatInstallAside $finalDest\r\n";
    ps += "  $installSwapped=[bool]$installAside\r\n";
    ps += "  if ($installSwapped) { $installMode='rename-aside' }\r\n";
    ps += "  else {\r\n";
    ps += "    $installMode='in-place-overwrite'\r\n";
    ps += "    Write-XCatLog 'rename aside failed; fallback to in-place overwrite'\r\n";
    ps += "    Clear-XCatInstallForInPlace $finalDest\r\n";
    ps += "  }\r\n";
    ps += "}\r\n";
    ps += "New-Item -ItemType Directory -Path $finalDest -Force | Out-Null\r\n";
    ps += "Copy-Item -Path (Join-Path $stage '*') -Destination $finalDest -Recurse -Force -ErrorAction Stop\r\n";
    // 原地覆盖时旧 state 可能删不干净：包内只有 travel_*.tsv 等种子，
    // Copy-Item 盖不掉包里没有的 user.ini / multiskill_select.tsv → 按包白名单再清一轮。
    // 另清 legacy 多发勾选路径，避免 ReadMultiSkillSelect 把旧 dumps/skill_catalog 勾选迁回 state。
    // 用户偏好稍后从 TEMP 白名单还原（不依赖残留）。
    ps += "if ($installMode -eq 'in-place-overwrite') {\r\n";
    ps += "  $dstState=Join-Path $finalDest 'XCat_data\\state'\r\n";
    ps += "  $srcState=Join-Path $stage 'XCat_data\\state'\r\n";
    ps += "  if (Test-Path -LiteralPath $dstState) { Remove-XCatPathRetry $dstState 6 | Out-Null }\r\n";
    ps += "  if (Test-Path -LiteralPath $srcState) {\r\n";
    ps += "    New-Item -ItemType Directory -Path $dstState -Force | Out-Null\r\n";
    ps += "    Copy-Item -Path (Join-Path $srcState '*') -Destination $dstState -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    ps += "  }\r\n";
    ps += "  $criticalStateLeftover=New-Object 'System.Collections.Generic.List[string]'\r\n";
    ps += "  if (Test-Path -LiteralPath $dstState) {\r\n";
    ps += "    $keep=@{}\r\n";
    ps += "    if (Test-Path -LiteralPath $srcState) {\r\n";
    ps += "      Get-ChildItem -LiteralPath $srcState -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {\r\n";
    ps += "        $rel=$_.FullName.Substring($srcState.Length).TrimStart('\\')\r\n";
    ps += "        if ($rel) { $keep[$rel.ToLowerInvariant()]=$true }\r\n";
    ps += "      }\r\n";
    ps += "    }\r\n";
    ps += "    $criticalNames=@('user.ini','multiskill_select.tsv','buffs.lkg','control.lkg')\r\n";
    ps += "    Get-ChildItem -LiteralPath $dstState -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {\r\n";
    ps += "      $rel=$_.FullName.Substring($dstState.Length).TrimStart('\\')\r\n";
    ps += "      if (-not $rel) { return }\r\n";
    ps += "      if ($keep.ContainsKey($rel.ToLowerInvariant())) { return }\r\n";
    ps += "      $leaf=(Split-Path -Leaf $rel).ToLowerInvariant()\r\n";
    // 失败通知可能跨会话残留，勿当脏配置清掉。
    ps += "      if ($leaf -eq 'update_failed.notify') { return }\r\n";
    ps += "      $isCritical=$criticalNames -contains $leaf\r\n";
    ps += "      $tries=if ($isCritical) { 8 } else { 4 }\r\n";
    ps += "      if (Remove-XCatPathRetry $_.FullName $tries) { Write-XCatLog ('in-place state purged: ' + $rel) }\r\n";
    ps += "      else {\r\n";
    ps += "        Write-XCatLog ('in-place state purge leftover: ' + $_.FullName)\r\n";
    ps += "        if ($isCritical) { [void]$criticalStateLeftover.Add($rel) }\r\n";
    ps += "      }\r\n";
    ps += "    }\r\n";
    // 空目录壳不影响业务，顺手清掉以免脏树越积越多。
    ps += "    Get-ChildItem -LiteralPath $dstState -Recurse -Directory -Force -ErrorAction SilentlyContinue |\r\n";
    ps += "      Sort-Object { $_.FullName.Length } -Descending |\r\n";
    ps += "      ForEach-Object {\r\n";
    ps += "        $kids=@(Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue)\r\n";
    ps += "        if ($kids.Count -eq 0) { Remove-XCatPathRetry $_.FullName 2 | Out-Null }\r\n";
    ps += "      }\r\n";
    ps += "  }\r\n";
    ps += "  foreach ($legacyRel in @(\r\n";
    ps += "    'XCat_data\\skill_catalog\\multiskill_select.tsv',\r\n";
    ps += "    'XCat_data\\dumps\\Lua\\skill_catalog\\multiskill_select.tsv',\r\n";
    ps += "    'dumps\\Lua\\skill_catalog\\multiskill_select.tsv'\r\n";
    ps += "  )) {\r\n";
    ps += "    $legacy=Join-Path $finalDest $legacyRel\r\n";
    ps += "    if (-not (Test-Path -LiteralPath $legacy -PathType Leaf)) { continue }\r\n";
    ps += "    if (Remove-XCatPathRetry $legacy 8) { Write-XCatLog ('in-place legacy multiskill purged: ' + $legacyRel) }\r\n";
    ps += "    else {\r\n";
    ps += "      Write-XCatLog ('WARN in-place legacy multiskill leftover: ' + $legacy)\r\n";
    ps += "      [void]$criticalStateLeftover.Add($legacyRel)\r\n";
    ps += "    }\r\n";
    ps += "  }\r\n";
    ps += "  if ($criticalStateLeftover.Count -gt 0) {\r\n";
    ps += "    Write-XCatLog ('WARN critical state leftovers after purge: ' + ([string]::Join('; ', $criticalStateLeftover)))\r\n";
    ps += "  }\r\n";
    ps += "  Write-XCatLog 'in-place state replaced from package (prefs restore follows)'\r\n";
    ps += "}\r\n";
    // 还原白名单偏好：覆盖包默认/残留；并剥掉行程运行态 section，避免跨版本半截补给行程。
    ps += "if ($userPrefsBak -and (Test-Path -LiteralPath $userPrefsBak)) {\r\n";
    ps += "  try {\r\n";
    ps += "    $dstState=Join-Path $finalDest 'XCat_data\\state'\r\n";
    ps += "    New-Item -ItemType Directory -Path $dstState -Force | Out-Null\r\n";
    ps += "    foreach ($leaf in @('user.ini','multiskill_select.tsv','buffs.lkg','control.lkg')) {\r\n";
    ps += "      $s=Join-Path $userPrefsBak $leaf\r\n";
    ps += "      if (-not (Test-Path -LiteralPath $s -PathType Leaf)) { continue }\r\n";
    ps += "      $d=Join-Path $dstState $leaf\r\n";
    ps += "      Copy-Item -LiteralPath $s -Destination $d -Force -ErrorAction Stop\r\n";
    ps += "      Write-XCatLog ('user pref restored: ' + $leaf)\r\n";
    ps += "    }\r\n";
    ps += "    $iniPath=Join-Path $dstState 'user.ini'\r\n";
    ps += "    if (Test-Path -LiteralPath $iniPath -PathType Leaf) {\r\n";
    ps += "      $dropSections=@('auto_supply_status')\r\n";
    // Get-Content UTF8 会吃掉读入 BOM；写回必须用无 BOM，否则 C++ LoadIni 会把首段 [meta] 认废。
    ps += "      $lines=@(Get-Content -LiteralPath $iniPath -Encoding UTF8)\r\n";
    ps += "      $kept=New-Object 'System.Collections.Generic.List[string]'\r\n";
    ps += "      $skip=$false\r\n";
    ps += "      $stripped=$false\r\n";
    ps += "      foreach ($line in $lines) {\r\n";
    ps += "        if ($line -match '^\\s*\\[([^\\]]+)\\]\\s*$') {\r\n";
    ps += "          $sec=$Matches[1].Trim()\r\n";
    ps += "          $skip=$false\r\n";
    ps += "          foreach ($drop in $dropSections) { if ($sec -ieq $drop) { $skip=$true; break } }\r\n";
    ps += "          if ($skip) { $stripped=$true; Write-XCatLog ('user.ini stripped runtime section: [' + $sec + ']'); continue }\r\n";
    ps += "        }\r\n";
    ps += "        if (-not $skip) { [void]$kept.Add($line) }\r\n";
    ps += "      }\r\n";
    ps += "      if ($stripped) {\r\n";
    ps += "        $utf8NoBom=New-Object System.Text.UTF8Encoding $false\r\n";
    ps += "        [System.IO.File]::WriteAllLines($iniPath, $kept.ToArray(), $utf8NoBom)\r\n";
    ps += "        Write-XCatLog 'user.ini rewritten without BOM after section strip'\r\n";
    ps += "      }\r\n";
    ps += "    }\r\n";
    ps += "    Write-XCatLog 'user prefs whitelist restore done'\r\n";
    ps += "  } catch { Write-XCatLog ('restore user prefs failed: ' + ($_ | Out-String)) }\r\n";
    ps += "}\r\n";
    ps += "Write-XCatLog ('install mode=' + $installMode + '; package seeds + user prefs whitelist')\r\n";
    // 包内后执行钩子：新树已就位且白名单偏好已还原；可做合并/修补。失败中止（此时已 committed 前）。
    // 注意：须在 $installCommitted=$true 之前，以便失败仍可 aside 回滚。
    ps += "$postApply=Join-Path $stage 'XCat_data\\update\\post_apply.ps1'\r\n";
    ps += "if (Test-Path -LiteralPath $postApply -PathType Leaf) {\r\n";
    ps += "  Write-XCatLog ('post_apply begin path=' + $postApply)\r\n";
    ps += "  try { Unblock-File -LiteralPath $postApply -ErrorAction SilentlyContinue } catch {}\r\n";
    ps += "  $prevEap=$ErrorActionPreference\r\n";
    ps += "  $ErrorActionPreference='Stop'\r\n";
    ps += "  $asideArg=if ($installAside) { [string]$installAside } else { '' }\r\n";
    ps += "  $prefsBakArg=if ($userPrefsBak) { [string]$userPrefsBak } else { '' }\r\n";
    ps += "  try {\r\n";
    ps += "    $hookOut=@(& $postApply -OldDest $oldDest -FinalDest $finalDest -Stage $stage -Work $work -InstallAside $asideArg -InstallMode $installMode -UserPrefsBak $prefsBakArg 2>&1)\r\n";
    ps += "    foreach ($line in $hookOut) { Write-XCatLog ('post_apply| ' + $line) }\r\n";
    ps += "    Write-XCatLog 'post_apply ok'\r\n";
    ps += "  } catch {\r\n";
    ps += "    throw ('post_apply failed: ' + ($_ | Out-String))\r\n";
    ps += "  } finally { $ErrorActionPreference=$prevEap }\r\n";
    ps += "} else {\r\n";
    ps += "  Write-XCatLog 'post_apply skipped (no XCat_data\\update\\post_apply.ps1 in package)'\r\n";
    ps += "}\r\n";
    ps += "if ($prevLogsBak -and (Test-Path -LiteralPath $prevLogsBak)) {\r\n";
    ps += "  try {\r\n";
    ps += "    $prevLogs=Join-Path $finalDest 'XCat_data\\logs\\prev'\r\n";
    ps += "    New-Item -ItemType Directory -Path $prevLogs -Force | Out-Null\r\n";
    ps += "    Copy-Item -Path (Join-Path $prevLogsBak '*') -Destination $prevLogs -Recurse -Force\r\n";
    ps += "    if (Test-Path -LiteralPath $log -PathType Leaf) { Copy-Item -LiteralPath $log -Destination (Join-Path $prevLogs 'update_apply.log') -Force -ErrorAction SilentlyContinue }\r\n";
    ps += "    Write-XCatLog ('prev logs restored -> ' + $prevLogs)\r\n";
    ps += "  } catch { Write-XCatLog ('restore prev logs failed: ' + ($_ | Out-String)) }\r\n";
    ps += "}\r\n";
    ps += "if (-not (Test-Path -LiteralPath (Join-Path $finalDest 'xcat.exe') -PathType Leaf)) { throw 'install missing xcat.exe after copy' }\r\n";
    ps += "if (-not (Test-Path -LiteralPath (Join-Path $finalDest 'XCat_data\\xcat.dll') -PathType Leaf)) { Write-XCatLog 'WARN install has no xcat.dll after copy (TWMS launcher-only package ok)' }\r\n";
    // 关键文件已就位：此后失败只重拉 finalDest，禁止再把 aside 滚回来盖掉新包。
    ps += "$installCommitted=$true\r\n";
    ps += "Write-XCatLog 'install committed (exe verified)'\r\n";
    ps += "try { Update-XCatDesktopShortcut $finalDest $oldDest } catch { Write-XCatLog ('desktop shortcut failed: ' + ($_ | Out-String)) }\r\n";
    // 安装过程中 NGS/残留可能又拉起 msw/NGM；拉起前清一轮 + 固定冷却 10s，再短确认。
    ps += "Wait-XCatProcessGone 'xcat' 0 5 $false\r\n";
    ps += "Write-XCatLog 'pre-relaunch process settle 10s'\r\n";
    ps += "Start-Sleep -Seconds 10\r\n";
    ps += "Wait-XCatProcessGone 'xcat' 0 3 $false\r\n";
    // 只写启动器冷启标记；新启动器在完整冷启成功前保留该标记，失败重试仍强制清栈。
    ps += "$coldFlagDir=Join-Path $finalDest 'XCat_data\\state'\r\n";
    ps += "New-Item -ItemType Directory -Path $coldFlagDir -Force | Out-Null\r\n";
    ps += "$coldFlag=Join-Path $coldFlagDir 'post_update_cold_start.flag'\r\n";
    ps += "Set-Content -LiteralPath $coldFlag -Value ((Get-Date -Format o) + ' post_update_cold_start') -Encoding UTF8\r\n";
    ps += "Write-XCatLog ('post-update cold-start flag written: ' + $coldFlag)\r\n";
    // 先清 aside，再拉起：避免新实例旁长期挂着 __xcat_old_* 邻居目录。
    ps += "if ($installAside -and (Test-Path -LiteralPath $installAside)) { Remove-XCatAsideTrash $installAside; $installAside=$null }\r\n";
    ps += "Write-XCatLog ('copy ok; restart launcher dest=' + $finalDest)\r\n";
    ps += "Start-Process -FilePath (Join-Path $finalDest 'xcat.exe') -WorkingDirectory $finalDest\r\n";
    ps += "} catch {\r\n";
    ps += "  $failReason=($_ | Out-String)\r\n";
    ps += "  Write-XCatLog ('FAILED ' + $failReason)\r\n";
    ps += "  $rolledBack=$false\r\n";
    ps += "  if ((-not $installCommitted) -and $installSwapped -and $installAside -and (Test-Path -LiteralPath $installAside)) {\r\n";
    ps += "    try {\r\n";
    ps += "      if (Test-Path -LiteralPath $finalDest) { Remove-XCatPathRetry $finalDest 3 | Out-Null }\r\n";
    ps += "      if (-not (Test-Path -LiteralPath $finalDest)) {\r\n";
    ps += "        Rename-Item -LiteralPath $installAside -NewName (Split-Path -Leaf $finalDest) -ErrorAction Stop\r\n";
    ps += "        Write-XCatLog ('rolled back install from aside: ' + $installAside)\r\n";
    ps += "        $installAside=$null\r\n";
    ps += "        $rolledBack=$true\r\n";
    ps += "      } else {\r\n";
    ps += "        Write-XCatLog ('rollback skipped: finalDest still present; manual restore: 1) delete/rename away broken [' + $finalDest + '] 2) rename [' + $installAside + '] -> [' + (Split-Path -Leaf $finalDest) + ']')\r\n";
    ps += "      }\r\n";
    ps += "    } catch { Write-XCatLog ('rollback failed: ' + ($_ | Out-String) + '; manual restore: 1) delete/rename away broken [' + $finalDest + '] 2) rename [' + $installAside + '] -> [' + (Split-Path -Leaf $finalDest) + ']') }\r\n";
    ps += "  } elseif ($installCommitted -and $installAside -and (Test-Path -LiteralPath $installAside)) {\r\n";
    ps += "    Write-XCatLog 'install committed; skip rollback; trash leftover aside'\r\n";
    ps += "    Remove-XCatAsideTrash $installAside\r\n";
    ps += "    $installAside=$null\r\n";
    ps += "  }\r\n";
    // 先落失败通知再拉起：state + TEMP；新包靠 Consume 气泡，旧包/无进程靠 Popup。
    ps += "  $failSummary=Write-XCatUpdateFailedNotify $finalDest $failReason\r\n";
    ps += "  if ($oldDest -and ($oldDest -ne $finalDest)) { Write-XCatUpdateFailedNotify $oldDest $failReason | Out-Null }\r\n";
    // 启动器已 ExitProcess；committed / 回滚成功 / 未 swap：只要 finalDest 有 exe 就重拉。
    ps += "  $relaunched=$false\r\n";
    ps += "  $relaunchExe=Join-Path $finalDest 'xcat.exe'\r\n";
    ps += "  $canRelaunchFinal=$installCommitted -or $rolledBack -or (-not $installSwapped)\r\n";
    ps += "  if ($canRelaunchFinal -and (Test-Path -LiteralPath $relaunchExe -PathType Leaf)) {\r\n";
    ps += "    try {\r\n";
    ps += "      Wait-XCatProcessGone 'xcat' 0 3 $false\r\n";
    ps += "      Write-XCatLog 'failure-relaunch process settle 10s'\r\n";
    ps += "      Start-Sleep -Seconds 10\r\n";
    ps += "      Start-Process -FilePath $relaunchExe -WorkingDirectory $finalDest -ErrorAction Stop\r\n";
    ps += "      $relaunched=$true\r\n";
    ps += "      Write-XCatLog 'relaunched launcher after update failure'\r\n";
    ps += "    } catch { Write-XCatLog ('relaunch after failure failed: ' + ($_ | Out-String)) }\r\n";
    ps += "  }\r\n";
    // Popup 兜底：无 relaunch、或回滚/未 committed（多半旧包无 Consume）。新包已拉起则只靠 ImGui。
    ps += "  if ((-not $relaunched) -or $rolledBack -or (-not $installCommitted)) {\r\n";
    ps += "    Show-XCatUpdateFailedUi $failSummary\r\n";
    ps += "  }\r\n";
    // 已恢复则可 exit 0；先落到 finally 清 TEMP，再退出（勿在 catch 里直接 exit 跳过清理）。
    ps += "  if ($relaunched) { $recoverExitZero=$true; Write-XCatLog 'update failed but launcher recovered; will exit 0 after cleanup' }\r\n";
    ps += "  else { throw }\r\n";
    ps += "}\r\n";
    ps += "finally {\r\n";
    ps += "  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    ps += "  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    ps += "  if ($prevLogsBak) { Remove-Item -LiteralPath $prevLogsBak -Recurse -Force -ErrorAction SilentlyContinue }\r\n";
    ps += "  if ($userPrefsBak) { Remove-Item -LiteralPath $userPrefsBak -Recurse -Force -ErrorAction SilentlyContinue }\r\n";
    ps += "}\r\n";
    ps += "if ($recoverExitZero) { exit 0 }\r\n";
    if (!WriteUtf8FileBom(scriptPath, ps)) {
        err = "写入更新脚本失败";
        return false;
    }
    const std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                                scriptPath + L"\"";
    // lpDirectory=TEMP：勿继承 xcat 的安装目录 cwd（否则 rename aside 必败）。
    wchar_t tempDir[MAX_PATH]{};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempDir);
    const wchar_t* workDir = (tempLen > 0 && tempLen < MAX_PATH) ? tempDir : nullptr;
    HINSTANCE r =
        ShellExecuteW(nullptr, L"open", L"powershell.exe", params.c_str(), workDir, SW_HIDE);
    if (reinterpret_cast<uintptr_t>(r) <= 32) {
        err = "启动更新脚本失败";
        return false;
    }
    return true;
}

void DownloadWorker();

std::string ResolveDownloadUrl(const std::string& manifestUrl, const Manifest& m) {
    ParsedUrl parsed{};
    if (!ParseUrl(manifestUrl, parsed)) return {};
    if (m.downloadUrl.rfind("http://", 0) == 0 || m.downloadUrl.rfind("https://", 0) == 0) {
        return m.downloadUrl;
    }
    if (!m.downloadUrl.empty() && m.downloadUrl[0] == '/') return parsed.origin + m.downloadUrl;
    if (!m.downloadUrl.empty()) return ManifestUrlDirectory(manifestUrl) + m.downloadUrl;
    if (!m.zipName.empty()) return ManifestUrlDirectory(manifestUrl) + m.zipName;
    return {};
}

void CheckWorker(std::string serviceUrl) {
    const std::string manifestUrl = UpdateManifestUrlFromServiceUrl(serviceUrl);
    if (manifestUrl.empty()) {
        SetSnapshot(UpdatePhase::Failed, "请先填写上报/更新服务器地址");
        return;
    }
    xcat::log::Info("Update", "check begin manifest=%s", manifestUrl.c_str());
    const HttpResult resp = HttpGetText(manifestUrl);
    if (!resp.err.empty()) {
        xcat::log::Warn("Update", "check failed manifest=%s err=%s", manifestUrl.c_str(),
                        resp.err.c_str());
        SetSnapshot(UpdatePhase::Failed, resp.err);
        return;
    }
    if (resp.status != 200) {
        char msg[96]{};
        snprintf(msg, sizeof(msg), "检查更新失败 HTTP %lu", static_cast<unsigned long>(resp.status));
        xcat::log::Warn("Update", "check failed manifest=%s status=%lu", manifestUrl.c_str(),
                        static_cast<unsigned long>(resp.status));
        SetSnapshot(UpdatePhase::Failed, msg);
        return;
    }
    Manifest manifest = ParseManifest(resp.body);
    if (manifest.buildId == 0 || manifest.sha256.empty()) {
        SetSnapshot(UpdatePhase::Failed, "更新 manifest 无效");
        return;
    }
    const std::string downloadUrl = ResolveDownloadUrl(manifestUrl, manifest);
    if (downloadUrl.empty()) {
        SetSnapshot(UpdatePhase::Failed, "更新包地址无效");
        return;
    }

    bool shouldAutoDownload = false;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.manifest = manifest;
        g_state.manifestUrl = manifestUrl;
        g_state.downloadUrl = downloadUrl;
        g_state.snapshot.manifestUrl = manifestUrl;
        g_state.snapshot.latestVersion = manifest.version;
        g_state.snapshot.currentBuildId = xcat::kXcatBuildId;
        g_state.snapshot.latestBuildId = manifest.buildId;
        g_state.snapshot.zipName = manifest.zipName;
        g_state.snapshot.zipPath.clear();
        if (manifest.buildId > xcat::kXcatBuildId) {
            xcat::log::Info("Update", "available current=%u latest=%u version=%s zip=%s",
                            xcat::kXcatBuildId, manifest.buildId, manifest.version.c_str(),
                            manifest.zipName.c_str());
            if (g_state.autoInstallAfterDownload) {
                SetPhaseUiLocked(UpdatePhase::Downloading, "发现新版本，下载安装中...");
                g_state.snapshot.progress = -1.f;
                shouldAutoDownload = true;
            } else {
                SetPhaseUiLocked(UpdatePhase::UpdateAvailable, "发现新版本");
            }
        } else {
            g_state.autoInstallAfterDownload = false;
            SetPhaseUiLocked(UpdatePhase::UpToDate, "已是最新版本");
            xcat::log::Info("Update", "up-to-date current=%u latest=%u", xcat::kXcatBuildId,
                            manifest.buildId);
        }
    }
    if (shouldAutoDownload) {
        DownloadWorker();
    }
}

void DownloadWorker() {
    Manifest manifest{};
    std::string downloadUrl;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        manifest = g_state.manifest;
        downloadUrl = g_state.downloadUrl;
    }
    if (downloadUrl.empty()) {
        SetSnapshot(UpdatePhase::Failed, "请先检查更新");
        return;
    }
    const std::wstring zipPath =
        std::filesystem::path(TempUpdateDir()) / xcat::Utf8ToWide(SanitizeFileName(manifest.zipName));
    std::string err;
    xcat::log::Info("Update", "download begin url=%s zip=%s", downloadUrl.c_str(),
                    manifest.zipName.c_str());
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        SetPhaseUiLocked(UpdatePhase::Downloading, "开始下载更新…");
        g_state.snapshot.progress = -1.f;
        g_state.snapshot.downloadedBytes = 0;
        g_state.snapshot.totalBytes = manifest.size;
    }
    if (!DownloadFile(downloadUrl, zipPath, manifest.size, err)) {
        xcat::log::Warn("Update", "download failed url=%s err=%s", downloadUrl.c_str(),
                        err.c_str());
        SetSnapshot(UpdatePhase::Failed, err);
        return;
    }
    if (manifest.size > 0) {
        std::error_code ec;
        const uint64_t actualSize = std::filesystem::file_size(zipPath, ec);
        if (ec || actualSize != manifest.size) {
            char msg[160]{};
            snprintf(msg, sizeof(msg), "更新包大小不匹配 expected=%llu actual=%llu",
                     static_cast<unsigned long long>(manifest.size),
                     static_cast<unsigned long long>(ec ? 0 : actualSize));
            xcat::log::Warn("Update", "%s", msg);
            SetSnapshot(UpdatePhase::Failed, msg);
            return;
        }
    }
    std::string digest;
    SetSnapshot(UpdatePhase::Downloading, "校验更新包完整性…");
    if (!Sha256File(zipPath, digest, err)) {
        xcat::log::Warn("Update", "sha256 failed zip=%s err=%s", xcat::WideToUtf8(zipPath).c_str(),
                        err.c_str());
        SetSnapshot(UpdatePhase::Failed, err);
        return;
    }
    if (digest != manifest.sha256) {
        xcat::log::Warn("Update", "sha256 mismatch zip=%s expected=%s actual=%s",
                        xcat::WideToUtf8(zipPath).c_str(), manifest.sha256.c_str(),
                        digest.c_str());
        SetSnapshot(UpdatePhase::Failed, "更新包 SHA-256 不匹配");
        return;
    }

    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.snapshot.zipPath = xcat::WideToUtf8(zipPath);
    SetPhaseUiLocked(UpdatePhase::ReadyToInstall,
                     g_state.autoInstallAfterDownload ? "下载完成，准备安装并重启..."
                                                      : "更新包已下载，等待安装");
    xcat::log::Info("Update", "download ok zip=%s sha256=%s autoInstall=%d",
                    g_state.snapshot.zipPath.c_str(), digest.c_str(),
                    g_state.autoInstallAfterDownload ? 1 : 0);
}

void ForcePollWorker(std::string serviceUrl, std::string payloadBinDir) {
    const auto finish = []() {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.forcePollInFlight = false;
    };

    ParsedUrl parsed{};
    if (!ParseUrl(serviceUrl, parsed)) {
        finish();
        return;
    }

    const std::string forceUrl =
        parsed.origin + UpdateForcePathFromServicePath(xcat::WideToUtf8(parsed.path));

    // 探活带上本机身份，运维端才能把同公网 IP / NAT 后的多台电脑拆开。
    const ClientHostIdentity id = ResolveClientHostIdentity(payloadBinDir);
    auto sanitizeHdr = [](std::wstring s) {
        for (wchar_t& ch : s) {
            if (ch == L'\r' || ch == L'\n' || ch == L'\0') ch = L'_';
        }
        if (s.size() > 96) s.resize(96);
        return s;
    };
    wchar_t identityHeaders[512]{};
    {
        char ver[64]{};
        std::snprintf(ver, sizeof(ver), "%s build %u", xcat::kXcatVersionString, xcat::kXcatBuildId);
        const std::wstring machineW = sanitizeHdr(xcat::Utf8ToWide(id.machine));
        const std::wstring deviceW = sanitizeHdr(xcat::Utf8ToWide(id.deviceId));
        const std::wstring verW = sanitizeHdr(xcat::Utf8ToWide(ver));
        _snwprintf(identityHeaders, 512,
                   L"X-XCat-Machine: %s\r\nX-XCat-Device-Id: %s\r\nX-XCat-App-Version: %s\r\n",
                   machineW.c_str(), deviceW.c_str(), verW.c_str());
    }

    const HttpResult resp = HttpGetText(forceUrl, identityHeaders);
    if (!resp.err.empty()) {
        xcat::log::Warn("Update", "force update poll failed url=%s err=%s", forceUrl.c_str(),
                        resp.err.c_str());
        finish();
        return;
    }
    if (resp.status == 404) {
        finish();
        return;
    }
    if (resp.status != 200) {
        xcat::log::Warn("Update", "force update poll failed url=%s status=%lu", forceUrl.c_str(),
                        static_cast<unsigned long>(resp.status));
        finish();
        return;
    }

    const Manifest forced = ParseManifest(resp.body);
    if (forced.buildId <= xcat::kXcatBuildId || forced.sha256.size() != 64 ||
        forced.zipName.empty()) {
        finish();
        return;
    }

    const std::string forceDownloadUrl = ResolveDownloadUrl(forceUrl, forced);
    if (forceDownloadUrl.empty()) {
        xcat::log::Warn("Update", "force update marker has no valid package url=%s", forceUrl.c_str());
        finish();
        return;
    }

    bool shouldUpdate = false;
    bool skipBusy = false;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        // 已在下载/安装排队时不要打断，否则会清掉 zipPath 导致强更半截失败。
        if (g_state.snapshot.phase == UpdatePhase::Checking ||
            g_state.snapshot.phase == UpdatePhase::Downloading ||
            g_state.snapshot.phase == UpdatePhase::Installing ||
            g_state.snapshot.phase == UpdatePhase::ReadyToInstall) {
            skipBusy = true;
        } else {
            g_state.manifest = forced;
            g_state.manifestUrl = forceUrl;
            g_state.downloadUrl = forceDownloadUrl;
            g_state.snapshot.manifestUrl = forceUrl;
            g_state.snapshot.latestVersion = forced.version;
            g_state.snapshot.currentBuildId = xcat::kXcatBuildId;
            g_state.snapshot.latestBuildId = forced.buildId;
            g_state.snapshot.zipName = forced.zipName;
            g_state.snapshot.zipPath.clear();
            SetPhaseUiLocked(UpdatePhase::Downloading, "运维已推送强制更新，正在下载安装...");
            g_state.snapshot.progress = -1.f;
            g_state.autoInstallAfterDownload = true;
            shouldUpdate = true;
        }
    }
    if (skipBusy) {
        finish();
        return;
    }
    if (shouldUpdate) {
        xcat::log::Info("Update", "force update received build=%llu current=%u",
                        static_cast<unsigned long long>(forced.buildId), xcat::kXcatBuildId);
        DownloadWorker();
    }
    finish();
}

}  // namespace

std::string UpdateManifestUrlFromServiceUrl(const std::string& serviceUrl) {
    ParsedUrl parsed{};
    if (!ParseUrl(serviceUrl, parsed)) return {};
    return parsed.origin + UpdatePathFromServicePath(xcat::WideToUtf8(parsed.path));
}

bool UpdateBusy() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    return g_state.snapshot.phase == UpdatePhase::Checking ||
           g_state.snapshot.phase == UpdatePhase::Downloading ||
           g_state.snapshot.phase == UpdatePhase::Installing;
}

bool UpdateNeedsVisibleUi() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    switch (g_state.snapshot.phase) {
        case UpdatePhase::Checking:
        case UpdatePhase::Downloading:
        case UpdatePhase::ReadyToInstall:
        case UpdatePhase::Installing:
            return true;
        default:
            return false;
    }
}

bool UpdateShouldDrawProgressUi() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    switch (g_state.snapshot.phase) {
        case UpdatePhase::Checking:
        case UpdatePhase::Downloading:
        case UpdatePhase::ReadyToInstall:
        case UpdatePhase::Installing:
            return true;
        case UpdatePhase::UpToDate:
        case UpdatePhase::Failed:
            return g_state.stickyProgressUiUntilMs != 0 &&
                   GetTickCount64() < g_state.stickyProgressUiUntilMs;
        default:
            return false;
    }
}

void LoadAutoReceiveUpdates(const std::string& payloadBinDir) {
    // 强制开启：把旧版 user.ini autoReceive=0 迁成 1，避免后人误读盘上关闭值。
    if (!payloadBinDir.empty()) (void)WriteAutoReceiveUpdates(payloadBinDir, true);
}

bool AutoReceiveUpdatesEnabled() {
    return true;
}

bool SetAutoReceiveUpdatesEnabled(const std::string& payloadBinDir, bool enabled) {
    // UI 已移除；强制保持开启，避免旧调用把偏好写成关闭。
    (void)enabled;
    return WriteAutoReceiveUpdates(payloadBinDir, true);
}

bool StartUpdateCheck(const std::string& serviceUrl) {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (g_state.snapshot.phase == UpdatePhase::Checking ||
            g_state.snapshot.phase == UpdatePhase::Downloading ||
            g_state.snapshot.phase == UpdatePhase::ReadyToInstall ||
            g_state.snapshot.phase == UpdatePhase::Installing) {
            return false;
        }
        SetPhaseUiLocked(UpdatePhase::Checking, "检查更新中...");
        g_state.snapshot.zipPath.clear();
        // 一键更新：有新版本则自动下载，UI 侧 TryStartAutoInstall 后装包重启。
        g_state.autoInstallAfterDownload = true;
    }
    std::thread(CheckWorker, serviceUrl).detach();
    return true;
}

bool StartUpdateDownload() {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (g_state.snapshot.phase != UpdatePhase::UpdateAvailable) return false;
        SetPhaseUiLocked(UpdatePhase::Downloading, "下载安装中...");
        g_state.snapshot.progress = -1.f;
        g_state.snapshot.zipPath.clear();
        g_state.autoInstallAfterDownload = true;
    }
    std::thread(DownloadWorker).detach();
    return true;
}

void UpdateForcePollTick(const std::string& serviceUrl, const std::string& payloadBinDir) {
    if (serviceUrl.empty()) return;

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (g_state.forcePollInFlight ||
            (g_state.lastForcePollMs != 0 && now - g_state.lastForcePollMs < 30000)) {
            return;
        }
        g_state.lastForcePollMs = now;
        g_state.forcePollInFlight = true;
    }
    std::thread(ForcePollWorker, serviceUrl, payloadBinDir).detach();
}

void InstallWorker(std::string installDir) {
    std::string zipUtf8;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (g_state.snapshot.phase != UpdatePhase::Installing ||
            g_state.snapshot.zipPath.empty()) {
            zipUtf8.clear();
        } else {
            zipUtf8 = g_state.snapshot.zipPath;
        }
    }
    if (zipUtf8.empty()) {
        SetSnapshot(UpdatePhase::Failed, "没有可安装的更新包");
        return;
    }

    // TWMS：本轮不强制清游戏进程；换包脚本会等本进程退出后改目录。
    SetInstallStatus("等待启动器退出并释放目录…", -1.f);
    Sleep(1500);

    SetInstallStatus("正在启动安装脚本并重启…", -1.f);
    std::string err;
    if (!LaunchUpdaterScript(xcat::Utf8ToWide(zipUtf8), xcat::Utf8ToWide(installDir), err)) {
        SetSnapshot(UpdatePhase::Failed, err);
        return;
    }
    xcat::log::Info("Update", "apply update zip=%s", zipUtf8.c_str());
    SetInstallStatus("安装脚本已启动，正在退出以释放目录…", -1.f);
    g_requestExitForUpdate.store(true, std::memory_order_release);
}

bool StartInstallDownloadedUpdate(const std::string& installDir) {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (g_state.snapshot.phase != UpdatePhase::ReadyToInstall ||
            g_state.snapshot.zipPath.empty()) {
            return false;
        }
        g_state.autoInstallAfterDownload = false;
        SetPhaseUiLocked(UpdatePhase::Installing, "准备安装更新（请勿关闭窗口）…");
        g_state.snapshot.progress = -1.f;
    }
    std::thread(InstallWorker, installDir).detach();
    return true;
}

bool TryStartAutoInstall(const std::string& installDir) {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.autoInstallAfterDownload ||
            g_state.snapshot.phase != UpdatePhase::ReadyToInstall ||
            g_state.snapshot.zipPath.empty()) {
            if (g_state.snapshot.phase == UpdatePhase::Failed) {
                g_state.autoInstallAfterDownload = false;
            }
            return false;
        }
        g_state.autoInstallAfterDownload = false;
        SetPhaseUiLocked(UpdatePhase::Installing, "准备安装更新（请勿关闭窗口）…");
        g_state.snapshot.progress = -1.f;
    }
    std::thread(InstallWorker, installDir).detach();
    return true;
}

bool ConsumeUpdateProcessExitRequest() {
    return g_requestExitForUpdate.exchange(false, std::memory_order_acq_rel);
}

UpdateSnapshot GetUpdateSnapshot() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    return g_state.snapshot;
}

bool ConsumePostUpdateColdStartRequest(const std::string& payloadBinDir) {
    if (payloadBinDir.empty()) return false;
    std::string path = payloadBinDir;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "state\\post_update_cold_start.flag";
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    // 清栈门禁失败后必须保留标记，确保下一次启动仍强制冷启，而非热附加残留会话。
    xcat::log::Info("Update", "detected post-update cold-start flag path=%s", path.c_str());
    return true;
}

bool ClearPostUpdateColdStartRequest(const std::string& payloadBinDir) {
    if (payloadBinDir.empty()) return false;
    std::string path = payloadBinDir;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "state\\post_update_cold_start.flag";
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }
    if (!DeleteFileA(path.c_str())) {
        xcat::log::Warn("Update", "post-update cold-start flag delete failed path=%s gle=%lu",
                        path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    xcat::log::Info("Update", "cleared post-update cold-start flag path=%s", path.c_str());
    return true;
}

bool ConsumeUpdateFailedNotify(const std::string& payloadBinDir) {
    auto readNotifyFile = [](const std::string& path, std::string& outBody) -> bool {
        const DWORD attr = GetFileAttributesA(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return false;
        }
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
        char buf[1536]{};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        outBody.assign(buf, n);
        if (!DeleteFileA(path.c_str())) {
            xcat::log::Warn("Update", "update_failed.notify delete failed path=%s gle=%lu",
                            path.c_str(), static_cast<unsigned long>(GetLastError()));
        }
        return true;
    };

    std::string path;
    std::string body;
    bool found = false;
    if (!payloadBinDir.empty()) {
        path = payloadBinDir;
        if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
        path += "state\\update_failed.notify";
        found = readNotifyFile(path, body);
    }
    // state 缺失时读 TEMP 兜底（目录被清 / 旧包无 Consume 后手动开新包）。
    char tempDir[MAX_PATH]{};
    const DWORD tempLen = GetTempPathA(MAX_PATH, tempDir);
    std::string tempPath;
    if (tempLen > 0 && tempLen < MAX_PATH) {
        tempPath.assign(tempDir);
        tempPath += "xcat_update_failed.notify";
        std::string tempBody;
        if (readNotifyFile(tempPath, tempBody)) {
            if (!found) {
                body.swap(tempBody);
                path = tempPath;
                found = true;
            }
        }
    }
    if (!found) return false;

    // 去 UTF-8 BOM；格式：时间戳 / 摘要 / 日志提示。
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    std::string detail;
    {
        size_t i = 0;
        auto nextLine = [&](std::string& line) -> bool {
            line.clear();
            if (i >= body.size()) return false;
            const size_t start = i;
            while (i < body.size() && body[i] != '\r' && body[i] != '\n') ++i;
            line.assign(body, start, i - start);
            while (i < body.size() && (body[i] == '\r' || body[i] == '\n')) ++i;
            return true;
        };
        std::string line;
        (void)nextLine(line);  // timestamp
        while (nextLine(line)) {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            if (b) line.erase(0, b);
            if (line.empty()) continue;
            if (line.rfind("At ", 0) == 0 || line.rfind("+", 0) == 0 || line.rfind("---", 0) == 0) {
                continue;
            }
            if (line.rfind("\xE6\x97\xA5\xE5\xBF\x97:", 0) == 0) continue;  // "日志:"

            detail = std::move(line);
            break;
        }
    }
    if (detail.empty()) {
        detail = "自动更新安装失败，请查看 %TEMP%\\xcat_update_apply.log";
    }
    if (detail.size() > 280) {
        detail.resize(277);
        detail += "...";
    }

    NotifyUpdateFail("更新失败", detail.c_str());
    xcat::log::Warn("Update", "consumed update_failed.notify path=%s detail=%s", path.c_str(),
                    detail.c_str());
    return true;
}

}  // namespace xcat::app
