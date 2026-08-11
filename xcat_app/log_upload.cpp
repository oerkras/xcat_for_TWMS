#include "log_upload.h"

#include "process_util.h"
#include "xcat_config_ini.h"
#include "xcat_log.h"
#include "xcat_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xcat::app {
namespace {

constexpr size_t kMaxBytesPerLog = 512 * 1024;  // 与 payload 单文件轮转上限对齐（整文件收取）
// 全量上限与 payload LogInit maxBackups / kLogUploadBackupsFull 对齐；收集时再按 mode 截断。
constexpr size_t kMaxLogBackups = kLogUploadBackupsFull;
// 会话创建未返回 maxFiles 时的保守上限（旧服务曾默认 512）。
constexpr size_t kFallbackSessionMaxFiles = 512;
constexpr size_t kMaxLieEventsZipBytes = 12 * 1024 * 1024;      // 与服务端 softMax / max-file 对齐
constexpr size_t kMaxLieEventsStageBytes = 48 * 1024 * 1024;    // 压缩前暂存预算（BMP 压缩比高）
constexpr size_t kMaxLieEventsFiles = 120;
constexpr size_t kMinLieEventsFiles = 4;
constexpr size_t kMinLieEventsStageBytes = 512 * 1024;
constexpr int kLieEventsZipShrinkAttempts = 8;
constexpr DWORD kUploadTimeoutMs = 180000;      // 含 zip 大文件

// meta.json / 上报 JSON 的 appVersion：与 UI、launcher 启动日志同源。
std::string AppVersionLabel() {
    char buf[64]{};
    snprintf(buf, sizeof(buf), "%s build %u", xcat::kXcatVersionString, xcat::kXcatBuildId);
    return buf;
}

enum class LieEventsAttach : uint8_t {
    Absent = 0,    // 本地无 lie_events
    Attached = 1,  // 已加入 lie_events.zip（可能已抽样缩容）
    Failed = 2,    // 本地有事件但未能附带
};

constexpr uint32_t kLogUploadIniVersion = 1u;

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    std::wstring path;  // .../v1/logs （兼容旧协议）
    std::wstring root;  // 服务根，如 /fengxing 或空
};

struct LogBlob {
    std::string name;
    std::string source;
    uint64_t    size = 0;
    bool        truncated = false;
    std::string bytes;
};

struct HttpResult {
    DWORD       status = 0;
    std::string body;
    std::string err;
};

struct State {
    std::mutex        mtx;
    LogUploadSnapshot snapshot{};
    std::vector<LogUploadHistoryEntry> history;
};

State g_state;

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

std::string PayloadBinDirFromExe(const std::string& exeBinDir) {
    return xcat::JoinBinPath(exeBinDir.c_str(), "XCat_data");
}

bool EnsureStateDir(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return false;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%sstate", payloadBinDir);
    CreateDirectoryA(dir, nullptr);
    return true;
}

bool ReadLegacyLogUploadIni(const std::string& exeBinDir, std::string& url) {
    if (exeBinDir.empty()) return false;
    const std::wstring ini =
        (std::filesystem::path(xcat::Utf8ToWide(exeBinDir)) / L"xcat_log_upload.ini").wstring();
    if (!std::filesystem::exists(ini)) return false;

    wchar_t buf[1024]{};
    GetPrivateProfileStringW(L"LogUpload", L"url", L"", buf,
                             static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), ini.c_str());
    if (!buf[0]) return false;
    url = xcat::WideToUtf8(buf);
    return !url.empty();
}

LogUploadMode ParseUploadMode(std::string_view raw) {
    if (raw == "full" || raw == "Full" || raw == "FULL") return LogUploadMode::Full;
    return LogUploadMode::Light;
}

bool ReadLogUploadIni(const char* payloadBinDir, std::string& url, LogUploadMode* outMode,
                      uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (outMode) *outMode = LogUploadMode::Light;
    if (!payloadBinDir || !payloadBinDir[0]) return false;

    xcat::IniStore ini{};
    const std::string path = xcat::UserConfigIniPath(payloadBinDir);
    if (!xcat::LoadIniFile(path.c_str(), ini)) return false;

    uint32_t version = 0;
    if (!xcat::IniGetU32(ini, "log_upload", "version", version) || version != kLogUploadIniVersion)
        return false;

    std::string value;
    if (!xcat::IniGetString(ini, "log_upload", "url", value) || value.empty()) return false;
    url = value;
    if (outWriteTick) xcat::IniGetU64(ini, "log_upload", "writeTickMs", *outWriteTick);
    if (outMode) {
        std::string modeStr;
        if (xcat::IniGetString(ini, "log_upload", "uploadMode", modeStr) && !modeStr.empty()) {
            *outMode = ParseUploadMode(modeStr);
        }
    }
    return true;
}

bool WriteLogUploadIni(const char* payloadBinDir, const std::string& url, LogUploadMode mode,
                       uint64_t writeTickMs) {
    if (!payloadBinDir || !payloadBinDir[0]) return false;
    if (!EnsureStateDir(payloadBinDir)) return false;

    const std::string path = xcat::UserConfigIniPath(payloadBinDir);
    return xcat::UpdateIniFile(path.c_str(), [&](xcat::IniStore& ini) {
        xcat::IniSetU32(ini, "meta", "version", static_cast<uint32_t>(xcat::kUserConfigIniVersion));
        xcat::IniSetU32(ini, "log_upload", "version", kLogUploadIniVersion);
        xcat::IniSetU64(ini, "log_upload", "writeTickMs", writeTickMs);
        xcat::IniSetString(ini, "log_upload", "url", url.c_str());
        xcat::IniSetString(ini, "log_upload", "uploadMode", LogUploadModeLabel(mode));
    });
}

// 历史 Artale/创世/枫星服务根 → TWMS。上传与更新共用服务根。
std::string RedirectLegacyServiceUrl(const std::string& url) {
    if (url.empty()) return url;
    std::string out = url;
    auto rewritePath = [&](const char* from, const char* to) {
        const size_t fromLen = std::strlen(from);
        const size_t toLen = std::strlen(to);
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            const size_t after = pos + fromLen;
            if (after == out.size() || out[after] == '/' || out[after] == '?' ||
                out[after] == '#') {
                out.replace(pos, fromLen, to);
                pos += toLen;
            } else {
                pos = after;
            }
        }
    };
    rewritePath("/artale", "/twms");
    rewritePath("/chuangshi", "/twms");
    rewritePath("/fengxing", "/twms");
    // TWMS 默认口 18789；顺带收拢旧 Artale/枫星口
    auto rewritePort = [&](const char* from, const char* to) {
        size_t pos = 0;
        const std::string f = from;
        const std::string t = to;
        while ((pos = out.find(f, pos)) != std::string::npos) {
            out.replace(pos, f.size(), t);
            pos += t.size();
        }
    };
    rewritePort(":18787", ":18789");
    rewritePort(":18788", ":18789");
    // 开发期本机地址 → 公网域名（对齐枫星 xcat.work 默认）
    auto rewriteHost = [&](const char* from, const char* to) {
        size_t pos = 0;
        const std::string f = from;
        const std::string t = to;
        while ((pos = out.find(f, pos)) != std::string::npos) {
            out.replace(pos, f.size(), t);
            pos += t.size();
        }
    };
    rewriteHost("://127.0.0.1:", "://xcat.work:");
    rewriteHost("://localhost:", "://xcat.work:");
    while (out.size() > 8 && out.back() == '/') out.pop_back();
    return out;
}

bool ParseUrl(const std::string& url, ParsedUrl& out) {
    out = {};
    std::string u = url;
    if (u.rfind("https://", 0) == 0) {
        out.secure = true;
        out.port = INTERNET_DEFAULT_HTTPS_PORT;
        u = u.substr(8);
    } else if (u.rfind("http://", 0) == 0) {
        out.secure = false;
        out.port = INTERNET_DEFAULT_HTTP_PORT;
        u = u.substr(7);
    } else {
        return false;
    }

    const size_t slash = u.find('/');
    const std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    std::string pathText = slash == std::string::npos ? "/" : u.substr(slash);
    while (pathText.size() > 1 && pathText.back() == '/') pathText.pop_back();
    if (pathText.empty() || pathText == "/") {
        pathText = "/v1/logs";
    } else if (pathText.size() < 8 || pathText.substr(pathText.size() - 8) != "/v1/logs") {
        pathText += "/v1/logs";
    }
    out.path = xcat::Utf8ToWide(pathText);
    if (pathText.size() >= 8 && pathText.substr(pathText.size() - 8) == "/v1/logs") {
        out.root = xcat::Utf8ToWide(pathText.substr(0, pathText.size() - 8));
    } else {
        out.root.clear();
    }

    const size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        out.host = xcat::Utf8ToWide(hostPort.substr(0, colon));
        try {
            out.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
        } catch (...) {
            return false;
        }
    } else {
        out.host = xcat::Utf8ToWide(hostPort);
    }
    return !out.host.empty() && !out.path.empty();
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8]{};
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string Base64Encode(const std::string& bytes) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v =
            (static_cast<unsigned char>(bytes[i]) << 16) |
            (static_cast<unsigned char>(bytes[i + 1]) << 8) |
            static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
    }

    if (i < bytes.size()) {
        uint32_t v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? kTable[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

bool ReadTailBytes(const std::string& path, size_t maxBytes, LogBlob& out) {
    const std::wstring wide = xcat::Utf8ToWide(path);
    if (wide.empty()) return false;

    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }

    out.size = static_cast<uint64_t>(size.QuadPart);
    out.truncated = out.size > maxBytes;
    const uint64_t start = out.truncated ? out.size - maxBytes : 0;
    const DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(out.size - start, maxBytes));

    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(start);
    if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }

    std::string data(toRead, '\0');
    DWORD read = 0;
    const BOOL ok = toRead == 0 || ReadFile(file, data.data(), toRead, &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;

    data.resize(read);
    out.bytes = std::move(data);
    return true;
}

