// Classic TWMS — encounter strategy FSM.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "encounter.h"

#include "../auto_lie/auto_lie.h"
#include "../channel_hop/channel_hop.h"
#include "../notify/notify.h"
#include "../player_hide/player_hide.h"
#include "../ports/user_pool_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
#include "../../runtime/log.h"
#include "xcat_sound.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace x::features::encounter {
namespace {

constexpr DWORD kTickMs = 200;
constexpr DWORD kCheckIntervalMs = 1000;
constexpr DWORD kConfirmMs = 4000;
constexpr DWORD kFirstLandGraceMs = 5000;
// BIN：宽限须 ≥ channel_hop 成功冷却，否则宽限一过就狂 RequestHop
constexpr DWORD kPostHopGraceMs = 22000;  // 对齐成功冷却 20s + 余量
constexpr DWORD kHopDeferLogMs = 4000;
constexpr DWORD kGmAlarmPulseMs = 3000;  // 对齐 auto_lie：威胁期间每 3s 强制 Alarm
constexpr int kHideSuspectConfirmSamples = 2;  // 连续 N 拍才认隐身，压进图/加载假阳
constexpr uint32_t kHopSeqBase = 0xE1000000u;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gEnabled{false};
std::atomic<bool> gStopCombat{true};
std::atomic<bool> gReconnect{true};
std::atomic<bool> gGmEscalate{true};
std::atomic<unsigned> gState{static_cast<unsigned>(State::Idle)};
std::atomic<int> gLastOther{-1};
std::atomic<int> gLastAdminLike{0};
std::atomic<int> gLastHideSuspect{0};

DWORD gPhaseAt = 0;
DWORD gLastSampleAt = 0;
DWORD gConfirmSince = 0;
DWORD gLandedAt = 0;
DWORD gHopGraceUntil = 0;
DWORD gLastHopDeferLog = 0;
int gLastMapId = 0;
bool gPaused = false;
uint32_t gHopSeq = kHopSeqBase;
int gNotifyOther = -1;
int gNotifyThreatKey = -1;  // admin*1000 + hide；变化才弹威胁通知
DWORD gLastGmAlarmAt = 0;
bool gGmAlarmActive = false;
int gHideSuspectStreak = 0;

void SetState(State s) { gState.store(static_cast<unsigned>(s)); }
State GetStateLocal() { return static_cast<State>(gState.load()); }

void Log(const char* fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    x::runtime::LogI("Encounter", "%s", body);
}

void Notify(notify::NotificationKind kind, const char* key, const char* title, const char* body) {
    notify::PublishNotification(notify::NotificationEvent{kind, key, title, body, 5200});
}

// payload 进程直播 Alarm（waveOut），不走面板 soundMuted；面板侧对同 key 跳过叠播。
void PulseGmAlarm(DWORD now, bool force) {
    if (!force && gLastGmAlarmAt != 0 && now - gLastGmAlarmAt < kGmAlarmPulseMs) return;
    gLastGmAlarmAt = now;
    gGmAlarmActive = true;
    xcat::sound::PlayAsync(xcat::sound::Id::Alarm);
}

void StopGmAlarm() {
    const bool was = gGmAlarmActive || gLastGmAlarmAt != 0;
    gGmAlarmActive = false;
    gLastGmAlarmAt = 0;
    if (!was) return;
    xcat::sound::CancelPlayback();
    notify::DismissNotification("encounter-gm-threat");
    notify::DismissNotification("encounter-gm-hop");
}

// 图号中间三位 000 → 主城启发式（对照枫星；误判只跳过触发）.
bool IsTownMapIdHeuristic(int mapId) {
    if (mapId <= 0) return false;
    return ((mapId / 1000) % 1000) == 0;
}

void PauseExposure(int other, bool threat) {
    if (gPaused) {
        // 热保持：有人期间每拍再钉 Encounter 位（位掩码下测谎清不掉本模块位，仍防漏闸）。
        if (gStopCombat.load() && !auto_lie::IsBusy())
            simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, true);
        return;
    }
    gPaused = true;
    if (gStopCombat.load())
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, true);

    char body[128]{};
    if (threat) {
        snprintf(body, sizeof(body), "同图疑似 GM/隐身（远程 %d），已立即停手。", other);
        Notify(notify::NotificationKind::Danger, "encounter-gm-threat", "疑似 GM/隐身", body);
        PulseGmAlarm(GetTickCount(), /*force=*/true);
    } else {
        snprintf(body, sizeof(body), "同图其他玩家 %d 人，已执行遇人策略。", other);
        Notify(notify::NotificationKind::Warning, "encounter-detected", "遇人策略已触发", body);
    }
    Log("pause other=%d threat=%d stopCombat=%d", other, threat ? 1 : 0, (int)gStopCombat.load());
}

