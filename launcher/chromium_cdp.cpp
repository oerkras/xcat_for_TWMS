#include "chromium_cdp.h"
#include "http_gamapass_login.h"
#include "msc_launch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace msc::cdp {
namespace {

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

void CopyFileTo(const std::wstring& src, const std::wstring& dst) {
    if (!FileExists(src) && !DirExists(src)) return;
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
}

// Chrome 136+：对「默认」User Data 静默忽略 --remote-debugging-port。
// 必须改用非默认目录；这里同步会话关键文件到 XCat 专用目录。
bool IsStandardChromiumUserData(const std::wstring& userData) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return false;
    const std::wstring chrome = std::wstring(localApp) + L"\\Google\\Chrome\\User Data";
    const std::wstring edge = std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data";
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
    return eq(userData, chrome) || eq(userData, edge) || eq(userData, chrome360x) ||
           eq(userData, chrome360);
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

bool PrepareCdpSafeUserData(const std::wstring& srcUserData, std::wstring& outCdpData, const LogFn& log) {
    outCdpData.clear();
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return false;
    outCdpData = std::wstring(localApp) + L"\\XCat\\GamaPassCdpProfile";
    if (!EnsureDir(std::wstring(localApp) + L"\\XCat") || !EnsureDir(outCdpData)) return false;

    // 同步会话相关到副本（只读源→副本，绝不反向写回日常 User Data）
    CopyFileTo(srcUserData + L"\\Local State", outCdpData + L"\\Local State");
    const std::wstring srcDef = srcUserData + L"\\Default";
    const std::wstring dstDef = outCdpData + L"\\Default";
    EnsureDir(dstDef);
    const wchar_t* files[] = {L"Preferences",         L"Secure Preferences", L"Login Data",
                              L"Login Data-journal",  L"Web Data",           L"Web Data-journal",
                              L"History",             L"Bookmarks",          L"Favicons",
                              L"Cookies",             L"Cookies-journal"};
    for (const wchar_t* f : files) {
        CopyFileTo(srcDef + L"\\" + f, dstDef + L"\\" + f);
    }
    MirrorTreeFile(srcDef + L"\\Network", dstDef + L"\\Network");
    // Local Storage / IndexedDB 可能承载 Gama Pass 会话
    MirrorTreeFile(srcDef + L"\\Local Storage", dstDef + L"\\Local Storage");
    MirrorTreeFile(srcDef + L"\\Session Storage", dstDef + L"\\Session Storage");
    MirrorTreeFile(srcDef + L"\\IndexedDB", dstDef + L"\\IndexedDB");

    LogLine(log, L"[cdp] Chromium 标准目录不能开调试口，已同步会话到：" + outCdpData);
    return DirExists(outCdpData);
}

}  // namespace

bool ResolvePreferredChromium(BrowserProfile& out, const LogFn& log) {
    out = {};
    if (!msc::launcher::HttpGamaPassPreferredBrowserExe(out.exe)) {
        LogLine(log, L"[cdp] 未找到 Chromium 系浏览器（Chrome / Edge / Chrome++ / 360）");
        return false;
    }
    if (!msc::launcher::HttpGamaPassResolveUserDataDir(out.exe, out.userData)) {
        // 官方 / 360 默认兜底
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
    // Chrome 136+：标准目录改走 CDP 安全副本（保留源路径仅用于同步）
    if (IsStandardChromiumUserData(out.userData)) {
        std::wstring cdpData;
        if (!PrepareCdpSafeUserData(out.userData, cdpData, log)) {
            LogLine(log, L"[cdp] 无法准备 CDP 专用 User Data 副本");
            return false;
        }
        out.userData = cdpData;
    }
    LogLine(log, L"[cdp] 浏览器=" + out.exe);
    LogLine(log, L"[cdp] UserData=" + out.userData);
    return true;
}

Session::Session() = default;

Session::~Session() { Close(); }

void Session::Close() {
    if (ws_) {
        WinHttpWebSocketClose((HINTERNET)ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle((HINTERNET)ws_);
        ws_ = nullptr;
    }
    if (connect_) {
        WinHttpCloseHandle((HINTERNET)connect_);
        connect_ = nullptr;
    }
    if (session_) {
        WinHttpCloseHandle((HINTERNET)session_);
        session_ = nullptr;
    }
    pageWsUrl_.clear();
    browserVersion_.clear();
}

bool Session::HttpGetLocal(int port, const wchar_t* path, std::string& body) {
    return HttpLocal(port, L"GET", path, body);
}

bool Session::HttpLocal(int port, const wchar_t* method, const wchar_t* path, std::string& body) {
    body.clear();
    HINTERNET ses = WinHttpOpen(L"xcat-cdp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 3000, 3000, 3000, 3000);
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return false;
    }
    HINTERNET req =
        WinHttpOpenRequest(con, method, path, nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    bool ok = false;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                  0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, 0);
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
            chunk.resize(read);
            body += chunk;
        }
        ok = !body.empty();
    }
    if (req) WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
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
        // /login、/error、oauth 半截：不优先附着（启动层会重新开 Galaxy）
        return 0;
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
    // WinHttpCrackUrl 不认 ws:// / wss://，先改成 http(s) 再解析
    std::wstring crackUrl = wsUrl;
    bool secure = false;
    if (crackUrl.rfind(L"ws://", 0) == 0) {
        crackUrl.replace(0, 5, L"http://");
    } else if (crackUrl.rfind(L"wss://", 0) == 0) {
        crackUrl.replace(0, 6, L"https://");
        secure = true;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(crackUrl.c_str(), 0, 0, &uc)) {
        LogLine(log, L"[cdp] CrackUrl 失败 url=" + wsUrl.substr(0, 120));
        return false;
    }
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) secure = true;
    HINTERNET ses = WinHttpOpen(L"xcat-cdp/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 10000, 10000, 30000, 30000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return false;
    }
    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        LogLine(log, L"[cdp] UPGRADE_TO_WEB_SOCKET 失败");
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        LogLine(log, L"[cdp] WebSocket 握手失败");
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!ws) {
        LogLine(log, L"[cdp] WebSocketCompleteUpgrade 失败");
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }
    // 会话与连接句柄须保持打开，直至 WebSocket 关闭
    session_ = ses;
    connect_ = con;
    ws_ = ws;
    return true;
}

