#include "llm_http.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace xcat::app::anti_bot_ai {
namespace {

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port  = INTERNET_DEFAULT_HTTPS_PORT;
    bool         secure = true;
    std::wstring path;
};

std::wstring Utf8ToWide(const std::string& u8) {
    if (u8.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, out.data(), need);
    return out;
}

bool ParseUrl(const std::string& url, ParsedUrl& out) {
    out = {};
    std::string u = url;
    if (u.rfind("https://", 0) == 0) {
        out.secure = true;
        out.port   = INTERNET_DEFAULT_HTTPS_PORT;
        u          = u.substr(8);
    } else if (u.rfind("http://", 0) == 0) {
        out.secure = false;
        out.port   = INTERNET_DEFAULT_HTTP_PORT;
        u          = u.substr(7);
    } else {
        return false;
    }

    const size_t slash         = u.find('/');
    const std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    out.path = slash == std::string::npos ? L"/" : Utf8ToWide(u.substr(slash));

    const size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        out.host = Utf8ToWide(hostPort.substr(0, colon));
        try {
            out.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
        } catch (...) {
            return false;
        }
    } else {
        out.host = Utf8ToWide(hostPort);
    }
    return !out.host.empty();
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

struct HttpResult {
    DWORD       status = 0;
    std::string body;
    std::string err;
    DWORD       winErr = 0;  // 失败时的 GetLastError()（WinHTTP 错误码），0 表示无
};

// WinHTTP 错误码 → 可读标签，便于日志定位（区分超时/DNS/连接/TLS）。
const char* WinHttpErrLabel(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:                 return "TIMEOUT";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:       return "NAME_NOT_RESOLVED(DNS)";
    case ERROR_WINHTTP_CANNOT_CONNECT:          return "CANNOT_CONNECT";
    case ERROR_WINHTTP_CONNECTION_ERROR:        return "CONNECTION_ERROR";
    case ERROR_WINHTTP_SECURE_FAILURE:          return "SECURE_FAILURE(TLS)";
    case ERROR_WINHTTP_OPERATION_CANCELLED:     return "CANCELLED";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return "CLIENT_CERT_NEEDED";
    case ERROR_WINHTTP_INVALID_URL:             return "INVALID_URL";
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:     return "UNRECOGNIZED_SCHEME";
    case ERROR_WINHTTP_LOGIN_FAILURE:           return "LOGIN_FAILURE";
    case ERROR_WINHTTP_RESEND_REQUEST:          return "RESEND_REQUEST";
    case ERROR_WINHTTP_SHUTDOWN:                return "SHUTDOWN";
    default:                                    return "";
    }
}

// 是否为可重试的瞬时网络错误（超时/DNS/连接/重发/TLS 握手抖动）。
// 明确不重试：URL/scheme 非法、需客户端证书、登录失败等确定性错误。
bool IsTransientWinErr(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_RESEND_REQUEST:
    case ERROR_WINHTTP_SECURE_FAILURE:
        return true;
    default:
        return false;
    }
}

std::string FormatWinErr(const char* stage, DWORD code) {
    char buf[192]{};
    const char* label = WinHttpErrLabel(code);
    if (label[0])
        snprintf(buf, sizeof(buf), "%s (winhttp %lu %s)", stage, static_cast<unsigned long>(code),
                 label);
    else
        snprintf(buf, sizeof(buf), "%s (err %lu)", stage, static_cast<unsigned long>(code));
    return buf;
}

