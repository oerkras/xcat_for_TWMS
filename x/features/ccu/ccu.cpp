#include "ccu.h"
#include "ccu_passive.h"

#include "../../runtime/log.h"

#include <atomic>

namespace x {
namespace features {
namespace ccu {
namespace {

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};

DWORD WINAPI WorkerProc(LPVOID) {
    x::runtime::LogI("CCU", "worker start");
    Ccu_Init();
    while (!gStop.load()) {
        Ccu_Tick(GetTickCount());
        Sleep(200);
    }
    Ccu_Shutdown();
    x::runtime::LogI("CCU", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false);
    x::runtime::LogI("CCU", "ready (login UI or auto_enter; world switch updates)");
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE t = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    gThread.store(t);
}

void StopWorker() {
    gStop.store(true);
    // Signal only under loader lock — do not join.
}

CcuStatus GetCcuStatus() { return Ccu_GetStatus(); }

bool HasSnapshot() {
    const CcuStatus s = Ccu_GetStatus();
    return s.worldChannelOnline >= 0 && s.worldChannelCount > 0;
}

int32_t SnapshotWorldId() { return Ccu_SnapshotWorldId(); }

bool ShouldSkipFeed(int32_t worldId, bool allowRefresh) {
    return Ccu_ShouldSkipFeed(worldId, allowRefresh);
}

bool NotifyWorldChannelSnapshot(long long sum, int channelCount, const char* src, int32_t worldId,
                                bool allowRefresh) {
    return Ccu_NotifySnapshot(sum, channelCount, src, worldId, allowRefresh);
}

void NotifyChannelFillTable(const ChannelFillRow* rows, int n, const char* src, int32_t worldId) {
    Ccu_NotifyFillTable(rows, n, src, worldId);
}

ChannelPickHint GetChannelPickHint(int zeroBasedIdx) {
    return Ccu_GetChannelPickHint(zeroBasedIdx);
}

void MarkChannelRejected(int zeroBasedIdx) {
    Ccu_MarkChannelRejected(zeroBasedIdx);
}

}  // namespace ccu
}  // namespace features
}  // namespace x
