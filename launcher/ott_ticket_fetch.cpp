#include "ott_ticket_fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace msc::launcher {
namespace {

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(static_cast<char>(c));
                }
        }
    }
    return o;
}

// 极简 JSON 字符串取值：找 "key" 后的 "value"（支持转义）；数字则读裸数字
bool JsonGetString(const std::string& json, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = 0;
    while (true) {
        pos = json.find(pat, pos);
        if (pos == std::string::npos) return false;
        size_t i = pos + pat.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') {
            pos += pat.size();
            continue;
        }
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size()) return false;
        if (json[i] == '"') {
            ++i;
            std::string val;
            while (i < json.size()) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    val.push_back(json[i + 1]);
                    i += 2;
                    continue;
                }
                if (json[i] == '"') break;
                val.push_back(json[i++]);
            }
            out = std::move(val);
            return true;
        }
        // number / bool / null → stringify digits
        size_t j = i;
        while (j < json.size() &&
               (std::isalnum(static_cast<unsigned char>(json[j])) || json[j] == '-' ||
                json[j] == '+' || json[j] == '.')) {
            ++j;
        }
        out = json.substr(i, j - i);
        return !out.empty();
    }
}

bool JsonGetInt(const std::string& json, const char* key, int& out) {
    std::string s;
    if (!JsonGetString(json, key, s)) return false;
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

// 取 "data": { ... } 对象子串（括号匹配）
bool JsonExtractObject(const std::string& json, const char* key, std::string& objOut) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    size_t i = pos + pat.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != ':') return false;
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != '{') return false;
    int depth = 0;
    const size_t start = i;
    for (; i < json.size(); ++i) {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') {
            --depth;
            if (depth == 0) {
                objOut = json.substr(start, i - start + 1);
                return true;
            }
        }
    }
    return false;
}

std::string PickField(const std::string& obj, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v;
        if (JsonGetString(obj, k, v) && !v.empty() && v != "null") return v;
    }
    return {};
}

TicketFetchResult FillFromJson(const std::string& json) {
    TicketFetchResult r;
    int code = 0;
    if (!JsonGetInt(json, "code", code) && !JsonGetInt(json, "Code", code)) {
        r.message = "响应缺少 code";
        return r;
    }
    r.apiCode = code;
    std::string msg;
    JsonGetString(json, "message", msg);
    if (msg.empty()) JsonGetString(json, "Message", msg);
    r.message = msg;

    if (code != 1) {
        r.message = msg.empty() ? ("api code=" + std::to_string(code)) : msg;
        return r;
    }

    std::string data;
    if (!JsonExtractObject(json, "data", data) && !JsonExtractObject(json, "Data", data)) {
        r.message = "成功响应缺少 data";
        return r;
    }

    GalaxyTicket t;
    t.userObjectId = WidenUtf8(PickField(data, {"userObjectID", "UserObjectID", "userObjectId"}));
    t.userSessionToken =
        WidenUtf8(PickField(data, {"userSessionToken", "UserSessionToken"}));
    t.gid = WidenUtf8(PickField(data, {"gid", "Gid", "GID"}));
    t.galaxyGameId =
        WidenUtf8(PickField(data, {"galaxy_GameId", "Galaxy_GameId", "galaxyGameId"}));
    t.ngmGameId = WidenUtf8(PickField(data, {"game", "Game"}));

    if (!TicketLooksUsable(t)) {
        r.message = "data 字段不完整或含非法字符";
        return r;
    }
    r.ticket = std::move(t);
    r.ok = true;
    return r;
}

}  // namespace

