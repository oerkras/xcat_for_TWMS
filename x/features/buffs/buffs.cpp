// Classic TWMS buffs — skill-only renew FSM (fengxing logic port, Il2Cpp cast).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "buffs.h"

#include "../ports/attack_input_port.h"
#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
#include "../../ipc/payload_buffs.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x::features::buffs {
namespace {

constexpr uint32_t kSafeGapMs = 1200;
constexpr DWORD kVerifyTimeoutMs = 4500;
constexpr DWORD kNotReadyRetryMs = 700;
constexpr DWORD kRejectBackoffMinMs = 2000;
constexpr DWORD kRejectBackoffMaxMs = 12000;
constexpr DWORD kAssumedActiveVerifyMs = kVerifyTimeoutMs;
constexpr float kPresenceRenewRemainSec = 5.f;
constexpr DWORD kLightIntervalMs = 1000;
constexpr DWORD kHeavyMinIntervalMs = 3000;
constexpr DWORD kWorkerSleepMs = 50;
constexpr DWORD kProbeLogMs = 15000;
constexpr DWORD kNotReadyLogThrottleMs = 2000;
// 施法前后短暂停战斗：BIN（a20d2e）master 开后 DoActive 插在 Melee 风暴中 → 客户报卡住。
// 对照枫星 HoldCombatForBuffCast；本仓用 AcquireExternalPause（可与 timed_keys 重叠）。
// Hold 后 WaitFireIdle 再 Cast，堵住同拍 Firing / 主线程 Pulse 未完的缝。
constexpr DWORD kCombatHoldAfterCastMs = 800;
constexpr DWORD kCombatHoldSettleTimeoutMs = 80;
constexpr DWORD kCombatHoldSettleAfterFireMs = 32;

enum class SlotState : uint32_t { Idle = 0, Retry, AwaitVerify };

struct SlotRuntime {
    uint32_t enabled = 0;
    uint32_t kind = xcat::kBuffKindSkill;
    char code[64]{};
    int skillId = 0;
    uint32_t intervalMs = 180000;
    uint32_t strategy = xcat::kBuffRenewByPresence;
    DWORD nextTick = 0;
    uint32_t forceOnce = 0;
    SlotState state = SlotState::Idle;
    DWORD retryAt = 0;
    DWORD verifyDeadline = 0;
    DWORD castAt = 0;
    uint32_t backoffMs = 0;
    DWORD assumedActiveUntil = 0;
    DWORD lastNotReadyLogAt = 0;
    char lastNotReadyReason[48]{};

    void ResetFsm() {
        state = SlotState::Idle;
        retryAt = 0;
        verifyDeadline = 0;
        castAt = 0;
        backoffMs = 0;
        assumedActiveUntil = 0;
        lastNotReadyLogAt = 0;
        lastNotReadyReason[0] = 0;
    }
};

SlotRuntime g_slots[xcat::kBuffSlotCount]{};
bool g_active = false;
std::mutex g_pendingMu;
xcat::BuffsConfig g_pendingCfg{};
bool g_pendingDirty = false;
DWORD g_nextCastAt = 0;
DWORD g_nextLightAt = 0;
DWORD g_nextHeavyAt = 0;
uint32_t g_appliedRefreshSeq = 0;
uint32_t g_ackRefreshSeq = 0;
bool g_heavyPending = false;

xcat::BuffsRuntimeSnapshot g_runtime{};
SRWLOCK g_runtimeLock = SRWLOCK_INIT;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gCastingBusy{false};
bool gCombatHeldForBuff = false;
DWORD gCombatHoldUntil = 0;

void HoldCombatForBuffCast(DWORD now) {
    if (!gCombatHeldForBuff) {
        simple_combat::AcquireExternalPause();
        gCombatHeldForBuff = true;
    }
    gCombatHoldUntil = now + kCombatHoldAfterCastMs;
}

void ReleaseCombatHoldIfDue(DWORD now) {
    if (!gCombatHeldForBuff) return;
    if (static_cast<int>(now - gCombatHoldUntil) < 0) return;
    simple_combat::ReleaseExternalPause();
    gCombatHeldForBuff = false;
    gCombatHoldUntil = 0;
}

void ReleaseCombatHoldNow() {
    if (!gCombatHeldForBuff) return;
    simple_combat::ReleaseExternalPause();
    gCombatHeldForBuff = false;
    gCombatHoldUntil = 0;
}

struct ExclusiveLock {
    explicit ExclusiveLock(SRWLOCK& lock) : lockRef(lock) { AcquireSRWLockExclusive(&lockRef); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lockRef); }
    SRWLOCK& lockRef;
};

