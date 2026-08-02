// Classic TWMS timed_keys — slot queue + KeyDownTouch pulse (fengxing logic port).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "timed_keys.h"

#include "../ports/attack_input_port.h"
#include "../ports/input_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
#include "../../ipc/payload_timed_keys.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x::features::timed_keys {
namespace {

constexpr DWORD kRetryDelayMs = 500u;
constexpr DWORD kRetryBackoffCapMs = 8000u;
constexpr uint32_t kMaxConsecutiveFails = 8u;
constexpr DWORD kQueueSpacingMs = 150u;
constexpr DWORD kQueueSnapshotEveryMs = 10000u;
constexpr DWORD kWorkerSleepMs = 20u;
// 发键前后短暂停战斗（对齐 buffs 800ms）：避免与 Melee/瞬移抢动作。
constexpr DWORD kCombatHoldAfterKeyMs = 800u;

struct SlotRuntime {
    uint32_t intervalMs = 0;
    uint32_t vk = 0;
    DWORD nextTick = 0;
    uint32_t failCount = 0;
    bool enabled = false;
    bool queued = false;
    bool halted = false;
    bool persistNeeded = false;
};

SlotRuntime g_slots[xcat::kTimedKeySlotCount]{};
bool g_active = false;
bool g_wasActive = false;
bool g_wasLanded = false;
size_t g_rrStart = 0;
DWORD g_nextFireAt = 0;
DWORD g_nextSnapshotAt = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

struct QueuedSlot {
    size_t index = 0;
    uint32_t vk = 0;
    uint32_t intervalMs = 0;
    bool valid = false;
};

QueuedSlot g_queue[xcat::kTimedKeySlotCount]{};
size_t g_queueHead = 0;
size_t g_queueCount = 0;

struct DueSlot {
    size_t index = 0;
    uint32_t vk = 0;
    uint32_t intervalMs = 0;
    bool valid = false;
};

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

bool gCombatHeldForKey = false;
DWORD gCombatHoldUntil = 0;

void HoldCombatForTimedKey(DWORD now) {
    // 会话内只 Acquire 一次；续期只推后 until，避免深度泄漏。
    if (!gCombatHeldForKey) {
        simple_combat::AcquireExternalPause();
        gCombatHeldForKey = true;
    }
    gCombatHoldUntil = now + kCombatHoldAfterKeyMs;
}

void ReleaseCombatHoldIfDue(DWORD now) {
    if (!gCombatHeldForKey) return;
    if (static_cast<int>(now - gCombatHoldUntil) < 0) return;
    simple_combat::ReleaseExternalPause();
    gCombatHeldForKey = false;
    gCombatHoldUntil = 0;
}

void ReleaseCombatHoldNow() {
    if (!gCombatHeldForKey) return;
    simple_combat::ReleaseExternalPause();
    gCombatHeldForKey = false;
    gCombatHoldUntil = 0;
}

struct ExclusiveLock {
    explicit ExclusiveLock(SRWLOCK& lock) : lockRef(lock) { AcquireSRWLockExclusive(&lockRef); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lockRef); }
    SRWLOCK& lockRef;
};

uint32_t NextDelayMs(uint32_t intervalMs);

void ClearQueueLocked() {
    for (auto& slot : g_slots) slot.queued = false;
    for (auto& entry : g_queue) entry = {};
    g_queueHead = 0;
    g_queueCount = 0;
}

void RemoveQueuedSlotLocked(size_t index) {
    if (index >= xcat::kTimedKeySlotCount || g_queueCount == 0) {
        if (index < xcat::kTimedKeySlotCount) g_slots[index].queued = false;
        return;
    }
    QueuedSlot kept[xcat::kTimedKeySlotCount]{};
    size_t keptCount = 0;
    for (size_t n = 0; n < g_queueCount; ++n) {
        const size_t pos = (g_queueHead + n) % xcat::kTimedKeySlotCount;
        const auto entry = g_queue[pos];
        if (!entry.valid || entry.index == index) continue;
        kept[keptCount++] = entry;
    }
    for (auto& entry : g_queue) entry = {};
    g_queueHead = 0;
    g_queueCount = keptCount;
    for (size_t n = 0; n < keptCount; ++n) g_queue[n] = kept[n];
    g_slots[index].queued = false;
}