bool Session::SendRecv(const std::string& method, const std::string& paramsJson,
                       std::string& resultJson, const LogFn& log) {
    resultJson.clear();
    if (!ws_) return false;
    const int id = nextId_++;
    std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\"";
    if (!paramsJson.empty()) msg += ",\"params\":" + paramsJson;
    msg += "}";
    if (WinHttpWebSocketSend((HINTERNET)ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)msg.data(), (DWORD)msg.size()) != ERROR_SUCCESS) {
        LogLine(log, L"[cdp] WebSocketSend 失败 method=" + Utf8ToWide(method));
        return false;
    }
    // 读到匹配 id 的响应（跳过事件）
    const DWORD t0 = GetTickCount();
    std::string buf;
    while (GetTickCount() - t0 < 15000) {
        BYTE chunk[8192];
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE typ{};
        DWORD st = WinHttpWebSocketReceive((HINTERNET)ws_, chunk, sizeof(chunk), &got, &typ);
        if (st != ERROR_SUCCESS) {
            LogLine(log, L"[cdp] WebSocketReceive 失败");
            return false;
        }
        if (typ == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
        buf.append(reinterpret_cast<char*>(chunk), got);
        if (typ == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            typ == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            continue;
        // complete message
        const std::string idPat = "\"id\":" + std::to_string(id);
        if (buf.find(idPat) != std::string::npos) {
            resultJson = std::move(buf);
            return resultJson.find("\"error\"") == std::string::npos ||
                   resultJson.find("\"result\"") != std::string::npos;
        }
        buf.clear();  // event, ignore
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

bool IsGamaPassCdpCopyDir(const std::wstring& userData) {
    const std::wstring n = NormalizePathKey(userData);
    return n.find(L"\\xcat\\gamapasscdpprofile") != std::wstring::npos;
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

}  // namespace

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
    if (profile.userData.empty()) return false;

    const bool usingCdpCopy = IsGamaPassCdpCopyDir(profile.userData);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return false;
    }

    DWORD hitPid = 0;
    std::wstring hitExe;
    do {
        const wchar_t* name = pe.szExeFile;
        const bool want = (_wcsicmp(name, L"chrome.exe") == 0) ||
                          (_wcsicmp(name, L"msedge.exe") == 0) ||
                          (_wcsicmp(name, L"chromium.exe") == 0) ||
                          (_wcsicmp(name, L"360chrome.exe") == 0) ||
                          (_wcsicmp(name, L"360chromex.exe") == 0) ||
                          (_wcsicmp(name, L"360se.exe") == 0) ||
                          (_wcsicmp(name, L"360browser.exe") == 0);
        if (!want) continue;

        const DWORD pid = pe.th32ProcessID;
        std::wstring img;
        if (!QueryProcessImagePath(pid, img)) continue;

        const std::wstring cmd = msc::launcher::GetProcessCommandLineW(pid);
        if (CmdHasRemoteDebugPort(cmd, debugPort)) continue;  // 已是我们要的调试实例

        std::wstring ud;
        const bool hasUd = ExtractUserDataDirFromCmd(cmd, ud);

        // 显式 --user-data-dir 命中目标目录 → 冲突
        if (hasUd && PathKeysEqual(ud, profile.userData)) {
            hitPid = pid;
            hitExe = img;
            break;
        }

        // 官方 Chrome/Edge 走 GamaPassCdpProfile 副本时：日常浏览器默认目录不算冲突
        if (usingCdpCopy) continue;

        // Chrome++ / 便携直用日常目录：同安装目录且未开本调试口 → 多半会锁 data_dir
        if (!SameBrowserInstall(img, profile.exe)) continue;
        if (hasUd && !PathKeysEqual(ud, profile.userData)) continue;  // 明确用了别的目录
        hitPid = pid;
        hitExe = img;
        break;
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);

    if (!hitPid) return false;

    const wchar_t* leaf = hitExe.c_str();
    const size_t slash = hitExe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) leaf = hitExe.c_str() + slash + 1;

    outHint = L"检测到浏览器已在运行且未开启调试口（";
    outHint += leaf;
    outHint += L" pid=";
    outHint += std::to_wstring(hitPid);
    outHint += L"）。请先自行关闭该浏览器窗口后再点一键启动"
               L"（程序不会自动结束进程；Chrome++ 需关掉日常窗口）。";
    LogLine(log, L"[cdp] 防呆：" + outHint);
    LogLine(log, L"[cdp] 冲突配置目录=" + profile.userData);
    return true;
}

