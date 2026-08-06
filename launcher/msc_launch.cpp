#include "msc_launch.h"

#include <TlHelp32.h>
#include <shellapi.h>

#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

namespace msc::launcher {
namespace {

constexpr int kProcessCommandLineInformation = 60;

struct UNICODE_STRING_BUF {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

void Emit(ProgressCallback& cb, Stage stage, const std::string& msg, DWORD pid = 0) {
    if (!cb) return;
    Progress p;
    p.stage = stage;
    p.message = msg;
    p.gamePid = pid;
    cb(p);
}

std::string ErrSuffix(DWORD err) {
    if (!err) return {};
    return " err=" + std::to_string(err);
}

std::wstring NowTimestampMs() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return std::to_wstring(ms);
}

std::string NarrowLossy(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool FieldSafeForPassarg(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (c < 0x20) return false;
        if (c == L'\'' || c == L'"') return false;
        if (c == L' ' || c == L'\t') return false;
        if (c == L'\\' || c == L'\r' || c == L'\n') return false;
    }
    return true;
}

std::wstring RedactTokenish(const std::wstring& s) {
    if (s.size() <= 6) return L"***";
    return s.substr(0, 6) + L"***";
}

std::vector<DWORD> CollectPidsByExeName(const wchar_t* exeName) {
    std::vector<DWORD> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool PidAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code == STILL_ACTIVE;
}

struct ShellOpenResult {
    bool ok = false;
    DWORD err = 0;
};

ShellOpenResult ShellOpen(const std::wstring& target) {
    ShellOpenResult r;
    const HINSTANCE hr = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOW);
    const auto code = reinterpret_cast<uintptr_t>(hr);
    r.ok = code > 32;
    if (!r.ok) r.err = static_cast<DWORD>(code);
    return r;
}

struct CreateResult {
    bool ok = false;
    DWORD err = 0;
};

CreateResult CreateProcessOnNgm(const std::wstring& ngmPath, const std::wstring& deepLink) {
    CreateResult r;
    std::wstring cmd = L"\"" + ngmPath + L"\" \"" + deepLink + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        r.err = GetLastError();
        return r;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.ok = true;
    return r;
}

std::wstring ExpandEnv(const wchar_t* pathWithEnv) {
    wchar_t out[MAX_PATH]{};
    const DWORD n = ExpandEnvironmentStringsW(pathWithEnv, out, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return {};
    return out;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring QueryImagePathByPid(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t path[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(proc, 0, path, &sz) && path[0]) out = path;
    CloseHandle(proc);
    return out;
}

std::wstring QueryImagePathByExeName(const wchar_t* exeName) {
    for (DWORD pid : CollectPidsByExeName(exeName)) {
        std::wstring p = QueryImagePathByPid(pid);
        if (!p.empty() && FileExists(p)) return p;
    }
    return {};
}

// 从协议命令行 `"C:\...\NGM64.exe" "%1"` 抽出可执行路径
std::wstring ExtractQuotedExe(const std::wstring& v) {
    if (v.empty()) return {};
    size_t start = 0;
    if (v[0] == L'"') {
        const auto q = v.find(L'"', 1);
        if (q == std::wstring::npos || q <= 1) return {};
        return v.substr(1, q - 1);
    }
    while (start < v.size() && (v[start] == L' ' || v[start] == L'\t')) ++start;
    const auto sp = v.find_first_of(L" \t", start);
    return sp == std::wstring::npos ? v.substr(start) : v.substr(start, sp - start);
}

std::wstring FindNgmFromProtocol() {
    auto tryKey = [](HKEY root, const wchar_t* sub) -> std::wstring {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
        wchar_t regBuf[1024]{};
        DWORD typ = 0;
        DWORD cb = sizeof(regBuf);
        const LONG st =
            RegQueryValueExW(key, nullptr, nullptr, &typ, reinterpret_cast<LPBYTE>(regBuf), &cb);
        RegCloseKey(key);
        if (st != ERROR_SUCCESS || (typ != REG_SZ && typ != REG_EXPAND_SZ)) return {};
        std::wstring path = ExtractQuotedExe(regBuf);
        if (typ == REG_EXPAND_SZ) {
            const std::wstring exp = ExpandEnv(path.c_str());
            if (!exp.empty()) path = exp;
        }
        return (!path.empty() && FileExists(path)) ? path : std::wstring{};
    };

    // 对照仓优先 HKCR；再兜 HKLM Classes（部分精简镜像）
    if (std::wstring p = tryKey(HKEY_CLASSES_ROOT, L"ngm\\Shell\\Open\\Command"); !p.empty())
        return p;
    if (std::wstring p =
            tryKey(HKEY_LOCAL_MACHINE, L"Software\\Classes\\ngm\\Shell\\Open\\Command");
        !p.empty())
        return p;
    return {};
}

std::wstring FindNgmFromInstallRegistry() {
    static const wchar_t* kRegSubKeys[] = {
        L"SOFTWARE\\Nexon\\NGM",
        L"SOFTWARE\\WOW6432Node\\Nexon\\NGM",
        L"SOFTWARE\\Nexon\\NGM64",
        L"SOFTWARE\\WOW6432Node\\Nexon\\NGM64",
        nullptr,
    };
    static const HKEY kHives[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    static const wchar_t* kValueNames[] = {L"Path", L"InstallPath", L"ExePath", nullptr};

    for (HKEY hive : kHives) {
        for (int ki = 0; kRegSubKeys[ki]; ++ki) {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(hive, kRegSubKeys[ki], 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;
            for (int vi = 0; kValueNames[vi]; ++vi) {
                wchar_t val[MAX_PATH]{};
                DWORD sz = sizeof(val);
                if (RegQueryValueExW(hKey, kValueNames[vi], nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(val), &sz) != ERROR_SUCCESS ||
                    !val[0])
                    continue;
                std::wstring path = val;
                if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".exe") == 0) {
                    if (FileExists(path)) {
                        RegCloseKey(hKey);
                        return path;
                    }
                } else {
                    if (!path.empty() && path.back() != L'\\') path += L'\\';
                    const std::wstring ngm64 = path + L"NGM64.exe";
                    if (FileExists(ngm64)) {
                        RegCloseKey(hKey);
                        return ngm64;
                    }
                    const std::wstring ngm = path + L"NGM.exe";
                    if (FileExists(ngm)) {
                        RegCloseKey(hKey);
                        return ngm;
                    }
                }
            }
            RegCloseKey(hKey);
        }
    }
    return {};
}

// 经典版游戏本体候选（仅用于反推旁路 NGM，不替代 NGM deep-link 拉起）
std::wstring FindClassicExePath() {
    if (std::wstring p = QueryImagePathByExeName(L"Maplestory_Classic.exe"); !p.empty()) return p;

    static const wchar_t* kCandidates[] = {
        L"%LOCALAPPDATA%\\Nexon\\MapleStory Classic\\Maplestory_Classic.exe",
        L"%ProgramFiles%\\Nexon\\MapleStory Classic\\Maplestory_Classic.exe",
        L"%ProgramFiles(x86)%\\Nexon\\MapleStory Classic\\Maplestory_Classic.exe",
        L"C:\\Nexon\\MapleStory Classic\\Maplestory_Classic.exe",
        L"C:\\nexon\\MapleStory Classic\\Maplestory_Classic.exe",
        L"C:\\Nexon\\maplestory_classic\\Maplestory_Classic.exe",
        L"C:\\Games\\maplestory_classic\\Maplestory_Classic.exe",
        nullptr,
    };
    for (const wchar_t* tmpl : kCandidates) {
        const std::wstring p = ExpandEnv(tmpl);
        if (!p.empty() && FileExists(p)) return p;
    }

    static const wchar_t* kRegSubKeys[] = {
        L"SOFTWARE\\Nexon\\MapleStory Classic",
        L"SOFTWARE\\WOW6432Node\\Nexon\\MapleStory Classic",
        L"SOFTWARE\\Nexon\\maplestory_classic",
        L"SOFTWARE\\WOW6432Node\\Nexon\\maplestory_classic",
        nullptr,
    };
    static const HKEY kHives[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    static const wchar_t* kValueNames[] = {L"InstallPath", L"ExePath", L"Path", nullptr};
    for (HKEY hive : kHives) {
        for (int ki = 0; kRegSubKeys[ki]; ++ki) {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(hive, kRegSubKeys[ki], 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;
            for (int vi = 0; kValueNames[vi]; ++vi) {
                wchar_t val[MAX_PATH]{};
                DWORD sz = sizeof(val);
                if (RegQueryValueExW(hKey, kValueNames[vi], nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(val), &sz) != ERROR_SUCCESS ||
                    !val[0])
                    continue;
                std::wstring path = val;
                if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".exe") == 0) {
                    if (FileExists(path)) {
                        RegCloseKey(hKey);
                        return path;
                    }
                } else {
                    if (!path.empty() && path.back() != L'\\') path += L'\\';
                    path += L"Maplestory_Classic.exe";
                    if (FileExists(path)) {
                        RegCloseKey(hKey);
                        return path;
                    }
                }
            }
            RegCloseKey(hKey);
        }
    }
    return {};
}

// …\GameDir\Maplestory_Classic.exe → …\NGM\NGM64.exe（对照仓 msw→NGM 旁路，经典版同布局）
std::wstring FindNgmBesideClassicInstall() {
    const std::wstring classic = FindClassicExePath();
    if (classic.empty()) return {};
    const size_t slash = classic.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    const std::wstring gameDir = classic.substr(0, slash);
    const size_t parentSlash = gameDir.find_last_of(L"\\/");
    if (parentSlash == std::wstring::npos) return {};
    const std::wstring nexonRoot = gameDir.substr(0, parentSlash);
    const std::wstring ngm64 = nexonRoot + L"\\NGM\\NGM64.exe";
    if (FileExists(ngm64)) return ngm64;
    const std::wstring ngm = nexonRoot + L"\\NGM\\NGM.exe";
    if (FileExists(ngm)) return ngm;
    return {};
}

FILETIME ProcessCreationTime(DWORD pid) {
    FILETIME zero{};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return zero;
    FILETIME create{}, exitT{}, kernel{}, user{};
    if (!GetProcessTimes(h, &create, &exitT, &kernel, &user)) {
        CloseHandle(h);
        return zero;
    }
    CloseHandle(h);
    return create;
}

bool FileTimeNewer(const FILETIME& a, const FILETIME& b) {
    ULARGE_INTEGER ua{}, ub{};
    ua.LowPart = a.dwLowDateTime;
    ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime;
    ub.HighPart = b.dwHighDateTime;
    return ua.QuadPart > ub.QuadPart;
}

bool FileTimeGeq(const FILETIME& a, const FILETIME& b) {
    ULARGE_INTEGER ua{}, ub{};
    ua.LowPart = a.dwLowDateTime;
    ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime;
    ub.HighPart = b.dwHighDateTime;
    return ua.QuadPart >= ub.QuadPart;
}

// 「不早于 sec 秒前」的 FILETIME；对标 gamapass_cdp 的 sessionNotBefore，同留 2s 时钟偏差。
FILETIME FileTimeSecondsAgo(int sec) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    if (sec <= 0) return FILETIME{};
    ULARGE_INTEGER uli{};
    uli.LowPart = now.dwLowDateTime;
    uli.HighPart = now.dwHighDateTime;
    const ULONGLONG back = (static_cast<ULONGLONG>(sec) + 2ULL) * 10000000ULL;
    uli.QuadPart = uli.QuadPart > back ? uli.QuadPart - back : 0ULL;
    FILETIME out{};
    out.dwLowDateTime = uli.LowPart;
    out.dwHighDateTime = uli.HighPart;
    return out;
}

bool CreationTimeOk(DWORD pid, const FILETIME* notBefore) {
    if (!notBefore) return true;
    ULARGE_INTEGER nb{};
    nb.LowPart = notBefore->dwLowDateTime;
    nb.HighPart = notBefore->dwHighDateTime;
    if (nb.QuadPart == 0) return true;
    const FILETIME ct = ProcessCreationTime(pid);
    ULARGE_INTEGER uc{};
    uc.LowPart = ct.dwLowDateTime;
    uc.HighPart = ct.dwHighDateTime;
    if (uc.QuadPart == 0) return false;  // 读不到创建时间则保守跳过
    return FileTimeGeq(ct, *notBefore);
}

// 等新 PID，且 cmdline 四元组匹配 ticket（对标 WaitForMswWithAuth）
DWORD WaitForClassicWithTicket(const wchar_t* exeName, const std::unordered_set<DWORD>& before,
                               const GalaxyTicket& ticket, int timeoutSec,
                               std::wstring* outCmd) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
        for (DWORD pid : CollectPidsByExeName(exeName)) {
            if (!PidAlive(pid)) continue;
            if (before.find(pid) != before.end()) continue;
            const std::wstring cmd = GetProcessCommandLineW(pid);
            if (cmd.empty()) continue;
            if (!CmdMatchesGalaxyTicket(cmd, ticket)) continue;
            if (outCmd) *outCmd = cmd;
            return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return 0;
}

}  // namespace

bool TicketLooksUsable(const GalaxyTicket& t) {
    return FieldSafeForPassarg(t.userObjectId) && FieldSafeForPassarg(t.userSessionToken) &&
           FieldSafeForPassarg(t.gid) && FieldSafeForPassarg(t.galaxyGameId) &&
           FieldSafeForPassarg(t.ngmGameId);
}

std::wstring BuildNgmPassarg(const GalaxyTicket& t) {
    return t.userObjectId + L" " + t.userSessionToken + L" " + t.gid + L" " + t.galaxyGameId;
}

std::wstring BuildNgmDeepLink(const GalaxyTicket& t, NgmMode mode) {
    const wchar_t* modeStr = (mode == NgmMode::Restore) ? L"restore" : L"launch";
    std::wstring args = L"-mode:";
    args += modeStr;
    args += L" -game:'";
    args += t.ngmGameId;
    args += L"' -passarg:'";
    args += BuildNgmPassarg(t);
    args += L"' -architectureplatform:'none' -timestamp:";
    args += NowTimestampMs();
    return L"ngm://launch/ " + args;
}

std::string FormatDeepLinkForLog(const std::wstring& deepLink) {
    std::wstring w = deepLink;
    const std::wstring key = L"-passarg:'";
    const auto pos = w.find(key);
    if (pos != std::wstring::npos) {
        const auto start = pos + key.size();
        const auto end = w.find(L"'", start);
        if (end != std::wstring::npos) {
            w.replace(start, end - start, L"***");
        }
    }
    return NarrowLossy(w);
}

std::vector<std::wstring> SplitCommandLineArgs(const std::wstring& cmdLine) {
    std::vector<std::wstring> args;
    size_t i = 0;
    const size_t n = cmdLine.size();
    while (i < n) {
        while (i < n && (cmdLine[i] == L' ' || cmdLine[i] == L'\t')) ++i;
        if (i >= n) break;
        std::wstring tok;
        if (cmdLine[i] == L'"') {
            ++i;
            while (i < n && cmdLine[i] != L'"') {
                tok.push_back(cmdLine[i++]);
            }
            if (i < n && cmdLine[i] == L'"') ++i;
        } else {
            while (i < n && cmdLine[i] != L' ' && cmdLine[i] != L'\t') {
                tok.push_back(cmdLine[i++]);
            }
        }
        args.push_back(std::move(tok));
    }
    return args;
}

ClassicPassArgs ParseClassicPassArgs(const std::wstring& cmdLine) {
    ClassicPassArgs out;
    const auto args = SplitCommandLineArgs(cmdLine);
    // args[0]=exe path；其后恰好四段为 Galaxy 票（实机采证）
    if (args.size() < 5) return out;
    out.userObjectId = args[1];
    out.userSessionToken = args[2];
    out.gid = args[3];
    out.galaxyGameId = args[4];
    out.ok = !out.userObjectId.empty() && !out.userSessionToken.empty() && !out.gid.empty() &&
             !out.galaxyGameId.empty();
    return out;
}

bool CmdMatchesGalaxyTicket(const std::wstring& cmdLine, const GalaxyTicket& ticket) {
    const auto parsed = ParseClassicPassArgs(cmdLine);
    if (!parsed.ok) return false;
    return parsed.userObjectId == ticket.userObjectId &&
           parsed.userSessionToken == ticket.userSessionToken && parsed.gid == ticket.gid &&
           parsed.galaxyGameId == ticket.galaxyGameId;
}

std::string FormatCmdLineForLog(const std::wstring& cmdLine) {
    auto args = SplitCommandLineArgs(cmdLine);
    if (args.empty()) return {};
    std::wstring out;
    out.push_back(L'"');
    out += args[0];
    out.push_back(L'"');
    if (args.size() >= 5) {
        // "exe" uid token*** gid galaxyId
        out.push_back(L' ');
        out += args[1];
        out.push_back(L' ');
        out += RedactTokenish(args[2]);
        out.push_back(L' ');
        out += args[3];
        out.push_back(L' ');
        out += args[4];
        for (size_t i = 5; i < args.size(); ++i) {
            out.push_back(L' ');
            out += RedactTokenish(args[i]);
        }
    } else {
        for (size_t i = 1; i < args.size(); ++i) {
            out.push_back(L' ');
            out += RedactTokenish(args[i]);
        }
    }
    return NarrowLossy(out);
}

std::wstring GetProcessCommandLineW(DWORD pid) {
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return result;

    using NtQIP_t = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const auto NtQIP = reinterpret_cast<NtQIP_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!NtQIP) {
        CloseHandle(h);
        return result;
    }

