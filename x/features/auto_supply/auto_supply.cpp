#include "auto_supply.h"

#include "../autopot/autopot.h"
#include "../fly/fly.h"
#include "../notify/notify.h"
#include "../ports/consumable_port.h"
#include "../ports/shop_port.h"
#include "../ports/teleport_port.h"
#include "../ports/travel_port.h"
#include "../ports/world_port.h"
#include "../sellbag/sellbag.h"
#include "../simple_combat/simple_combat.h"
#include "../char_boot/char_boot.h"
#include "../travel/travel.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"
#include "xcat_auto_supply.h"
#include "xcat_item_catalog.h"
#include "xcat_map_towns.h"
#include "xcat_sellbag.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace x::features::auto_supply {

namespace {
std::atomic<bool> gAbortReq{false};
char gAbortWhy[128]{};
std::atomic<bool> gReturnReq{false};
std::atomic<bool> gTripReq{false};
std::atomic<bool> gRechargeReq{false};
char gLastFarmMap[64]{};
}  // namespace shared

namespace {

namespace shop = ports::shop;
namespace consumable = ports::consumable;

std::atomic<bool> gDesired{false};
char gShopMap[64]{};
char gShopExclude[64]{};
char gResolvedNpc[24]{};
int gShopStockReroute = 0;  // 本趟因「店内无目标货」改道次数
constexpr int kMaxShopStockReroute = 2;
int gRefillStockMiss = 0;   // PlanRefills：需要补但店内无货/无价的条目数
int gEquipTrigger = 0;
xcat::AutoSupplyConfig gCfg{};
uint64_t gSeenCfgTick = 0;
uint32_t gHandledManualSeq = 0;
bool gManualSeqBootstrapped = false;
bool gPendingReturnFarm = false;

enum class Phase {
    Idle,
    Pause,
    GoingTown,
    OpeningShop,
    Selling,
    Buying,
    Charging,  // 已开店：仅飞镖 Charge（一键充值，不关店、不回城）
    ClosingShop,
    Returning,
    Cooldown,
};
Phase gPhase = Phase::Idle;
DWORD gPhaseSince = 0;
DWORD gLastBagPoll = 0;
DWORD gCooldownUntil = 0;
DWORD gWaitOpenNotified = 0;
DWORD gLastTalkAttempt = 0;
int gTalkMissStreak = 0;  // OpeningShop：目标 NPC 连续对不上 → 提早改道（BIN 4bb7ea）
constexpr int kTalkMissReroute = 3;
DWORD gLastMenuAttempt = 0;
DWORD gLastBuyAttempt = 0;
DWORD gLastCloseShopAttempt = 0;
DWORD gShopReadySince = 0;
int gCloseShopAttempts = 0;
DWORD gScrollAttemptAt = 0;
DWORD gScrollRetryAfter = 0;  // no_consume 等硬失败：到期前不重发卷
int gScrollTries = 0;
bool gTriedScroll = false;
bool gScrollPendingLand = false;  // 已成功用卷，等换图后再结束用卷阶段
char gScrollMapAtUse[64]{};       // 用卷时地图；换图后视为落地
bool gPreferDirect = false;
bool gPausedCombat = false;
bool gPausedFly = false;
bool gManualTrip = false;
bool gPotionEmptyArmed = true;  // 缺药触发后闭锁，直到绑定药再次达标才重开
bool gCustomLowArmed = true;
bool gCustom2LowArmed = true;
bool gFeedLowArmed = true;
int gBoundHpItemId = 0;
int gBoundMpItemId = 0;
// 监视快照（PublishStatusIni）
int gWatchCustomHave = -1, gWatchCustomBelow = 0;
int gWatchCustom2Have = -1, gWatchCustom2Below = 0;
int gWatchFeedHave = -1, gWatchFeedBelow = 0;
int gWatchPotHpHave = -1, gWatchPotMpHave = -1, gWatchPotBelow = 1;
DWORD gReturnStableSince = 0;
DWORD gTripTravelArmAt = 0;  // 非 0：此前禁止 RequestGoto（开趟冷却窗）
Status gStatus{};

enum class BuyStep : int {
    ReturnScroll = 0,
    Charge,
    PlanRefills,
    BuyRefills,
    ConfirmBuy,  // 等消耗栏到账后再扣 need（避免 FIRED 但服拒/未入包就当买完）
    Done,
};
BuyStep gBuyStep = BuyStep::ReturnScroll;

enum class BuySlot : int {
    Hp = 0,
    Mp,
    Custom,
    Custom2,
    Feed,
    FeedAlt,
    Done,
};
BuySlot gBuySlot = BuySlot::Hp;
int gBuyNeed = 0;
int gActiveBuyId = 0;
int gBuyConfirmBefore = 0;   // ConfirmBuy：发包前 CountConsume
int gBuyConfirmExpect = 0;   // ConfirmBuy：本拍实发 qty（MaxSlot 截断后）
DWORD gBuyConfirmSince = 0;
int gBuyNoGainStreak = 0;    // 连续「发包后库存未增」次数；过大则跳过该目标
int gBuySoftBatchCap = 0;    // >0：到账失败后的保守单批上限（如 1000）
int64_t gMesoBudgetOverride = -1;  // >=0：规划用保守金币
int gScrollJustBoughtPrice = 0;    // 本趟已买回城卷标价
int64_t gMesoBeforeScrollBuy = -1; // 买卷前读数；用于判断 Money 是否已扣
constexpr int kBuySlotCount = 6;
int gPlannedNeed[kBuySlotCount]{};  // per slot after meso split
int gPlannedPrice[kBuySlotCount]{}; // 对应单价，买入时再按实时 meso 截断

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};

constexpr DWORD kBagPollMs = 2500;   // 缺药/自定义/饲料低库存监视（主线程扫栏，保持节流）
constexpr DWORD kBoundPeekMs = 500;  // 绑定药 ID → status → 面板补红/蓝对齐（跟手）
constexpr DWORD kGotoTimeoutMs = 180000;
constexpr DWORD kWaitOpenTimeoutMs = 90000;
constexpr DWORD kSellTimeoutMs = 120000;
constexpr DWORD kBuyTimeoutMs = 120000;
// 老机器 / 深洞多跳贴门慢；180s 易误熔断。回程不走卷，只赶路。
constexpr DWORD kReturnTimeoutMs = 420000;
constexpr DWORD kCooldownMs = 45000;
// BIN 15:57：到站立刻 Resume，目标图仍 InterStage/lu=null → 黑屏卡住。
constexpr DWORD kReturnStableMs = 2000;
// BIN 16:11：贴怪 Doing 狂点后立刻补给赶路 → pendingError=205 硬断。
// 开趟先停战斗，再强制瞬移自冷 + 墙钟冷却，到期才 RequestGoto。
constexpr DWORD kTripStartCoolMs = 2800;
constexpr DWORD kPhaseTickMs = 200;
constexpr DWORD kTalkRetryMs = 3500;
constexpr DWORD kMenuConfirmMs = 900;  // 菜单/Say 推进节流，避免每 tick 连点确定
constexpr DWORD kShopReadySettleMs = 700;  // 开窗后稍等再卖（BIN: 015D 后立刻卖 → 205）
constexpr DWORD kBuyRetryMs = 400;
constexpr DWORD kChargeRetryMs = 80;  // 飞镖 Charge 轻量；BIN 21:20 约 400ms/格偏慢
// 行程补货内 Charge：LIST_STALE 死等上限（BIN 23:19 魔法森林店 sellListN=0 卡 ~50s）
constexpr DWORD kBuyChargeStaleMaxMs = 8000;
constexpr DWORD kCloseShopRetryMs = 400;
constexpr int kMaxCloseShopAttempts = 8;
constexpr DWORD kScrollWaitMs = 8000;
// 用卷：最多 3 次（含首次）。no_consume 不立刻 walk，隔 ~1.6s 再试（BIN 23:18 落台后首发偶失败）。
constexpr int kMaxScrollTries = 3;
constexpr DWORD kScrollRetryGapMs = 1600;
constexpr int kInvConsume = 2;

bool IsTownMapIdHeuristic(int mapId) {
    if (mapId <= 0) return false;
    // 挂机图禁记 / 城镇判定：共用 common 真源（仅 map_info.town=1）。
    // 勿用 QueryNativeIsTown / mapId%1000000==0（BIN 2026-08-15 沼澤地Ⅰ误判）。
    return xcat::IsMapInfoTown(runtime::GetBinDir(), mapId);
}

bool IsTownMapName(const char* map) {
    if (!map || !map[0]) return false;
    const int id = atoi(map);
    if (id > 0) return IsTownMapIdHeuristic(id);
    return false;
}

void SetMsg(const char* m) {
    strncpy_s(gStatus.message, m ? m : "", _TRUNCATE);
}

void Publish(notify::NotificationKind kind, const char* key, const char* title, const char* body) {
    notify::PublishNotification(notify::NotificationEvent{kind, key, title, body, 5500});
}

void SyncStatus() {
    gStatus.busy = gPhase != Phase::Idle && gPhase != Phase::Cooldown;
    switch (gPhase) {
    case Phase::Idle: gStatus.state = TripState::Idle; break;
    case Phase::Pause: gStatus.state = TripState::Pause; break;
    case Phase::GoingTown: gStatus.state = TripState::GoingTown; break;
    case Phase::OpeningShop: gStatus.state = TripState::OpeningShop; break;
    case Phase::Selling: gStatus.state = TripState::Selling; break;
    case Phase::Buying: gStatus.state = TripState::Buying; break;
    case Phase::Charging: gStatus.state = TripState::Buying; break;  // 内部复用 Buying；落盘见 Recharging
    case Phase::ClosingShop:
    case Phase::Returning: gStatus.state = TripState::Returning; break;
    case Phase::Cooldown: gStatus.state = TripState::Cooldown; break;
    }
    strncpy_s(gStatus.lastFarmMap, gLastFarmMap, _TRUNCATE);
    strncpy_s(gStatus.shopMap, gShopMap, _TRUNCATE);
}

void PublishStatusIni() {
    xcat::AutoSupplyStatus st{};
    xcat::AutoSupplyStatusSetDefaults(st);
    switch (gPhase) {
    case Phase::Idle: st.state = xcat::kAutoSupplyStateIdle; break;
    case Phase::GoingTown: st.state = xcat::kAutoSupplyStateGoingTown; break;
    case Phase::OpeningShop: st.state = xcat::kAutoSupplyStateOpeningShop; break;
    case Phase::Selling: st.state = xcat::kAutoSupplyStateSelling; break;
    case Phase::Buying: st.state = xcat::kAutoSupplyStateBuying; break;
    case Phase::Charging: st.state = xcat::kAutoSupplyStateRecharging; break;
    case Phase::ClosingShop:
    case Phase::Returning: st.state = xcat::kAutoSupplyStateReturning; break;
    case Phase::Cooldown: st.state = xcat::kAutoSupplyStateTripDone; break;
    default: st.state = xcat::kAutoSupplyStateTripTrading; break;
    }
    strncpy_s(st.message, gStatus.message, _TRUNCATE);
    strncpy_s(st.lastFarmMapName, gLastFarmMap, _TRUNCATE);
    st.pendingReturnFarm = gPendingReturnFarm ? 1u : 0u;
    st.boundHpItemId = gBoundHpItemId > 0 ? gBoundHpItemId : 0;
    st.boundMpItemId = gBoundMpItemId > 0 ? gBoundMpItemId : 0;
    st.watchCustomHave = gWatchCustomHave;
    st.watchCustomBelow = gWatchCustomBelow;
    st.watchCustomArmed = gCustomLowArmed ? 1u : 0u;
    st.watchCustom2Have = gWatchCustom2Have;
    st.watchCustom2Below = gWatchCustom2Below;
    st.watchCustom2Armed = gCustom2LowArmed ? 1u : 0u;
    st.watchFeedHave = gWatchFeedHave;
    st.watchFeedBelow = gWatchFeedBelow;
    st.watchFeedArmed = gFeedLowArmed ? 1u : 0u;
    st.watchPotHpHave = gWatchPotHpHave;
    st.watchPotMpHave = gWatchPotMpHave;
    st.watchPotBelow = gWatchPotBelow;
    st.watchPotArmed = gPotionEmptyArmed ? 1u : 0u;
    st.writeTickMs = GetTickCount64();
    (void)xcat::WriteAutoSupplyStatus(runtime::GetBinDir(), st);
}

void Enter(Phase p, const char* msg) {
    gPhase = p;
    gPhaseSince = GetTickCount();
    SetMsg(msg);
    SyncStatus();
    PublishStatusIni();
    runtime::LogI("AutoSupply", "phase → %d %s", static_cast<int>(p), msg ? msg : "");
}

void PauseSystems() {
    simple_combat::SetHardPause(simple_combat::HardPauseHolder::AutoSupply, true);
    gPausedCombat = true;
    fly::SetExternalPause(true);
    gPausedFly = true;
}

