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
    bool combatEnabled = false;  // F5 / simpleCombat；停表原因高亮用
    uint32_t progressStaleSec = 0;
    uint32_t statusStaleSec = 0;
    uint32_t backoffRemainingSec = 0;
    uint32_t recoveryElapsedSec = 0;
    uint32_t combatHoldHardLimitSec = 0;
    uint32_t noExpLimitSec = 0;
    const char* gate = "none";
    int localHour = 0;
    uint32_t mask = 0;
    // 一键后尚未 localPlayerOk：顶栏显示「冷启中 Xs」（Xs=距硬顶剩余）。
    bool coldStart = false;
    // 主门 awaiting：Classic 起来后剩余至 2×noExp；次门 secondary：剩余至 noExp。
    uint32_t coldStartGraceRemainSec = 0;
    // awaiting 且 Classic 尚未起来：不倒计时（主门硬顶仅进程已起后生效）。
    bool coldStartWaitingProcess = false;
    // 干净重拉杀进程/settle 阶段（不含随后的一键冷启）。
    bool cleanRelaunchKillSettle = false;
    // 一键归属仍在、冷启已结束：顶栏可显示「未进图 a/b」。
    bool launchOwnedPendingPlayable = false;
    // 踢线后短等干净重拉剩余秒（0=无）。
    uint32_t kickRelaunchRemainSec = 0;
    // ports::world::SceneState：4=CashShop 5=GlobalMarket；-1=未知。safe-zone 文案用。
    int32_t sceneState = -1;
    uint32_t currentMapId = 0;
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
// F5 开着且经验表停走时的原因（主城/拍卖/商城/转职考验/赶路）；否则 nullptr。
const char* FormatWatchdogF5PauseReason(const Snapshot& snap);

// 1Hz: hangup kill/start + optional no-exp guardian restart.
void Tick(LaunchUiState& ui, bool appExiting);
void Reset();

// 底部「杀死游戏后重拉」：杀 Classic → 等退净 → 冷却 → 一键冷启（绕过守护冷却）。
// IsCleanRelaunchInFlight：含 settle 后一键冷启未结束，供底部按钮禁用。
bool RequestManualCleanRelaunch(LaunchUiState& ui);
bool IsCleanRelaunchInFlight();

// 一键/干净重拉刚启动：置 awaitingPlayable 状态位，直到 localPlayerOk（非定时硬编码）。
void NoteLaunchStarted(uint32_t graceSec = 0);

}  // namespace hangup_schedule
}  // namespace xcat::app
