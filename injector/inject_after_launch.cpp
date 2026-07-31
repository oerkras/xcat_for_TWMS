#include "inject_after_launch.h"

#include "classic_loadlibrary.h"
#include "inject_log.h"

#include "../common/process_util.h"

#include <TlHelp32.h>

#include <cstdio>

namespace xcat::twms_inject {
namespace {

void LogLine(const LogFn& log, const std::wstring& line) {
    if (log) log(line);
}

}  // namespace

std::wstring DefaultPayloadDllBesideExe() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    std::wstring dir = ParentDirWithSlash(exe);
    return dir + L"XCat_data\\xcat.dll";
}

bool WaitForModuleByName(DWORD pid, const wchar_t* moduleName, int timeoutSec, LogFn log) {
    if (!pid || !moduleName || !moduleName[0]) return false;
    const DWORD deadline =
        GetTickCount() + static_cast<DWORD>((timeoutSec > 0 ? timeoutSec : 60) * 1000);
    for (;;) {
        if (!IsProcessAlive(pid)) {
            LogLine(log, L"[FAIL] 等待模块时游戏进程已退出");
            return false;
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{sizeof(me)};
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, moduleName) == 0) {
                        CloseHandle(snap);
                        return true;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
        if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
        Sleep(400);
    }
    LogLine(log, std::wstring(L"[FAIL] 等待模块超时：") + moduleName);
    return false;
}

Result InjectIntoClassic(const Options& opt, LogFn log) {
    Result out{};
    if (!opt.pid) {
        out.message = "pid 无效";
        LogLine(log, L"[FAIL] 注入：pid 无效");
        return out;
    }
    if (!IsProcessAlive(opt.pid)) {
        out.message = "目标进程不存在";
        LogLine(log, L"[FAIL] 注入：目标进程不存在");
        return out;
    }

    std::wstring dll = opt.dllPath.empty() ? DefaultPayloadDllBesideExe() : opt.dllPath;
    std::wstring abs;
    if (!ResolveAbsolutePath(dll, abs)) {
        out.message = "无法解析 DLL 路径";
        LogLine(log, L"[FAIL] 注入：无法解析 DLL " + dll);
        return out;
    }
    if (GetFileAttributesW(abs.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.message = "DLL 不存在（请先编 xcat_probe → bin/XCat_data/xcat.dll）";
        LogLine(log, L"[FAIL] 注入：未找到 " + abs);
        return out;
    }

    inject_log::RegisterFromDllPath(abs);

    LogLine(log, L"[…] 等待 GameAssembly.dll…");
    if (!WaitForModuleByName(opt.pid, L"GameAssembly.dll", opt.waitGameAssemblySec, log)) {
        out.message = "等待 GameAssembly.dll 超时";
        return out;
    }
    LogLine(log, L"[OK] 已见 GameAssembly.dll");
    if (opt.settleMs > 0) Sleep(static_cast<DWORD>(opt.settleMs));

    if (ClassicFindLoadedModuleBase(opt.pid, abs)) {
        out.ok = true;
        out.moduleLoaded = true;
        out.message = "payload already loaded";
        LogLine(log, L"[OK] payload 已在目标进程中，跳过重复注入");
        return out;
    }

    LogLine(log, L"[…] Classic LoadLibraryW 注入…");
    out.inject = ClassicLoadLibraryInject(opt.pid, abs);
    out.moduleLoaded = ClassicFindLoadedModuleBase(opt.pid, abs) != 0;
    out.ok = out.inject.ok && out.moduleLoaded;
    out.message = out.inject.message;
    if (out.ok) {
        LogLine(log, L"[OK] 注入成功 base=0x" +
                         [&]() {
                             wchar_t buf[32]{};
                             swprintf_s(buf, L"%llX",
                                        static_cast<unsigned long long>(out.inject.baseAddress));
                             return std::wstring(buf);
                         }());
    } else {
        LogLine(log, L"[FAIL] 注入失败：" + Utf8ToWide(out.message));
    }
    return out;
}

}  // namespace xcat::twms_inject