bool EnqueueSlotLocked(size_t index, DWORD now, const char* reason) {
    if (index >= xcat::kTimedKeySlotCount) return false;
    auto& slot = g_slots[index];
    if (!slot.enabled || slot.queued || slot.halted) return false;
    if (g_queueCount >= xcat::kTimedKeySlotCount) {
        runtime::LogW("TimedKeys", "queue full, drop '%s' reason=%s",
                      xcat::TimedKeySlotLabel(index), reason ? reason : "?");
        return false;
    }
    const size_t pos = (g_queueHead + g_queueCount) % xcat::kTimedKeySlotCount;
    g_queue[pos] = QueuedSlot{index, slot.vk, slot.intervalMs, true};
    ++g_queueCount;
    slot.queued = true;
    slot.nextTick = now + NextDelayMs(slot.intervalMs);
    runtime::LogI("TimedKeys", "queue '%s' reason=%s depth=%u", xcat::TimedKeySlotLabel(index),
                  reason ? reason : "?", static_cast<unsigned>(g_queueCount));
    return true;
}

void QueueLandingImmediateLocked(DWORD now, const char* reason) {
    if (!g_active) return;
    int queued = 0;
    for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
        auto& slot = g_slots[i];
        if (!slot.enabled || slot.queued || slot.halted) continue;
        if (g_queueCount >= xcat::kTimedKeySlotCount) break;
        const size_t pos = (g_queueHead + g_queueCount) % xcat::kTimedKeySlotCount;
        g_queue[pos] = QueuedSlot{i, slot.vk, slot.intervalMs, true};
        ++g_queueCount;
        slot.queued = true;
        slot.nextTick = now + NextDelayMs(slot.intervalMs);
        ++queued;
    }
    if (queued > 0) runtime::LogI("TimedKeys", "%s → queued %d slot(s)", reason, queued);
}

bool DequeueReadyLocked(DWORD now, DueSlot& due) {
    if (g_queueCount == 0) return false;
    if (g_nextFireAt != 0 && static_cast<int>(now - g_nextFireAt) < 0) return false;

    const QueuedSlot entry = g_queue[g_queueHead];
    g_queue[g_queueHead] = {};
    g_queueHead = (g_queueHead + 1u) % xcat::kTimedKeySlotCount;
    --g_queueCount;
    if (entry.index < xcat::kTimedKeySlotCount) g_slots[entry.index].queued = false;
    if (!entry.valid) return false;

    due.index = entry.index;
    due.vk = entry.vk;
    due.intervalMs = entry.intervalMs;
    due.valid = true;
    return true;
}

void LogQueueSnapshotLocked(DWORD now) {
    if (g_nextSnapshotAt != 0 && static_cast<int>(now - g_nextSnapshotAt) < 0) return;
    g_nextSnapshotAt = now + kQueueSnapshotEveryMs;

    char slots[160]{};
    size_t used = 0;
    for (size_t i = 0; i < xcat::kTimedKeySlotCount && used < sizeof(slots); ++i) {
        const auto& slot = g_slots[i];
        if (!slot.enabled) continue;
        const int remainMs = static_cast<int>(slot.nextTick - now);
        const int sec = remainMs > 0 ? (remainMs + 999) / 1000 : 0;
        const int n = snprintf(slots + used, sizeof(slots) - used, "%s%s:%ds%s", used ? "," : "",
                               xcat::TimedKeySlotLabel(i), sec, slot.queued ? "*" : "");
        if (n <= 0) break;
        used += static_cast<size_t>(n);
    }
    if (used == 0) snprintf(slots, sizeof(slots), "none");
    const int fireRemainMs = static_cast<int>(g_nextFireAt - now);
    runtime::LogI("TimedKeys", "snapshot active=%d depth=%u nextFire=%dms slots=%s",
                  g_active ? 1 : 0, static_cast<unsigned>(g_queueCount),
                  fireRemainMs > 0 ? fireRemainMs : 0, slots);
}

void InitSlotDefaults() {
    ExclusiveLock lock(g_lock);
    xcat::TimedKeysConfig defaults{};
    xcat::TimedKeysSetDefaults(defaults);
    for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
        g_slots[i].intervalMs = defaults.slots[i].intervalMs;
        g_slots[i].vk = defaults.slots[i].vk;
        g_slots[i].nextTick = 0;
        g_slots[i].failCount = 0;
        g_slots[i].enabled = false;
        g_slots[i].queued = false;
        g_slots[i].halted = false;
        g_slots[i].persistNeeded = false;
    }
    ClearQueueLocked();
    g_active = false;
    g_wasActive = false;
    g_wasLanded = false;
    g_rrStart = 0;
    g_nextFireAt = 0;
    g_nextSnapshotAt = 0;
}

