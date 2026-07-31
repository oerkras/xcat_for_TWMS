#include "ops_health.h"

#include <cstring>
#include <vector>

namespace xcat::ops {
namespace {

HealthResult HttpRequest(const wchar_t* method,
                         const wchar_t* host,
                         INTERNET_PORT port,
                         const wchar_t* path,
                         const char* bodyUtf8,
                         DWORD timeoutMs,
                         size_t maxBodyBytes) {
    HealthResult out;
    // NO_PROXY: system proxy can hang/fail localhost probes and freeze the UI tick.
    HINTERNET session = WinHttpOpen(L"xcat-ops/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = "WinHttpOpen failed";
        return out;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        out.error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return out;
    }

    HINTERNET request = WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        out.error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return out;
    }

    const DWORD bodyLen = bodyUtf8 ? static_cast<DWORD>(strlen(bodyUtf8)) : 0;
    const wchar_t* headers = bodyUtf8 ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    LPVOID bodyPtr = bodyUtf8 ? const_cast<char*>(bodyUtf8) : WINHTTP_NO_REQUEST_DATA;

    if (!WinHttpSendRequest(request, headers, headers ? static_cast<DWORD>(-1) : 0, bodyPtr, bodyLen, bodyLen,
                            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        out.error = "request failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return out;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (avail == 0) break;
        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
        body.append(chunk.data(), chunk.data() + read);
        if (body.size() > maxBodyBytes) break;
    }
    out.body = std::move(body);
    out.ok = (out.status >= 200 && out.status < 400);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
}

}  // namespace

HealthResult HttpGet(const wchar_t* host, INTERNET_PORT port, const wchar_t* path, DWORD timeoutMs,
                     size_t maxBodyBytes) {
    return HttpRequest(L"GET", host, port, path, nullptr, timeoutMs, maxBodyBytes);
}

HealthResult HttpPost(const wchar_t* host, INTERNET_PORT port, const wchar_t* path, const char* bodyUtf8,
                      DWORD timeoutMs, size_t maxBodyBytes) {
    return HttpRequest(L"POST", host, port, path, bodyUtf8 ? bodyUtf8 : "{}", timeoutMs, maxBodyBytes);
}

}  // namespace xcat::ops
