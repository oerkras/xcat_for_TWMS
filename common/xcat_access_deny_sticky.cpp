#include "xcat_access_deny_sticky.h"

#include "process_util.h"

#include <Windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>

namespace xcat {
namespace {

std::string MachineStickyPath() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &base)) ||
        !base) {
        return {};
    }
    std::wstring w = base;
    CoTaskMemFree(base);
    if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w.push_back(L'\\');
    w += L"{E4B7C2A9-1F8D-4E3A-9C6B-7A2D5F1E0C8B}\\svc.dat";
    return WideToUtf8(w);
}

std::string LocalStickyPath(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return {};
    std::string path = payloadBinDir;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "state\\wc.cache";
    return path;
}

std::string LegacyMachineStickyPath() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &base)) ||
        !base) {
        return {};
    }
    std::wstring w = base;
    CoTaskMemFree(base);
    if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w.push_back(L'\\');
    w += L"XCatTWMS\\access_deny.json";
    return WideToUtf8(w);
}

std::string LegacyLocalStickyPath(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return {};
    std::string path = payloadBinDir;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "state\\access_deny.json";
    return path;
}

bool FileLooksLikeAccessDeny(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char buf[256]{};
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return false;
    // 混淆包：魔数 WC1\0
    if (n >= 4 && buf[0] == 'W' && buf[1] == 'C' && buf[2] == '1' && buf[3] == '\0') return true;
    // 旧明文
    const std::string raw(buf, n);
    return raw.find("\"denied\":true") != std::string::npos ||
           raw.find("\"denied\": true") != std::string::npos;
}

}  // namespace

bool AccessDenyStickyPresent(const char* payloadBinDir) {
    const std::string paths[] = {
        MachineStickyPath(),
        LocalStickyPath(payloadBinDir),
        LegacyMachineStickyPath(),
        LegacyLocalStickyPath(payloadBinDir),
    };
    for (const auto& p : paths) {
        if (FileLooksLikeAccessDeny(p)) return true;
    }
    return false;
}

}  // namespace xcat
