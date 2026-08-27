#include "chromium_cdp.h"
#include "gamapass_login_phase.h"
#include "http_gamapass_login.h"
#include "msc_launch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace msc::cdp {
namespace {

BOOL CALLBACK WsaInitOnceCb(PINIT_ONCE, PVOID, PVOID*) {
    WSADATA w{};
    WSAStartup(MAKEWORD(2, 2), &w);
    return TRUE;
}

void EnsureWsa() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, WsaInitOnceCb, nullptr, nullptr);
}

// 环回 TCP 短超时。WinHttp 即便 NO_PROXY 在部分机器上仍会空等（SNAILVPC 0.1.188 空罐 about:blank）。
bool TcpConnectLoopbackTo(SOCKET s, const sockaddr* addr, int addrLen, int timeoutMs) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, addr, addrLen);
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int sel = select(0, nullptr, &w, nullptr, &tv);
    int err = 0;
    int elen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &elen);
    if (sel <= 0 || err != 0) return false;
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 800u;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    return true;
}

bool TcpConnectFam(int af, int port, int timeoutMs, SOCKET* outSock) {
    if (outSock) *outSock = INVALID_SOCKET;
    if (port <= 0) return false;
    EnsureWsa();
    if (af == AF_INET) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (TcpConnectLoopbackTo(s, reinterpret_cast<sockaddr*>(&a), sizeof(a), timeoutMs)) {
            if (outSock) *outSock = s;
            else closesocket(s);
            return true;
        }
        closesocket(s);
        return false;
    }
    SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD v6only = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET6, "::1", &a.sin6_addr);
    if (TcpConnectLoopbackTo(s, reinterpret_cast<sockaddr*>(&a), sizeof(a), timeoutMs)) {
        if (outSock) *outSock = s;
        else closesocket(s);
        return true;
    }
    closesocket(s);
    return false;
}

bool TcpConnectLoopback(int port, int timeoutMs, SOCKET* outSock) {
    if (TcpConnectFam(AF_INET, port, timeoutMs, outSock)) return true;
    return TcpConnectFam(AF_INET6, port, timeoutMs, outSock);
}

bool TcpLoopbackOpen(int port, int timeoutMs) {
    SOCKET s = INVALID_SOCKET;
    if (!TcpConnectLoopback(port, timeoutMs, &s)) return false;
    closesocket(s);
    return true;
}

std::string PeekRawForLog(const std::string& raw) {
    std::string o;
    if (raw.empty()) return "(empty)";
    for (size_t i = 0; i < raw.size() && o.size() < 56; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\r')
            o += "\\r";
        else if (c == '\n')
            o += "\\n";
        else if (c >= 32 && c < 127)
            o.push_back(static_cast<char>(c));
        else
            o.push_back('.');
    }
    return o;
}

