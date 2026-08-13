#include "game_exit_probe.h"

#include <Windows.h>
#include <winevt.h>

#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "wevtapi.lib")

namespace xcat::app::game_exit_probe {
namespace {

constexpr char kClassicExeA[] = "Maplestory_Classic.exe";

bool IEqualsAscii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        const unsigned char ca = static_cast<unsigned char>(*a++);
        const unsigned char cb = static_cast<unsigned char>(*b++);
        const unsigned char la = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + 32) : ca;
        const unsigned char lb = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + 32) : cb;
        if (la != lb) return false;
    }
    return *a == *b;
}

// 在 UTF-16 XML 里找 ASCII 子串（事件 XML 里 Data 多为 ASCII）。
const wchar_t* FindAsciiInsensitive(const wchar_t* hay, const char* needle) {
    if (!hay || !needle || !*needle) return nullptr;
    for (const wchar_t* p = hay; *p; ++p) {
        const char* n = needle;
        const wchar_t* q = p;
        while (*n) {
            wchar_t hc = *q;
            if (hc >= L'A' && hc <= L'Z') hc = static_cast<wchar_t>(hc + 32);
            char nc = *n;
            if (nc >= 'A' && nc <= 'Z') nc = static_cast<char>(nc + 32);
            if (!*q || hc != static_cast<wchar_t>(nc)) break;
            ++q;
            ++n;
        }
        if (!*n) return p;
    }
    return nullptr;
}

bool ExtractNamedData(const wchar_t* xml, const char* name, char* out, size_t outCap) {
    if (!xml || !name || !out || outCap == 0) return false;
    out[0] = '\0';
    char open[96]{};
    snprintf(open, sizeof(open), "Name=\"%s\">", name);
    const wchar_t* p = FindAsciiInsensitive(xml, open);
    if (!p) return false;
    p = wcschr(p, L'>');
    if (!p) return false;
    ++p;
    size_t n = 0;
    while (p[n] && p[n] != L'<' && n + 1 < outCap) {
        const wchar_t ch = p[n];
        out[n] = (ch < 128) ? static_cast<char>(ch) : '?';
        ++n;
    }
    out[n] = '\0';
    return n > 0;
}

uint32_t ParsePidOrHex(const char* s) {
    if (!s || !*s) return 0;
    while (*s == ' ' || *s == '\t') ++s;
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        v = strtoul(s + 2, nullptr, 16);
    } else {
        v = strtoul(s, nullptr, 0);
    }
    if (v > 0xFFFFFFFFul) return 0;
    return static_cast<uint32_t>(v);
}

bool RenderEventXml(EVT_HANDLE event, std::wstring& out) {
    out.clear();
    DWORD bytes = 0;
    DWORD props = 0;
    if (EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &bytes, &props) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) {
        return false;
    }
    out.resize(bytes / sizeof(wchar_t) + 1);
    if (!EvtRender(nullptr, event, EvtRenderEventXml, bytes, out.data(), &bytes, &props)) {
        out.clear();
        return false;
    }
    // EvtRender 写入含 NUL；按字节数裁掉尾部垃圾。
    const size_t chars = bytes / sizeof(wchar_t);
    if (chars > 0) out.resize(chars - (out[chars - 1] == L'\0' ? 1 : 0));
    return !out.empty();
}

bool LooksLikeClassicAppName(const char* appName) {
    if (!appName || !*appName) return false;
    if (IEqualsAscii(appName, kClassicExeA)) return true;
    // 兼容路径尾段或大小写变体。
    return _stricmp(appName, kClassicExeA) == 0 ||
           strstr(appName, "Maplestory_Classic") != nullptr ||
           strstr(appName, "maplestory_classic") != nullptr;
}