bool Session::EnsureBrowser(const BrowserProfile& profile, int port, const LogFn& log,
                            std::wstring* outFailHint) {
    if (outFailHint) outFailHint->clear();
    if (Connect(port, log)) return true;
    if (profile.exe.empty() || profile.userData.empty()) {
        if (outFailHint) *outFailHint = L"未解析到浏览器可执行文件或配置目录";
        return false;
    }

    std::wstring busyHint;
    if (ProbeUserDataConflict(profile, port, busyHint, log)) {
        if (outFailHint) *outFailHint = busyHint;
        return false;
    }

    // 启动：只开调试口 + 空白页；Galaxy 由上层按需 Navigate 一次（禁止启动参数再带登录 URL 造成双开）
    // 清掉上次 park/about:blank 留下的 Session 恢复，避免再开出「空标签页」
    {
        const std::wstring def = profile.userData + L"\\Default";
        DeleteFileW((def + L"\\Current Session").c_str());
        DeleteFileW((def + L"\\Current Tabs").c_str());
        DeleteFileW((def + L"\\Last Session").c_str());
        DeleteFileW((def + L"\\Last Tabs").c_str());
    }
    std::wstring args = L"--remote-debugging-port=" + std::to_wstring(port) +
                        L" --remote-allow-origins=*"
                        L" --user-data-dir=\"" + profile.userData +
                        L"\" --no-first-run --no-default-browser-check"
                        L" --disable-session-crashed-bubble about:blank";
    LogLine(log, L"[cdp] 启动浏览器（带调试口）…");
    HINSTANCE sh =
        ShellExecuteW(nullptr, L"open", profile.exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(sh) <= 32) {
        std::wstring hint =
            L"启动浏览器失败。若浏览器已打开，请先自行关闭后再试（不会自动结束进程）。";
        LogLine(log, L"[cdp] " + hint);
        if (outFailHint) *outFailHint = hint;
        return false;
    }
    for (int i = 0; i < 40; ++i) {
        Sleep(500);
        if (Connect(port, log)) return true;
        // 中途若发现同目录被无调试口实例占用，提前给出防呆（例如用户又开了一份日常窗口）
        if ((i == 4 || i == 14 || i == 29) &&
            ProbeUserDataConflict(profile, port, busyHint, log)) {
            if (outFailHint) *outFailHint = busyHint;
            return false;
        }
    }
    if (ProbeUserDataConflict(profile, port, busyHint, log)) {
        if (outFailHint) *outFailHint = busyHint;
        return false;
    }
    const std::wstring hint =
        L"等待浏览器调试口超时。请先关掉占用中的 Chrome/Edge/Chrome++ 后再点启动"
        L"（程序不会自动结束进程）。";
    LogLine(log, L"[cdp] " + hint);
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
    const DWORD sendSt =
        WinHttpWebSocketSend((HINTERNET)ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)msg.data(), (DWORD)msg.size());
    if (sendSt != ERROR_SUCCESS) {
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
    Session s;
    std::string ver;
    return s.HttpGetLocal(port, L"/json/version", ver);
}

bool CloseRemoteBrowser(int port, const LogFn& log) {
    if (port <= 0) port = kDefaultRemoteDebugPort;

    // 先礼后兵：CDP 优雅关 → 再按调试口精确杀进程（Chrome++ 上 Browser.close 常关不干净）
    Session s;
    (void)s.QuitBrowser(port, log);
    Sleep(300);

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
