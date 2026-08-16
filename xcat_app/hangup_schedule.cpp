#include "hangup_schedule.h"

#include "app_notify.h"
#include "attach_inject.h"
#include "game_exit_probe.h"
#include "guardian_policy.h"
#include "launch_panel.h"
#include "update_client.h"

#include "msc_webview_login.h"
#include "process_util.h"
#include "xcat_log.h"
#include "xcat_payload_control.h"
#include "xcat_payload_status.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace xcat::app::hangup_schedule {
namespace {

constexpr wchar_t kClassicExe[] = L"Maplestory_Classic.exe";
constexpr wchar_t kNgmExe[] = L"NGM.exe";
constexpr wchar_t kNgm64Exe[] = L"NGM64.exe";
constexpr uint32_t kHangupStartCooldownSec = 120;
// Async clean relaunch (UI-thread state machine; no long Sleep in Tick).
constexpr uint64_t kRelaunchGoneWaitMs = 15000;
constexpr uint64_t kRelaunchRetryKillAtMs = 8000;
constexpr uint64_t kRelaunchSettleMs = 2500;
// soft RESULT success 后短窗：KickSniff 落地静默期内仍会 bump disconnectSeq，
// 而 RequestAttempt 因 land_quiet 早退 hold=0。守护 Tick 1Hz，BIN 实测
// success absorb 后约 1s 把下一跳 seq 当踢线硬杀。窗长须盖住 land_quiet(1.5s)
// + 一拍 Tick；result==2 仍立刻硬杀。不得把 result==1 做成永久吞 seq。
constexpr uint64_t kSoftSuccessGraceMs = 8000;

enum class RelaunchPhase : uint8_t {
    None = 0,
    WaitingGone,  // kill issued; poll until Classic absent
    Settling,     // gone; brief settle before one-click
};

struct RelaunchJob {
    RelaunchPhase phase = RelaunchPhase::None;
    uint64_t phaseSince = 0;
    bool retriedKill = false;
    char logLine[192]{};
    char statusLine[128]{};
};

struct State {
    uint64_t lastTick = 0;
    uint64_t lastStartTick = 0;
    uint64_t lastRestartTick = 0;
    uint64_t lastGateLogTick = 0;
    bool haveScheduleActive = false;
    bool scheduleActive = true;
    bool launchBusy = false;
    bool hangupOn = false;
    bool watchdogOn = false;
    bool combatEnabled = false;
    bool combatHold = false;
    uint32_t combatHoldHardLimitSec = 0;
    uint32_t noExpLimitSec = 0;
    UiMode mode = UiMode::Disabled;
    WatchdogUiMode watchdogMode = WatchdogUiMode::Off;
    const char* gate = "none";
    int localHour = 0;
    uint32_t mask = 0;
    // Sticky session: survive crash so ProcessDead can fire (fengxing keeps ui.gamePid).
    DWORD trackedPid = 0;
    bool sessionArmed = false;
    // 一键/干净重拉归属：进图前也要能 ProcessDead；配合冷启宽限（主门 2N / 次门 N）。
    bool awaitingPlayable = false;
    uint64_t coldProcessSeenTick = 0;
    uint64_t coldHandshakeSeenTick = 0;
    // 顶栏冷启文案（Tick 写入，GetSnapshot 读取）。
    bool uiColdStart = false;
    uint32_t uiColdGraceRemainSec = 0;
    bool uiColdWaitingProcess = false;
    bool uiLaunchOwnedPendingPlayable = false;
    // kick_sniff disconnectSeq baseline：武装后先对齐再边沿触发，避免历史断线误重拉。
    uint32_t lastConsumedDisconnectSeq = 0;
    uint32_t lastLoggedDisconnectSeq = 0;
    bool haveDisconnectBaseline = false;
    // softLoginResult==1 上升沿武装短窗；窗内吞新 seq，窗后新 seq 仍当踢线。
    uint32_t lastSeenSoftLoginResult = 0;
    uint64_t softSuccessGraceUntil = 0;
    // 最近一次新鲜 SHM 见到的 softLoginHold；进程已死后 stFresh 常假，靠 latch 说明 ProcessDead。
    bool softLoginHoldLatched = false;
    // hold 上升沿墙钟；到期后不再挡无经验/状态停滞的干净重拉。
    uint64_t softHoldSinceTick = 0;
    uint64_t lastCooldownBlockLogTick = 0;
    int32_t sceneState = -1;  // PayloadStatus.sceneState；拍卖/商城停表用
    uint32_t currentMapId = 0;
    RelaunchJob relaunch{};
    guardian_policy::RuntimeState runtime{};
};

State g{};

uint32_t ClampMaskImpl(uint32_t mask) { return xcat::ClampHangupScheduleMask(mask); }

int CurrentLocalHourImpl() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return static_cast<int>(st.wHour);
}

bool IsHourActiveImpl(uint32_t mask, int hour) {
    if (hour < 0 || hour > 23) return false;
    return (ClampMaskImpl(mask) & (1u << hour)) != 0;
}

DWORD ClassicPid() { return xcat::FindProcessIdByName(kClassicExe); }
bool ClassicPresent() { return ClassicPid() != 0; }
bool NgmPresent() {
    return xcat::FindProcessIdByName(kNgmExe) != 0 || xcat::FindProcessIdByName(kNgm64Exe) != 0;
}
// 干净重拉须清掉 Classic + NGM：只杀游戏会留下旧 NGM，下一轮官网 Main 常不再拉新经典版。
bool LaunchChainPresent() { return ClassicPresent() || NgmPresent(); }
unsigned KillLaunchChain(const char* why, const char* logTag = "Watchdog") {
    const unsigned classic = xcat::KillProcessesByExeName(kClassicExe);
    const unsigned ngm64 = xcat::KillProcessesByExeName(kNgm64Exe);
    const unsigned ngm = xcat::KillProcessesByExeName(kNgmExe);
    xcat::log::Info(logTag && logTag[0] ? logTag : "Watchdog",
                    "kill launch-chain (%s): Classic x%u NGM64 x%u NGM x%u",
                    why && why[0] ? why : "launch-chain", classic, ngm64, ngm);
    return classic + ngm64 + ngm;
}
bool RelaunchInFlight() { return g.relaunch.phase != RelaunchPhase::None; }

void PushStatus(LaunchUiState& ui, const char* line, const char* status) {
    if (line && line[0]) LaunchPanel_AppendLog(ui, xcat::Utf8ToWide(line));
    if (status && status[0]) ui.status = status;
}

WatchdogUiMode ToWatchdogUi(guardian_policy::Mode mode, bool watchdogOn) {
    if (!watchdogOn) return WatchdogUiMode::Off;
    switch (mode) {
    case guardian_policy::Mode::Healthy: return WatchdogUiMode::Healthy;
    case guardian_policy::Mode::Recovering: return WatchdogUiMode::Recovering;
    case guardian_policy::Mode::Backoff: return WatchdogUiMode::Backoff;
    case guardian_policy::Mode::Idle:
    default: return WatchdogUiMode::Idle;
    }
}

