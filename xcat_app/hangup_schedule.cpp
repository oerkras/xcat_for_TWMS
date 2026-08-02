#include "hangup_schedule.h"

#include "app_notify.h"
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
constexpr uint32_t kHangupStartCooldownSec = 120;
// Async clean relaunch (UI-thread state machine; no long Sleep in Tick).
constexpr uint64_t kRelaunchGoneWaitMs = 15000;
constexpr uint64_t kRelaunchRetryKillAtMs = 8000;
constexpr uint64_t kRelaunchSettleMs = 2500;

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
    // kick_sniff disconnectSeq baseline：武装后先对齐再边沿触发，避免历史断线误重拉。
    uint32_t lastConsumedDisconnectSeq = 0;
    uint32_t lastLoggedDisconnectSeq = 0;
    bool haveDisconnectBaseline = false;
    uint64_t lastCooldownBlockLogTick = 0;
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
    g.lastConsumedDisconnectSeq = 0;
    g.lastLoggedDisconnectSeq = 0;
    g.haveDisconnectBaseline = false;
}

void ArmSessionIfLive(DWORD livePid, bool handshakeOk) {
    if (livePid == 0) return;
    g.trackedPid = livePid;
    if (handshakeOk) g.sessionArmed = true;
}

// Begin clean relaunch: kill Classic, wait-gone, settle, then one-click.
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
    if (!msc::weblogin::CanStartOneClick()) {
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

    const unsigned n = xcat::KillProcessesByExeName(kClassicExe);
    xcat::log::Info("Watchdog", "clean relaunch: kill Classic x%u", n);

    g.relaunch = {};
    g.relaunch.phase = RelaunchPhase::WaitingGone;
    g.relaunch.phaseSince = now;
    if (logLine) strncpy_s(g.relaunch.logLine, logLine, _TRUNCATE);
    if (statusLine) strncpy_s(g.relaunch.statusLine, statusLine, _TRUNCATE);
    PushStatus(ui, g.relaunch.logLine,
               statusLine && statusLine[0] ? statusLine : "守护模式：正在结束旧游戏…");
    g.mode = UiMode::Starting;
    g.launchBusy = true;  // block hangup parallel start

    // 气泡 + 历史事件（PushLocal → eventlog::Record）；此前只写启动面板日志，历史空白。
    const char* body =
        (statusLine && statusLine[0]) ? statusLine
        : (logLine && logLine[0])     ? logLine
                                     : "正在杀死游戏并干净重拉";
    notify::PushLocal(/*Warning*/ 2, "watchdog-clean-relaunch", "守护干净重拉", body, 7000);
    return true;
}

