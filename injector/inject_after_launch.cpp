#include "inject_after_launch.h"

#include "classic_loadlibrary.h"
#include "inject_log.h"

#include "../common/process_util.h"

#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace xcat::twms_inject {
namespace {

void LogLine(const LogFn& log, const std::wstring& line) {
    if (log) log(line);
}

bool EnableDebugPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(tok);
    return ok == TRUE && err != ERROR_NOT_ALL_ASSIGNED;
}

HANDLE OpenTargetForModules(DWORD pid, DWORD* errOut) {
    HANDLE process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process) {
        if (errOut) *errOut = ERROR_SUCCESS;
        return process;
    }
    const DWORD first = GetLastError();
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process) {
        if (errOut) *errOut = ERROR_SUCCESS;
        return process;
    }
    if (errOut) *errOut = first ? first : GetLastError();
    return nullptr;
}

// Psapi 优先（Toolhelp 对部分游戏进程会一直 ACCESS_DENIED）。
bool ModuleLoadedByName(DWORD pid, const wchar_t* moduleName, DWORD* lastErr) {
    if (lastErr) *lastErr = ERROR_SUCCESS;

    DWORD openErr = ERROR_SUCCESS;
    HANDLE process = OpenTargetForModules(pid, &openErr);
    if (process) {
        constexpr int kEnumAttempts = 6;
        std::vector<HMODULE> modules(512);
        for (int attempt = 0; attempt < kEnumAttempts; ++attempt) {
            DWORD needed = 0;
            const DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
            if (!EnumProcessModulesEx(process, modules.data(), bytes, &needed, LIST_MODULES_ALL)) {
                const DWORD err = GetLastError();
                if (lastErr) *lastErr = err;
                // 目标仍在初始化时模块表可能读到一半 → ERROR_PARTIAL_COPY，属瞬时错误：
                // 这才是本重试预算的用途。Toolhelp 对游戏进程常 ACCESS_DENIED，别一有抖动就落到它。
                if (err == ERROR_PARTIAL_COPY && attempt + 1 < kEnumAttempts) {
                    Sleep(60);
                    continue;
                }
                break;
            }
            const size_t count = needed / sizeof(HMODULE);
            if (count > modules.size()) {
                modules.resize(count);
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                wchar_t baseName[MAX_PATH]{};
                if (GetModuleBaseNameW(process, modules[i], baseName, MAX_PATH) &&
                    _wcsicmp(baseName, moduleName) == 0) {
                    CloseHandle(process);
                    return true;
                }
            }
            CloseHandle(process);
            return false;
        }
        CloseHandle(process);
    } else if (lastErr) {
        *lastErr = openErr;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        if (lastErr && *lastErr == ERROR_SUCCESS) *lastErr = GetLastError();
        return false;
    }
    MODULEENTRY32W me{sizeof(me)};
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
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
    DWORD lastLog = 0;
    DWORD stickyErr = ERROR_SUCCESS;
    for (;;) {
        if (!IsProcessAlive(pid)) {
            LogLine(log, L"[FAIL] 等待模块时游戏进程已退出");
            return false;
        }
        DWORD err = ERROR_SUCCESS;
        if (ModuleLoadedByName(pid, moduleName, &err)) return true;
        if (err != ERROR_SUCCESS) stickyErr = err;

        const DWORD now = GetTickCount();
        if (lastLog == 0 || now - lastLog >= 5000) {
            lastLog = now;
            const int leftSec =
                (std::max)(0, static_cast<int>(deadline - now) / 1000);
            wchar_t buf[160]{};
            swprintf_s(buf, L"[…] 等待 %s… 剩余约%ds Open/枚举err=%lu", moduleName, leftSec,
                       static_cast<unsigned long>(stickyErr));
            LogLine(log, buf);
        }
        if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
        Sleep(400);
    }
    wchar_t fail[160]{};
    swprintf_s(fail, L"[FAIL] 等待模块超时：%s lastErr=%lu（若为5=拒绝访问，请以管理员运行 XCat）",
               moduleName, static_cast<unsigned long>(stickyErr));
    LogLine(log, fail);
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

    if (EnableDebugPrivilege()) {
        LogLine(log, L"[OK] 已启用 SeDebugPrivilege");
    } else {
        LogLine(log, L"[…] SeDebugPrivilege 未生效（未提权时常见；若 OpenProcess 失败请用管理员运行）");
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