void AddLogIfPresent(std::vector<LogBlob>& logs, const std::string& name,
                     const std::string& source, const std::string& path) {
    if (path.empty()) return;
    LogBlob blob{};
    blob.name = name;
    blob.source = source;
    if (!ReadTailBytes(path, kMaxBytesPerLog, blob)) return;
    if (blob.bytes.empty() && blob.size == 0) return;
    logs.push_back(std::move(blob));
}

void AddRotatedLogsIfPresent(std::vector<LogBlob>& logs, const std::string& name,
                             const std::string& source, const std::string& path,
                             size_t maxBackups) {
    AddLogIfPresent(logs, name, source, path);
    const size_t cap = maxBackups > kMaxLogBackups ? kMaxLogBackups : maxBackups;
    for (size_t i = 1; i <= cap; ++i) {
        const std::string suffix = "." + std::to_string(i);
        AddLogIfPresent(logs, name + suffix, source + suffix, path + suffix);
    }
}

/** 解析 `combat.log` / `x.jsonl.12` → 基名；非日志文件返回空。 */
std::string ParseRotatedLogBaseName(const std::string& leaf) {
    if (leaf.empty() || leaf[0] == '.') return {};
    auto ends_with_ci = [](std::string_view s, std::string_view suf) {
        if (s.size() < suf.size()) return false;
        for (size_t i = 0; i < suf.size(); ++i) {
            const unsigned char a = static_cast<unsigned char>(s[s.size() - suf.size() + i]);
            const unsigned char b = static_cast<unsigned char>(suf[i]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    };
    if (ends_with_ci(leaf, ".log") || ends_with_ci(leaf, ".jsonl")) return leaf;

    // name.ext.N
    const size_t lastDot = leaf.rfind('.');
    if (lastDot == std::string::npos || lastDot == 0 || lastDot + 1 >= leaf.size()) return {};
    for (size_t i = lastDot + 1; i < leaf.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(leaf[i]))) return {};
    }
    const std::string base = leaf.substr(0, lastDot);
    if (ends_with_ci(base, ".log") || ends_with_ci(base, ".jsonl")) return base;
    return {};
}

/**
 * 扫 XCat_data/logs 下全部频道、全部现存卷（combat.log / combat.log.24 …）。
 * 已收录的精确文件名跳过；白名单已收的基名整族跳过；prev/ 另走专收。
 * maxBackups：轮转序号上限（.N 的 N）；当前卷（无序号）始终收。
 */
void AddFeatureChannelLogs(std::vector<LogBlob>& logs, const char* payloadBinDir,
                           size_t maxBackups) {
    if (!payloadBinDir || !payloadBinDir[0]) return;
    namespace fs = std::filesystem;
    const std::string logsDirUtf8 = xcat::JoinBinPath(payloadBinDir, "logs");
    std::error_code ec;
    const fs::path logsDir(xcat::Utf8ToWide(logsDirUtf8));
    if (!fs::is_directory(logsDir, ec)) return;

    std::unordered_set<std::string> already;
    already.reserve(logs.size() * 2 + 32);
    std::unordered_set<std::string> alreadyBases;
    for (const LogBlob& b : logs) {
        if (b.name.empty()) continue;
        already.insert(b.name);
        const std::string base = ParseRotatedLogBaseName(b.name);
        if (!base.empty()) alreadyBases.insert(base);
    }

    const size_t cap = maxBackups > kMaxLogBackups ? kMaxLogBackups : maxBackups;

    // 只读采证 BIN：默认已关，但磁盘上常残留数百 MB；勿进日常上传拖垮包体。
    // 要传时手动拷，或临时 XCAT_WALK_BIN=1 / XCAT_KEYMACRO_BIN=1 再开一轮只收那次。
    static const char* kSkipDiagBases[] = {
        "keypad_walk_bin.log",
        "key_macro_bin.log",
    };

    for (fs::directory_iterator it(logsDir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        const std::string leaf = xcat::WideToUtf8(it->path().filename().wstring());
        const std::string base = ParseRotatedLogBaseName(leaf);
        if (base.empty()) continue;
        bool skipDiag = false;
        for (const char* b : kSkipDiagBases) {
            if (base == b) {
                skipDiag = true;
                break;
            }
        }
        if (skipDiag) continue;
        if (alreadyBases.count(base)) continue;  // 白名单已收该频道（含其轮转）
        if (already.count(leaf)) continue;

        // leaf == base → 当前卷；leaf == base.N → 序号 N
        if (leaf.size() > base.size() + 1 && leaf[base.size()] == '.') {
            unsigned long idx = 0;
            try {
                idx = std::stoul(leaf.substr(base.size() + 1));
            } catch (...) {
                continue;
            }
            if (idx == 0 || idx > cap) continue;
        }

        AddLogIfPresent(logs, leaf, "XCat_data/logs/" + leaf,
                        xcat::WideToUtf8(it->path().wstring()));
        already.insert(leaf);
    }
}

int LieEventPriority(const std::filesystem::path& path) {
    const std::wstring name = path.filename().wstring();
    std::wstring lower = name;
    for (wchar_t& c : lower) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    // 元数据优先：保证抽样缩容后仍能还原题面/轨迹摘要。
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".txt") == 0) return 0;
    if (lower == L"summary.json" || lower == L"frames.jsonl") return 0;
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, L".json") == 0) return 1;
    if (lower.size() >= 6 && lower.compare(lower.size() - 6, 6, L".jsonl") == 0) return 1;
    if (lower.find(L".crop.") != std::wstring::npos) return 2;
    // BMP（测谎题截图）压缩比高；JPEG（鼠标题帧）几乎不压缩，优先级更低以免饿死题图。
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".bmp") == 0) return 3;
    if (lower.size() >= 4 &&
        (lower.compare(lower.size() - 4, 4, L".jpg") == 0 ||
         lower.compare(lower.size() - 5, 5, L".jpeg") == 0)) {
        return 4;
    }
    return 5;
}

bool ReadWholeFileCapped(const std::string& path, size_t maxBytes, LogBlob& out) {
    const std::wstring wide = xcat::Utf8ToWide(path);
    if (wide.empty()) return false;

    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    out.size = static_cast<uint64_t>(size.QuadPart);
    out.truncated = false;
    if (out.size == 0) {
        CloseHandle(file);
        out.bytes.clear();
        return true;
    }
    if (out.size > maxBytes) {
        CloseHandle(file);
        return false;
    }

    const DWORD toRead = static_cast<DWORD>(out.size);
    std::string data(toRead, '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, data.data(), toRead, &read, nullptr);
    CloseHandle(file);
    if (!ok || read != toRead) return false;
    out.bytes = std::move(data);
    return true;
}

std::string PsSingleQuote(const std::wstring& text) {
    std::string utf8 = xcat::WideToUtf8(text);
    std::string out = "'";
    out.reserve(utf8.size() + 8);
    for (char c : utf8) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

bool ZipDirectoryToFile(const std::filesystem::path& srcDir, const std::filesystem::path& zipPath,
                        bool includeBaseDirectory, std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(zipPath, ec);
    ec.clear();
    fs::create_directories(zipPath.parent_path(), ec);

    // 写成临时脚本，避免 -Command 引号嵌套踩坑（与 update apply 脚本同套路）。
    const fs::path scriptPath = zipPath.parent_path() / L"zip_lie_events.ps1";
    std::string ps;
    ps += "$ErrorActionPreference='Stop'\r\n";
    ps += "Add-Type -AssemblyName System.IO.Compression.FileSystem\r\n";
    ps += "[System.IO.Compression.ZipFile]::CreateFromDirectory(";
    ps += PsSingleQuote(srcDir.wstring());
    ps += ", ";
    ps += PsSingleQuote(zipPath.wstring());
    ps += ", [System.IO.Compression.CompressionLevel]::Optimal, ";
    ps += includeBaseDirectory ? "$true)\r\n" : "$false)\r\n";

    {
        const std::wstring wideScript = xcat::Utf8ToWide(ps);
        HANDLE file = CreateFileW(scriptPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            err = "写入压缩脚本失败";
            return false;
        }
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        const bool ok =
            WriteFile(file, bom, sizeof(bom), &written, nullptr) &&
            WriteFile(file, ps.data(), static_cast<DWORD>(ps.size()), &written, nullptr);
        CloseHandle(file);
        if (!ok) {
            err = "写入压缩脚本失败";
            return false;
        }
        (void)wideScript;
    }

    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"";
    cmd += scriptPath.wstring();
    cmd += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        err = "启动压缩进程失败";
        return false;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (wait != WAIT_OBJECT_0) {
        err = "压缩超时";
        return false;
    }
    if (exitCode != 0 || !fs::is_regular_file(zipPath, ec)) {
        err = "压缩失败";
        return false;
    }
    return true;
}

bool StageLieEventsSubset(const std::filesystem::path& root, const std::filesystem::path& stageRoot,
                          size_t maxFiles, size_t maxStageBytes, size_t& stagedFiles,
                          size_t& stagedBytes, size_t& skipped) {
    namespace fs = std::filesystem;
    stagedFiles = 0;
    stagedBytes = 0;
    skipped = 0;
    if (maxFiles == 0 || maxStageBytes == 0) return false;
    std::error_code ec;
    fs::remove_all(stageRoot, ec);
    ec.clear();
    fs::create_directories(stageRoot, ec);

    struct Candidate {
        fs::path path;
        fs::path rel;
        uint64_t size = 0;
        int priority = 5;
        fs::file_time_type mtime{};
    };
    std::vector<Candidate> candidates;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec) || ec) continue;
        Candidate c;
        c.path = entry.path();
        c.size = static_cast<uint64_t>(entry.file_size(ec));
        if (ec || c.size == 0) continue;
        c.mtime = entry.last_write_time(ec);
        c.priority = LieEventPriority(c.path);
        c.rel = fs::relative(c.path, root, ec);
        if (ec || c.rel.empty()) continue;
        candidates.push_back(std::move(c));
    }
    if (candidates.empty()) return false;

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.mtime > b.mtime;
    });

    for (const Candidate& c : candidates) {
        if (stagedFiles >= maxFiles ||
            stagedBytes + static_cast<size_t>(c.size) > maxStageBytes) {
            skipped += 1;
            continue;
        }
        const fs::path dest = stageRoot / c.rel;
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(c.path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            skipped += 1;
            ec.clear();
            continue;
        }
        stagedFiles += 1;
        stagedBytes += static_cast<size_t>(c.size);
    }
    return stagedFiles > 0;
}

