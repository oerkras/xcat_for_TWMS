#include "classic_loadlibrary.h"

#include "inject_log.h"
#include "../common/process_util.h"

#include <Psapi.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace xcat {
namespace {

// kernel32.dll 在同一次 Windows 启动会话内各进程基址一致，可直接用本进程 GetProcAddress 结果作远程线程入口。
LPTHREAD_START_ROUTINE ResolveLoadLibraryW() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return nullptr;
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
}

LPTHREAD_START_ROUTINE ResolveFreeLibrary() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return nullptr;
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "FreeLibrary"));
}

using NtMapViewOfSectionFn = LONG(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
                                          PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
using NtUnmapViewOfSectionFn = LONG(NTAPI*)(HANDLE, PVOID);

NtMapViewOfSectionFn ResolveNtMapViewOfSection() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<NtMapViewOfSectionFn>(GetProcAddress(ntdll, "NtMapViewOfSection"));
}

NtUnmapViewOfSectionFn ResolveNtUnmapViewOfSection() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<NtUnmapViewOfSectionFn>(GetProcAddress(ntdll, "NtUnmapViewOfSection"));
}

const wchar_t* BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path.c_str() : path.c_str() + slash + 1;
}

uintptr_t FindLoadedModuleBaseViaPsapi(DWORD pid, const std::wstring& absoluteDll) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 0;

    uintptr_t byName = 0;
    const wchar_t* wantBase = BaseName(absoluteDll);
    std::vector<HMODULE> modules(512);
    for (int attempt = 0; attempt < 8; ++attempt) {
        DWORD needed = 0;
        const DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (EnumProcessModulesEx(process, modules.data(), bytes, &needed, LIST_MODULES_ALL)) {
            const size_t count = needed / sizeof(HMODULE);
            if (count > modules.size()) {
                modules.resize(count);
                continue;
            }

            for (size_t i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH]{};
                if (GetModuleFileNameExW(process, modules[i], path, MAX_PATH) &&
                    _wcsicmp(path, absoluteDll.c_str()) == 0) {
                    CloseHandle(process);
                    return reinterpret_cast<uintptr_t>(modules[i]);
                }

                wchar_t baseName[MAX_PATH]{};
                if (!byName && GetModuleBaseNameW(process, modules[i], baseName, MAX_PATH) &&
                    _wcsicmp(baseName, wantBase) == 0) {
                    byName = reinterpret_cast<uintptr_t>(modules[i]);
                }
            }

            CloseHandle(process);
            return byName;
        }

        Sleep(25);
    }

    CloseHandle(process);
    return byName;
}

uintptr_t FindLoadedModuleBaseImpl(DWORD pid, const std::wstring& absoluteDll) {
    if (const uintptr_t base = FindLoadedModuleBaseViaPsapi(pid, absoluteDll)) return base;

    // LoadLibrary 刚返回时模块表仍在变动，大进程的快照会偶发 ERROR_BAD_LENGTH；MSDN 要求重试。
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 16; ++attempt) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(25);
    }
    if (snap == INVALID_HANDLE_VALUE) return 0;

    uintptr_t byName = 0;
    const wchar_t* wantBase = BaseName(absoluteDll);
    MODULEENTRY32W me{sizeof(me)};
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szExePath, absoluteDll.c_str()) == 0) {
                CloseHandle(snap);
                return reinterpret_cast<uintptr_t>(me.modBaseAddr);
            }
            if (!byName && _wcsicmp(me.szModule, wantBase) == 0) {
                byName = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return byName;
}

struct RemoteString {
    void*  remote = nullptr;
    HANDLE section = nullptr;
    bool   sectionMapped = false;
};

void ReleaseRemoteString(HANDLE process, RemoteString& s) {
    if (!process || !s.remote) return;
    if (s.sectionMapped) {
        if (const auto ntUnmap = ResolveNtUnmapViewOfSection()) {
            ntUnmap(process, s.remote);
        }
        if (s.section) CloseHandle(s.section);
    } else {
        VirtualFreeEx(process, s.remote, 0, MEM_RELEASE);
    }
    s = {};
}

void AbandonRemoteStringView(RemoteString& s) {
    if (s.sectionMapped && s.section) CloseHandle(s.section);
    s = {};
}