bool CooldownBlocks(uint64_t now, uint64_t last, uint32_t cooldownSec) {
    if (!last || now < last) return false;
    return now - last < static_cast<uint64_t>(cooldownSec) * 1000u;
}

void ClearRelaunchJob() { g.relaunch = RelaunchJob{}; }

void ClearSessionTrack() {
    g.trackedPid = 0;
    g.sessionArmed = false;
    g.awaitingPlayable = false;
    g.coldProcessSeenTick = 0;
    g.coldHandshakeSeenTick = 0;
    g.uiColdStart = false;
    g.uiColdGraceRemainSec = 0;
    g.uiColdWaitingProcess = false;
    g.uiLaunchOwnedPendingPlayable = false;
    g.lastConsumedDisconnectSeq = 0;
    g.lastLoggedDisconnectSeq = 0;
    g.haveDisconnectBaseline = false;
    g.lastSeenSoftLoginResult = 0;
    g.softSuccessGraceUntil = 0;
    g.softLoginHoldLatched = false;
    g.softHoldSinceTick = 0;
}

void ArmSessionIfLive(DWORD livePid, bool handshakeOk) {
    if (livePid == 0) return;
    g.trackedPid = livePid;
    if (handshakeOk) g.sessionArmed = true;
}

// Begin clean relaunch: kill Classic+NGM, wait-gone, settle, then one-click.
// Returns false if busy/cooldown/WebView not ready (does not consume cooldown).
bool BeginCleanRelaunch(LaunchUiState& ui, uint64_t now, uint32_t cooldownSec,
                        const char* logLine, const char* statusLine) {
    if (RelaunchInFlight() || g.launchBusy || msc::weblogin::IsBusy()) return false;
    if (CooldownBlocks(now, g.lastRestartTick, cooldownSec)) {
        if (g.lastCooldownBlockLogTick == 0 || now - g.lastCooldownBlockLogTick >= 5000u) {
            g.lastCooldownBlockLogTick = now;
            const uint64_t elapsed = now >= g.lastRestartTick ? now - g.lastRestartTick : 0;
            const uint64_t limitMs = static_cast<uint64_t>(cooldownSec) * 1000u;
            const uint32_t remainSec =
                elapsed >= limitMs
                    ? 0u
                    : static_cast<uint32_t>((limitMs - elapsed + 999u) / 1000u);
            xcat::log::Info("Watchdog",
                            "restart suppressed: cooldown remain=%us limit=%us reason=%s",
                            remainSec, cooldownSec,
                            logLine && logLine[0] ? logLine : "watchdog");
        }
        return false;
    }
    const bool attachMode =
        attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode());
    if (!attachMode && !msc::weblogin::CanStartOneClick()) {
        if (g.lastCooldownBlockLogTick == 0 || now - g.lastCooldownBlockLogTick >= 5000u) {
            g.lastCooldownBlockLogTick = now;
            xcat::log::Warn("Watchdog", "restart suppressed: one-click not ready");
        }
        return false;
    }

    g.lastRestartTick = now;
    g.lastStartTick = now;
    g.lastCooldownBlockLogTick = 0;
    ClearSessionTrack();

    (void)KillLaunchChain("begin");

    g.relaunch = {};
    g.relaunch.phase = RelaunchPhase::WaitingGone;
    g.relaunch.phaseSince = now;
    if (logLine) strncpy_s(g.relaunch.logLine, logLine, _TRUNCATE);
    if (statusLine) strncpy_s(g.relaunch.statusLine, statusLine, _TRUNCATE);
    PushStatus(ui, g.relaunch.logLine,
               statusLine && statusLine[0] ? statusLine : "守护模式：正在结束旧游戏与 NGM…");
    g.mode = UiMode::Starting;
    g.launchBusy = true;  // block hangup parallel start

    // 气泡 + 历史事件（PushLocal → eventlog::Record）；此前只写启动面板日志，历史空白。
    const char* body =
        (statusLine && statusLine[0]) ? statusLine
        : (logLine && logLine[0])     ? logLine
                                     : "正在杀死游戏/NGM 并干净重拉";
    notify::PushLocal(/*Warning*/ 2, "watchdog-clean-relaunch", "守护干净重拉", body, 7000);
    return true;
}

// Advance async clean relaunch; call once per Tick while phase != None.
void PumpCleanRelaunch(LaunchUiState& ui, uint64_t now) {
    if (!RelaunchInFlight()) return;

    switch (g.relaunch.phase) {
    case RelaunchPhase::WaitingGone: {
        if (!LaunchChainPresent()) {
            g.relaunch.phase = RelaunchPhase::Settling;
            g.relaunch.phaseSince = now;
            PushStatus(ui, nullptr, "守护模式：旧进程已退出，冷却后重拉…");
            xcat::log::Info("Watchdog", "clean relaunch: launch-chain gone → settle %llums",
                            static_cast<unsigned long long>(kRelaunchSettleMs));
            break;
        }
        const uint64_t waited = now >= g.relaunch.phaseSince ? now - g.relaunch.phaseSince : 0;
        if (!g.relaunch.retriedKill && waited >= kRelaunchRetryKillAtMs) {
            g.relaunch.retriedKill = true;
            (void)KillLaunchChain("retry");
        }
        if (waited >= kRelaunchGoneWaitMs) {
            xcat::log::Warn(
                "Watchdog",
                "clean relaunch aborted: launch-chain still present after %llums "
                "(Classic=%u NGM=%u)",
                static_cast<unsigned long long>(kRelaunchGoneWaitMs),
                ClassicPresent() ? 1u : 0u, NgmPresent() ? 1u : 0u);
            PushStatus(ui, "[Watchdog] 旧游戏/NGM 未清净，已中止重拉",
                       "守护模式：旧进程未退出，禁止重拉");
            ClearRelaunchJob();
            g.launchBusy = msc::weblogin::IsBusy();
            g.runtime.restartInFlight = false;
            // Keep Recovering so backoff/retry can pick up; do not clear sessionArm mid-crash
            // if process somehow returns — next Tick will re-arm from live pid.
        }
        break;
    }
    case RelaunchPhase::Settling: {
        if (LaunchChainPresent()) {
            // Respawned / kill raced — back to waiting.
            g.relaunch.phase = RelaunchPhase::WaitingGone;
            g.relaunch.phaseSince = now;
            g.relaunch.retriedKill = false;
            xcat::log::Warn("Watchdog",
                            "clean relaunch: launch-chain reappeared during settle "
                            "(Classic=%u NGM=%u)",
                            ClassicPresent() ? 1u : 0u, NgmPresent() ? 1u : 0u);
            break;
        }
        if (now - g.relaunch.phaseSince < kRelaunchSettleMs) break;

        // Final confirm (fengxing: gone=false forbids relaunch).
        if (LaunchChainPresent()) {
            PushStatus(ui, "[Watchdog] settle 后仍有 Classic/NGM，中止重拉",
                       "守护模式：旧进程未清净");
            ClearRelaunchJob();
            g.launchBusy = false;
            g.runtime.restartInFlight = false;
            break;
        }

        PushStatus(ui, g.relaunch.logLine[0] ? g.relaunch.logLine : "[Watchdog] 干净重拉",
                   g.relaunch.statusLine[0] ? g.relaunch.statusLine
                                            : "守护模式：正在重新启动…");
        ClearRelaunchJob();
        if (attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode())) {
            if (!attach_inject::IsWatching()) {
                (void)attach_inject::StartWatch();
            }
            ui.pendingAutoLaunch = false;
            ui.autoLaunchNotBeforeMs = 0;
            PushStatus(ui, "[Watchdog] 请手动重开游戏，将自动注入",
                       "守护模式：请手动重开游戏（监视注入中）");
            g.launchBusy = false;
            g.runtime.restartInFlight = false;
            g.mode = UiMode::Starting;
            // BeginCleanRelaunch 已 ClearSessionTrack（await=0）。必须重新 NoteLaunch，
            // 否则 Backoff 到期时进程已手动开起但尚未进图 → RecoveryRetry 误杀活进程（c73656）。
            NoteLaunchStarted(0);
            xcat::log::Info("Watchdog", "clean relaunch: attach-watch waiting for manual launch");
        } else {
            if (attach_inject::GetLaunchMode() == attach_inject::LaunchMode::GamaPassAuto) {
                msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
            } else if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
                msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
            }
            if (!LaunchPanel_StartOneClick(ui, /*honorStrategyPrep=*/false)) {
                g.launchBusy = false;
                g.runtime.restartInFlight = false;
                xcat::log::Warn("Watchdog", "clean relaunch: StartOneClick failed");
            } else {
                g.launchBusy = true;
                g.mode = UiMode::Starting;
                g.runtime.restartInFlight = true;
                xcat::log::Info("Watchdog", "clean relaunch: one-click started");
            }
        }
        break;
    }
    case RelaunchPhase::None:
    default:
        break;
    }
}