LieEventsAttach AddLieEventsIfPresent(std::vector<LogBlob>& logs, const std::string& payloadBinDir) {
    namespace fs = std::filesystem;
    const std::string rootUtf8 =
        xcat::JoinBinPath(payloadBinDir.c_str(), "state\\lie_events");
    const fs::path root = fs::path(xcat::Utf8ToWide(rootUtf8));
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return LieEventsAttach::Absent;

    bool anyFile = false;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && !ec) {
            anyFile = true;
            break;
        }
    }
    if (!anyFile) return LieEventsAttach::Absent;

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const DWORD tick = GetTickCount();
    const DWORD pid = GetCurrentProcessId();
    const fs::path work =
        fs::path(temp) / (L"xcat_lie_events_" + std::to_wstring(pid) + L"_" + std::to_wstring(tick));
    const fs::path stage = work / L"stage";
    const fs::path zipPath = work / L"lie_events.zip";
    fs::create_directories(work, ec);

    size_t maxFiles = kMaxLieEventsFiles;
    size_t maxStageBytes = kMaxLieEventsStageBytes;

    for (int attempt = 0; attempt < kLieEventsZipShrinkAttempts; ++attempt) {
        size_t stagedFiles = 0;
        size_t stagedBytes = 0;
        size_t skipped = 0;
        if (!StageLieEventsSubset(root, stage, maxFiles, maxStageBytes, stagedFiles, stagedBytes,
                                  skipped)) {
            xcat::log::Warn("LogUpload", "lie_events stage empty attempt=%d maxFiles=%zu", attempt,
                            maxFiles);
            fs::remove_all(work, ec);
            return LieEventsAttach::Failed;
        }

        std::string zipErr;
        if (!ZipDirectoryToFile(stage, zipPath, /*includeBaseDirectory=*/false, zipErr)) {
            xcat::log::Warn("LogUpload", "lie_events zip failed: %s", zipErr.c_str());
            fs::remove_all(work, ec);
            return LieEventsAttach::Failed;
        }

        const uintmax_t zipSize = fs::file_size(zipPath, ec);
        if (ec || zipSize == 0) {
            xcat::log::Warn("LogUpload", "lie_events zip empty attempt=%d stagedFiles=%zu", attempt,
                            stagedFiles);
            fs::remove_all(work, ec);
            return LieEventsAttach::Failed;
        }

        if (zipSize <= kMaxLieEventsZipBytes) {
            LogBlob blob{};
            blob.name = "lie_events.zip";
            blob.source = "XCat_data/state/lie_events.zip";
            if (!ReadWholeFileCapped(xcat::WideToUtf8(zipPath.wstring()), kMaxLieEventsZipBytes,
                                     blob)) {
                xcat::log::Warn("LogUpload", "lie_events zip read failed");
                fs::remove_all(work, ec);
                return LieEventsAttach::Failed;
            }
            logs.push_back(std::move(blob));
            xcat::log::Info(
                "LogUpload",
                "lie_events zip ok files=%zu stageBytes=%zu zipBytes=%zu skipped=%zu attempt=%d",
                stagedFiles, stagedBytes, static_cast<size_t>(zipSize), skipped, attempt);
            fs::remove_all(work, ec);
            return LieEventsAttach::Attached;
        }

        // JPEG 鼠标题几乎不压缩：按超限比例缩容后重试，禁止静默丢弃整包。
        const double scale =
            (static_cast<double>(kMaxLieEventsZipBytes) * 0.85) / static_cast<double>(zipSize);
        size_t nextFiles =
            std::max(kMinLieEventsFiles, static_cast<size_t>(stagedFiles * scale));
        size_t nextBytes =
            std::max(kMinLieEventsStageBytes, static_cast<size_t>(stagedBytes * scale));
        if (nextFiles >= maxFiles && nextBytes >= maxStageBytes) {
            nextFiles = std::max(kMinLieEventsFiles, maxFiles / 2);
            nextBytes = std::max(kMinLieEventsStageBytes, maxStageBytes / 2);
        }
        if (nextFiles >= stagedFiles && nextBytes >= stagedBytes &&
            stagedFiles <= kMinLieEventsFiles) {
            xcat::log::Warn(
                "LogUpload",
                "lie_events zip too large after shrink size=%llu limit=%zu stagedFiles=%zu",
                static_cast<unsigned long long>(zipSize), kMaxLieEventsZipBytes, stagedFiles);
            fs::remove_all(work, ec);
            return LieEventsAttach::Failed;
        }

        xcat::log::Warn(
            "LogUpload",
            "lie_events zip shrink size=%llu limit=%zu stagedFiles=%zu -> maxFiles=%zu "
            "maxStageBytes=%zu attempt=%d",
            static_cast<unsigned long long>(zipSize), kMaxLieEventsZipBytes, stagedFiles,
            nextFiles, nextBytes, attempt);
        maxFiles = nextFiles;
        maxStageBytes = nextBytes;
        fs::remove(zipPath, ec);
        ec.clear();
    }

    xcat::log::Warn("LogUpload", "lie_events zip shrink exhausted");
    fs::remove_all(work, ec);
    return LieEventsAttach::Failed;
}

struct CollectedLogs {
    std::vector<LogBlob> logs;
    LieEventsAttach lieEvents = LieEventsAttach::Absent;
};

