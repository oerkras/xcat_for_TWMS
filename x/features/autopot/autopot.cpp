// AutoPot — Classic TWMS (policy port from fengxing; IL2CPP use path).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "autopot.h"
#include "autopot_config.h"

#include "../../ipc/payload_control.h"
#include "../../runtime/log.h"
#include "../../ui/player_vitals.h"
#include "../ports/consumable_port.h"
#include "../ports/input_port.h"
#include "../ports/world_port.h"

#include <atomic>
#include <cstring>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x::features::autopot {
namespace {

std::atomic<bool> gHpEnabled{false};
std::atomic<bool> gMpEnabled{false};
std::atomic<int> gHpThreshold{config::kHpThresholdPct};
std::atomic<int> gMpThreshold{config::kMpThresholdPct};

Stats gStats{};
DWORD gLastCheck = 0;
DWORD gLastHpPot = 0;
DWORD gLastMpPot = 0;
DWORD gHpBackoffUntil = 0;
DWORD gMpBackoffUntil = 0;
DWORD gHpBindMissUntil = 0;  // 未绑/soft拒/空袋：紧急也不可破
DWORD gMpBindMissUntil = 0;
DWORD gHpEmptyCdUntil = 0;  // UseRequest empty（qty 未降）：游戏 CD 窗
DWORD gMpEmptyCdUntil = 0;
DWORD gHpLastEmptyStreakAt = 0;  // empty streak 按 empty-CD 间距计数
DWORD gMpLastEmptyStreakAt = 0;
DWORD gHpHealStuckUntil = 0;
DWORD gMpHealStuckUntil = 0;
DWORD gHpVerifyAt = 0;
DWORD gMpVerifyAt = 0;
int gHpBeforePot = -1;
int gMpBeforePot = -1;
int gHpEmptyStreak = 0;         // UseRequest empty（qty 未降）
int gMpEmptyStreak = 0;
int gHpIneffectiveStreak = 0;   // VerifyPot：喝了但 vitals 未涨
int gMpIneffectiveStreak = 0;
bool gPrevHpEnabled = false;
bool gPrevMpEnabled = false;
bool gPrevLanded = false;
DWORD gLandedAt = 0;
DWORD gDualOneSince = 0;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

void ResetHpRuntime() {
    gLastHpPot = 0;
    gHpBackoffUntil = 0;
    gHpBindMissUntil = 0;
    gHpEmptyCdUntil = 0;
    gHpLastEmptyStreakAt = 0;
    gHpHealStuckUntil = 0;
    gHpVerifyAt = 0;
    gHpBeforePot = -1;
    gHpEmptyStreak = 0;
    gHpIneffectiveStreak = 0;
}

void ResetMpRuntime() {
    gLastMpPot = 0;
    gMpBackoffUntil = 0;
    gMpBindMissUntil = 0;
    gMpEmptyCdUntil = 0;
    gMpLastEmptyStreakAt = 0;
    gMpHealStuckUntil = 0;
    gMpVerifyAt = 0;
    gMpBeforePot = -1;
    gMpEmptyStreak = 0;
    gMpIneffectiveStreak = 0;
}

// UseRequest 连续无效时，危急下 unity_kbd 脉冲 PageDown/PageUp（对齐枫星兜底；非 OnKey）。
void FirePotionKeyFallback(bool wantHp) {
    if (!ports::input::Ready()) return;
    ports::input::InjectKeyHold(wantHp ? VK_NEXT : VK_PRIOR, 80);
}

void VerifyPot(DWORD& verifyAt, int& before, int& streak, DWORD& backoffUntil, DWORD& healStuckUntil,
               int cur, DWORD now, const char* tag) {
    if (!verifyAt || static_cast<int>(now - verifyAt) < 0) return;
    if (cur < 0) return;
    if (before >= 0 && cur <= before) {
        if (++streak >= config::kFailStreakLimit) {
            backoffUntil = now + config::kEmptyPotBackoffMs;
            healStuckUntil = now + config::kHealStuckBackoffMs;
            x::runtime::LogW("AutoPot",
                             "%s pot ineffective (%s=%d<=%d), soft-backoff %us heal-stuck %us", tag,
                             tag, cur, before, config::kEmptyPotBackoffMs / 1000,
                             config::kHealStuckBackoffMs / 1000);
            // 读数失效/死亡期不脉冲，避免空烧绑定栏（对称门控）。
            const bool likelyDesync = (cur <= 1);
            const bool dead = (gStats.hpPct == 0);
            if (!likelyDesync && !dead) {
                if (std::strcmp(tag, "hp") == 0 && cur < config::kHpEmergencyPct) {
                    FirePotionKeyFallback(true);
                } else if (std::strcmp(tag, "mp") == 0 && cur < config::kMpEmergencyPct) {
                    FirePotionKeyFallback(false);
                }
            }
            streak = 0;
        }
    } else {
        if (before >= 0 && cur > before) {
            backoffUntil = 0;
            healStuckUntil = 0;
        }
        streak = 0;
    }
    verifyAt = 0;
    before = -1;
}

void Tick(DWORD now) {
    const bool hpOn = gHpEnabled.load(std::memory_order_relaxed);
    const bool mpOn = gMpEnabled.load(std::memory_order_relaxed);
    if (gPrevHpEnabled && !hpOn) ResetHpRuntime();
    if (gPrevMpEnabled && !mpOn) ResetMpRuntime();
    gPrevHpEnabled = hpOn;
    gPrevMpEnabled = mpOn;
    if (!hpOn && !mpOn) return;

    x::ipc::PayloadControl_Poll();

    if (gLastCheck && static_cast<int>(now - gLastCheck) < (int)config::kCheckIntervalMs) return;
    gLastCheck = now;

    // 登录/商城/切图：不喝药（合成门控 = 地图场景 ∧ WM alive）
    if (!ports::world::IsPlayReady()) {
        if (gPrevLanded) {
            ResetHpRuntime();
            ResetMpRuntime();
            gStats = {};
            gDualOneSince = 0;
            x::ui::player::ClearReadyLatch();
            gPrevLanded = false;
            gLandedAt = 0;
        }
        return;
    }

    x::ui::player::Vitals vit{};
    const bool vitOk = x::ui::player::ResolveAndRead(vit, now, false);
    if (!vitOk) {
        if (gPrevLanded) {
            ResetHpRuntime();
            ResetMpRuntime();
            gStats = {};
            gDualOneSince = 0;
            x::ui::player::ClearReadyLatch();
            gPrevLanded = false;
            gLandedAt = 0;
        }
        return;
    }

    if (!gPrevLanded) {
        gLandedAt = now;
        gDualOneSince = 0;
        x::ui::player::ClearReadyLatch();
    }
    gPrevLanded = true;

    if (gLandedAt && static_cast<int>(now - gLandedAt) < (int)config::kLandGraceMs) return;

    x::ui::player::NoteSample(vit, now);
    if (!x::ui::player::IsReadyLatched()) {
        gStats.valid = false;
        return;
    }

    if (x::ui::player::IsDead(vit)) {
        gStats.hpPct = 0;
        gStats.mpPct = x::ui::player::MpPct(vit);
        gStats.valid = true;
        gDualOneSince = 0;
        gHpVerifyAt = 0;
        gMpVerifyAt = 0;
        return;
    }

    int hpPct = x::ui::player::HpPct(vit);
    int mpPct = x::ui::player::MpPct(vit);

    if (hpPct == 1 && mpPct == 1) {
        if (!gDualOneSince) gDualOneSince = now;
        if (static_cast<int>(now - gDualOneSince) < (int)config::kDualOneDesyncMs) {
            gStats.hpPct = 1;
            gStats.mpPct = 1;
            gStats.valid = false;
            return;
        }
        gStats.valid = true;
    } else {
        gDualOneSince = 0;
        gStats.hpPct = hpPct;
        gStats.mpPct = mpPct;
        gStats.valid = true;
    }

    VerifyPot(gHpVerifyAt, gHpBeforePot, gHpIneffectiveStreak, gHpBackoffUntil, gHpHealStuckUntil,
              hpPct, now, "hp");
    VerifyPot(gMpVerifyAt, gMpBeforePot, gMpIneffectiveStreak, gMpBackoffUntil, gMpHealStuckUntil,
              mpPct, now, "mp");

    bool allowHp =
        hpOn && (!gLastHpPot || static_cast<int>(now - gLastHpPot) >= (int)config::kHpCooldownMs);
    bool allowMp =
        mpOn && (!gLastMpPot || static_cast<int>(now - gLastMpPot) >= (int)config::kMpCooldownMs);
    if (gHpBackoffUntil && static_cast<int>(now - gHpBackoffUntil) < 0) allowHp = false;
    if (gMpBackoffUntil && static_cast<int>(now - gMpBackoffUntil) < 0) allowMp = false;

    const bool hpVerifyPending = gHpVerifyAt && static_cast<int>(now - gHpVerifyAt) < 0;
    const bool mpVerifyPending = gMpVerifyAt && static_cast<int>(now - gMpVerifyAt) < 0;
    if (hpVerifyPending) allowHp = false;
    if (mpVerifyPending) allowMp = false;

    const bool hpHealStuck = gHpHealStuckUntil && static_cast<int>(now - gHpHealStuckUntil) < 0;
    const bool mpHealStuck = gMpHealStuckUntil && static_cast<int>(now - gMpHealStuckUntil) < 0;
    if (hpHealStuck) allowHp = false;
    if (mpHealStuck) allowMp = false;

    const int hpTh = gHpThreshold.load();
    const int mpTh = gMpThreshold.load();
    const bool hpEmergency = hpOn && hpPct >= 0 && hpPct < config::kHpEmergencyPct;
    const bool mpEmergency = mpOn && mpPct >= 0 && mpPct < config::kMpEmergencyPct;
    // 危急可破软退避，仍须过冷却；empty-CD / bind-miss / heal-stuck 不可破。
    if (hpEmergency && !hpVerifyPending && !hpHealStuck) {
        allowHp =
            !gLastHpPot || static_cast<int>(now - gLastHpPot) >= (int)config::kHpCooldownMs;
    }
    if (mpEmergency && !mpVerifyPending && !mpHealStuck) {
        allowMp =
            !gLastMpPot || static_cast<int>(now - gLastMpPot) >= (int)config::kMpCooldownMs;
    }
    if (gHpEmptyCdUntil && static_cast<int>(now - gHpEmptyCdUntil) < 0) allowHp = false;
    if (gMpEmptyCdUntil && static_cast<int>(now - gMpEmptyCdUntil) < 0) allowMp = false;
    if (gHpBindMissUntil && static_cast<int>(now - gHpBindMissUntil) < 0) allowHp = false;
    if (gMpBindMissUntil && static_cast<int>(now - gMpBindMissUntil) < 0) allowMp = false;

    if (hpPct >= hpTh) {
        gHpHealStuckUntil = 0;
        gHpEmptyCdUntil = 0;
        gHpEmptyStreak = 0;
        gHpIneffectiveStreak = 0;
    }
    if (mpPct >= mpTh) {
        gMpHealStuckUntil = 0;
        gMpEmptyCdUntil = 0;
        gMpEmptyStreak = 0;
        gMpIneffectiveStreak = 0;
    }

    const bool tryHp = allowHp && ((hpPct >= 0 && hpPct < hpTh) || hpEmergency);
    const bool tryMp = allowMp && ((mpPct >= 0 && mpPct < mpTh) || mpEmergency);
    if (!tryHp && !tryMp) return;

    // Prefer HP when both needed (保命优先); mage-first omitted in MVP.
    if (tryHp) {
        ports::consumable::FindResult fr{};
        if (ports::consumable::FindAndUseBoundPotion(true, fr)) {
            gStats.hpPotionQty = fr.qty;
            gLastHpPot = now;
            gHpEmptyCdUntil = 0;
            gHpEmptyStreak = 0;
            gHpBeforePot = hpPct;
            gHpVerifyAt = now + config::kPotEffectDelayMs;
            x::runtime::LogI("AutoPot", "act=hp pos=%d id=%d qty=%d hp=%d%%", fr.pos, fr.itemId,
                             fr.qty, hpPct);
        } else if (!fr.ok) {
            gStats.hpPotionQty = -1;
            gHpBindMissUntil = now + config::kBindMissBackoffMs;
            const char* why = fr.missWhy ? fr.missWhy : "unknown";
            static DWORD s_noHp = 0;
            if (!s_noHp || static_cast<int>(now - s_noHp) >= 10000) {
                s_noHp = now;
                if (std::strcmp(why, "not_in_bag") == 0) {
                    x::runtime::LogW("AutoPot", "PageDown HP bind not in bag id=%d", fr.itemId);
                } else if (std::strcmp(why, "not_item") == 0) {
                    x::runtime::LogW("AutoPot", "PageDown key not Item (value=%d)", fr.itemId);
                } else if (std::strcmp(why, "empty_bind") == 0 || std::strcmp(why, "no_fkm") == 0 ||
                           std::strcmp(why, "bad_vk") == 0 || std::strcmp(why, "no_getdata") == 0) {
                    x::runtime::LogW("AutoPot", "no PageDown HP bind (%s)", why);
                } else {
                    x::runtime::LogW("AutoPot", "PageDown HP resolve miss why=%s id=%d", why,
                                     fr.itemId);
                }
            }
        } else {
            // 找到药但 qty 未降：按游戏 CD/拒用处理，勿在同窗连打进 heal-stuck。
            gStats.hpPotionQty = fr.qty;
            gLastHpPot = now;
            gHpEmptyCdUntil = now + config::kEmptyUseCooldownMs;
            const bool countStreak =
                !gHpLastEmptyStreakAt ||
                static_cast<int>(now - gHpLastEmptyStreakAt) >= (int)config::kEmptyStreakGapMs;
            if (countStreak) {
                gHpLastEmptyStreakAt = now;
                if (++gHpEmptyStreak >= config::kFailStreakLimit) {
                    // 只加长 empty-CD（危急不可破）；不软退避/heal-stuck，避免非危急空窗 8s。
                    gHpEmptyCdUntil = now + config::kEmptyStreakCooldownMs;
                    x::runtime::LogW("AutoPot",
                                     "hp use empty streak → empty-cd %ums (no soft-backoff)",
                                     config::kEmptyStreakCooldownMs);
                    if (hpEmergency && hpPct > 1) FirePotionKeyFallback(true);
                    gHpEmptyStreak = 0;
                } else {
                    x::runtime::LogW("AutoPot",
                                     "hp use empty → cd %ums streak=%d/%d pos=%d id=%d",
                                     config::kEmptyUseCooldownMs, gHpEmptyStreak,
                                     config::kFailStreakLimit, fr.pos, fr.itemId);
                }
            }
        }
    }
    if (tryMp) {
        ports::consumable::FindResult fr{};
        if (ports::consumable::FindAndUseBoundPotion(false, fr)) {
            gStats.mpPotionQty = fr.qty;
            gLastMpPot = now;
            gMpEmptyCdUntil = 0;
            gMpEmptyStreak = 0;
            gMpBeforePot = mpPct;
            gMpVerifyAt = now + config::kPotEffectDelayMs;
            x::runtime::LogI("AutoPot", "act=mp pos=%d id=%d qty=%d mp=%d%%", fr.pos, fr.itemId,
                             fr.qty, mpPct);
        } else if (!fr.ok) {
            gStats.mpPotionQty = -1;
            gMpBindMissUntil = now + config::kBindMissBackoffMs;
            const char* why = fr.missWhy ? fr.missWhy : "unknown";
            static DWORD s_noMp = 0;
            if (!s_noMp || static_cast<int>(now - s_noMp) >= 10000) {
                s_noMp = now;
                if (std::strcmp(why, "not_in_bag") == 0) {
                    x::runtime::LogW("AutoPot", "PageUp MP bind not in bag id=%d", fr.itemId);
                } else if (std::strcmp(why, "not_item") == 0) {
                    x::runtime::LogW("AutoPot", "PageUp key not Item (value=%d)", fr.itemId);
                } else if (std::strcmp(why, "empty_bind") == 0 || std::strcmp(why, "no_fkm") == 0 ||
                           std::strcmp(why, "bad_vk") == 0 || std::strcmp(why, "no_getdata") == 0) {
                    x::runtime::LogW("AutoPot", "no PageUp MP bind (%s)", why);
                } else {
                    x::runtime::LogW("AutoPot", "PageUp MP resolve miss why=%s id=%d", why,
                                     fr.itemId);
                }
            }
        } else {
            gStats.mpPotionQty = fr.qty;
            gLastMpPot = now;
            gMpEmptyCdUntil = now + config::kEmptyUseCooldownMs;
            const bool countStreak =
                !gMpLastEmptyStreakAt ||
                static_cast<int>(now - gMpLastEmptyStreakAt) >= (int)config::kEmptyStreakGapMs;
            if (countStreak) {
                gMpLastEmptyStreakAt = now;
                if (++gMpEmptyStreak >= config::kFailStreakLimit) {
                    // 只加长 empty-CD（危急不可破）；不软退避/heal-stuck，避免非危急空窗 8s。
                    gMpEmptyCdUntil = now + config::kEmptyStreakCooldownMs;
                    x::runtime::LogW("AutoPot",
                                     "mp use empty streak → empty-cd %ums (no soft-backoff)",
                                     config::kEmptyStreakCooldownMs);
                    if (mpEmergency && mpPct > 1) FirePotionKeyFallback(false);
                    gMpEmptyStreak = 0;
                } else {
                    x::runtime::LogW("AutoPot",
                                     "mp use empty → cd %ums streak=%d/%d pos=%d id=%d",
                                     config::kEmptyUseCooldownMs, gMpEmptyStreak,
                                     config::kFailStreakLimit, fr.pos, fr.itemId);
                }
            }
        }
    }
}

DWORD WINAPI WorkerProc(void*) {
    timeBeginPeriod(1);
    while (!gWorkerStop.load()) {
        Tick(GetTickCount());
        Sleep(20);
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    gHpEnabled.store(false);
    gMpEnabled.store(false);
    gHpThreshold.store(config::kHpThresholdPct);
    gMpThreshold.store(config::kMpThresholdPct);
    gStats = {};
    ResetHpRuntime();
    ResetMpRuntime();
    gPrevLanded = false;
    gDualOneSince = 0;
    ports::consumable::Init();
    x::ui::player::Init();
    x::runtime::LogI("Feature", "autopot ready (TWMS: PageDown/PageUp bind + UseRequest)");
}

void Shutdown() {
    StopWorker();
    ports::consumable::Shutdown();
    x::ui::player::Shutdown();
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE h = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    gWorkerThread.store(h);
}

void StopWorker() {
    gWorkerStop.store(true);
    // Do not join under loader lock.
}

void SetHpEnabled(bool on) { gHpEnabled.store(on); }
void SetMpEnabled(bool on) { gMpEnabled.store(on); }
bool IsHpEnabled() { return gHpEnabled.load(); }
bool IsMpEnabled() { return gMpEnabled.load(); }
bool IsEnabled() { return gHpEnabled.load() || gMpEnabled.load(); }

void SetHpThresholdPct(int pct) {
    gHpThreshold.store(pct < 1 ? 1 : (pct > 99 ? 99 : pct));
}
void SetMpThresholdPct(int pct) {
    gMpThreshold.store(pct < 1 ? 1 : (pct > 99 ? 99 : pct));
}
int GetHpThresholdPct() { return gHpThreshold.load(); }
int GetMpThresholdPct() { return gMpThreshold.load(); }
Stats GetStats() { return gStats; }

}  // namespace x::features::autopot