    ULONG need = 0;
    ULONG cap = 4096;
    std::vector<BYTE> buf(cap);
    LONG st = NtQIP(h, kProcessCommandLineInformation, buf.data(), cap, &need);
    if (st == static_cast<LONG>(0xC0000004)) {  // STATUS_INFO_LENGTH_MISMATCH
        cap = (need > 0 && need < 64 * 1024) ? need : 8192;
        buf.assign(cap, 0);
        st = NtQIP(h, kProcessCommandLineInformation, buf.data(), cap, &need);
    }
    if (st != 0) {
        CloseHandle(h);
        return result;
    }

    const auto* us = reinterpret_cast<const UNICODE_STRING_BUF*>(buf.data());
    if (!us->Buffer || us->Length == 0 || us->Length > 8192) {
        CloseHandle(h);
        return result;
    }
    result.assign(us->Buffer, us->Buffer + (us->Length / sizeof(wchar_t)));
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    CloseHandle(h);
    return result;
}

std::wstring FindNgmPath() {
    // 1) 已在跑的 NGM：直接用其映像路径（对照仓同序）
    if (std::wstring p = QueryImagePathByExeName(L"NGM64.exe"); !p.empty()) return p;
    if (std::wstring p = QueryImagePathByExeName(L"NGM.exe"); !p.empty()) return p;

    // 2) ngm:// 协议注册
    if (std::wstring p = FindNgmFromProtocol(); !p.empty()) return p;

    // 3) 固定/环境变量候选（含对照仓 C:\Nexon\NGM）
    static const wchar_t* kCandidates[] = {
        L"C:\\ProgramData\\Nexon\\NGM\\NGM64.exe",
        L"C:\\ProgramData\\Nexon\\NGM\\NGM.exe",
        L"%ProgramFiles(x86)%\\Nexon\\NGM\\NGM.exe",
        L"%ProgramFiles%\\Nexon\\NGM\\NGM.exe",
        L"%LOCALAPPDATA%\\Nexon\\NGM\\NGM.exe",
        L"C:\\Nexon\\NGM\\NGM64.exe",
        L"C:\\Nexon\\NGM\\NGM.exe",
        L"C:\\nexon\\NGM\\NGM64.exe",
        L"C:\\nexon\\NGM\\NGM.exe",
        nullptr,
    };
    for (const wchar_t* tmpl : kCandidates) {
        const std::wstring p = ExpandEnv(tmpl);
        if (!p.empty() && FileExists(p)) return p;
    }

    // 4) 安装注册表 Path / InstallPath / ExePath
    if (std::wstring p = FindNgmFromInstallRegistry(); !p.empty()) return p;

    // 5) 从经典版安装目录旁路反推（对照仓从 msw 反推 → 本仓从 Maplestory_Classic）
    if (std::wstring p = FindNgmBesideClassicInstall(); !p.empty()) return p;

    return {};
}

