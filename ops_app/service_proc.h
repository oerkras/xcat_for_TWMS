#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace xcat::ops {

struct ServiceProc {
    PROCESS_INFORMATION pi{};
    HANDLE logHandle = nullptr;
    std::wstring name;
    bool started = false;

    ServiceProc() { ZeroMemory(&pi, sizeof(pi)); }
    ~ServiceProc() { Stop(); }

    ServiceProc(const ServiceProc&) = delete;
    ServiceProc& operator=(const ServiceProc&) = delete;

    bool Start(const std::wstring& exe,
               const std::wstring& args,
               const std::wstring& cwd,
               const std::wstring& logPath,
               std::string& err);
    void Stop();
    bool IsRunning() const;
    DWORD Pid() const { return pi.dwProcessId; }
};

bool FindOnPath(const wchar_t* exeName, std::wstring& outPath);
bool EnsureParentDir(const std::wstring& filePath);
bool RunPowerShellFile(const std::wstring& scriptPath,
                       const std::wstring& args,
                       const std::wstring& cwd,
                       DWORD timeoutMs,
                       std::string& err);
void KillListenersOnPort(int port);
void KillProcessesMatching(const wchar_t* exeNameSubstring, const wchar_t* commandLineNeedle);

}  // namespace xcat::ops
