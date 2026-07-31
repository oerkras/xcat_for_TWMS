// TWMS Classic injected payload (xcat.dll).
// LoadLibraryW target for injector; hosts feature workers (fly first).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>

#include "../features/auto_enter/auto_enter.h"
#include "../features/fly/fly.h"
#include "../features/invuln/invuln.h"
#include "../features/kick_sniff/kick_sniff.h"
#include "../features/titlebar/titlebar.h"
#include "../runtime/bin_dir.h"
#include "../runtime/log.h"

namespace {

HANDLE gReadyEvent = nullptr;

void MarkReady() {
    wchar_t name[64]{};
    swprintf_s(name, L"Local\\XCatTwmsProbeReady_%lu", GetCurrentProcessId());
    gReadyEvent = CreateEventW(nullptr, TRUE, TRUE, name);
}

void UnmarkReady() {
    if (gReadyEvent) {
        CloseHandle(gReadyEvent);
        gReadyEvent = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(h);
        x::runtime::SetImageModule(h);
        // LogInit 内部会 CreateFile / mutex；DllMain 里尽量短，但需先于 feature 才能落盘。
        x::runtime::LogInit();
        MarkReady();
        x::runtime::LogI("Bootstrap",
                         "attached — fly + invuln + kick_sniff + auto_enter + titlebar");
        x::features::kick_sniff::Init();
        x::features::kick_sniff::StartWorker();
        x::features::fly::Init();
        x::features::fly::StartWorker();
        x::features::invuln::Init();
        x::features::invuln::StartWorker();
        x::features::auto_enter::Init();
        x::features::auto_enter::StartWorker();
        x::features::titlebar::Init();
        x::features::titlebar::StartWorker();
        break;
    case DLL_PROCESS_DETACH:
        // Signal only; never join (loader lock).
        x::features::titlebar::StopWorker();
        x::features::auto_enter::StopWorker();
        x::features::invuln::StopWorker();
        x::features::fly::StopWorker();
        x::features::kick_sniff::StopWorker();
        UnmarkReady();
        x::runtime::LogI("Bootstrap", "detached");
        x::runtime::LogShutdown();
        break;
    default:
        break;
    }
    return TRUE;
}