void LogGateThrottled(uint64_t now, const char* gate, const xcat::PayloadControl& cfg) {
    if (!gate) return;
    if (g.lastGateLogTick && now - g.lastGateLogTick < 15000 && g.gate &&
        strcmp(g.gate, gate) == 0) {
        return;
    }
    g.lastGateLogTick = now;
    xcat::log::Info("Watchdog",
                    "gate=%s watchdog=%u hangup=%u noExp=%u combat=%u schedule=%s "
                    "trackedPid=%lu armed=%u await=%u",
                    gate, cfg.launcherWatchdog, cfg.launcherHangupSchedule,
                    cfg.launcherWatchdogNoExpSec, cfg.simpleCombat,
                    g.scheduleActive ? "on" : "off",
                    static_cast<unsigned long>(g.trackedPid), g.sessionArmed ? 1u : 0u,
                    g.awaitingPlayable ? 1u : 0u);
}

}  // namespace

uint32_t ClampScheduleMask(uint32_t mask) { return ClampMaskImpl(mask); }
int CurrentLocalHour() { return CurrentLocalHourImpl(); }
bool IsHangupHourActive(uint32_t mask, int hour) { return IsHourActiveImpl(mask, hour); }

bool AllowsLaunch(const std::string& prefsBinDir) {
    if (prefsBinDir.empty()) return true;
    xcat::PayloadControl cfg{};
    if (!xcat::ReadPayloadControl(prefsBinDir.c_str(), cfg)) return true;
    if (cfg.launcherHangupSchedule == 0) return true;
    return IsHourActiveImpl(ClampMaskImpl(cfg.launcherHangupScheduleMask),
                            CurrentLocalHourImpl());
}

// 2转考验场（map_names：弓/法/剑/盗 108000100-102 / 200-202 / 300-302 / 400-402）。
// 怪无经验、掉任务道具；守护按主城同款停表，禁止 EXP 停滞倒计时。
bool IsSecondJobTrialMap(uint32_t mapId) {
    if (mapId < 108000100u || mapId > 108000402u) return false;
    const uint32_t rem = mapId - 108000000u;
    const uint32_t job = rem / 100u;
    const uint32_t var = rem % 100u;
    return job >= 1u && job <= 4u && var <= 2u;
}

Snapshot GetSnapshot() {
    Snapshot s{};
    s.mode = g.mode;
    s.watchdogMode = g.watchdogMode;
    s.scheduleActive = g.scheduleActive;
    s.hangupOn = g.hangupOn;
    s.watchdogOn = g.watchdogOn;
    s.launchBusy = g.launchBusy || RelaunchInFlight();
    s.combatHold = g.combatHold;
    s.hasProgress = g.runtime.haveExp;
    s.combatEnabled = g.combatEnabled;
    s.noExpLimitSec = g.noExpLimitSec;
    s.combatHoldHardLimitSec = g.combatHoldHardLimitSec;
    s.gate = g.gate ? g.gate : "none";
    s.localHour = g.localHour;
    s.mask = g.mask;
    const uint64_t now = GetTickCount64();
    if (g.runtime.haveExp && g.runtime.lastProgressTick && now >= g.runtime.lastProgressTick) {
        s.progressStaleSec =
            static_cast<uint32_t>((now - g.runtime.lastProgressTick) / 1000u);
    }
    if (g.runtime.statusLostTick && now >= g.runtime.statusLostTick) {
        s.statusStaleSec = static_cast<uint32_t>((now - g.runtime.statusLostTick) / 1000u);
    }
    if (g.runtime.mode == guardian_policy::Mode::Backoff && g.runtime.backoffUntilTick > now) {
        s.backoffRemainingSec =
            static_cast<uint32_t>((g.runtime.backoffUntilTick - now + 999u) / 1000u);
    }
    if (g.runtime.mode == guardian_policy::Mode::Recovering &&
        g.runtime.recoveryStartedTick && now >= g.runtime.recoveryStartedTick) {
        s.recoveryElapsedSec =
            static_cast<uint32_t>((now - g.runtime.recoveryStartedTick) / 1000u);
    }
    s.coldStart = g.uiColdStart;
    s.coldStartGraceRemainSec = g.uiColdGraceRemainSec;
    s.coldStartWaitingProcess = g.uiColdWaitingProcess;
    s.launchOwnedPendingPlayable = g.uiLaunchOwnedPendingPlayable;
    s.cleanRelaunchKillSettle = RelaunchInFlight();
    s.sceneState = g.sceneState;
    s.currentMapId = g.currentMapId;
    return s;
}