HttpResult WinHttpRequest(const ParsedUrl& url, const wchar_t* method, const std::wstring& path,
                          const std::wstring& extraHeaders, const std::string& body,
                          DWORD timeoutMs) {
    HttpResult result;
    HINTERNET hSes =
        WinHttpOpen(L"XCat-AntiBotAI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        result.winErr = GetLastError();
        result.err    = FormatWinErr("WinHttpOpen failed", result.winErr);
        return result;
    }
    WinHttpSetTimeouts(hSes, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs),
                       static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));

    HINTERNET hConn = WinHttpConnect(hSes, url.host.c_str(), url.port, 0);
    if (!hConn) {
        result.winErr = GetLastError();
        result.err    = FormatWinErr("WinHttpConnect failed", result.winErr);
        WinHttpCloseHandle(hSes);
        return result;
    }

    const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq =
        WinHttpOpenRequest(hConn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        result.winErr = GetLastError();
        result.err    = FormatWinErr("WinHttpOpenRequest failed", result.winErr);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSes);
        return result;
    }

    const void* reqBody = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
    const DWORD reqLen  = body.empty() ? 0 : static_cast<DWORD>(body.size());

    if (!WinHttpSendRequest(hReq,
                            extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                 : extraHeaders.c_str(),
                            extraHeaders.empty() ? 0 : static_cast<DWORD>(extraHeaders.size()),
                            const_cast<void*>(reqBody), reqLen, reqLen, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        result.winErr = GetLastError();
        result.err    = FormatWinErr("WinHttpSendRequest/ReceiveResponse failed", result.winErr);
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSes);
        return result;
    }

    DWORD status     = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    result.status = status;

    std::string resp;
    resp.reserve(32768);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        std::string buf(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf.data(), avail, &read) || read == 0) break;
        buf.resize(read);
        resp += buf;
        if (resp.size() > 512 * 1024) break;
    }
    result.body = std::move(resp);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSes);
    return result;
}

