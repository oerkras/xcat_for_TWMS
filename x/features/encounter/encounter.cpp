// Classic TWMS — encounter strategy FSM.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "encounter.h"

#include "../auto_lie/auto_lie.h"
#include "../channel_hop/channel_hop.h"
#include "../notify/notify.h"
#include "../player_hide/player_hide.h"
#include "../ports/mob_pool_port.h"
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
constexpr DWORD kConfirmMs = 1000;  // 再提速：2s→1s（采样约 1s，约一拍确认）
constexpr DWORD kFirstLandGraceMs = 5000;
// BIN：再 hop 宽限须 ≥ channel_hop 成功冷却，否则宽限一过就狂 RequestHop.
// 停手恢复不绑这条：Hopping 在 !HasPending 后即可采样 Resume（出刀另受 hop 4s 静默）.
constexpr DWORD kPostHopGraceMs = 0;  // 用户：遇人再 hop 不绑宽限（冷却已关；仅 HasPending 挡并发）
constexpr DWORD kHopDeferLogMs = 4000;
constexpr DWORD kGmAlarmPulseMs = 3000;  // 对齐 auto_lie：威胁期间每 3s 强制 Alarm
constexpr int kHideSuspectConfirmSamples = 2;  // 连续 N 拍才认隐身，压进图/加载假阳
constexpr int kPostHopClearSamples = 2;  // 宽限内连续无人确认，压落地 UserPool 假空
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
int gPostHopClearStreak = 0;  // 宽限内连续 other==0 拍数
char gLastNamesLog[384]{};    // 上次已落盘的 names= 指纹，变化才再记

void SetState(State s) { gState.store(static_cast<unsigned>(s)); }
State GetStateLocal() { return static_cast<State>(gState.load()); }