bool FillFromXml(const wchar_t* xml, uint32_t preferPid, Result& r) {
    char appName[96]{};
    const bool namedApp = ExtractNamedData(xml, "AppName", appName, sizeof(appName));
    if (namedApp) {
        if (!LooksLikeClassicAppName(appName)) return false;
    } else if (!FindAsciiInsensitive(xml, kClassicExeA)) {
        return false;
    }

    char buf[96]{};
    if (ExtractNamedData(xml, "ExceptionCode", buf, sizeof(buf))) {
        r.exceptionCode = ParsePidOrHex(buf);
    }
    if (ExtractNamedData(xml, "FaultingProcessId", buf, sizeof(buf)) ||
        ExtractNamedData(xml, "ProcessId", buf, sizeof(buf))) {
        r.faultingPid = ParsePidOrHex(buf);
    }
    if (ExtractNamedData(xml, "FaultModuleName", buf, sizeof(buf)) ||
        ExtractNamedData(xml, "ModuleName", buf, sizeof(buf))) {
        snprintf(r.faultingModule, sizeof(r.faultingModule), "%s", buf);
    }

    r.kind = Kind::CrashEvidence;
    r.pidMatched = preferPid != 0 && r.faultingPid != 0 && preferPid == r.faultingPid;

    const char* pidNote =
        r.pidMatched ? " (pid-match)" : (preferPid ? " (pid-unmatched/unknown)" : "");
    if (r.faultingModule[0]) {
        snprintf(r.detail, sizeof(r.detail),
                 "Application Error(1000) app=Maplestory_Classic.exe mod=%s ex=0x%08X pid=%u%s",
                 r.faultingModule, r.exceptionCode, r.faultingPid, pidNote);
    } else {
        snprintf(r.detail, sizeof(r.detail),
                 "Application Error(1000) app=Maplestory_Classic.exe ex=0x%08X pid=%u%s",
                 r.exceptionCode, r.faultingPid, pidNote);
    }
    return true;
}

}  // namespace

const char* ReasonLabel(Kind kind) {
    switch (kind) {
    case Kind::CrashEvidence:
        return "游戏崩溃(Application Error)";
    case Kind::NoCrashEvidence:
        return "游戏进程已退出(无崩溃证据)";
    case Kind::Unknown:
    default:
        return "游戏进程已退出(崩溃探测失败)";
    }
}

Result ProbeRecentClassicFault(uint32_t preferPid, uint32_t lookbackSec) {
    Result r{};
    if (lookbackSec == 0) lookbackSec = 180;
    if (lookbackSec > 900) lookbackSec = 900;
    const uint32_t lookbackMs = lookbackSec * 1000u;

    // timediff(@SystemTime) 单位 ms；反向取最近事件。
    wchar_t query[256]{};
    _snwprintf_s(query, _TRUNCATE,
                 L"*[System[Provider[@Name='Application Error'] and (EventID=1000) and "
                 L"TimeCreated[timediff(@SystemTime) <= %u]]]",
                 lookbackMs);

    const EVT_HANDLE hQuery =
        EvtQuery(nullptr, L"Application", query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!hQuery) {
        snprintf(r.detail, sizeof(r.detail), "EvtQuery failed gle=%lu", GetLastError());
        r.kind = Kind::Unknown;
        return r;
    }

    EVT_HANDLE events[8]{};
    DWORD returned = 0;
    bool found = false;
    // 最多扫几页；Classic 命中即停。
    for (int page = 0; page < 4 && !found; ++page) {
        if (!EvtNext(hQuery, 8, events, 2000, 0, &returned) || returned == 0) {
            break;
        }
        for (DWORD i = 0; i < returned; ++i) {
            std::wstring xml;
            if (RenderEventXml(events[i], xml) && FillFromXml(xml.c_str(), preferPid, r)) {
                found = true;
            }
            EvtClose(events[i]);
            events[i] = nullptr;
            if (found) {
                for (DWORD j = i + 1; j < returned; ++j) {
                    if (events[j]) EvtClose(events[j]);
                }
                break;
            }
        }
    }
    EvtClose(hQuery);

    if (!found) {
        r = Result{};
        r.kind = Kind::NoCrashEvidence;
        snprintf(r.detail, sizeof(r.detail),
                 "no Application Error(1000) for Maplestory_Classic.exe in last %us (preferPid=%u)",
                 lookbackSec, preferPid);
    }
    return r;
}

}  // namespace xcat::app::game_exit_probe
