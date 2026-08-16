// TWMS Classic — titlebar orchestration; game reads and Win32 writes live in siblings.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "titlebar.h"
#include "titlebar_game.h"
#include "titlebar_win.h"

#include "../ports/mob_pool_port.h"
#include "../ports/world_port.h"
#include "../channel_hop/channel_hop.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "User32.lib")

namespace x::features::titlebar {
namespace {

constexpr DWORD kUpdateIntervalMs = 250;
constexpr DWORD kTitleWriteMinMs = 300;
constexpr DWORD kRebindMissMs = 3000;
constexpr DWORD kRebindOkMs = 30000;
constexpr DWORD kIdleSleepMs = 50;
constexpr int kRingCap = 64;
constexpr DWORD kRateMinDtMs = 3000;
constexpr DWORD kResolveMissLogMs = 30000;
constexpr uint32_t kResolveMissLogSlot = 910;

std::atomic<bool> gEnabled{true};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
HWND gHwnd = nullptr;
bool gSavedTitle = false;
std::wstring gOrigTitle;
DWORD gLastUpdate = 0;
DWORD gLastRebind = 0;
bool gHaveValidVitals = false;
DWORD gLastTitleWriteTick = 0;
std::string gPendingTitle;
bool gHavePendingTitle = false;
std::string gLastWrittenTitle;

struct Sample {
    DWORD tick = 0;
    double expCum = 0.0;
    double meso = 0.0;
    double lootCum = 0.0;
    double mpSpentCum = 0.0;
    double mpPotCum = 0.0;
};

Sample gRing[kRingCap]{};
int gRingCount = 0;
int gRingHead = 0;
double gExpCum = 0.0;
double gLootValueCum = 0.0;
double gMpSpentCum = 0.0;
double gMpPotCum = 0.0;
double gLastExp = 0.0;
double gLastMaxExp = 0.0;
int gLastMp = 0;
int gLastMmp = 0;
bool gHaveLast = false;
bool gHaveLastMp = false;
bool gRateActive = false;
DWORD gRateStartTick = 0;
bool gHaveCachedRate = false;
double gCachedExpPer = 0.0;
double gCachedMesoPer = 0.0;
double gCachedLootPer = 0.0;
double gCachedMpPer = 0.0;
double gCachedMpPotPer = 0.0;
uint64_t gLootKnownCount = 0;
uint64_t gLootUnknownCount = 0;

std::string FormatNum(long long value) {
    char buffer[32]{};
    snprintf(buffer, sizeof(buffer), "%lld", value);
    return buffer;
}

std::string FormatCompactAbs(double value) {
    const double absolute = std::fabs(value);
    if (absolute < 10000.0) return FormatNum(static_cast<long long>(absolute + 0.5));
    const char* unit = "万";
    double scaled = absolute / 10000.0;
    if (absolute >= 100000000.0) {
        unit = "亿";
        scaled = absolute / 100000000.0;
    }
    char buffer[48]{};
    if (scaled >= 100.0) snprintf(buffer, sizeof(buffer), "%.0f%s", scaled, unit);
    else if (scaled >= 10.0) snprintf(buffer, sizeof(buffer), "%.1f%s", scaled, unit);
    else snprintf(buffer, sizeof(buffer), "%.2f%s", scaled, unit);
    return buffer;
}

std::string FormatSignedRate(double perMinute) {
    return std::string(perMinute < 0.0 ? "-" : "+") + FormatCompactAbs(perMinute);
}

void ResetRates() {
    gRingCount = 0;
    gRingHead = 0;
    gExpCum = 0.0;
    gLootValueCum = 0.0;
    gMpSpentCum = 0.0;
    gMpPotCum = 0.0;
    gLastExp = 0.0;
    gLastMaxExp = 0.0;
    gLastMp = 0;
    gLastMmp = 0;
    gHaveLast = false;
    gHaveLastMp = false;
    gRateActive = false;
    gRateStartTick = 0;
    gHaveCachedRate = false;
    gCachedExpPer = 0.0;
    gCachedMesoPer = 0.0;
    gCachedLootPer = 0.0;
    gCachedMpPer = 0.0;
    gCachedMpPotPer = 0.0;
    gLootKnownCount = 0;
    gLootUnknownCount = 0;
    game::ResetLootBaseline();
}

void PushSample(DWORD now, double meso) {
    gRing[gRingHead] = Sample{now, gExpCum, meso, gLootValueCum, gMpSpentCum, gMpPotCum};
    gRingHead = (gRingHead + 1) % kRingCap;
    if (gRingCount < kRingCap) ++gRingCount;
}

bool ComputeRates(DWORD now, double& expPer, double& mesoPer, double& lootPer, double& mpPer,
                  double& mpPotPer) {
    if (gRingCount < 2) return false;
    const Sample& oldest = gRing[(gRingHead - gRingCount + kRingCap) % kRingCap];
    const DWORD elapsed = now - oldest.tick;
    if (elapsed < kRateMinDtMs) return false;
    const Sample& newest = gRing[(gRingHead + kRingCap - 1) % kRingCap];
    const double elapsedMs = static_cast<double>(elapsed);
    expPer = (gExpCum - oldest.expCum) / elapsedMs * 60000.0;
    mesoPer = (newest.meso - oldest.meso) / elapsedMs * 60000.0;
    lootPer = (gLootValueCum - oldest.lootCum) / elapsedMs * 60000.0;
    mpPer = (gMpSpentCum - oldest.mpSpentCum) / elapsedMs * 60000.0;
    mpPotPer = (gMpPotCum - oldest.mpPotCum) / elapsedMs * 60000.0;
    if (expPer < 0.0) expPer = 0.0;
    if (mesoPer < 0.0) mesoPer = 0.0;
    if (lootPer < 0.0) lootPer = 0.0;
    if (mpPer < 0.0) mpPer = 0.0;
    if (mpPotPer < 0.0) mpPotPer = 0.0;
    return true;
}

void NoteMpSpend(const game::Vitals& vitals) {
    if (!gHaveLastMp) {
        gLastMp = vitals.mp;
        gLastMmp = vitals.mmp;
        gHaveLastMp = true;
        return;
    }
    // 死亡/换装导致 MMP 跳变：只对齐，不当消耗。
    if (vitals.hp <= 0 || (vitals.mmp != gLastMmp && gLastMmp > 0)) {
        gLastMp = vitals.mp;
        gLastMmp = vitals.mmp;
        return;
    }
    if (vitals.mp < gLastMp) {
        gMpSpentCum += static_cast<double>(gLastMp - vitals.mp);
    }
    gLastMp = vitals.mp;
    gLastMmp = vitals.mmp;
}

void UpdateRates(DWORD now, const game::Vitals& vitals) {
    if (!vitals.ok || vitals.maxExp <= 0) {
        if (gRateActive) ResetRates();
        return;
    }
    const double exp = static_cast<double>(vitals.exp);
    const double maxExp = static_cast<double>(vitals.maxExp);
    const double meso = static_cast<double>(vitals.meso);
    if (!gRateActive) {
        gRateActive = true;
        gRateStartTick = now;
        gLastExp = exp;
        gLastMaxExp = maxExp;
        gHaveLast = true;
        NoteMpSpend(vitals);
        game::UpdateLootDelta(false);
        PushSample(now, meso);
        return;
    }
    if (gHaveLast) {
        if (maxExp != gLastMaxExp && maxExp > 0.0 && gLastMaxExp > 0.0) {
            gExpCum += exp < gLastExp ? gLastMaxExp - gLastExp + exp : exp - gLastExp;
        } else if (exp >= gLastExp) {
            gExpCum += exp - gLastExp;
        }
    }
    gLastExp = exp;
    gLastMaxExp = maxExp;
    NoteMpSpend(vitals);
    uint64_t known = 0, unknown = 0, mpPots = 0;
    const double lootDelta = game::UpdateLootDelta(true, &known, &unknown, &mpPots);
    gLootValueCum += lootDelta;
    gMpPotCum += static_cast<double>(mpPots);
    gLootKnownCount += known;
    gLootUnknownCount += unknown;
    if (known || unknown) {
        x::runtime::LogI("Titlebar", "loot delta value=%.0f known+=%llu unknown+=%llu total=%.0f",
                         lootDelta, static_cast<unsigned long long>(known),
                         static_cast<unsigned long long>(unknown), gLootValueCum);
    }
    if (mpPots) {
        x::runtime::LogI("Titlebar", "mp-pot consumed+=%llu total=%.0f",
                         static_cast<unsigned long long>(mpPots), gMpPotCum);
    }
    PushSample(now, meso);
    double expPer = 0.0, mesoPer = 0.0, lootPer = 0.0, mpPer = 0.0, mpPotPer = 0.0;
    if (ComputeRates(now, expPer, mesoPer, lootPer, mpPer, mpPotPer)) {
        gCachedExpPer = expPer;
        gCachedMesoPer = mesoPer;
        gCachedLootPer = lootPer;
        gCachedMpPer = mpPer;
        gCachedMpPotPer = mpPotPer;
        gHaveCachedRate = true;
    }
}

std::string BuildMobSegment() {
    const int alive = ports::mob::GetCachedAliveCount();
    if (alive < 0) return {};
    const int maxSlots = ports::mob::GetCachedSpawnSlots();
    char buffer[40]{};
    if (maxSlots > 0 && maxSlots >= alive) snprintf(buffer, sizeof(buffer), "    怪 %d/%d", alive, maxSlots);
    else snprintf(buffer, sizeof(buffer), "    怪 %d", alive);
    return buffer;
}

std::string BuildChannelSegment() {
    // Display = 列表 id + 1（sticky 优先）。未种上则省略。禁止把 0 画成频道。
    const int ch = x::features::channel_hop::DisplayChannel1Based();
    if (ch <= 0) return {};
    char buffer[24]{};
    snprintf(buffer, sizeof(buffer), "    頻道 %d", ch);
    return buffer;
}

std::string BuildTitle(DWORD now, const game::Vitals& vitals) {
    char jobFallback[16]{};
    std::string who = vitals.name[0] ? vitals.name : "?";
    who += " 职业:";
    who += game::JobText(vitals.job, jobFallback);
    std::string rate;
    if (gHaveCachedRate) {
        rate = "    " + FormatSignedRate(gCachedMesoPer) + " 金/分    " +
               FormatSignedRate(gCachedExpPer) + " 经/分    " +
               FormatSignedRate(gCachedLootPer) + " 物值/分    " +
               FormatCompactAbs(gCachedMpPer) + " MP/分    " +
               FormatCompactAbs(gCachedMpPotPer) + " 藍瓶/分";
    } else if (gRateActive) {
        const DWORD elapsed = now - gRateStartTick;
        if (elapsed < kRateMinDtMs) {
            char wait[48]{};
            snprintf(wait, sizeof(wait), "    收益採樣 %u/3s",
                     static_cast<unsigned>((elapsed + 999) / 1000));
            rate = wait;
        } else {
            rate = "    收益等待資料";
        }
    }
    char title[768]{};
    const int written = snprintf(
        title, sizeof(title),
        "Lv.%d %s    HP %d/%d    MP %d/%d    EXP %d/%d    背包金 %s%s%s%s",
        vitals.level, who.c_str(), vitals.hp, vitals.mhp, vitals.mp, vitals.mmp, vitals.exp,
        vitals.maxExp, FormatCompactAbs(static_cast<double>(vitals.meso)).c_str(),
        BuildChannelSegment().c_str(), BuildMobSegment().c_str(), rate.c_str());
    if (written < 0 || written >= static_cast<int>(sizeof(title))) {
        x::runtime::LogWThrottled(911, 30000, "Titlebar", "title overflow n=%d cap=%d", written,
                                  static_cast<int>(sizeof(title)));
    }
    return title;
}

void FlushTitleIfDue(DWORD now, const std::string& title, bool force) {
    if (!gHwnd || !IsWindow(gHwnd) || title.empty()) return;
    if (title == gLastWrittenTitle) {
        gHavePendingTitle = false;
        gPendingTitle.clear();
        return;
    }
    if (!force && gLastTitleWriteTick && now - gLastTitleWriteTick < kTitleWriteMinMs) {
        gPendingTitle = title;
        gHavePendingTitle = true;
        return;
    }
    const std::wstring wideTitle = win::Utf8ToWide(title.c_str());
    win::SetTitleSafe(gHwnd, wideTitle.c_str(), 200);
    gLastTitleWriteTick = now;
    gLastWrittenTitle = title;
    gHavePendingTitle = false;
    gPendingTitle.clear();
}

void RestoreTitle() {
    if (gHwnd && IsWindow(gHwnd) && gSavedTitle) win::SetTitleSafe(gHwnd, gOrigTitle.c_str(), 500);
    gHwnd = nullptr;
    gSavedTitle = false;
    gLastWrittenTitle.clear();
    gPendingTitle.clear();
    gHavePendingTitle = false;
    gLastTitleWriteTick = 0;
}

void RestoreOrigTitleText() {
    if (!gHwnd || !IsWindow(gHwnd) || !gSavedTitle) return;
    win::SetTitleSafe(gHwnd, gOrigTitle.c_str(), 200);
    gLastWrittenTitle.clear();
    gPendingTitle.clear();
    gHavePendingTitle = false;
    gLastTitleWriteTick = 0;
}

void OnLeavePlay(const char* reason) {
    if (gRateActive) {
        x::runtime::LogI("Titlebar", "rate stop (%s) samples=%d", reason ? reason : "leave", gRingCount);
        ResetRates();
    }
    gHaveValidVitals = false;
    // WM/地图态改由 ImGui 顶栏 MAP 灯展示；离图恢复游戏原标题。
    RestoreOrigTitleText();
}

void Tick(DWORD now) {
    if (!gEnabled.load() || (gLastUpdate && now - gLastUpdate < kUpdateIntervalMs)) return;
    gLastUpdate = now;
    if (!gHwnd || !IsWindow(gHwnd)) {
        gHwnd = win::FindGameWindow();
        if (!gHwnd) return;
        wchar_t title[256]{};
        GetWindowTextW(gHwnd, title, 256);
        gOrigTitle = title;
        gSavedTitle = true;
    }
    // 防 splash→主窗重建后再次居中；有窗后只尝试一次（成败都停），避免晚还原窗口被突然回贴。
    static bool sTriedSnapTopLeft = false;
    if (!sTriedSnapTopLeft) {
        sTriedSnapTopLeft = true;
        if (win::PositionGameTopLeft(gHwnd)) {
            x::runtime::LogI("Titlebar", "game window snapped top-left hwnd=%p", (void*)gHwnd);
        }
    }

    const bool worldAlive = ports::world::IsAlive();
    const bool playReady = ports::world::IsPlayReady();

    // 未进图：只维持 WM 场景门控，禁止 LocalUser/IDM FindAll（即便 login-freeze 被误清）。
    if (!playReady) {
        if (gHaveValidVitals || gRateActive) OnLeavePlay("not_play_ready");
        else x::runtime::LogWThrottled(kResolveMissLogSlot, kResolveMissLogMs, "Titlebar",
                                       "not play ready (scene/WM)");
        if (!worldAlive || now - gLastRebind >= kRebindMissMs) {
            gLastRebind = now;
            (void)ports::world::Rebind(!worldAlive);
        }
        return;
    }

    if (gHaveValidVitals && worldAlive) {
        if (now - gLastRebind >= kRebindOkMs) {
            gLastRebind = now;
            game::TryResolveLocalUser();
            game::TryResolveItemDataManager();
        } else if (!game::LocalUserLooksOk()) {
            game::TryResolveLocalUser();
        }
    } else if (!worldAlive || !game::LocalCharacterStat() ||
               now - gLastRebind >= (gHaveValidVitals ? kRebindOkMs : kRebindMissMs)) {
        gLastRebind = now;
        (void)ports::world::Rebind(!worldAlive);
        game::TryResolveLocalUser();
        game::TryResolveItemDataManager();
    }
    if (gHavePendingTitle && gLastTitleWriteTick &&
        now - gLastTitleWriteTick >= kTitleWriteMinMs) {
        FlushTitleIfDue(now, gPendingTitle, true);
    }
    game::Vitals vitals{};
    if (!game::ReadVitals(vitals)) {
        if (gHaveValidVitals || gRateActive) OnLeavePlay("invalid_vitals");
        else x::runtime::LogWThrottled(kResolveMissLogSlot, kResolveMissLogMs, "Titlebar",
                                       "vitals miss (lobby/loading / no CharacterData)");
        return;
    }
    gHaveValidVitals = true;
    x::runtime::managed_main::SetLoginFreeze(false);
    UpdateRates(now, vitals);
    FlushTitleIfDue(now, BuildTitle(now, vitals), false);
}

DWORD WINAPI TitlebarThread(LPVOID) {
    timeBeginPeriod(1);
    x::runtime::LogI("Titlebar", "worker start (WorldManager→CharacterData→CharacterStat)");
    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gWorkerStop.load(); ++i) Sleep(50);
    if (gWorkerStop.load()) { timeEndPeriod(1); return 0; }
    if (!game::BindApis()) { timeEndPeriod(1); return 1; }
    Sleep(2000);
    // 登录期只绑 WM（bypassFreeze 主线程 FindAll）；LocalUser/IDM 等 play-ready。
    (void)ports::world::GetWorldManager();
    while (!gWorkerStop.load()) {
        Tick(GetTickCount());
        Sleep(kIdleSleepMs);
    }
    RestoreTitle();
    x::runtime::LogI("Titlebar", "worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    gEnabled.store(true);
    ResetRates();
    game::ClearLocalUser();
    RestoreTitle();
    gLastUpdate = 0;
    gLastRebind = 0;
    gHaveValidVitals = false;
}

void Shutdown() {
    gEnabled.store(false);
    RestoreTitle();
    ResetRates();
    game::ClearLocalUser();
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    gWorkerThread.store(CreateThread(nullptr, 0, TitlebarThread, nullptr, 0, nullptr));
}

void StopWorker() {
    gWorkerStop.store(true);
}

void SetEnabled(bool on) {
    if (gEnabled.exchange(on) == on) return;
    ResetRates();
    gHaveValidVitals = false;
    gLastUpdate = 0;
    if (!on) {
        RestoreTitle();
    } else {
        gLastWrittenTitle.clear();
        gPendingTitle.clear();
        gHavePendingTitle = false;
        gLastTitleWriteTick = 0;
    }
    x::runtime::LogI("Titlebar", "enabled=%d", on ? 1 : 0);
}

bool IsEnabled() {
    return gEnabled.load();
}

}  // namespace x::features::titlebar
