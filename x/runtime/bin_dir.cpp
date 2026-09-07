#include "bin_dir.h"

#include "../../common/process_util.h"
#include "../../common/xcat_log.h"

#include <cstdio>
#include <cstring>

namespace x::runtime {
namespace {

char g_binDir[1024]{};
HMODULE g_imageModule = nullptr;

void FallbackBinDirFromModule() {
    HMODULE self = GetImageModule();
    if (!self) return;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH) || !path[0]) return;
    // DLL 在 bin/rtcache/<payload.dll> → GetBinDir = 载荷目录/（PayloadLog 写 logs/x.jsonl）
    const std::string binDir = xcat::WideToUtf8(xcat::ParentDirWithSlash(path));
    strncpy_s(g_binDir, binDir.c_str(), _TRUNCATE);
}

}  // namespace

const char* GetBinDir() {
    if (!g_binDir[0]) FallbackBinDirFromModule();
    return g_binDir;
}

void GetLogFilePath(char* out, int size) {
    if (!out || size <= 0) return;
    const std::string path = xcat::log::paths::PayloadLog(GetBinDir());
    snprintf(out, size, "%s", path.c_str());
}

HMODULE GetImageModule() {
    if (g_imageModule) return g_imageModule;
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GetImageModule), &self) &&
        self) {
        g_imageModule = self;
    }
    return self;
}

void SetImageModule(HMODULE mod) { g_imageModule = mod; }

}  // namespace x::runtime
