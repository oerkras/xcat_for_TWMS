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
    static const wchar_t* kCandidates[] = {
        L"C:\\ProgramData\\Nexon\\NGM\\NGM64.exe",
        L"C:\\ProgramData\\Nexon\\NGM\\NGM.exe",
        L"%ProgramFiles(x86)%\\Nexon\\NGM\\NGM.exe",
        L"%ProgramFiles%\\Nexon\\NGM\\NGM.exe",
        L"%LOCALAPPDATA%\\Nexon\\NGM\\NGM.exe",
    };
    for (const wchar_t* c : kCandidates) {
        std::wstring p = (wcschr(c, L'%') ? ExpandEnv(c) : std::wstring(c));
        if (!p.empty() && FileExists(p)) return p;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\ngm\\Shell\\Open\\Command", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t regBuf[1024]{};
        DWORD typ = 0;
        DWORD cb = sizeof(regBuf);
        if (RegQueryValueExW(key, nullptr, nullptr, &typ, reinterpret_cast<LPBYTE>(regBuf), &cb) ==
                ERROR_SUCCESS &&
            (typ == REG_SZ || typ == REG_EXPAND_SZ)) {
            std::wstring v = regBuf;
            if (!v.empty() && v.front() == L'"') {
                const auto q = v.find(L'"', 1);
                if (q != std::wstring::npos) {
                    const std::wstring path = v.substr(1, q - 1);
                    if (FileExists(path)) {
                        RegCloseKey(key);
                        return path;
                    }
                }
            }
        }
        RegCloseKey(key);
    }
    return {};
}

bool IsNgmProcessRunning() {
    return !CollectPidsByExeName(L"NGM.exe").empty() ||
           !CollectPidsByExeName(L"NGM64.exe").empty();
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

    Emit(cb, Stage::FindingNgm, "定位 NGM64.exe");
    const std::wstring ngm = FindNgmPath();
    if (ngm.empty()) {
        r.finalStage = Stage::Failed;
        r.errorMessage = "未找到 NGM（期望 C:\\ProgramData\\Nexon\\NGM\\NGM64.exe 或 ngm:// 注册）";
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
