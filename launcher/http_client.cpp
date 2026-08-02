#include "http_client.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace msc::http {
namespace {

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::wstring ParentHostKey(const std::wstring& host) {
    // cookie domain matching: exact host key only (simple jar)
    return ToLower(host);
}

}  // namespace

std::string UrlEncodeForm(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            o.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            o.push_back('+');
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 0xF]);
        }
    }
    return o;
}

std::string HtmlAttr(const std::string& html, const char* id) {
    // id="__VIEWSTATE" value="..."
    const std::string needle = std::string("id=\"") + id + "\"";
    size_t pos = html.find(needle);
    if (pos == std::string::npos) {
        // allow flexible whitespace
        std::regex re(std::string("id\\s*=\\s*\"") + id + "\"[^>]*value\\s*=\\s*\"([^\"]*)\"",
                      std::regex::icase);
        std::smatch m;
        if (std::regex_search(html, m, re) && m.size() > 1) return m[1].str();
        return {};
    }
    const size_t v = html.find("value=\"", pos);
    if (v == std::string::npos || v > pos + 200) return {};
    const size_t start = v + 7;
    const size_t end = html.find('"', start);
    if (end == std::string::npos) return {};
    return html.substr(start, end - start);
}

std::string RegexGroup1(const std::string& text, const char* pattern) {
    try {
        std::regex re(pattern);
        std::smatch m;
        if (std::regex_search(text, m, re) && m.size() > 1) return m[1].str();
    } catch (...) {
    }
    return {};
}

bool ContainsI(const std::string& hay, const char* needle) {
    if (!needle || !needle[0]) return false;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string h = hay;
    std::string n = needle;
    std::transform(h.begin(), h.end(), h.begin(), lower);
    std::transform(n.begin(), n.end(), n.begin(), lower);
    return h.find(n) != std::string::npos;
}

Client::Client(int timeoutMs) : timeoutMs_(timeoutMs) {
    session_ = WinHttpOpen(userAgent_.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        WinHttpSetTimeouts(session_, timeoutMs_, timeoutMs_, timeoutMs_, timeoutMs_);
    }
}

Client::~Client() {
    if (session_) WinHttpCloseHandle(session_);
}

void Client::SetUserAgent(const std::wstring& ua) {
    userAgent_ = ua;
    if (session_) {
        WinHttpCloseHandle(session_);
        session_ = WinHttpOpen(userAgent_.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) {
            WinHttpSetTimeouts(session_, timeoutMs_, timeoutMs_, timeoutMs_, timeoutMs_);
        }
    }
}

void Client::SetTimeoutMs(int ms) {
    timeoutMs_ = ms;
    if (session_) WinHttpSetTimeouts(session_, ms, ms, ms, ms);
}

void Client::ClearCookies() { cookies_.clear(); }

std::string Client::GetCookie(const std::string& name) const {
    for (const auto& hostPair : cookies_) {
        auto it = hostPair.second.find(name);
        if (it != hostPair.second.end()) return it->second;
    }
    return {};
}

bool Client::CrackUrl(const std::wstring& url, std::wstring& host, std::wstring& path,
                      INTERNET_PORT& port, bool& https) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
        static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    port = uc.nPort ? uc.nPort
                    : (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    return true;
}

std::wstring Client::JoinUrl(const std::wstring& base, const std::wstring& loc) {
    if (loc.empty()) return {};
    if (loc.rfind(L"http://", 0) == 0 || loc.rfind(L"https://", 0) == 0) return loc;
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!CrackUrl(base, host, path, port, https)) return loc;
    const std::wstring scheme = https ? L"https://" : L"http://";
    if (!loc.empty() && loc[0] == L'/') return scheme + host + loc;
    const size_t slash = path.find_last_of(L'/');
    const std::wstring dir = (slash == std::wstring::npos) ? L"/" : path.substr(0, slash + 1);
    return scheme + host + dir + loc;
}

void Client::IngestSetCookie(const std::wstring& host, const std::wstring& setCookieLine) {
    // name=value; Path=...; Domain=...
    const auto semi = setCookieLine.find(L';');
    const std::wstring nv =
        semi == std::wstring::npos ? setCookieLine : setCookieLine.substr(0, semi);
    const auto eq = nv.find(L'=');
    if (eq == std::wstring::npos || eq == 0) return;
    std::string name = Narrow(nv.substr(0, eq));
    std::string value = Narrow(nv.substr(eq + 1));
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    std::wstring domain = ParentHostKey(host);
    const std::wstring lower = ToLower(setCookieLine);
    const size_t dpos = lower.find(L"domain=");
    if (dpos != std::wstring::npos) {
        size_t start = dpos + 7;
        while (start < setCookieLine.size() &&
               (setCookieLine[start] == L' ' || setCookieLine[start] == L'\t'))
            ++start;
        size_t end = start;
        while (end < setCookieLine.size() && setCookieLine[end] != L';') ++end;
        std::wstring dom = setCookieLine.substr(start, end - start);
        if (!dom.empty() && dom[0] == L'.') dom.erase(0, 1);
        if (!dom.empty()) domain = ToLower(dom);
    }
    cookies_[domain][name] = value;
}

std::wstring Client::CookieHeaderForUrl(const std::wstring& url) const {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!CrackUrl(url, host, path, port, https)) return {};
    const std::wstring hostL = ToLower(host);
    std::map<std::string, std::string> merged;
    for (const auto& hp : cookies_) {
        const std::wstring& dom = hp.first;
        if (hostL == dom ||
            (hostL.size() > dom.size() && hostL.compare(hostL.size() - dom.size(), dom.size(),
                                                        dom) == 0 &&
             hostL[hostL.size() - dom.size() - 1] == L'.')) {
            for (const auto& kv : hp.second) merged[kv.first] = kv.second;
        }
    }
    if (merged.empty()) return {};
    std::wstring out;
    for (const auto& kv : merged) {
        if (!out.empty()) out += L"; ";
        out += Widen(kv.first);
        out += L'=';
        out += Widen(kv.second);
    }
    return out;
}