void ResumeSystems() {
    if (gPausedCombat) {
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::AutoSupply, false);
        gPausedCombat = false;
    }
    if (gPausedFly) {
        fly::SetExternalPause(false);
        gPausedFly = false;
    }
}

// 开趟冷却：停手后强制瞬移自冷，并设墙钟；到期前禁止 RequestGoto。
void ArmTripStartCool(DWORD now, const char* why) {
    PauseSystems();
    ports::teleport::ForceNativeCooldownMs(kTripStartCoolMs);
    const DWORD rem = ports::teleport::NativeCooldownRemainingMs();
    const DWORD wait = rem > kTripStartCoolMs ? rem : kTripStartCoolMs;
    gTripTravelArmAt = now + wait;
    runtime::LogI("AutoSupply", "trip cool arm wait=%ums nativeRem=%ums why=%s", (unsigned)wait,
                  (unsigned)rem, why ? why : "?");
}

bool TripTravelReady(DWORD now) {
    if (!gTripTravelArmAt) return true;
    if (static_cast<int>(now - gTripTravelArmAt) < 0) {
        SetMsg("开趟冷却中…");
        return false;
    }
    const DWORD rem = ports::teleport::NativeCooldownRemainingMs();
    if (rem > 0) {
        SetMsg("开趟冷却中…");
        return false;
    }
    gTripTravelArmAt = 0;
    runtime::LogI("AutoSupply", "trip cool done — allow travel");
    return true;
}

void ClearTripTravelArm() { gTripTravelArmAt = 0; }

void RearmLowStockLatchesAfterTrip(const char* why);

// 硬闸上升沿会 BeginLieSafeLand；用卷/赶路/开店前必须站稳，否则底层低 Y 掉出图外重载。
// review：不能只看 IsSafeLandActive——soft_or_net_quiet 拆台后 inactive 仍可能悬空。
bool WaitSafeLand(DWORD now) {
    if (simple_combat::IsSafeLandActive()) {
        SetMsg("安全落台中…");
        static DWORD sLog = 0;
        if (!sLog || now - sLog >= 1500) {
            sLog = now;
            runtime::LogI("AutoSupply", "wait safe land (before scroll/travel/shop)");
        }
        return false;
    }

    ports::teleport::FlightState st{};
    const bool have = ports::teleport::QueryFlightState(st) && st.ok;
    if (have && st.onFh) return true;

    // Travel 托空中：不抢 Combat 落台，也不要空中发卷/开店。
    if (travel::IsActive()) {
        SetMsg("赶路稳图中…");
        static DWORD sTravelLog = 0;
        if (!sTravelLog || now - sTravelLog >= 1500) {
            sTravelLog = now;
            runtime::LogI("AutoSupply", "wait travel settle (airborne before scroll/shop)");
        }
        return false;
    }

    if (have && !st.onFh) {
        runtime::LogI("AutoSupply", "wait safe land re-request airborne ap=(%.0f,%.0f)", st.x,
                      st.y);
        simple_combat::RequestSafeLand("auto_supply_wait_airborne");
        SetMsg("安全落台中…");
        return false;
    }

    // 飞控读不到：放行（避免 InterStage 卡死）；EnsureSafeLandIfAirborne 下一拍会再兜。
    return true;
}

// BIN 9d504e：Travel settle 到站后落台可能已结束（挂机图 onFh 早卸），店图仍悬空。
// 未挂台且 Travel 已 Idle → 再开同款落台，再等 WaitSafeLand。
void EnsureSafeLandIfAirborne(DWORD now) {
    if (simple_combat::IsSafeLandActive()) return;
    if (travel::IsActive()) return;
    ports::teleport::FlightState st{};
    if (!ports::teleport::QueryFlightState(st) || !st.ok) return;
    if (st.onFh) return;
    runtime::LogI("AutoSupply", "request safe land airborne ap=(%.0f,%.0f)", st.x, st.y);
    simple_combat::RequestSafeLand("auto_supply_airborne");
    (void)now;
}

void RearmLowStockLatchesAfterTrip(const char* why);

void FailTrip(const char* why) {
    travel::RequestStop();
    sellbag::Abort(why);
    ResumeSystems();
    gManualTrip = false;
    gRechargeReq.store(false, std::memory_order_release);
    gPendingReturnFarm = false;
    gReturnStableSince = 0;
    ClearTripTravelArm();
    RearmLowStockLatchesAfterTrip("fail_trip");
    PublishStatusIni();
    Publish(notify::NotificationKind::Warning, "auto-supply-fail", "自动补给中止", why);
    gCooldownUntil = GetTickCount() + kCooldownMs;
    Enter(Phase::Cooldown, why);
}

// 对照枫星 TownGotoWait：只对硬失败收尾；AlreadyThere 是陈旧终态/同图提示，不能当行程失败。
bool TravelFailIsHard(travel::FailKind k) {
    return k == travel::FailKind::Unreachable || k == travel::FailKind::FakeFireStop ||
           k == travel::FailKind::BadTarget || k == travel::FailKind::FireStuck;
}

bool EquipTriggerMet(int& used, int& cap) {
    used = 0;
    cap = 0;
    if (!shop::QueryBagUsage(true, used, cap)) return false;
    gStatus.equipUsed = used;
    gStatus.equipCap = cap;
    if (cap <= 0) return false;
    if (gEquipTrigger <= 0) return used >= cap;
    return used >= gEquipTrigger;
}

int CountConsume(int itemId);  // 自定义低库存触发 / 补货计划共用

// 绑定药数量；not_in_bag 视为 0。其它 miss 返回 false（*outHave 可仍为 -1）。
bool BoundPotionHave(bool wantHp, int& outHave) {
    outHave = -1;
    consumable::FindResult fr{};
    (void)consumable::ResolveBoundPotion(wantHp, fr);
    if (!fr.ok) {
        if (!fr.missWhy || std::strcmp(fr.missWhy, "not_in_bag") != 0) return false;
        outHave = 0;
        return true;
    }
    outHave = fr.qty;
    return true;
}

bool BoundPotionQtyLow(bool wantHp, int below, char* why, size_t whyCap) {
    if (below < 1) below = 1;
    consumable::FindResult fr{};
    (void)consumable::ResolveBoundPotion(wantHp, fr);
    int have = -1;
    if (!fr.ok) {
        if (!fr.missWhy || std::strcmp(fr.missWhy, "not_in_bag") != 0) return false;
        have = 0;
    } else {
        have = fr.qty;
    }
    if (have >= below) return false;
    if (why && whyCap) {
        snprintf(why, whyCap, "%s have=%d<%d id=%d", wantHp ? "hp" : "mp", have, below, fr.itemId);
    }
    return true;
}

bool BoundPotionSidesStillLow(int below) {
    // 与触发一致：未勾选「补红/补蓝」的一侧不监视、不闭锁
    const bool hpOn = autopot::IsHpEnabled() && gCfg.refillHpEnabled != 0;
    const bool mpOn = autopot::IsMpEnabled() && gCfg.refillMpEnabled != 0;
    if (!hpOn && !mpOn) return false;
    if (hpOn && BoundPotionQtyLow(true, below, nullptr, 0)) return true;
    if (mpOn && BoundPotionQtyLow(false, below, nullptr, 0)) return true;
    return false;
}

bool PotionLowTriggerMet(char* why, size_t whyCap) {
    if (!gCfg.tripOnPotionEmpty) return false;
    // 缺药回城只对「已勾选补货」的绑定侧生效；未勾选红/蓝绝不因该侧空药开趟去补
    const bool hpOn = autopot::IsHpEnabled() && gCfg.refillHpEnabled != 0;
    const bool mpOn = autopot::IsMpEnabled() && gCfg.refillMpEnabled != 0;
    if (!hpOn && !mpOn) return false;
    const int below = gCfg.tripOnPotionBelow > 0 ? gCfg.tripOnPotionBelow : 1;
    gWatchPotBelow = below;
    int hpHave = -1, mpHave = -1;
    if (hpOn) (void)BoundPotionHave(true, hpHave);
    if (mpOn) (void)BoundPotionHave(false, mpHave);
    gWatchPotHpHave = hpOn ? hpHave : -1;
    gWatchPotMpHave = mpOn ? mpHave : -1;
    if (!gPotionEmptyArmed) {
        if (!BoundPotionSidesStillLow(below)) {
            gPotionEmptyArmed = true;
            runtime::LogI("AutoSupply", "缺药触发已重开（绑定药已达标 below=%d）", below);
        }
        return false;
    }
    if (hpOn && BoundPotionQtyLow(true, below, why, whyCap)) return true;
    if (mpOn && BoundPotionQtyLow(false, below, why, whyCap)) return true;
    return false;
}

// 消耗栏 CODE 低库存触发（自定义1/2）；armed 指针可空。
bool ConsumeCodeLowTriggerMet(bool tripOn, bool refillOn, const char* code, int below, int buyTo,
                              bool* armed, int* outHave, int* outBelow, const char* tag, char* why,
                              size_t whyCap) {
    if (outHave) *outHave = -1;
    if (outBelow) *outBelow = below;
    if (!tripOn || !refillOn || below <= 0 || buyTo <= 0) return false;
    const int id = (code && code[0]) ? atoi(code) : 0;
    if (id <= 0) return false;
    const int have = CountConsume(id);
    if (outHave) *outHave = have;
    if (armed && !*armed) {
        if (have >= below) {
            *armed = true;
            runtime::LogI("AutoSupply", "%s低库存触发已重开 id=%d have=%d below=%d", tag ? tag : "?",
                          id, have, below);
        }
        return false;
    }
    if (have >= below) return false;
    if (why && whyCap) {
        snprintf(why, whyCap, "%s id=%d have=%d<%d", tag ? tag : "?", id, have, below);
    }
    return true;
}

bool CustomLowTriggerMet(char* why, size_t whyCap) {
    return ConsumeCodeLowTriggerMet(gCfg.tripOnCustomLow != 0, gCfg.refillCustomEnabled != 0,
                                    gCfg.refillCustomCode, gCfg.tripOnCustomBelow,
                                    gCfg.refillCustomBuyTo, &gCustomLowArmed, &gWatchCustomHave,
                                    &gWatchCustomBelow, "自定义", why, whyCap);
}

bool Custom2LowTriggerMet(char* why, size_t whyCap) {
    return ConsumeCodeLowTriggerMet(gCfg.tripOnCustom2Low != 0, gCfg.refillCustom2Enabled != 0,
                                    gCfg.refillCustom2Code, gCfg.tripOnCustom2Below,
                                    gCfg.refillCustom2BuyTo, &gCustom2LowArmed, &gWatchCustom2Have,
                                    &gWatchCustom2Below, "自定义2", why, whyCap);
}

int CountFeedHave() {
    const int primary = gCfg.refillFeedCode[0] ? atoi(gCfg.refillFeedCode) : 0;
    const int alt = atoi(xcat::kAutoSupplyDefaultRefillFeedAltCode);
    int sum = 0;
    if (primary > 0) sum += CountConsume(primary);
    if (alt > 0) sum += CountConsume(alt);
    return sum;
}

bool FeedLowTriggerMet(char* why, size_t whyCap) {
    gWatchFeedHave = -1;
    gWatchFeedBelow = gCfg.tripOnFeedBelow;
    if (!gCfg.tripOnFeedLow || !gCfg.refillFeedEnabled) return false;
    if (gCfg.tripOnFeedBelow <= 0 || gCfg.refillFeedBuyTo <= 0) return false;
    const int have = CountFeedHave();
    const int below = gCfg.tripOnFeedBelow;
    gWatchFeedHave = have;
    if (!gFeedLowArmed) {
        if (have >= below) {
            gFeedLowArmed = true;
            runtime::LogI("AutoSupply", "饲料低库存触发已重开 have=%d below=%d", have, below);
        }
        return false;
    }
    if (have >= below) return false;
    if (why && whyCap) snprintf(why, whyCap, "feed have=%d<%d", have, below);
    return true;
}

