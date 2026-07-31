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
    HMODULE self = g_imageModule;
    if (!self) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GetBinDir), &self);
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    // DLL 在 bin/XCat_data/xcat.dll → GetBinDir = XCat_data/（PayloadLog 写 logs/x.jsonl）
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

HMODULE GetImageModule() { return g_imageModule; }

void SetImageModule(HMODULE mod) { g_imageModule = mod; }

}  // namespace x::runtime