const char* UiModeLabel(UiMode mode) {
    switch (mode) {
    case UiMode::Disabled: return "已关闭（忽略时段表）";
    case UiMode::OffHour: return "非挂机（已关机）";
    case UiMode::OnHour: return "挂机中";
    case UiMode::Starting: return "正在按时段拉起…";
    }
    return "未知";
}

const char* WatchdogUiModeLabel(WatchdogUiMode mode) {
    switch (mode) {
    case WatchdogUiMode::Off: return "守护关闭";
    case WatchdogUiMode::Idle: return "守护空闲";
    case WatchdogUiMode::Healthy: return "守护健康";
    case WatchdogUiMode::Recovering: return "守护恢复中";
    case WatchdogUiMode::Backoff: return "守护退避";
    }
    return "未知";
}

const char* WatchdogSafeZonePauseLabel(const Snapshot& snap) {
    if (snap.sceneState == 5) return "拍卖行（暂停无经验）";
    if (snap.sceneState == 4) return "商城（暂停无经验）";
    if (IsSecondJobTrialMap(snap.currentMapId)) return "转职考验（暂停无经验）";
    return "主城（暂停无经验）";
}

const char* FormatWatchdogF5PauseReason(const Snapshot& snap) {
    if (!snap.watchdogOn || !snap.combatEnabled || !snap.scheduleActive) return nullptr;
    if (!snap.gate) return nullptr;
    if (std::strcmp(snap.gate, "safe-zone") == 0) return WatchdogSafeZonePauseLabel(snap);
    if (std::strcmp(snap.gate, "travel") == 0) return "赶路（暂停无经验）";
    return nullptr;
}