void ResumeExposure(bool townSkip = false) {
    if (!gPaused) return;
    if (auto_lie::IsBusy()) {
        Log("resume deferred (auto_lie busy)");
        return;
    }
    if (gStopCombat.load())
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, false);
    gPaused = false;
    StopGmAlarm();
    if (townSkip) {
        Notify(notify::NotificationKind::Info, "encounter-town-skip", "主城已跳过遇人",
               "当前在主城，遇人停手已解除。");
        Log("resume town-skip");
    } else {
        Notify(notify::NotificationKind::Info, "encounter-restored", "遇人后已恢复",
               "同图已无其他玩家，挂机策略已恢复。");
        Log("resume");
    }
}

void ReleaseIfDisabled() {
    if (gPaused && !gEnabled.load()) {
        if (!auto_lie::IsBusy())
            simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, false);
        gPaused = false;
        StopGmAlarm();
        gHideSuspectStreak = 0;
    }
}

bool SampleThreat(ports::user_pool::RemoteThreatSample* out) {
    if (!out) return false;
    // 「隐藏同图其他玩家」会主动 SetActive(false)——绝不能当隐身 GM.
    const bool checkHide = !player_hide::IsEnabled();
    if (!ports::user_pool::SampleRemoteThreat(out, checkHide)) return false;
    if (out->remoteCount < 0) out->remoteCount = 0;
    gLastOther.store(out->remoteCount);
    gLastAdminLike.store(out->adminLikeCount);
    gLastHideSuspect.store(out->hideSuspectCount);
    return true;
}

// Admin/Manager 立刻升级；纯隐身嫌疑需连续确认，避免进图 avatar 未就绪误报.
bool IsElevatedThreat(const ports::user_pool::RemoteThreatSample& t) {
    if (t.adminLikeCount > 0) {
        gHideSuspectStreak = 0;
        return true;
    }
    if (t.hideSuspectCount > 0)
        ++gHideSuspectStreak;
    else
        gHideSuspectStreak = 0;
    return gHideSuspectStreak >= kHideSuspectConfirmSamples;
}

void RequestHop(int other, bool threat) {
    if (channel_hop::HasPending()) {
        Log("hop skip: channel_hop busy");
        return;
    }
    const DWORD cd = channel_hop::CooldownRemainingMs();
    if (cd > 0) {
        const DWORD now = GetTickCount();
        if (now - gLastHopDeferLog > kHopDeferLogMs) {
            gLastHopDeferLog = now;
            Log("hop defer: channel_hop cooldown %ums other=%d threat=%d", (unsigned)cd, other,
                threat ? 1 : 0);
        }
        // 不烧 seq：回 Watching，等冷却后再重新 confirm
        SetState(State::Watching);
        gConfirmSince = 0;
        if (now + cd > gHopGraceUntil) gHopGraceUntil = now + cd;
        return;
    }
    ++gHopSeq;
    if (gHopSeq < kHopSeqBase) gHopSeq = kHopSeqBase + 1;
    channel_hop::RequestManualRejoin(gHopSeq);
    char body[128]{};
    if (threat) {
        snprintf(body, sizeof(body), "疑似 GM/隐身（远程 %d），已立即换频。", other);
        Notify(notify::NotificationKind::Danger, "encounter-gm-hop", "疑似 GM 立即换频", body);
        // Alarm 已由 PauseExposure / Tick Pulse 负责，此处不再 force，避免同拍双响。
    } else {
        snprintf(body, sizeof(body), "检测到同图其他玩家 %d 人，已立即换频。", other);
        Notify(notify::NotificationKind::Warning, "encounter-hop", "遇人立即换频", body);
    }
    Log("hop seq=%u other=%d threat=%d", gHopSeq, other, threat ? 1 : 0);
    SetState(State::Hopping);
    gPhaseAt = GetTickCount();
    gHopGraceUntil = gPhaseAt + kPostHopGraceMs;
    gConfirmSince = 0;
}