std::string ExtractJsonStringAfterKey(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos         = json.find(needle);
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

bool ParseDirectionFromText(const std::string& text, std::string& outDir) {
    auto upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    struct Cand {
        const char* token;
        const char* dir;
    };
    static const Cand kDirs[] = {{"UP", "UP"}, {"DOWN", "DOWN"}, {"LEFT", "LEFT"},
                                 {"RIGHT", "RIGHT"}};
    for (const auto& c : kDirs) {
        if (upper.find(c.token) != std::string::npos) {
            outDir = c.dir;
            return true;
        }
    }
    if (text.find("上") != std::string::npos) {
        outDir = "UP";
        return true;
    }
    if (text.find("下") != std::string::npos) {
        outDir = "DOWN";
        return true;
    }
    if (text.find("左") != std::string::npos) {
        outDir = "LEFT";
        return true;
    }
    if (text.find("右") != std::string::npos) {
        outDir = "RIGHT";
        return true;
    }
    return false;
}

std::wstring AuthHeader(const std::string& apiKey) {
    return L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
}

std::wstring ModelsPath(const std::wstring& chatPath) {
    static const std::wstring kSuffix = L"/chat/completions";
    if (chatPath.size() >= kSuffix.size() &&
        chatPath.compare(chatPath.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        return chatPath.substr(0, chatPath.size() - kSuffix.size()) + L"/models";
    }
    return L"/v1/models";
}

bool PostChatJson(const LlmEndpoint& ep, const std::string& bodyJson, std::string& outContent,
                  std::string& outErr) {
    outContent.clear();
    outErr.clear();
    if (ep.apiKey.empty()) {
        outErr = "API Key 为空";
        return false;
    }

    ParsedUrl url{};
    if (!ParseUrl(ep.apiUrl, url)) {
        outErr = "API URL 无效";
        return false;
    }

    const std::wstring headers =
        AuthHeader(ep.apiKey) + L"Accept: application/json\r\nContent-Type: application/json\r\n";

    // 有界重试：总预算严格锁定 ep.timeoutMs（默认 20s < 主程序 25s 等待窗口），
    // 瞬时网络错误（超时/DNS/连接/TLS 抖动）或 429/5xx 在剩余预算内重试；
    // 快速失败可立即自愈，持续故障则耗尽预算后干净失败，绝不越窗拖累主程序。
    const DWORD totalBudgetMs = ep.timeoutMs > 0 ? static_cast<DWORD>(ep.timeoutMs) : 20000u;
    const DWORD startTick     = GetTickCount();
    constexpr int   kMaxAttempts  = 3;
    constexpr DWORD kMinAttemptMs = 2000;  // 剩余预算不足此值则不再发起新尝试
    constexpr DWORD kBackoffMs    = 400;

    std::string lastErr;
    int         attemptsDone = 0;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const DWORD elapsed = GetTickCount() - startTick;
        const DWORD remain  = elapsed >= totalBudgetMs ? 0u : totalBudgetMs - elapsed;
        if (attempt > 1 && remain < kMinAttemptMs) break;
        const DWORD attemptTimeout = remain > 0 ? remain : kMinAttemptMs;

        ++attemptsDone;
        const HttpResult resp =
            WinHttpRequest(url, L"POST", url.path, headers, bodyJson, attemptTimeout);

        if (resp.err.empty() && resp.status == 200) {
            outContent = ExtractJsonStringAfterKey(resp.body, "content");
            if (!outContent.empty()) return true;
            // 200 但解析不到 message.content：绝不再把整段原始 JSON 当答案返回——
            // 上层测谎会对文本做数字扫描，原始 JSON 里的数字会被误判成选项导致盲答。
            // 视为失败（200 非瞬时，不重试），把少量 body 附给诊断。
            lastErr = "HTTP 200 但响应缺少 content 字段";
            if (!resp.body.empty()) {
                const size_t clip = std::min(resp.body.size(), size_t{180});
                lastErr += ": ";
                lastErr += resp.body.substr(0, clip);
            }
            break;
        }

        bool transient = false;
        if (!resp.err.empty()) {
            lastErr   = resp.err;
            transient = IsTransientWinErr(resp.winErr);
        } else {
            char buf[128]{};
            snprintf(buf, sizeof(buf), "HTTP %lu", static_cast<unsigned long>(resp.status));
            lastErr = buf;
            if (!resp.body.empty()) {
                const size_t clip = std::min(resp.body.size(), size_t{180});
                lastErr += ": ";
                lastErr += resp.body.substr(0, clip);
            }
            transient = (resp.status == 429) || (resp.status >= 500 && resp.status < 600);
        }

        if (!transient) break;

        const DWORD elapsed2 = GetTickCount() - startTick;
        if (attempt >= kMaxAttempts || elapsed2 + kBackoffMs + kMinAttemptMs > totalBudgetMs) break;
        Sleep(kBackoffMs);
    }

    if (lastErr.empty()) lastErr = "请求失败";
    if (attemptsDone > 1) {
        char suffix[32]{};
        snprintf(suffix, sizeof(suffix), " [tries=%d]", attemptsDone);
        lastErr += suffix;
    }
    outErr = lastErr;
    return false;
}

}  // namespace