bool PrepareRemoteStringWithWrite(HANDLE process, const std::wstring& text,
                                  RemoteString& out, DWORD& errorOut) {
    errorOut = ERROR_SUCCESS;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        errorOut = GetLastError();
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, text.c_str(), bytes, &written) ||
        written != bytes) {
        errorOut = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    out.remote = remote;
    return true;
}

bool PrepareRemoteStringWithSection(HANDLE process, const std::wstring& text,
                                    RemoteString& out, LONG& statusOut,
                                    DWORD& errorOut) {
    statusOut = 0;
    errorOut = ERROR_SUCCESS;

    const auto ntMap = ResolveNtMapViewOfSection();
    if (!ntMap) {
        errorOut = ERROR_PROC_NOT_FOUND;
        return false;
    }

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(bytes), nullptr);
    if (!section) {
        errorOut = GetLastError();
        return false;
    }

    void* local = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, bytes);
    if (!local) {
        errorOut = GetLastError();
        CloseHandle(section);
        return false;
    }
    std::memcpy(local, text.c_str(), bytes);

    PVOID remote = nullptr;
    SIZE_T viewSize = bytes;
    constexpr ULONG kViewUnmap = 2;
    statusOut = ntMap(section, process, &remote, 0, 0, nullptr, &viewSize, kViewUnmap, 0,
                      PAGE_READONLY);
    UnmapViewOfFile(local);

    if (statusOut < 0 || !remote) {
        CloseHandle(section);
        return false;
    }

    out.remote = remote;
    out.section = section;
    out.sectionMapped = true;
    return true;
}

}  // namespace

uintptr_t ClassicFindLoadedModuleBase(DWORD pid, const std::wstring& absoluteDll) {
    if (!pid || absoluteDll.empty() || !IsProcessAlive(pid)) return 0;
    return FindLoadedModuleBaseImpl(pid, absoluteDll);
}

bool ClassicWaitForModuleUnload(DWORD pid, const std::wstring& absoluteDll,
                                uintptr_t expectedBase, int timeoutMs) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 5000);
    for (;;) {
        if (!IsProcessAlive(pid)) return true;
        const uintptr_t base = ClassicFindLoadedModuleBase(pid, absoluteDll);
        if (!base) return true;
        if (expectedBase && base != expectedBase) return true;
        if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
        Sleep(100);
    }
    inject_log::Warn("Classic", "等待模块卸载超时 base=0x%llX dll=%s",
                     static_cast<unsigned long long>(expectedBase),
                     WideToUtf8(absoluteDll).c_str());
    return false;
}