std::string FormatWatchdogTimerText(const Snapshot& snap) {
    char buf[80]{};
    if (!snap.watchdogOn) return "守护关闭";
    if (!snap.scheduleActive) return "非挂机（已关机）";
    if (snap.cleanRelaunchKillSettle ||
        (snap.launchBusy && snap.mode == UiMode::Starting)) {
        return "正在干净重拉…";
    }
    if (snap.coldStartWaitingProcess) return "冷启等待进程…";
    if (snap.coldStart && snap.coldStartGraceRemainSec > 0) {
        snprintf(buf, sizeof(buf), "冷启中 %us", snap.coldStartGraceRemainSec);
        return buf;
    }
    if (snap.watchdogMode == WatchdogUiMode::Backoff && snap.backoffRemainingSec > 0) {
        snprintf(buf, sizeof(buf), "等待 %us", snap.backoffRemainingSec);
        return buf;
    }
    if (snap.watchdogMode == WatchdogUiMode::Recovering) {
        snprintf(buf, sizeof(buf), "恢复 %us", snap.recoveryElapsedSec);
        return buf;
    }
    const uint32_t elapsed = snap.statusStaleSec > 0 ? snap.statusStaleSec : snap.progressStaleSec;
    const uint32_t limit = snap.noExpLimitSec ? snap.noExpLimitSec : 180u;
    if (snap.launchOwnedPendingPlayable) {
        snprintf(buf, sizeof(buf), "未进图 %u/%u秒", elapsed, limit);
        return buf;
    }
    if (snap.combatHold) {
        const uint32_t holdLimit =
            snap.combatHoldHardLimitSec ? snap.combatHoldHardLimitSec : limit + 60u;
        snprintf(buf, sizeof(buf), "战斗暂缓 %u/%u秒", elapsed, holdLimit);
        return buf;
    }
    if (snap.gate && std::strcmp(snap.gate, "safe-zone") == 0) {
        return WatchdogSafeZonePauseLabel(snap);
    }
    if (snap.gate && std::strcmp(snap.gate, "travel") == 0) return "赶路（暂停无经验）";
    if (snap.gate && std::strcmp(snap.gate, "combat-off") == 0) return "战斗关闭（暂停无经验）";
    if (snap.watchdogMode == WatchdogUiMode::Healthy || snap.hasProgress) {
        snprintf(buf, sizeof(buf), "挂机中 %u/%u秒", elapsed, limit);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%u/%u秒", elapsed, limit);
    return buf;
}

void Reset() { g = State{}; }

void Tick(LaunchUiState& ui, bool appExiting) {
    if (ui.prefsBinDir.empty()) return;
    if (appExiting) {
        ClearRelaunchJob();
        return;
    }

    const uint64_t now = GetTickCount64();
    if (g.lastTick && now - g.lastTick < 1000) return;
    g.lastTick = now;

    if (UpdateNeedsVisibleUi()) {
        g.launchBusy = msc::weblogin::IsBusy() || RelaunchInFlight();
        return;
    }

    // Always pump in-flight clean relaunch first.
    if (RelaunchInFlight()) {
        PumpCleanRelaunch(ui, now);
        g.launchBusy = msc::weblogin::IsBusy() || RelaunchInFlight();
        // Still evaluate hangup/watchdog gates below while waiting, but starts are blocked.
    }

    xcat::PayloadControl cfg{};
    if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), cfg)) {
        xcat::PayloadControlSetDefaults(cfg);
    }

    const bool hangupOn = cfg.launcherHangupSchedule != 0;
    const bool watchdogOn = cfg.launcherWatchdog != 0;
    g.hangupOn = hangupOn;
    g.watchdogOn = watchdogOn;
    g.combatEnabled = cfg.simpleCombat != 0;
    g.launchBusy = msc::weblogin::IsBusy() || RelaunchInFlight();
    cfg.launcherHangupScheduleMask = ClampMaskImpl(cfg.launcherHangupScheduleMask);
    cfg.launcherWatchdogNoExpSec = xcat::ClampWatchdogNoExpSec(cfg.launcherWatchdogNoExpSec);
    cfg.launcherWatchdogCooldownSec =
        xcat::ClampWatchdogCooldownSec(cfg.launcherWatchdogCooldownSec);
    g.mask = cfg.launcherHangupScheduleMask;
    g.localHour = CurrentLocalHourImpl();
    g.noExpLimitSec = cfg.launcherWatchdogNoExpSec;

    if (!hangupOn && !watchdogOn) {
        // 手动干净重拉不依赖挂机/守护开关；在途时禁止 Reset 清掉 job / restartInFlight。
        if (RelaunchInFlight() || g.runtime.restartInFlight || msc::weblogin::IsBusy()) {
            g.launchBusy =
                msc::weblogin::IsBusy() || RelaunchInFlight() || g.runtime.restartInFlight;
            if (!msc::weblogin::IsBusy() && !RelaunchInFlight()) {
                g.runtime.restartInFlight = false;
                g.launchBusy = false;
                g.mode = UiMode::Disabled;
            } else {
                g.mode = UiMode::Starting;
            }
            return;
        }
        Reset();
        return;
    }

    const bool scheduleActive =
        hangupOn ? IsHourActiveImpl(g.mask, g.localHour) : true;
    if (hangupOn &&
        (!g.haveScheduleActive || g.scheduleActive != scheduleActive)) {
        xcat::log::Info("Hangup", "schedule %s->%s hour=%d mask=0x%06X",
                        g.haveScheduleActive ? (g.scheduleActive ? "on" : "off") : "init",
                        scheduleActive ? "on" : "off", g.localHour, g.mask);
        g.haveScheduleActive = true;
    }
    if (!hangupOn) {
        if (g.haveScheduleActive) {
            xcat::log::Info("Hangup", "hangup off->ignore mask hour=%d", g.localHour);
        }
        g.haveScheduleActive = false;
    }
    g.scheduleActive = scheduleActive;

    // ---- Hangup kill / start ----
    if (hangupOn && !g.launchBusy && !RelaunchInFlight()) {
        if (!scheduleActive) {
            g.mode = UiMode::OffHour;
            if (LaunchChainPresent()) {
                (void)KillLaunchChain("hangup-off", "Hangup");
                PushStatus(ui, "[Hangup] 非挂机时段，结束游戏与 NGM", "挂机时段：非挂机（已关机）");
                ClearSessionTrack();
            }
        } else if (!ClassicPresent()) {
            // If watchdog is on and session was armed/awaiting, let ProcessDead own the relaunch
            // (cleaner path). Hangup only cold-starts when no prior session.
            const bool deferToWatchdog =
                watchdogOn && (g.sessionArmed || g.awaitingPlayable);
            const bool attachMode =
                attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode());
            if (!deferToWatchdog &&
                !CooldownBlocks(now, g.lastStartTick, kHangupStartCooldownSec) &&
                (attachMode || msc::weblogin::CanStartOneClick())) {
                // 冷启前清残留 NGM：否则官网 Main 常不再拉新经典版（与干净重拉同根）。
                bool ngmBlocking = false;
                if (NgmPresent()) {
                    (void)KillLaunchChain("hangup-cold", "Hangup");
                    if (NgmPresent()) {
                        ngmBlocking = true;
                        g.mode = UiMode::Starting;
                        PushStatus(ui, "[Hangup] 挂机时段：正在清理残留 NGM…",
                                   "挂机时段：清理 NGM 后拉起…");
                    }
                }
                if (!ngmBlocking) {
                    g.lastStartTick = now;
                    g.mode = UiMode::Starting;
                    if (attachMode) {
                        xcat::log::Info("Hangup",
                                        "schedule on->attach-watch hour=%d mask=0x%06X", g.localHour,
                                        g.mask);
                        PushStatus(ui, "[Hangup] 挂机时段：请手动开游戏（监视自动注入）",
                                   "挂机时段：监视中，请手动拉起游戏");
                        if (!attach_inject::IsWatching()) {
                            (void)attach_inject::StartWatch();
                        }
                        ui.pendingAutoLaunch = false;
                        ui.autoLaunchNotBeforeMs = 0;
                    } else {
                        if (attach_inject::GetLaunchMode() ==
                            attach_inject::LaunchMode::GamaPassAuto) {
                            msc::weblogin::SetAuthStrategy(
                                msc::weblogin::AuthStrategy::GamaPassAuto);
                            xcat::log::Info("Hangup", "schedule on->gamapass hour=%d mask=0x%06X",
                                            g.localHour, g.mask);
                            PushStatus(ui, "[Hangup] 挂机时段，GAMA PASS 自动启动并注入",
                                       "挂机时段：正在按时段拉起…");
                        } else {
                            if (msc::weblogin::GetAuthStrategy() ==
                                msc::weblogin::AuthStrategy::GamaPassAuto) {
                                msc::weblogin::SetAuthStrategy(
                                    msc::weblogin::AuthStrategy::HttpFirst);
                            }
                            xcat::log::Info("Hangup",
                                            "schedule on->http-oneclick hour=%d mask=0x%06X",
                                            g.localHour, g.mask);
                            PushStatus(ui, "[Hangup] 挂机时段，gamania (HK) 自动启动并注入",
                                       "挂机时段：正在按时段拉起…");
                        }
                        if (LaunchPanel_StartOneClick(ui, /*honorStrategyPrep=*/false))
                            g.launchBusy = true;
                    }
                }
            } else {
                g.mode = UiMode::OnHour;
            }
        } else {
            g.mode = UiMode::OnHour;
        }
    } else if (!hangupOn) {
        g.mode = UiMode::Disabled;
    } else if (g.launchBusy || RelaunchInFlight()) {
        g.mode = UiMode::Starting;
    }

    if (!watchdogOn) {
        // 仅关守护时仍可能有手动/挂机触发的干净重拉；勿整表清 runtime 把 restartInFlight 冲掉。
        if (RelaunchInFlight() || g.runtime.restartInFlight || msc::weblogin::IsBusy()) {
            g.launchBusy =
                msc::weblogin::IsBusy() || RelaunchInFlight() || g.runtime.restartInFlight;
            if (!msc::weblogin::IsBusy() && !RelaunchInFlight()) {
                g.runtime.restartInFlight = false;
                g.launchBusy = false;
            }
            g.watchdogMode = WatchdogUiMode::Off;
            g.combatHold = false;
            g.gate = hangupOn ? (scheduleActive ? "schedule-on" : "schedule-off") : "none";
            if (!RelaunchInFlight() && !g.runtime.restartInFlight) ClearSessionTrack();
            return;
        }
        g.runtime = {};
        g.watchdogMode = WatchdogUiMode::Off;
        g.gate = hangupOn ? (scheduleActive ? "schedule-on" : "schedule-off") : "none";
        g.combatHold = false;
        if (!RelaunchInFlight()) ClearSessionTrack();
        return;
    }

    // ---- Guardian ----
    // 进程名快照偶发漏检：CreateToolhelp32Snapshot 空表会被当成 ProcessDead，
    // 下一拍干净重拉（UI 写「正在杀死」），其实 TerminateProcess 一个都没杀到。
    // 先信 trackedPid 的 OpenProcess，再信 payload SHM 心跳（DLL 还在写 = 进程还在）。
    DWORD livePid = ClassicPid();
    if (livePid == 0 && g.trackedPid != 0 && xcat::IsProcessAlive(g.trackedPid)) {
        livePid = g.trackedPid;
        static uint64_t sLastNameMissLog = 0;
        if (sLastNameMissLog == 0 || now - sLastNameMissLog >= 5000u) {
            sLastNameMissLog = now;
            xcat::log::Warn(
                "Watchdog",
                "Classic name lookup miss — trackedPid=%u still alive (not process-dead)",
                static_cast<unsigned>(g.trackedPid));
        }
    }
    bool processAlive = livePid != 0 && xcat::IsProcessAlive(livePid);

    xcat::PayloadStatus st{};
    const bool stOk = xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st);
    const bool stFresh = stOk && xcat::PayloadStatusHeartbeatFresh(st, now, 5000);
    const bool handshakeOk = stFresh && st.ipcHandshake != 0;
    if (!processAlive && stFresh && handshakeOk) {
        processAlive = true;
        if (livePid == 0) livePid = g.trackedPid;
        static uint64_t sLastHbAliveLog = 0;
        if (sLastHbAliveLog == 0 || now - sLastHbAliveLog >= 5000u) {
            sLastHbAliveLog = now;
            xcat::log::Warn(
                "Watchdog",
                "Classic pid empty but payload heartbeat fresh — skip process-dead pid=%u",
                static_cast<unsigned>(livePid));
        }
    }
    const bool playable = stFresh && st.localPlayerOk != 0;
    g.sceneState = stFresh ? st.sceneState : -1;
    g.currentMapId = (stFresh && st.mapId != 0u) ? st.mapId : 0u;
    // 拍卖 GlobalMarket=5 / 商城 CashShop=4：已离开挂机图，无经验是正常的。
    // 若当 status-not-ready，会从进拍卖起计 180s 再干净重拉。
    // InterStage(1) / Login(2) / None(0) 不停表：切图或选角卡死正是守护要抬的。
    const bool inMarket = stFresh && (st.sceneState == 4 || st.sceneState == 5);

    ArmSessionIfLive(livePid, handshakeOk);

    // 进图成功：结束冷启归属计时。
    if (playable && processAlive) {
        g.awaitingPlayable = false;
        g.coldProcessSeenTick = 0;
        g.coldHandshakeSeenTick = 0;
    }

    // 冷启宽限（对照 UI 文案）：主门 2×N（进程起来后）+ 次门 N（已握手）→ 之后才计未进图。
    // 进程已退出时仍保持 launchOwned，好让 ProcessDead 能干净重拉（不再卡死在 status-not-ready）。
    const uint32_t noExp = cfg.launcherWatchdogNoExpSec;
    bool progressGrace = false;
    bool payloadHeartbeatStale = false;
    bool prePlayableStuck = false;
    g.uiColdStart = false;
    g.uiColdGraceRemainSec = 0;
    g.uiColdWaitingProcess = false;
    g.uiLaunchOwnedPendingPlayable = false;

    if (g.awaitingPlayable && !playable) {
        g.uiColdStart = true;
        if (!processAlive) {
            // 尚未见过本轮 Classic：等待拉起。已 track 后退出 → 交给 ProcessDead，不挡重启。
            if (g.trackedPid == 0) {
                progressGrace = true;
                g.uiColdWaitingProcess = true;
            }
        } else {
            if (!g.coldProcessSeenTick) g.coldProcessSeenTick = now;
            const uint64_t mainCapMs = static_cast<uint64_t>(noExp) * 2u * 1000u;
            const uint64_t sinceProc =
                now >= g.coldProcessSeenTick ? now - g.coldProcessSeenTick : 0;
            if (sinceProc < mainCapMs) {
                progressGrace = true;
                g.uiColdGraceRemainSec =
                    static_cast<uint32_t>((mainCapMs - sinceProc + 999u) / 1000u);
            } else if (handshakeOk) {
                if (!g.coldHandshakeSeenTick) g.coldHandshakeSeenTick = now;
                const uint64_t secCapMs = static_cast<uint64_t>(noExp) * 1000u;
                const uint64_t sinceHs =
                    now >= g.coldHandshakeSeenTick ? now - g.coldHandshakeSeenTick : 0;
                if (sinceHs < secCapMs) {
                    progressGrace = true;
                    g.uiColdGraceRemainSec =
                        static_cast<uint32_t>((secCapMs - sinceHs + 999u) / 1000u);
                } else {
                    prePlayableStuck = true;
                    g.uiLaunchOwnedPendingPlayable = true;
                }
            } else if (!stFresh) {
                payloadHeartbeatStale = true;
                g.uiLaunchOwnedPendingPlayable = true;
            } else {
                prePlayableStuck = true;
                g.uiLaunchOwnedPendingPlayable = true;
            }
        }
    } else if (g.sessionArmed && processAlive && !playable) {
        // 已进图武装过又掉可玩态，或冷启归属已清但仍未 localPlayerOk。
        if (!stFresh) {
            payloadHeartbeatStale = true;
        } else if (!g.runtime.haveStatus) {
            prePlayableStuck = true;
            g.uiLaunchOwnedPendingPlayable = true;
        }
    }

    // Crash / 退出：awaiting 或已武装 → sticky injected，Evaluate 走 ProcessDead。
    const bool launchOwned = g.awaitingPlayable || g.sessionArmed;
    const bool crashedArmed = !processAlive && launchOwned && g.trackedPid != 0;
    const bool liveOwned =
        processAlive &&
        (handshakeOk ||
         (launchOwned && g.trackedPid != 0 && livePid == g.trackedPid));

    guardian_policy::Input input{};
    input.now = now;
    input.noExpSec = noExp;
    input.launchWorkerBusy = g.launchBusy || msc::weblogin::IsBusy() || RelaunchInFlight();
    input.gamePid =
        processAlive ? static_cast<uint32_t>(livePid)
                     : (crashedArmed ? static_cast<uint32_t>(g.trackedPid) : 0u);
    input.processAlive = processAlive;
    input.injected = liveOwned || crashedArmed;
    input.combatEnabled = cfg.simpleCombat != 0;
    input.scheduleActive = scheduleActive;
    input.statusReady = playable || inMarket;
    input.progressGrace = progressGrace;
    input.payloadHeartbeatStale = payloadHeartbeatStale;
    input.prePlayableStuck = prePlayableStuck;
    input.playerExpValid = stFresh && st.playerExpValid != 0;
    input.playerExp = input.playerExpValid ? st.playerExp : 0;
    // combatHold 要靠这戳：从未赋值时 CombatRecentlyActive 恒假，F5 打怪也会
    // 按「EXP 停滞」180s 干净重拉（BIN 11:45:40 exp=9417、Classic x1）。
    // 进图且自动打怪开着：每拍刷新活动戳，硬顶 noExp+60s；真有经验涨仍会清零。
    if (input.combatEnabled && playable && processAlive) {
        input.combatActivityTickMs = now;
    }
    input.mapIdValid = stFresh && st.mapId != 0;
    input.mapId = input.mapIdValid ? st.mapId : 0;
    // 主城暂停无经验：仅信 payload mapIsTown（map_info.town=1，含药店室内）。
    // 不信 mapId%1000000==0（107000000 沼澤地Ⅰ误判），也不信原生 IsTown。
    // 2转考验场：无经验任务怪（BIN 108000400 盜賊2轉考驗場），按主城停表。
    // 拍卖/商城：无 mapId，靠 sceneState 停表（勿走 status-not-ready 计时）。
    if (inMarket) {
        input.safeZonePause = true;
    } else if (input.mapIdValid && IsSecondJobTrialMap(input.mapId)) {
        input.safeZonePause = true;
    } else if (stFresh && st.version >= 11u && st.mapIsTownValid != 0) {
        input.safeZonePause = st.mapIsTown != 0;
    } else {
        input.safeZonePause = false;
    }

    // 服务器踢线/断线边沿 → hard-fail → 干净重拉（不依赖「自动打怪」）。
    // 契约：hold 只挡踢线 seq 硬杀，让软路径先试。
    // 不挡：进程已死；无经验/心跳停滞已满 noExpSec；hold 墙钟已满 noExpSec。
    // 成功上升沿吞掉本轮 disconnectSeq，避免 hold 结束后误重拉。
    // 尚未进图（awaitingPlayable / !haveStatus）：吞掉选频选角 Session 闪断，勿硬杀。
    bool softHoldBlocksRelaunch = false;
    if (g.sessionArmed && stFresh) {
        const uint32_t softResult = (st.version >= 8u) ? st.softLoginResult : 0u;
        g.softLoginHoldLatched = (st.version >= 8u && st.softLoginHold != 0u);
        if (g.softLoginHoldLatched) {
            if (g.softHoldSinceTick == 0) g.softHoldSinceTick = now;
        } else {
            g.softHoldSinceTick = 0;
        }
        // 0→1 武装短窗（即使本拍 seq 没涨：下一跳闪断常落在 hold 已放之后）。
        if (softResult == 1u && g.lastSeenSoftLoginResult != 1u) {
            g.softSuccessGraceUntil = now + kSoftSuccessGraceMs;
            if (st.disconnectSeq > g.lastConsumedDisconnectSeq) {
                xcat::log::Info("Watchdog",
                                "soft_login success — absorb disconnectSeq %u->%u (no clean relaunch)",
                                g.lastConsumedDisconnectSeq, st.disconnectSeq);
                g.lastConsumedDisconnectSeq = st.disconnectSeq;
                g.lastLoggedDisconnectSeq = st.disconnectSeq;
            }
        }
        // 软路径完全失败：显式放行踢线硬失败（不依赖 hold 已清的竞态）。
        if (softResult == 2u && g.lastSeenSoftLoginResult != 2u) {
            g.softSuccessGraceUntil = 0;
            input.reloginHardFailed = true;
            input.hardFailCode = xcat::kHardFailServerKick;
            xcat::log::Warn(
                "Watchdog",
                "soft_login completely failed — allow guardian clean relaunch seq=%u",
                st.disconnectSeq);
        }
        g.lastSeenSoftLoginResult = softResult;
        if (!g.haveDisconnectBaseline) {
            g.lastConsumedDisconnectSeq = st.disconnectSeq;
            g.haveDisconnectBaseline = true;
        } else if (st.disconnectSeq > g.lastConsumedDisconnectSeq) {
            if (st.version >= 8u && st.softLoginHold != 0u) {
                if (g.lastLoggedDisconnectSeq != st.disconnectSeq) {
                    g.lastLoggedDisconnectSeq = st.disconnectSeq;
                    xcat::log::Info(
                        "Watchdog",
                        "soft_login hold — defer kick relaunch seq=%u state=%d err=%d",
                        st.disconnectSeq, st.sessionState, st.pendingErrorCode);
                }
            } else if ((g.awaitingPlayable || !g.runtime.haveStatus) && !playable &&
                       !input.reloginHardFailed) {
                // 尚未进图（选频/选角/加载）：AutoEnter 确认角色时常 Connected→Disconnected 闪断
                // （upload caa553：verdict=lean_local_or_soft，软重连未开时被硬杀）。
                // 吞掉本轮 seq；真卡死交给 prePlayableStuck / 冷启超时，不在此干净杀。
                if (g.lastLoggedDisconnectSeq != st.disconnectSeq) {
                    g.lastLoggedDisconnectSeq = st.disconnectSeq;
                    xcat::log::Info(
                        "Watchdog",
                        "pre-playable absorb disconnectSeq %u->%u state=%d err=%d "
                        "(await=%u haveStatus=%u — no clean relaunch)",
                        g.lastConsumedDisconnectSeq, st.disconnectSeq, st.sessionState,
                        st.pendingErrorCode, g.awaitingPlayable ? 1u : 0u,
                        g.runtime.haveStatus ? 1u : 0u);
                }
                g.lastConsumedDisconnectSeq = st.disconnectSeq;
            } else if (g.softSuccessGraceUntil != 0 && now < g.softSuccessGraceUntil &&
                       !input.reloginHardFailed) {
                // hold 已放、result 仍为 1：落地静默闪断不得硬杀（BIN 01:00:24→25 / 01:04:49→51）。
                if (g.lastLoggedDisconnectSeq != st.disconnectSeq) {
                    g.lastLoggedDisconnectSeq = st.disconnectSeq;
                    xcat::log::Info(
                        "Watchdog",
                        "soft_login post-success absorb disconnectSeq %u->%u state=%d err=%d "
                        "(grace remain=%llums — no clean relaunch)",
                        g.lastConsumedDisconnectSeq, st.disconnectSeq, st.sessionState,
                        st.pendingErrorCode,
                        static_cast<unsigned long long>(g.softSuccessGraceUntil - now));
                }
                g.lastConsumedDisconnectSeq = st.disconnectSeq;
            } else if (!input.reloginHardFailed) {
                // 软未 hold（未武装/未接管）且已进过图：立即硬失败。
                // 已在 result=2 上升沿置位则勿重复刷日志。
                input.reloginHardFailed = true;
                input.hardFailCode = xcat::kHardFailServerKick;
                if (g.lastLoggedDisconnectSeq != st.disconnectSeq) {
                    g.lastLoggedDisconnectSeq = st.disconnectSeq;
                    xcat::log::Warn(
                        "Watchdog",
                        "server kick/disconnect seq %u->%u state=%d err=%d",
                        g.lastConsumedDisconnectSeq, st.disconnectSeq, st.sessionState,
                        st.pendingErrorCode);
                }
            }
        }
    }
    // soft hold 期间心跳常不新鲜：勿因 !stFresh 清 latch，否则 ProcessDead 诊断 Warn 丢。
    // latch 只在：新鲜 SHM 写 hold=0/1、ProcessDead 打过日志、ClearSessionTrack。
    // 进程仍在时：latch/hold 默认挡住踢线干净重拉；到期的无经验/状态停滞见下方放行。
    softHoldBlocksRelaunch = processAlive && g.softLoginHoldLatched;

    const guardian_policy::RuntimeState runtimeBefore = g.runtime;
    guardian_policy::Decision decision = guardian_policy::Evaluate(g.runtime, input);
    if (decision.gate == guardian_policy::Gate::ProcessDead &&
        decision.action == guardian_policy::Action::Restart) {
        const auto probe = game_exit_probe::ProbeRecentClassicFault(
            static_cast<uint32_t>(g.trackedPid), 180u);
        decision.restartReason = game_exit_probe::ReasonLabel(probe.kind);
        xcat::log::Info("Watchdog", "process-dead probe %s",
                        probe.detail[0] ? probe.detail : decision.restartReason);
    }
    g.runtime = decision.next;
    g.gate = guardian_policy::GateLabel(decision.gate);
    g.watchdogMode = ToWatchdogUi(g.runtime.mode, true);
    g.combatHold = decision.combatHold;
    g.combatHoldHardLimitSec =
        decision.combatHold ? guardian_policy::CombatHoldHardLimitSec(input.noExpSec) : 0;

    if (decision.next.mode == guardian_policy::Mode::Healthy && processAlive) {
        // Healthy live session: refresh track; keep armed.
        ArmSessionIfLive(livePid, handshakeOk);
    }

    if (decision.stateChanged) {
        if (decision.gate == guardian_policy::Gate::ProcessDead && g.softLoginHoldLatched) {
            xcat::log::Warn(
                "Watchdog",
                "process-dead during soft_login hold (Classic already gone — not kick-kill; "
                "soft attempt aborted)");
            g.softLoginHoldLatched = false;
            g.softHoldSinceTick = 0;
            softHoldBlocksRelaunch = false;
        }
        xcat::log::Info("Watchdog", "mode %s->%s gate=%s action=%s reason=%s stale=%u",
                        guardian_policy::ModeLabel(decision.previousMode),
                        guardian_policy::ModeLabel(decision.next.mode), g.gate,
                        decision.action == guardian_policy::Action::Restart ? "restart" : "none",
                        decision.restartReason ? decision.restartReason : "-",
                        decision.staleSec);
    }

    LogGateThrottled(now, g.gate, cfg);

    if (decision.warnStatusConfirm) {
        xcat::log::Warn("Watchdog", "payload stale confirm 1/2 stale=%us limit=%us",
                        decision.staleSec, input.noExpSec);
        return;
    }
    if (decision.warnExpConfirm) {
        xcat::log::Warn("Watchdog", "exp stale confirm 1/2 stale=%us limit=%us exp=%llu",
                         decision.staleSec, input.noExpSec,
                         static_cast<unsigned long long>(st.playerExp));
        return;
    }

    if (decision.action != guardian_policy::Action::Restart) return;
    if (!scheduleActive) return;
    if (RelaunchInFlight()) return;
    if (softHoldBlocksRelaunch) {
        const uint64_t holdMs =
            (g.softHoldSinceTick != 0 && now >= g.softHoldSinceTick)
                ? (now - g.softHoldSinceTick)
                : 0;
        const uint64_t capMs = static_cast<uint64_t>(noExp) * 1000u;
        const bool staleDue = decision.staleSec >= noExp && !input.reloginHardFailed;
        const bool holdDue = capMs > 0 && holdMs >= capMs;
        if (staleDue || holdDue) {
            xcat::log::Warn(
                "Watchdog",
                "soft_login hold expired — allow relaunch reason=%s stale=%us hold=%llums "
                "cap=%us",
                decision.restartReason ? decision.restartReason : "?", decision.staleSec,
                static_cast<unsigned long long>(holdMs), noExp);
            softHoldBlocksRelaunch = false;
        }
    }
    if (softHoldBlocksRelaunch) {
        // 丢弃本拍 Restart 状态推进，避免 restartInFlight 空转却不杀进程。
        g.runtime = runtimeBefore;
        g.gate = guardian_policy::GateLabel(guardian_policy::Gate::None);
        g.watchdogMode = ToWatchdogUi(g.runtime.mode, true);
        g.combatHold = false;
        g.combatHoldHardLimitSec = 0;
        static uint64_t sLastSoftBlockLog = 0;
        if (sLastSoftBlockLog == 0 || now - sLastSoftBlockLog >= 5000u) {
            sLastSoftBlockLog = now;
            xcat::log::Info(
                "Watchdog",
                "soft_login hold — defer guardian relaunch reason=%s (wait soft fail/success)",
                decision.restartReason ? decision.restartReason : "?");
        }
        return;
    }

    char line[192]{};
    snprintf(line, sizeof(line), "[Watchdog] 干净重拉：%s (stale=%us)",
             decision.restartReason ? decision.restartReason : "未知", decision.staleSec);
    char status[128]{};
    snprintf(status, sizeof(status), "守护模式：%s",
             decision.restartReason ? decision.restartReason : "正在干净重拉…");
    const uint32_t cooldownSec =
        decision.bypassRestartCooldown ? 0u : cfg.launcherWatchdogCooldownSec;
    if (BeginCleanRelaunch(ui, now, cooldownSec, line, status)) {
        g.runtime.restartInFlight = true;
        g.watchdogMode = WatchdogUiMode::Recovering;
    }
}