uint32_t NextDelayMs(uint32_t intervalMs) {
    constexpr uint32_t kJitterMs = 500u;
    const int span = static_cast<int>(kJitterMs * 2u + 1u);
    const int jitter = (std::rand() % span) - static_cast<int>(kJitterMs);
    int jittered = static_cast<int>(intervalMs) + jitter;
    const int minMs = static_cast<int>(xcat::kTimedKeysMinIntervalSec * 1000u);
    if (jittered < minMs) jittered = minMs;
    return static_cast<uint32_t>(jittered);
}

DWORD HoldMsForSlot() { return 180u + static_cast<DWORD>(std::rand() % 61u); }

void ReleaseSlotKey(uint32_t vkRaw) {
    const WORD vk = static_cast<WORD>(vkRaw & 0xFFFFu);
    if (!vk) return;
    ports::input::ForceReleaseVk(vk);
}

bool PersistDisableSlot(size_t index) {
    if (index >= xcat::kTimedKeySlotCount) return false;
    const char* binDir = runtime::GetBinDir();
    for (int attempt = 0; attempt < 3; ++attempt) {
        xcat::TimedKeysConfig cfg{};
        if (!xcat::ReadTimedKeys(binDir, cfg)) {
            runtime::LogWThrottled(221, 5000, "TimedKeys", "halt persist '%s': read cfg fail",
                                   xcat::TimedKeySlotLabel(index));
            return false;
        }
        if (cfg.slots[index].enabled == 0) return true;
        const uint64_t seenTick = cfg.writeTickMs;
        cfg.slots[index].enabled = 0;
        xcat::TimedKeysNormalizeMasterEnabled(cfg);
        bool conflict = false;
        if (!xcat::WriteTimedKeysCas(binDir, cfg, seenTick, &conflict)) {
            if (conflict) continue;
            runtime::LogWThrottled(222, 5000, "TimedKeys", "halt persist '%s': write cfg fail",
                                   xcat::TimedKeySlotLabel(index));
            return false;
        }
        xcat::TimedKeysConfig after{};
        if (!xcat::ReadTimedKeys(binDir, after)) return false;
        if (after.slots[index].enabled == 0) {
            runtime::LogI("TimedKeys", "halt persist '%s' → disabled in user.ini (attempt %d)",
                          xcat::TimedKeySlotLabel(index), attempt + 1);
            return true;
        }
    }
    runtime::LogWThrottled(223, 5000, "TimedKeys",
                           "halt persist '%s': still enabled after cas retries",
                           xcat::TimedKeySlotLabel(index));
    return false;
}

void FlushPersistNeeded(DWORD now) {
    constexpr DWORD kPersistRetryMs = 2000u;
    static DWORD s_nextRetryAt = 0;
    size_t pending[xcat::kTimedKeySlotCount]{};
    size_t pendingCount = 0;
    {
        ExclusiveLock lock(g_lock);
        for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
            if (!g_slots[i].persistNeeded) continue;
            pending[pendingCount++] = i;
        }
    }
    if (pendingCount == 0) {
        s_nextRetryAt = 0;
        return;
    }
    if (s_nextRetryAt != 0 && static_cast<int>(now - s_nextRetryAt) < 0) return;

    bool anyFail = false;
    for (size_t n = 0; n < pendingCount; ++n) {
        const size_t index = pending[n];
        if (!PersistDisableSlot(index)) {
            anyFail = true;
            continue;
        }
        ExclusiveLock lock(g_lock);
        if (index < xcat::kTimedKeySlotCount) g_slots[index].persistNeeded = false;
    }
    s_nextRetryAt = anyFail ? (now + kPersistRetryMs) : 0;
}

enum class LandBlock : int {
    Ok = 0,
    NoPlay,
    VitalsFail,
    NotLatched,
    Dead,
};

LandBlock EvalPlayLanded(DWORD now, x::ui::player::Vitals* outVit) {
    if (outVit) *outVit = {};
    if (!ports::world::IsPlayReady()) return LandBlock::NoPlay;
    x::ui::player::Vitals vit{};
    if (!x::ui::player::ResolveAndRead(vit, now, false)) return LandBlock::VitalsFail;
    x::ui::player::NoteSample(vit, now);
    if (outVit) *outVit = vit;
    if (x::ui::player::IsDead(vit)) return LandBlock::Dead;
    if (!x::ui::player::IsReadyLatched()) return LandBlock::NotLatched;
    return LandBlock::Ok;
}