// Advance async clean relaunch; call once per Tick while phase != None.
void PumpCleanRelaunch(LaunchUiState& ui, uint64_t now) {
    if (!RelaunchInFlight()) return;

    switch (g.relaunch.phase) {
    case RelaunchPhase::WaitingGone: {
        if (!ClassicPresent()) {
            g.relaunch.phase = RelaunchPhase::Settling;
            g.relaunch.phaseSince = now;
            PushStatus(ui, nullptr, "守护模式：旧进程已退出，冷却后重拉…");
            xcat::log::Info("Watchdog", "clean relaunch: Classic gone → settle %llums",
                            static_cast<unsigned long long>(kRelaunchSettleMs));
            break;
        }
        const uint64_t waited = now >= g.relaunch.phaseSince ? now - g.relaunch.phaseSince : 0;
        if (!g.relaunch.retriedKill && waited >= kRelaunchRetryKillAtMs) {
            g.relaunch.retriedKill = true;
            const unsigned n = xcat::KillProcessesByExeName(kClassicExe);
            xcat::log::Warn("Watchdog", "clean relaunch: retry kill Classic x%u", n);
        }
        if (waited >= kRelaunchGoneWaitMs) {
            xcat::log::Warn("Watchdog",
                            "clean relaunch aborted: Classic still present after %llums",
                            static_cast<unsigned long long>(kRelaunchGoneWaitMs));
            PushStatus(ui, "[Watchdog] 旧游戏未清净，已中止重拉",
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
        if (ClassicPresent()) {
            // Respawned / kill raced — back to waiting.
            g.relaunch.phase = RelaunchPhase::WaitingGone;
            g.relaunch.phaseSince = now;
            g.relaunch.retriedKill = false;
            xcat::log::Warn("Watchdog", "clean relaunch: Classic reappeared during settle");
            break;
        }
        if (now - g.relaunch.phaseSince < kRelaunchSettleMs) break;

        // Final confirm (fengxing: gone=false forbids relaunch).
        if (ClassicPresent()) {
            PushStatus(ui, "[Watchdog] settle 后仍有 Classic，中止重拉",
                       "守护模式：旧进程未清净");
            ClearRelaunchJob();
            g.launchBusy = false;
            g.runtime.restartInFlight = false;
            break;
        }

        PushStatus(ui, g.relaunch.logLine[0] ? g.relaunch.logLine : "[Watchdog] 干净重拉",
                   g.relaunch.statusLine[0] ? g.relaunch.statusLine
                                            : "守护模式：正在重新一键启动…");
        ClearRelaunchJob();
        if (!LaunchPanel_StartOneClick(ui)) {
            g.launchBusy = false;
            g.runtime.restartInFlight = false;
            xcat::log::Warn("Watchdog", "clean relaunch: StartOneClick failed");
        } else {
            g.launchBusy = true;
            g.mode = UiMode::Starting;
            g.runtime.restartInFlight = true;
            xcat::log::Info("Watchdog", "clean relaunch: one-click started");
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
                    "trackedPid=%lu armed=%u",
                    gate, cfg.launcherWatchdog, cfg.launcherHangupSchedule,
                    cfg.launcherWatchdogNoExpSec, cfg.simpleCombat,
                    g.scheduleActive ? "on" : "off",
                    static_cast<unsigned long>(g.trackedPid), g.sessionArmed ? 1u : 0u);
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
    if (g.runtime.haveStatus && g.runtime.statusLostTick && now >= g.runtime.statusLostTick) {
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

std::string FormatWatchdogTimerText(const Snapshot& snap) {
    char buf[80]{};
    if (!snap.watchdogOn) return "守护关闭";
    if (!snap.scheduleActive) return "非挂机（已关机）";
    if (snap.launchBusy && snap.mode == UiMode::Starting) return "正在干净重拉…";
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
    if (snap.combatHold) {
        const uint32_t holdLimit =
            snap.combatHoldHardLimitSec ? snap.combatHoldHardLimitSec : limit + 60u;
        snprintf(buf, sizeof(buf), "战斗暂缓 %u/%u秒", elapsed, holdLimit);
        return buf;
    }
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
            if (ClassicPresent()) {
                const unsigned n = xcat::KillProcessesByExeName(kClassicExe);
                xcat::log::Info("Hangup", "schedule off->kill Classic x%u hour=%d", n,
                                g.localHour);
                PushStatus(ui, "[Hangup] 非挂机时段，结束游戏", "挂机时段：非挂机（已关机）");
                ClearSessionTrack();
            }
        } else if (!ClassicPresent()) {
            // If watchdog is on and session was armed, let ProcessDead own the relaunch
            // (cleaner path). Hangup only cold-starts when no prior session.
            const bool deferToWatchdog = watchdogOn && g.sessionArmed;
            if (!deferToWatchdog &&
                !CooldownBlocks(now, g.lastStartTick, kHangupStartCooldownSec) &&
                msc::weblogin::CanStartOneClick()) {
                g.lastStartTick = now;
                g.mode = UiMode::Starting;
                xcat::log::Info("Hangup", "schedule on->start hour=%d mask=0x%06X", g.localHour,
                                g.mask);
                PushStatus(ui, "[Hangup] 挂机时段，自动启动并注入",
                           "挂机时段：正在按时段拉起…");
                if (LaunchPanel_StartOneClick(ui)) g.launchBusy = true;
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
    const DWORD livePid = ClassicPid();
    const bool processAlive = livePid != 0 && xcat::IsProcessAlive(livePid);

    xcat::PayloadStatus st{};
    const bool stOk = xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st);
    const bool stFresh = stOk && xcat::PayloadStatusHeartbeatFresh(st, now, 5000);
    const bool handshakeOk = stFresh && st.ipcHandshake != 0;
    const bool playable = stFresh && st.localPlayerOk != 0;

    ArmSessionIfLive(livePid, handshakeOk);
    // Crash: livePid==0 but sessionArmed → sticky gamePid + injected so ProcessDead fires.
    const bool crashedArmed = !processAlive && g.sessionArmed && g.trackedPid != 0;

    guardian_policy::Input input{};
    input.now = now;
    input.noExpSec = cfg.launcherWatchdogNoExpSec;
    input.launchWorkerBusy = g.launchBusy || msc::weblogin::IsBusy() || RelaunchInFlight();
    input.gamePid =
        processAlive ? static_cast<uint32_t>(livePid)
                     : (crashedArmed ? static_cast<uint32_t>(g.trackedPid) : 0u);
    input.processAlive = processAlive;
    input.injected =
        (processAlive && handshakeOk) || crashedArmed;
    input.combatEnabled = cfg.simpleCombat != 0;
    input.scheduleActive = scheduleActive;
    input.statusReady = playable;
    input.playerExpValid = stFresh && st.playerExpValid != 0;
    input.playerExp = input.playerExpValid ? st.playerExp : 0;
    input.mapIdValid = stFresh && st.mapId != 0;
    input.mapId = input.mapIdValid ? st.mapId : 0;

    // 服务器踢线/断线边沿 → hard-fail → 干净重拉（不依赖「自动打怪」）。
    if (g.sessionArmed && stFresh) {
        if (!g.haveDisconnectBaseline) {
            g.lastConsumedDisconnectSeq = st.disconnectSeq;
            g.haveDisconnectBaseline = true;
        } else if (st.disconnectSeq > g.lastConsumedDisconnectSeq) {
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

    const guardian_policy::Decision decision = guardian_policy::Evaluate(g.runtime, input);
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
    if (!msc::weblogin::CanStartOneClick()) {
        PushStatus(ui, nullptr,
                   msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::WebViewOnly
                       ? "WebView2 尚未就绪，无法重拉"
                       : "当前无法启动（请检查策略/账号）");
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

}  // namespace xcat::app::hangup_schedule