Response Client::Get(const std::wstring& url, bool followRedirects) {
    return Request(L"GET", url, nullptr, nullptr, followRedirects, {});
}

Response Client::PostForm(const std::wstring& url,
                          const std::vector<std::pair<std::string, std::string>>& fields,
                          bool followRedirects, const std::wstring& referer) {
    std::ostringstream oss;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) oss << '&';
        oss << UrlEncodeForm(fields[i].first) << '=' << UrlEncodeForm(fields[i].second);
    }
    const std::string body = oss.str();
    return Request(L"POST", url, &body, L"application/x-www-form-urlencoded", followRedirects,
                   referer);
}

Response Client::PostJson(const std::wstring& url, const std::string& jsonBody,
                          bool followRedirects, const std::wstring& referer,
                          const std::vector<std::pair<std::wstring, std::wstring>>& extraHeaders) {
    return Request(L"POST", url, &jsonBody, L"application/json", followRedirects, referer,
                   extraHeaders.empty() ? nullptr : &extraHeaders);
}

Response Client::Request(const std::wstring& method, const std::wstring& url,
                         const std::string* body, const wchar_t* contentType, bool followRedirects,
                         const std::wstring& referer,
                         const std::vector<std::pair<std::wstring, std::wstring>>* extraHeaders) {
    Response r;
    if (!session_) {
        r.error = "WinHttpOpen failed";
        return r;
    }

    std::wstring cur = url;
    std::wstring curMethod = method;
    const std::string* curBody = body;
    const wchar_t* curCt = contentType;
    std::wstring curReferer = referer;

    for (int hop = 0; hop < 12; ++hop) {
        r.redirectChain.push_back(cur);
        std::wstring host, path;
        INTERNET_PORT port = 0;
        bool https = false;
        if (!CrackUrl(cur, host, path, port, https)) {
            r.error = "bad url";
            return r;
        }

        HINTERNET connect = WinHttpConnect(session_, host.c_str(), port, 0);
        if (!connect) {
            r.error = "WinHttpConnect failed err=" + std::to_string(GetLastError());
            return r;
        }
        const DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request =
            WinHttpOpenRequest(connect, curMethod.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            WinHttpCloseHandle(connect);
            r.error = "WinHttpOpenRequest failed";
            return r;
        }

        DWORD opt = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));

        std::wstring headers =
            L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
            L"Accept-Language: zh-HK,zh;q=0.9,en;q=0.8\r\n";
        if (curCt && curBody) {
            headers += L"Content-Type: ";
            headers += curCt;
            headers += L"\r\n";
        }
        if (!curReferer.empty()) {
            headers += L"Referer: ";
            headers += curReferer;
            headers += L"\r\n";
        }
        if (extraHeaders) {
            for (const auto& h : *extraHeaders) {
                if (h.first.empty()) continue;
                headers += h.first;
                headers += L": ";
                headers += h.second;
                headers += L"\r\n";
            }
        }
        const std::wstring cookie = CookieHeaderForUrl(cur);
        if (!cookie.empty()) {
            headers += L"Cookie: ";
            headers += cookie;
            headers += L"\r\n";
        }

        LPVOID bodyPtr = curBody ? (LPVOID)curBody->data() : WINHTTP_NO_REQUEST_DATA;
        DWORD bodyLen = curBody ? static_cast<DWORD>(curBody->size()) : 0;

        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), bodyPtr, bodyLen,
                                bodyLen, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            r.error = "HTTP send/recv failed err=" + std::to_string(GetLastError());
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            return r;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);

        // Parse Set-Cookie from raw headers
        {
            DWORD need = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &need, WINHTTP_NO_HEADER_INDEX);
            if (need > 2) {
                std::vector<wchar_t> raw(need / sizeof(wchar_t) + 1);
                DWORD sz = need;
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                        WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &sz,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    std::wstring hdrs(raw.data());
                    size_t pos = 0;
                    const std::wstring key = L"Set-Cookie:";
                    while ((pos = hdrs.find(key, pos)) != std::wstring::npos) {
                        pos += key.size();
                        while (pos < hdrs.size() && (hdrs[pos] == L' ' || hdrs[pos] == L'\t')) ++pos;
                        size_t end = hdrs.find(L"\r\n", pos);
                        if (end == std::wstring::npos) end = hdrs.size();
                        IngestSetCookie(host, hdrs.substr(pos, end - pos));
                        pos = end;
                    }
                }
            }
        }

        wchar_t locBuf[2048]{};
        DWORD locSz = sizeof(locBuf);
        std::wstring location;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, locBuf,
                                &locSz, WINHTTP_NO_HEADER_INDEX)) {
            location = locBuf;
            r.location = location;
        }

        std::string bodyAcc;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || !avail) break;
            std::vector<char> chunk(avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read) break;
            bodyAcc.append(chunk.data(), read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);

        r.finalUrl = cur;
        r.body = std::move(bodyAcc);

        const bool isRedirect =
            (status == 301 || status == 302 || status == 303 || status == 307 || status == 308);
        if (!isRedirect || location.empty() || !followRedirects) {
            r.ok = (status >= 200 && status < 400) || isRedirect;
            return r;
        }

        curReferer = cur;
        cur = JoinUrl(cur, location);
        curMethod = L"GET";
        curBody = nullptr;
        curCt = nullptr;
    }

    r.error = "too many redirects";
    return r;
}

}  // namespace msc::http