// 仅对「本趟已闭锁且仍低」的监视项告警，避免装备/手动一趟误报「已暂停」。
void NotifyIfStillLowAfterTrip() {
    char detail[160]{};
    size_t n = 0;
    auto append = [&](const char* s) {
        if (!s || !s[0] || n + 1 >= sizeof(detail)) return;
        if (n) detail[n++] = ';';
        const size_t len = strlen(s);
        if (n + len >= sizeof(detail)) return;
        memcpy(detail + n, s, len);
        n += len;
        detail[n] = 0;
    };
    if (!gCustomLowArmed && gCfg.tripOnCustomLow && gCfg.refillCustomEnabled &&
        gCfg.tripOnCustomBelow > 0) {
        const int id = atoi(gCfg.refillCustomCode);
        if (id > 0 && CountConsume(id) < gCfg.tripOnCustomBelow) {
            append("自定义仍低");
            gCustomLowArmed = true;
            runtime::LogW("AutoSupply", "自定义闭锁已重开（本趟未补上，冷却后可再试）");
        }
    }
    if (!gCustom2LowArmed && gCfg.tripOnCustom2Low && gCfg.refillCustom2Enabled &&
        gCfg.tripOnCustom2Below > 0) {
        const int id = atoi(gCfg.refillCustom2Code);
        if (id > 0 && CountConsume(id) < gCfg.tripOnCustom2Below) {
            append("自定义2仍低");
            gCustom2LowArmed = true;
            runtime::LogW("AutoSupply", "自定义2闭锁已重开（本趟未补上，冷却后可再试）");
        }
    }
    if (!gFeedLowArmed && gCfg.tripOnFeedLow && gCfg.refillFeedEnabled &&
        gCfg.tripOnFeedBelow > 0) {
        if (CountFeedHave() < gCfg.tripOnFeedBelow) {
            append("饲料仍低");
            gFeedLowArmed = true;
            runtime::LogW("AutoSupply", "饲料闭锁已重开（本趟未补上，冷却后可再试）");
        }
    }
    if (!gPotionEmptyArmed && gCfg.tripOnPotionEmpty) {
        const int below = gCfg.tripOnPotionBelow > 0 ? gCfg.tripOnPotionBelow : 1;
        if (BoundPotionSidesStillLow(below)) {
            append("绑药仍低");
            // 空趟/店无货时若一直闭锁，用户会感觉「没蓝了也不再回去」
            gPotionEmptyArmed = true;
            runtime::LogW("AutoSupply", "缺药闭锁已重开（本趟未补上，冷却后可再试）");
        }
    }
    if (!detail[0]) return;
    runtime::LogW("AutoSupply", "趟后仍低库存：%s", detail);
    Publish(notify::NotificationKind::Warning, "auto-supply-still-low", "本趟未补够",
            detail[0] ? detail : "店内可能无货/未到账/背包满；冷却后可再试");
}

// 行程结束（含冷却开始）必须重开闭锁。
// 否则：店内补满 → 闭锁 → 回城/冷却 45s 内喝光 → Idle 时仍判「偏低」永远不重开（本机 22:48 蓝药复现）。
void RearmLowStockLatchesAfterTrip(const char* why) {
    const bool any = !gPotionEmptyArmed || !gCustomLowArmed || !gCustom2LowArmed || !gFeedLowArmed;
    gPotionEmptyArmed = true;
    gCustomLowArmed = true;
    gCustom2LowArmed = true;
    gFeedLowArmed = true;
    if (any) {
        runtime::LogI("AutoSupply", "低库存触发已重开（%s）", why && why[0] ? why : "trip_done");
    }
}

bool MapMatchesTarget(const char* target) {
    if (!target || !target[0]) return false;
    const int curId = ports::travel::CurrentMapId();
    char curIdStr[32]{};
    if (curId > 0) snprintf(curIdStr, sizeof(curIdStr), "%d", curId);
    if (curIdStr[0] && _stricmp(curIdStr, target) == 0) return true;
    const std::string key = ports::travel::CurrentMapKey();
    if (!key.empty() && _stricmp(key.c_str(), target) == 0) return true;
    travel::Snapshot snap{};
    if (travel::QuerySnapshot(snap) && snap.curMap[0] && _stricmp(snap.curMap, target) == 0)
        return true;
    return false;
}

bool FillCurrentMapName(char* out, size_t outSz) {
    if (!out || outSz < 2) return false;
    out[0] = 0;
    const int id = ports::travel::CurrentMapId();
    if (id > 0) {
        snprintf(out, outSz, "%d", id);
        return true;
    }
    const std::string key = ports::travel::CurrentMapKey();
    if (!key.empty()) {
        strncpy_s(out, outSz, key.c_str(), _TRUNCATE);
        return true;
    }
    return false;
}

bool ResolveShopTarget(char* msg, size_t msgSz);

// 回城卷落地后：若 cur→店 比 farm→店 更远（或不可达），就地重寻店；本趟不再用卷。
void ReplanAfterScrollLand(const char* curMap) {
    gTriedScroll = true;
    gScrollPendingLand = false;
    gPreferDirect = true;
    if (!curMap || !curMap[0] || !gShopMap[0]) return;

    const int hopsCur = travel::PathHopCount(curMap, gShopMap);
    const int hopsFarm =
        gLastFarmMap[0] ? travel::PathHopCount(gLastFarmMap, gShopMap) : -1;
    runtime::LogI("AutoSupply", "scroll land cur=%s shop=%s cur→shop=%d farm→shop=%d", curMap,
                  gShopMap, hopsCur, hopsFarm);

    // 已到店图：上层 Tick 会进 OpeningShop
    if (MapMatchesTarget(gShopMap)) return;

    const bool worseThanFarm =
        hopsCur < 0 || (hopsFarm >= 0 && hopsCur > hopsFarm);
    if (!worseThanFarm) return;
    if (gCfg.shopMapName[0]) {
        runtime::LogW("AutoSupply", "scroll land worse but shopMap locked=%s → direct walk",
                      gShopMap);
        return;
    }

    char oldShop[64]{};
    strncpy_s(oldShop, gShopMap, _TRUNCATE);
    char msg[96]{};
    if (!ResolveShopTarget(msg, sizeof(msg)) || !gShopMap[0]) {
        strncpy_s(gShopMap, oldShop, _TRUNCATE);
        runtime::LogW("AutoSupply", "scroll land replan fail, keep shop=%s", gShopMap);
        return;
    }
    if (_stricmp(oldShop, gShopMap) != 0) {
        runtime::LogW("AutoSupply", "scroll land replan %s → %s (%s)", oldShop, gShopMap,
                      msg[0] ? msg : "");
        Publish(notify::NotificationKind::Info, "auto-supply-replan", "回城后改就近店", gShopMap);
    }
}

void RememberFarmMapInternal(bool allowTownOverwrite) {
    const int id = ports::travel::CurrentMapId();
    char buf[64]{};
    if (id > 0) {
        snprintf(buf, sizeof(buf), "%d", id);
    } else {
        const std::string key = ports::travel::CurrentMapKey();
        if (!key.empty()) strncpy_s(buf, key.c_str(), _TRUNCATE);
    }
    if (!buf[0]) return;
    if (!allowTownOverwrite && IsTownMapName(buf)) {
        runtime::LogI("AutoSupply", "skip farm map (town) %s", buf);
        return;
    }
    strncpy_s(gLastFarmMap, buf, _TRUNCATE);
}

bool PredictTownOutdoor(const char* farmMap, char* out, size_t outSz) {
    // 经典版回家卷=最近主城：委托学习图 hop（勿用前缀截断冒充）。
    return travel::PredictReturnScrollTownOutdoor(farmMap, out, outSz);
}

bool ShouldPreferDirectShop(const char* farmMap, const char* shopMap) {
    if (!farmMap || !farmMap[0] || !shopMap || !shopMap[0]) return false;
    if (_stricmp(farmMap, shopMap) == 0) return true;
    char town[16]{};
    if (!PredictTownOutdoor(farmMap, town, sizeof(town))) return false;
    const int hopsFarmShop = travel::PathHopCount(farmMap, shopMap);
    const int hopsTownShop = travel::PathHopCount(town, shopMap);
    if (hopsFarmShop < 0) return false;  // 直达不可达 → 仍尝试回城卷
    if (hopsTownShop < 0) return false;  // 估价不全 → 保守用卷
    runtime::LogI("AutoSupply",
                  "scroll-vs-direct farm=%s shop=%s town=%s farm→shop=%d town→shop=%d → %s",
                  farmMap, shopMap, town, hopsFarmShop, hopsTownShop,
                  hopsFarmShop < hopsTownShop ? "direct" : "scroll");
    return hopsFarmShop < hopsTownShop;
}

bool ResolveShopTarget(char* msg, size_t msgSz) {
    gShopMap[0] = 0;
    gResolvedNpc[0] = 0;
    if (gCfg.shopMapName[0]) {
        strncpy_s(gShopMap, gCfg.shopMapName, _TRUNCATE);
        if (msg && msgSz) snprintf(msg, msgSz, "使用配置店图 %s", gShopMap);
        return true;
    }
    // 寻店只为就近「能卖」：不关心货架有没有补给项（店内有则买、无则跳过，由用户自选品类）。
    std::string npc, shopId, mapName;
    int mapId = 0;
    const char* excl = gShopExclude[0] ? gShopExclude : nullptr;
    const bool ok = shop::ResolveShopNpcForSell(npc, shopId, mapName, mapId, excl);
    if (!ok || mapName.empty()) {
        if (msg && msgSz) snprintf(msg, msgSz, "自动寻店失败（无商店种子/不可达）");
        return false;
    }
    strncpy_s(gShopMap, mapName.c_str(), _TRUNCATE);
    if (!npc.empty()) strncpy_s(gResolvedNpc, npc.c_str(), _TRUNCATE);
    if (msg && msgSz) snprintf(msg, msgSz, "自动寻店 %s npc=%s", gShopMap, gResolvedNpc);
    return true;
}

int ParseItemCode(const char* code) {
    if (!code || !code[0]) return 0;
    return atoi(code);
}

// 回家卷軸：主码 2030000 + 同名备用 2030059（离线 item_catalog）。
// BIN b19da8：PortalScroll 已换图但 qty 读 -1 时，consumable 现认 map_changed；此处再兜一层，
// 避免误 try next / 误 walk，并防止第二张卷在主城再发一次。
bool TryUseReturnScroll(consumable::FindResult& outFr, int& outUsedId) {
    outFr = {};
    outUsedId = 0;
    const char* codes[] = {
        xcat::kAutoSupplyDefaultReturnScrollCode,
        xcat::kAutoSupplyAltReturnScrollCode,
    };
    char mapAtStart[64]{};
    FillCurrentMapName(mapAtStart, sizeof(mapAtStart));
    bool anyValid = false;
    for (const char* code : codes) {
        const int id = ParseItemCode(code);
        if (id <= 0) {
            runtime::LogW("AutoSupply", "用回城卷 bad_code raw=%s", code ? code : "(null)");
            continue;
        }
        anyValid = true;
        consumable::FindResult fr{};
        if (consumable::FindAndUseByItemId(id, fr)) {
            outFr = fr;
            outUsedId = id;
            runtime::LogI("AutoSupply", "用回城卷 ok id=%d pos=%d qty=%d", id, fr.pos, fr.qty);
            return true;
        }
        // FindAndUse 报 fail 但图已变：卷已生效，勿试下一码。
        char cur[64]{};
        if (FillCurrentMapName(cur, sizeof(cur)) && mapAtStart[0] && cur[0] &&
            _stricmp(cur, mapAtStart) != 0) {
            outFr = fr;
            outUsedId = id;
            runtime::LogW("AutoSupply",
                          "用回城卷 false_fail id=%d but map %s→%s — treat ok", id, mapAtStart,
                          cur);
            return true;
        }
        runtime::LogW("AutoSupply", "用回城卷 fail id=%d → try next", id);
    }
    if (!anyValid) {
        runtime::LogW("AutoSupply", "用回城卷 fail bad_code (no valid id)");
    } else {
        runtime::LogW("AutoSupply", "用回城卷 fail tried=%s,%s → walk",
                      xcat::kAutoSupplyDefaultReturnScrollCode,
                      xcat::kAutoSupplyAltReturnScrollCode);
    }
    return false;
}

int CountConsume(int itemId) {
    if (itemId <= 0) return 0;
    bool present = false;
    int count = 0;
    if (!shop::QueryItemPresent(kInvConsume, itemId, present, count)) return 0;
    return present ? count : 0;
}

void StartReturnOrDone() {
    NotifyIfStillLowAfterTrip();
    RearmLowStockLatchesAfterTrip("supply_done");
    gPendingReturnFarm = gLastFarmMap[0] != 0;
    PublishStatusIni();
    if (!gLastFarmMap[0]) {
        ResumeSystems();
        ClearTripTravelArm();
        Publish(notify::NotificationKind::Success, "auto-supply-done", "补给完成",
                "未记录挂机图，请手动返回。");
        gManualTrip = false;
        gPendingReturnFarm = false;
        gCooldownUntil = GetTickCount() + kCooldownMs;
        Enter(Phase::Cooldown, "完成（无挂机图）");
        return;
    }
    if (MapMatchesTarget(gLastFarmMap)) {
        ResumeSystems();
        ClearTripTravelArm();
        gPendingReturnFarm = false;
        PublishStatusIni();
        Publish(notify::NotificationKind::Success, "auto-supply-done", "补给完成",
                "已在挂机图，继续挂机。");
        gManualTrip = false;
        gCooldownUntil = GetTickCount() + kCooldownMs;
        Enter(Phase::Cooldown, "完成已在挂机图");
        return;
    }
    // 店窗未关就 DirectEnter → BIN: out00 fake_fire_stop（对照枫星必须先关店）
    bool shopOpen = false;
    if (shop::ShopReady(shopOpen) && shopOpen) {
        gCloseShopAttempts = 0;
        gLastCloseShopAttempt = 0;
        Enter(Phase::ClosingShop, "关闭商店…");
        return;
    }
    ArmTripStartCool(GetTickCount(), "return_after_supply");
    gReturnStableSince = 0;
    Enter(Phase::Returning, "返回挂机图…");
}

