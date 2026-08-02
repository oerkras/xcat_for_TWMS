#include "runtime_leds.h"

#include "msc_webview_login.h"

#include "xcat_payload_status.h"

#include <TlHelp32.h>
#include <Windows.h>

#include <cstring>

namespace xcat::app {
namespace {

constexpr uint64_t kPayloadLedStaleMs = 5000;
constexpr uint64_t kRuntimeLedGraceMs = 2500;

unsigned long FindClassicGamePid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    unsigned long pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Maplestory_Classic.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

struct LedCache {
    unsigned long pid = 0;
    RuntimeLeds leds{};
    uint64_t lastFreshTickMs = 0;
    bool valid = false;
};

LedCache g_ledCache;

RuntimeLeds CachedOrEmpty(unsigned long gamePid, uint64_t now, uint64_t webReadyTickMs) {
    RuntimeLeds out{};
    out.gamePid = gamePid;
    out.webReadyTickMs = webReadyTickMs;
    if (g_ledCache.valid && g_ledCache.pid == gamePid && g_ledCache.lastFreshTickMs != 0 &&
        now >= g_ledCache.lastFreshTickMs &&
        now - g_ledCache.lastFreshTickMs <= kRuntimeLedGraceMs) {
        out = g_ledCache.leds;
        out.gamePid = gamePid;
        out.webReadyTickMs = webReadyTickMs;
        return out;
    }
    return out;
}

}  // namespace

RuntimeLeds QueryRuntimeLeds(const char* prefsBinDir) {
    static uint64_t s_webReadyTick = 0;
    RuntimeLeds out{};
    const uint64_t now = GetTickCount64();

    const bool webReady = msc::weblogin::IsReady();
    if (webReady) {
        if (!s_webReadyTick) s_webReadyTick = now;
        out.webReadyTickMs = s_webReadyTick;
    } else {
        s_webReadyTick = 0;
        out.webReadyTickMs = 0;
    }

    out.gamePid = FindClassicGamePid();
    // 注入前基线：WebView / 进程探测
    out.ipc = webReady;
    out.gameContext = out.gamePid != 0;

    if (!prefsBinDir || !prefsBinDir[0] || !out.gamePid) {
        g_ledCache = {};
        return out;
    }

    xcat::PayloadStatus ps{};
    if (!xcat::ReadPayloadStatus(prefsBinDir, ps) ||
        !xcat::PayloadStatusHeartbeatFresh(ps, now, kPayloadLedStaleMs)) {
        RuntimeLeds grace = CachedOrEmpty(out.gamePid, now, out.webReadyTickMs);
        // 缓存命中时保留 LP/Map/Cache；否则退回注入前基线
        if (grace.localPlayer || grace.mapOk || grace.quizCache || grace.ipc) {
            // 进程仍在时保留 GC；IPC 仍可与 WebView OR
            grace.ipc = grace.ipc || webReady;
            grace.gameContext = grace.gameContext || (out.gamePid != 0);
            grace.gamePid = out.gamePid;
            grace.webReadyTickMs = out.webReadyTickMs;
            return grace;
        }
        return out;
    }

    // 注入后：以 payload 心跳为准（IPC/GC 与注入前探测 OR，避免闪灭）
    out.ipc = webReady || ps.ipcHandshake != 0;
    out.gameContext = (out.gamePid != 0) || ps.gameContextOk != 0;
    out.localPlayer = ps.localPlayerOk != 0;
    out.mapId = ps.mapId == 0xFFFFFFFFu ? 0 : static_cast<int>(ps.mapId);
    if (ps.currentMapName[0]) strncpy_s(out.currentMapName, ps.currentMapName, _TRUNCATE);
    out.playReady = ps.playReady != 0;
    out.wmAlive = ps.wmAlive != 0;
    out.sceneState = ps.sceneState;
    out.mapOk = out.playReady;
    out.quizCache = ps.quizCacheRootOk != 0;

    g_ledCache.pid = out.gamePid;
    g_ledCache.leds = out;
    g_ledCache.lastFreshTickMs = now;
    g_ledCache.valid = true;
    return out;
}

}  // namespace xcat::app