bool IsNgmProcessRunning() {
    return !CollectPidsByExeName(L"NGM.exe").empty() ||
           !CollectPidsByExeName(L"NGM64.exe").empty();
}

bool IsNgmProcessRunningCreatedAfter(const FILETIME& notBefore) {
    for (DWORD pid : CollectPidsByExeName(L"NGM64.exe")) {
        if (PidAlive(pid) && CreationTimeOk(pid, &notBefore)) return true;
    }
    for (DWORD pid : CollectPidsByExeName(L"NGM.exe")) {
        if (PidAlive(pid) && CreationTimeOk(pid, &notBefore)) return true;
    }
    return false;
}

bool EnsureNgmRunning() {
    if (IsNgmProcessRunning()) return true;
    const std::wstring ngm = FindNgmPath();
    if (ngm.empty()) return false;
    const auto opened = ShellOpen(ngm);
    if (!opened.ok) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (IsNgmProcessRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return true;
}

unsigned long FindExistingClassicPid(const GalaxyTicket& ticket, const wchar_t* exeName,
                                     std::wstring* outCmd, bool* ticketMatched,
                                     const FILETIME* createdNotBefore, int* outUnmatched) {
    if (ticketMatched) *ticketMatched = false;
    if (outCmd) outCmd->clear();
    if (outUnmatched) *outUnmatched = 0;
    if (!exeName || !*exeName) exeName = L"Maplestory_Classic.exe";

    DWORD matchedPid = 0;
    std::wstring matchedCmd;
    DWORD anyPid = 0;
    std::wstring anyCmd;
    FILETIME anyCreate{};
    int unmatched = 0;

    for (DWORD pid : CollectPidsByExeName(exeName)) {
        if (!PidAlive(pid)) continue;
        if (!CreationTimeOk(pid, createdNotBefore)) continue;
        const std::wstring cmd = GetProcessCommandLineW(pid);
        if (TicketLooksUsable(ticket) && !cmd.empty() && CmdMatchesGalaxyTicket(cmd, ticket)) {
            matchedPid = pid;
            matchedCmd = cmd;
            break;
        }
        ++unmatched;
        const FILETIME ct = ProcessCreationTime(pid);
        if (!anyPid || FileTimeNewer(ct, anyCreate)) {
            anyPid = pid;
            anyCmd = cmd;
            anyCreate = ct;
        }
    }

    if (outUnmatched) *outUnmatched = unmatched;
    if (matchedPid) {
        if (ticketMatched) *ticketMatched = true;
        if (outCmd) *outCmd = matchedCmd;
        return matchedPid;
    }
    if (anyPid) {
        if (ticketMatched) *ticketMatched = false;
        if (outCmd) *outCmd = anyCmd;
        return anyPid;
    }
    return 0;
}

Result Run(const Options& opts, ProgressCallback cb) {
    Result r;
    Emit(cb, Stage::Init, "MSC NGM launch skeleton");

    if (!TicketLooksUsable(opts.ticket)) {
        r.finalStage = Stage::BlockedNeedsTicket;
        r.errorMessage =
            "GalaxyTicket 缺失或含非法字符（空/空白/引号/控制符）。"
            "先走官网 OTT→GetOneTimeWebInfo，或客户端 UIGalaxyLoginWebView 登录后再填入。";
        Emit(cb, Stage::BlockedNeedsTicket, r.errorMessage);
        return r;
    }

    if (opts.attachExistingClassic) {
        // 年龄窗：上一局残留的客户端不接管，否则会注入到别的账号而本票账号根本没拉起来。
        const FILETIME cutoff = FileTimeSecondsAgo(opts.attachMaxAgeSec);
        const FILETIME* cutoffPtr = opts.attachMaxAgeSec > 0 ? &cutoff : nullptr;

        bool matched = false;
        int unmatched = 0;
        std::wstring cmd;
        const DWORD existing = FindExistingClassicPid(opts.ticket, opts.gameExeName.c_str(), &cmd,
                                                     &matched, cutoffPtr, &unmatched);
        // 票匹配 → 确定是本次登录的实例，直接接管。
        // 无匹配但只有一个候选 → 官网自启的常见情形，按既有策略接管。
        // 无匹配且有多个候选 → 无从判定哪个属于本票，拒绝猜测，改走 NGM 正常拉起。
        const bool ambiguous = !matched && unmatched > 1;
        if (existing && !ambiguous) {
            r.ok = true;
            r.gamePid = existing;
            r.cmdLineSummary = FormatCmdLineForLog(cmd);
            r.finalStage = Stage::Done;
            Emit(cb, Stage::Done,
                 matched ? "接管已有经典版（cmdline 票匹配），跳过 NGM"
                         : "接管已有经典版（cmdline 未匹配本票，可能为官网自启），跳过 NGM",
                 existing);
            return r;
        }
        if (ambiguous) {
            Emit(cb, Stage::LaunchingGame,
                 "发现 " + std::to_string(unmatched) +
                     " 个经典版且无一匹配本票，不猜测接管（避免串到别的账号），改走 NGM 拉起");
        } else {
            Emit(cb, Stage::LaunchingGame, "未发现可接管的经典版（含年龄窗过滤），改走 NGM 拉起");
        }
    }

    Emit(cb, Stage::FindingNgm, "定位 NGM64.exe");
    const std::wstring ngm = FindNgmPath();
    if (ngm.empty()) {
        r.finalStage = Stage::Failed;
        r.errorMessage =
            "未找到 NGM（已搜：运行中进程 / ngm:// 协议 / ProgramData·ProgramFiles·Nexon 目录 / "
            "安装注册表 / 经典版旁路）。请确认 Nexon NGM 已安装。";
        Emit(cb, Stage::Failed, r.errorMessage);
        return r;
    }
    Emit(cb, Stage::FindingNgm, "NGM=" + NarrowLossy(ngm));

    const std::wstring deepLink = BuildNgmDeepLink(opts.ticket, opts.mode);
    r.deepLinkSummary = FormatDeepLinkForLog(deepLink);
    Emit(cb, Stage::LaunchingGame, "deep-link " + r.deepLinkSummary);

    if (opts.dryRunDeepLinkOnly) {
        r.ok = true;
        r.finalStage = Stage::Done;
        Emit(cb, Stage::Done, "dry-run：仅生成 deep-link 摘要，未拉起进程");
        return r;
    }

    std::unordered_set<DWORD> before;
    {
        const auto snap = CollectPidsByExeName(opts.gameExeName.c_str());
        before.insert(snap.begin(), snap.end());
        Emit(cb, Stage::LaunchingGame,
             "拉起前已有 " + NarrowLossy(opts.gameExeName) + " ×" + std::to_string(before.size()));
    }

    if (!EnsureNgmRunning()) {
        Emit(cb, Stage::LaunchingGame, "EnsureNgmRunning 未就绪，仍尝试 deep-link");
    }

    auto shell = ShellOpen(deepLink);
    bool launched = shell.ok;
    if (!launched) {
        Emit(cb, Stage::LaunchingGame,
             "ShellExecute ngm:// 失败" + ErrSuffix(shell.err) + "，改 CreateProcess");
        auto cp = CreateProcessOnNgm(ngm, deepLink);
        launched = cp.ok;
        if (!launched) {
            r.finalStage = Stage::Failed;
            r.errorMessage = "NGM 拉起失败：ShellExecute" + ErrSuffix(shell.err) +
                             "；CreateProcess" + ErrSuffix(cp.err);
            Emit(cb, Stage::Failed, r.errorMessage);
            return r;
        }
        Emit(cb, Stage::LaunchingGame, "CreateProcess(NGM) 已发起");
    } else {
        Emit(cb, Stage::LaunchingGame, "ShellExecute(ngm://) 已发起");
    }

    Emit(cb, Stage::WaitingForGame,
         "等待新进程且 cmdline 四元组匹配 ticket（uid/token/gid/galaxyId）");

    std::wstring matchedCmd;
    const DWORD pid = WaitForClassicWithTicket(opts.gameExeName.c_str(), before, opts.ticket,
                                               opts.waitGameSec, &matchedCmd);
    if (!pid) {
        r.finalStage = Stage::Failed;
        r.errorMessage =
            "等待带匹配 Galaxy 票的新进程超时；可能票过期、NGM 未透传 passarg，或仅有无票旧进程";
        Emit(cb, Stage::Failed, r.errorMessage);
        return r;
    }

    r.ok = true;
    r.gamePid = pid;
    r.cmdLineSummary = FormatCmdLineForLog(matchedCmd);
    r.finalStage = Stage::Done;
    Emit(cb, Stage::Done, "cmdline 验票通过 " + r.cmdLineSummary, pid);
    return r;
}

}  // namespace msc::launcher