void BeginReturningToFarm(const char* msg) {
    if (!gLastFarmMap[0]) return;
    gReturnStableSince = 0;
    bool shopOpen = false;
    if (shop::ShopReady(shopOpen) && shopOpen) {
        gCloseShopAttempts = 0;
        gLastCloseShopAttempt = 0;
        gPendingReturnFarm = true;
        Enter(Phase::ClosingShop, "关闭商店…");
        return;
    }
    ArmTripStartCool(GetTickCount(), "return_farm");
    Enter(Phase::Returning, msg && msg[0] ? msg : "返回挂机图…");
}

bool BeginBuying() {
    gBuyStep = BuyStep::ReturnScroll;
    gBuySlot = BuySlot::Hp;
    gBuyNeed = 0;
    gActiveBuyId = 0;
    gBuyConfirmBefore = 0;
    gBuyConfirmExpect = 0;
    gBuyConfirmSince = 0;
    gBuyNoGainStreak = 0;
    gBuySoftBatchCap = 0;
    gMesoBudgetOverride = -1;
    gScrollJustBoughtPrice = 0;
    gMesoBeforeScrollBuy = -1;
    gLastBuyAttempt = 0;
    memset(gPlannedNeed, 0, sizeof(gPlannedNeed));
    memset(gPlannedPrice, 0, sizeof(gPlannedPrice));
    Enter(Phase::Buying, "补货中…");
    return true;
}

void PlanRefillsWithMeso() {
    memset(gPlannedNeed, 0, sizeof(gPlannedNeed));
    memset(gPlannedPrice, 0, sizeof(gPlannedPrice));
    gRefillStockMiss = 0;
    struct Want {
        int slot = -1;
        int id = 0;
        int need = 0;
        int price = 0;
    };
    Want wants[kBuySlotCount]{};
    int n = 0;
    auto push = [&](BuySlot slot, bool en, const char* code, int buyTo) {
        if (!en || buyTo <= 0 || n >= kBuySlotCount) return;
        const int id = ParseItemCode(code);
        if (id <= 0) return;
        const int have = CountConsume(id);
        if (have >= buyTo) return;
        bool inShop = false;
        int price = 0;
        if (!shop::QueryShopBuyOffer(id, inShop, price) || !inShop || price <= 0) {
            ++gRefillStockMiss;
            // 首次 miss：dump 买栏，区分「真没货」vs「读栏空/错价」
            if (gRefillStockMiss == 1) (void)shop::LogBuyShelfSnapshot(id);
            runtime::LogW("AutoSupply", "补货跳过 slot=%d id=%d：店内无货或无价 inShop=%d price=%d",
                          static_cast<int>(slot), id, inShop ? 1 : 0, price);
            return;
        }
        wants[n].slot = static_cast<int>(slot);
        wants[n].id = id;
        wants[n].need = buyTo - have;
        wants[n].price = price;
        ++n;
    };
    push(BuySlot::Hp, gCfg.refillHpEnabled != 0, gCfg.refillHpCode, gCfg.refillHpBuyTo);
    push(BuySlot::Mp, gCfg.refillMpEnabled != 0, gCfg.refillMpCode, gCfg.refillMpBuyTo);
    push(BuySlot::Custom, gCfg.refillCustomEnabled != 0, gCfg.refillCustomCode,
         gCfg.refillCustomBuyTo);
    push(BuySlot::Custom2, gCfg.refillCustom2Enabled != 0, gCfg.refillCustom2Code,
         gCfg.refillCustom2BuyTo);
    push(BuySlot::Feed, gCfg.refillFeedEnabled != 0, gCfg.refillFeedCode, gCfg.refillFeedBuyTo);
    if (gCfg.refillFeedEnabled) {
        const int primary = ParseItemCode(gCfg.refillFeedCode);
        if (!(primary > 0 && CountConsume(primary) >= gCfg.refillFeedBuyTo)) {
            // only add alt if primary not already planned enough
            bool hasPrimary = false;
            for (int i = 0; i < n; ++i)
                if (wants[i].slot == static_cast<int>(BuySlot::Feed)) hasPrimary = true;
            if (!hasPrimary)
                push(BuySlot::FeedAlt, true, xcat::kAutoSupplyDefaultRefillFeedAltCode,
                     gCfg.refillFeedBuyTo);
        }
    }

    if (n == 0) {
        runtime::LogI("AutoSupply", "refill plan empty（无启用项/已达标/店内无货）");
        return;
    }

    long long totalCost = 0;
    for (int i = 0; i < n; ++i) totalCost += 1LL * wants[i].need * wants[i].price;

    int64_t mesoRaw = shop::QueryMeso();
    int64_t meso = mesoRaw;
    if (gMesoBudgetOverride >= 0) {
        meso = gMesoBudgetOverride;
        gMesoBudgetOverride = -1;
    } else if (gScrollJustBoughtPrice > 0 && gMesoBeforeScrollBuy >= 0 && mesoRaw >= 0) {
        const int64_t expectAfter =
            gMesoBeforeScrollBuy >= gScrollJustBoughtPrice
                ? gMesoBeforeScrollBuy - static_cast<int64_t>(gScrollJustBoughtPrice)
                : 0;
        // Money 仍明显高于「买卷后应有值」→ 视为未扣，按 expectAfter 做比例
        if (mesoRaw > expectAfter + 50) {
            runtime::LogW("AutoSupply",
                          "refill meso stale raw=%lld beforeScroll=%lld scroll=%d → budget=%lld",
                          static_cast<long long>(mesoRaw),
                          static_cast<long long>(gMesoBeforeScrollBuy), gScrollJustBoughtPrice,
                          static_cast<long long>(expectAfter));
            meso = expectAfter;
        }
        gScrollJustBoughtPrice = 0;
        gMesoBeforeScrollBuy = -1;
    }

    double scale = 1.0;
    if (meso >= 0 && totalCost > meso && totalCost > 0)
        scale = static_cast<double>(meso) / static_cast<double>(totalCost);

    // 按比例后再用「剩余预算」串行截断，避免多槽合计仍超金币
    int64_t budget = meso >= 0 ? meso : 0;
    for (int i = 0; i < n; ++i) {
        int need = static_cast<int>(wants[i].need * scale);
        if (need < 0) need = 0;
        if (scale < 1.0 && need == 0 && wants[i].need > 0 && budget >= wants[i].price) need = 1;
        while (need > 0 && 1LL * need * wants[i].price > budget) --need;
        if (wants[i].slot >= 0 && wants[i].slot < kBuySlotCount) {
            gPlannedNeed[wants[i].slot] = need;
            gPlannedPrice[wants[i].slot] = wants[i].price;
        }
        if (need > 0 && wants[i].price > 0) budget -= 1LL * need * wants[i].price;
    }
    runtime::LogI("AutoSupply",
                  "refill plan mesoRaw=%lld budget=%lld totalCost=%lld scale=%.3f n=%d",
                  static_cast<long long>(mesoRaw), static_cast<long long>(meso), totalCost, scale,
                  n);
}

bool NextBuyTarget(int& outId, int& outNeed, const char*& outLabel) {
    outId = 0;
    outNeed = 0;
    outLabel = "";
    while (gBuySlot != BuySlot::Done) {
        const int slotIdx = static_cast<int>(gBuySlot);
        const char* code = nullptr;
        const char* label = "";
        switch (gBuySlot) {
        case BuySlot::Hp:
            code = gCfg.refillHpCode;
            label = "补红";
            break;
        case BuySlot::Mp:
            code = gCfg.refillMpCode;
            label = "补蓝";
            break;
        case BuySlot::Custom:
            code = gCfg.refillCustomCode;
            label = "补自定义";
            break;
        case BuySlot::Custom2:
            code = gCfg.refillCustom2Code;
            label = "补自定义2";
            break;
        case BuySlot::Feed:
            code = gCfg.refillFeedCode;
            label = "补饲料";
            break;
        case BuySlot::FeedAlt:
            code = xcat::kAutoSupplyDefaultRefillFeedAltCode;
            label = "补饲料(备选)";
            break;
        default:
            gBuySlot = BuySlot::Done;
            return false;
        }
        gBuySlot = static_cast<BuySlot>(slotIdx + 1);
        int need = (slotIdx >= 0 && slotIdx < kBuySlotCount) ? gPlannedNeed[slotIdx] : 0;
        if (need <= 0) continue;
        const int id = ParseItemCode(code);
        if (id <= 0) continue;
        outId = id;
        outNeed = need;
        outLabel = label;
        return true;
    }
    return false;
}