struct SharedLock {
    explicit SharedLock(SRWLOCK& lock) : lockRef(lock) { AcquireSRWLockShared(&lockRef); }
    ~SharedLock() { ReleaseSRWLockShared(&lockRef); }
    SRWLOCK& lockRef;
};

int ParseSkillId(const char* code) {
    if (!code || !code[0]) return 0;
    int v = 0;
    for (const char* p = code; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 2000000000) return 0;
    }
    return v;
}

bool Landed() {
    // 玩法就绪优先；再 MyUser/vitals（防登录/商城误放技能）
    if (!ports::world::IsPlayReady()) return false;
    if (ports::skill::Ready()) return true;
    if (x::ui::player::IsReadyLatched()) return true;
    x::ui::player::Vitals v{};
    if (x::ui::player::ReadCached(v) && v.ok && !x::ui::player::IsDead(v) && v.level >= 1)
        return true;
    return false;
}

bool EffectActive(int skillId, float* remain) {
    return ports::skill::IsSkillActive(skillId, remain);
}

void PublishRuntime(const xcat::BuffsRuntimeSnapshot& snap) {
    {
        ExclusiveLock lk(g_runtimeLock);
        g_runtime = snap;
    }
    (void)xcat::WriteBuffsRuntimeSnapshot(runtime::GetBinDir(), snap);
}

