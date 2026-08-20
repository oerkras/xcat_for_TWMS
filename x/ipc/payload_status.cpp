// TWMS PayloadStatus 发布：单一写者，合并 CCU + 顶栏灯（对照枫星 payload_status，精简版）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "payload_status.h"

#include "../features/auto_supply/auto_supply.h"
#include "../features/ccu/ccu.h"
#include "../features/channel_hop/channel_hop.h"
#include "../features/frame_lock/frame_lock.h"
#include "../features/kick_sniff/kick_sniff.h"
#include "../features/soft_login_probe/soft_login_probe.h"
#include "../features/ports/mob_gather_port.h"
#include "../features/ports/security_attack_port.h"
#include "../features/ports/travel_port.h"
#include "../features/ports/world_port.h"
#include "../features/sellbag/sellbag.h"
#include "../features/titlebar/titlebar_game.h"
#include "../runtime/bin_dir.h"
#include "../runtime/il2cpp_bind.h"
#include "../runtime/il2cpp_prefab.h"
#include "../runtime/log.h"
#include "../runtime/managed_main.h"
#include "../runtime/anchor_lamps.h"

#include "../../common/xcat_map_names.h"
#include "../../common/xcat_payload_status.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>

namespace x::ipc {
namespace {

constexpr DWORD kPublishIntervalMs = 500;

// docs/features/auto_lie/P0a — Prefab；类哈希 remount 2026-08-06（与 anti_macro_port 对齐）
constexpr char kAntiMacroUtilClass[] =
    "a1ba7c522e1ab4ba635ab49512129c0b35ef7edbb6e2adbf5ea9cdf790f4f03";
constexpr char kAntiMacroNonFiniteClass[] =
    "f97cd5fa9bd9845d8bef6a8a90ca5674c316f443c11982181d1b8d9ebdb668c";
constexpr char kAntiMacroTextCaptchaClass[] =
    "ade4b2d0f8a10a31ab5f037bec1bd8e4787b39a9c0c43ffc4d9ddc533798be7";
constexpr char kPrefabNonFinite[] = "UIAntiMacroNonFinite";
constexpr char kPrefabTextCaptcha[] = "UIAntiMacroTextCaptcha";

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};
std::atomic<bool> gQuizTypesOk{false};
bool gQuizTypesLogged = false;

bool GameAssemblyPresent() {
    return GetModuleHandleW(L"GameAssembly.dll") != nullptr;
}

// Cache 灯：Util 哈希 + Text/NonFinite（哈希优先、Prefab 兜底）就绪即 latch。
// 证据 5e3768：login-freeze 期间 worker 上 FindClass → 与 fe04a3 同类 GC 风险。
bool EnsureQuizTypesResolved() {
    if (gQuizTypesOk.load(std::memory_order_relaxed)) return true;
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    if (!x::runtime::il2cpp::Ensure()) return false;

    void* util = x::runtime::il2cpp::FindClass("", kAntiMacroUtilClass);
    void* nonFinite =
        x::runtime::il2cpp_prefab::FindClassCached(kAntiMacroNonFiniteClass, kPrefabNonFinite)
            .klass;
    void* textCaptcha =
        x::runtime::il2cpp_prefab::FindClassCached(kAntiMacroTextCaptchaClass, kPrefabTextCaptcha)
            .klass;
    if (!util || !nonFinite || !textCaptcha) return false;

    gQuizTypesOk.store(true, std::memory_order_relaxed);
    if (!gQuizTypesLogged) {
        gQuizTypesLogged = true;
        x::runtime::LogI("PayloadStatus",
                         "quiz TypeResolve ok (Util+NonFinite+TextCaptcha klass)");
    }
    return true;
}

void FillLeds(xcat::PayloadStatus& st) {
    st.ipcHandshake = 1u;

    const bool ga = GameAssemblyPresent();
    const bool wmAlive =
        x::features::ports::world::EnsureBound() && x::features::ports::world::IsAlive();
    st.wmAlive = wmAlive ? 1u : 0u;
    st.gameContextOk = (ga && wmAlive) ? 1u : 0u;
    st.sceneState = static_cast<int32_t>(x::features::ports::world::GetSceneState());

    st.localPlayerOk = x::features::titlebar::game::LocalUserLooksOk() ? 1u : 0u;

    const bool play = x::features::ports::world::IsPlayReady();
    st.playReady = play ? 1u : 0u;

    int mapId = -1;
    if (play && x::features::ports::world::HasMapData()) {
        mapId = x::features::ports::travel::CurrentMapId();
    }
    st.mapId = mapId >= 0 ? static_cast<uint32_t>(mapId) : 0u;
    st.currentMapName[0] = '\0';
    st.channelId = 0;
    if (play) {
        const xcat::MapNamesPack& names = xcat::GetSharedMapNames(x::runtime::GetBinDir());
        std::string label = mapId >= 0 ? xcat::MapNamesLabelById(names, mapId) : std::string{};
        if (label.empty()) label = "地圖";
        xcat::CopyUtf8Truncate(st.currentMapName, sizeof(st.currentMapName), label.c_str());
        // 给人看的 ch.N（列表 id + 1）；未知保持 0，上报侧不得把 0 写成频道。
        const int ch1 = x::features::channel_hop::DisplayChannel1Based();
        if (ch1 > 0) st.channelId = ch1;
    }

    st.quizCacheRootOk = EnsureQuizTypesResolved() ? 1u : 0u;

    st.playerExp = 0;
    st.playerExpValid = 0;
    st.playerCharValid = 0;
    st.playerLevel = 0;
    st.playerJob = 0;
    st.playerMeso = 0;
    st.playerName[0] = '\0';
    st.playerJobName[0] = '\0';
    st.playerWealthScrollsValid = 0;
    st.playerWealthScrolls[0] = '\0';
    if (st.localPlayerOk) {
        x::features::titlebar::game::Vitals vitals{};
        if (x::features::titlebar::game::ReadVitals(vitals) && vitals.ok) {
            st.playerExp = static_cast<uint64_t>(static_cast<uint32_t>(vitals.exp));
            st.playerExpValid = 1u;
            if (vitals.name[0] && vitals.level > 0) {
                st.playerCharValid = 1u;
                st.playerLevel = vitals.level;
                st.playerJob = vitals.job;
                st.playerMeso = vitals.meso;
                strncpy_s(st.playerName, vitals.name, _TRUNCATE);
                char jobFallback[16]{};
                const char* jobTw =
                    x::features::titlebar::game::JobText(vitals.job, jobFallback);
                if (jobTw) strncpy_s(st.playerJobName, jobTw, _TRUNCATE);
                // 仅 playReady：换图空窗勿把空背包当成「卷轴清零」。纯内存读，无 UI。
                if (st.playReady &&
                    x::features::titlebar::game::FormatWealthScrolls(
                        st.playerWealthScrolls, sizeof(st.playerWealthScrolls))) {
                    st.playerWealthScrollsValid = 1u;
                }
            }
        }
    }
}

}  // namespace