void TickIdle(DWORD now) {
    // 未就绪时保留 gTripReq，勿 exchange 丢单（BIN：点「立即一趟」被吃掉）
    // 手动一趟必须优先于 pendingReturn：BIN「每次第一下无效」=
    // 续跑回挂机抢在卖装指令前，gTripReq 干等到回图结束才执行。
    if (gTripReq.load(std::memory_order_acquire)) {
        if (char_boot::IsBusy()) {
            gTripReq.store(false, std::memory_order_release);
            Publish(notify::NotificationKind::Warning, "auto-supply-trip", "无法启动",
                    "起号进行中");
            return;
        }
        if (!ports::world::IsPlayReady() || sellbag::IsBusy() || travel::IsActive()) return;
        char msg[96]{};
        if (!ResolveShopTarget(msg, sizeof(msg))) {
            gTripReq.store(false, std::memory_order_release);
            Publish(notify::NotificationKind::Warning, "auto-supply-trip", "无法启动",
                    msg[0] ? msg : "自动寻店失败；可手填杂货地图");
            return;
        }
        gTripReq.store(false, std::memory_order_release);
        // 开趟优先：丢掉挂起的一键充，避免回 Idle 后迟到弹「请先开店」
        gRechargeReq.store(false, std::memory_order_release);
        // 用户显式开趟：取消崩溃续跑，避免先回挂机再卖
        if (gPendingReturnFarm) {
            runtime::LogI("AutoSupply", "manual trip overrides pendingReturn farm=%s",
                          gLastFarmMap[0] ? gLastFarmMap : "-");
            gPendingReturnFarm = false;
            PublishStatusIni();
        }
        gManualTrip = true;
        gShopExclude[0] = 0;
        gShopStockReroute = 0;
        gCooldownUntil = 0;
        travel::RequestStop();  // 清掉上次 already_there 等陈旧 failKind
        runtime::LogI("AutoSupply", "手动一趟 shop=%s (%s)", gShopMap, msg);
        Publish(notify::NotificationKind::Info, "auto-supply-trip", "补给已接单",
                "先开趟冷却，再赶路卖装");
        Enter(Phase::Pause, "手动补给：停手并记下挂机图…");
        return;
    }

    // 一键充飞镖：需已开店；不赶路、不关店、不回城（与一键卖装同语义）。
    if (gRechargeReq.load(std::memory_order_acquire)) {
        if (!ports::world::IsPlayReady() || sellbag::IsBusy() || travel::IsActive()) return;
        gRechargeReq.store(false, std::memory_order_release);
        bool ready = false;
        if (!shop::ShopReady(ready) || !ready) {
            SetMsg("请先打开 NPC 商店");
            Publish(notify::NotificationKind::Warning, "auto-supply-charge", "请先打开 NPC 商店",
                    "打开杂货店后再点一键充值飞镖。");
            PublishStatusIni();
            runtime::LogW("AutoSupply", "一键充飞镖：未开店，拒绝");
            return;
        }
        gLastBuyAttempt = 0;
        runtime::LogI("AutoSupply", "一键充飞镖开始");
        shop::ResetChargeSession();
        Enter(Phase::Charging, "飞镖充值中…");
        return;
    }

    // 崩溃/踢线后续：优先回挂机图（无手动指令时）。
    // BIN 16:11：enabled=0 时仍恢复 pendingReturn；续跑不依赖自动补给开关。
    if (gPendingReturnFarm && gLastFarmMap[0]) {
        if (!ports::world::IsPlayReady() || sellbag::IsBusy() || travel::IsActive()) {
            static DWORD sLastPendingWaitLog = 0;
            if (!sLastPendingWaitLog || now - sLastPendingWaitLog >= 5000) {
                sLastPendingWaitLog = now;
                runtime::LogI("AutoSupply",
                              "pendingReturn wait play=%d sellBusy=%d travel=%d farm=%s",
                              ports::world::IsPlayReady() ? 1 : 0, sellbag::IsBusy() ? 1 : 0,
                              travel::IsActive() ? 1 : 0, gLastFarmMap);
            }
            return;  // 等待时别掉进自动装备触发
        }
        if (MapMatchesTarget(gLastFarmMap)) {
            gPendingReturnFarm = false;
            PublishStatusIni();
            runtime::LogI("AutoSupply", "pendingReturn clear already_on_farm %s", gLastFarmMap);
        } else {
            PauseSystems();
            BeginReturningToFarm("续跑：返回挂机图…");
            return;
        }
    }

    // 绑定药 ID 刷新不依赖补给开关：只要进图 Idle，就写 status 给 GUI 对齐补红/蓝。
    // 与 kBagPollMs 拆开：面板跟手用短间隔；缺药开趟仍 2.5s 节流。
    if (ports::world::IsPlayReady()) {
        static DWORD sLastBoundPeek = 0;
        if (!sLastBoundPeek || now - sLastBoundPeek >= kBoundPeekMs) {
            sLastBoundPeek = now;
            int hpId = 0, mpId = 0;
            const bool hpOk = consumable::PeekBoundPotionItemId(true, hpId);
            const bool mpOk = consumable::PeekBoundPotionItemId(false, mpId);
            const int nextHp = hpOk ? hpId : 0;
            const int nextMp = mpOk ? mpId : 0;
            if (nextHp != gBoundHpItemId || nextMp != gBoundMpItemId) {
                runtime::LogI("AutoSupply", "bound potion id hp=%d→%d mp=%d→%d", gBoundHpItemId,
                              nextHp, gBoundMpItemId, nextMp);
                gBoundHpItemId = nextHp;
                gBoundMpItemId = nextMp;
                PublishStatusIni();
            }
        }
    }

    if (!gDesired.load()) return;
    if (now < gCooldownUntil) return;
    if (gLastBagPoll && now - gLastBagPoll < kBagPollMs) return;
    gLastBagPoll = now;

    if (!ports::world::IsPlayReady()) return;
    if (char_boot::IsBusy()) return;
    if (sellbag::IsBusy() || travel::IsActive()) return;

    static int sPotionLowStreak = 0;
    static int sCustomLowStreak = 0;
    static int sCustom2LowStreak = 0;
    static int sFeedLowStreak = 0;
    char potionWhy[96]{};
    char customWhy[96]{};
    char custom2Why[96]{};
    char feedWhy[96]{};
    const bool potionLow = PotionLowTriggerMet(potionWhy, sizeof(potionWhy));
    const bool customLow = CustomLowTriggerMet(customWhy, sizeof(customWhy));
    const bool custom2Low = Custom2LowTriggerMet(custom2Why, sizeof(custom2Why));
    const bool feedLow = FeedLowTriggerMet(feedWhy, sizeof(feedWhy));
    auto streak2 = [](bool met, int& streak) -> bool {
        if (!met) {
            streak = 0;
            return false;
        }
        return ++streak >= 2;
    };
    const bool potionFire = streak2(potionLow, sPotionLowStreak);
    const bool customFire = streak2(customLow, sCustomLowStreak);
    const bool custom2Fire = streak2(custom2Low, sCustom2LowStreak);
    const bool feedFire = streak2(feedLow, sFeedLowStreak);
    // 任一还在攒 streak：等下一拍（已确认的也一起等，避免半拍开火）
    if ((potionLow && !potionFire) || (customLow && !customFire) || (custom2Low && !custom2Fire) ||
        (feedLow && !feedFire))
        return;

    const bool sellTriggerOn =
        (gCfg.enabled != 0) || (gCfg.autoSellOnBagFullEnabled != 0);
    int used = 0, cap = 0;
    const bool equipMet = sellTriggerOn && EquipTriggerMet(used, cap);
    if (!potionFire && !customFire && !custom2Fire && !feedFire && !equipMet) return;

    char msg[96]{};
    if (!ResolveShopTarget(msg, sizeof(msg))) {
        SetMsg(msg[0] ? msg : "自动寻店失败");
        return;
    }
        gShopExclude[0] = 0;
        gShopStockReroute = 0;
        // 并发触发必须全部闭锁：旧 if/else 只关一路，另一路仍 armed 会冷却后空转第二趟。
        const char* pauseMsg = "停手并记下挂机图…";
    if (potionFire || customFire || custom2Fire || feedFire) {
        if (potionFire) {
            gPotionEmptyArmed = false;
            runtime::LogI("AutoSupply", "缺药触发 %s → shop=%s（已闭锁）",
                          potionWhy[0] ? potionWhy : "-", gShopMap);
        }
        if (customFire) {
            gCustomLowArmed = false;
            runtime::LogI("AutoSupply", "自定义低库存触发 %s → shop=%s（已闭锁）",
                          customWhy[0] ? customWhy : "-", gShopMap);
        }
        if (custom2Fire) {
            gCustom2LowArmed = false;
            runtime::LogI("AutoSupply", "自定义2低库存触发 %s → shop=%s（已闭锁）",
                          custom2Why[0] ? custom2Why : "-", gShopMap);
        }
        if (feedFire) {
            gFeedLowArmed = false;
            runtime::LogI("AutoSupply", "饲料低库存触发 %s → shop=%s（已闭锁）",
                          feedWhy[0] ? feedWhy : "-", gShopMap);
        }
        if (potionFire && !customFire && !custom2Fire && !feedFire) {
            Publish(notify::NotificationKind::Info, "auto-supply-potion", "缺药自动补",
                    "绑定药水偏低，开始回城补给");
            pauseMsg = "缺药补给：停手并记下挂机图…";
        } else if (customFire && !potionFire && !custom2Fire && !feedFire) {
            Publish(notify::NotificationKind::Info, "auto-supply-custom", "自定义物品补给",
                    "自定义物品数量过低，开始回城补给");
            pauseMsg = "自定义低库存：停手并记下挂机图…";
        } else if (custom2Fire && !potionFire && !customFire && !feedFire) {
            Publish(notify::NotificationKind::Info, "auto-supply-custom2", "自定义2补给",
                    "自定义2数量过低，开始回城补给");
            pauseMsg = "自定义2低库存：停手并记下挂机图…";
        } else if (feedFire && !potionFire && !customFire && !custom2Fire) {
            Publish(notify::NotificationKind::Info, "auto-supply-feed", "饲料补给",
                    "饲料数量过低，开始回城补给");
            pauseMsg = "饲料低库存：停手并记下挂机图…";
        } else {
            Publish(notify::NotificationKind::Info, "auto-supply-multi", "低库存补给",
                    "多项库存偏低，开始回城补给");
            pauseMsg = "低库存补给：停手并记下挂机图…";
        }
    } else {
        runtime::LogI("AutoSupply", "装备触发 %d/%d thr=%d → shop=%s", used, cap, gEquipTrigger,
                      gShopMap);
    }
    sPotionLowStreak = 0;
    sCustomLowStreak = 0;
    sCustom2LowStreak = 0;
    sFeedLowStreak = 0;
    gManualTrip = false;
    gRechargeReq.store(false, std::memory_order_release);
    PublishStatusIni();
    Enter(Phase::Pause, pauseMsg);
}

void TickPause(DWORD now) {
    RememberFarmMapInternal(/*allowTown=*/false);
    if (!gLastFarmMap[0]) {
        // 已在城镇触发时允许记下当前图以免卡死
        RememberFarmMapInternal(/*allowTown=*/true);
    }
    if (!gLastFarmMap[0]) {
        FailTrip("无法记录挂机图");
        return;
    }
    ArmTripStartCool(now, "pause_to_town");
    gTriedScroll = false;
    gScrollTries = 0;
    gScrollAttemptAt = 0;
    gScrollRetryAfter = 0;
    gScrollPendingLand = false;
    gScrollMapAtUse[0] = 0;
    gPreferDirect = ShouldPreferDirectShop(gLastFarmMap, gShopMap);
    if (MapMatchesTarget(gShopMap)) {
        // 已在店图：冷却主要用于停战斗瞬移；对话不依赖 Doing
        Enter(Phase::OpeningShop, "已在店图，尝试对话开店");
        gWaitOpenNotified = 0;
        gLastTalkAttempt = 0;
        gTalkMissStreak = 0;
        gLastMenuAttempt = 0;
        gShopReadySince = 0;
        return;
    }
    travel::RequestStop();  // 进入赶路前清陈旧终态，避免首 tick 被 already_there 误杀
    Enter(Phase::GoingTown, "开趟冷却中…");
}

void TickGoingTown(DWORD now) {
    if (now - gPhaseSince > kGotoTimeoutMs) {
        FailTrip("前往店图超时");
        return;
    }
    if (MapMatchesTarget(gShopMap) && !travel::IsActive()) {
        ClearTripTravelArm();
        Enter(Phase::OpeningShop, "尝试对话开店");
        gWaitOpenNotified = 0;
        gLastTalkAttempt = 0;
        gTalkMissStreak = 0;
        gLastMenuAttempt = 0;
        gShopReadySince = 0;
        return;
    }

    // 开趟冷却可与落台并行；到期后仍须等站稳再发卷/RequestGoto。
    if (!TripTravelReady(now)) return;
    EnsureSafeLandIfAirborne(now);
    if (!WaitSafeLand(now)) return;

    if (!gPreferDirect && !gTriedScroll) {
        // 已成功用卷：等离开用卷前地图（或离开挂机图），落地后结束用卷并重估店。
        if (gScrollPendingLand) {
            char cur[64]{};
            const bool haveCur = FillCurrentMapName(cur, sizeof(cur));
            const bool leftUseMap =
                haveCur && gScrollMapAtUse[0] && _stricmp(cur, gScrollMapAtUse) != 0;
            // BIN 122ea3：用卷返回时 CurrentMap 已是落点，若误记 atUse=落点则 leftUseMap
            // 恒假；用「已离开挂机图」兜底，才能触发 replan、避免再耗一张卷。
            const bool leftFarm =
                haveCur && gLastFarmMap[0] && _stricmp(cur, gLastFarmMap) != 0;
            if (leftUseMap || leftFarm) {
                runtime::LogI("AutoSupply", "scroll landed atUse=%s farm=%s cur=%s",
                              gScrollMapAtUse[0] ? gScrollMapAtUse : "-",
                              gLastFarmMap[0] ? gLastFarmMap : "-", cur);
                ReplanAfterScrollLand(cur);
            } else if (now - gScrollAttemptAt >= kScrollWaitMs) {
                // 仍停在用卷前/挂机图：这次用卷未生效。
                runtime::LogW("AutoSupply", "scroll no map change after %ums tries=%d/%d cur=%s",
                              (unsigned)kScrollWaitMs, gScrollTries, kMaxScrollTries,
                              haveCur ? cur : "?");
                gScrollPendingLand = false;
                gScrollMapAtUse[0] = 0;
                if (gScrollTries >= kMaxScrollTries) {
                    gTriedScroll = true;
                    gPreferDirect = true;
                }
            } else {
                SetMsg("已用回城卷，等待换图…");
                return;
            }
        }

        if (!gTriedScroll && !gScrollPendingLand && !travel::IsActive()) {
            if (gScrollRetryAfter && static_cast<int>(now - gScrollRetryAfter) < 0) {
                SetMsg("回城卷未生效，稍后重试…");
                return;
            }
            gScrollRetryAfter = 0;
            // 必须在发包前记下地图：BIN 122ea3 里 ok 日志前 Fly 已到 102000000。
            char before[64]{};
            FillCurrentMapName(before, sizeof(before));
            consumable::FindResult fr{};
            int usedId = 0;
            if (TryUseReturnScroll(fr, usedId)) {
                gScrollAttemptAt = now;
                ++gScrollTries;
                gScrollPendingLand = true;
                strncpy_s(gScrollMapAtUse, before, _TRUNCATE);
                char cur[64]{};
                if (FillCurrentMapName(cur, sizeof(cur)) && before[0] &&
                    _stricmp(cur, before) != 0) {
                    runtime::LogI("AutoSupply", "scroll landed immediate %s → %s", before, cur);
                    ReplanAfterScrollLand(cur);
                } else {
                    SetMsg("已用回城卷…");
                }
                return;
            }
            // 双码都报 fail，但图已离开用卷前/挂机图：仍当卷已落地（consumable 漏判兜底）。
            char cur[64]{};
            const bool haveCur = FillCurrentMapName(cur, sizeof(cur));
            const bool leftBefore =
                haveCur && before[0] && cur[0] && _stricmp(cur, before) != 0;
            const bool leftFarm =
                haveCur && gLastFarmMap[0] && cur[0] && _stricmp(cur, gLastFarmMap) != 0;
            if (leftBefore || leftFarm) {
                runtime::LogW("AutoSupply",
                              "用回城卷 API fail but left map before=%s farm=%s cur=%s — treat land",
                              before[0] ? before : "-", gLastFarmMap[0] ? gLastFarmMap : "-",
                              cur);
                gScrollAttemptAt = now;
                ++gScrollTries;
                gScrollPendingLand = true;
                strncpy_s(gScrollMapAtUse, before[0] ? before : gLastFarmMap, _TRUNCATE);
                ReplanAfterScrollLand(cur);
                return;
            }
            // no_consume / not_found：同码可再试（不立刻 walk）；耗尽才贴门。
            ++gScrollTries;
            if (gScrollTries < kMaxScrollTries) {
                gScrollRetryAfter = now + kScrollRetryGapMs;
                runtime::LogW("AutoSupply",
                              "用回城卷硬失败，%ums 后重试 tries=%d/%d",
                              (unsigned)kScrollRetryGapMs, gScrollTries, kMaxScrollTries);
                SetMsg("回城卷未生效，稍后重试…");
                return;
            }
            runtime::LogW("AutoSupply", "用回城卷重试耗尽 tries=%d → walk", gScrollTries);
            gTriedScroll = true;
            gPreferDirect = true;
        }
    } else {
        gTriedScroll = true;
    }

    if (!travel::IsActive()) {
        travel::Snapshot snap{};
        if (travel::QuerySnapshot(snap) && TravelFailIsHard(snap.failKind)) {
            FailTrip(snap.lastMsg[0] ? snap.lastMsg : "赶路失败（可能跨板块）");
            return;
        }
        // AlreadyThere 等软终态：忽略并重新 goto（对照枫星不把 already 当 EndTrip）
        // 冷却窗已过才 goto；用 phaseSince 相对时间会在冷却期误判，改看 arm 已清
        if (now - gPhaseSince > 400) travel::RequestGoto(gShopMap);
    }
}

