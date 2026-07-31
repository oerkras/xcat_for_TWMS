#include "single_instance.h"

#include "xcat_log.h"

namespace xcat::app {
namespace {

HANDLE g_mutex = nullptr;
constexpr wchar_t kMutexName[] = L"Local\\XCatTwms.Launcher.SingleInstance";

}  // namespace

bool AcquireXcatSingleInstance(DWORD maxWaitMs) {
    if (g_mutex) return true;

    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g_mutex) {
        xcat::log::Error("App", "CreateMutex single-instance failed err=%lu", GetLastError());
        return false;
    }

    const DWORD wait = WaitForSingleObject(g_mutex, maxWaitMs);
    if (wait == WAIT_OBJECT_0) {
        xcat::log::Info("App", "single-instance acquired");
        return true;
    }
    if (wait == WAIT_ABANDONED) {
        xcat::log::Warn("App", "single-instance acquired after abandon");
        return true;
    }

    xcat::log::Warn("App", "single-instance wait timed out (%lums)", maxWaitMs);
    CloseHandle(g_mutex);
    g_mutex = nullptr;
    return false;
}

void ReleaseXcatSingleInstance() {
    if (!g_mutex) return;
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    g_mutex = nullptr;
    xcat::log::Info("App", "single-instance released");
}

}  // namespace xcat::app
