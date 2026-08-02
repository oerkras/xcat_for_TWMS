#include "http_beanfun_login.h"

#include "http_client.h"
#include "ott_ticket_fetch.h"

#include <algorithm>
#include <vector>

namespace msc::launcher {
namespace {

constexpr wchar_t kBfLoginDefault[] =
    L"https://bfweb.hk.beanfun.com/beanfun_block/bflogin/default.aspx"
    L"?service=610076_T0"
    L"&url=https%3A%2F%2Fgalaxy.games.gamania.com%2Fwebapi%2Fview%2Flogin%2Fresult%2Fmstc%2Fghk";

constexpr wchar_t kLoginFormBase[] =
    L"https://login.hk.beanfun.com/login/id-pass_form_newBF.aspx";

constexpr wchar_t kReturnAspx[] =
    L"https://bfweb.hk.beanfun.com/beanfun_block/bflogin/return.aspx";

constexpr wchar_t kClassicMain[] = L"https://maplestoryclassic.beanfun.com/Main";

// 对齐 WebView：先拿 Galaxy init 会话 OTT（LOGIN_OTT_mstc），再 beanfun，再 POST result
constexpr wchar_t kGalaxyLoginMstc[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/mstc"
    L"?redirect_url=https://maplestoryclassic.beanfun.com/Main";

constexpr wchar_t kGalaxyResultGhk[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/result/mstc/ghk";

void Log(const HttpLoginLogFn& log, const std::wstring& line) {
    if (log) log(line);
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string ExtractOtp1(const std::wstring& url) {
    const std::string u = NarrowUtf8(url);
    std::string v = msc::http::RegexGroup1(u, "otp1=([^&]*)");
    if (v.empty()) v = msc::http::RegexGroup1(u, "skey=([^&]*)");
    return v;
}

bool LooksCaptcha(const std::string& html) {
    return msc::http::ContainsI(html, "g-recaptcha") ||
           msc::http::ContainsI(html, "recaptcha") ||
           msc::http::ContainsI(html, "驗證碼") ||
           msc::http::ContainsI(html, "验证码") ||
           msc::http::ContainsI(html, "CodeTextBox") ||
           msc::http::ContainsI(html, "samplecaptcha");
}

bool LooksDual(const std::string& html) {
    return msc::http::ContainsI(html, "請輸入雙重驗證碼") ||
           msc::http::ContainsI(html, "双重验证") ||
           msc::http::ContainsI(html, "雙重驗證");
}

// Galaxy /login/init/.../OTT: 是登录会话票，不能拿去 GetOneTimeWebInfo
bool IsGalaxyInitUrl(const std::wstring& u) {
    return u.find(L"/login/init/") != std::wstring::npos;
}

bool IsClassicMainOttUrl(const std::wstring& u) {
    return u.find(L"maplestoryclassic.beanfun.com") != std::wstring::npos &&
           (u.find(L"OTT=") != std::wstring::npos || u.find(L"OTT:") != std::wstring::npos);
}

// 换票用 OTT：OTT:<galaxyGameId>:Login:<token>，禁止引号/空白/HTML 脏尾巴
bool IsTicketOtt(const std::wstring& ott) {
    if (ott.size() < 16) return false;
    if (ott.rfind(L"OTT:", 0) != 0) return false;
    if (ott.find(L":Login:") == std::wstring::npos) return false;
    for (wchar_t c : ott) {
        if (c == L'\'' || c == L'"' || c == L'<' || c == L'>' || c == L'(' || c == L')' ||
            c == L' ' || c == L'\r' || c == L'\n' || c == L'\t' || c == L';' || c == L'&') {
            return false;
        }
    }
    // token 段应主要为可见安全字符
    const size_t login = ott.find(L":Login:");
    const std::wstring token = ott.substr(login + 7);
    if (token.size() < 4) return false;
    return true;
}

std::wstring RedactOttForLog(const std::wstring& ott) {
    if (ott.empty()) return L"(empty)";
    if (!IsTicketOtt(ott)) {
        const size_t n = (std::min)(ott.size(), size_t{48});
        return L"invalid shape len=" + std::to_wstring(ott.size()) + L" snip=" + ott.substr(0, n);
    }
    const size_t login = ott.find(L":Login:");
    const std::wstring head = ott.substr(0, login + 7);
    const std::wstring token = ott.substr(login + 7);
    if (token.size() <= 4) return head + L"***";
    return head + token.substr(0, 4) + L"*** len=" + std::to_wstring(token.size());
}

// 只从「可信 URL」抽换票 OTT；忽略 Galaxy init、HTML 脏片段
std::wstring TryOttFromUrl(const std::wstring& url) {
    if (url.empty() || IsGalaxyInitUrl(url)) return {};
    std::wstring ott = ExtractOttToken(url);
    if (!IsTicketOtt(ott)) return {};
    // 优先认官网 Main 回跳；其它 URL 仅当已是纯 OTT: 形态且非 init
    if (IsClassicMainOttUrl(url)) return ott;
    if (url.rfind(L"OTT:", 0) == 0) return ott;
    // Galaxy result?WebToken=… 跳转链上偶发带 OTT=；允许
    if (url.find(L"galaxy.games.gamania.com") != std::wstring::npos &&
        url.find(L"OTT=") != std::wstring::npos) {
        return ott;
    }
    return {};
}

std::wstring TryOttFromRedirects(const std::vector<std::wstring>& chain, const std::wstring& finalUrl) {
    for (const auto& u : chain) {
        std::wstring ott = TryOttFromUrl(u);
        if (!ott.empty()) return ott;
    }
    return TryOttFromUrl(finalUrl);
}

// HTML 里只匹配 Main?OTT= 或明确 OTT:944:Login: 安全字符
std::wstring FindTicketOttInText(const std::string& text) {
    // Main 回跳 URL
    const std::string mainUrl = msc::http::RegexGroup1(
        text, "(https?://maplestoryclassic\\.beanfun\\.com/[^\\s\"']*OTT[^\\s\"']*)");
    if (!mainUrl.empty()) {
        std::wstring ott = TryOttFromUrl(WidenUtf8(mainUrl));
        if (!ott.empty()) return ott;
    }
    const std::string raw =
        msc::http::RegexGroup1(text, "(OTT:[0-9]+:Login:[A-Za-z0-9_\\-+/=]+)");
    if (!raw.empty()) {
        std::wstring ott = WidenUtf8(raw);
        if (IsTicketOtt(ott)) return ott;
    }
    return {};
}

// Galaxy /login/init/mstc/OTT:... → 会话 OTT（给 result POST 用，不是最终换票票）
std::wstring ExtractLoginSessionOtt(const std::vector<std::wstring>& chain,
                                    const std::wstring& finalUrl) {
    auto fromUrl = [](const std::wstring& u) -> std::wstring {
        if (!IsGalaxyInitUrl(u)) return {};
        std::wstring ott = ExtractOttToken(u);
        if (ott.rfind(L"OTT:", 0) != 0) return {};
        if (ott.find(L":Login:") == std::wstring::npos) return {};
        return ott;
    };
    for (const auto& u : chain) {
        std::wstring ott = fromUrl(u);
        if (!ott.empty()) return ott;
    }
    return fromUrl(finalUrl);
}

// Galaxy result 页 JS：POST 同 URL，body {ott, fromSelf:true} → Results.Ott / Redirect
std::wstring ParseGalaxyResultOtt(const std::string& json) {
    // "Ott":"OTT:944:Login:..."
    std::string raw = msc::http::RegexGroup1(json, "\"Ott\"\\s*:\\s*\"(OTT:[^\"]+)\"");
    if (raw.empty()) raw = msc::http::RegexGroup1(json, "\"OTT\"\\s*:\\s*\"(OTT:[^\"]+)\"");
    if (raw.empty()) return {};
    std::wstring ott = WidenUtf8(raw);
    return IsTicketOtt(ott) ? ott : std::wstring{};
}

std::string JsonEscapeUtf8(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            continue;
        } else {
            o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

HttpLoginResult Fail(HttpLoginError e, const std::string& msg) {
    HttpLoginResult r;
    r.ok = false;
    r.error = e;
    r.message = msg;
    return r;
}

}  // namespace

const char* HttpLoginErrorName(HttpLoginError e) {
    switch (e) {
        case HttpLoginError::Ok:
            return "Ok";
        case HttpLoginError::BadInput:
            return "BadInput";
        case HttpLoginError::Network:
            return "Network";
        case HttpLoginError::Banned:
            return "Banned";
        case HttpLoginError::CaptchaRequired:
            return "CaptchaRequired";
        case HttpLoginError::DualVerifyRequired:
            return "DualVerifyRequired";
        case HttpLoginError::Protocol:
            return "Protocol";
        case HttpLoginError::OttMissing:
            return "OttMissing";
    }
    return "Unknown";
}

HttpLoginResult HttpBeanfunLoginToOtt(const std::wstring& user, const std::wstring& pass,
                                      HttpLoginLogFn log, int timeoutMs) {
    if (user.empty() || pass.empty()) {
        return Fail(HttpLoginError::BadInput, "账号或密码为空");
    }

    msc::http::Client http(timeoutMs);
    http.ClearCookies();

    // ① Galaxy 入口：拿到 /login/init/mstc/OTT:...（浏览器里进 localStorage LOGIN_OTT_mstc）
    Log(log, L"[http] GET Galaxy login/mstc 取会话 OTT…");
    auto gLogin = http.Get(kGalaxyLoginMstc, /*follow=*/true);
    std::wstring loginOtt = ExtractLoginSessionOtt(gLogin.redirectChain, gLogin.finalUrl);
    if (loginOtt.empty()) {
        // 有时停在 mstc 页；再跟一次 init 或扫 body
        loginOtt = ExtractLoginSessionOtt(gLogin.redirectChain, gLogin.location);
        if (loginOtt.empty()) {
            const std::string raw =
                msc::http::RegexGroup1(gLogin.body, "(OTT:[0-9]+:Login:[A-Za-z0-9_\\-+/=]+)");
            if (!raw.empty()) loginOtt = WidenUtf8(raw);
        }
    }
    if (loginOtt.empty()) {
        Log(log, L"[http] FAIL 未拿到 Galaxy sessionOtt；final=" + gLogin.finalUrl +
                     L" status=" + std::to_wstring(gLogin.status));
        return Fail(HttpLoginError::Protocol,
                    "未拿到 Galaxy 登录会话 OTT（/login/init/mstc/OTT:…）");
    }
    Log(log, L"[http] sessionOtt=" + RedactOttForLog(loginOtt) +
                 L" （Galaxy /login/init，仅供 result POST，非换票票）");

    Log(log, L"[http] GET bflogin default (service=610076_T0)…");
    auto r0 = http.Get(kBfLoginDefault, /*follow=*/true);
    if (!r0.ok && r0.status == 0) {
        return Fail(HttpLoginError::Network, r0.error.empty() ? "bflogin 网络失败" : r0.error);
    }
    if (msc::http::ContainsI(r0.body, "IP已自動被系統鎖定") ||
        msc::http::ContainsI(r0.body, "IP已自动被系统锁定")) {
        return Fail(HttpLoginError::Banned, "IP 已被 beanfun 锁定");
    }
    if (msc::http::ContainsI(r0.body, "目前無法在您的國家或地區瀏覽此網站")) {
        return Fail(HttpLoginError::Banned, "当前地区无法访问 beanfun HK");
    }

    std::string otp1;
    for (const auto& u : r0.redirectChain) {
        otp1 = ExtractOtp1(u);
        if (!otp1.empty()) break;
    }
    if (otp1.empty()) otp1 = ExtractOtp1(r0.finalUrl);
    if (otp1.empty()) otp1 = ExtractOtp1(r0.location);
    // 也扫 body
    if (otp1.empty()) {
        otp1 = msc::http::RegexGroup1(r0.body, "otp1=([^&\"']+)");
    }
    if (otp1.empty()) {
        Log(log, L"[http] FAIL 未解析到 otp1/session；final=" + r0.finalUrl);
        return Fail(HttpLoginError::Protocol, "未拿到登录 otp1（session key）");
    }
    Log(log, L"[http] otp1 已取得 len=" + std::to_wstring(otp1.size()));

    const std::wstring formUrl = std::wstring(kLoginFormBase) + L"?otp1=" + WidenUtf8(otp1);
    Log(log, L"[http] GET 账密表单…");
    auto r1 = http.Get(formUrl, true);
    if (!r1.ok && r1.body.empty()) {
        return Fail(HttpLoginError::Network, r1.error.empty() ? "表单页失败" : r1.error);
    }

    std::string viewstate = msc::http::HtmlAttr(r1.body, "__VIEWSTATE");
    std::string eventvalidation = msc::http::HtmlAttr(r1.body, "__EVENTVALIDATION");
    std::string viewstateGen = msc::http::HtmlAttr(r1.body, "__VIEWSTATEGENERATOR");
    if (viewstate.empty() || eventvalidation.empty()) {
        Log(log, L"[http] FAIL 缺 VIEWSTATE/EVENTVALIDATION");
        return Fail(HttpLoginError::Protocol, "登录表单字段解析失败");
    }
    if (LooksCaptcha(r1.body) && msc::http::ContainsI(r1.body, "data-sitekey")) {
        return Fail(HttpLoginError::CaptchaRequired,
                    "登录页要求验证码/reCAPTCHA：请用浏览器打开官网完成一次登录后再点一键，"
                    "或改用「HTTP优先」并安装 WebView2 以弹出登录窗");
    }

    Log(log, L"[http] POST 账密…");
    std::vector<std::pair<std::string, std::string>> fields = {
        {"__EVENTTARGET", ""},
        {"__EVENTARGUMENT", ""},
        {"__VIEWSTATEENCRYPTED", ""},
        {"__VIEWSTATE", viewstate},
        {"__VIEWSTATEGENERATOR", viewstateGen},
        {"__EVENTVALIDATION", eventvalidation},
        {"t_AccountID", NarrowUtf8(user)},
        {"t_Password", NarrowUtf8(pass)},
        {"token1", ""},
        {"g-recaptcha-response", ""},
        {"btn_login", "登入"},
        {"checkbox_remember_account", "on"},
    };
    auto r2 = http.PostForm(formUrl, fields, /*follow=*/true, formUrl);
    if (!r2.ok && r2.body.empty()) {
        return Fail(HttpLoginError::Network, r2.error.empty() ? "登录 POST 失败" : r2.error);
    }

    if (LooksDual(r2.body)) {
        return Fail(HttpLoginError::DualVerifyRequired,
                    "需要双重验证：请用浏览器打开官网完成验证后再点一键，"
                    "或改用「HTTP优先」+ WebView2 弹出登录窗手动完成");
    }
    if (LooksCaptcha(r2.body) &&
        (msc::http::ContainsI(r2.body, "驗證碼錯誤") || msc::http::ContainsI(r2.body, "请完成验证") ||
         msc::http::ContainsI(r2.body, "reCAPTCHA"))) {
        return Fail(HttpLoginError::CaptchaRequired,
                    "服务器要求验证码：请自行到网页登录一次（过验证码）后再重试一键启动");
    }

    const std::string errMsg =
        msc::http::RegexGroup1(r2.body, "ShowMsgBox\\('([^']*)'");
    if (!errMsg.empty()) {
        return Fail(HttpLoginError::Protocol, std::string("beanfun: ") + errMsg);
    }

    std::string akey = msc::http::RegexGroup1(r2.body, "AuthKey\\.value\\s*=\\s*\"([^\"]*)\"");
    if (akey.empty()) akey = msc::http::RegexGroup1(r2.body, "akey=([^&\"'\\s]+)");
    if (akey.empty()) {
        for (const auto& u : r2.redirectChain) {
            akey = msc::http::RegexGroup1(NarrowUtf8(u), "akey=([^&]+)");
            if (!akey.empty()) break;
        }
    }
    if (akey.empty()) {
        // 可能仍是验证码页
        if (LooksCaptcha(r2.body)) {
            return Fail(HttpLoginError::CaptchaRequired,
                        "登录后未拿到 AuthKey（疑似验证码）：请自行到网页登录一次后再重试");
        }
        Log(log, L"[http] FAIL 无 AuthKey；body_snip=" +
                     WidenUtf8(r2.body.substr(0, (std::min)(r2.body.size(), size_t{180}))));
        return Fail(HttpLoginError::Protocol, "登录成功响应中无 AuthKey");
    }
    Log(log, L"[http] AuthKey 已取得");

    Log(log, L"[http] POST return.aspx…");
    std::vector<std::pair<std::string, std::string>> retFields = {
        {"SessionKey", otp1},
        {"AuthKey", akey},
        {"ServiceCode", ""},
        {"ServiceRegion", ""},
        {"ServiceAccountSN", "0"},
    };
    auto r3 = http.PostForm(kReturnAspx, retFields, true, formUrl);
    if (!r3.ok && r3.body.empty()) {
        return Fail(HttpLoginError::Network, r3.error.empty() ? "return.aspx 失败" : r3.error);
    }

    const std::string bfToken = http.GetCookie("bfWebToken");
    std::string webToken = bfToken;
    if (webToken.empty()) {
        for (const auto& u : r3.redirectChain) {
            webToken = msc::http::RegexGroup1(NarrowUtf8(u), "[Ww]eb[Tt]oken=([^&]+)");
            if (!webToken.empty()) break;
        }
        if (webToken.empty()) {
            webToken = msc::http::RegexGroup1(NarrowUtf8(r3.finalUrl), "[Ww]eb[Tt]oken=([^&]+)");
        }
    }
    if (webToken.empty()) {
        Log(log, L"[http] WARN 无 bfWebToken/WebToken，继续追 OTT…");
    } else {
        Log(log, L"[http] WebToken ok len=" + std::to_wstring(webToken.size()));
    }

    // 对齐 Galaxy result 页 JS：POST 同 URL，带 sessionOtt → Results.Ott（ticketOtt）
    std::wstring ott = TryOttFromRedirects(r3.redirectChain, r3.finalUrl);
    if (ott.empty()) ott = FindTicketOttInText(r3.body);
    if (!ott.empty()) {
        Log(log, L"[http] ticketOtt(from redirect)=" + RedactOttForLog(ott));
    }

    if (ott.empty() && !webToken.empty()) {
        const std::wstring resultUrl =
            std::wstring(kGalaxyResultGhk) + L"?WebToken=" + WidenUtf8(webToken);

        // 可选 CSRF：先 GET result 页抽 meta csrf-token（无则仍直接 POST，与成功路径一致）
        std::string csrf;
        {
            Log(log, L"[http] GET Galaxy result 页（抽 CSRF，可空）…");
            auto rgGet = http.Get(resultUrl, /*follow=*/true);
            csrf = msc::http::RegexGroup1(
                rgGet.body, "name=[\"']csrf-token[\"']\\s+content=[\"']([^\"']+)[\"']");
            if (csrf.empty()) {
                csrf = msc::http::RegexGroup1(
                    rgGet.body, "content=[\"']([^\"']+)[\"']\\s+name=[\"']csrf-token[\"']");
            }
            if (csrf.empty()) {
                csrf = msc::http::RegexGroup1(rgGet.body, "\"csrfToken\"\\s*:\\s*\"([^\"]+)\"");
            }
            if (!csrf.empty()) {
                Log(log, L"[http] csrf-token 已捕获 len=" + std::to_wstring(csrf.size()));
            }
        }

        Log(log, L"[http] POST Galaxy result?WebToken= + sessionOtt…");
        std::string body = std::string("{\"ott\":\"") + JsonEscapeUtf8(NarrowUtf8(loginOtt)) +
                           "\",\"fromSelf\":true";
        if (!csrf.empty()) {
            body += ",\"_token\":\"" + JsonEscapeUtf8(csrf) + "\"";
        }
        body += "}";

        std::vector<std::pair<std::wstring, std::wstring>> extra;
        if (!csrf.empty()) {
            const std::wstring csrfW = WidenUtf8(csrf);
            extra.emplace_back(L"X-CSRF-TOKEN", csrfW);
            extra.emplace_back(L"RequestVerificationToken", csrfW);
        }
        auto rg = http.PostJson(resultUrl, body, /*follow=*/true, resultUrl, extra);
        ott = ParseGalaxyResultOtt(rg.body);
        if (ott.empty()) ott = TryOttFromRedirects(rg.redirectChain, rg.finalUrl);
        if (ott.empty()) ott = FindTicketOttInText(rg.body);
        // Redirect 字段偶发带官网
        if (ott.empty()) {
            const std::string redir =
                msc::http::RegexGroup1(rg.body, "\"Redirect\"\\s*:\\s*\"([^\"]+)\"");
            if (!redir.empty()) {
                std::string decoded = redir;
                // JSON 里 \/ → /
                for (size_t p = 0; (p = decoded.find("\\/", p)) != std::string::npos;) {
                    decoded.replace(p, 2, "/");
                }
                ott = TryOttFromUrl(WidenUtf8(decoded));
                if (ott.empty()) {
                    const std::string ottField =
                        msc::http::RegexGroup1(rg.body, "\"Ott\"\\s*:\\s*\"(OTT:[^\"]+)\"");
                    if (!ottField.empty()) {
                        const std::wstring combined =
                            WidenUtf8(decoded + (decoded.find('?') == std::string::npos ? "?" : "&") +
                                      "OTT=" + ottField);
                        ott = TryOttFromUrl(combined);
                        if (ott.empty() && IsTicketOtt(WidenUtf8(ottField))) {
                            ott = WidenUtf8(ottField);
                        }
                    }
                }
            }
        }
        if (ott.empty()) {
            const std::string rcode =
                msc::http::RegexGroup1(rg.body, "\"RCode\"\\s*:\\s*\"?(-?[0-9]+)");
            const std::string msg =
                msc::http::RegexGroup1(rg.body, "\"Message\"\\s*:\\s*\"([^\"]*)\"");
            const std::string err =
                msc::http::RegexGroup1(rg.body, "\"ErrorMessage\"\\s*:\\s*\"([^\"]*)\"");
            Log(log, L"[http] Galaxy result POST fail status=" + std::to_wstring(rg.status) +
                         L" ok=" + (rg.ok ? L"1" : L"0") + L" RCode=" +
                         WidenUtf8(rcode.empty() ? "-" : rcode) + L" Message=" +
                         WidenUtf8(msg.empty() ? (err.empty() ? "-" : err) : msg) +
                         L" err=" + WidenUtf8(rg.error.empty() ? "-" : rg.error) + L" body_snip=" +
                         WidenUtf8(rg.body.substr(0, (std::min)(rg.body.size(), size_t{280}))));
        } else {
            Log(log, L"[http] ticketOtt(from POST result)=" + RedactOttForLog(ott));
        }
    }

    if (ott.empty() && !webToken.empty()) {
        Log(log, L"[http] GET Classic Main?WebToken= 兜底…");
        const std::wstring withTok =
            std::wstring(kClassicMain) + L"?WebToken=" + WidenUtf8(webToken);
        auto rt = http.Get(withTok, true);
        ott = TryOttFromRedirects(rt.redirectChain, rt.finalUrl);
        if (ott.empty()) ott = FindTicketOttInText(rt.body);
        if (!ott.empty()) {
            Log(log, L"[http] ticketOtt(from Main?WebToken)=" + RedactOttForLog(ott));
        }
    }

    if (ott.empty() || !IsTicketOtt(ott)) {
        Log(log, L"[http] FAIL ticketOtt 未就绪 " + RedactOttForLog(ott) +
                     L"（sessionOtt 不能直接换票）");
        return Fail(HttpLoginError::OttMissing,
                    "登录成功但未捕获官网 Main?OTT=；可改用 WebView 或检查账号是否需验证码");
    }
    Log(log, L"[http] ticketOtt=" + RedactOttForLog(ott) + L" → GetOneTimeWebInfo…");

    TicketFetchOptions fo;
    fo.ott = ott;
    fo.timeoutMs = timeoutMs;
    auto fr = FetchGalaxyTicketFromOtt(fo);
    HttpLoginResult out;
    out.ott = ott;
    if (!fr.ok) {
        Log(log, L"[http] GetOneTimeWebInfo 失败：" + WidenUtf8(fr.message) + L" ticketOtt=" +
                     RedactOttForLog(ott));
        out.ok = false;
        out.error = HttpLoginError::Protocol;
        out.message = fr.message.empty() ? "GetOneTimeWebInfo 失败" : fr.message;
        return out;
    }
    out.ok = true;
    out.error = HttpLoginError::Ok;
    out.message = "ok";
    out.ticket = std::move(fr.ticket);
    out.ticketFilled = true;
    return out;
}

bool HttpIsTicketOtt(const std::wstring& ott) { return IsTicketOtt(ott); }

std::wstring HttpParseGalaxyResultOtt(const std::string& json) {
    return ParseGalaxyResultOtt(json);
}

std::wstring HttpRedactOtt(const std::wstring& ott) { return RedactOttForLog(ott); }

}  // namespace msc::launcher