bool TryRerouteShopAfterOpenMiss(const char* why) {
    if (!gShopMap[0] || gCfg.shopMapName[0]) return false;
    strncpy_s(gShopExclude, gShopMap, _TRUNCATE);
    char msg[96]{};
    if (!ResolveShopTarget(msg, sizeof(msg)) || MapMatchesTarget(gShopMap)) return false;
    runtime::LogW("AutoSupply", "%s，改道 %s npc=%s", why ? why : "开店失败", gShopMap,
                  gResolvedNpc);
    gTalkMissStreak = 0;
    gWaitOpenNotified = 0;
    gLastTalkAttempt = 0;
    gLastMenuAttempt = 0;
    gShopReadySince = 0;
    gTriedScroll = true;
    gPreferDirect = true;
    Enter(Phase::GoingTown, "改道其他杂货店…");
    return true;
}

// 店已开但买栏没有目标药水/饲料：关店排除本店，改去下一家（BIN：勇士村店空计划空回）。
bool TryRerouteShopAfterStockMiss(const char* why) {
    if (!gShopMap[0] || gCfg.shopMapName[0]) return false;
    if (gShopStockReroute >= kMaxShopStockReroute) return false;
    strncpy_s(gShopExclude, gShopMap, _TRUNCATE);
    char msg[96]{};
    if (!ResolveShopTarget(msg, sizeof(msg)) || MapMatchesTarget(gShopMap)) return false;
    ++gShopStockReroute;
    runtime::LogW("AutoSupply", "%s（miss=%d reroute=%d），改道 %s npc=%s",
                  why ? why : "店内无目标货", gRefillStockMiss, gShopStockReroute, gShopMap,
                  gResolvedNpc);
    (void)shop::CloseShop();
    gTalkMissStreak = 0;
    gWaitOpenNotified = 0;
    gLastTalkAttempt = 0;
    gLastMenuAttempt = 0;
    gShopReadySince = 0;
    gTriedScroll = true;
    gPreferDirect = true;
    gLastBuyAttempt = 0;
    Enter(Phase::GoingTown, "店内无货，改道…");
    return true;
}

void TickOpeningShop(DWORD now) {
    // 已在店图开趟：Travel 到站后仍可能悬空；先请求落台再等站稳。
    EnsureSafeLandIfAirborne(now);
    if (!WaitSafeLand(now)) return;
    if (now - gPhaseSince > kWaitOpenTimeoutMs) {
        if (TryRerouteShopAfterOpenMiss("开店超时")) return;
        FailTrip("等待开店超时");
        return;
    }
    if (!gWaitOpenNotified || now - gWaitOpenNotified > 20000) {
        gWaitOpenNotified = now;
        Publish(notify::NotificationKind::Info, "auto-supply-open", "正在尝试打开 NPC 商店",
                "靠近杂货 NPC；自动对话并尝试点选「商店」菜单。");
        SetMsg("尝试对话开店…");
    }

    // 已有对话框时优先推进/点菜单（节流，避免每 tick 连点 Say）
    if (!gLastMenuAttempt || now - gLastMenuAttempt >= kMenuConfirmMs) {
        gLastMenuAttempt = now;
        (void)shop::TryConfirmShopScriptMenu();
    }

    bool ready = false;
    if (shop::ShopReady(ready) && ready) {
        if (!gShopReadySince) gShopReadySince = now;
        if (now - gShopReadySince < kShopReadySettleMs) {
            SetMsg("商店已开，稍候卖出…");
            return;
        }
        if (!sellbag::RequestSellQuiet(xcat::kSellbagBagAll)) {
            FailTrip("卖出排队失败");
            return;
        }
        gTalkMissStreak = 0;
        Enter(Phase::Selling, "自动卖出中…");
        return;
    }
    gShopReadySince = 0;

    if (!gLastTalkAttempt || now - gLastTalkAttempt >= kTalkRetryMs) {
        gLastTalkAttempt = now;
        const int tpl = gResolvedNpc[0] ? atoi(gResolvedNpc) : 0;
        const bool talked = shop::TryTalkNearestNpc(0.f, tpl);
        if (!talked) {
            ++gTalkMissStreak;
            // 与改版前一致：Talk 失败再用 FuncKey 兜底（经典版可远距开店）
            (void)shop::TryNpcTalkFuncKey();
            // BIN 4bb7ea：戶外吉姆卷落點對不上 → 勿乾等 90s，連 miss 後排除改道室內藥店
            if (tpl > 0 && gTalkMissStreak >= kTalkMissReroute &&
                TryRerouteShopAfterOpenMiss("开店找不到目标NPC")) {
                return;
            }
        } else {
            gTalkMissStreak = 0;
        }
        // Talk 后立刻再扫一次菜单（服端回包稍后；节流由 gLastMenuAttempt 管）
        gLastMenuAttempt = now;
        (void)shop::TryConfirmShopScriptMenu();
    }
}

void TickSelling(DWORD now) {
    if (now - gPhaseSince > kSellTimeoutMs) {
        FailTrip("自动卖出超时");
        return;
    }
    if (sellbag::IsBusy()) return;

    sellbag::Status st{};
    sellbag::GetStatus(st);
    if (st.state == 3u) {
        FailTrip(st.message[0] ? st.message : "卖出失败");
        return;
    }
    BeginBuying();
}

void TickBuyingReal(DWORD now) {
    if (now - gPhaseSince > kBuyTimeoutMs) {
        runtime::LogW("AutoSupply", "buy phase timeout → return");
        StartReturnOrDone();
        return;
    }
    // ConfirmBuy 要轮询到账；Charge 用更短间隔（见 Tick 内 kChargeRetryMs）
    if (gBuyStep != BuyStep::ConfirmBuy && gBuyStep != BuyStep::Charge) {
        if (gLastBuyAttempt && now - gLastBuyAttempt < kBuyRetryMs) return;
    }
    if (gBuyStep == BuyStep::Charge) {
        if (gLastBuyAttempt && now - gLastBuyAttempt < kChargeRetryMs) return;
    }

    bool ready = false;
    if (!shop::ShopReady(ready) || !ready) {
        if (gBuyStep == BuyStep::ReturnScroll || gBuyStep == BuyStep::Charge) {
            // 关店后无法补卷/充镖，继续回图
            StartReturnOrDone();
            return;
        }
        SetMsg("商店已关，跳过补货");
        StartReturnOrDone();
        return;
    }

    if (gBuyStep == BuyStep::ReturnScroll) {
        gLastBuyAttempt = now;
        const int scrollId = ParseItemCode(xcat::kAutoSupplyDefaultReturnScrollCode);
        bool inShop = false;
        int price = 0;
        gScrollJustBoughtPrice = 0;
        gMesoBeforeScrollBuy = -1;
        if (scrollId > 0 && shop::QueryShopBuyOffer(scrollId, inShop, price) && inShop) {
            std::string err;
            gMesoBeforeScrollBuy = shop::QueryMeso();
            if (!shop::BuyItem(scrollId, 1, err)) {
                if (err == "SHOP_BUSY") return;
                runtime::LogW("AutoSupply", "补回城卷 fail %s → skip", err.c_str());
                gMesoBeforeScrollBuy = -1;
            } else {
                gScrollJustBoughtPrice = price > 0 ? price : 0;
                runtime::LogI("AutoSupply", "补回城卷 ok price=%d mesoBefore=%lld", price,
                              static_cast<long long>(gMesoBeforeScrollBuy));
            }
        } else {
            runtime::LogI("AutoSupply", "店内无回城卷，跳过");
        }
        gBuyStep = BuyStep::Charge;
        return;
    }

    if (gBuyStep == BuyStep::Charge) {
        if (!gCfg.rechargeStarsEnabled) {
            gBuyStep = BuyStep::PlanRefills;
            return;
        }
        static bool sChargeSessArmed = false;
        static DWORD sChargeStaleSince = 0;
        static DWORD sChargeArmPhase = 0;
        if (sChargeArmPhase != gPhaseSince) {
            sChargeArmPhase = gPhaseSince;
            sChargeSessArmed = false;
            sChargeStaleSince = 0;
        }
        if (!sChargeSessArmed) {
            shop::ResetChargeSession();
            sChargeSessArmed = true;
            sChargeStaleSince = 0;
        }
        int charged = 0, skipMeso = 0, skipOther = 0;
        std::string err;
        (void)shop::RechargeShurikensInOpenShop(charged, skipMeso, skipOther, err);
        if (err == "SHOP_BUSY" || err == "LIST_STALE") {
            if (!sChargeStaleSince) sChargeStaleSince = now;
            if (now - sChargeStaleSince >= kBuyChargeStaleMaxMs) {
                runtime::LogW("AutoSupply",
                              "飞镖充值 LIST_STALE/BUSY 超时 %ums → 跳过继续补货",
                              static_cast<unsigned>(now - sChargeStaleSince));
                SetMsg("飞镖充值超时，继续补货");
                Publish(notify::NotificationKind::Warning, "auto-supply-charge", "飞镖充值超时",
                        "卖栏投影未就绪；已跳过充镖，继续买药。");
                sChargeSessArmed = false;
                sChargeStaleSince = 0;
                gBuyStep = BuyStep::PlanRefills;
                return;
            }
            // 不打节流戳：下拍立刻重试（切 TAB / 忙标记清）
            SetMsg("飞镖充值等待店务…");
            return;
        }
        sChargeStaleSince = 0;
        if (charged > 0) {
            gLastBuyAttempt = now;
            SetMsg("飞镖充值已发包，继续…");
            return;  // 下一拍再扫：忙清后充下一格 / 确认已满
        }
        sChargeSessArmed = false;
        if (skipMeso > 0) {
            SetMsg("飞镖充值金币不足，跳过");
            Publish(notify::NotificationKind::Warning, "auto-supply-charge", "飞镖充值金币不足",
                    "有未满飞镖但金币不够整格 Charge；继续补货。");
        } else if (err == "NO_SHOP" || err == "UNBOUND" || err == "NO_RPC" || err == "MAIN_TIMEOUT")
            SetMsg("飞镖充值失败，跳过");
        else
            SetMsg("飞镖充值处理完毕");
        gBuyStep = BuyStep::PlanRefills;
        return;
    }

    if (gBuyStep == BuyStep::PlanRefills) {
        PlanRefillsWithMeso();
        // 需要补的药/饲料店里全没有 → 改道，勿空趟回挂机（本机 03:18 勇士村复现）
        if (gRefillStockMiss > 0) {
            bool anyPlanned = false;
            for (int i = 0; i < kBuySlotCount; ++i) {
                if (gPlannedNeed[i] > 0) {
                    anyPlanned = true;
                    break;
                }
            }
            if (!anyPlanned && TryRerouteShopAfterStockMiss("补货规划店内无目标货")) return;
        }
        gBuySlot = BuySlot::Hp;
        gActiveBuyId = 0;
        gBuyNeed = 0;
        gBuyNoGainStreak = 0;
        gBuySoftBatchCap = 0;
        gBuyStep = BuyStep::BuyRefills;
        return;
    }

    if (gBuyStep == BuyStep::ConfirmBuy) {
        constexpr DWORD kBuyConfirmMinMs = 250;
        constexpr DWORD kBuyConfirmMaxMs = 1800;
        if (now - gBuyConfirmSince < kBuyConfirmMinMs) return;
        const int have = CountConsume(gActiveBuyId);
        const int gained = have - gBuyConfirmBefore;
        if (gained > 0) {
            gBuyNeed -= gained;
            if (gBuyNeed < 0) gBuyNeed = 0;
            gBuyNoGainStreak = 0;
            if (gBuySoftBatchCap > 0 && gained >= gBuyConfirmExpect) gBuySoftBatchCap = 0;
            runtime::LogI("AutoSupply", "buy confirm id=%d +%d have=%d needLeft=%d", gActiveBuyId,
                          gained, have, gBuyNeed);
            if (gBuyNeed <= 0) {
                gActiveBuyId = 0;
                gBuyNeed = 0;
            }
            gBuyStep = BuyStep::BuyRefills;
            gLastBuyAttempt = now;  // 下一拍再买，给 SHOP_BUSY 消散时间
            return;
        }
        if (now - gBuyConfirmSince < kBuyConfirmMaxMs) {
            SetMsg("等待入包…");
            return;
        }
        // 超时仍未到账：多半超堆叠/背包满/服拒；缩小单批重试，避免再发同样大包
        ++gBuyNoGainStreak;
        runtime::LogW("AutoSupply",
                      "buy no-gain id=%d expect=%d before=%d have=%d streak=%d", gActiveBuyId,
                      gBuyConfirmExpect, gBuyConfirmBefore, have, gBuyNoGainStreak);
        if (gBuyConfirmExpect > 1000) {
            gBuySoftBatchCap = 1000;
        } else if (gBuyConfirmExpect > 200) {
            gBuySoftBatchCap = 200;
        }
        if (gBuyNoGainStreak >= 3) {
            runtime::LogW("AutoSupply", "buy give-up id=%d after %d no-gain → next", gActiveBuyId,
                          gBuyNoGainStreak);
            gActiveBuyId = 0;
            gBuyNeed = 0;
            gBuyNoGainStreak = 0;
            gBuySoftBatchCap = 0;
        }
        gBuyStep = BuyStep::BuyRefills;
        gLastBuyAttempt = now;
        return;
    }

    if (gBuyStep == BuyStep::BuyRefills) {
        if (gActiveBuyId <= 0 || gBuyNeed <= 0) {
            int id = 0, need = 0;
            const char* label = "";
            if (!NextBuyTarget(id, need, label)) {
                StartReturnOrDone();
                return;
            }
            gActiveBuyId = id;
            gBuyNeed = need;
            gBuyNoGainStreak = 0;
            char msg[96]{};
            snprintf(msg, sizeof(msg), "%s #%d 还需%d", label, id, need);
            SetMsg(msg);
        }
        gLastBuyAttempt = now;
        // 协议 Encode2(nCount)，UI「补到」上限 9999；店侧 MaxSlot 在 BuyItem 再截。
        // SoftCap：超大批到账失败后的保守重试（日志曾见 cnt=3000 未入包）。
        constexpr int kBuyBatchMax = 9999;
        int batchMax = kBuyBatchMax;
        if (gBuySoftBatchCap > 0 && gBuySoftBatchCap < batchMax) batchMax = gBuySoftBatchCap;
        int batch = gBuyNeed > batchMax ? batchMax : gBuyNeed;
        // 买入前再按实时金币截断（规划后飞镖充值/读数滞后都可能让计划偏大）
        {
            bool inShop = false;
            int price = 0;
            if (shop::QueryShopBuyOffer(gActiveBuyId, inShop, price) && inShop && price > 0) {
                const int64_t mesoNow = shop::QueryMeso();
                if (mesoNow >= 0) {
                    const int afford = static_cast<int>(mesoNow / price);
                    if (afford <= 0) {
                        runtime::LogW("AutoSupply", "buy skip id=%d NO_MESO meso=%lld price=%d",
                                      gActiveBuyId, static_cast<long long>(mesoNow), price);
                        StartReturnOrDone();
                        return;
                    }
                    if (batch > afford) {
                        runtime::LogI("AutoSupply",
                                      "buy meso-afford clamp id=%d %d→%d meso=%lld price=%d",
                                      gActiveBuyId, batch, afford,
                                      static_cast<long long>(mesoNow), price);
                        batch = afford;
                    }
                }
            }
        }
        const int before = CountConsume(gActiveBuyId);
        std::string err;
        int bought = 0;
        if (!shop::BuyItem(gActiveBuyId, batch, err, &bought)) {
            if (err == "SHOP_BUSY") return;
            if (err == "NO_MESO") {
                runtime::LogW("AutoSupply", "buy NO_MESO → finish buying");
                StartReturnOrDone();
                return;
            }
            runtime::LogW("AutoSupply", "buy fail %s → next", err.c_str());
            gActiveBuyId = 0;
            gBuyNeed = 0;
            return;
        }
        const int expect = bought > 0 ? bought : batch;
        gBuyConfirmBefore = before;
        gBuyConfirmExpect = expect;
        gBuyConfirmSince = now;
        gBuyStep = BuyStep::ConfirmBuy;
        char msg[96]{};
        snprintf(msg, sizeof(msg), "买入 #%d x%d…", gActiveBuyId, expect);
        SetMsg(msg);
        return;
    }

    StartReturnOrDone();
}