CollectedLogs CollectLogs(const LogUploadRequest& req) {
    CollectedLogs out;
    const std::string launcherJsonl = xcat::log::paths::LauncherLog(req.exeBinDir.c_str());
    const std::string injectJsonl = xcat::log::paths::InjectLog(req.exeBinDir.c_str());
    const std::string payloadJsonl = xcat::log::paths::PayloadLog(req.payloadBinDir.c_str());
    const std::string overlayJsonl = xcat::JoinBinPath(req.payloadBinDir.c_str(),
                                                       "state\\overlay_host.jsonl");
    const std::string freezeIncidentJsonl =
        xcat::JoinBinPath(req.payloadBinDir.c_str(), "logs\\freeze_incident.jsonl");
    const size_t backups = LogUploadBackupsForMode(req.mode);

    // 现行：只收集 JSONL。旧版 .log / app.jsonl 若仍残留则顺带捡起，便于过渡期排障。
    AddRotatedLogsIfPresent(out.logs, "freeze_incident.jsonl",
                            "XCat_data/logs/freeze_incident.jsonl", freezeIncidentJsonl, backups);
    AddRotatedLogsIfPresent(out.logs, "freeze_incident.log",
                            "XCat_data/logs/freeze_incident.log",
                            xcat::log::paths::TextLog(freezeIncidentJsonl), backups);
    AddRotatedLogsIfPresent(out.logs, "x.jsonl", "XCat_data/logs/x.jsonl", payloadJsonl, backups);
    AddRotatedLogsIfPresent(out.logs, "x.log", "XCat_data/logs/x.log",
                            xcat::log::paths::TextLog(payloadJsonl), backups);
    AddRotatedLogsIfPresent(out.logs, "launcher.jsonl", "bin/logs/launcher.jsonl",
                            launcherJsonl, backups);
    AddRotatedLogsIfPresent(out.logs, "launcher.log", "bin/logs/launcher.log",
                            xcat::log::paths::TextLog(launcherJsonl), backups);
    // 过渡期：旧主进程曾写 app.jsonl；现已归一到 launcher.jsonl。
    AddRotatedLogsIfPresent(out.logs, "app.jsonl", "bin/logs/app.jsonl",
                            xcat::JoinBinPath(req.exeBinDir.c_str(), "logs\\app.jsonl"), backups);
    AddLogIfPresent(out.logs, "msc_launcher.log", "bin/launcher.log",
                    xcat::JoinBinPath(req.exeBinDir.c_str(), "launcher.log"));
    AddRotatedLogsIfPresent(out.logs, "inject.jsonl", "bin/logs/inject.jsonl", injectJsonl,
                            backups);
    AddRotatedLogsIfPresent(out.logs, "inject.log", "bin/logs/inject.log",
                            xcat::log::paths::TextLog(injectJsonl), backups);
    AddRotatedLogsIfPresent(out.logs, "overlay_host.jsonl", "XCat_data/state/overlay_host.jsonl",
                            overlayJsonl, backups);
    AddRotatedLogsIfPresent(out.logs, "overlay_host.log", "XCat_data/state/overlay_host.log",
                            xcat::log::paths::TextLog(overlayJsonl), backups);

    // 更新器脚本日志 / 失败通知在 %TEMP%（及 state），不随旧安装目录删除——热更失败采证关键。
    {
        wchar_t tempDir[MAX_PATH]{};
        const DWORD n = GetTempPathW(MAX_PATH, tempDir);
        if (n > 0 && n < MAX_PATH) {
            const std::wstring tempRoot(tempDir);
            AddLogIfPresent(out.logs, "update_apply.log", "TEMP/xcat_update_apply.log",
                            xcat::WideToUtf8(tempRoot + L"xcat_update_apply.log"));
            AddLogIfPresent(out.logs, "update_failed.notify", "TEMP/xcat_update_failed.notify",
                            xcat::WideToUtf8(tempRoot + L"xcat_update_failed.notify"));
        }
        AddLogIfPresent(out.logs, "update_failed_state.notify",
                        "XCat_data/state/update_failed.notify",
                        xcat::JoinBinPath(req.payloadBinDir.c_str(), "state\\update_failed.notify"));
    }

    // 测谎作答统计（角色 × 题型累计）：几百字节，随包带上才看得到客户端战绩。
    AddLogIfPresent(out.logs, "lie_stats.tsv", "XCat_data/state/lie_stats.tsv",
                    xcat::JoinBinPath(req.payloadBinDir.c_str(), "state\\lie_stats.tsv"));

    // 功能频道日志：combat / foothold / petloot / invuln / auto_enter …（白名单未列的一律扫入）。
    AddFeatureChannelLogs(out.logs, req.payloadBinDir.c_str(), backups);

    // 上一版本遗留日志：更新器删旧目录前拷入 XCat_data\logs\prev（旧目录已不存在）。
    {
        namespace fs = std::filesystem;
        const std::string prevDirUtf8 = xcat::JoinBinPath(req.payloadBinDir.c_str(), "logs\\prev");
        std::error_code ec;
        const fs::path prevDir(xcat::Utf8ToWide(prevDirUtf8));
        if (fs::is_directory(prevDir, ec)) {
            for (fs::directory_iterator it(prevDir, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec) || ec) continue;
                const std::string leaf = xcat::WideToUtf8(it->path().filename().wstring());
                if (leaf.empty()) continue;
                // name 不含斜杠（会进 PUT URL）；source 保留层级供服务端归档。
                AddLogIfPresent(out.logs, "prev_" + leaf, "XCat_data/logs/prev/" + leaf,
                                xcat::WideToUtf8(it->path().wstring()));
            }
        }
    }

    out.lieEvents = AddLieEventsIfPresent(out.logs, req.payloadBinDir);
    return out;
}

std::string MachineName() {
    char buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = sizeof(buf);
    if (!GetComputerNameA(buf, &len) || len == 0) return {};
    return std::string(buf, len);
}

std::vector<std::string> CollectLocalMacs() {
    std::vector<std::string> out;
    ULONG size = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size);
    if (rc != ERROR_BUFFER_OVERFLOW || size == 0) return out;

    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);
    if (rc != ERROR_SUCCESS) return out;

    auto looksVirtual = [](const wchar_t* desc, const wchar_t* friendly) {
        auto has = [](const wchar_t* s, const wchar_t* needle) {
            if (!s || !needle) return false;
            std::wstring hay(s);
            for (auto& c : hay) c = static_cast<wchar_t>(towlower(c));
            std::wstring n(needle);
            return hay.find(n) != std::wstring::npos;
        };
        return has(desc, L"virtual") || has(desc, L"vmware") || has(desc, L"hyper-v") ||
               has(desc, L"virtualbox") || has(desc, L"vpn") || has(desc, L"tap-") ||
               has(desc, L"loopback") || has(friendly, L"virtual") || has(friendly, L"vpn");
    };

    for (auto* a = addrs; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp && a->OperStatus != IfOperStatusDormant) continue;
        if (a->PhysicalAddressLength != 6) continue;
        if (looksVirtual(a->Description, a->FriendlyName)) continue;
        char mac[24]{};
        std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", a->PhysicalAddress[0],
                      a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3],
                      a->PhysicalAddress[4], a->PhysicalAddress[5]);
        // 全 0 / 全 f 丢弃
        if (std::strcmp(mac, "00:00:00:00:00:00") == 0 ||
            std::strcmp(mac, "ff:ff:ff:ff:ff:ff") == 0) {
            continue;
        }
        if (std::find(out.begin(), out.end(), mac) != out.end()) continue;
        // 有线/无线优先：插到前面
        const bool preferred = a->IfType == IF_TYPE_ETHERNET_CSMACD ||
                               a->IfType == IF_TYPE_IEEE80211;
        if (preferred) out.insert(out.begin(), mac);
        else out.push_back(mac);
        if (out.size() >= 8) break;
    }
    return out;
}

bool IsValidDeviceId(const std::string& id) {
    // 32 hex 或 8-4-4-4-12 UUID；多 VM 同计算机名时靠此区分。
    if (id.size() == 32) {
        return std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
    }
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(c)) {
            return false;
        }
    }
    return true;
}

std::string NewDeviceId() {
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const uint64_t mix = static_cast<uint64_t>(qpc.QuadPart) ^
                             (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                             GetTickCount64();
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<unsigned char>((mix >> ((i % 8) * 8)) ^ (0xA5u + i * 17u));
        }
    }
    // RFC 4122 variant bits for UUID v4 shape (readable + stable).
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    char buf[40]{};
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

// 与门禁粘性同目录（无 XCat 字样）。按安装路径分文件，避免同机多目录互抢；
// 另保留 legacy id.dat 作迁移兜底（清目录后同路径重装靠 id_<hash>）。
std::string DeviceIdMachineDir() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &base)) ||
        !base) {
        return {};
    }
    std::wstring w = base;
    CoTaskMemFree(base);
    if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w.push_back(L'\\');
    w += L"{E4B7C2A9-1F8D-4E3A-9C6B-7A2D5F1E0C8B}";
    return xcat::WideToUtf8(w);
}

std::string NormalizeInstallKey(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return {};
    std::error_code ec;
    const std::wstring wide = xcat::Utf8ToWide(payloadBinDir);
    std::filesystem::path p(wide);
    std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
    if (ec || abs.empty()) abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    std::string s = xcat::WideToUtf8(abs.wstring());
    for (char& c : s) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

uint32_t HashInstallKey(const std::string& key) {
    // FNV-1a 32-bit：短、稳定，仅用于文件名分片。
    uint32_t h = 2166136261u;
    for (unsigned char c : key) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::string DeviceIdMachinePathLegacy() {
    const std::string dir = DeviceIdMachineDir();
    if (dir.empty()) return {};
    return dir + "\\id.dat";
}

std::string DeviceIdMachinePathForInstall(const char* payloadBinDir) {
    const std::string dir = DeviceIdMachineDir();
    if (dir.empty()) return {};
    const std::string key = NormalizeInstallKey(payloadBinDir);
    if (key.empty()) return DeviceIdMachinePathLegacy();
    char name[32]{};
    std::snprintf(name, sizeof(name), "\\id_%08x.dat", HashInstallKey(key));
    return dir + name;
}

bool ReadDeviceIdFile(const std::string& path, std::string& out) {
    out.clear();
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char buf[80]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    std::string id(buf, n);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ' ||
                           id.back() == '\t' || id.back() == '\0')) {
        id.pop_back();
    }
    size_t start = 0;
    while (start < id.size() &&
           (id[start] == ' ' || id[start] == '\t' || id[start] == '\r' || id[start] == '\n')) {
        ++start;
    }
    if (start) id.erase(0, start);
    if (!IsValidDeviceId(id)) return false;
    out = std::move(id);
    return true;
}

bool WriteDeviceIdFile(const std::string& path, const std::string& id) {
    if (path.empty() || !IsValidDeviceId(id)) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(xcat::Utf8ToWide(path)).parent_path(),
                                        ec);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const std::string body = id + "\n";
    const size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return n == body.size();
}

bool PersistDeviceIdToUserIni(const char* payloadBinDir, const std::string& id) {
    if (!payloadBinDir || !payloadBinDir[0] || !IsValidDeviceId(id)) return false;
    const std::string path = xcat::UserConfigIniPath(payloadBinDir);
    return xcat::UpdateIniFile(path.c_str(), [&](xcat::IniStore& store) {
        xcat::IniSetU32(store, "meta", "version",
                       static_cast<uint32_t>(xcat::kUserConfigIniVersion));
        uint32_t version = 0;
        if (!xcat::IniGetU32(store, "log_upload", "version", version) || version == 0) {
            xcat::IniSetU32(store, "log_upload", "version", kLogUploadIniVersion);
        }
        xcat::IniSetString(store, "log_upload", "deviceId", id.c_str());
    });
}