const char* LandBlockName(LandBlock b) {
    switch (b) {
        case LandBlock::Ok:
            return "ok";
        case LandBlock::NoPlay:
            return "no_play";
        case LandBlock::VitalsFail:
            return "vitals_fail";
        case LandBlock::NotLatched:
            return "not_latched";
        case LandBlock::Dead:
            return "dead";
        default:
            return "?";
    }
}

void TickOnce(DWORD now) {
    ports::input::TickReleases(now);
    x::ipc::PayloadTimedKeys_Poll();
    ReleaseCombatHoldIfDue(now);

    x::ui::player::Vitals landVit{};
    const LandBlock block = EvalPlayLanded(now, &landVit);
    const bool landed = (block == LandBlock::Ok);
    static DWORD s_lastLandLog = 0;
    if (!landed) {
        if (!s_lastLandLog || static_cast<int>(now - s_lastLandLog) >= 5000) {
            s_lastLandLog = now;
            runtime::LogI("TimedKeys",
                          "wait land reason=%s scene=%d wmAlive=%d hp=%d/%d latch=%d",
                          LandBlockName(block),
                          static_cast<int>(ports::world::GetSceneState()),
                          ports::world::IsAlive() ? 1 : 0, landVit.hp, landVit.mhp,
                          x::ui::player::IsReadyLatched() ? 1 : 0);
        }
    } else {
        s_lastLandLog = 0;
    }

    DueSlot due{};
    bool skipFire = false;

    {
        ExclusiveLock lock(g_lock);
        if (!landed) {
            g_wasLanded = false;
        } else if (!g_wasLanded) {
            g_wasLanded = true;
            QueueLandingImmediateLocked(now, "play path landed");
        }

        if (!g_active || !landed) {
            skipFire = true;
        } else {
            for (unsigned pass = 0; pass < xcat::kTimedKeySlotCount; ++pass) {
                const size_t i = (g_rrStart + pass) % xcat::kTimedKeySlotCount;
                auto& slot = g_slots[i];
                if (!slot.enabled || slot.halted) continue;
                if (static_cast<int>(now - slot.nextTick) >= 0) {
                    EnqueueSlotLocked(i, now, "interval");
                }
            }
            g_rrStart = (g_rrStart + 1u) % xcat::kTimedKeySlotCount;
            DequeueReadyLocked(now, due);
        }
    }

    if (skipFire || !due.valid) {
        FlushPersistNeeded(now);
        return;
    }

    // 先估 hold，再停战；InjectKeyHold 失败也保留短 pause，避免立刻被普攻打断。
    const DWORD holdMs = HoldMsForSlot();
    HoldCombatForTimedKey(now);
    (void)ports::attack::WaitFireIdle(80, 32);
    HoldCombatForTimedKey(GetTickCount());  // settle 后重算 until
    const WORD vk = static_cast<WORD>(due.vk & 0xFFFFu);
    const bool fired = vk && ports::input::InjectKeyHold(vk, holdMs);
    if (fired) {
        runtime::LogI("TimedKeys", "fire '%s' VK=0x%02X hold=%ums OK",
                      xcat::TimedKeySlotLabel(due.index), static_cast<unsigned>(vk),
                      static_cast<unsigned>(holdMs));
    }
    {
        ExclusiveLock lock(g_lock);
        auto& slot = g_slots[due.index];
        if (!slot.enabled || slot.vk != due.vk) {
            g_rrStart = (due.index + 1u) % xcat::kTimedKeySlotCount;
        } else if (!fired) {
            ++slot.failCount;
            if (slot.failCount >= kMaxConsecutiveFails) {
                slot.halted = true;
                slot.persistNeeded = true;
                runtime::LogW("TimedKeys",
                              "slot '%s' halted after %u consecutive fire fails "
                              "(uncheck persisted; re-enable to retry)",
                              xcat::TimedKeySlotLabel(due.index),
                              static_cast<unsigned>(kMaxConsecutiveFails));
            } else {
                uint32_t shift = slot.failCount - 1u;
                if (shift > 4u) shift = 4u;
                DWORD backoff = kRetryDelayMs << shift;
                if (backoff > kRetryBackoffCapMs) backoff = kRetryBackoffCapMs;
                slot.nextTick = now + backoff;
                runtime::LogWThrottled(220, 3000, "TimedKeys",
                                       "fire '%s' FAIL failCount=%u retryIn=%ums",
                                       xcat::TimedKeySlotLabel(due.index),
                                       static_cast<unsigned>(slot.failCount),
                                       static_cast<unsigned>(backoff));
            }
            g_rrStart = (due.index + 1u) % xcat::kTimedKeySlotCount;
        } else {
            slot.failCount = 0;
            slot.nextTick = now + NextDelayMs(due.intervalMs);
            g_nextFireAt = (g_queueCount > 0) ? (now + kQueueSpacingMs) : 0;
            g_rrStart = (due.index + 1u) % xcat::kTimedKeySlotCount;
            LogQueueSnapshotLocked(now);
        }
    }
    FlushPersistNeeded(now);
}

