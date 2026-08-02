#pragma once

#include "inject_result.h"

#include <functional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace xcat::twms_inject {

struct Options {
    DWORD pid = 0;
    std::wstring dllPath;      // 空：旁路 bin/XCat_data/xcat.dll
    int waitGameAssemblySec = 120;
    // 仅缓冲：GA 出现 ≠ 托管就绪。真正的冷启门闩在 xcat.dll Bootstrap（等 MainPump）。
    int settleMs = 3000;
};

struct Result {
    bool ok = false;
    bool moduleLoaded = false;
    std::string message;
    InjectResult inject{};
};

using LogFn = std::function<void(const std::wstring& line)>;

std::wstring DefaultPayloadDllBesideExe();
bool WaitForModuleByName(DWORD pid, const wchar_t* moduleName, int timeoutSec, LogFn log);

// Classic LoadLibraryW：等 GameAssembly → 注入 → 校验模块在 LDR
Result InjectIntoClassic(const Options& opt, LogFn log = {});

}  // namespace xcat::twms_inject
