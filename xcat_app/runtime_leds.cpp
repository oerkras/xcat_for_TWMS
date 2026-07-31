#include "runtime_leds.h"

#include "msc_webview_login.h"

#include <TlHelp32.h>
#include <Windows.h>

namespace xcat::app {
namespace {

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

}  // namespace

RuntimeLeds QueryRuntimeLeds() {
    static uint64_t s_webReadyTick = 0;
    RuntimeLeds out{};

    out.ipc = msc::weblogin::IsReady();
    if (out.ipc) {
        if (!s_webReadyTick) s_webReadyTick = GetTickCount64();
        out.webReadyTickMs = s_webReadyTick;
    } else {
        s_webReadyTick = 0;
        out.webReadyTickMs = 0;
    }

    out.gamePid = FindClassicGamePid();
    out.gameContext = out.gamePid != 0;
    // LocalPlayer / Map / Cache：注入与 payload 状态契约到位后再点亮
    out.localPlayer = false;
    out.mapOk = false;
    out.quizCache = false;
    return out;
}

}  // namespace xcat::app
