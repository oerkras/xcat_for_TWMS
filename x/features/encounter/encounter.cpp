// Classic TWMS — encounter strategy FSM.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "encounter.h"

#include "../auto_lie/auto_lie.h"
#include "../channel_hop/channel_hop.h"
#include "../notify/notify.h"
#include "../ports/user_pool_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
#include "../../runtime/log.h"

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
constexpr uint32_t kHopSeqBase = 0xE1000000u;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gEnabled{false};
std::atomic<bool> gStopCombat{true};
std::atomic<bool> gReconnect{true};
std::atomic<unsigned> gState{static_cast<unsigned>(State::Idle)};
std::atomic<int> gLastOther{-1};

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

// 图号中间三位 000 → 主城启发式（对照枫星；误判只跳过触发）.
bool IsTownMapIdHeuristic(int mapId) {
    if (mapId <= 0) return false;
    return ((mapId / 1000) % 1000) == 0;
}

void PauseExposure(int other) {
    if (gPaused) {
        // 热保持：有人期间每拍再钉 Encounter 位（位掩码下测谎清不掉本模块位，仍防漏闸）。
        if (gStopCombat.load() && !auto_lie::IsBusy())
            simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, true);
        return;
    }
    gPaused = true;
    if (gStopCombat.load())
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, true);

    char body[96]{};
    snprintf(body, sizeof(body), "同图其他玩家 %d 人，已执行遇人策略。", other);
    Notify(notify::NotificationKind::Warning, "encounter-detected", "遇人策略已触发", body);
    Log("pause other=%d stopCombat=%d", other, (int)gStopCombat.load());
}

void ResumeExposure() {
    if (!gPaused) return;
    if (auto_lie::IsBusy()) {
        Log("resume deferred (auto_lie busy)");
        return;
    }
    if (gStopCombat.load())
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, false);
    gPaused = false;
    Notify(notify::NotificationKind::Info, "encounter-restored", "遇人后已恢复",
           "同图已无其他玩家，挂机策略已恢复。");
    Log("resume");
}

void ReleaseIfDisabled() {
    if (gPaused && !gEnabled.load()) {
        if (!auto_lie::IsBusy())
            simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, false);
        gPaused = false;
    }
}

bool SampleOther(int* outOther) {
    int remote = 0;
    if (!ports::user_pool::SampleRemoteUserCount(&remote)) return false;
    if (remote < 0) remote = 0;
    *outOther = remote;
    gLastOther.store(remote);
    return true;
}

void RequestHop(int other) {
    if (channel_hop::HasPending()) {
        Log("hop skip: channel_hop busy");
        return;
    }
    const DWORD cd = channel_hop::CooldownRemainingMs();
    if (cd > 0) {
        const DWORD now = GetTickCount();
        if (now - gLastHopDeferLog > kHopDeferLogMs) {
            gLastHopDeferLog = now;
            Log("hop defer: channel_hop cooldown %ums other=%d", (unsigned)cd, other);
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
    char body[96]{};
    snprintf(body, sizeof(body), "检测到同图其他玩家 %d 人，已立即换频。", other);
    Notify(notify::NotificationKind::Warning, "encounter-hop", "遇人立即换频", body);
    Log("hop seq=%u other=%d", gHopSeq, other);
    SetState(State::Hopping);
    gPhaseAt = GetTickCount();
    gHopGraceUntil = gPhaseAt + kPostHopGraceMs;
    gConfirmSince = 0;
}

void OnMapChange(int mapId, DWORD now) {
    gLastMapId = mapId;
    gLandedAt = now;
    gConfirmSince = 0;
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
    gPaused = false;
    gHopGraceUntil = 0;
    gLastHopDeferLog = 0;
    gConfirmSince = 0;
    Log("Init (UserPool + channel_hop, no Reload)");
}

void Shutdown() { StopWorker(); }

void SetEnabled(bool on) {
    gEnabled.store(on);
    if (!on) {
        ReleaseIfDisabled();
        SetState(State::Idle);
        gConfirmSince = 0;
    }
}

void SetStrategies(bool stopCombat, bool reconnect) {
    gStopCombat.store(stopCombat);
    gReconnect.store(reconnect);
    if (!reconnect && GetStateLocal() == State::Confirming) SetState(State::Watching);
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
        if (channel_hop::HasPending()) return;
        // 与 channel_hop 冷却对齐（成功 15s / 失败 3s）；失败也至少吃满 PostHopGrace
        const DWORD cd = channel_hop::CooldownRemainingMs();
        if (cd > 0) {
            const DWORD until = now + cd;
            if (until > gHopGraceUntil) gHopGraceUntil = until;
        }
        if (now < gHopGraceUntil) return;
        // 宽限结束：重新采样决定 Watching / Resume.
        SetState(State::Idle);
        gPhaseAt = now;
    }

    if (now - gLastSampleAt < kCheckIntervalMs) return;
    gLastSampleAt = now;

    int other = 0;
    if (!SampleOther(&other)) {
        Log("sample fail");
        return;
    }

    if (IsTownMapIdHeuristic(mapId)) {
        if (other > 0 && gNotifyOther != other) {
            gNotifyOther = other;
            Log("town skip map=%d other=%d", mapId, other);
        }
        if (other <= 0) {
            gNotifyOther = -1;
            ResumeExposure();
            SetState(State::Idle);
            gConfirmSince = 0;
        }
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

    // other > 0
    PauseExposure(other);
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
        RequestHop(other);
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
