#pragma once

#include "msc_launch.h"

#include <functional>
#include <string>

namespace msc::launcher {

struct TicketFetchOptions {
    std::wstring baseUrl = L"https://maplestoryclassic.beanfun.com";
    std::wstring ott;  // 例如 OTT:944:Login:...（来自官网回跳 ?OTT=）
    int timeoutMs = 15000;
};

struct TicketFetchResult {
    bool ok = false;
    int httpStatus = 0;
    int apiCode = 0;  // 响应 code；成功期望 1
    std::string message;
    GalaxyTicket ticket;
};

using TicketFetchLogFn = std::function<void(const std::wstring& line)>;

// POST /api/Login/GetOneTimeWebInfo  body: {"OTT":"..."}
// 成功：code==1 且 data 含 userObjectID/userSessionToken/gid/galaxy_GameId/game
// 不落盘明文 token；调用方只应把 ticket 送进 Run()
TicketFetchResult FetchGalaxyTicketFromOtt(const TicketFetchOptions& opts);

// 橘子平台偶发登录成功后 code!=1：同 OTT 短间隔重试（网络 5xx/429 同样重试）。
// 解析失败 / Galaxy init URL 不重试。maxAttempts 含首次。
TicketFetchResult FetchGalaxyTicketFromOttWithRetry(const TicketFetchOptions& opts,
                                                    int maxAttempts = 4,
                                                    TicketFetchLogFn log = nullptr);

// 从任意含 OTT= 的 URL 或纯 OTT 字符串抽出票值
std::wstring ExtractOttToken(const std::wstring& urlOrOtt);

// Galaxy /login/init/... 会话票，不能拿去 GetOneTimeWebInfo
bool IsGalaxyLoginInitUrl(const std::wstring& s);

}  // namespace msc::launcher