bool HttpExchangeOnSock(SOCKET s, int af, int port, const char* methodA, const char* pathA,
                        std::string& body, std::string* peek) {
    body.clear();
    char host[80]{};
    if (af == AF_INET)
        sprintf_s(host, "127.0.0.1:%d", port);
    else
        sprintf_s(host, "[::1]:%d", port);
    char req[768]{};
    sprintf_s(req,
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept: */*\r\n\r\n",
              methodA, pathA, host);
    const int reqLen = static_cast<int>(strlen(req));
    DWORD rcvTo = 800;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTo), sizeof(rcvTo));
    if (send(s, req, reqLen, 0) != reqLen) {
        if (peek) *peek = "(send-fail)";
        return false;
    }
    std::string raw;
    const DWORD t0 = GetTickCount();
    char buf[2048];
    for (;;) {
        if (GetTickCount() - t0 > 1500) break;
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            if (raw.size() > 2 * 1024 * 1024) break;
        } else {
            break;
        }
        size_t hdr = raw.find("\r\n\r\n");
        size_t sep = 4;
        if (hdr == std::string::npos) {
            hdr = raw.find("\n\n");
            sep = 2;
        }
        if (hdr == std::string::npos) continue;
        std::string headers = raw.substr(0, hdr);
        for (auto& c : headers)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        int cl = -1;
        const size_t clPos = headers.find("content-length:");
        if (clPos != std::string::npos) {
            cl = 0;
            for (size_t i = clPos + 15; i < headers.size(); ++i) {
                if (headers[i] == '\r' || headers[i] == '\n') break;
                if (headers[i] >= '0' && headers[i] <= '9') cl = cl * 10 + (headers[i] - '0');
            }
        }
        if (cl >= 0 && raw.size() >= hdr + sep + static_cast<size_t>(cl)) break;
    }
    if (peek) *peek = PeekRawForLog(raw);
    size_t hdr = raw.find("\r\n\r\n");
    size_t sep = 4;
    if (hdr == std::string::npos) {
        hdr = raw.find("\n\n");
        sep = 2;
    }
    if (hdr == std::string::npos) return false;
    body = raw.substr(hdr + sep);
    if (raw.find("200") == std::string::npos && raw.find("HTTP") != 0) return !body.empty();
    return !body.empty();
}

bool LoopbackHttp(int port, const wchar_t* method, const wchar_t* path, std::string& body, int* usedAf,
                  std::string* peek, bool* tcp4, bool* tcp6) {
    body.clear();
    if (tcp4) *tcp4 = false;
    if (tcp6) *tcp6 = false;
    if (usedAf) *usedAf = 0;
    if (peek) peek->clear();
    if (!method || !path || port <= 0) return false;
    char methodA[16]{};
    char pathA[256]{};
    WideCharToMultiByte(CP_UTF8, 0, method, -1, methodA, sizeof(methodA), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, sizeof(pathA), nullptr, nullptr);
    const int afs[2] = {AF_INET, AF_INET6};
    std::string lastPeek;
    for (int af : afs) {
        SOCKET s = INVALID_SOCKET;
        const bool tcp = TcpConnectFam(af, port, 400, &s);
        if (af == AF_INET && tcp4) *tcp4 = tcp;
        if (af == AF_INET6 && tcp6) *tcp6 = tcp;
        if (!tcp) continue;
        std::string p;
        std::string b;
        const bool ok = HttpExchangeOnSock(s, af, port, methodA, pathA, b, &p);
        closesocket(s);
        lastPeek = p;
        if (ok) {
            body = std::move(b);
            if (usedAf) *usedAf = af;
            if (peek) *peek = std::move(p);
            return true;
        }
    }
    if (peek) *peek = lastPeek.empty() ? "(no-tcp)" : lastPeek;
    return false;
}

bool ReadDevToolsActivePort(const std::wstring& userData, int* outPort) {
    if (outPort) *outPort = 0;
    if (userData.empty()) return false;
    const std::wstring path = userData + L"\\DevToolsActivePort";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256]{};
    DWORD n = 0;
    const BOOL rd = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    if (!rd || n == 0) return false;
    int port = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9')
            port = port * 10 + (buf[i] - '0');
        else
            break;
    }
    if (port <= 0 || port > 65535) return false;
    if (outPort) *outPort = port;
    return true;
}

void LogLine(const LogFn& log, const std::wstring& s) {
    if (log) log(s);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string JsonGetString(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    while (p + 1 < json.size() && (json[p + 1] == ' ' || json[p + 1] == '\t')) ++p;
    if (p + 1 >= json.size() || json[p + 1] != '"') return {};
    size_t i = p + 2;
    std::string out;
    while (i < json.size()) {
        char c = json[i++];
        if (c == '\\' && i < json.size()) {
            char e = json[i++];
            if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else if (e == '"' || e == '\\' || e == '/') out.push_back(e);
            else if (e == 'u' && i + 3 < json.size()) i += 4;  // skip
            else out.push_back(e);
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& p) {
    if (DirExists(p)) return true;
    return CreateDirectoryW(p.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

void WipeDirFiles(const std::wstring& dir) {
    if (!DirExists(dir)) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void ReplaceAll(std::string& s, const char* from, const char* to) {
    const size_t fl = std::strlen(from);
    const size_t tl = std::strlen(to);
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, fl, to);
        p += tl;
    }
}

// 只改已有 Preferences 的退出标记。禁止从零写残缺 JSON（Chrome 会当崩溃退出再弹恢复条）。
void MarkProfileExitedCleanly(const std::wstring& prefsPath) {
    if (!FileExists(prefsPath)) return;
    HANDLE h = CreateFileW(prefsPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 4 * 1024 * 1024) {
        CloseHandle(h);
        return;
    }
    std::string body(sz, 0);
    DWORD n = 0;
    if (!ReadFile(h, body.data(), sz, &n, nullptr) || n != sz) {
        CloseHandle(h);
        return;
    }
    ReplaceAll(body, "\"exit_type\":\"Crashed\"", "\"exit_type\":\"Normal\"");
    ReplaceAll(body, "\"exit_type\": \"Crashed\"", "\"exit_type\": \"Normal\"");
    ReplaceAll(body, "\"exited_cleanly\":false", "\"exited_cleanly\":true");
    ReplaceAll(body, "\"exited_cleanly\": false", "\"exited_cleanly\": true");
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    DWORD w = 0;
    WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
    SetEndOfFile(h);
    CloseHandle(h);
}

void PrepareIsolatedLaunchUserData(const std::wstring& userData) {
    const std::wstring def = userData + L"\\Default";
    CreateDirectoryW(userData.c_str(), nullptr);
    CreateDirectoryW(def.c_str(), nullptr);
    HANDLE fr = CreateFileW((userData + L"\\First Run").c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fr != INVALID_HANDLE_VALUE) CloseHandle(fr);
    DeleteFileW((def + L"\\Current Session").c_str());
    DeleteFileW((def + L"\\Current Tabs").c_str());
    DeleteFileW((def + L"\\Last Session").c_str());
    DeleteFileW((def + L"\\Last Tabs").c_str());
    WipeDirFiles(def + L"\\Sessions");
    MarkProfileExitedCleanly(def + L"\\Preferences");
}

void CopyFileTo(const std::wstring& src, const std::wstring& dst) {
    if (!FileExists(src) && !DirExists(src)) return;
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
}

// Chrome 136+：默认 User Data 静默忽略调试口 → 官方 Chrome/360 必须走非默认副本目录。
// Edge：直开日常（拷贝后 Cookie 常解不开）。Chrome++：非标准目录，直开。
bool IsStandardChromiumUserData(const std::wstring& userData) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return false;
    const std::wstring chrome = std::wstring(localApp) + L"\\Google\\Chrome\\User Data";
    const std::wstring chrome360x = std::wstring(localApp) + L"\\360ChromeX\\Chrome\\User Data";
    const std::wstring chrome360 = std::wstring(localApp) + L"\\360Chrome\\Chrome\\User Data";
    auto eq = [](std::wstring a, std::wstring b) {
        for (auto& c : a)
            if (c == L'/') c = L'\\';
        for (auto& c : b)
            if (c == L'/') c = L'\\';
        while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    };
    return eq(userData, chrome) || eq(userData, chrome360x) || eq(userData, chrome360);
}

bool IsEdgeUserDataDir(const std::wstring& userData) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return false;
    const std::wstring edge = std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data";
    auto eq = [](std::wstring a, std::wstring b) {
        for (auto& c : a)
            if (c == L'/') c = L'\\';
        for (auto& c : b)
            if (c == L'/') c = L'\\';
        while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    };
    return eq(userData, edge);
}

bool MirrorTreeFile(const std::wstring& src, const std::wstring& dst) {
    if (DirExists(src)) {
        if (!EnsureDir(dst)) return false;
        WIN32_FIND_DATAW fd{};
        const std::wstring pat = src + L"\\*";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return true;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            MirrorTreeFile(src + L"\\" + fd.cFileName, dst + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return true;
    }
    if (FileExists(src)) {
        const size_t slash = dst.find_last_of(L"\\/");
        if (slash != std::wstring::npos) EnsureDir(dst.substr(0, slash));
        return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
    }
    return false;
}

constexpr wchar_t kForceSessionSyncMarker[] = L".xcat_force_session_sync";

bool HasUsableCookies(const std::wstring& profileDef) {
    const std::wstring candidates[] = {
        profileDef + L"\\Network\\Cookies",
        profileDef + L"\\Cookies",
    };
    for (const auto& p : candidates) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(p.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        const ULARGE_INTEGER sz{fd.nFileSizeLow, fd.nFileSizeHigh};
        if (sz.QuadPart > 64) return true;
    }
    return false;
}

bool PeekForceSessionSync(const std::wstring& cdpRoot) {
    return FileExists(cdpRoot + L"\\" + kForceSessionSyncMarker);
}

void ClearForceSessionSync(const std::wstring& cdpRoot) {
    DeleteFileW((cdpRoot + L"\\" + kForceSessionSyncMarker).c_str());
}

// 只读日常 → 写入副本；绝不反向写回。不结束日常 Chrome。
// 重灌仅：空副本 / 强制标记（不用 mtime 自动盖，避免半残日常毁掉可用 SSO）。
bool PrepareCdpSafeUserData(const std::wstring& srcUserData, std::wstring& outCdpData, const LogFn& log) {
    outCdpData.clear();
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return false;
    outCdpData = std::wstring(localApp) + L"\\XCat\\GamaPassCdpProfile";
    if (!EnsureDir(std::wstring(localApp) + L"\\XCat") || !EnsureDir(outCdpData)) return false;

    LogLine(log, L"[cdp] 会话目录：只读源（日常）=" + srcUserData);
    LogLine(log, L"[cdp] 会话目录：写入目标（副本）=" + outCdpData);
    LogLine(log, L"[cdp] 会话目录：不写回日常、不清日常 Cookie/LS、不结束日常浏览器");

    const std::wstring srcDef = srcUserData + L"\\Default";
    const std::wstring dstDef = outCdpData + L"\\Default";
    EnsureDir(dstDef);

    CopyFileTo(srcUserData + L"\\Local State", outCdpData + L"\\Local State");
    const wchar_t* alwaysFiles[] = {L"Preferences", L"Secure Preferences", L"Login Data",
                                    L"Login Data-journal", L"Web Data",     L"Web Data-journal",
                                    L"History",     L"Bookmarks",          L"Favicons"};
    for (const wchar_t* f : alwaysFiles) {
        CopyFileTo(srcDef + L"\\" + f, dstDef + L"\\" + f);
    }

    const bool forceSync = PeekForceSessionSync(outCdpData);
    const bool emptyDst = !HasUsableCookies(dstDef);
    const bool needSeed = forceSync || emptyDst;
    if (needSeed) {
        CopyFileTo(srcDef + L"\\Cookies", dstDef + L"\\Cookies");
        CopyFileTo(srcDef + L"\\Cookies-journal", dstDef + L"\\Cookies-journal");
        MirrorTreeFile(srcDef + L"\\Network", dstDef + L"\\Network");
        MirrorTreeFile(srcDef + L"\\Local Storage", dstDef + L"\\Local Storage");
        MirrorTreeFile(srcDef + L"\\Session Storage", dstDef + L"\\Session Storage");
        MirrorTreeFile(srcDef + L"\\IndexedDB", dstDef + L"\\IndexedDB");
        if (HasUsableCookies(dstDef)) {
            ClearForceSessionSync(outCdpData);
            const wchar_t* why = forceSync ? L"强制重同步" : L"首次同步";
            LogLine(log, std::wstring(L"[cdp] Chromium 标准目录不能开调试口，") + why +
                             L"（仅写入副本，未结束日常浏览器）：" + outCdpData);
        } else {
            LogLine(log, L"[cdp] 会话同步未得到可用 Cookies（日常可能被锁或尚未登录）；"
                         L"保留强制重同步标记，下次再试（未改日常、未杀浏览器）：" + outCdpData);
            if (!forceSync) {
                const std::wstring marker = outCdpData + L"\\" + kForceSessionSyncMarker;
                HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        }
    } else {
        LogLine(log, L"[cdp] Chromium 标准目录不能开调试口，复用已有会话"
                     L"（未覆盖副本 Cookies，未改日常）：" + outCdpData);
    }
    return DirExists(outCdpData);
}

std::string B64Encode16(const unsigned char raw[16]) {
    static const char kTab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.resize(24);
    int oix = 0;
    for (int i = 0; i < 16; i += 3) {
        unsigned v = (unsigned)raw[i] << 16;
        if (i + 1 < 16) v |= (unsigned)raw[i + 1] << 8;
        if (i + 2 < 16) v |= raw[i + 2];
        o[oix++] = kTab[(v >> 18) & 63];
        o[oix++] = kTab[(v >> 12) & 63];
        o[oix++] = (i + 1 < 16) ? kTab[(v >> 6) & 63] : '=';
        o[oix++] = (i + 2 < 16) ? kTab[v & 63] : '=';
    }
    o.resize(static_cast<size_t>(oix));
    return o;
}

bool RecvExactFrom(SOCKET s, std::string& leftover, char* p, int n) {
    int got = 0;
    while (got < n) {
        if (!leftover.empty()) {
            const int take = (std::min)(n - got, static_cast<int>(leftover.size()));
            memcpy(p + got, leftover.data(), static_cast<size_t>(take));
            leftover.erase(0, static_cast<size_t>(take));
            got += take;
            continue;
        }
        const int r = recv(s, p + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool ParseLoopbackWsUrl(const std::wstring& wsUrl, int* outPort, std::string* outPath) {
    const std::string u = WideToUtf8(wsUrl);
    const char* p = u.c_str();
    if (u.rfind("ws://", 0) == 0)
        p += 5;
    else if (u.rfind("http://", 0) == 0)
        p += 7;
    else
        return false;
    if (*p == '[') {
        const char* rb = strchr(p, ']');
        if (!rb) return false;
        p = rb + 1;
    } else {
        while (*p && *p != ':' && *p != '/') ++p;
    }
    int port = 80;
    if (*p == ':') {
        ++p;
        port = 0;
        while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
    }
    if (*p != '/' || port <= 0) return false;
    if (outPort) *outPort = port;
    if (outPath) *outPath = p;
    return true;
}

bool WsSendText(SOCKET s, const std::string& msg) {
    const uint64_t n = msg.size();
    std::string framed;
    framed.push_back(static_cast<char>(0x81));
    if (n <= 125) {
        framed.push_back(static_cast<char>(0x80 | static_cast<unsigned>(n)));
    } else if (n <= 0xFFFF) {
        framed.push_back(static_cast<char>(0x80 | 126));
        framed.push_back(static_cast<char>(n >> 8));
        framed.push_back(static_cast<char>(n));
    } else {
        framed.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) framed.push_back(static_cast<char>(n >> (8 * i)));
    }
    unsigned char mask[4] = {
        static_cast<unsigned char>(GetTickCount()),
        static_cast<unsigned char>(GetCurrentThreadId()),
        static_cast<unsigned char>(n ^ 0xA5),
        static_cast<unsigned char>(n >> 8),
    };
    framed.append(reinterpret_cast<char*>(mask), 4);
    const size_t off = framed.size();
    framed.resize(off + static_cast<size_t>(n));
    for (uint64_t k = 0; k < n; ++k)
        framed[off + static_cast<size_t>(k)] =
            static_cast<char>(static_cast<unsigned char>(msg[static_cast<size_t>(k)]) ^ mask[k % 4]);
    return send(s, framed.data(), static_cast<int>(framed.size()), 0) ==
           static_cast<int>(framed.size());
}

bool WsRecvMessage(SOCKET s, std::string& leftover, std::string& out) {
    out.clear();
    for (;;) {
        unsigned char h2[2];
        if (!RecvExactFrom(s, leftover, reinterpret_cast<char*>(h2), 2)) return false;
        const int opcode = h2[0] & 0x0F;
        const bool fin = (h2[0] & 0x80) != 0;
        const bool masked = (h2[1] & 0x80) != 0;
        uint64_t len = h2[1] & 0x7F;
        if (len == 126) {
            unsigned char e[2];
            if (!RecvExactFrom(s, leftover, reinterpret_cast<char*>(e), 2)) return false;
            len = (static_cast<uint64_t>(e[0]) << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (!RecvExactFrom(s, leftover, reinterpret_cast<char*>(e), 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        unsigned char mask[4]{};
        if (masked && !RecvExactFrom(s, leftover, reinterpret_cast<char*>(mask), 4)) return false;
        if (len > 8ull * 1024ull * 1024ull) return false;
        std::string payload(static_cast<size_t>(len), 0);
        if (len && !RecvExactFrom(s, leftover, payload.data(), static_cast<int>(len))) return false;
        if (masked) {
            for (size_t k = 0; k < payload.size(); ++k)
                payload[k] = static_cast<char>(static_cast<unsigned char>(payload[k]) ^ mask[k % 4]);
        }
        if (opcode == 0x8) return false;
        if (opcode == 0x9) {
            unsigned char pongHdr[2] = {0x8A, static_cast<unsigned char>(payload.size() <= 125
                                                                            ? payload.size()
                                                                            : 0)};
            if (payload.size() <= 125) {
                send(s, reinterpret_cast<char*>(pongHdr), 2, 0);
                if (!payload.empty()) send(s, payload.data(), static_cast<int>(payload.size()), 0);
            }
            continue;
        }
        if (opcode == 0xA) continue;
        out += payload;
        if (fin && (opcode == 0x0 || opcode == 0x1 || opcode == 0x2)) return true;
        if (!fin) continue;
        return !out.empty();
    }
}

}  // namespace

void RequestCdpSessionResync() {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return;
    const std::wstring xcat = std::wstring(localApp) + L"\\XCat";
    const std::wstring profile = xcat + L"\\GamaPassCdpProfile";
    CreateDirectoryW(xcat.c_str(), nullptr);
    CreateDirectoryW(profile.c_str(), nullptr);
    const std::wstring marker = profile + L"\\.xcat_force_session_sync";
    HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

bool ResolvePreferredChromium(BrowserProfile& out, const LogFn& log) {
    out = {};
    if (!msc::launcher::HttpGamaPassPreferredBrowserExe(out.exe)) {
        LogLine(log, L"[cdp] 未找到 Chromium 系浏览器（请将系统默认浏览器设为 Chrome / Edge / Chrome++）");
        return false;
    }
    if (!msc::launcher::HttpGamaPassResolveUserDataDir(out.exe, out.userData)) {
        wchar_t localApp[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
            std::wstring leaf = out.exe;
            size_t slash = leaf.find_last_of(L"\\/");
            if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
            for (auto& c : leaf) c = (wchar_t)towlower(c);
            std::wstring cand;
            if (leaf.find(L"msedge") != std::wstring::npos)
                cand = std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data";
            else if (leaf.find(L"360chromex") != std::wstring::npos)
                cand = std::wstring(localApp) + L"\\360ChromeX\\Chrome\\User Data";
            else if (leaf.find(L"360chrome") != std::wstring::npos)
                cand = std::wstring(localApp) + L"\\360Chrome\\Chrome\\User Data";
            else if (leaf.find(L"360se") != std::wstring::npos)
                cand = std::wstring(localApp) + L"\\360se6\\User Data";
            else
                cand = std::wstring(localApp) + L"\\Google\\Chrome\\User Data";
            if (DirExists(cand)) out.userData = cand;
        }
    }
    if (out.userData.empty() || !DirExists(out.userData)) {
        LogLine(log, L"[cdp] 未解析到 User Data：" + out.exe);
        return false;
    }

    // 官方 Chrome/360：副本（调试口）；不杀日常窗。Edge/Chrome++：直开日常，须先释放目录锁。
    const bool standardChrome = IsStandardChromiumUserData(out.userData);
    if (!standardChrome) {
        const unsigned n = KillBrowsersBlockingProfile(out, kDefaultRemoteDebugPort, log);
        if (n > 0) Sleep(800);
    } else {
        LogLine(log, L"[cdp] Chrome/360 走 GamaPassCdpProfile 副本（绕过默认目录禁调试口；"
                     L"不结束日常浏览器，不写回日常 Cookie）");
    }

    if (standardChrome) {
        std::wstring cdpData;
        if (!PrepareCdpSafeUserData(out.userData, cdpData, log)) {
            LogLine(log, L"[cdp] 无法准备 CDP 专用 User Data 副本");
            return false;
        }
        out.userData = cdpData;
    } else if (IsEdgeUserDataDir(out.userData)) {
        LogLine(log, L"[cdp] 会话目录：Edge 直开日常 User Data（不建副本）=" + out.userData);
        LogLine(log, L"[cdp] 会话目录：已尝试结束日常 Edge 主进程；关调试窗只杀带调试口实例，"
                     L"不清 Cookie");
    } else {
        LogLine(log, L"[cdp] 会话目录：直开用户目录（非标准 User Data）=" + out.userData);
        LogLine(log, L"[cdp] 会话目录：已尝试结束占用主进程；关窗只杀带调试口实例");
    }
    LogLine(log, L"[cdp] 浏览器=" + out.exe);
    LogLine(log, L"[cdp] UserData=" + out.userData);
    return true;
}

Session::Session() = default;

Session::~Session() { Close(); }

void Session::Close() {
    if (wsSock_) {
        SOCKET s = static_cast<SOCKET>(wsSock_);
        closesocket(s);
        wsSock_ = 0;
    }
    wsLeftover_.clear();
    pageWsUrl_.clear();
    browserVersion_.clear();
}

bool Session::HttpGetLocal(int port, const wchar_t* path, std::string& body) {
    return HttpLocal(port, L"GET", path, body);
}

bool Session::HttpLocal(int port, const wchar_t* method, const wchar_t* path, std::string& body) {
    int af = 0;
    const bool ok = LoopbackHttp(port, method, path, body, &af, nullptr, nullptr, nullptr);
    if (ok && af) loopbackFam_ = af;
    return ok;
}

bool Session::PickPageWsUrl(int port, std::wstring& outWs, const LogFn& log) {
    outWs.clear();
    std::string body;
    // 优先附着已在 Galaxy / 选账号 / 选号 / 官网票 流程中的标签，避免乱挂空白页再 Navigate 冲登录入口
    auto scoreUrl = [](const std::string& urlUtf8) -> int {
        std::string u = urlUtf8;
        for (auto& c : u)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (u.find("selectgameaccount") != std::string::npos) return 100;
        if (u.find("access_token=") != std::string::npos || u.find("webtoken=") != std::string::npos)
            return 95;
        // 带 OTT 的 Main 可直接收票；空 Main 是守护重拉残留，勿高分附着
        if (u.find("maplestoryclassic.beanfun.com") != std::string::npos) {
            if (u.find("ott=") != std::string::npos || u.find("ott:") != std::string::npos) return 90;
            return 15;
        }
        if (u.find("select-account") != std::string::npos) return 85;
        if (u.find("galaxy.games.gamania.com") != std::string::npos) return 80;
        // 启动参数 about:blank 常是用户看见的那一页；优先于 chrome://newtab
        if (u.empty() || u == "about:blank" || u.rfind("about:blank", 0) == 0) return 5;
        // /login、/error、oauth 半截：不优先附着（启动层会重新开 Galaxy）
        return 0;
    };
    auto isJunkTab = [](const std::string& urlUtf8) -> bool {
        std::string u = urlUtf8;
        for (auto& c : u)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return u.rfind("chrome-extension://", 0) == 0 || u.rfind("chrome://", 0) == 0 ||
               u.rfind("edge://", 0) == 0 || u.rfind("devtools://", 0) == 0 ||
               u.rfind("extension://", 0) == 0 || u.find("ntp.msn.com") != std::string::npos ||
               u.find("msn.com/spartan") != std::string::npos;
    };

    if (HttpGetLocal(port, L"/json/list", body)) {
        int bestScore = -1;
        std::string bestWs;
        std::string bestUrl;
        size_t pos = 0;
        while ((pos = body.find("\"type\"", pos)) != std::string::npos) {
            size_t typeVal = body.find('"', pos + 5);
            if (typeVal == std::string::npos) break;
            size_t typeStart = body.find('"', typeVal + 1);
            if (typeStart == std::string::npos) break;
            ++typeStart;
            size_t typeEnd = body.find('"', typeStart);
            if (typeEnd == std::string::npos) break;
            std::string typ = body.substr(typeStart, typeEnd - typeStart);
            if (typ == "page" || typ == "Page") {
                size_t winStart = (pos > 500) ? pos - 500 : 0;
                size_t winEnd = (std::min)(body.size(), pos + 1000);
                std::string win = body.substr(winStart, winEnd - winStart);
                std::string ws = JsonGetString(win, "webSocketDebuggerUrl");
                if (!ws.empty()) {
                    std::string pageUrl = JsonGetString(win, "url");
                    // Chrome 144 会把扩展 background.html 标成 page；挂上去 Navigate Galaxy 不会走
                    //（AA7E 03:38：nkeimhogj…/background.html score=1，随后把真 blank 清掉）
                    if (isJunkTab(pageUrl)) continue;
                    const int sc = scoreUrl(pageUrl);
                    // 同分保留先扫到的；有分的优先于 0
                    if (sc > bestScore || (bestWs.empty() && sc == 0 && bestScore < 0)) {
                        bestScore = sc;
                        bestWs = ws;
                        bestUrl = pageUrl;
                        if (bestScore < 0) bestScore = 0;
                    }
                }
            }
            pos = typeEnd + 1;
        }
        if (!bestWs.empty()) {
            outWs = Utf8ToWide(bestWs);
            if (bestScore > 0) {
                LogLine(log, L"[cdp] 复用流程标签 score=" + std::to_wstring(bestScore) + L" url=" +
                                 Utf8ToWide(bestUrl).substr(0, 120));
            }
            return true;
        }
    }
    body.clear();
    // 没有可用 page 时才新建（Chrome 新版本：/json/new 需 PUT）
    LogLine(log, L"[cdp] 没有可附着的登录标签（已跳过扩展/chrome://），新建 about:blank");
    if (HttpLocal(port, L"PUT", L"/json/new", body)) {
        std::string ws = JsonGetString(body, "webSocketDebuggerUrl");
        if (!ws.empty()) {
            outWs = Utf8ToWide(ws);
            return true;
        }
    }
    LogLine(log, L"[cdp] 未找到 page 调试 WebSocket");
    return false;
}

bool Session::OpenWs(const std::wstring& wsUrl, const LogFn& log) {
    Close();
    int port = 0;
    std::string path;
    if (!ParseLoopbackWsUrl(wsUrl, &port, &path) || path.empty()) {
        LogLine(log, L"[cdp] CrackUrl 失败 url=" + wsUrl.substr(0, 120));
        return false;
    }
    int order[2] = {AF_INET, AF_INET6};
    if (loopbackFam_ == AF_INET6) {
        order[0] = AF_INET6;
        order[1] = AF_INET;
    }
    unsigned char rawKey[16];
    const DWORD mix = GetTickCount() ^ (GetCurrentThreadId() << 7);
    for (int i = 0; i < 16; ++i)
        rawKey[i] = static_cast<unsigned char>(mix >> (i % 4) * 8 ^ (i * 29));
    const std::string keyB64 = B64Encode16(rawKey);
    for (int k = 0; k < 2; ++k) {
        const int af = order[k];
        SOCKET s = INVALID_SOCKET;
        if (!TcpConnectFam(af, port, 3000, &s)) continue;
        char host[80]{};
        if (af == AF_INET)
            sprintf_s(host, "127.0.0.1:%d", port);
        else
            sprintf_s(host, "[::1]:%d", port);
        char req[1536]{};
        sprintf_s(req,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "Upgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "\r\n",
                  path.c_str(), host, keyB64.c_str());
        const int reqLen = static_cast<int>(strlen(req));
        if (send(s, req, reqLen, 0) != reqLen) {
            closesocket(s);
            continue;
        }
        std::string raw;
        char buf[1024];
        const DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 3000) {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            if (raw.find("\r\n\r\n") != std::string::npos) break;
            if (raw.size() > 8192) break;
        }
        if (raw.find("101") == std::string::npos || raw.find("\r\n\r\n") == std::string::npos) {
            closesocket(s);
            continue;
        }
        const size_t hdrEnd = raw.find("\r\n\r\n");
        wsLeftover_ = raw.substr(hdrEnd + 4);
        DWORD to = 8000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        wsSock_ = static_cast<std::uintptr_t>(s);
        loopbackFam_ = af;
        return true;
    }
    LogLine(log, L"[cdp] WebSocket 握手失败 port=" + std::to_wstring(port));
    return false;
}

bool Session::SendRecv(const std::string& method, const std::string& paramsJson,
                       std::string& resultJson, const LogFn& log) {
    resultJson.clear();
    if (!wsSock_) return false;
    SOCKET s = static_cast<SOCKET>(wsSock_);
    const int id = nextId_++;
    std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\"";
    if (!paramsJson.empty()) msg += ",\"params\":" + paramsJson;
    msg += "}";
    if (!WsSendText(s, msg)) {
        LogLine(log, L"[cdp] WebSocketSend 失败 method=" + Utf8ToWide(method));
        return false;
    }
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 15000) {
        std::string buf;
        if (!WsRecvMessage(s, wsLeftover_, buf)) {
            LogLine(log, L"[cdp] WebSocketReceive 失败");
            return false;
        }
        const std::string idPat = "\"id\":" + std::to_string(id);
        if (buf.find(idPat) != std::string::npos) {
            resultJson = std::move(buf);
            return resultJson.find("\"error\"") == std::string::npos ||
                   resultJson.find("\"result\"") != std::string::npos;
        }
    }
    LogLine(log, L"[cdp] 等待响应超时 method=" + Utf8ToWide(method));
    return false;
}

namespace {

std::wstring NormalizePathKey(std::wstring p) {
    for (auto& c : p) {
        if (c == L'/') c = L'\\';
        c = (wchar_t)towlower(c);
    }
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

bool PathKeysEqual(const std::wstring& a, const std::wstring& b) {
    return NormalizePathKey(a) == NormalizePathKey(b);
}

std::wstring ParentPathW(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return p.substr(0, slash);
}

bool ExtractUserDataDirFromCmd(const std::wstring& cmd, std::wstring& outDir) {
    outDir.clear();
    const auto args = msc::launcher::SplitCommandLineArgs(cmd);
    for (size_t i = 0; i < args.size(); ++i) {
        std::wstring a = args[i];
        std::wstring al = a;
        for (auto& c : al) c = (wchar_t)towlower(c);
        if (al.rfind(L"--user-data-dir=", 0) == 0) {
            outDir = a.substr(15);
            while (!outDir.empty() && (outDir.front() == L'"' || outDir.back() == L'"')) {
                if (!outDir.empty() && outDir.front() == L'"') outDir.erase(outDir.begin());
                if (!outDir.empty() && outDir.back() == L'"') outDir.pop_back();
            }
            return !outDir.empty();
        }
        if (al == L"--user-data-dir" && i + 1 < args.size()) {
            outDir = args[i + 1];
            return !outDir.empty();
        }
    }
    return false;
}

bool CmdHasRemoteDebugPort(const std::wstring& cmd, int port) {
    const auto args = msc::launcher::SplitCommandLineArgs(cmd);
    const std::wstring eq = L"--remote-debugging-port=" + std::to_wstring(port);
    for (size_t i = 0; i < args.size(); ++i) {
        if (_wcsicmp(args[i].c_str(), eq.c_str()) == 0) return true;
        if (_wcsicmp(args[i].c_str(), L"--remote-debugging-port") == 0 && i + 1 < args.size()) {
            if (_wtoi(args[i + 1].c_str()) == port) return true;
        }
    }
    return false;
}

bool SameBrowserInstall(const std::wstring& runningExe, const std::wstring& profileExe) {
    if (runningExe.empty() || profileExe.empty()) return false;
    if (PathKeysEqual(runningExe, profileExe)) return true;
    return PathKeysEqual(ParentPathW(runningExe), ParentPathW(profileExe));
}

bool QueryProcessImagePath(DWORD pid, std::wstring& outPath) {
    outPath.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
    CloseHandle(h);
    if (!ok || !path[0]) return false;
    outPath = path;
    return true;
}

bool IsWantedChromiumProcessName(const wchar_t* name) {
    return (_wcsicmp(name, L"chrome.exe") == 0) || (_wcsicmp(name, L"msedge.exe") == 0) ||
           (_wcsicmp(name, L"chromium.exe") == 0) || (_wcsicmp(name, L"360chrome.exe") == 0) ||
           (_wcsicmp(name, L"360chromex.exe") == 0) || (_wcsicmp(name, L"360se.exe") == 0) ||
           (_wcsicmp(name, L"360browser.exe") == 0);
}

// GPU/renderer/utility/crashpad 等子进程 cmdline 带 --type=；杀掉它们既释放不了 Singleton，
// 还可能误伤刚拉起的调试实例（E216：等待期把 msedge 杀掉后调试口永死）。
bool IsChromiumSubprocessCmd(const std::wstring& cmd) {
    if (cmd.empty()) return false;
    std::wstring al = cmd;
    for (auto& c : al) c = (wchar_t)towlower(c);
    return al.find(L"--type=") != std::wstring::npos;
}

struct ConflictHit {
    DWORD pid = 0;
    std::wstring leaf;
};

bool IsIsolatedXcatCdpUserData(const std::wstring& userData) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return false;
    const std::wstring root = std::wstring(localApp) + L"\\XCat\\";
    if (PathKeysEqual(userData, root + L"GamaPassCdpProfile")) return true;
    const std::wstring gp = NormalizePathKey(root + L"GpDeviceLoginProfile");
    const std::wstring ud = NormalizePathKey(userData);
    if (ud == gp) return true;
    return ud.size() > gp.size() && ud.compare(0, gp.size(), gp) == 0 && ud[gp.size()] == L'\\';
}

void CollectConflictingBrowserHits(const BrowserProfile& profile, int debugPort,
                                   std::vector<ConflictHit>& out) {
    out.clear();
    if (profile.userData.empty()) return;

    // 独立目录：只杀显式 --user-data-dir=该目录 的实例，绝不按「同安装」误杀日常 Chrome。
    const bool usingCdpCopy = IsIsolatedXcatCdpUserData(profile.userData);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        if (!IsWantedChromiumProcessName(pe.szExeFile)) continue;

        const DWORD pid = pe.th32ProcessID;
        std::wstring img;
        if (!QueryProcessImagePath(pid, img)) continue;

        const std::wstring cmd = msc::launcher::GetProcessCommandLineW(pid);
        if (CmdHasRemoteDebugPort(cmd, debugPort)) continue;  // 已是我们要的调试实例
        if (IsChromiumSubprocessCmd(cmd)) continue;           // 只杀浏览器主进程

        std::wstring ud;
        const bool hasUd = ExtractUserDataDirFromCmd(cmd, ud);

        bool hit = false;
        if (hasUd && PathKeysEqual(ud, profile.userData)) {
            hit = true;
        } else if (!usingCdpCopy && SameBrowserInstall(img, profile.exe) &&
                   !(hasUd && !PathKeysEqual(ud, profile.userData))) {
            // 直开日常：同安装主进程且未显式指向其它 User Data → 多半锁日常目录
            hit = true;
        }

        if (!hit) continue;

        bool dup = false;
        for (const auto& existing : out) {
            if (existing.pid == pid) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        ConflictHit c;
        c.pid = pid;
        c.leaf = pe.szExeFile;
        out.push_back(std::move(c));
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
}

unsigned TerminateConflictHits(const std::vector<ConflictHit>& hits, const LogFn& log) {
    unsigned killed = 0;
    for (const auto& h : hits) {
        HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, h.pid);
        if (!proc) {
            LogLine(log, L"[cdp] 防呆：无法打开进程终止权限 pid=" + std::to_wstring(h.pid) +
                             L" name=" + h.leaf);
            continue;
        }
        if (TerminateProcess(proc, 1)) {
            WaitForSingleObject(proc, 3000);
            ++killed;
            LogLine(log, L"[cdp] 防呆：已结束 " + h.leaf + L" pid=" + std::to_wstring(h.pid) +
                             L"（释放 User Data，未清 Cookie）");
        }
        CloseHandle(proc);
    }
    return killed;
}

bool AnyChromiumHasDebugPort(int debugPort) {
    if (debugPort <= 0) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return false;
    }
    bool found = false;
    do {
        if (!IsWantedChromiumProcessName(pe.szExeFile)) continue;
        const std::wstring cmd = msc::launcher::GetProcessCommandLineW(pe.th32ProcessID);
        if (CmdHasRemoteDebugPort(cmd, debugPort)) {
            found = true;
            break;
        }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

// CreateProcess 比 ShellExecute 更稳：Edge 经 Shell 启动时偶发丢掉调试参数（E216 调试口永不起）。
bool LaunchChromiumWithDebugPort(const BrowserProfile& profile, int port, const LogFn& log,
                                 DWORD* outPid) {
    if (outPid) *outPid = 0;
    if (profile.exe.empty() || profile.userData.empty() || port < 0) return false;

    PrepareIsolatedLaunchUserData(profile.userData);

    // 整行命令行；user-data-dir 加引号防空格路径
    // 不钉 --remote-debugging-address=127.0.0.1：Hyper-V 常把 19223 的 IPv4 占成黑洞
    //（WIN-20260826WCU：tcp=1 http=0），Chrome 绑 127.0.0.1 失败后窗仍开、CDP 永不起来。
    // 让 Chrome 绑 localhost（::1 或 127.0.0.1）；HTTP 探活两族都试。port=0 则读 DevToolsActivePort。
    std::wstring cmd = L"\"";
    cmd += profile.exe;
    cmd += L"\" --remote-debugging-port=";
    cmd += std::to_wstring(port);
    cmd += L" --remote-allow-origins=* --user-data-dir=\"";
    cmd += profile.userData;
    cmd += L"\" --no-first-run --no-default-browser-check"
           L" --force-renderer-accessibility"
           L" --disable-session-crashed-bubble --hide-crash-restore-bubble"
           // 独立罐登录只要几十秒：别去查更新，否则右上角弹「无法安装更新」（客户机 Chrome 120 已过期）。
           L" --check-for-update-interval=31536000 --disable-background-networking"
           // 独立罐勿弹「翻译此页」：翻译条会挡住账号卡，首点看起来像卡死（BIN 02:38 select-account）。
           // Translate* 管 Chrome；msEdgeTranslate / msTranslate / 首次运行管 Edge。
           L" --disable-translate --disable-sync --disable-infobars --disable-features="
           L"Translate,TranslateUI,msEdgeTranslate,msTranslate,"
           L"msEdgeFirstRunExperience,msImplicitSignin,"
           L"ChromeWhatsNewUI,SafetyHub,OutdatedUpgradeBubble,InfiniteSessionRestore"
           L" --new-window about:blank";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    LogLine(log, L"[cdp] 启动浏览器（CreateProcess + 调试口）…");
    if (!CreateProcessW(profile.exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        const DWORD err = GetLastError();
        LogLine(log, L"[cdp] CreateProcess 失败 err=" + std::to_wstring(err) + L" exe=" + profile.exe);
        return false;
    }
    if (outPid) *outPid = pi.dwProcessId;
    LogLine(log, L"[cdp] 已拉起浏览器 pid=" + std::to_wstring(pi.dwProcessId) + L" port=" +
                     std::to_wstring(port) +
                     (port == 0 ? L"（系统分配，等 DevToolsActivePort）" : L""));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

unsigned KillBrowsersBlockingProfile(const BrowserProfile& profile, int debugPort, const LogFn& log) {
    if (profile.userData.empty()) return 0;
    unsigned total = 0;
    for (int round = 0; round < 3; ++round) {
        std::vector<ConflictHit> hits;
        CollectConflictingBrowserHits(profile, debugPort, hits);
        if (hits.empty()) break;
        total += TerminateConflictHits(hits, log);
        Sleep(400);
    }
    if (total > 0) {
        LogLine(log, L"[cdp] 防呆：已结束占用配置目录的浏览器 ×" + std::to_wstring(total) +
                         L" dir=" + profile.userData);
    }
    return total;
}

unsigned KillDailyBrowsersForUiaLogin(const std::wstring& preferredExe, const LogFn& log) {
    if (preferredExe.empty()) return 0;

    unsigned total = 0;
    for (int round = 0; round < 3; ++round) {
        std::vector<ConflictHit> hits;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) break;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == GetCurrentProcessId()) continue;
                if (!IsWantedChromiumProcessName(pe.szExeFile)) continue;

                const DWORD pid = pe.th32ProcessID;
                std::wstring img;
                if (!QueryProcessImagePath(pid, img)) continue;
                if (!SameBrowserInstall(img, preferredExe)) continue;

                const std::wstring cmd = msc::launcher::GetProcessCommandLineW(pid);
                if (IsChromiumSubprocessCmd(cmd)) continue;  // 只杀主进程

                bool dup = false;
                for (const auto& existing : hits) {
                    if (existing.pid == pid) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                ConflictHit c;
                c.pid = pid;
                c.leaf = pe.szExeFile;
                hits.push_back(std::move(c));
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        if (hits.empty()) break;
        total += TerminateConflictHits(hits, log);
        Sleep(500);
    }

    if (total > 0) {
        LogLine(log, L"[uia] 自动登录前已结束已开浏览器主进程 ×" + std::to_wstring(total) +
                         L"（同安装；不清 Cookie，本轮只拉起登录窗）");
        Sleep(800);  // 等 User Data 锁释放
    } else {
        LogLine(log, L"[uia] 自动登录前：未发现需关闭的已开浏览器主进程");
    }
    return total;
}

bool Session::Connect(int port, const LogFn& log) {
    Close();
    port_ = port;
    std::string ver;
    if (!HttpGetLocal(port, L"/json/version", ver)) {
        LogLine(log, L"[cdp] 调试口无响应 port=" + std::to_wstring(port));
        return false;
    }
    browserVersion_ = Utf8ToWide(JsonGetString(ver, "Browser"));
    if (browserVersion_.empty()) browserVersion_ = L"(unknown)";
    std::wstring wsUrl;
    if (!PickPageWsUrl(port, wsUrl, log)) return false;
    pageWsUrl_ = wsUrl;
    if (!OpenWs(wsUrl, log)) return false;
    std::string ignore;
    SendRecv("Page.enable", "{}", ignore, log);
    SendRecv("Runtime.enable", "{}", ignore, log);
    LogLine(log, L"[cdp] 已连接 " + browserVersion_ + L" port=" + std::to_wstring(port));
    return true;
}

bool Session::ProbeUserDataConflict(const BrowserProfile& profile, int debugPort,
                                    std::wstring& outHint, const LogFn& log) {
    outHint.clear();
    std::vector<ConflictHit> hits;
    CollectConflictingBrowserHits(profile, debugPort, hits);
    if (hits.empty()) return false;

    outHint = L"检测到浏览器仍占用配置目录（";
    outHint += hits.front().leaf;
    outHint += L" pid=";
    outHint += std::to_wstring(hits.front().pid);
    if (hits.size() > 1) {
        outHint += L" 等共 ";
        outHint += std::to_wstring(hits.size());
        outHint += L" 个进程";
    }
    outHint += L"）。已尝试自动结束仍残留，请手动关闭该浏览器后重试。";
    LogLine(log, L"[cdp] 防呆残留：" + outHint);
    LogLine(log, L"[cdp] 冲突配置目录=" + profile.userData);
    return true;
}

bool Session::EnsureBrowser(const BrowserProfile& profile, int port, const LogFn& log,
                            std::wstring* outFailHint) {
    if (outFailHint) outFailHint->clear();
    auto failCanceled = [&]() -> bool {
        LogLine(log, L"[cdp] 用户取消，停止打开调试浏览器");
        if (outFailHint) *outFailHint = L"用户取消登录";
        (void)CloseRemoteBrowser(port, log);
        return false;
    };
    if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
    if (Connect(port, log)) {
        if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
        return true;
    }
    if (profile.exe.empty() || profile.userData.empty()) {
        if (outFailHint) *outFailHint = L"未解析到浏览器可执行文件或配置目录";
        return false;
    }
    if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();

    // 自动登录前清占用独立罐的进程（含上次残留的 --remote-debugging-port=19223 黑洞实例）
    {
        const unsigned n = KillBrowsersBlockingProfile(profile, -1, log);
        if (n > 0) Sleep(800);
    }

    std::wstring busyHint;
    if (ProbeUserDataConflict(profile, port, busyHint, log)) {
        if (outFailHint) *outFailHint = busyHint;
        return false;
    }

    int launchPort = port;
    auto tryLaunch = [&]() -> bool {
        DWORD pid = 0;
        if (!LaunchChromiumWithDebugPort(profile, launchPort, log, &pid)) {
            std::wstring hint = L"启动浏览器失败（CreateProcess）。请确认 Edge/Chrome 可手动打开后重试。";
            LogLine(log, L"[cdp] " + hint);
            if (outFailHint) *outFailHint = hint;
            return false;
        }
        return true;
    };

    if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
    if (!tryLaunch()) return false;

    // 等待 CDP HTTP。tcp=1 不等于调试口：Hyper-V/WinNAT 会占 19223，接 TCP 但不说 HTTP
    //（WIN-20260826WCU 0.1.189：全程 tcp=1 http=0，窗停在 about:blank）。
    int relaunches = 0;
    for (int i = 0; i < 40; ++i) {
        if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
        Sleep(250);
        int filePort = 0;
        ReadDevToolsActivePort(profile.userData, &filePort);
        const int candidates[3] = {filePort, launchPort, port};
        bool http = false;
        int livePort = 0;
        bool tcp4 = false;
        bool tcp6 = false;
        std::string peek;
        std::string ver;
        for (int ci = 0; ci < 3; ++ci) {
            const int tp = candidates[ci];
            if (tp <= 0) continue;
            bool dup = false;
            for (int pj = 0; pj < ci; ++pj) {
                if (candidates[pj] == tp) dup = true;
            }
            if (dup) continue;
            bool t4 = false;
            bool t6 = false;
            std::string p;
            int af = 0;
            if (LoopbackHttp(tp, L"GET", L"/json/version", ver, &af, &p, &t4, &t6)) {
                http = true;
                livePort = tp;
                tcp4 = t4;
                tcp6 = t6;
                peek = std::move(p);
                loopbackFam_ = af;
                break;
            }
            const int logPort = filePort > 0 ? filePort : (launchPort > 0 ? launchPort : port);
            if (tp == logPort) {
                tcp4 = t4;
                tcp6 = t6;
                peek = std::move(p);
            }
        }
        if (i == 0 || i == 3 || i == 8 || (i % 10) == 9) {
            LogLine(log, L"[cdp] 等待调试口 tick=" + std::to_wstring(i) + L" tcp4=" +
                             (tcp4 ? L"1" : L"0") + L" tcp6=" + (tcp6 ? L"1" : L"0") + L" http=" +
                             (http ? L"1" : L"0") + L" want=" + std::to_wstring(launchPort) +
                             L" file=" + std::to_wstring(filePort) + L" peek=" + Utf8ToWide(peek));
        }
        if (http && Connect(livePort, log)) {
            if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
            return true;
        }

        const bool checkpoint = (i == 4 || i == 10 || i == 18 || i == 28);
        if (!checkpoint) continue;

        if (!http && relaunches < 2) {
            const bool blackHole = (tcp4 || tcp6) && filePort <= 0;
            const bool stillBooting = !tcp4 && !tcp6 && i < 10;
            const bool fileButNoHttp = filePort > 0 && i < 18;
            if (stillBooting || fileButNoHttp) {
                LogLine(log, L"[cdp] 已检测到调试口进程，继续等待 file=" +
                                 std::to_wstring(filePort) + L" tcp4=" + (tcp4 ? L"1" : L"0") +
                                 L" tcp6=" + (tcp6 ? L"1" : L"0"));
                continue;
            }
            if (!blackHole && i < 10) continue;
            LogLine(log, L"[cdp] 调试口无 HTTP（file=" + std::to_wstring(filePort) + L" tcp4=" +
                             (tcp4 ? L"1" : L"0") + L" tcp6=" + (tcp6 ? L"1" : L"0") + L" peek=" +
                             Utf8ToWide(peek) + L"），改用系统分配端口重开…");
            if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
            (void)KillBrowsersBlockingProfile(profile, -1, log);
            Sleep(600);
            launchPort = 0;
            if (!tryLaunch()) return false;
            ++relaunches;
            continue;
        }

        if (relaunches >= 2) continue;
        if (msc::launcher::GamaPassLoginCanceled()) return failCanceled();
        LogLine(log, L"[cdp] 未检测到可用调试口，防呆结束占用后重开…");
        (void)KillBrowsersBlockingProfile(profile, -1, log);
        Sleep(600);
        launchPort = 0;
        if (!tryLaunch()) return false;
        ++relaunches;
    }

    if (ProbeUserDataConflict(profile, port, busyHint, log)) {
        if (outFailHint) *outFailHint = busyHint;
        return false;
    }
    const std::wstring hint =
        L"等待浏览器调试口超时。19223 上有 TCP 不等于 Chrome CDP（Hyper-V 常占成黑洞）。"
        L"已尝试系统分配端口；若仍失败请关掉独立罐 Chrome 后重试。";
    LogLine(log, L"[cdp] " + hint);
    if (!AnyChromiumHasDebugPort(port)) {
        LogLine(log, L"[cdp] 诊断：全程未出现 cmdline 含 --remote-debugging-port=" +
                         std::to_wstring(port) + L" 的进程（参数可能被浏览器忽略）");
    }
    if (outFailHint) *outFailHint = hint;
    return false;
}

bool Session::Navigate(const std::wstring& url, const LogFn& log) {
    std::string raw = WideToUtf8(url);
    std::string esc;
    esc.reserve(raw.size() + 16);
    for (char c : raw) {
        if (c == '\\' || c == '"') {
            esc.push_back('\\');
            esc.push_back(c);
        } else if (c == '\n') {
            esc += "\\n";
        } else if (c == '\r') {
            esc += "\\r";
        } else {
            esc.push_back(c);
        }
    }
    std::string params = std::string("{\"url\":\"") + esc + "\"}";
    std::string res;
    if (!SendRecv("Page.navigate", params, res, log)) return false;
    Sleep(1200);
    return true;
}

bool Session::Command(const std::string& method, const std::string& paramsJson,
                      std::string& resultJson, const LogFn& log) {
    return SendRecv(method, paramsJson, resultJson, log);
}

bool Session::Evaluate(const std::wstring& jsExpression, std::string& outResultJson,
                       const LogFn& log) {
    outResultJson.clear();
    // JSON-escape the expression
    std::string js = WideToUtf8(jsExpression);
    std::string esc;
    esc.reserve(js.size() + 16);
    for (char c : js) {
        if (c == '\\' || c == '"') {
            esc.push_back('\\');
            esc.push_back(c);
        } else if (c == '\n') {
            esc += "\\n";
        } else if (c == '\r') {
            esc += "\\r";
        } else if (c == '\t') {
            esc += "\\t";
        } else {
            esc.push_back(c);
        }
    }
    std::string params = "{\"expression\":\"" + esc + "\",\"returnByValue\":true}";
    std::string res;
    if (!SendRecv("Runtime.evaluate", params, res, log)) return false;
    // result.result.value
    size_t v = res.find("\"value\"");
    if (v == std::string::npos) {
        outResultJson = res;
        return true;
    }
    outResultJson = res.substr(v);
    return true;
}

bool Session::GetUrl(std::wstring& outUrl, const LogFn& log) {
    outUrl.clear();
    std::string res;
    if (!Evaluate(L"location.href", res, log)) return false;
    // value":"https://..."
    std::string u = JsonGetString(std::string("{" ) + res, "value");
    if (u.empty()) {
        // try from raw
        size_t p = res.find("\"value\":\"");
        if (p != std::string::npos) {
            p += 9;
            size_t e = res.find('"', p);
            if (e != std::string::npos) u = res.substr(p, e - p);
        }
    }
    outUrl = Utf8ToWide(u);
    return !outUrl.empty();
}

int Session::CloseExtraBlankPages(const LogFn& log) {
    if (port_ <= 0) return 0;
    std::string body;
    if (!HttpGetLocal(port_, L"/json/list", body)) return 0;

    int closed = 0;
    size_t pos = 0;
    while ((pos = body.find("\"id\"", pos)) != std::string::npos) {
        size_t winStart = (pos > 400) ? pos - 400 : 0;
        size_t winEnd = (std::min)(body.size(), pos + 800);
        std::string win = body.substr(winStart, winEnd - winStart);
        const std::string typ = JsonGetString(win, "type");
        if (typ != "page" && typ != "Page") {
            pos += 4;
            continue;
        }
        std::string url = JsonGetString(win, "url");
        for (auto& c : url)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        const bool blank = url.empty() || url == "about:blank" || url.rfind("about:blank", 0) == 0;
        if (!blank) {
            pos += 4;
            continue;
        }
        // 保留当前附着页（若仍是 blank，留给上层 Navigate）
        const std::string ws = JsonGetString(win, "webSocketDebuggerUrl");
        if (!pageWsUrl_.empty() && !ws.empty()) {
            const std::wstring wsW = Utf8ToWide(ws);
            if (_wcsicmp(wsW.c_str(), pageWsUrl_.c_str()) == 0) {
                pos += 4;
                continue;
            }
        }
        const std::string id = JsonGetString(win, "id");
        if (id.empty()) {
            pos += 4;
            continue;
        }
        std::wstring path = L"/json/close/";
        path.append(id.begin(), id.end());
        std::string ignore;
        if (HttpGetLocal(port_, path.c_str(), ignore)) {
            ++closed;
            LogLine(log, L"[cdp] 已关闭多余空白标签 id=" + Utf8ToWide(id));
        }
        pos += 4;
    }
    return closed;
}

bool Session::ActivateAttachedPage(const LogFn& log) {
    if (port_ <= 0 || pageWsUrl_.empty()) return false;
    std::string body;
    if (!HttpGetLocal(port_, L"/json/list", body)) return false;
    size_t pos = 0;
    while ((pos = body.find("\"id\"", pos)) != std::string::npos) {
        size_t winStart = (pos > 400) ? pos - 400 : 0;
        size_t winEnd = (std::min)(body.size(), pos + 800);
        std::string win = body.substr(winStart, winEnd - winStart);
        const std::string ws = JsonGetString(win, "webSocketDebuggerUrl");
        if (!ws.empty()) {
            const std::wstring wsW = Utf8ToWide(ws);
            if (_wcsicmp(wsW.c_str(), pageWsUrl_.c_str()) == 0) {
                const std::string id = JsonGetString(win, "id");
                if (id.empty()) break;
                std::wstring path = L"/json/activate/";
                path.append(id.begin(), id.end());
                std::string ignore;
                if (HttpGetLocal(port_, path.c_str(), ignore)) {
                    LogLine(log, L"[cdp] 已把附着标签拉到前台 id=" + Utf8ToWide(id));
                    return true;
                }
                LogLine(log, L"[cdp] 激活标签失败 id=" + Utf8ToWide(id));
                return false;
            }
        }
        pos += 4;
    }
    return false;
}

bool Session::ClickViewport(double x, double y, const LogFn& log) {
    auto send = [&](const char* type, const char* button, int clickCount) -> bool {
        char buf[288];
        sprintf_s(buf,
                  "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\",\"clickCount\":%d,"
                  "\"pointerType\":\"mouse\"}",
                  type, x, y, button, clickCount);
        std::string res;
        return SendRecv("Input.dispatchMouseEvent", buf, res, log);
    };
    (void)ActivateAttachedPage(log);
    if (!send("mouseMoved", "none", 0)) {
        LogLine(log, L"[cdp] Input.mouseMoved 失败");
        return false;
    }
    Sleep(20);
    if (!send("mousePressed", "left", 1)) {
        LogLine(log, L"[cdp] Input.mousePressed 失败");
        return false;
    }
    Sleep(30);
    if (!send("mouseReleased", "left", 1)) {
        LogLine(log, L"[cdp] Input.mouseReleased 失败");
        return false;
    }
    return true;
}

bool Session::QuitBrowser(int port, const LogFn& log) {
    if (port <= 0) port = port_ > 0 ? port_ : kDefaultRemoteDebugPort;
    std::string ver;
    if (!HttpGetLocal(port, L"/json/version", ver)) {
        LogLine(log, L"[cdp] 关闭浏览器：调试口无响应 port=" + std::to_wstring(port));
        return false;
    }
    const std::string wsUtf8 = JsonGetString(ver, "webSocketDebuggerUrl");
    if (wsUtf8.empty()) {
        LogLine(log, L"[cdp] 关闭浏览器：无 browser WebSocket");
        return false;
    }
    // 切到浏览器级调试口（Page 级 WS 不一定有 Browser 域）
    Close();
    port_ = port;
    if (!OpenWs(Utf8ToWide(wsUtf8), log)) {
        LogLine(log, L"[cdp] 关闭浏览器：连接 browser WS 失败");
        return false;
    }
    // Browser.close 常直接拆掉连接，不等待完整 JSON 响应
    const int id = nextId_++;
    const std::string msg =
        std::string("{\"id\":") + std::to_string(id) + ",\"method\":\"Browser.close\",\"params\":{}}";
    if (!WsSendText(static_cast<SOCKET>(wsSock_), msg)) {
        LogLine(log, L"[cdp] Browser.close 发送失败");
        Close();
        return false;
    }
    Sleep(400);
    Close();
    LogLine(log, L"[cdp] 已请求 Browser.close");
    return true;
}

// 结束占用本调试口的进程：只认 cmdline 含 --remote-debugging-port=N
//（不限 chrome.exe 名，覆盖 Edge / 360ChromeX / 换皮 Chromium）
unsigned KillBrowsersOnDebugPort(int port, const LogFn& log) {
    if (port <= 0) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return 0;
    }

    unsigned killed = 0;
    do {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;

        const std::wstring cmd = msc::launcher::GetProcessCommandLineW(pe.th32ProcessID);
        if (cmd.empty() || !CmdHasRemoteDebugPort(cmd, port)) continue;

        HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!proc) {
            LogLine(log, L"[cdp] 无法打开进程终止权限 pid=" + std::to_wstring(pe.th32ProcessID) +
                             L" name=" + pe.szExeFile);
            continue;
        }
        if (TerminateProcess(proc, 1)) {
            WaitForSingleObject(proc, 3000);
            ++killed;
            LogLine(log, std::wstring(L"[cdp] 已结束调试浏览器 ") + pe.szExeFile + L" pid=" +
                             std::to_wstring(pe.th32ProcessID) + L" port=" + std::to_wstring(port));
        }
        CloseHandle(proc);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return killed;
}

bool Session::IsPortAlive(int port) {
    if (port <= 0) return false;
    if (!TcpLoopbackOpen(port, 200)) return false;
    Session s;
    std::string ver;
    return s.HttpGetLocal(port, L"/json/version", ver);
}

bool CloseRemoteBrowser(int port, const LogFn& log) {
    if (port <= 0) port = kDefaultRemoteDebugPort;

    // 先礼后兵：Browser.close → 轮询等调试口自行消失（Cookie 落盘窗口）→ 再精确杀残留。
    // 口已死则立刻往下，不再盲等满额；最长约 800ms。
    Session s;
    (void)s.QuitBrowser(port, log);
    const DWORD deadline = GetTickCount() + 800u;
    while (Session::IsPortAlive(port)) {
        if (GetTickCount() >= deadline) break;
        Sleep(50);
    }

    const unsigned n = KillBrowsersOnDebugPort(port, log);
    // 再扫一轮残留
    if (n > 0) {
        Sleep(200);
        (void)KillBrowsersOnDebugPort(port, log);
    }

    if (Session::IsPortAlive(port)) {
        LogLine(log, L"[cdp] 警告：调试口仍在响应 port=" + std::to_wstring(port) +
                         L"（可能权限不足，请手动关浏览器）");
        return false;
    }
    if (n == 0) {
        LogLine(log, L"[cdp] 登录浏览器已关闭（无残留调试进程）port=" + std::to_wstring(port));
    } else {
        LogLine(log, L"[cdp] 登录浏览器已强制结束 ×" + std::to_wstring(n) +
                         L"（仅调试口实例，未清 Cookie）");
    }
    return true;
}

}  // namespace msc::cdp