bool PersistDeviceIdMirrors(const char* payloadBinDir, const std::string& id) {
    bool any = false;
    const std::string scoped = DeviceIdMachinePathForInstall(payloadBinDir);
    if (WriteDeviceIdFile(scoped, id)) any = true;
    // legacy：仅作「无路径/旧版」兜底，不作为多目录共享真源。
    if (payloadBinDir && payloadBinDir[0]) {
        (void)WriteDeviceIdFile(DeviceIdMachinePathLegacy(), id);
    }
    return any;
}

bool ReadDeviceIdMirror(const char* payloadBinDir, std::string& out) {
    out.clear();
    if (ReadDeviceIdFile(DeviceIdMachinePathForInstall(payloadBinDir), out)) return true;
    // 迁移：旧版单文件 id.dat → 读到后由调用方写回路径分片。
    return ReadDeviceIdFile(DeviceIdMachinePathLegacy(), out);
}

std::string EnsureDeviceId(const char* payloadBinDir) {
    // 整段加锁：进程缓存 + 双写原子，避免两线程首调各 mint 一次。
    static std::mutex mtx;
    static std::unordered_map<std::string, std::string> sessionCache;
    std::lock_guard<std::mutex> lk(mtx);

    const std::string cacheKey =
        (payloadBinDir && payloadBinDir[0]) ? std::string(payloadBinDir) : std::string("{}");
    {
        const auto it = sessionCache.find(cacheKey);
        if (it != sessionCache.end() && IsValidDeviceId(it->second)) return it->second;
    }

    auto remember = [&](std::string id) -> std::string {
        if (!IsValidDeviceId(id)) id = NewDeviceId();
        sessionCache[cacheKey] = id;
        return id;
    };

    std::string mirrorId;
    const bool haveMirror = ReadDeviceIdMirror(payloadBinDir, mirrorId);

    if (!payloadBinDir || !payloadBinDir[0]) {
        if (haveMirror) return remember(std::move(mirrorId));
        const std::string id = NewDeviceId();
        (void)WriteDeviceIdFile(DeviceIdMachinePathLegacy(), id);
        return remember(id);
    }

    if (!EnsureStateDir(payloadBinDir)) {
        if (haveMirror) {
            (void)PersistDeviceIdMirrors(payloadBinDir, mirrorId);
            return remember(std::move(mirrorId));
        }
        const std::string id = NewDeviceId();
        (void)PersistDeviceIdMirrors(payloadBinDir, id);
        xcat::log::Warn("LogUpload", "state dir unavailable; deviceId machine-only");
        return remember(id);
    }

    const std::string iniPath = xcat::UserConfigIniPath(payloadBinDir);
    xcat::IniStore ini{};
    const bool iniLoaded = xcat::LoadIniFile(iniPath.c_str(), ini);
    const bool iniExists =
        GetFileAttributesA(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES;

    if (iniLoaded) {
        std::string id;
        if (xcat::IniGetString(ini, "log_upload", "deviceId", id) && IsValidDeviceId(id)) {
            (void)PersistDeviceIdMirrors(payloadBinDir, id);
            return remember(std::move(id));
        }
    }

    if (haveMirror) {
        if (!iniExists || iniLoaded) {
            if (!PersistDeviceIdToUserIni(payloadBinDir, mirrorId)) {
                xcat::log::Warn("LogUpload", "heal user.ini deviceId failed path=%s",
                                iniPath.c_str());
            }
        } else {
            xcat::log::Warn("LogUpload",
                            "user.ini unreadable; reusing machine deviceId (not rewriting ini)");
        }
        // 旧 id.dat 迁到路径分片，避免继续被其他安装目录覆盖。
        (void)PersistDeviceIdMirrors(payloadBinDir, mirrorId);
        return remember(std::move(mirrorId));
    }

    if (iniExists && !iniLoaded) {
        const std::string id = NewDeviceId();
        if (!PersistDeviceIdMirrors(payloadBinDir, id)) {
            xcat::log::Warn("LogUpload", "persist machine deviceId failed while ini unreadable");
        }
        xcat::log::Warn("LogUpload",
                        "user.ini unreadable and no machine mirror; session-stable deviceId issued");
        return remember(id);
    }

    const std::string id = NewDeviceId();
    if (!PersistDeviceIdToUserIni(payloadBinDir, id)) {
        xcat::log::Warn("LogUpload", "persist deviceId failed path=%s", iniPath.c_str());
    }
    if (!PersistDeviceIdMirrors(payloadBinDir, id)) {
        xcat::log::Warn("LogUpload", "persist machine deviceId failed");
    }
    return remember(id);
}

std::string ClientId(const std::string& machine, const std::string& deviceId) {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const std::string shortId =
        deviceId.size() >= 8 ? deviceId.substr(0, 8) : (deviceId.empty() ? "noid" : deviceId);
    char buf[192]{};
    snprintf(buf, sizeof(buf), "%s-%s-%lu-%llx", machine.empty() ? "client" : machine.c_str(),
             shortId.c_str(), GetCurrentProcessId(),
             static_cast<unsigned long long>(qpc.QuadPart));
    return buf;
}

std::string BuildUploadJson(const LogUploadRequest& req, const std::vector<LogBlob>& logs) {
    const std::string machine = MachineName();
    const std::string deviceId = EnsureDeviceId(req.payloadBinDir.c_str());
    const std::string client = ClientId(machine, deviceId);

    std::string body;
    body.reserve(4096 + logs.size() * kMaxBytesPerLog);
    body += "{\"version\":1";
    body += ",\"profile\":\"" + JsonEscape(req.profileId) + "\"";
    body += ",\"clientId\":\"" + JsonEscape(client) + "\"";
    body += ",\"machine\":\"" + JsonEscape(machine) + "\"";
    body += ",\"deviceId\":\"" + JsonEscape(deviceId) + "\"";
    body += ",\"appVersion\":\"" + JsonEscape(AppVersionLabel()) + "\"";
    body += ",\"note\":\"" + JsonEscape(req.note) + "\"";
    body += ",\"uploadMode\":\"" + JsonEscape(LogUploadModeLabel(req.mode)) + "\"";
    body += ",\"logs\":[";
    for (size_t i = 0; i < logs.size(); ++i) {
        const LogBlob& log = logs[i];
        if (i) body += ',';
        body += "{\"name\":\"" + JsonEscape(log.name) + "\"";
        body += ",\"source\":\"" + JsonEscape(log.source) + "\"";
        body += ",\"size\":" + std::to_string(log.size);
        body += ",\"truncated\":";
        body += log.truncated ? "true" : "false";
        body += ",\"base64\":\"" + Base64Encode(log.bytes) + "\"}";
    }
    body += "]}";
    return body;
}

ClientHostIdentity ResolveClientHostIdentityImpl(const std::string& payloadBinDir);

std::wstring SanitizeUploadHdr(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t ch : in) {
        if (ch == L'\r' || ch == L'\n' || ch == 0) continue;
        out.push_back(ch);
    }
    return out;
}

// 让 /v1/logs* 门禁能认设备（OPS 拉取封禁机日志时靠此匹配 pending）。
std::wstring BuildLogUploadIdentityHeaders(const std::string& payloadBinDir) {
    const ClientHostIdentity id = ResolveClientHostIdentityImpl(payloadBinDir);
    const std::string token = LoadOpsToken(payloadBinDir);
    std::string macJoined;
    for (size_t i = 0; i < id.macs.size(); ++i) {
        if (i) macJoined += ',';
        macJoined += id.macs[i];
        if (macJoined.size() > 180) break;
    }
    wchar_t buf[768]{};
    _snwprintf(buf, 768,
               L"X-XCat-Machine: %s\r\nX-XCat-Device-Id: %s\r\n"
               L"X-XCat-Mac: %s\r\nX-XCat-Token: %s\r\n",
               SanitizeUploadHdr(xcat::Utf8ToWide(id.machine)).c_str(),
               SanitizeUploadHdr(xcat::Utf8ToWide(id.deviceId)).c_str(),
               SanitizeUploadHdr(xcat::Utf8ToWide(macJoined)).c_str(),
               SanitizeUploadHdr(xcat::Utf8ToWide(token)).c_str());
    return buf;
}