DWORD WINAPI WorkerProc(void*) {
    timeBeginPeriod(1);
    while (!gWorkerStop.load(std::memory_order_acquire)) {
        TickOnce(GetTickCount());
        Sleep(kWorkerSleepMs);
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    InitSlotDefaults();
    ports::input::Init();
    x::ui::player::Init();
    runtime::LogI("TimedKeys", "init (KeyDownTouch pulse; hold combat ~hold+250ms per fire)");
}

void Shutdown() {
    StopWorker();
    ReleaseCombatHoldNow();
    uint32_t keys[xcat::kTimedKeySlotCount]{};
    {
        ExclusiveLock lock(g_lock);
        for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) keys[i] = g_slots[i].vk;
    }
    for (uint32_t vk : keys) ReleaseSlotKey(vk);
    InitSlotDefaults();
    ports::input::Shutdown();
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE h = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    gWorkerThread.store(h);
}

void StopWorker() {
    gWorkerStop.store(true);
    ReleaseCombatHoldNow();
}

void ApplyConfig(const xcat::TimedKeysConfig& cfg) {
    uint32_t releaseKeys[xcat::kTimedKeySlotCount]{};
    size_t releaseCount = 0;
    {
        ExclusiveLock lock(g_lock);
        const DWORD now = GetTickCount();
        const bool wasActive = g_wasActive;
        const bool anySlot = xcat::TimedKeysAnySlotEnabled(cfg);
        g_active = anySlot || cfg.masterEnabled != 0;
        const bool masterRising = g_active && !wasActive;
        g_wasActive = g_active;

        for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
            const bool wasEnabled = g_slots[i].enabled;
            const bool newEnabled = cfg.slots[i].enabled != 0;
            const uint32_t newInterval = xcat::TimedKeysClampIntervalMs(cfg.slots[i].intervalMs);
            const uint32_t oldVk = g_slots[i].vk;

            if (wasEnabled && (!newEnabled || oldVk != cfg.slots[i].vk)) {
                releaseKeys[releaseCount++] = oldVk;
                RemoveQueuedSlotLocked(i);
            }

            g_slots[i].vk = cfg.slots[i].vk;
            if (oldVk != cfg.slots[i].vk) {
                g_slots[i].failCount = 0;
                g_slots[i].halted = false;
                g_slots[i].persistNeeded = false;
            }

            if (newInterval != g_slots[i].intervalMs) {
                g_slots[i].intervalMs = newInterval;
                if (newEnabled && wasEnabled) g_slots[i].nextTick = now + newInterval;
            }

            const bool slotRising = newEnabled && !wasEnabled;
            const bool fireOnce = slotRising || (masterRising && newEnabled);
            if (fireOnce) {
                g_slots[i].enabled = true;
                g_slots[i].failCount = 0;
                g_slots[i].halted = false;
                g_slots[i].persistNeeded = false;
                EnqueueSlotLocked(i, now, "enable");
                runtime::LogI("TimedKeys", "slot '%s' enabled → queued first, then every %us±0.5s",
                              xcat::TimedKeySlotLabel(i), g_slots[i].intervalMs / 1000u);
            } else if (!newEnabled && wasEnabled) {
                RemoveQueuedSlotLocked(i);
                g_slots[i].enabled = false;
                g_slots[i].failCount = 0;
                g_slots[i].halted = false;
                g_slots[i].persistNeeded = false;
                runtime::LogI("TimedKeys", "slot '%s' disabled", xcat::TimedKeySlotLabel(i));
            } else {
                g_slots[i].enabled = newEnabled;
                if (!newEnabled) {
                    g_slots[i].failCount = 0;
                    g_slots[i].halted = false;
                    g_slots[i].persistNeeded = false;
                }
            }
        }
    }
    for (size_t i = 0; i < releaseCount; ++i) ReleaseSlotKey(releaseKeys[i]);
}

bool IsMasterEnabled() {
    ExclusiveLock lock(g_lock);
    return g_active;
}

}  // namespace x::features::timed_keys