void TickCharging(DWORD now) {
    // 一键充飞镖：店内循环 Charge，结束回 Idle（不关店、不回城）。
    static int sFired = 0;
    static DWORD sArmPhase = 0;
    if (sArmPhase != gPhaseSince) {
        sArmPhase = gPhaseSince;
        sFired = 0;
    }
    if (now - gPhaseSince > 60000) {
        if (sFired > 0) {
            SetMsg("飞镖充值完成");
            Publish(notify::NotificationKind::Info, "auto-supply-charge", "飞镖充值完成",
                    "已充部分飞镖；余下格投影未刷新已跳过。");
            Enter(Phase::Idle, "飞镖充值完成");
        } else {
            SetMsg("飞镖充值超时");
            Publish(notify::NotificationKind::Warning, "auto-supply-charge", "飞镖充值超时",
                    "商店可能卡住；可关店重开后再试。");
            Enter(Phase::Idle, "飞镖充值超时");
        }
        sFired = 0;
        return;
    }
    bool ready = false;
    if (!shop::ShopReady(ready) || !ready) {
        SetMsg("商店已关闭");
        Publish(notify::NotificationKind::Warning, "auto-supply-charge", "商店已关闭",
                "充飞镖中断；请重新开店后再点。");
        Enter(Phase::Idle, "商店已关闭");
        sFired = 0;
        return;
    }
    // 仅成功发包后限频；BUSY/STALE 不打戳，下拍（~200ms）即可重试
    if (gLastBuyAttempt && now - gLastBuyAttempt < kChargeRetryMs) return;

    int charged = 0, skipMeso = 0, skipOther = 0;
    std::string err;
    // 首拍常 LIST_STALE（切消耗 TAB）：同 tick 立刻再扫一次，省掉一整拍空等
    for (int pass = 0; pass < 2; ++pass) {
        charged = 0;
        skipMeso = 0;
        skipOther = 0;
        err.clear();
        (void)shop::RechargeShurikensInOpenShop(charged, skipMeso, skipOther, err);
        if (err == "LIST_STALE" && pass == 0) continue;
        break;
    }
    if (err == "SHOP_BUSY" || err == "LIST_STALE") {
        SetMsg("飞镖充值等待店务…");
        return;
    }
    if (charged > 0) {
        ++sFired;
        gLastBuyAttempt = now;
        SetMsg("飞镖充值已发包，继续…");
        return;  // 下一拍再扫下一格
    }
    if (skipMeso > 0) {
        SetMsg("飞镖充值金币不足");
        Publish(notify::NotificationKind::Warning, "auto-supply-charge", "飞镖充值金币不足",
                "有未满飞镖但金币不够 Charge。");
    } else if (err == "NO_SHOP" || err == "UNBOUND" || err == "NO_RPC" || err == "MAIN_TIMEOUT" ||
               err == "EXCEPTION") {
        SetMsg("飞镖充值失败");
        Publish(notify::NotificationKind::Warning, "auto-supply-charge", "飞镖充值失败",
                err.c_str());
    } else if (err == "NONE" || err.empty()) {
        SetMsg("飞镖充值完成");
        Publish(notify::NotificationKind::Info, "auto-supply-charge", "飞镖充值完成",
                sFired > 0 ? "飞镖已充完。" : "无可充飞镖，或已全部充满。");
    } else {
        SetMsg("飞镖充值完成");
        Publish(notify::NotificationKind::Info, "auto-supply-charge", "飞镖充值完成",
                "消耗栏可充手里剑已处理。");
    }
    sFired = 0;
    Enter(Phase::Idle, gStatus.message[0] ? gStatus.message : "飞镖充值完成");
}

void TickClosingShop(DWORD now) {
    if (now - gPhaseSince > 30000) {
        FailTrip("关店超时");
        return;
    }
    bool ready = false;
    if (!shop::ShopReady(ready) || !ready) {
        runtime::LogI("AutoSupply", "关店完成 attempts=%d", gCloseShopAttempts);
        gReturnStableSince = 0;
        ArmTripStartCool(now, "close_shop_return");
        Enter(Phase::Returning, "返回挂机图…");
        return;
    }
    if (gLastCloseShopAttempt && now - gLastCloseShopAttempt < kCloseShopRetryMs) return;
    gLastCloseShopAttempt = now;
    ++gCloseShopAttempts;
    const bool closed = shop::CloseShop();
    runtime::LogI("AutoSupply", "关店尝试 %d/%d closed=%d", gCloseShopAttempts,
                  kMaxCloseShopAttempts, closed ? 1 : 0);
    // 关不死就回图会带店窗假火（BIN fake_fire_stop）；宁可中止
    if (gCloseShopAttempts >= kMaxCloseShopAttempts) {
        FailTrip("关店失败，无法返回");
    }
}

void FinishReturnToFarm(DWORD now) {
    gReturnStableSince = 0;
    ClearTripTravelArm();
    ResumeSystems();
    gPendingReturnFarm = false;
    RearmLowStockLatchesAfterTrip("back_to_farm");
    PublishStatusIni();
    char body[160]{};
    snprintf(body, sizeof(body), "已返回 %s", gLastFarmMap);
    Publish(notify::NotificationKind::Success, "auto-supply-done", "补给完成", body);
    gManualTrip = false;
    gCooldownUntil = now + kCooldownMs;
    Enter(Phase::Cooldown, "已回挂机图");
}

// 到站后连续 PlayReady 才 Resume，避免卸图窗里恢复战斗/飞导致黑屏。
bool ReturnMapStable(DWORD now) {
    if (!MapMatchesTarget(gLastFarmMap) || travel::IsActive()) {
        gReturnStableSince = 0;
        return false;
    }
    if (!ports::world::IsPlayReady()) {
        gReturnStableSince = 0;
        SetMsg("回图稳图中…");
        return false;
    }
    if (!gReturnStableSince) {
        gReturnStableSince = now;
        SetMsg("回图稳图中…");
        runtime::LogI("AutoSupply", "return stable wait %ums map=%s", (unsigned)kReturnStableMs,
                      gLastFarmMap);
        return false;
    }
    if (now - gReturnStableSince < kReturnStableMs) {
        SetMsg("回图稳图中…");
        return false;
    }
    return true;
}

void TickReturning(DWORD now) {
    if (now - gPhaseSince > kReturnTimeoutMs) {
        FailTrip("返回挂机图超时");
        return;
    }
    if (ReturnMapStable(now)) {
        FinishReturnToFarm(now);
        return;
    }
    if (MapMatchesTarget(gLastFarmMap) && travel::IsActive()) {
        // Travel 仍在 arrive_settle：等它 Idle，不要抢跑 Resume
        SetMsg("回图稳图中…");
        return;
    }
    if (!TripTravelReady(now)) return;
    EnsureSafeLandIfAirborne(now);
    if (!WaitSafeLand(now)) return;
    if (!travel::IsActive()) {
        travel::Snapshot snap{};
        if (travel::QuerySnapshot(snap)) {
            if (TravelFailIsHard(snap.failKind)) {
                FailTrip(snap.lastMsg[0] ? snap.lastMsg : "回图赶路失败");
                return;
            }
            // 回图 already_there = 已在挂机图，仍走稳图再 Resume
            if (snap.failKind == travel::FailKind::AlreadyThere && MapMatchesTarget(gLastFarmMap)) {
                if (ReturnMapStable(now)) FinishReturnToFarm(now);
                return;
            }
        }
        if (!MapMatchesTarget(gLastFarmMap) && now - gPhaseSince > 800)
            travel::RequestGoto(gLastFarmMap);
    }
}

void TickCooldown(DWORD now) {
    // 冷却中点「立即一趟 / 回挂机 / 一键充飞镖」：立刻让出，勿干等 45s
    if (gTripReq.load(std::memory_order_acquire) || gReturnReq.load(std::memory_order_acquire) ||
        gRechargeReq.load(std::memory_order_acquire)) {
        RearmLowStockLatchesAfterTrip("cooldown_abort");
        gCooldownUntil = 0;
        Enter(Phase::Idle, "空闲");
        SetMsg("");
        return;
    }
    if (now >= gCooldownUntil) {
        RearmLowStockLatchesAfterTrip("cooldown_end");
        Enter(Phase::Idle, "空闲");
        SetMsg("");
    }
}

