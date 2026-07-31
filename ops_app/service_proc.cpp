#include <winsock2.h>
#include <ws2tcpip.h>

#include "service_proc.h"

#include "../common/process_util.h"

#include <TlHelp32.h>
#include <iphlpapi.h>

#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace xcat::ops {
namespace {

std::string FormatWinError(DWORD code) {
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "win32=%lu", static_cast<unsigned long>(code));
    return buf;
}

}  // namespace

bool FindOnPath(const wchar_t* exeName, std::wstring& outPath) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = SearchPathW(nullptr, exeName, L".exe", MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return false;
    outPath.assign(buf);
    return true;
}

bool EnsureParentDir(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    const std::wstring dir = filePath.substr(0, slash);
    if (dir.empty()) return true;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ServiceProc::Start(const std::wstring& exe,
                        const std::wstring& args,
                        const std::wstring& cwd,
                        const std::wstring& logPath,
                        std::string& err) {
    Stop();
    err.clear();

    if (!EnsureParentDir(logPath)) {
        err = "cannot create log directory";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    logHandle = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        err = "cannot open log file: " + FormatWinError(GetLastError());
        logHandle = nullptr;
        return false;
    }
    SetFilePointer(logHandle, 0, nullptr, FILE_END);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = logHandle;
    si.hStdError = logHandle;

    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) {
        cmd.push_back(L' ');
        cmd += args;
    }
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    PROCESS_INFORMATION localPi{};
    const BOOL ok = CreateProcessW(
        exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &si, &localPi);
    if (!ok) {
        err = "CreateProcess failed: " + FormatWinError(GetLastError());
        CloseHandle(logHandle);
        logHandle = nullptr;
        return false;
    }

    pi = localPi;
    started = true;
    name = exe;
    return true;
}

void ServiceProc::Stop() {
    if (pi.hProcess) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 3000);
        }
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
    }
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = nullptr;
    }
    pi.dwProcessId = 0;
    pi.dwThreadId = 0;
    started = false;
    if (logHandle) {
        CloseHandle(logHandle);
        logHandle = nullptr;
    }
}

bool ServiceProc::IsRunning() const {
    if (!pi.hProcess) return false;
    return WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
}

bool RunPowerShellFile(const std::wstring& scriptPath,
                       const std::wstring& args,
                       const std::wstring& cwd,
                       DWORD timeoutMs,
                       std::string& err) {
    err.clear();
    std::wstring ps;
    if (!FindOnPath(L"powershell", ps)) {
        err = "powershell not found in PATH";
        return false;
    }

    std::wstring cmdArgs = L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";
    if (!args.empty()) {
        cmdArgs.push_back(L' ');
        cmdArgs += args;
    }

    ServiceProc helper;
    if (!helper.Start(ps, cmdArgs, cwd, cwd + L"\\artifacts\\ops_logs\\helper.log", err)) {
        // Fallback log next to script if artifacts path fails.
        const std::wstring fallbackLog = scriptPath + L".ops.log";
        if (!helper.Start(ps, cmdArgs, cwd, fallbackLog, err)) return false;
    }
    const DWORD wait = WaitForSingleObject(helper.pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        helper.Stop();
        err = "powershell timed out";
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(helper.pi.hProcess, &code);
    helper.Stop();
    if (code != 0) {
        err = "powershell exit " + std::to_string(code);
        return false;
    }
    return true;
}

void KillListenersOnPort(int port) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (size == 0) return;
    std::vector<char> buf(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) {
        return;
    }
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        // dwLocalPort is network byte order.
        const uint16_t localPort = static_cast<uint16_t>(((row.dwLocalPort >> 8) & 0xFF) |
                                                         ((row.dwLocalPort & 0xFF) << 8));
        if (localPort != static_cast<uint16_t>(port)) continue;
        const DWORD pid = row.dwOwningPid;
        if (pid == 0 || pid == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) continue;
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

void KillProcessesMatching(const wchar_t* exeNameSubstring, const wchar_t* commandLineNeedle) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        std::wstring name = pe.szExeFile;
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        std::wstring needle = exeNameSubstring ? exeNameSubstring : L"";
        for (auto& c : needle) c = static_cast<wchar_t>(towlower(c));
        if (name.find(needle) == std::wstring::npos) continue;

        // Command-line filter requires WMI/NtQuery; for node/caddy we also kill by port.
        // Here terminate matching exe names only when needle is empty or we accept name match.
        if (commandLineNeedle && commandLineNeedle[0]) {
            // Best-effort: skip generic name-only kill when a needle is required.
            // Port-based kill covers the common orphan case.
            continue;
        }

        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (!h) continue;
        TerminateProcess(h, 1);
        CloseHandle(h);
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    (void)commandLineNeedle;
}

}  // namespace xcat::ops
