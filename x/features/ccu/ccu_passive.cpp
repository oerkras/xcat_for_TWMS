// TWMS Classic CCU — only accepts auto_enter feed (PickLeast WorldItem.ci sum).
// No login-page FindAll / passive probe (avoids main-pump contention & GC).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "ccu_passive.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace x {
namespace features {
namespace ccu {
namespace {

constexpr DWORD kWaitLogMs = 15000;

constexpr int kFillSlots = 128;

struct FillSlot {
    int16_t users = -1;
    int16_t cap = -1;
    uint8_t adult = 0;
    uint8_t known = 0;
    uint8_t rejected = 0;
};

struct Store {
    long long sum = -1;
    int count = 0;
    DWORD updateTick = 0;
    FillSlot fill[kFillSlots]{};
    int fillKnown = 0;
    int fillPrefer = 0;
    DWORD fillTick = 0;
};

Store gStore{};
CRITICAL_SECTION gStoreCs{};
bool gStoreCsReady = false;

bool gDone = false;
DWORD gLastWaitLog = 0;

HANDLE gLog = INVALID_HANDLE_VALUE;

void EnsureCs() {
    if (!gStoreCsReady) {
        InitializeCriticalSection(&gStoreCs);
        gStoreCsReady = true;
    }
}

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    CreateDirectoryA(dir, nullptr);
    gLog = x::runtime::OpenRotatingDbgLogA(dir, "ccu.log");
}

void LogLine(const char* fmt, ...) {
    OpenLog();
    char body[480]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char buf[512]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s\r\n", st.wHour, st.wMinute, st.wSecond, body);
    if (n > 0 && gLog != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(gLog, buf, (DWORD)n, &w, nullptr);
    }
    x::runtime::LogI("CCU", "%s", body);
}

bool ApplyTotals(long long sum, int count, const char* src) {
    if (count <= 0 || sum < 0) return false;
    const DWORD now = GetTickCount();
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    if (gDone && gStore.sum >= 0) {
        LeaveCriticalSection(&gStoreCs);
        return false;
    }
    gStore.sum = sum;
    gStore.count = count;
    gStore.updateTick = now;
    gDone = true;
    LeaveCriticalSection(&gStoreCs);
    LogLine("频道快照[%s]: 分区在线=%lld channels=%d", src ? src : "?", sum, count);
    LogLine("latched once — feed-only; SHM via PayloadStatus publisher");
    return true;
}

bool AlreadyLatched() {
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    const bool ok = gDone && gStore.sum >= 0;
    LeaveCriticalSection(&gStoreCs);
    return ok;
}

void NotifySnapshotImpl(long long sum, int channelCount, const char* src) {
    OpenLog();
    (void)ApplyTotals(sum, channelCount, src && src[0] ? src : "auto_enter");
}

}  // namespace

void Ccu_NotifySnapshot(long long sum, int channelCount, const char* src) {
    NotifySnapshotImpl(sum, channelCount, src);
}

void Ccu_NotifyFillTable(const ChannelFillRow* rows, int n, const char* src) {
    if (!rows || n <= 0) return;
    EnsureCs();
    OpenLog();
    EnterCriticalSection(&gStoreCs);
    uint8_t keepRejected[kFillSlots]{};
    for (int i = 0; i < kFillSlots; ++i) {
        keepRejected[i] = gStore.fill[i].rejected;
        gStore.fill[i] = FillSlot{};
    }
    int known = 0;
    int prefer = 0;
    for (int i = 0; i < n; ++i) {
        const int id1 = static_cast<int>(rows[i].channelId);
        if (id1 < 1 || id1 > kFillSlots) continue;
        const int idx = id1 - 1;
        FillSlot& slot = gStore.fill[idx];
        slot.users = rows[i].users;
        slot.cap = rows[i].cap;
        slot.adult = rows[i].adult ? 1 : 0;
        slot.known = 1;
        slot.rejected = keepRejected[idx];
        ++known;
        const bool full = slot.cap > 0 && slot.users >= slot.cap;
        if (!slot.adult && !full && !slot.rejected) ++prefer;
    }
    // 未出现在新表里的频：只保留拒收标记，其余未知
    for (int i = 0; i < kFillSlots; ++i) {
        if (gStore.fill[i].known) continue;
        if (keepRejected[i]) gStore.fill[i].rejected = 1;
    }
    gStore.fillKnown = known;
    gStore.fillPrefer = prefer;
    gStore.fillTick = GetTickCount();
    LeaveCriticalSection(&gStoreCs);
    LogLine("频道填表[%s]: known=%d prefer=%d of %d rows", src ? src : "?", known, prefer, n);
}

ChannelPickHint Ccu_GetChannelPickHint(int zeroBasedIdx) {
    if (zeroBasedIdx < 0 || zeroBasedIdx >= kFillSlots) return ChannelPickHint::Neutral;
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    const FillSlot slot = gStore.fill[zeroBasedIdx];
    LeaveCriticalSection(&gStoreCs);
    if (slot.rejected) return ChannelPickHint::Avoid;
    if (!slot.known) return ChannelPickHint::Neutral;
    if (slot.adult) return ChannelPickHint::Avoid;
    if (slot.cap > 0 && slot.users >= slot.cap) return ChannelPickHint::Avoid;
    return ChannelPickHint::Prefer;
}

void Ccu_MarkChannelRejected(int zeroBasedIdx) {
    if (zeroBasedIdx < 0 || zeroBasedIdx >= kFillSlots) return;
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    FillSlot& slot = gStore.fill[zeroBasedIdx];
    if (!slot.rejected) {
        const bool wasPrefer = slot.known && !slot.adult &&
                               !(slot.cap > 0 && slot.users >= slot.cap);
        slot.rejected = 1;
        if (wasPrefer && gStore.fillPrefer > 0) --gStore.fillPrefer;
    }
    LeaveCriticalSection(&gStoreCs);
    LogLine("mark rejected idx=%d (ch.%d)", zeroBasedIdx, zeroBasedIdx + 1);
}

void Ccu_Init() {
    EnsureCs();
    OpenLog();
    gDone = false;
    gLastWaitLog = 0;
    EnterCriticalSection(&gStoreCs);
    gStore = Store{};
    LeaveCriticalSection(&gStoreCs);
    LogLine("Init (feed-only: wait auto_enter PickLeast)");
}

void Ccu_Shutdown() {
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void Ccu_Tick(DWORD now) {
    if (AlreadyLatched()) return;
    // 未 latch：不 Probe；只等 auto_enter 喂数。SHM 由 PayloadStatus 发布线程写。
    if (!gLastWaitLog || now - gLastWaitLog >= kWaitLogMs) {
        gLastWaitLog = now;
        LogLine("waiting auto_enter feed…");
    }
}

CcuStatus Ccu_GetStatus() {
    CcuStatus s{};
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    s.worldChannelOnline = gStore.sum;
    s.worldChannelCount = gStore.count;
    s.fillKnown = gStore.fillKnown;
    s.fillPrefer = gStore.fillPrefer;
    const DWORD updateTick = gStore.updateTick;
    const DWORD fillTick = gStore.fillTick;
    LeaveCriticalSection(&gStoreCs);
    const DWORD now = GetTickCount();
    if (updateTick) {
        s.worldChannelAgeSec = (now - updateTick) / 1000u;
    }
    if (fillTick) {
        s.fillAgeSec = (now - fillTick) / 1000u;
    }
    return s;
}

}  // namespace ccu
}  // namespace features
}  // namespace x
