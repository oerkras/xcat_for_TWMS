#pragma once

// 轻量 WinHTTP + Cookie 罐（跟随/禁止重定向可控）

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winhttp.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace msc::http {

struct Response {
    bool ok = false;
    int status = 0;
    std::string body;
    std::wstring finalUrl;     // 最后一次请求 URL（未跟随时仍是请求 URL）
    std::wstring location;     // Location 头（若有）
    std::string error;
    std::vector<std::wstring> redirectChain;
};

class Client {
public:
    explicit Client(int timeoutMs = 20000);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void SetUserAgent(const std::wstring& ua);
    void SetTimeoutMs(int ms);

    // followRedirects=false 时只拿 Location，不自动跳
    Response Get(const std::wstring& url, bool followRedirects = true);
    Response PostForm(const std::wstring& url,
                      const std::vector<std::pair<std::string, std::string>>& fields,
                      bool followRedirects = true,
                      const std::wstring& referer = {});
    Response PostJson(const std::wstring& url, const std::string& jsonBody,
                      bool followRedirects = true, const std::wstring& referer = {},
                      const std::vector<std::pair<std::wstring, std::wstring>>& extraHeaders = {});

    std::string GetCookie(const std::string& name) const;
    void ClearCookies();

private:
    Response Request(const std::wstring& method, const std::wstring& url, const std::string* body,
                     const wchar_t* contentType, bool followRedirects, const std::wstring& referer,
                     const std::vector<std::pair<std::wstring, std::wstring>>* extraHeaders = nullptr);
    void IngestSetCookie(const std::wstring& host, const std::wstring& setCookieLine);
    std::wstring CookieHeaderForUrl(const std::wstring& url) const;
    static bool CrackUrl(const std::wstring& url, std::wstring& host, std::wstring& path,
                         INTERNET_PORT& port, bool& https);
    static std::wstring JoinUrl(const std::wstring& base, const std::wstring& loc);

    HINTERNET session_ = nullptr;
    int timeoutMs_ = 20000;
    std::wstring userAgent_ =
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    // host(lower) -> name -> value
    std::map<std::wstring, std::map<std::string, std::string>> cookies_;
};

std::string UrlEncodeForm(const std::string& s);
std::string HtmlAttr(const std::string& html, const char* id);
std::string RegexGroup1(const std::string& text, const char* pattern);
bool ContainsI(const std::string& hay, const char* needle);

}  // namespace msc::http