void TickManualCmds() {
    if (gAbortReq.exchange(false, std::memory_order_acq_rel)) {
        gRechargeReq.store(false, std::memory_order_release);
        if (gPhase == Phase::Charging) {
            // 一键充不关店、不进冷却；停手即回空闲
            Enter(Phase::Idle, gAbortWhy[0] ? gAbortWhy : "已停止充飞镖");
            return;
        }
        if (gPhase != Phase::Idle && gPhase != Phase::Cooldown) {
            FailTrip(gAbortWhy[0] ? gAbortWhy : "用户中止行程");
            return;
        }
        gPendingReturnFarm = false;
        PublishStatusIni();
    }

    if (gReturnReq.exchange(false, std::memory_order_acq_rel)) {
        if (gPhase == Phase::Charging) {
            gRechargeReq.store(false, std::memory_order_release);
            Enter(Phase::Idle, "已停止充飞镖");
        }
        if (gPhase != Phase::Idle && gPhase != Phase::Cooldown) {
            // 打断当前行程，改回图
            travel::RequestStop();
            sellbag::Abort("改回挂机图");
            if (!gLastFarmMap[0]) {
                Publish(notify::NotificationKind::Warning, "auto-supply-return", "回挂机图失败",
                        "尚无记录挂机图。");
                FailTrip("无挂机图");
            } else {
                PauseSystems();
                gPendingReturnFarm = true;
                BeginReturningToFarm("返回挂机图…");
            }
        } else if (!gLastFarmMap[0]) {
            Publish(notify::NotificationKind::Warning, "auto-supply-return", "回挂机图失败",
                    "尚无记录挂机图（需先跑过一次补给或开启挂机）。");
        } else {
            BeginReturningToFarm("返回挂机图…");
            Publish(notify::NotificationKind::Info, "auto-supply-return", "回挂机图", gLastFarmMap);
        }
    }
}

// 对照枫星：注入瞬间对齐磁盘 manualSeq，只忽略「加载前已存在」的旧命令。
// 注意：绝不能在「用户已经点过按钮之后」的首次 HotRead 里用当前磁盘 seq 做 bootstrap，
// 否则会把第一下写成的 seq 当成旧命令吞掉（BIN：第一次点「立即一趟」无效）。
void BootstrapManualSeq(uint32_t seqOnDisk, const char* via) {
    if (gManualSeqBootstrapped) return;
    gManualSeqBootstrapped = true;
    gHandledManualSeq = seqOnDisk;
    runtime::LogI("AutoSupply", "bootstrap manualSeq=%u via=%s (ignore pre-inject command)",
                  gHandledManualSeq, via ? via : "?");
}

bool DiskBusyState(uint32_t state) { return xcat::AutoSupplyStateIsBusy(state); }

void HotReadConfig() {
    xcat::AutoSupplyConfig cfg{};
    if (!xcat::ReadAutoSupply(runtime::GetBinDir(), cfg)) return;

    if (cfg.writeTickMs == gSeenCfgTick && gSeenCfgTick != 0) {
        // 同 tick 仍可能只改了 manualSeq（GUI 双写）；补抓命令游标（对照枫星）
        if (cfg.manualSeq != gCfg.manualSeq) {
            gCfg.manualSeq = cfg.manualSeq;
            gCfg.manualKind = cfg.manualKind;
            gCfg.enabled = cfg.enabled;
            runtime::LogI("AutoSupply", "manualSeq catch-up seq=%u kind=%u", gCfg.manualSeq,
                          gCfg.manualKind);
        }
    } else {
        gSeenCfgTick = cfg.writeTickMs;
        gCfg = cfg;
    }

    const bool on = (gCfg.enabled != 0) || (gCfg.autoSellOnBagFullEnabled != 0) ||
                    (gCfg.tripOnPotionEmpty != 0) || (gCfg.tripOnCustomLow != 0) ||
                    (gCfg.tripOnCustom2Low != 0) || (gCfg.tripOnFeedLow != 0);
    gDesired.store(on, std::memory_order_release);
    gEquipTrigger = gCfg.sellFreeSlotsAtOrBelow;

    // Init 已保证 bootstrapped。若仍未就绪：只能按 0 对齐，禁止用「当前磁盘 seq」收养，
    // 否则会把用户第一下刚写入的 manualTrip 当成 pre-inject 旧命令吞掉。
    if (!gManualSeqBootstrapped) BootstrapManualSeq(0, "reload-guard");

    if (gCfg.manualSeq != gHandledManualSeq) {
        gHandledManualSeq = gCfg.manualSeq;
        if (gCfg.manualKind == xcat::kAutoSupplyManualTrip) {
            gRechargeReq.store(false, std::memory_order_release);
            gTripReq.store(true, std::memory_order_release);
            runtime::LogI("AutoSupply", "accept manualTrip seq=%u", gHandledManualSeq);
        } else if (gCfg.manualKind == xcat::kAutoSupplyManualReturnFarm) {
            gRechargeReq.store(false, std::memory_order_release);
            gReturnReq.store(true, std::memory_order_release);
            runtime::LogI("AutoSupply", "accept manualReturn seq=%u", gHandledManualSeq);
        } else if (gCfg.manualKind == xcat::kAutoSupplyManualRechargeStars) {
            if (gPhase != Phase::Idle && gPhase != Phase::Cooldown) {
                SetMsg("忙碌中，无法充飞镖");
                Publish(notify::NotificationKind::Warning, "auto-supply-charge", "忙碌中",
                        "请先等卖装/补给结束，或点「停止动作」。");
                PublishStatusIni();
                runtime::LogW("AutoSupply", "reject manualRechargeStars seq=%u phase busy",
                              gHandledManualSeq);
            } else if (gTripReq.load(std::memory_order_acquire) ||
                       gReturnReq.load(std::memory_order_acquire)) {
                // 已有更高优先级手动指令排队：勿挂起充值以免行程后迟到开火
                SetMsg("已有其它补给指令，跳过充飞镖");
                Publish(notify::NotificationKind::Warning, "auto-supply-charge", "已有其它指令",
                        "请等当前一趟/回挂机完成后再充飞镖。");
                PublishStatusIni();
                runtime::LogW("AutoSupply",
                              "reject manualRechargeStars seq=%u trip/return pending",
                              gHandledManualSeq);
            } else {
                gRechargeReq.store(true, std::memory_order_release);
                runtime::LogI("AutoSupply", "accept manualRechargeStars seq=%u",
                              gHandledManualSeq);
            }
        } else if (gCfg.manualKind == xcat::kAutoSupplyManualStop) {
            gRechargeReq.store(false, std::memory_order_release);
            gDesired.store(false, std::memory_order_release);
            AbortTrip("用户停止动作");
            gCfg.enabled = 0;
            gCfg.autoSellOnBagFullEnabled = 0;
            gCfg.tripOnPotionEmpty = 0;
            gCfg.tripOnCustomLow = 0;
            gCfg.tripOnCustom2Low = 0;
            gCfg.tripOnFeedLow = 0;
            gCfg.manualKind = xcat::kAutoSupplyManualNone;
            gCfg.writeTickMs = GetTickCount64();
            (void)xcat::WriteAutoSupply(runtime::GetBinDir(), gCfg);
        }
    }
}

void Tick(DWORD now) {
    HotReadConfig();
    TickManualCmds();
    SyncStatus();

    switch (gPhase) {
    case Phase::Idle: TickIdle(now); break;
    case Phase::Pause: TickPause(now); break;
    case Phase::GoingTown: TickGoingTown(now); break;
    case Phase::OpeningShop: TickOpeningShop(now); break;
    case Phase::Selling: TickSelling(now); break;
    case Phase::Buying: TickBuyingReal(now); break;
    case Phase::Charging: TickCharging(now); break;
    case Phase::ClosingShop: TickClosingShop(now); break;
    case Phase::Returning: TickReturning(now); break;
    case Phase::Cooldown: TickCooldown(now); break;
    }
}

DWORD WINAPI Worker(LPVOID) {
    runtime::LogI("AutoSupply", "worker start");
    while (!gStop.load(std::memory_order_acquire)) {
        Tick(GetTickCount());
        Sleep(kPhaseTickMs);
    }
    ResumeSystems();
    runtime::LogI("AutoSupply", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    gPhase = Phase::Idle;
    gDesired.store(false);
    gSeenCfgTick = 0;
    gHandledManualSeq = 0;
    gManualSeqBootstrapped = false;
    gRechargeReq.store(false, std::memory_order_release);
    gShopMap[0] = 0;
    gShopExclude[0] = 0;
    gShopStockReroute = 0;
    gLastFarmMap[0] = 0;
    gAbortWhy[0] = 0;
    gPendingReturnFarm = false;
    SetMsg("");

    // 对照枫星：先恢复落盘挂机图/待回图，再吞旧 manualSeq
    {
        xcat::AutoSupplyStatus disk{};
        if (xcat::ReadAutoSupplyStatus(runtime::GetBinDir(), disk)) {
            if (disk.lastFarmMapName[0]) strncpy_s(gLastFarmMap, disk.lastFarmMapName, _TRUNCATE);
            gPendingReturnFarm = disk.pendingReturnFarm != 0;
            // 磁盘忙状态 + 有挂机图，即使 pending 位丢失也视为需续回
            if (DiskBusyState(disk.state) && gLastFarmMap[0] && !gPendingReturnFarm) {
                gPendingReturnFarm = true;
                runtime::LogW("AutoSupply",
                              "恢复：磁盘 state=%u 忙碌且 pending=0，补挂 pendingReturn farm=%s",
                              disk.state, gLastFarmMap);
            }
            runtime::LogI("AutoSupply", "恢复记忆 lastFarm=%s pendingReturn=%u diskState=%u",
                          gLastFarmMap[0] ? gLastFarmMap : "-", gPendingReturnFarm ? 1u : 0u,
                          disk.state);
            if (gPendingReturnFarm) {
                SetMsg("检测到中断的补给，将自动回挂机图");
            }
        }
        xcat::AutoSupplyConfig boot{};
        if (xcat::ReadAutoSupply(runtime::GetBinDir(), boot)) {
            gCfg = boot;
            gSeenCfgTick = boot.writeTickMs;
            const bool on = (gCfg.enabled != 0) || (gCfg.autoSellOnBagFullEnabled != 0) ||
                            (gCfg.tripOnPotionEmpty != 0) || (gCfg.tripOnCustomLow != 0) ||
                            (gCfg.tripOnCustom2Low != 0) || (gCfg.tripOnFeedLow != 0);
            gDesired.store(on, std::memory_order_release);
            BootstrapManualSeq(boot.manualSeq, "init");
        } else {
            // 尚无 [auto_supply] 时也必须先对齐：否则首次 HotRead 会用用户第一下的 seq 做
            // bootstrap，把「立即回城卖装一趟」吞成无效。
            BootstrapManualSeq(0, "init-empty");
        }
    }
    SyncStatus();
    PublishStatusIni();
}

void Shutdown() {
    StopWorker();
    gManualSeqBootstrapped = false;
    gSeenCfgTick = 0;
}

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    gThread.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

void SetDesired(bool on) {
    gDesired.store(on, std::memory_order_release);
    gCfg.enabled = on ? 1u : 0u;
    gCfg.autoSellOnBagFullEnabled = on ? 1u : 0u;
}

bool IsDesired() { return gDesired.load(std::memory_order_acquire); }

bool IsBusy() { return gPhase != Phase::Idle && gPhase != Phase::Cooldown; }

void RecordHangupFarmMap(const char* reason) {
    const int id = ports::travel::CurrentMapId();
    char buf[64]{};
    if (id > 0) snprintf(buf, sizeof(buf), "%d", id);
    else {
        const std::string key = ports::travel::CurrentMapKey();
        if (!key.empty()) strncpy_s(buf, key.c_str(), _TRUNCATE);
    }
    if (!buf[0]) return;
    if (IsTownMapName(buf)) {
        runtime::LogI("AutoSupply", "RecordHangupFarmMap skip town %s (%s)", buf,
                      reason ? reason : "");
        return;
    }
    strncpy_s(gLastFarmMap, buf, _TRUNCATE);
    runtime::LogI("AutoSupply", "RecordHangupFarmMap %s (%s)", gLastFarmMap,
                  reason ? reason : "");
    PublishStatusIni();
}

bool IsTownMapIdHeuristic(int mapId) {
    if (mapId <= 0) return false;
    return xcat::IsMapInfoTown(runtime::GetBinDir(), mapId);
}

bool PeekLastFarmMap(char* out, size_t outCap) {
    if (!out || outCap == 0) return false;
    if (!gLastFarmMap[0]) {
        out[0] = 0;
        return false;
    }
    strncpy_s(out, outCap, gLastFarmMap, _TRUNCATE);
    return true;
}

void RequestReturnFarm() { gReturnReq.store(true, std::memory_order_release); }

void RequestManualTrip() { gTripReq.store(true, std::memory_order_release); }

void AbortTrip(const char* reason) {
    strncpy_s(gAbortWhy, reason ? reason : "中止", _TRUNCATE);
    gAbortReq.store(true, std::memory_order_release);
}

void GetStatus(Status& out) {
    SyncStatus();
    out = gStatus;
}

}  // namespace x::features::auto_supply