HttpResult HttpExchangeOnce(const ParsedUrl& base, const std::wstring& path, const wchar_t* method,
                            const void* body, DWORD bodyLen, const std::wstring& headers,
                            DWORD accessType, const char* mode) {
    HttpResult result;
    HINTERNET session =
        WinHttpOpen(L"XCat-LogUpload/2.0", accessType, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.err = WinHttpFailure("WinHttpOpen", GetLastError(), mode);
        return result;
    }
    WinHttpSetTimeouts(session, 15000, 15000, kUploadTimeoutMs, kUploadTimeoutMs);

    HINTERNET conn = WinHttpConnect(session, base.host.c_str(), base.port, 0);
    if (!conn) {
        result.err = WinHttpFailure("WinHttpConnect", GetLastError(), mode);
        WinHttpCloseHandle(session);
        return result;
    }

    const DWORD flags = base.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request =
        WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        result.err = WinHttpFailure("WinHttpOpenRequest", GetLastError(), mode);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return result;
    }

    if (!WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0,
                            bodyLen, 0)) {
        result.err = WinHttpFailure("WinHttpSendRequest", GetLastError(), mode);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return result;
    }

    if (body && bodyLen > 0) {
        const char* bytes = static_cast<const char*>(body);
        DWORD offset = 0;
        while (offset < bodyLen) {
            const DWORD remain = bodyLen - offset;
            const DWORD chunk = remain > 65536u ? 65536u : remain;
            DWORD written = 0;
            if (!WinHttpWriteData(request, bytes + offset, chunk, &written) || written != chunk) {
                result.err = WinHttpFailure("WinHttpWriteData", GetLastError(), mode);
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(conn);
                WinHttpCloseHandle(session);
                return result;
            }
            offset += written;
        }
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        result.err = WinHttpFailure("WinHttpReceiveResponse", GetLastError(), mode);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    result.status = status;

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        std::string buf(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read)) {
            result.err = WinHttpFailure("WinHttpReadData", GetLastError(), mode);
            break;
        }
        if (read == 0) break;
        buf.resize(read);
        result.body += buf;
        if (result.body.size() > 64 * 1024) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

HttpResult HttpExchange(const ParsedUrl& base, const std::wstring& path, const wchar_t* method,
                        const void* body, DWORD bodyLen, const std::wstring& headers) {
    HttpResult direct = HttpExchangeOnce(base, path, method, body, bodyLen, headers,
                                         WINHTTP_ACCESS_TYPE_NO_PROXY, "direct");
    if (direct.err.empty()) return direct;
    xcat::log::Warn("LogUpload", "request failed err=%s; retry default-proxy", direct.err.c_str());
    HttpResult fallback = HttpExchangeOnce(base, path, method, body, bodyLen, headers,
                                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, "default-proxy");
    if (fallback.err.empty()) return fallback;
    xcat::log::Warn("LogUpload", "request failed err=%s; retry auto-proxy", fallback.err.c_str());
    return HttpExchangeOnce(base, path, method, body, bodyLen, headers,
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, "auto-proxy");
}

HttpResult PostJson(const ParsedUrl& url, const std::string& body, const std::string& payloadBinDir) {
    const std::wstring headers =
        L"Content-Type: application/json\r\n"
        L"Accept: application/json\r\n"
        L"Connection: close\r\n" +
        BuildLogUploadIdentityHeaders(payloadBinDir);
    return HttpExchange(url, url.path, L"POST", body.data(), static_cast<DWORD>(body.size()), headers);
}

std::wstring JoinServicePath(const ParsedUrl& url, const wchar_t* suffix) {
    std::wstring out = url.root;
    if (!out.empty() && out.back() == L'/') out.pop_back();
    out += suffix;
    return out;
}

std::string ExtractJsonString(const std::string& json, const char* key) {
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
            switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += esc; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

/** 解析 JSON 非负整数字段（无引号）；失败返回 0。 */
size_t ExtractJsonU64(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    size_t i = pos + needle.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return 0;
    unsigned long long v = 0;
    while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) {
        v = v * 10ull + static_cast<unsigned long long>(json[i] - '0');
        if (v > static_cast<unsigned long long>(SIZE_MAX)) return SIZE_MAX;
        ++i;
    }
    return static_cast<size_t>(v);
}

/** 轮转序号：当前卷=0；`name.N`→N；解析失败当很大（优先丢掉）。 */
unsigned long LogRotationIndex(const std::string& name) {
    const size_t lastDot = name.rfind('.');
    if (lastDot == std::string::npos || lastDot + 1 >= name.size()) return 0;
    for (size_t i = lastDot + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return 0;
    }
    // 基名需像 .log / .jsonl（避免把 x.jsonl 的「jsonl」当序号——上面已要求后缀全数字）
    const std::string base = name.substr(0, lastDot);
    if (base.size() >= 4) {
        const std::string_view b(base);
        auto ends = [](std::string_view s, std::string_view suf) {
            return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
        };
        if (ends(b, ".log") || ends(b, ".jsonl") || ends(b, ".tsv") || ends(b, ".txt")) {
            try {
                return std::stoul(name.substr(lastDot + 1));
            } catch (...) {
                return ULONG_MAX;
            }
        }
    }
    return 0;
}

bool IsUploadKeepFirst(const std::string& name) {
    if (name.empty()) return false;
    if (name == "lie_events.zip" || name.rfind("lie_events", 0) == 0) return true;
    if (name.rfind("freeze_incident", 0) == 0) return true;
    return false;
}

/**
 * 会话文件上限裁剪：优先保留 lie/freeze 与当前卷，再按轮转序号从小到大（新→旧）。
 * 全量模式可收 360×多频道，超过服务端 maxFiles 时否则 PUT 400 整单失败。
 */
void TrimLogsToMaxFiles(std::vector<LogBlob>& logs, size_t maxFiles) {
    if (maxFiles == 0 || logs.size() <= maxFiles) return;

    std::vector<size_t> order(logs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const LogBlob& A = logs[a];
        const LogBlob& B = logs[b];
        const bool keepA = IsUploadKeepFirst(A.name);
        const bool keepB = IsUploadKeepFirst(B.name);
        if (keepA != keepB) return keepA;
        const unsigned long ra = LogRotationIndex(A.name);
        const unsigned long rb = LogRotationIndex(B.name);
        if (ra != rb) return ra < rb;
        return a < b;
    });

    std::vector<LogBlob> kept;
    kept.reserve(maxFiles);
    for (size_t i = 0; i < maxFiles; ++i) {
        kept.push_back(std::move(logs[order[i]]));
    }
    logs.swap(kept);
}

void SetSnapshot(LogUploadPhase phase, std::string message, std::string uploadId = {},
                 uint32_t httpStatus = 0) {
    if (phase == LogUploadPhase::Failed) {
        // 上屏脱敏：WinHTTP/URL 原文只留本地日志。
        const bool leak = message.find("http://") != std::string::npos ||
                          message.find("https://") != std::string::npos ||
                          message.find("://") != std::string::npos ||
                          message.find("xcat.work") != std::string::npos ||
                          message.find("127.0.0.1") != std::string::npos ||
                          message.find("localhost") != std::string::npos ||
                          message.find("WinHttp") != std::string::npos;
        if (leak || message.empty()) {
            message = httpStatus != 0
                          ? (std::string("上传失败 HTTP ") + std::to_string(httpStatus))
                          : "上传失败（详情见本地日志）";
        }
    }
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.snapshot.phase = phase;
    g_state.snapshot.message = std::move(message);
    g_state.snapshot.uploadId = std::move(uploadId);
    g_state.snapshot.httpStatus = httpStatus;
}

std::string NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

void AddHistoryEntry(const std::string& uploadId, const std::string& message, uint32_t httpStatus,
                     size_t files) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    LogUploadHistoryEntry e{};
    e.timeText = NowText();
    e.uploadId = uploadId;
    e.message = message;
    e.httpStatus = httpStatus;
    e.files = static_cast<uint32_t>(std::min<size_t>(files, UINT32_MAX));
    g_state.history.push_back(std::move(e));
    if (g_state.history.size() > 32) {
        g_state.history.erase(g_state.history.begin(),
                              g_state.history.begin() + static_cast<ptrdiff_t>(g_state.history.size() - 32));
    }
}