void OnMapChange(int mapId, DWORD now) {
    gLastMapId = mapId;
    gLandedAt = now;
    gConfirmSince = 0;
    gNotifyThreatKey = -1;
    gHideSuspectStreak = 0;
    if (GetStateLocal() == State::Confirming) SetState(State::Watching);
    Log("map change id=%d grace=%ums", mapId, (unsigned)kFirstLandGraceMs);
    // 打怪侧：清锁 + 武装宽限，避免换图后 F5 立刻远跳脱同步。
    simple_combat::ResetForMapChange();
}

}  // namespace

void Init() {
    gWorkerStop.store(false);
    SetState(State::Idle);
    gLastOther.store(-1);
    gLastAdminLike.store(0);
    gLastHideSuspect.store(0);
    gPaused = false;
    gHopGraceUntil = 0;
    gLastHopDeferLog = 0;
    gConfirmSince = 0;
    gNotifyThreatKey = -1;
    gLastGmAlarmAt = 0;
    gGmAlarmActive = false;
    gHideSuspectStreak = 0;
    Log("Init (UserPool + optional GM/hide escalate + forced Alarm, no Reload)");
}

void Shutdown() { StopWorker(); }

void SetEnabled(bool on) {
    gEnabled.store(on);
    if (!on) {
        ReleaseIfDisabled();
        SetState(State::Idle);
        gConfirmSince = 0;
        gNotifyThreatKey = -1;
        gHideSuspectStreak = 0;
        StopGmAlarm();
    }
}

void SetStrategies(bool stopCombat, bool reconnect, bool gmEscalate) {
    gStopCombat.store(stopCombat);
    gReconnect.store(reconnect);
    const bool wasEscalate = gGmEscalate.exchange(gmEscalate);
    if (!reconnect && GetStateLocal() == State::Confirming) SetState(State::Watching);
    if (wasEscalate && !gmEscalate) {
        gHideSuspectStreak = 0;
        gNotifyThreatKey = -1;
        StopGmAlarm();
    }
}

State GetState() { return GetStateLocal(); }

const char* GetStateName() {
    switch (GetStateLocal()) {
    case State::Idle:
        return "Idle";
    case State::Watching:
        return "Watching";
    case State::Confirming:
        return "Confirming";
    case State::Hopping:
        return "Hopping";
    }
    return "?";
}

int LastOtherCount() { return gLastOther.load(); }

int LastAdminLikeCount() { return gLastAdminLike.load(); }

int LastHideSuspectCount() { return gLastHideSuspect.load(); }

bool HoldsCombatPause() {
    return gPaused && gStopCombat.load() && gEnabled.load();
}