void Log(const char* fmt, ...) {
    char body[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    x::runtime::LogI("Encounter", "%s", body);
}

void Notify(notify::NotificationKind kind, const char* key, const char* title, const char* body) {
    notify::PublishNotification(notify::NotificationEvent{kind, key, title, body, 5200});
}

// 把 RemoteThreatSample.names 拼成逗号列表；无人或全失败 → "?"。
// ,+N 仅表示「超过 names[] 容量未列出」，不把读名失败算进 +N。
void FormatRemoteNames(const ports::user_pool::RemoteThreatSample& t, char* out, int outSz) {
    if (!out || outSz <= 0) return;
    out[0] = 0;
    if (t.nameCount <= 0) {
        if (t.remoteCount > 0) snprintf(out, outSz, "?");
        return;
    }
    int used = 0;
    for (int i = 0; i < t.nameCount; ++i) {
        const char* nm = t.names[i];
        if (!nm || !nm[0]) continue;
        const int need = static_cast<int>(std::strlen(nm)) + (used > 0 ? 1 : 0);
        if (used + need + 1 >= outSz) {
            if (used + 4 < outSz) {
                out[used++] = ',';
                out[used++] = '.';
                out[used++] = '.';
                out[used++] = '.';
                out[used] = 0;
            }
            break;
        }
        if (used > 0) out[used++] = ',';
        for (const char* p = nm; *p && used + 1 < outSz; ++p) out[used++] = *p;
        out[used] = 0;
    }
    if (!out[0] && t.remoteCount > 0) {
        snprintf(out, outSz, "?");
        return;
    }
    // 仅容量截断：names 槽已满且远程人数更多
    if (t.nameCount >= ports::user_pool::kRemoteNameMax &&
        t.remoteCount > t.nameCount && used + 8 < outSz) {
        snprintf(out + used, outSz - used, ",+%d", t.remoteCount - t.nameCount);
    }
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

// 无刷怪图 ≈ 标题栏「怪 N/M」均为 0（LifeList M=0 且活怪 n=0）。
// 废都狩猎图图号也常带 000 段，旧图号启发式会误豁免；改跟 mob 缓存（与 titlebar 同源）。
// · 缓存图号≠当前图 → **现读** LifeList（勿直接 false：BIN 10:55 回城后旧缓存导致主城误 Pause+落台）
// · M 未知 / 缓存无 mapId → 现读 CountMapMobLifeSlots（禁用陈旧 M=0）
// · 现读成功且 M==0 → 豁免（不依赖可能属旧图的 n）
// · outUnknown：换图窗现读也失败 → 调用方进图宽限内勿普通 Pause（GM 升级路径不走这里）
bool IsTownLikeNoMobMap(int mapId, ports::mob::Snapshot* outSnap = nullptr,
                        bool* outUnknown = nullptr) {
    if (outUnknown) *outUnknown = false;

    auto applyLive = [&](int liveM) {
        if (outSnap) {
            outSnap->spawnSlots = liveM;
            outSnap->lifeMob = liveM;
            if (mapId > 0) outSnap->mapId = mapId;
        }
    };

    ports::mob::Snapshot snap{};
    const bool haveCache = ports::mob::GetCached(snap);
    if (haveCache && outSnap) *outSnap = snap;

    if (!haveCache) {
        const int liveM = ports::mob::CountMapMobLifeSlots();
        if (liveM < 0) {
            if (outUnknown) *outUnknown = true;
            return false;
        }
        applyLive(liveM);
        return liveM == 0;
    }

    const int n = snap.count;
    int m = snap.spawnSlots;
    const int cachedMap = snap.mapId;
    const bool cacheStale = cachedMap > 0 && mapId > 0 && cachedMap != mapId;

    // 换图瞬间旧 n/M 不可信：只信当前图 LifeList。
    const bool needLive =
        cacheStale || (m < 0) || (cachedMap == 0 && mapId > 0);
    if (needLive) {
        const int liveM = ports::mob::CountMapMobLifeSlots();
        if (liveM < 0) {
            if (outUnknown) *outUnknown = true;
            return false;
        }
        applyLive(liveM);
        // 现读 M：只信 LifeList；狩猎图 M>0 不会误豁免
        return liveM == 0;
    }

    return n <= 0 && m == 0;
}

void PauseExposure(const ports::user_pool::RemoteThreatSample& t, bool threat) {
    const int other = t.remoteCount > 0 ? t.remoteCount : 1;
    char names[384]{};
    FormatRemoteNames(t, names, sizeof(names));
    auto publishPauseNotify = [&](bool forceAlarm) {
        char body[256]{};
        if (threat) {
            snprintf(body, sizeof(body), "同图疑似 GM/隐身（远程 %d：%s），已立即停手。", other,
                     names[0] ? names : "?");
            Notify(notify::NotificationKind::Danger, "encounter-gm-threat", "疑似 GM/隐身", body);
            if (forceAlarm) PulseGmAlarm(GetTickCount(), /*force=*/true);
        } else {
            snprintf(body, sizeof(body), "同图其他玩家 %d 人（%s），已执行遇人策略。", other,
                     names[0] ? names : "?");
            Notify(notify::NotificationKind::Warning, "encounter-detected", "遇人策略已触发",
                   body);
        }
    };

    if (gPaused) {
        // 热保持：有人期间每拍再钉 Encounter 位（位掩码下测谎清不掉本模块位，仍防漏闸）。
        if (gStopCombat.load() && !auto_lie::IsBusy())
            simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, true);
        // 名单变化（新人进图）：日志 + 刷新通知，避免面板还挂着旧名。
        if (names[0] && std::strcmp(names, gLastNamesLog) != 0) {
            std::snprintf(gLastNamesLog, sizeof(gLastNamesLog), "%s", names);
            Log("still other=%d names=[%s] threat=%d", other, names, threat ? 1 : 0);
            publishPauseNotify(/*forceAlarm=*/false);
        }
        return;
    }
    gPaused = true;
    if (gStopCombat.load())
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::Encounter, true);

    std::snprintf(gLastNamesLog, sizeof(gLastNamesLog), "%s", names);
    publishPauseNotify(/*forceAlarm=*/true);
    Log("pause other=%d names=[%s] threat=%d stopCombat=%d", other, names[0] ? names : "?",
        threat ? 1 : 0, (int)gStopCombat.load());
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
    gLastNamesLog[0] = 0;
    StopGmAlarm();
    if (townSkip) {
        Notify(notify::NotificationKind::Info, "encounter-town-skip", "无刷怪图已跳过遇人",
               "当前图不刷怪（怪 0/0），遇人停手已解除。");
        Log("resume town-skip (no-mob)");
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
        gLastNamesLog[0] = 0;
        StopGmAlarm();
        gHideSuspectStreak = 0;
        gPostHopClearStreak = 0;
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

void RequestHop(const ports::user_pool::RemoteThreatSample& t, bool threat) {
    const int other = t.remoteCount > 0 ? t.remoteCount : 1;
    char names[384]{};
    FormatRemoteNames(t, names, sizeof(names));
    if (channel_hop::HasPending()) {
        Log("hop skip: channel_hop busy");
        return;
    }
    const DWORD cd = channel_hop::CooldownRemainingMs();
    if (cd > 0) {
        const DWORD now = GetTickCount();
        if (now - gLastHopDeferLog > kHopDeferLogMs) {
            gLastHopDeferLog = now;
            Log("hop defer: channel_hop cooldown %ums other=%d names=[%s] threat=%d",
                (unsigned)cd, other, names[0] ? names : "?", threat ? 1 : 0);
        }
        // 不烧 seq：回 Watching，等冷却后再重新 confirm
        SetState(State::Watching);
        gConfirmSince = 0;
        if (now + cd > gHopGraceUntil) gHopGraceUntil = now + cd;
        return;
    }
    ++gHopSeq;
    if (gHopSeq < kHopSeqBase) gHopSeq = kHopSeqBase + 1;
    // 离开当前挤频前软拉黑，下一次选池优先避开（本图 TTL）
    channel_hop::NoteCrowdedChannel();
    channel_hop::RequestManualRejoin(gHopSeq);
    char body[256]{};
    if (threat) {
        snprintf(body, sizeof(body), "疑似 GM/隐身（远程 %d：%s），已立即换频。", other,
                 names[0] ? names : "?");
        Notify(notify::NotificationKind::Danger, "encounter-gm-hop", "疑似 GM 立即换频", body);
        // Alarm 已由 PauseExposure / Tick Pulse 负责，此处不再 force，避免同拍双响。
    } else {
        snprintf(body, sizeof(body), "检测到同图其他玩家 %d 人（%s），已立即换频。", other,
                 names[0] ? names : "?");
        Notify(notify::NotificationKind::Warning, "encounter-hop", "遇人立即换频", body);
    }
    Log("hop seq=%u other=%d names=[%s] threat=%d", gHopSeq, other, names[0] ? names : "?",
        threat ? 1 : 0);
    SetState(State::Hopping);
    gPhaseAt = GetTickCount();
    gHopGraceUntil = gPhaseAt + kPostHopGraceMs;
    gConfirmSince = 0;
    gPostHopClearStreak = 0;
}

void OnMapChange(int mapId, DWORD now) {
    gLastMapId = mapId;
    gLandedAt = now;
    gConfirmSince = 0;
    gNotifyThreatKey = -1;
    gHideSuspectStreak = 0;
    if (GetStateLocal() == State::Confirming) SetState(State::Watching);
    Log("map change id=%d grace=%ums", mapId, (unsigned)kFirstLandGraceMs);
    channel_hop::OnMapChanged(mapId);
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
    gPostHopClearStreak = 0;
    gLastNamesLog[0] = 0;
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
        gPostHopClearStreak = 0;
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
            // 迁频空窗：不采样（避免离图误清）；威胁续响.
            if (gGmAlarmActive) PulseGmAlarm(now, /*force=*/false);
            return;
        }
        // 与 channel_hop 冷却对齐（成功/失败均已关冷却）；宽限 kPostHopGraceMs=0
        // 宽限只挡「再 RequestHop」，不挡采样/Resume：落地后无人即可恢复打怪
        // （真正出刀还受 channel_hop 结算后 4s 静默约束）.
        const DWORD cd = channel_hop::CooldownRemainingMs();
        if (cd > 0) {
            const DWORD until = now + cd;
            if (until > gHopGraceUntil) gHopGraceUntil = until;
        }
        if (gGmAlarmActive) PulseGmAlarm(now, /*force=*/false);
        if (now >= gHopGraceUntil) {
            SetState(State::Idle);
            gPhaseAt = now;
        }
        // 宽限未满：保持 Hopping，落下去采样（可 ResumeExposure；再 hop 仍被下方门控拦住）.
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
        char names[384]{};
        FormatRemoteNames(threat, names, sizeof(names));
        if (key != gNotifyThreatKey) {
            gNotifyThreatKey = key;
            Log("threat admin=%d hideSuspect=%d job=%u other=%d names=[%s] hideCheck=%d",
                threat.adminLikeCount, threat.hideSuspectCount,
                (unsigned)threat.sampleAdminJob, other, names[0] ? names : "?",
                player_hide::IsEnabled() ? 0 : 1);
        }
        // GM/隐身：主城也处理；跳过进图宽限与 4s confirm，能 hop 就立刻 hop.
        gPostHopClearStreak = 0;
        PauseExposure(threat, true);
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
        RequestHop(threat, true);
        return;
    }

    // 非升级：仅在曾拉起 GM 告警时停警（避免每秒空转 Cancel/Dismiss）。
    // 隐身 streak 仅在「本拍无 hide 嫌疑」时清，避免 2 拍确认被同拍清零掐死。
    if (gNotifyThreatKey != -1) gNotifyThreatKey = -1;
    if (threat.hideSuspectCount <= 0) gHideSuspectStreak = 0;
    if (gGmAlarmActive || gLastGmAlarmAt != 0) StopGmAlarm();

    ports::mob::Snapshot mobSnap{};
    bool townUnknown = false;
    if (IsTownLikeNoMobMap(mapId, &mobSnap, &townUnknown)) {
        // 普通遇人：无刷怪图豁免 = 不维持 pause（升级路径已 early return，不会走到这里）。
        // 关 GM 升级 / 威胁消失后若仍钉着硬闸，必须在此松开。
        if (other > 0 && gNotifyOther != other) {
            gNotifyOther = other;
            char names[384]{};
            FormatRemoteNames(threat, names, sizeof(names));
            Log("town skip map=%d other=%d names=[%s] n=%d m=%d lifeMob=%d cachedMap=%d (no-mob)",
                mapId, other, names[0] ? names : "?", mobSnap.count, mobSnap.spawnSlots,
                mobSnap.lifeMob, mobSnap.mapId);
        }
        if (other <= 0) gNotifyOther = -1;
        ResumeExposure(/*townSkip=*/true);
        SetState(State::Idle);
        gConfirmSince = 0;
        return;
    }

    const bool inLandGrace = (now - gLandedAt) < kFirstLandGraceMs;

    // 换图后缓存未对齐且 LifeList 暂不可读：宽限内普通遇人先别 Pause/落台。
    // GM 升级已在上方 early return，不受影响；宽限后仍 unknown 则走原逻辑（偏狩猎图不停手漏）。
    if (townUnknown && inLandGrace) {
        static DWORD sTownUnkLog = 0;
        if (!sTownUnkLog || now - sTownUnkLog >= 2000) {
            sTownUnkLog = now;
            Log("town-like unknown map=%d other=%d cachedMap=%d (defer pause in land grace)",
                mapId, other, mobSnap.mapId);
        }
        SetState(State::Watching);
        gConfirmSince = 0;
        return;
    }

    if (other <= 0) {
        gConfirmSince = 0;
        gNotifyOther = -1;
        // 换频宽限内且仍停手：连续无人确认后再 Resume，避免落地 UserPool 短暂空表误恢复。
        // 已 Resume 则不再走 streak（避免 grace 内刷 1/2 日志）。
        if (now < gHopGraceUntil && gPaused) {
            ++gPostHopClearStreak;
            if (gPostHopClearStreak < kPostHopClearSamples) {
                Log("post-hop clear confirm %d/%d (hold pause)", gPostHopClearStreak,
                    kPostHopClearSamples);
                return;
            }
        }
        gPostHopClearStreak = 0;
        ResumeExposure();
        SetState(State::Idle);
        return;
    }

    // other > 0（普通远程）
    gPostHopClearStreak = 0;
    PauseExposure(threat, false);
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
        char names[384]{};
        FormatRemoteNames(threat, names, sizeof(names));
        Log("confirm start other=%d names=[%s]", other, names[0] ? names : "?");
        return;
    }
    if (now - gConfirmSince >= kConfirmMs) {
        RequestHop(threat, false);
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
