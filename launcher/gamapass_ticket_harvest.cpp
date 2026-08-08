#include "gamapass_ticket_harvest.h"

#include "msc_launch.h"

namespace msc::launcher {
namespace {

void Log(const HttpLoginLogFn& log, const std::wstring& s) {
    if (log) log(s);
}

}  // namespace

FILETIME GamaPassSessionNotBeforeNow() {
    FILETIME sessionNotBefore{};
    GetSystemTimeAsFileTime(&sessionNotBefore);
    ULARGE_INTEGER uli{};
    uli.LowPart = sessionNotBefore.dwLowDateTime;
    uli.HighPart = sessionNotBefore.dwHighDateTime;
    constexpr ULONGLONG kSkew100ns = 2ULL * 10000000ULL;
    if (uli.QuadPart > kSkew100ns) uli.QuadPart -= kSkew100ns;
    sessionNotBefore.dwLowDateTime = uli.LowPart;
    sessionNotBefore.dwHighDateTime = uli.HighPart;
    return sessionNotBefore;
}

bool GamaPassNoteNgmLaunchHint(const FILETIME& sessionNotBefore, bool& sawNgmHint,
                               HttpLoginLogFn log, const wchar_t* logTag) {
    if (sawNgmHint) return false;
    if (!IsNgmProcessRunningCreatedAfter(sessionNotBefore)) return false;
    sawNgmHint = true;
    const wchar_t* tag = (logTag && logTag[0]) ? logTag : L"[gamapass]";
    Log(log, std::wstring(tag) +
                 L" 探测到 NGM 已启动（官网拉起中；成功门禁仍等经典版 cmdline 票）…");
    return true;
}

HttpLoginResult GamaPassTryHarvestClassicTicket(const FILETIME& sessionNotBefore,
                                                bool& sawNgmHint, HttpLoginLogFn log,
                                                const wchar_t* logTag) {
    const wchar_t* tag = (logTag && logTag[0]) ? logTag : L"[gamapass]";
    GamaPassNoteNgmLaunchHint(sessionNotBefore, sawNgmHint, log, tag);

    HttpLoginResult out;
    std::wstring cmd;
    bool matched = false;
    GalaxyTicket empty{};
    const DWORD pid = FindExistingClassicPid(empty, L"Maplestory_Classic.exe", &cmd, &matched,
                                             &sessionNotBefore);
    if (!pid) return out;
    const auto parsed = ParseClassicPassArgs(cmd);
    if (!parsed.ok) {
        Log(log, std::wstring(tag) + L" 已有经典版 PID=" + std::to_wstring(pid) +
                     L" 但 cmdline 无 Galaxy 四元组，继续等…");
        return out;
    }
    Log(log, std::wstring(tag) + L" 官网已拉起经典版 PID=" + std::to_wstring(pid) +
                 L"，从 cmdline 接管票（跳过 NGM 重开）" +
                 (sawNgmHint ? L"（此前已见 NGM）" : L""));
    out.ok = true;
    out.error = HttpLoginError::Ok;
    out.message = "attach-existing-classic";
    out.ticket.userObjectId = parsed.userObjectId;
    out.ticket.userSessionToken = parsed.userSessionToken;
    out.ticket.gid = parsed.gid;
    out.ticket.galaxyGameId = parsed.galaxyGameId;
    out.ticket.ngmGameId = parsed.galaxyGameId;
    out.ticketFilled = true;
    out.ott = L"(from-classic-cmdline)";
    return out;
}

}  // namespace msc::launcher