std::wstring ExtractOttToken(const std::wstring& urlOrOtt) {
    if (urlOrOtt.empty()) return {};

    auto trimTrail = [](std::wstring v) {
        while (!v.empty()) {
            const wchar_t c = v.back();
            if (c == L'/' || c == L'?' || c == L'&' || c == L'#' || c == L' ' || c == L'\r' ||
                c == L'\n') {
                v.pop_back();
                continue;
            }
            break;
        }
        return v;
    };

    // 纯 OTT
    if (urlOrOtt.rfind(L"OTT:", 0) == 0) return trimTrail(urlOrOtt);

    auto digQuery = [&](const std::wstring& key) -> std::wstring {
        const size_t pos = urlOrOtt.find(key);
        if (pos == std::wstring::npos) return {};
        size_t i = pos + key.size();
        std::wstring v;
        while (i < urlOrOtt.size() && urlOrOtt[i] != L'&' && urlOrOtt[i] != L'#' &&
               urlOrOtt[i] != L' ') {
            v.push_back(urlOrOtt[i++]);
        }
        std::wstring out;
        for (size_t j = 0; j < v.size(); ++j) {
            if (v[j] == L'%' && j + 2 < v.size()) {
                auto hex = [](wchar_t c) -> int {
                    if (c >= L'0' && c <= L'9') return c - L'0';
                    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                    return -1;
                };
                const int hi = hex(v[j + 1]);
                const int lo = hex(v[j + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<wchar_t>((hi << 4) | lo));
                    j += 2;
                    continue;
                }
            }
            out.push_back(v[j] == L'+' ? L' ' : v[j]);
        }
        return trimTrail(out);
    };

    // 1) 官网回跳：...?OTT=OTT:944:Login:...
    std::wstring v = digQuery(L"OTT=");
    if (v.empty()) v = digQuery(L"ott=");
    if (!v.empty()) return v;

    // 2) Galaxy 路径内嵌：.../login/init/mstc/OTT:944:Login:...?locale=
    const size_t pos = urlOrOtt.find(L"OTT:");
    if (pos != std::wstring::npos) {
        size_t i = pos;
        std::wstring out;
        while (i < urlOrOtt.size() && urlOrOtt[i] != L'?' && urlOrOtt[i] != L'&' &&
               urlOrOtt[i] != L'#' && urlOrOtt[i] != L' ' && urlOrOtt[i] != L'"' &&
               urlOrOtt[i] != L'\'' && urlOrOtt[i] != L')' && urlOrOtt[i] != L'<' &&
               urlOrOtt[i] != L'>' && urlOrOtt[i] != L';') {
            out.push_back(urlOrOtt[i++]);
        }
        return trimTrail(out);
    }
    return {};
}

bool IsGalaxyLoginInitUrl(const std::wstring& s) {
    if (s.find(L"/login/init/") != std::wstring::npos) return true;
    return s.find(L"galaxy.games.gamania.com") != std::wstring::npos &&
           s.find(L"/view/login/") != std::wstring::npos &&
           s.find(L"maplestoryclassic.beanfun.com") == std::wstring::npos;
}

TicketFetchResult FetchGalaxyTicketFromOtt(const TicketFetchOptions& opts) {
    TicketFetchResult r;
    if (IsGalaxyLoginInitUrl(opts.ott) &&
        opts.ott.find(L"maplestoryclassic.beanfun.com") == std::wstring::npos) {
        r.message =
            "这是 Galaxy 登录页/初始化 URL，不是换票用的回跳。"
            "请先完成登录，等浏览器跳到 maplestoryclassic.beanfun.com/Main?OTT=... 后再复制地址栏。";
        return r;
    }

    const std::wstring ott = ExtractOttToken(opts.ott);
    if (ott.empty() || ott.find(L'\'') != std::wstring::npos ||
        ott.find(L'"') != std::wstring::npos) {
        r.message =
            "未能解析 OTT。请粘贴：纯 OTT:944:Login:...，或回跳 URL（含 OTT= / 路径中的 OTT:）。";
        return r;
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
        static_cast<DWORD>(-1);
    std::wstring base = opts.baseUrl;
    if (!WinHttpCrackUrl(base.c_str(), 0, 0, &uc)) {
        r.message = "baseUrl 无效";
        return r;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (path.empty() || path.back() == L'/') {
        /* keep */
    }
    // 固定 API 路径
    path = L"/api/Login/GetOneTimeWebInfo";
    const INTERNET_PORT port = uc.nPort ? uc.nPort
                                        : (uc.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT
                                                                              : INTERNET_DEFAULT_HTTP_PORT);
    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    const std::string body =
        std::string("{\"OTT\":\"") + JsonEscape(NarrowUtf8(ott)) + "\"}";

    HINTERNET session =
        WinHttpOpen(L"msc-launcher/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.message = "WinHttpOpen 失败";
        return r;
    }
    WinHttpSetTimeouts(session, opts.timeoutMs, opts.timeoutMs, opts.timeoutMs, opts.timeoutMs);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        r.message = "WinHttpConnect 失败";
        return r;
    }

    HINTERNET request =
        WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        r.message = "WinHttpOpenRequest 失败";
        return r;
    }

    const wchar_t* hdrs = L"Content-Type: application/json; charset=UTF-8\r\n";
    const BOOL sent =
        WinHttpSendRequest(request, hdrs, static_cast<DWORD>(-1L), (LPVOID)body.data(),
                           static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        r.message = "HTTP 发送/接收失败 err=" + std::to_string(GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return r;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    r.httpStatus = static_cast<int>(status);

    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (!avail) break;
        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read) break;
        resp.append(chunk.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (resp.empty()) {
        r.message = "空响应 http=" + std::to_string(r.httpStatus);
        return r;
    }

    auto parsed = FillFromJson(resp);
    parsed.httpStatus = r.httpStatus;
    return parsed;
}

}  // namespace msc::launcher