void PayloadStatus_Publish() {
    const char* binDir = x::runtime::GetBinDir();
    if (!binDir || !binDir[0]) return;

    xcat::PayloadStatus st{};
    xcat::PayloadStatusSetDefaults(st);

    const auto ccu = x::features::ccu::GetCcuStatus();
    st.worldChannelOnline = ccu.worldChannelOnline;
    st.worldChannelCount = ccu.worldChannelCount;
    st.worldChannelAgeSec = ccu.worldChannelAgeSec;

    FillLeds(st);

    x::features::sellbag::Status sellbag{};
    x::features::sellbag::GetStatus(sellbag);
    st.sellbagBusy = sellbag.busy ? 1u : 0u;
    st.sellbagState = sellbag.state;
    st.sellbagLastBagMask = sellbag.lastBagMask;
    st.sellbagEquipSold = sellbag.equipSold;
    st.sellbagEtcSold = sellbag.etcSold;
    st.sellbagKept = sellbag.kept;
    st.sellbagFailed = sellbag.failed;
    st.sellbagLastRunTickMs = sellbag.lastRunTickMs;
    strncpy_s(st.sellbagMessage, sellbag.message, _TRUNCATE);

    st.disconnectSeq = x::features::kick_sniff::DisconnectSeq();
    st.sessionState = x::features::kick_sniff::LastSessionState();
    st.pendingErrorCode = x::features::kick_sniff::LastPendingErrorCode();
    st.sawDisconnect = x::features::kick_sniff::SawDisconnect() ? 1u : 0u;

    st.frameLockOn = x::features::frame_lock::IsEnabled() ? 1u : 0u;
    st.frameLockWant = x::features::frame_lock::TargetFps();
    st.frameLockReadback = x::features::frame_lock::LastAppliedFps();

    st.softLoginHold = x::features::soft_login_probe::IsHoldActive() ? 1u : 0u;
    st.softLoginResult = x::features::soft_login_probe::ResultCode();

    {
        unsigned on = 0, paused = 0, remainMs = 0, needMs = 0;
        x::features::ports::mob_gather::QuerySoftReloginClock(&on, &paused, &remainMs, &needMs);
        st.softReloginOn = on;
        st.softReloginPaused = paused;
        st.softReloginRemainMs = remainMs;
        st.softReloginNeedMs = needMs;
        unsigned fires = 0, firesNeed = 0;
        x::features::ports::mob_gather::QueryHangupFires(&fires, &firesNeed);
        st.hangupFires = fires;
        st.hangupFiresNeed = firesNeed;
    }

    {
        x::features::ports::security_attack::WindowSnapshot w{};
        const bool ok = x::features::ports::security_attack::PeekWindow(&w);
        st.secAttackOk = ok ? 1u : 0u;
        st.secAttackInterceptOn = 0;
        st.secAttackTextHookOn = 0;
        st.secAttackPeak = w.peakKey;
        st.secAttackPktSum = w.pktSum;
        st.secAttackSkillSum = w.skillSum;
        st.secAttackPct = w.pctOfCheck;
        st.secAttackWipePeak = 0;
        st.secAttackWindowPeak = w.windowPeak;
        st.secAttackWindowPktSum = w.windowPktSum;
        st.secAttackWindowSkillSum = w.windowSkillSum;
        st.secAttackPktPeak = w.pktPeak;
        st.secAttackSkillPeak = w.skillPeak;
        st.secAttackPktPeakId = w.pktPeakId;
        st.secAttackSkillPeakId = w.skillPeakId;
        st.secAttackDetectTime = w.detectTime;
    }

    // 与 auto_supply 同口径：仅 map_info.town=1（含药店室内；勿用 %1000000 / 原生 IsTown）。
    if (st.mapId != 0u) {
        st.mapIsTownValid = 1u;
        st.mapIsTown =
            x::features::auto_supply::IsTownMapIdHeuristic(static_cast<int>(st.mapId)) ? 1u : 0u;
    } else {
        st.mapIsTownValid = 0u;
        st.mapIsTown = 0u;
    }

    st.writeTickMs = GetTickCount64();
    (void)xcat::WritePayloadStatus(binDir, st);
    x::runtime::anchor_lamps::Publish(binDir);
}

namespace {

DWORD WINAPI WorkerProc(LPVOID) {
    x::runtime::LogI("PayloadStatus", "publisher start");
    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gStop.load(); ++i)
        Sleep(50);
    while (!gStop.load()) {
        PayloadStatus_Publish();
        Sleep(kPublishIntervalMs);
    }
    x::runtime::LogI("PayloadStatus", "publisher stop");
    return 0;
}

}  // namespace

void PayloadStatus_Start() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE t = CreateThread(nullptr, 0, &WorkerProc, nullptr, 0, nullptr);
    if (!t) {
        x::runtime::LogW("PayloadStatus", "create thread failed err=%lu", GetLastError());
        return;
    }
    gThread.store(t);
}

void PayloadStatus_Stop() {
    gStop.store(true);
}

}  // namespace x::ipc
