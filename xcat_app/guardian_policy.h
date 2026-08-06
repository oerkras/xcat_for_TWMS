#pragma once

#include <cstdint>

namespace xcat::app::guardian_policy {

// Classic TWMS port of fengxing guardian_policy (no-exp restart decision core).
// Travel / auto-relogin hard-fail inputs are accepted but typically left false until
// those features publish matching PayloadStatus fields.

enum class Mode {
    Idle,
    Healthy,
    Recovering,
    Backoff,
};

struct RuntimeState {
    uint64_t lastExp = 0;
    uint64_t lastProgressTick = 0;
    uint64_t lastStatusReadyTick = 0;
    uint64_t statusLostTick = 0;
    uint64_t recoveryStartedTick = 0;
    uint64_t backoffUntilTick = 0;
    uint64_t travelActiveSinceTick = 0;
    uint64_t travelInactiveSinceTick = 0;
    uint64_t travelPauseTick = 0;
    uint32_t travelLastMapId = 0;
    uint32_t staleConfirm = 0;
    uint32_t statusStaleConfirm = 0;
    uint32_t recoveryAttempts = 0;
    bool haveExp = false;
    bool haveStatus = false;
    bool restartInFlight = false;
    Mode mode = Mode::Idle;
};

struct Input {
    uint64_t now = 0;
    uint32_t noExpSec = 180;
    uint32_t recoveryTimeoutSec = 180;
    uint32_t backoffBaseSec = 30;
    uint32_t backoffMaxSec = 300;
    bool launchWorkerBusy = false;
    bool injected = false;
    uint32_t gamePid = 0;
    bool processAlive = false;
    bool combatEnabled = false;
    bool scheduleActive = true;
    bool statusReady = false;
    bool progressGrace = false;
    // 已武装且进程仍在，但 Status SHM 心跳停更（整进程假死常见）。
    bool payloadHeartbeatStale = false;
    // 一键/重拉归属、冷启已结束、进程在但从未 localPlayerOk（选角/加载卡死，心跳可仍活）。
    bool prePlayableStuck = false;
    bool reloginHardFailed = false;
    bool freezeUnrecoverable = false;
    uint32_t hardFailCode = 0;
    bool charSelectStuck = false;
    bool playerExpValid = false;
    uint64_t playerExp = 0;
    uint64_t combatActivityTickMs = 0;
    int32_t mobSpawnSlots = -1;
    bool safeZonePause = false;
    bool travelActive = false;
    bool mapIdValid = false;
    uint32_t mapId = 0;
};

enum class Gate {
    None,
    LaunchWorkerBusy,
    NotInjected,
    ProcessDead,
    CombatOff,
    ScheduleOff,
    SafeZone,
    Travel,
    StatusNotReady,
    Progress,
    ExpStale,
    Recovering,
    Backoff,
};

enum class Action {
    None,
    Restart,
};

struct Decision {
    RuntimeState next;
    Mode previousMode = Mode::Idle;
    Gate gate = Gate::None;
    Action action = Action::None;
    const char* restartReason = nullptr;
    uint32_t staleSec = 0;
    uint32_t backoffSec = 0;
    bool warnStatusConfirm = false;
    bool warnExpConfirm = false;
    bool combatHold = false;
    bool stateChanged = false;
    bool enteredBackoff = false;
    // 踢线等 hard-fail：绕过「重启冷却」，尽快干净重拉。
    bool bypassRestartCooldown = false;
};

uint32_t CombatHoldHardLimitSec(uint32_t noExpSec);
Decision Evaluate(const RuntimeState& state, const Input& input);
const char* ModeLabel(Mode mode);
const char* GateLabel(Gate gate);

}  // namespace xcat::app::guardian_policy