bool UploadViaSession(const ParsedUrl& url, const LogUploadRequest& req,
                      std::vector<LogBlob>& logs, std::string& outUploadId, std::string& outErr,
                      DWORD& outStatus) {
    outUploadId.clear();
    outErr.clear();
    outStatus = 0;

    SetSnapshot(LogUploadPhase::Uploading, "上传中（创建会话）...");
    const std::wstring sessionPath = JoinServicePath(url, L"/v1/logs/sessions");
    const std::wstring identityHeaders = BuildLogUploadIdentityHeaders(req.payloadBinDir);
    const std::wstring jsonHeaders =
        L"Content-Type: application/json\r\n"
        L"Accept: application/json\r\n"
        L"Connection: close\r\n" +
        identityHeaders;
    HttpResult created = HttpExchange(url, sessionPath, L"POST", "{}", 2, jsonHeaders);
    outStatus = created.status;
    if (!created.err.empty()) {
        outErr = created.err;
        return false;
    }
    if (created.status == 404) {
        outErr = "session-api-unavailable";
        return false;
    }
    if (created.status < 200 || created.status >= 300) {
        char buf[128]{};
        snprintf(buf, sizeof(buf), "创建会话失败 HTTP %lu",
                 static_cast<unsigned long>(created.status));
        outErr = buf;
        return false;
    }

    const std::string sessionId = ExtractJsonString(created.body, "sessionId");
    if (sessionId.empty()) {
        outErr = "服务未返回 sessionId";
        return false;
    }

    size_t maxFiles = ExtractJsonU64(created.body, "maxFiles");
    if (maxFiles == 0) maxFiles = kFallbackSessionMaxFiles;
    const size_t collectedN = logs.size();
    TrimLogsToMaxFiles(logs, maxFiles);
    if (logs.size() < collectedN) {
        xcat::log::Warn("LogUpload",
                        "session file cap: upload %zu/%zu files (maxFiles=%zu mode=%s)",
                        logs.size(), collectedN, maxFiles, LogUploadModeLabel(req.mode));
    }
    if (logs.empty()) {
        outErr = "会话文件上限过低，无可上传日志";
        return false;
    }

    for (size_t i = 0; i < logs.size(); ++i) {
        const LogBlob& log = logs[i];
        char progress[128]{};
        snprintf(progress, sizeof(progress), "上传中（%zu/%zu）%s", i + 1, logs.size(),
                 log.name.c_str());
        SetSnapshot(LogUploadPhase::Uploading, progress);

        std::wstring filePath = JoinServicePath(url, L"/v1/logs/sessions/");
        filePath += xcat::Utf8ToWide(sessionId);
        filePath += L"/files/";
        filePath += xcat::Utf8ToWide(log.name);

        std::wstring headers =
            L"Content-Type: application/octet-stream\r\n"
            L"Accept: application/json\r\n"
            L"Connection: close\r\n"
            L"X-XCat-Source: ";
        headers += xcat::Utf8ToWide(log.source);
        headers += L"\r\nX-XCat-Original-Size: ";
        headers += std::to_wstring(log.size);
        headers += L"\r\nX-XCat-Truncated: ";
        headers += log.truncated ? L"1\r\n" : L"0\r\n";
        headers += identityHeaders;

        HttpResult put = HttpExchange(url, filePath, L"PUT", log.bytes.data(),
                                      static_cast<DWORD>(log.bytes.size()), headers);
        outStatus = put.status;
        if (!put.err.empty()) {
            outErr = put.err;
            return false;
        }
        if (put.status < 200 || put.status >= 300) {
            char buf[160]{};
            snprintf(buf, sizeof(buf), "上传 %s 失败 HTTP %lu", log.name.c_str(),
                     static_cast<unsigned long>(put.status));
            outErr = buf;
            return false;
        }
    }

    SetSnapshot(LogUploadPhase::Uploading, "上传中（提交）...");
    const std::string machine = MachineName();
    const std::string deviceId = EnsureDeviceId(req.payloadBinDir.c_str());
    const std::string client = ClientId(machine, deviceId);
    std::string commitBody = "{\"version\":2";
    commitBody += ",\"profile\":\"" + JsonEscape(req.profileId) + "\"";
    commitBody += ",\"clientId\":\"" + JsonEscape(client) + "\"";
    commitBody += ",\"machine\":\"" + JsonEscape(machine) + "\"";
    commitBody += ",\"deviceId\":\"" + JsonEscape(deviceId) + "\"";
    commitBody += ",\"appVersion\":\"" + JsonEscape(AppVersionLabel()) + "\"";
    commitBody += ",\"note\":\"" + JsonEscape(req.note) + "\"";
    commitBody += ",\"uploadMode\":\"" + JsonEscape(LogUploadModeLabel(req.mode)) + "\"}";

    std::wstring commitPath = JoinServicePath(url, L"/v1/logs/sessions/");
    commitPath += xcat::Utf8ToWide(sessionId);
    commitPath += L"/commit";
    HttpResult committed = HttpExchange(url, commitPath, L"POST", commitBody.data(),
                                        static_cast<DWORD>(commitBody.size()), jsonHeaders);
    outStatus = committed.status;
    if (!committed.err.empty()) {
        outErr = committed.err;
        return false;
    }
    if (committed.status < 200 || committed.status >= 300) {
        char buf[128]{};
        snprintf(buf, sizeof(buf), "提交失败 HTTP %lu",
                 static_cast<unsigned long>(committed.status));
        outErr = buf;
        return false;
    }
    outUploadId = ExtractJsonString(committed.body, "uploadId");
    return true;
}

void UploadWorker(LogUploadRequest req) {
    req.note = NormalizeUploadNote(req.note);
    if (!LogUploadConfigured(req)) {
        SetSnapshot(LogUploadPhase::Failed, "上报服务未就绪");
        return;
    }

    ParsedUrl url{};
    if (!ParseUrl(req.url, url)) {
        xcat::log::Warn("LogUpload", "parse url failed url=%s", req.url.c_str());
        SetSnapshot(LogUploadPhase::Failed, "上报服务配置无效");
        return;
    }

    const CollectedLogs collected = CollectLogs(req);
    std::vector<LogBlob> logs = std::move(collected.logs);
    const LieEventsAttach lieEvents = collected.lieEvents;
    if (logs.empty()) {
        SetSnapshot(LogUploadPhase::Failed, "未找到可上传的日志文件");
        return;
    }

    size_t totalBytes = 0;
    for (const auto& log : logs) totalBytes += log.bytes.size();
    xcat::log::Info("LogUpload",
                    "upload begin protocol=session-v2 mode=%s files=%zu bytes=%zu lie=%d url=%s",
                    LogUploadModeLabel(req.mode), logs.size(), totalBytes,
                    static_cast<int>(lieEvents), req.url.c_str());

    auto successMessage = [&](const std::string& id) -> std::string {
        std::string msg = id.empty() ? "上传成功" : ("上传成功: " + id);
        if (lieEvents == LieEventsAttach::Failed) {
            msg += "（lie_events 打包失败，未附带）";
        }
        return msg;
    };

    std::string uploadId;
    std::string err;
    DWORD status = 0;
    if (UploadViaSession(url, req, logs, uploadId, err, status)) {
        const std::string msg = successMessage(uploadId);
        xcat::log::Info("LogUpload", "upload ok protocol=session-v2 mode=%s id=%s files=%zu lie=%d",
                        LogUploadModeLabel(req.mode), uploadId.c_str(), logs.size(),
                        static_cast<int>(lieEvents));
        SetSnapshot(LogUploadPhase::Succeeded, msg, uploadId, status);
        AddHistoryEntry(uploadId, msg, status, logs.size());
        return;
    }

    if (err == "session-api-unavailable") {
        xcat::log::Warn("LogUpload", "session api missing; fallback legacy json");
        SetSnapshot(LogUploadPhase::Uploading, "上传中（兼容模式）...");
        const std::string body = BuildUploadJson(req, logs);
        const HttpResult resp = PostJson(url, body, req.payloadBinDir);
        if (!resp.err.empty()) {
            xcat::log::Warn("LogUpload", "legacy upload failed err=%s", resp.err.c_str());
            SetSnapshot(LogUploadPhase::Failed, resp.err, {}, resp.status);
            return;
        }
        uploadId = ExtractJsonString(resp.body, "uploadId");
        if (resp.status >= 200 && resp.status < 300) {
            const std::string msg = successMessage(uploadId);
            SetSnapshot(LogUploadPhase::Succeeded, msg, uploadId, resp.status);
            AddHistoryEntry(uploadId, msg, resp.status, logs.size());
            return;
        }
        char msg[256]{};
        snprintf(msg, sizeof(msg), "上传失败 HTTP %lu", static_cast<unsigned long>(resp.status));
        SetSnapshot(LogUploadPhase::Failed, msg, {}, resp.status);
        return;
    }

    xcat::log::Warn("LogUpload", "upload failed: %s", err.c_str());
    SetSnapshot(LogUploadPhase::Failed, err, {}, status);
}

ClientHostIdentity ResolveClientHostIdentityImpl(const std::string& payloadBinDir) {
    ClientHostIdentity out;
    out.machine = MachineName();
    out.deviceId = EnsureDeviceId(payloadBinDir.c_str());
    out.macs = CollectLocalMacs();
    return out;
}

}  // namespace

ClientHostIdentity ResolveClientHostIdentity(const std::string& payloadBinDir) {
    ClientHostIdentity out = ResolveClientHostIdentityImpl(payloadBinDir);
    out.token = LoadOpsToken(payloadBinDir);
    return out;
}

std::string NormalizeOpsToken(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7f) continue;
        if (c == ' ' || c == '\t') continue;
        // ASCII 大小写归一，与服务端 tok: 匹配一致。
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        s.push_back(static_cast<char>(c));
        if (s.size() >= kOpsTokenMaxChars) break;
    }
    return s;
}

// TOKEN 机级镜像：与 deviceId 同 ProgramData 目录；路径分片 + 全机 tok.dat 兜底
//（新路径重装也能找回）。不写注册表，避免 HKCU/提权与手工清理成本。
namespace {
std::string OpsTokenMachinePathLegacy() {
    const std::string dir = DeviceIdMachineDir();
    if (dir.empty()) return {};
    return dir + "\\tok.dat";
}

std::string OpsTokenMachinePathForInstall(const char* payloadBinDir) {
    const std::string dir = DeviceIdMachineDir();
    if (dir.empty()) return {};
    const std::string key = NormalizeInstallKey(payloadBinDir);
    if (key.empty()) return OpsTokenMachinePathLegacy();
    char name[32]{};
    std::snprintf(name, sizeof(name), "\\tok_%08x.dat", HashInstallKey(key));
    return dir + name;
}

bool ReadOpsTokenFile(const std::string& path, std::string& out) {
    out.clear();
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char buf[96]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    std::string raw(buf, n);
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' ' ||
                            raw.back() == '\t' || raw.back() == '\0')) {
        raw.pop_back();
    }
    out = NormalizeOpsToken(raw);
    return !out.empty();
}

bool WriteOpsTokenFile(const std::string& path, const std::string& token) {
    if (path.empty() || token.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(xcat::Utf8ToWide(path)).parent_path(),
                                        ec);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const std::string body = token + "\n";
    const size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return n == body.size();
}

