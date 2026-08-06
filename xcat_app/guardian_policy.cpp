#include "guardian_policy.h"

#include "xcat_payload_status.h"

namespace xcat::app::guardian_policy {

namespace {

constexpr const char* kReasonProcessDead = "游戏进程已退出";
constexpr const char* kReasonStatusStale = "payload 心跳/状态停滞";
constexpr const char* kReasonPayloadHung = "游戏假死(payload 心跳停滞)";
constexpr const char* kReasonPrePlayableStuck = "长期未进图(选角/加载超时)";
constexpr const char* kReasonExpStale = "EXP 停滞";
constexpr const char* kReasonRecoveryRetry = "守护恢复重试";
constexpr const char* kReasonRecoveryTimeout = "守护恢复超时";
constexpr const char* kReasonReloginStuck = "重进/选角流程卡住";
constexpr const char* kReasonCharSelectStuck = "选角流程超时";
constexpr const char* kReasonFreezeUnrecoverable = "冻结不可恢复";
constexpr const char* kReasonServerKick = "服务器踢线/断线";
constexpr const char* kReasonTravelStuck = "超级赶路超时(软卡死)";

uint32_t TravelStuckCapSec(const Input& input) {
    const uint32_t noExp = input.noExpSec == 0 ? 180u : input.noExpSec;
    const uint32_t scaled = noExp * 10u;
    return scaled > 1200u ? scaled : 1200u;
}

constexpr uint32_t kTravelInactiveGraceSec = 12u;

void ClearTravelRuntime(RuntimeState& state) {
    state.travelActiveSinceTick = 0;
    state.travelInactiveSinceTick = 0;
    state.travelPauseTick = 0;
    state.travelLastMapId = 0;
}

void ClearProgress(RuntimeState& state) {
    state.lastExp = 0;
    state.lastProgressTick = 0;
    state.staleConfirm = 0;
    state.haveExp = false;
}

void ClearStatus(RuntimeState& state) {
    state.lastStatusReadyTick = 0;
    state.statusLostTick = 0;
    state.statusStaleConfirm = 0;
    state.haveStatus = false;
}

void ClearAllRuntime(RuntimeState& state) {
    ClearProgress(state);
    ClearStatus(state);
    ClearTravelRuntime(state);
}

uint32_t ClampAttempts(uint32_t attempts) {
    return attempts == 0 ? 1u : attempts;
}

uint32_t BackoffSecForAttempt(uint32_t attempts, const Input& input) {
    const uint32_t base = input.backoffBaseSec == 0 ? 30u : input.backoffBaseSec;
    const uint32_t cap = input.backoffMaxSec == 0 ? base : input.backoffMaxSec;
    uint32_t sec = base;
    const uint32_t shift = ClampAttempts(attempts) - 1u;
    for (uint32_t i = 0; i < shift && sec < cap / 2u; ++i) {
        sec *= 2u;
    }
    return sec > cap ? cap : sec;
}

void EnterBackoff(Decision& decision, const Input& input) {
    ClearAllRuntime(decision.next);
    decision.next.mode = Mode::Backoff;
    decision.next.restartInFlight = false;
    decision.backoffSec = BackoffSecForAttempt(decision.next.recoveryAttempts, input);
    decision.next.backoffUntilTick = input.now + static_cast<uint64_t>(decision.backoffSec) * 1000u;
    decision.enteredBackoff = true;
    decision.gate = Gate::Backoff;
}

void RequestRestart(Decision& decision, const Input& input, const char* reason, uint32_t staleSec) {
    ClearAllRuntime(decision.next);
    ++decision.next.recoveryAttempts;
    decision.next.mode = Mode::Recovering;
    decision.next.recoveryStartedTick = input.now;
    decision.next.backoffUntilTick = 0;
    decision.next.restartInFlight = true;
    if (decision.gate == Gate::None) decision.gate = Gate::Recovering;
    decision.action = Action::Restart;
    decision.restartReason = reason;
    decision.staleSec = staleSec;
}

void ReissueRestart(Decision& decision, const char* reason, uint32_t staleSec) {
    decision.next.mode = Mode::Recovering;
    decision.next.restartInFlight = true;
    decision.next.backoffUntilTick = 0;
    if (decision.gate == Gate::None) decision.gate = Gate::Recovering;
    decision.action = Action::Restart;
    decision.restartReason = reason;
    decision.staleSec = staleSec;
}

const char* HardFailReason(const Input& input) {
    if (input.hardFailCode == xcat::kHardFailServerKick) return kReasonServerKick;
    if (input.freezeUnrecoverable) return kReasonFreezeUnrecoverable;
    if (input.charSelectStuck) return kReasonCharSelectStuck;
    return kReasonReloginStuck;
}

bool RecoveryTimedOut(const RuntimeState& state, const Input& input) {
    if (state.mode != Mode::Recovering || state.recoveryStartedTick == 0) return false;
    if (input.now < state.recoveryStartedTick) return false;
    const uint32_t timeoutSec =
        input.recoveryTimeoutSec > input.noExpSec ? input.recoveryTimeoutSec : input.noExpSec;
    return input.now - state.recoveryStartedTick >= static_cast<uint64_t>(timeoutSec) * 1000u;
}

bool CombatRecentlyActive(const Input& input) {
    if (!input.combatEnabled || input.combatActivityTickMs == 0) return false;
    if (input.now < input.combatActivityTickMs) return false;
    return input.now - input.combatActivityTickMs <=
           static_cast<uint64_t>(input.noExpSec) * 1000u;
}

}  // namespace

uint32_t CombatHoldHardLimitSec(uint32_t noExpSec) {
    const uint32_t base = noExpSec == 0 ? 180u : noExpSec;
    constexpr uint32_t kHoldExtraSec = 60u;
    return base + kHoldExtraSec;
}

namespace {

bool HealthyInput(const Input& input) {
    return input.injected && input.gamePid != 0 && input.processAlive && input.statusReady;
}

void MarkStatusHealthy(Decision& decision, const Input& input) {
    decision.gate = Gate::Progress;
    decision.next.lastStatusReadyTick = input.now;
    decision.next.statusLostTick = 0;
    decision.next.statusStaleConfirm = 0;
    decision.next.haveStatus = true;
    decision.next.restartInFlight = false;
    decision.next.recoveryAttempts = 0;
    decision.next.mode = Mode::Healthy;
}

void MarkProgressHealthy(Decision& decision, const Input& input) {
    MarkStatusHealthy(decision, input);
    decision.next.lastExp = input.playerExp;
    decision.next.lastProgressTick = input.now;
    decision.next.staleConfirm = 0;
    decision.next.haveExp = true;
}

}  // namespace

Decision Evaluate(const RuntimeState& state, const Input& input) {
    Decision decision{};
    decision.next = state;
    decision.previousMode = state.mode;

    const auto finish = [&decision]() {
        decision.stateChanged = decision.previousMode != decision.next.mode;
        return decision;
    };

    if (input.launchWorkerBusy) {
        decision.gate = state.mode == Mode::Recovering ? Gate::Recovering : Gate::LaunchWorkerBusy;
        return finish();
    }

    if (!input.scheduleActive) {
        decision.gate = Gate::ScheduleOff;
        ClearAllRuntime(decision.next);
        decision.next.restartInFlight = false;
        decision.next.recoveryAttempts = 0;
        decision.next.mode = Mode::Idle;
        return finish();
    }

    if (input.reloginHardFailed || input.freezeUnrecoverable || input.charSelectStuck) {
        decision.gate = Gate::StatusNotReady;
        const char* reason = HardFailReason(input);
        if (state.mode == Mode::Recovering && state.restartInFlight) {
            ReissueRestart(decision, reason, 0);
        } else {
            RequestRestart(decision, input, reason, 0);
        }
        decision.bypassRestartCooldown = true;
        return finish();
    }

    if (state.mode == Mode::Recovering && HealthyInput(input)) {
        MarkStatusHealthy(decision, input);
        return finish();
    }

    if (state.mode == Mode::Backoff) {
        if (HealthyInput(input)) {
            MarkStatusHealthy(decision, input);
            return finish();
        }
        if (input.now < state.backoffUntilTick) {
            decision.gate = Gate::Backoff;
            decision.backoffSec =
                static_cast<uint32_t>((state.backoffUntilTick - input.now + 999u) / 1000u);
            return finish();
        }
        // 冷启宽限内进程仍在：只延长等待，禁止「恢复重试」再杀一遍。
        if (input.progressGrace && input.processAlive) {
            decision.gate = Gate::Backoff;
            decision.backoffSec = 0;
            return finish();
        }
        RequestRestart(decision, input, kReasonRecoveryRetry, 0);
        return finish();
    }

    if (state.mode == Mode::Recovering && (!input.injected || input.gamePid == 0)) {
        if (input.progressGrace) {
            decision.gate = Gate::Recovering;
            return finish();
        }
        EnterBackoff(decision, input);
        return finish();
    }

    if (!input.injected || input.gamePid == 0) {
        decision.gate = Gate::NotInjected;
        ClearAllRuntime(decision.next);
        decision.next.restartInFlight = false;
        decision.next.recoveryAttempts = 0;
        decision.next.mode = Mode::Idle;
        return finish();
    }

    if (!input.processAlive) {
        decision.gate = Gate::ProcessDead;
        RequestRestart(decision, input, kReasonProcessDead, 0);
        return finish();
    }

    if (!input.statusReady) {
        if (decision.next.travelActiveSinceTick != 0 && decision.next.travelPauseTick == 0)
            decision.next.travelPauseTick = input.now;
        decision.gate = Gate::StatusNotReady;
        if (input.progressGrace) {
            decision.next.statusLostTick = 0;
            decision.next.statusStaleConfirm = 0;
            return finish();
        }
        if (RecoveryTimedOut(state, input)) {
            decision.restartReason = kReasonRecoveryTimeout;
            EnterBackoff(decision, input);
            return finish();
        }
        // haveStatus：曾进图后 Status 掉线
        // payloadHeartbeatStale：整进程假死（含从未进图）
        // prePlayableStuck：心跳仍活但长期卡选角/加载（一键归属、冷启已结束）
        if (decision.next.haveStatus || input.payloadHeartbeatStale ||
            input.prePlayableStuck) {
            if (!decision.next.statusLostTick) decision.next.statusLostTick = input.now;
            decision.staleSec =
                static_cast<uint32_t>((input.now - decision.next.statusLostTick) / 1000u);
            if (decision.staleSec >= input.noExpSec) {
                ++decision.next.statusStaleConfirm;
                if (decision.next.statusStaleConfirm < 2u) {
                    decision.warnStatusConfirm = true;
                    return finish();
                }
                const char* reason = input.payloadHeartbeatStale ? kReasonPayloadHung
                                     : input.prePlayableStuck    ? kReasonPrePlayableStuck
                                                                : kReasonStatusStale;
                RequestRestart(decision, input, reason, decision.staleSec);
            }
        } else {
            decision.next.statusLostTick = 0;
            decision.next.statusStaleConfirm = 0;
        }
        return finish();
    }

    if (decision.next.travelPauseTick != 0) {
        if (decision.next.travelActiveSinceTick != 0 &&
            input.now >= decision.next.travelPauseTick) {
            decision.next.travelActiveSinceTick += (input.now - decision.next.travelPauseTick);
        }
        decision.next.travelPauseTick = 0;
    }

    bool travelLatched = false;
    if (input.travelActive) {
        decision.next.travelInactiveSinceTick = 0;
        if (!decision.next.travelActiveSinceTick) decision.next.travelActiveSinceTick = input.now;
        travelLatched = true;
    } else if (decision.next.travelActiveSinceTick != 0) {
        if (!decision.next.travelInactiveSinceTick)
            decision.next.travelInactiveSinceTick = input.now;
        const uint32_t inactiveSec = static_cast<uint32_t>(
            (input.now - decision.next.travelInactiveSinceTick) / 1000u);
        if (inactiveSec < kTravelInactiveGraceSec) {
            travelLatched = true;
        } else {
            ClearTravelRuntime(decision.next);
        }
    }

    if (travelLatched) {
        if (input.mapIdValid && input.mapId != 0) {
            if (decision.next.travelLastMapId != 0 &&
                decision.next.travelLastMapId != input.mapId) {
                decision.next.travelActiveSinceTick = input.now;
                decision.next.travelInactiveSinceTick = 0;
            }
            decision.next.travelLastMapId = input.mapId;
        }

        const uint32_t travelSec = static_cast<uint32_t>(
            (input.now - decision.next.travelActiveSinceTick) / 1000u);
        const uint32_t capSec = TravelStuckCapSec(input);
        if (travelSec >= capSec) {
            decision.gate = Gate::Travel;
            RequestRestart(decision, input, kReasonTravelStuck, travelSec);
            return finish();
        }
        MarkStatusHealthy(decision, input);
        ClearProgress(decision.next);
        decision.staleSec = travelSec;
        if (!input.combatEnabled) {
            decision.gate = Gate::CombatOff;
            return finish();
        }
        if (input.safeZonePause && !input.progressGrace) {
            decision.gate = Gate::SafeZone;
            return finish();
        }
        decision.gate = Gate::Travel;
        return finish();
    }

    if (!input.combatEnabled) {
        MarkStatusHealthy(decision, input);
        decision.gate = Gate::CombatOff;
        ClearProgress(decision.next);
        return finish();
    }

    if (input.safeZonePause && !input.progressGrace) {
        MarkStatusHealthy(decision, input);
        decision.gate = Gate::SafeZone;
        ClearProgress(decision.next);
        return finish();
    }

    if (!decision.next.haveExp) {
        if (input.playerExpValid) {
            MarkProgressHealthy(decision, input);
        } else {
            MarkStatusHealthy(decision, input);
        }
        return finish();
    }

    if (input.playerExpValid && input.playerExp != decision.next.lastExp) {
        MarkProgressHealthy(decision, input);
        return finish();
    }

    decision.gate = Gate::ExpStale;
    decision.next.mode = Mode::Healthy;
    decision.next.statusLostTick = 0;
    decision.next.statusStaleConfirm = 0;

    const uint64_t staleMs = input.now - decision.next.lastProgressTick;
    decision.staleSec = static_cast<uint32_t>(staleMs / 1000u);
    if (input.progressGrace) {
        decision.next.staleConfirm = 0;
        return finish();
    }
    if (staleMs >= input.noExpSec * 1000u) {
        if (!input.playerExpValid) {
            decision.next.staleConfirm = 0;
            return finish();
        }
        if (CombatRecentlyActive(input)) {
            const uint64_t hardLimitMs =
                static_cast<uint64_t>(CombatHoldHardLimitSec(input.noExpSec)) * 1000u;
            if (staleMs < hardLimitMs) {
                decision.next.staleConfirm = 0;
                decision.combatHold = true;
                return finish();
            }
        }
        ++decision.next.staleConfirm;
        if (decision.next.staleConfirm < 2u) {
            decision.warnExpConfirm = true;
            return finish();
        }
        RequestRestart(decision, input, kReasonExpStale, decision.staleSec);
    }
    return finish();
}

const char* ModeLabel(Mode mode) {
    switch (mode) {
    case Mode::Idle: return "Idle";
    case Mode::Healthy: return "Healthy";
    case Mode::Recovering: return "Recovering";
    case Mode::Backoff: return "Backoff";
    default: return "?";
    }
}

const char* GateLabel(Gate gate) {
    switch (gate) {
    case Gate::LaunchWorkerBusy: return "launch-worker-busy";
    case Gate::NotInjected: return "not-injected";
    case Gate::ProcessDead: return "process-dead";
    case Gate::CombatOff: return "combat-off";
    case Gate::ScheduleOff: return "schedule-off";
    case Gate::SafeZone: return "safe-zone";
    case Gate::Travel: return "travel";
    case Gate::StatusNotReady: return "status-not-ready";
    case Gate::Progress: return "progress";
    case Gate::ExpStale: return "exp-stale";
    case Gate::Recovering: return "recovering";
    case Gate::Backoff: return "backoff";
    case Gate::None:
    default: return "none";
    }
}

}  // namespace xcat::app::guardian_policy
