#pragma once

// 港服 beanfun HTTP 登录 → 经典版 OTT（无需 WebView2）

#include "msc_launch.h"

#include <functional>
#include <string>

namespace msc::launcher {

enum class HttpLoginError {
    Ok = 0,
    BadInput,
    Network,
    Banned,
    CaptchaRequired,   // 图形验证码 / reCAPTCHA
    DualVerifyRequired,  // 双重验证
    Protocol,          // 表单/字段/跳转不符合预期
    OttMissing,
};

struct HttpLoginResult {
    bool ok = false;
    HttpLoginError error = HttpLoginError::Protocol;
    std::string message;
    std::wstring ott;       // OTT:944:Login:... 或含 OTT 的 URL
    GalaxyTicket ticket;    // 若已换票则填好；否则调用方再 FetchGalaxyTicketFromOtt
    bool ticketFilled = false;
    // GamaPass CDP：落到 accounts.gamania.com/error（外层干净重开用；其它路径保持 false）
    bool accountsOauthError = false;
};

using HttpLoginLogFn = std::function<void(const std::wstring& line)>;

// service=610076_T0 + Galaxy result 回跳；遇验证码/双验返回对应 Error，不做打码。
HttpLoginResult HttpBeanfunLoginToOtt(const std::wstring& user, const std::wstring& pass,
                                      HttpLoginLogFn log = nullptr, int timeoutMs = 25000);

const char* HttpLoginErrorName(HttpLoginError e);

// 供冒烟 / 单测：换票 OTT 形态、Galaxy result JSON 解析、脱敏
bool HttpIsTicketOtt(const std::wstring& ott);
std::wstring HttpParseGalaxyResultOtt(const std::string& json);
std::wstring HttpRedactOtt(const std::wstring& ott);

}  // namespace msc::launcher