bool DeleteOpsTokenFile(const std::string& path) {
    if (path.empty()) return true;
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    return DeleteFileA(path.c_str()) != 0;
}

bool PersistOpsTokenMirrors(const char* payloadBinDir, const std::string& token) {
    if (token.empty()) return false;
    bool any = false;
    if (WriteOpsTokenFile(OpsTokenMachinePathForInstall(payloadBinDir), token)) any = true;
    // 全机兜底：换目录/重装包后仍能读回。
    if (WriteOpsTokenFile(OpsTokenMachinePathLegacy(), token)) any = true;
    return any;
}

bool ClearOpsTokenMirrors(const char* payloadBinDir, const std::string& onlyIfLegacyEquals) {
    // 只删本安装分片；全机 tok.dat 仅当内容==被清空的 TOKEN 才删，避免同机多目录互抢。
    bool ok = DeleteOpsTokenFile(OpsTokenMachinePathForInstall(payloadBinDir));
    if (!onlyIfLegacyEquals.empty()) {
        std::string legacy;
        if (ReadOpsTokenFile(OpsTokenMachinePathLegacy(), legacy) &&
            legacy == onlyIfLegacyEquals) {
            if (!DeleteOpsTokenFile(OpsTokenMachinePathLegacy())) ok = false;
        }
    }
    return ok;
}

bool ReadOpsTokenMirror(const char* payloadBinDir, std::string& out) {
    out.clear();
    if (ReadOpsTokenFile(OpsTokenMachinePathForInstall(payloadBinDir), out)) return true;
    return ReadOpsTokenFile(OpsTokenMachinePathLegacy(), out);
}

bool PersistOpsTokenToUserIni(const char* payloadBinDir, const std::string& normalized) {
    if (!payloadBinDir || !payloadBinDir[0]) return false;
    const std::string path = xcat::UserConfigIniPath(payloadBinDir);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(xcat::Utf8ToWide(path)).parent_path(),
                                        ec);
    return xcat::UpdateIniFile(path.c_str(), [&](xcat::IniStore& ini) {
        xcat::IniSetU32(ini, "meta", "version", static_cast<uint32_t>(xcat::kUserConfigIniVersion));
        xcat::IniSetU32(ini, "update", "version", 1u);
        xcat::IniSetU64(ini, "update", "writeTickMs", GetTickCount64());
        auto eraseKey = [&](const char* section, const char* key) {
            auto it = ini.find(section);
            if (it != ini.end()) it->second.erase(key);
        };
        if (normalized.empty()) {
            eraseKey("update", "token");
        } else {
            xcat::IniSetString(ini, "update", "token", normalized.c_str());
        }
    });
}
}  // namespace

std::string LoadOpsToken(const std::string& payloadBinDir) {
    // 1) 本安装 user.ini  2) 路径分片镜像  3) 全机 tok.dat
    // 镜像命中且 ini 空 → 回填 ini，避免下次再靠兜底。
    std::string fromIni;
    if (!payloadBinDir.empty()) {
        xcat::IniStore ini{};
        const std::string path = xcat::UserConfigIniPath(payloadBinDir.c_str());
        if (xcat::LoadIniFile(path.c_str(), ini)) {
            std::string value;
            if (xcat::IniGetString(ini, "update", "token", value)) {
                fromIni = NormalizeOpsToken(value);
            }
        }
    }
    if (!fromIni.empty()) {
        (void)PersistOpsTokenMirrors(payloadBinDir.c_str(), fromIni);
        return fromIni;
    }

    std::string fromMirror;
    if (ReadOpsTokenMirror(payloadBinDir.c_str(), fromMirror) && !fromMirror.empty()) {
        if (!payloadBinDir.empty()) {
            if (PersistOpsTokenToUserIni(payloadBinDir.c_str(), fromMirror)) {
                xcat::log::Info("Update", "ops token healed from machine mirror");
            } else {
                xcat::log::Warn("Update", "ops token mirror ok but heal user.ini failed");
            }
            (void)PersistOpsTokenMirrors(payloadBinDir.c_str(), fromMirror);
        }
        return fromMirror;
    }
    return {};
}

bool SaveOpsToken(const std::string& payloadBinDir, std::string_view raw) {
    if (payloadBinDir.empty()) return false;
    const std::string normalized = NormalizeOpsToken(raw);
    if (normalized.empty()) {
        // 清空前记下当前值，供选择性删除全机 tok.dat（仅当内容一致）。
        std::string prev;
        {
            xcat::IniStore ini{};
            const std::string path = xcat::UserConfigIniPath(payloadBinDir.c_str());
            if (xcat::LoadIniFile(path.c_str(), ini)) {
                std::string value;
                if (xcat::IniGetString(ini, "update", "token", value)) {
                    prev = NormalizeOpsToken(value);
                }
            }
        }
        if (prev.empty()) (void)ReadOpsTokenMirror(payloadBinDir.c_str(), prev);
        const bool iniOk = PersistOpsTokenToUserIni(payloadBinDir.c_str(), normalized);
        const bool mirrorOk = ClearOpsTokenMirrors(payloadBinDir.c_str(), prev);
        return iniOk && mirrorOk;
    }
    const bool iniOk = PersistOpsTokenToUserIni(payloadBinDir.c_str(), normalized);
    const bool mirrorOk = PersistOpsTokenMirrors(payloadBinDir.c_str(), normalized);
    // ini 成功即可用；镜像失败只告警（权限/杀软偶发），不挡保存。
    if (!mirrorOk) {
        xcat::log::Warn("Update", "ops token saved to ini but machine mirror failed");
    }
    return iniOk;
}

std::string NormalizeUploadNote(std::string_view raw) {
    std::string s(raw);
    for (char& c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
    }
    std::string collapsed;
    collapsed.reserve(s.size());
    bool prevSpace = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (prevSpace) continue;
            collapsed.push_back(' ');
            prevSpace = true;
            continue;
        }
        collapsed.push_back(c);
        prevSpace = false;
    }
    while (!collapsed.empty() && collapsed.front() == ' ') collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();

    // 按 UTF-8 码点截断，避免切在多字节字符中间。
    size_t i = 0;
    size_t cps = 0;
    while (i < collapsed.size() && cps < kMaxUploadNoteCodePoints) {
        const auto c = static_cast<unsigned char>(collapsed[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            ++i;
            continue;
        }
        if (i + len > collapsed.size()) break;
        i += len;
        ++cps;
    }
    if (i < collapsed.size()) collapsed.resize(i);
    return collapsed;
}

LogUploadPrefs LoadLogUploadPrefs(const std::string& exeBinDir, const LogUploadPrefs& defaults) {
    LogUploadPrefs prefs{};
    prefs.url = RedirectLegacyServiceUrl(defaults.url);
    prefs.mode = defaults.mode;
    if (exeBinDir.empty()) return prefs;

    const std::string payloadBinDir = PayloadBinDirFromExe(exeBinDir);
    std::string iniUrl;
    LogUploadMode iniMode = defaults.mode;
    bool fromIni = false;
    if (ReadLogUploadIni(payloadBinDir.c_str(), iniUrl, &iniMode, nullptr)) {
        prefs.url = iniUrl;
        prefs.mode = iniMode;
        fromIni = true;
    } else {
        std::string legacyUrl;
        if (ReadLegacyLogUploadIni(exeBinDir, legacyUrl)) {
            prefs.url = legacyUrl;
            fromIni = true;
        }
    }

    const std::string redirected = RedirectLegacyServiceUrl(prefs.url);
    if (redirected != prefs.url) {
        xcat::log::Info("LogUpload", "redirect service url %s -> %s", prefs.url.c_str(),
                        redirected.c_str());
        prefs.url = redirected;
        WriteLogUploadIni(payloadBinDir.c_str(), prefs.url, prefs.mode, GetTickCount64());
    } else if (fromIni && !ReadLogUploadIni(payloadBinDir.c_str(), iniUrl, nullptr, nullptr)) {
        // 旧独立 ini 迁移进 user.ini
        WriteLogUploadIni(payloadBinDir.c_str(), prefs.url, prefs.mode, GetTickCount64());
    }
    return prefs;
}

void SaveLogUploadPrefs(const std::string& exeBinDir, const LogUploadPrefs& prefs) {
    if (exeBinDir.empty()) return;
    const std::string payloadBinDir = PayloadBinDirFromExe(exeBinDir);
    WriteLogUploadIni(payloadBinDir.c_str(), RedirectLegacyServiceUrl(prefs.url), prefs.mode,
                      GetTickCount64());
}

bool LogUploadConfigured(const LogUploadRequest& req) {
    return !req.url.empty();
}

bool LogUploadBusy() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    return g_state.snapshot.phase == LogUploadPhase::Uploading;
}

bool StartLogUpload(LogUploadRequest req) {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (g_state.snapshot.phase == LogUploadPhase::Uploading) return false;
        g_state.snapshot.phase = LogUploadPhase::Uploading;
        g_state.snapshot.message = "上传中...";
        g_state.snapshot.uploadId.clear();
        g_state.snapshot.httpStatus = 0;
    }
    std::thread(UploadWorker, std::move(req)).detach();
    return true;
}

LogUploadSnapshot GetLogUploadSnapshot() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    return g_state.snapshot;
}

std::vector<LogUploadHistoryEntry> GetLogUploadHistory() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    return g_state.history;
}

}  // namespace xcat::app
