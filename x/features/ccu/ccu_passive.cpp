// TWMS Classic CCU — latch 当前所选分区在线人数（login idle 或 auto_enter 喂数）。
// 不在本模块 FindAll；换分区可覆盖快照，同分区默认不重复写（软重连 allowRefresh 例外）。
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
    int32_t worldId = 0;  // 0 = 尚未绑定分区
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

// worldId：0 = 未知分区（仅首次可写）；非 0 时同分区默认拒覆盖，allowRefresh 时同区可刷新。
bool ApplyTotals(long long sum, int count, const char* src, int32_t worldId, bool allowRefresh) {
    if (count <= 0 || sum < 0) return false;
    const DWORD now = GetTickCount();
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    const bool had = gDone && gStore.sum >= 0;
    if (had) {
        if (worldId == 0) {
            LeaveCriticalSection(&gStoreCs);
            return false;  // 无分区 id 时不覆盖已有快照
        }
        if (gStore.worldId != 0 && worldId == gStore.worldId && !allowRefresh) {
            LeaveCriticalSection(&gStoreCs);
            return false;  // 同分区已采过
        }
    }
    const int32_t prevWorld = gStore.worldId;
    const long long prevSum = gStore.sum;
    const bool worldChanged = (had && worldId != 0 && prevWorld != 0 && prevWorld != worldId);
    const bool sameWorldRefresh =
        (had && allowRefresh && worldId != 0 && prevWorld == worldId);
    gStore.sum = sum;
    gStore.count = count;
    gStore.worldId = worldId;
    gStore.updateTick = now;
    gDone = true;
    // 换分区：旧分区的拒收标记对新 ci 无意义。同区刷新保留拒收。
    if (worldChanged) {
        for (int i = 0; i < kFillSlots; ++i) gStore.fill[i].rejected = 0;
    }
    LeaveCriticalSection(&gStoreCs);
    if (worldChanged) {
        LogLine("频道快照[%s]: 换分区 %d→%d 在线=%lld channels=%d", src ? src : "?", prevWorld,
                worldId, sum, count);
    } else if (sameWorldRefresh) {
        LogLine("频道快照[%s]: 刷新 在线=%lld (was %lld) channels=%d worldId=%d",
                src ? src : "?", sum, prevSum, count, worldId);
    } else {
        LogLine("频道快照[%s]: 分区在线=%lld channels=%d worldId=%d", src ? src : "?", sum, count,
                worldId);
    }
    LogLine("published — SHM via PayloadStatus publisher");
    return true;
}

bool AlreadyLatched() {
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    const bool ok = gDone && gStore.sum >= 0;
    LeaveCriticalSection(&gStoreCs);
    return ok;
}

}  // namespace

bool Ccu_ShouldSkipFeed(int32_t worldId, bool allowRefresh) {
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    const bool had = gDone && gStore.sum >= 0;
    const int32_t latched = gStore.worldId;
    LeaveCriticalSection(&gStoreCs);
    if (!had) return false;
    if (worldId == 0) return true;  // 无 id：不能换区/升级，防空刷
    if (latched == worldId && !allowRefresh) return true;  // 同分区默认 latch
    return false;                                          // 换区、0→真 id，或软重连刷新
}

bool Ccu_NotifySnapshot(long long sum, int channelCount, const char* src, int32_t worldId,
                        bool allowRefresh) {
    OpenLog();
    return ApplyTotals(sum, channelCount, src && src[0] ? src : "feed", worldId, allowRefresh);
}

void Ccu_NotifyFillTable(const ChannelFillRow* rows, int n, const char* src, int32_t worldId) {
    if (!rows || n <= 0) return;
    EnsureCs();
    OpenLog();
    EnterCriticalSection(&gStoreCs);
    // 拒收清理由 ApplyTotals 在换分区时完成；此处同分区保留。
    uint8_t keepRejected[kFillSlots]{};
    for (int i = 0; i < kFillSlots; ++i) {
        keepRejected[i] = gStore.fill[i].rejected;
        gStore.fill[i] = FillSlot{};
    }
    if (worldId != 0) gStore.worldId = worldId;
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
    for (int i = 0; i < kFillSlots; ++i) {
        if (gStore.fill[i].known) continue;
        if (keepRejected[i]) gStore.fill[i].rejected = 1;
    }
    gStore.fillKnown = known;
    gStore.fillPrefer = prefer;
    gStore.fillTick = GetTickCount();
    LeaveCriticalSection(&gStoreCs);
    LogLine("频道填表[%s]: known=%d prefer=%d of %d rows worldId=%d", src ? src : "?", known,
            prefer, n, worldId);
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
    LogLine("Init (wait login channel UI or auto_enter feed; world switch updates)");
}

void Ccu_Shutdown() {
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void Ccu_Tick(DWORD now) {
    if (AlreadyLatched()) return;
    if (!gLastWaitLog || now - gLastWaitLog >= kWaitLogMs) {
        gLastWaitLog = now;
        LogLine("waiting channel snapshot (login UI or auto_enter)…");
    }
}

CcuStatus Ccu_GetStatus() {
    CcuStatus s{};
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    s.worldChannelOnline = gStore.sum;
    s.worldChannelCount = gStore.count;
    s.worldId = gStore.worldId;
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

int32_t Ccu_SnapshotWorldId() {
    EnsureCs();
    EnterCriticalSection(&gStoreCs);
    const int32_t id = (gDone && gStore.sum >= 0) ? gStore.worldId : 0;
    LeaveCriticalSection(&gStoreCs);
    return id;
}

}  // namespace ccu
}  // namespace features
}  // namespace x
