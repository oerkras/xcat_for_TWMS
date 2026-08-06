#include "process_util.h"

#include <TlHelp32.h>
#include <cerrno>
#include <share.h>

namespace xcat {

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size);
    return out;
}

std::string Utf8ClampBytes(std::string_view src, size_t maxBytes) {
    if (src.size() <= maxBytes) return std::string(src);
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    return std::string(src.substr(0, n));
}

std::wstring ParentDirWithSlash(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash + 1);
}

bool ResolveAbsolutePath(const std::wstring& path, std::wstring& out) {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    if (len == 0 || len >= MAX_PATH) return false;
    out.assign(buffer);
    return true;
}

std::string JoinBinPath(const char* binDir, const char* relative) {
    if (!binDir || !binDir[0]) return relative ? relative : "";
    if (!relative || !relative[0]) return binDir;
    std::string out = binDir;
    if (out.back() != '\\' && out.back() != '/') out += '\\';
    out += relative;
    return out;
}

bool CreateDirectoryUtf8(const std::string& path) {
    if (path.empty()) return false;
    const std::wstring wide = Utf8ToWide(path);
    if (wide.empty()) return false;
    // Nested mkdir: a\b\c
    std::wstring cur;
    for (size_t i = 0; i < wide.size(); ++i) {
        const wchar_t ch = wide[i];
        cur.push_back(ch);
        if (ch == L'\\' || ch == L'/' || i + 1 == wide.size()) {
            if (cur.size() >= 3) {  // skip "C:" alone
                CreateDirectoryW(cur.c_str(), nullptr);
            }
        }
    }
    const DWORD attr = GetFileAttributesW(wide.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DeleteFileUtf8(const std::string& path) {
    if (path.empty()) return false;
    const std::wstring wide = Utf8ToWide(path);
    return !wide.empty() && DeleteFileW(wide.c_str()) != FALSE;
}

bool MoveFileExUtf8(const std::string& from, const std::string& to, DWORD flags) {
    const std::wstring fromW = Utf8ToWide(from);
    const std::wstring toW = Utf8ToWide(to);
    return !fromW.empty() && !toW.empty() && MoveFileExW(fromW.c_str(), toW.c_str(), flags);
}

bool CopyFileUtf8(const std::string& from, const std::string& to, bool failIfExists) {
    const std::wstring fromW = Utf8ToWide(from);
    const std::wstring toW = Utf8ToWide(to);
    return !fromW.empty() && !toW.empty() &&
           CopyFileW(fromW.c_str(), toW.c_str(), failIfExists ? TRUE : FALSE) != FALSE;
}

errno_t FopenUtf8(FILE** file, const std::string& path, const wchar_t* mode) {
    if (!file) return EINVAL;
    *file = nullptr;
    if (path.empty() || !mode || !mode[0]) return EINVAL;
    const std::wstring wide = Utf8ToWide(path);
    if (wide.empty()) return EINVAL;
    *file = _wfsopen(wide.c_str(), mode, _SH_DENYNO);
    return *file ? 0 : errno;
}

DWORD FindProcessIdByName(std::wstring_view exeName) {
    if (exeName.empty()) return 0;
    const std::wstring target(exeName);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, target.c_str()) != 0) continue;
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            pid = pe.th32ProcessID;
            break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

unsigned KillProcessesByExeName(std::wstring_view exeName) {
    if (exeName.empty()) return 0;
    const std::wstring target(exeName);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{sizeof(pe)};
    unsigned killed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, target.c_str()) != 0) continue;
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!proc) continue;
            if (TerminateProcess(proc, 1)) {
                WaitForSingleObject(proc, 3000);
                ++killed;
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

unsigned KillClassicGameWithRetry(unsigned maxRounds, DWORD gapMs) {
    if (maxRounds == 0) maxRounds = 1;
    unsigned total = 0;
    for (unsigned round = 0; round < maxRounds; ++round) {
        total += KillProcessesByExeName(L"Maplestory_Classic.exe");
        const DWORD waitMs = gapMs + 500u;
        if (WaitUntilNoProcessByName(L"Maplestory_Classic.exe", waitMs)) break;
        if (round + 1 < maxRounds && gapMs > 0) Sleep(gapMs);
    }
    return total;
}

bool WaitUntilNoProcessByName(std::wstring_view exeName, DWORD timeoutMs) {
    if (exeName.empty()) return true;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;) {
        if (FindProcessIdByName(exeName) == 0) return true;
        if (GetTickCount64() >= deadline) return FindProcessIdByName(exeName) == 0;
        Sleep(200);
    }
}

bool IsProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return ok && code == STILL_ACTIVE;
}

}  // namespace xcat