void BuildRuntimeSnapshot(bool heavy, DWORD now) {
    xcat::BuffsRuntimeSnapshot snap{};
    xcat::BuffsRuntimeSnapshotSetDefaults(snap);
    snap.writeTickMs = now;
    snap.refreshAckSeq = g_ackRefreshSeq;
    strncpy_s(snap.status, heavy ? "heavy ok" : "light ok", _TRUNCATE);

    ports::skill::SkillInfoLite learned[xcat::kBuffsRuntimeMaxSkills]{};
    int n = 0;
    if (heavy) {
        n = ports::skill::ListLearnedSkills(learned, static_cast<int>(xcat::kBuffsRuntimeMaxSkills));
    } else {
        // light：刷新已知 code（配置槽 + 上次 runtime）
        char codes[xcat::kBuffsRuntimeMaxSkills][32]{};
        int codeN = 0;
        for (size_t i = 0; i < xcat::kBuffSlotCount && codeN < (int)xcat::kBuffsRuntimeMaxSkills; ++i) {
            if (!g_slots[i].code[0]) continue;
            bool dup = false;
            for (int j = 0; j < codeN; ++j) {
                if (strcmp(codes[j], g_slots[i].code) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            strncpy_s(codes[codeN], g_slots[i].code, _TRUNCATE);
            ++codeN;
        }
        {
            SharedLock lk(g_runtimeLock);
            for (uint32_t i = 0; i < g_runtime.count && codeN < (int)xcat::kBuffsRuntimeMaxSkills; ++i) {
                if (!g_runtime.skills[i].code[0]) continue;
                bool dup = false;
                for (int j = 0; j < codeN; ++j) {
                    if (strcmp(codes[j], g_runtime.skills[i].code) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                strncpy_s(codes[codeN], g_runtime.skills[i].code, _TRUNCATE);
                ++codeN;
            }
        }
        for (int i = 0; i < codeN; ++i) {
            const int id = ParseSkillId(codes[i]);
            if (id <= 0) continue;
            auto& s = learned[n++];
            s = {};
            s.skillId = id;
            s.level = ports::skill::GetSkillLevel(id);
            s.learned = s.level > 0;
            float remain = 0.f;
            s.active = ports::skill::IsSkillActive(id, &remain);
            s.remainBuffSec = remain;
            s.remainCooldownSec = ports::skill::GetSkillCooldownRemainSec(id);
            s.cooldownSec = s.remainCooldownSec;
            snprintf(s.code, sizeof(s.code), "%d", id);
            ports::skill::ResolveSkillName(id, s.name, sizeof(s.name));
        }
    }

    uint32_t count = 0;
    for (int i = 0; i < n && count < xcat::kBuffsRuntimeMaxSkills; ++i) {
        if (!learned[i].learned && !learned[i].active) continue;
        auto& dst = snap.skills[count];
        strncpy_s(dst.code, learned[i].code, _TRUNCATE);
        strncpy_s(dst.name, learned[i].name[0] ? learned[i].name : learned[i].code, _TRUNCATE);
        strncpy_s(dst.typeAbbr, "BUFF", _TRUNCATE);
        dst.inJob = 1;
        dst.learned = learned[i].learned ? 1u : 0u;
        dst.show = 1;
        dst.active = learned[i].active ? 1u : 0u;
        dst.remainBuffSec = learned[i].remainBuffSec;
        dst.remainCooldownSec = learned[i].remainCooldownSec;
        dst.cooldownSec = learned[i].cooldownSec;
        dst.canCast = (learned[i].learned && learned[i].remainCooldownSec <= 0.01f) ? 1u : 0u;
        ++count;
    }
    snap.count = count;
    snap.ready = ports::skill::Ready() ? 1u : 0u;
    if (!snap.ready) strncpy_s(snap.status, "waiting MyUser", _TRUNCATE);
    PublishRuntime(snap);
}

void ApplyPendingConfig(DWORD now) {
    xcat::BuffsConfig cfg{};
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (!g_pendingDirty) return;
        cfg = g_pendingCfg;
        g_pendingDirty = false;
    }

    const bool was = g_active;
    g_active = cfg.masterEnabled != 0;
    if (cfg.refreshSeq != 0 && cfg.refreshSeq != g_appliedRefreshSeq) {
        g_appliedRefreshSeq = cfg.refreshSeq;
        g_heavyPending = true;
        g_nextHeavyAt = now;
    }
    if (!was && g_active) {
        g_heavyPending = true;
        g_nextHeavyAt = now;
    }

    for (size_t i = 0; i < xcat::kBuffSlotCount; ++i) {
        auto& s = g_slots[i];
        const auto& c = cfg.slots[i];
        const bool wasEn = s.enabled != 0;
        const int newId = ParseSkillId(c.code);
        const bool codeChanged = (newId != s.skillId) || (strcmp(s.code, c.code) != 0);
        s.enabled = (c.enabled != 0 && c.kind == xcat::kBuffKindSkill && newId > 0) ? 1u : 0u;
        s.kind = c.kind;
        strncpy_s(s.code, c.code, _TRUNCATE);
        s.skillId = newId;
        s.intervalMs = (c.intervalSec < 1 ? 1u : c.intervalSec) * 1000u;
        s.strategy = c.strategy;
        if (!s.enabled) {
            s.ResetFsm();
            s.forceOnce = 0;
            continue;
        }
        if (!wasEn || codeChanged) {
            s.ResetFsm();
            s.forceOnce = 1;
            s.nextTick = now;
        }
    }
    runtime::LogI("Buffs", "ApplyConfig master=%u refresh=%u", cfg.masterEnabled, cfg.refreshSeq);
}

bool SlotDue(const SlotRuntime& s, DWORD now, float remain) {
    if (!s.enabled || s.skillId <= 0) return false;
    if (s.state == SlotState::AwaitVerify) return false;
    if (s.state == SlotState::Retry && now < s.retryAt) return false;
    if (s.assumedActiveUntil && now < s.assumedActiveUntil) return false;

    const bool active = EffectActive(s.skillId, nullptr);
    const int level = ports::skill::GetSkillLevel(s.skillId);
    if (level <= 0) return false;

    if (s.forceOnce) return true;

    switch (s.strategy) {
    case xcat::kBuffRenewByCooldown:
        return !active;
    case xcat::kBuffRenewByInterval:
        return now >= s.nextTick;
    case xcat::kBuffRenewByPresence:
    default:
        if (!active) return true;
        return remain > 0.f && remain <= kPresenceRenewRemainSec;
    }
}

void EnterRetry(SlotRuntime& s, DWORD now, DWORD delayMs) {
    s.state = SlotState::Retry;
    s.retryAt = now + delayMs;
    s.assumedActiveUntil = 0;
}

void EnterVerify(SlotRuntime& s, DWORD now) {
    s.state = SlotState::AwaitVerify;
    s.castAt = now;
    s.verifyDeadline = now + kVerifyTimeoutMs;
    s.assumedActiveUntil = now + kAssumedActiveVerifyMs;
    s.forceOnce = 0;
}

void TickVerify(SlotRuntime& s, DWORD now) {
    if (s.state != SlotState::AwaitVerify) return;
    float remain = 0.f;
    if (EffectActive(s.skillId, &remain)) {
        s.state = SlotState::Idle;
        s.backoffMs = 0;
        s.assumedActiveUntil = 0;
        s.nextTick = now + s.intervalMs;
        runtime::LogI("Buffs", "verify ok skill=%d remain=%.1f", s.skillId, remain);
        return;
    }
    if (now < s.verifyDeadline) return;
    // 施放已 ok 且肉眼有图标，但 AffectedList/SS 偶发读不到：信任本轮，避免 reject 风暴。
    s.state = SlotState::Idle;
    s.backoffMs = 0;
    s.assumedActiveUntil = now + s.intervalMs;
    s.nextTick = now + s.intervalMs;
    runtime::LogW("Buffs", "verify soft skill=%d (cast ok, presence miss)", s.skillId);
}

bool TryCastSlot(size_t idx, DWORD now) {
    auto& s = g_slots[idx];
    if (now < g_nextCastAt) return false;
    HoldCombatForBuffCast(now);
    // 先停刀闸 + 松键，再短等近期出刀窗口结束，再上主线程 DoActive。
    (void)ports::attack::WaitFireIdle(kCombatHoldSettleTimeoutMs, kCombatHoldSettleAfterFireMs);
    gCastingBusy = true;
    bool notReady = false;
    char reason[48]{};
    const bool ok = ports::skill::CastSkill(s.skillId, &notReady, reason, sizeof(reason));
    gCastingBusy = false;
    // 以施法返回时刻起算 Hold，避免 settle 吃掉事后窗口。
    HoldCombatForBuffCast(GetTickCount());
    if (ok) {
        g_nextCastAt = GetTickCount() + kSafeGapMs;
        EnterVerify(s, GetTickCount());
        runtime::LogI("Buffs", "cast ok skill=%d slot=%zu reason=%s", s.skillId, idx + 1,
                      reason[0] ? reason : "ok");
        return true;
    }
    // 失败也保持短暂停战，避免下一拍立刻 OnFuncKey 打断失败态
    if (notReady) {
        g_nextCastAt = GetTickCount() + 300;
        EnterRetry(s, GetTickCount(), kNotReadyRetryMs);
        const char* why = reason[0] ? reason : "?";
        const bool reasonChanged = (strcmp(why, s.lastNotReadyReason) != 0);
        if (reasonChanged || GetTickCount() - s.lastNotReadyLogAt >= kNotReadyLogThrottleMs) {
            runtime::LogW("Buffs", "cast not_ready skill=%d reason=%s", s.skillId, why);
            s.lastNotReadyLogAt = GetTickCount();
            strncpy_s(s.lastNotReadyReason, why, _TRUNCATE);
        }
        return false;
    }
    if (s.backoffMs == 0) s.backoffMs = kRejectBackoffMinMs;
    else {
        s.backoffMs *= 2;
        if (s.backoffMs > kRejectBackoffMaxMs) s.backoffMs = kRejectBackoffMaxMs;
    }
    g_nextCastAt = GetTickCount() + kSafeGapMs;
    EnterRetry(s, GetTickCount(), s.backoffMs);
    runtime::LogW("Buffs", "cast fail skill=%d backoff=%u reason=%s", s.skillId, s.backoffMs,
                  reason[0] ? reason : "?");
    return false;
}

void TickOnce(DWORD now) {
    ApplyPendingConfig(now);
    ports::skill::EnsureBound();
    ReleaseCombatHoldIfDue(now);

    if (g_heavyPending && now >= g_nextHeavyAt) {
        BuildRuntimeSnapshot(true, now);
        g_heavyPending = false;
        g_ackRefreshSeq = g_appliedRefreshSeq;
        g_nextLightAt = now + kLightIntervalMs;
        g_nextHeavyAt = now + kHeavyMinIntervalMs;
        uint32_t listed = 0;
        {
            ExclusiveLock lk(g_runtimeLock);
            g_runtime.refreshAckSeq = g_ackRefreshSeq;
            g_runtime.writeTickMs = now;
            strncpy_s(g_runtime.status, "heavy ok", _TRUNCATE);
            listed = g_runtime.count;
            (void)xcat::WriteBuffsRuntimeSnapshot(runtime::GetBinDir(), g_runtime);
        }
        runtime::LogI("Buffs", "heavy ok listed=%u ack=%u", listed, g_ackRefreshSeq);
    } else if (now >= g_nextLightAt) {
        BuildRuntimeSnapshot(false, now);
        g_nextLightAt = now + kLightIntervalMs;
    }

    static DWORD s_lastProbe = 0;
    if (now - s_lastProbe > kProbeLogMs) {
        s_lastProbe = now;
        ports::skill::ActiveSkill act[32]{};
        const int n = ports::skill::ListActiveSkills(act, 32);
        runtime::LogI("Buffs", "probe landed=%d scene=%d activeN=%d master=%d", Landed() ? 1 : 0,
                      static_cast<int>(ports::world::GetSceneState()), n, g_active ? 1 : 0);
    }

    if (!g_active || !Landed()) return;

    for (size_t i = 0; i < xcat::kBuffSlotCount; ++i) TickVerify(g_slots[i], now);

    for (size_t i = 0; i < xcat::kBuffSlotCount; ++i) {
        auto& s = g_slots[i];
        if (!s.enabled) continue;
        if (s.state == SlotState::Retry && now >= s.retryAt) s.state = SlotState::Idle;
        float remain = 0.f;
        EffectActive(s.skillId, &remain);
        if (!SlotDue(s, now, remain)) continue;
        if (TryCastSlot(i, now)) break;  // one cast per tick wave
    }
}

DWORD WINAPI WorkerMain(void*) {
    timeBeginPeriod(1);
    runtime::LogI("Buffs", "worker start (skill-only Prepare→verify)");
    while (!gWorkerStop.load()) {
        x::ipc::PayloadBuffs_Poll();
        TickOnce(GetTickCount());
        Sleep(kWorkerSleepMs);
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    ports::skill::Init();
    runtime::LogI("Buffs", "init (classic: AffectedSkill + DoActiveSkillPrepare)");
}

void Shutdown() {
    StopWorker();
    ports::skill::Shutdown();
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop = false;
    HANDLE th = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
    gWorkerThread = th;
}

void StopWorker() {
    gWorkerStop = true;
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) {
        // DllMain: never join under loader lock — just signal.
        CloseHandle(th);
    }
    ReleaseCombatHoldNow();
}

void ApplyConfig(const xcat::BuffsConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_pendingMu);
    g_pendingCfg = cfg;
    g_pendingDirty = true;
}

bool IsMasterEnabled() { return g_active; }

bool IsCastingBusy() { return gCastingBusy.load(); }

}  // namespace x::features::buffs
