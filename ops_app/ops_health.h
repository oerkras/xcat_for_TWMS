#pragma once

#include <Windows.h>
#include <winhttp.h>

#include <string>

namespace xcat::ops {

struct HealthResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

HealthResult HttpGet(const wchar_t* host, INTERNET_PORT port, const wchar_t* path, DWORD timeoutMs = 2000,
                     size_t maxBodyBytes = 8192);
HealthResult HttpPost(const wchar_t* host, INTERNET_PORT port, const wchar_t* path,
                      const char* bodyUtf8, DWORD timeoutMs = 2000, size_t maxBodyBytes = 8192);

}  // namespace xcat::ops