bool ClassicFreeLibraryRemote(DWORD pid, uintptr_t moduleBase) {
    if (!pid || !moduleBase) return false;
    if (!IsProcessAlive(pid)) return false;

    const LPTHREAD_START_ROUTINE freeLibrary = ResolveFreeLibrary();
    if (!freeLibrary) {
        inject_log::Error("Classic", "kernel32!FreeLibrary 解析失败");
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION,
                                 FALSE, pid);
    if (!process) {
        inject_log::Error("Classic", "FreeLibrary OpenProcess 失败 pid=%lu err=%lu", pid,
                          GetLastError());
        return false;
    }

    HANDLE remoteThread =
        CreateRemoteThread(process, nullptr, 0, freeLibrary,
                           reinterpret_cast<LPVOID>(moduleBase), 0, nullptr);
    if (!remoteThread) {
        CloseHandle(process);
        inject_log::Error("Classic", "FreeLibrary CreateRemoteThread 失败 err=%lu", GetLastError());
        return false;
    }

    const DWORD wait = WaitForSingleObject(remoteThread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(remoteThread, &exitCode);
    CloseHandle(remoteThread);
    CloseHandle(process);

    const bool ok = wait == WAIT_OBJECT_0 && exitCode != 0;
    inject_log::Info("Classic", "FreeLibrary base=0x%llX pid=%lu ok=%d",
                     static_cast<unsigned long long>(moduleBase), pid, ok ? 1 : 0);
    return ok;
}

InjectResult ClassicLoadLibraryInject(DWORD pid, const std::wstring& absoluteDll) {
    InjectResult result{};

    if (!pid || absoluteDll.empty()) {
        result.message = "pid 或 dll 路径无效";
        return result;
    }
    if (!IsProcessAlive(pid)) {
        result.message = "目标进程不存在或已退出";
        return result;
    }

    inject_log::Info("Classic", "CreateRemoteThread(LoadLibraryW) pid=%lu dll=%s", pid,
                     WideToUtf8(absoluteDll).c_str());

    const LPTHREAD_START_ROUTINE loadLibraryW = ResolveLoadLibraryW();
    if (!loadLibraryW) {
        result.message = "本机 kernel32!LoadLibraryW 解析失败";
        inject_log::Error("Classic", "%s", result.message.c_str());
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        result.message = "OpenProcess 失败";
        inject_log::Error("Classic", "%s pid=%lu err=%lu", result.message.c_str(), pid,
                          GetLastError());
        return result;
    }

    RemoteString remotePath{};
    DWORD writeErr = ERROR_SUCCESS;
    if (!PrepareRemoteStringWithWrite(process, absoluteDll, remotePath, writeErr)) {
        inject_log::Warn("Classic", "WriteProcessMemory 路径写入失败 err=%lu，改用 section 映射",
                         writeErr);
        LONG mapStatus = 0;
        DWORD mapErr = ERROR_SUCCESS;
        if (!PrepareRemoteStringWithSection(process, absoluteDll, remotePath, mapStatus, mapErr)) {
            CloseHandle(process);
            result.message = "WriteProcessMemory/section 映射路径均失败";
            inject_log::Error("Classic", "%s writeErr=%lu mapErr=%lu nt=0x%08lX",
                              result.message.c_str(), writeErr, mapErr,
                              static_cast<unsigned long>(mapStatus));
            return result;
        }
        inject_log::Info("Classic", "section 映射远程路径 remote=0x%llX",
                         static_cast<unsigned long long>(
                             reinterpret_cast<uintptr_t>(remotePath.remote)));
    }

    HANDLE remoteThread =
        CreateRemoteThread(process, nullptr, 0, loadLibraryW, remotePath.remote, 0, nullptr);
    if (!remoteThread) {
        ReleaseRemoteString(process, remotePath);
        CloseHandle(process);
        result.message = "CreateRemoteThread 失败";
        inject_log::Error("Classic", "%s err=%lu", result.message.c_str(), GetLastError());
        return result;
    }

    const DWORD wait = WaitForSingleObject(remoteThread, 30000);
    DWORD threadExitCode = STILL_ACTIVE;
    GetExitCodeThread(remoteThread, &threadExitCode);

    if (wait != WAIT_OBJECT_0) {
        if (threadExitCode == STILL_ACTIVE) {
            AbandonRemoteStringView(remotePath);
            CloseHandle(remoteThread);
            CloseHandle(process);
            result.message = "LoadLibraryW 等待超时（远程线程仍在运行，保留远程路径内存）";
            inject_log::Error("Classic", "%s", result.message.c_str());
            return result;
        }
        CloseHandle(remoteThread);
        ReleaseRemoteString(process, remotePath);
        CloseHandle(process);
        result.message = "LoadLibraryW 等待超时";
        inject_log::Error("Classic", "%s exit=0x%08lX", result.message.c_str(), threadExitCode);
        return result;
    }

    CloseHandle(remoteThread);
    ReleaseRemoteString(process, remotePath);
    CloseHandle(process);

    const uintptr_t moduleBase = ClassicFindLoadedModuleBase(pid, absoluteDll);
    if (!moduleBase) {
        result.message = "LoadLibraryW 未找到已加载模块";
        inject_log::Error("Classic", "%s", result.message.c_str());
        return result;
    }

    result.ok = true;
    result.baseAddress = moduleBase;
    char buf[256]{};
    std::snprintf(buf, sizeof(buf),
                  "Classic LoadLibraryW OK base=0x%llX pid=%lu (CRT 远程线程+kernel32)",
                  static_cast<unsigned long long>(result.baseAddress), pid);
    result.message = buf;
    inject_log::Ok("Classic", "%s", result.message.c_str());
    return result;
}

}  // namespace xcat