bool IsCleanRelaunchInFlight() {
    // WaitingGone/Settling，或 settle 后一键冷启未结束（restartInFlight）。
    // 不含普通「一键启动」忙碌，避免误显「正在杀死并重拉」。
    return RelaunchInFlight() || g.runtime.restartInFlight;
}

bool RequestManualCleanRelaunch(LaunchUiState& ui) {
    if (RelaunchInFlight() || g.launchBusy || msc::weblogin::IsBusy()) {
        PushStatus(ui, nullptr, "正在启动/重拉中，请稍候再点");
        return false;
    }
    const bool attachMode =
        attach_inject::IsAttachWatchMode(attach_inject::GetLaunchMode());
    if (!attachMode && !msc::weblogin::CanStartOneClick()) {
        PushStatus(ui, nullptr, "当前无法启动（请检查 GAMA PASS 会话/环境）");
        return false;
    }
    if (!AllowsLaunch(ui.prefsBinDir)) {
        PushStatus(ui, "[UI] 非挂机时段，禁止手动重拉", "非挂机时段，已跳过重拉");
        return false;
    }
    const uint64_t now = GetTickCount64();
    if (!BeginCleanRelaunch(ui, now, /*cooldownSec=*/0, "[UI] 手动干净重拉",
                            "正在杀死游戏并重拉…")) {
        PushStatus(ui, nullptr, "无法开始重拉（忙碌或未就绪）");
        return false;
    }
    g.runtime.restartInFlight = true;
    g.watchdogMode = WatchdogUiMode::Recovering;
    return true;
}

void NoteLaunchStarted(uint32_t /*graceSec*/) {
    // 置 awaitingPlayable：进图前归属本轮一键；进程退出可 ProcessDead，未进图走冷启宽限。
    g.awaitingPlayable = true;
    g.coldProcessSeenTick = 0;
    g.coldHandshakeSeenTick = 0;
    g.lastStartTick = GetTickCount64();
    if (g.mode == UiMode::Disabled || g.mode == UiMode::OffHour) {
        // 非挂机调度触发时不改 mode；手动启动仍记时间戳即可。
    } else {
        g.mode = UiMode::Starting;
    }
    xcat::log::Info("Watchdog", "NoteLaunchStarted awaitingPlayable=1");
}

}  // namespace xcat::app::hangup_schedule
