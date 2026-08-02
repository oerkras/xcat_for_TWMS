#pragma once

#include <cstdint>
#include <string>

namespace xcat::app {

struct LaunchUiState;

namespace hangup_schedule {

// Classic TWMS launcher schedule + guardian (no-exp restart).
// Hangup and watchdog are orthogonal; game exe = Maplestory_Classic.exe.

enum class UiMode {
    Disabled,   // hangup master off (schedule ignored)
    OffHour,    // hangup on, current hour not in mask
    OnHour,     // hangup on / or hangup off with watchdog using full-day schedule
    Starting,   // launch in flight
};

enum class WatchdogUiMode {
    Off,
    Idle,
    Healthy,
    Recovering,
    Backoff,
};

struct Snapshot {
    UiMode mode = UiMode::Disabled;
    WatchdogUiMode watchdogMode = WatchdogUiMode::Off;
    bool scheduleActive = true;
    bool hangupOn = false;
    bool watchdogOn = false;
    bool launchBusy = false;
    bool combatHold = false;
    bool hasProgress = false;
    uint32_t progressStaleSec = 0;
    uint32_t statusStaleSec = 0;
    uint32_t backoffRemainingSec = 0;
    uint32_t recoveryElapsedSec = 0;
    uint32_t combatHoldHardLimitSec = 0;
    uint32_t noExpLimitSec = 0;
    const char* gate = "none";
    int localHour = 0;
    uint32_t mask = 0;
};

uint32_t ClampScheduleMask(uint32_t mask);
int CurrentLocalHour();
bool IsHangupHourActive(uint32_t mask, int hour);

// Hangup master off, or current hour in mask => allow launch.
bool AllowsLaunch(const std::string& prefsBinDir);

Snapshot GetSnapshot();
const char* UiModeLabel(UiMode mode);
const char* WatchdogUiModeLabel(WatchdogUiMode mode);
std::string FormatWatchdogTimerText(const Snapshot& snap);

// 1Hz: hangup kill/start + optional no-exp guardian restart.
void Tick(LaunchUiState& ui, bool appExiting);
void Reset();

// 底部「杀死游戏后重拉」：杀 Classic → 等退净 → 冷却 → 一键冷启（绕过守护冷却）。
// IsCleanRelaunchInFlight：含 settle 后一键冷启未结束，供底部按钮禁用。
bool RequestManualCleanRelaunch(LaunchUiState& ui);
bool IsCleanRelaunchInFlight();

}  // namespace hangup_schedule
}  // namespace xcat::app