std::string LlmBase64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || len == 0) return {};

    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
        i += 3;
    }
    if (i < len) {
        const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 63]);
        if (i + 1 < len) {
            const uint32_t n2 = n | (static_cast<uint32_t>(data[i + 1]) << 8);
            out.push_back(kTable[(n2 >> 12) & 63]);
            out.push_back(kTable[(n2 >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

bool LlmConnectTest(const LlmEndpoint& ep, std::string& outMsg) {
    ParsedUrl url{};
    if (!ParseUrl(ep.apiUrl, url)) {
        outMsg = "API URL 无效";
        return false;
    }

    const std::wstring headers =
        AuthHeader(ep.apiKey) + L"Accept: application/json\r\nContent-Type: application/json\r\n";
    const std::wstring modelsPath = ModelsPath(url.path);
    const HttpResult resp =
        WinHttpRequest(url, L"GET", modelsPath, headers, {}, static_cast<DWORD>(ep.timeoutMs));

    if (!resp.err.empty()) {
        outMsg = resp.err;
        return false;
    }

    char buf[256]{};
    snprintf(buf, sizeof(buf), "HTTP %lu", static_cast<unsigned long>(resp.status));
    if (resp.status == 200) {
        outMsg = std::string(buf) + " 连通/鉴权 OK";
        return true;
    }
    if (resp.status == 401) {
        outMsg = std::string(buf) + " 未授权（检查 API Key）";
        return false;
    }
    if (!resp.body.empty() && resp.body.size() < 200) {
        outMsg = std::string(buf) + " " + resp.body;
    } else {
        outMsg = buf;
    }
    return resp.status >= 200 && resp.status < 300;
}

bool LlmClassifyArrow(const LlmEndpoint& ep, const std::vector<uint8_t>& imgBytes,
                      const char* imgMime, std::string& outDir, std::string& outErr) {
    outDir.clear();
    outErr.clear();
    if (ep.apiKey.empty()) {
        outErr = "API Key 为空";
        return false;
    }
    if (imgBytes.empty()) {
        outErr = "图片为空";
        return false;
    }

    const char* mime          = (imgMime && imgMime[0]) ? imgMime : "image/bmp";
    const std::string b64     = LlmBase64Encode(imgBytes.data(), imgBytes.size());
    const std::string dataUrl = std::string("data:") + mime + ";base64," + b64;

    std::string body;
    body.reserve(b64.size() + 2048);
    body += "{";
    body += "\"model\":\"" + JsonEscape(ep.model) + "\",";
    body += "\"max_tokens\":8,\"temperature\":0,";
    body += "\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"";
    body += "Output exactly one of: UP DOWN LEFT RIGHT. No other text.\"},";
    body += "{\"role\":\"user\",\"content\":[";
    body += "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + JsonEscape(dataUrl) + "\"}},";
    body += "{\"type\":\"text\",\"text\":\"Which direction does this arrow point? Answer with one word.\"}";
    body += "]}";
    body += "]}";

    std::string content;
    if (!PostChatJson(ep, body, content, outErr)) return false;
    if (!ParseDirectionFromText(content, outDir)) {
        outErr = "无法从响应判定方向";
        return false;
    }
    return true;
}

bool LlmChatText(const LlmEndpoint& ep, const std::string& systemPrompt,
                 const std::string& userPrompt, bool thinking, std::string& outText,
                 std::string& outErr) {
    outText.clear();
    outErr.clear();

    std::string body;
    body.reserve(systemPrompt.size() + userPrompt.size() + 256);
    body += "{";
    body += "\"model\":\"" + JsonEscape(ep.model) + "\",";
    if (thinking) body += "\"thinking\":true,";
    body += "\"messages\":[";
    if (!systemPrompt.empty()) {
        body += "{\"role\":\"system\",\"content\":\"" + JsonEscape(systemPrompt) + "\"},";
    }
    body += "{\"role\":\"user\",\"content\":\"" + JsonEscape(userPrompt) + "\"}";
    body += "]}";

    return PostChatJson(ep, body, outText, outErr);
}

bool LlmAnswerQuiz(const LlmEndpoint& ep, const std::vector<uint8_t>& imgBytes, const char* imgMime,
                   const std::string& question, const std::vector<std::string>& options,
                   int& outIndex, std::string& outAnswer, std::string& outErr) {
    outIndex = 0;
    outAnswer.clear();
    outErr.clear();
    if (options.empty()) {
        outErr = "选项为空";
        return false;
    }

    // 选项块：1) xxx \n 2) yyy ...；要求模型只回命中选项的序号。
    std::string optsText;
    for (size_t i = 0; i < options.size(); ++i) {
        optsText += std::to_string(i + 1) + ") " + options[i] + "\\n";
    }

    std::string userText =
        "Look at the image and answer the question by choosing the single best option.\\n"
        "Question: " + JsonEscape(question) + "\\nOptions:\\n" + JsonEscape(optsText) +
        "Reply with ONLY the option number (1-" + std::to_string(options.size()) + ").";

    std::string body;
    body.reserve(userText.size() + 2048);
    body += "{";
    body += "\"model\":\"" + JsonEscape(ep.model) + "\",";
    body += "\"max_tokens\":8,\"temperature\":0,";
    body += "\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"";
    body += "You answer multiple-choice questions about an image. Reply with only the option number.\"},";
    body += "{\"role\":\"user\",\"content\":[";
    if (!imgBytes.empty()) {
        const char* mime          = (imgMime && imgMime[0]) ? imgMime : "image/bmp";
        const std::string b64     = LlmBase64Encode(imgBytes.data(), imgBytes.size());
        const std::string dataUrl = std::string("data:") + mime + ";base64," + b64;
        body += "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + JsonEscape(dataUrl) + "\"}},";
    }
    body += "{\"type\":\"text\",\"text\":\"" + userText + "\"}";
    body += "]}";
    body += "]}";

    std::string content;
    if (!PostChatJson(ep, body, content, outErr)) return false;

    // 抽取第一个落在 [1, N] 的数字作为命中选项。
    for (size_t i = 0; i < content.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(content[i]))) continue;
        size_t j = i;
        long v   = 0;
        while (j < content.size() && std::isdigit(static_cast<unsigned char>(content[j]))) {
            v = v * 10 + (content[j] - '0');
            ++j;
        }
        if (v >= 1 && v <= static_cast<long>(options.size())) {
            outIndex  = static_cast<int>(v);
            outAnswer = options[static_cast<size_t>(v - 1)];
            return true;
        }
        i = j;
    }

    // 数字未命中时回退：选项原文是否出现在响应里。
    for (size_t i = 0; i < options.size(); ++i) {
        if (!options[i].empty() && content.find(options[i]) != std::string::npos) {
            outIndex  = static_cast<int>(i + 1);
            outAnswer = options[i];
            return true;
        }
    }

    outErr = "无法从响应判定选项";
    return false;
}

bool LlmOcrCaptcha(const LlmEndpoint& ep, const std::vector<uint8_t>& imgBytes, const char* imgMime,
                   const std::string& hint, std::string& outAnswer, std::string& outErr) {
    outAnswer.clear();
    outErr.clear();
    if (imgBytes.empty()) {
        outErr = "无题图";
        return false;
    }

    const std::string prompt =
        hint.empty()
            ? "Read the distorted captcha text in the image. Reply with ONLY the captcha "
              "characters, no spaces or explanation."
            : ("Read the captcha in the image. Hint: " + JsonEscape(hint) +
               " Reply with ONLY the answer text.");

    std::string body;
    body += "{";
    body += "\"model\":\"" + JsonEscape(ep.model) + "\",";
    body += "\"max_tokens\":32,\"temperature\":0,";
    body += "\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"You OCR distorted captcha text. Reply with only "
            "the answer.\"},";
    body += "{\"role\":\"user\",\"content\":[";
    const char* mime = (imgMime && imgMime[0]) ? imgMime : "image/jpeg";
    const std::string b64 = LlmBase64Encode(imgBytes.data(), imgBytes.size());
    const std::string dataUrl = std::string("data:") + mime + ";base64," + b64;
    body += "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + JsonEscape(dataUrl) + "\"}},";
    body += "{\"type\":\"text\",\"text\":\"" + prompt + "\"}";
    body += "]}";
    body += "]}";

    std::string content;
    if (!PostChatJson(ep, body, content, outErr)) return false;

    // 取首行非空，去掉引号/空白
    std::string line;
    for (char c : content) {
        if (c == '\r') continue;
        if (c == '\n') break;
        line.push_back(c);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '"' || line.front() == '\''))
        line.erase(line.begin());
    while (!line.empty() && (line.back() == ' ' || line.back() == '"' || line.back() == '\'' ||
                             line.back() == '.'))
        line.pop_back();
    if (line.empty()) {
        outErr = "OCR 空响应";
        return false;
    }
    outAnswer = line;
    return true;
}

}  // namespace xcat::app::anti_bot_ai