void Tick(DWORD now) {
    if (!gEnabled.load()) {
        ReleaseIfDisabled();
        return;
    }
    if (!ports::world::IsPlayReady()) {
        if (GetStateLocal() == State::Hopping) {
            // 换频迁频空窗：保持 Hopping，等回图.
            return;
        }
        gConfirmSince = 0;
        return;
    }
    if (auto_lie::IsBusy()) {
        gConfirmSince = 0;
        return;
    }

    const auto ss = ports::world::GetSceneState();
    if (ss == ports::world::SceneState::CashShop || ss == ports::world::SceneState::Login) {
        gConfirmSince = 0;
        return;
    }

    const int mapId = ports::world::GetMapId();
    if (mapId > 0 && mapId != gLastMapId) OnMapChange(mapId, now);
    if (gLandedAt == 0) gLandedAt = now;

    if (GetStateLocal() == State::Hopping) {
        if (channel_hop::HasPending()) {
            // 换频进行中：威胁未消则续响（不重新采样，避免迁频空窗误清）.
            if (gGmAlarmActive) PulseGmAlarm(now, /*force=*/false);
            return;
        }
        // 与 channel_hop 冷却对齐（成功 15s / 失败 3s）；失败也至少吃满 PostHopGrace
        const DWORD cd = channel_hop::CooldownRemainingMs();
        if (cd > 0) {
            const DWORD until = now + cd;
            if (until > gHopGraceUntil) gHopGraceUntil = until;
        }
        if (now < gHopGraceUntil) {
            if (gGmAlarmActive) PulseGmAlarm(now, /*force=*/false);
            return;
        }
        // 宽限结束：重新采样决定 Watching / Resume.
        SetState(State::Idle);
        gPhaseAt = now;
    }

    if (now - gLastSampleAt < kCheckIntervalMs) return;
    gLastSampleAt = now;

    ports::user_pool::RemoteThreatSample threat{};
    if (!SampleThreat(&threat)) {
        Log("sample fail");
        return;
    }
    const int other = threat.remoteCount;
    bool elevated = false;
    if (gGmEscalate.load()) {
        elevated = IsElevatedThreat(threat);
    } else {
        gHideSuspectStreak = 0;
    }

    if (elevated) {
        const int key = threat.adminLikeCount * 1000 + threat.hideSuspectCount;
        if (key != gNotifyThreatKey) {
            gNotifyThreatKey = key;
            Log("threat admin=%d hideSuspect=%d job=%u other=%d hideCheck=%d",
                threat.adminLikeCount, threat.hideSuspectCount,
                (unsigned)threat.sampleAdminJob, other, player_hide::IsEnabled() ? 0 : 1);
        }
        // GM/隐身：主城也处理；跳过进图宽限与 4s confirm，能 hop 就立刻 hop.
        PauseExposure(other > 0 ? other : 1, true);
        PulseGmAlarm(now, /*force=*/false);  // 威胁持续期间强制续响（无视通知静音）
        if (!gReconnect.load()) {
            SetState(State::Watching);
            gConfirmSince = 0;
            return;
        }
        if (channel_hop::HasPending() || channel_hop::CooldownRemainingMs() > 0 ||
            now < gHopGraceUntil) {
            SetState(State::Watching);
            gConfirmSince = 0;
            return;
        }
        RequestHop(other > 0 ? other : 1, true);
        return;
    }

    // 非升级：仅在曾拉起 GM 告警时停警（避免每秒空转 Cancel/Dismiss）。
    // 隐身 streak 仅在「本拍无 hide 嫌疑」时清，避免 2 拍确认被同拍清零掐死。
    if (gNotifyThreatKey != -1) gNotifyThreatKey = -1;
    if (threat.hideSuspectCount <= 0) gHideSuspectStreak = 0;
    if (gGmAlarmActive || gLastGmAlarmAt != 0) StopGmAlarm();

    if (IsTownMapIdHeuristic(mapId)) {
        // 普通遇人：主城豁免 = 不维持 pause（升级路径已 early return，不会走到这里）。
        // 关 GM 升级 / 威胁消失后若仍钉着硬闸，必须在此松开。
        if (other > 0 && gNotifyOther != other) {
            gNotifyOther = other;
            Log("town skip map=%d other=%d", mapId, other);
        }
        if (other <= 0) gNotifyOther = -1;
        ResumeExposure(/*townSkip=*/true);
        SetState(State::Idle);
        gConfirmSince = 0;
        return;
    }

    const bool inLandGrace = (now - gLandedAt) < kFirstLandGraceMs;

    if (other <= 0) {
        gConfirmSince = 0;
        gNotifyOther = -1;
        ResumeExposure();
        SetState(State::Idle);
        return;
    }

    // other > 0（普通远程）
    PauseExposure(other, false);
    if (!gReconnect.load()) {
        SetState(State::Watching);
        gConfirmSince = 0;
        return;
    }
    if (inLandGrace) {
        SetState(State::Watching);
        gConfirmSince = 0;
        return;
    }

    // channel_hop 忙/冷却中：只 Watching，不启动 confirm（防警戒超时后连烧 seq）
    if (channel_hop::HasPending() || channel_hop::CooldownRemainingMs() > 0) {
        SetState(State::Watching);
        gConfirmSince = 0;
        return;
    }
    if (now < gHopGraceUntil) {
        SetState(State::Watching);
        gConfirmSince = 0;
        return;
    }

    if (gConfirmSince == 0) {
        gConfirmSince = now;
        SetState(State::Confirming);
        Log("confirm start other=%d", other);
        return;
    }
    if (now - gConfirmSince >= kConfirmMs) {
        RequestHop(other, false);
    } else {
        SetState(State::Confirming);
    }
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE t = CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD {
            Log("worker start");
            while (!gWorkerStop.load()) {
                Tick(GetTickCount());
                Sleep(kTickMs);
            }
            Log("worker stop");
            return 0;
        },
        nullptr, 0, nullptr);
    if (!t) {
        Log("StartWorker CreateThread failed");
        return;
    }
    gWorkerThread.store(t);
}

void StopWorker() {
    gWorkerStop.store(true);
    HANDLE t = gWorkerThread.exchange(nullptr);
    if (t) CloseHandle(t);
}

}  // namespace x::features::encounter
