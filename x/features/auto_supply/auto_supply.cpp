#include "auto_supply.h"

#include "../fly/fly.h"
#include "../notify/notify.h"
#include "../ports/consumable_port.h"
#include "../ports/shop_port.h"
#include "../ports/teleport_port.h"
#include "../ports/travel_port.h"
#include "../ports/world_port.h"
#include "../sellbag/sellbag.h"
#include "../simple_combat/simple_combat.h"
#include "../travel/travel.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"
#include "xcat_auto_supply.h"
#include "xcat_item_catalog.h"
#include "xcat_sellbag.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace x::features::auto_supply {

namespace {
std::atomic<bool> gAbortReq{false};
char gAbortWhy[128]{};
std::atomic<bool> gReturnReq{false};
std::atomic<bool> gTripReq{false};
char gLastFarmMap[64]{};
}  // namespace shared

namespace {

namespace shop = ports::shop;
namespace consumable = ports::consumable;

std::atomic<bool> gDesired{false};
char gShopMap[64]{};
char gShopExclude[64]{};
char gResolvedNpc[24]{};
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
DWORD gLastMenuAttempt = 0;
DWORD gLastBuyAttempt = 0;
DWORD gLastCloseShopAttempt = 0;
DWORD gShopReadySince = 0;
int gCloseShopAttempts = 0;
DWORD gScrollAttemptAt = 0;
int gScrollTries = 0;
bool gTriedScroll = false;
bool gScrollPendingLand = false;  // 已成功用卷，等换图后再结束用卷阶段
char gScrollMapAtUse[64]{};       // 用卷时地图；换图后视为落地
bool gPreferDirect = false;
bool gPausedCombat = false;
bool gPausedFly = false;
bool gManualTrip = false;
DWORD gReturnStableSince = 0;
DWORD gTripTravelArmAt = 0;  // 非 0：此前禁止 RequestGoto（开趟冷却窗）
Status gStatus{};

enum class BuyStep : int {
    ReturnScroll = 0,
    Charge,
    PlanRefills,
    BuyRefills,
    Done,
};
BuyStep gBuyStep = BuyStep::ReturnScroll;

enum class BuySlot : int {
    Hp = 0,
    Mp,
    Custom,
    Feed,
    FeedAlt,
    Done,
};
BuySlot gBuySlot = BuySlot::Hp;
int gBuyNeed = 0;
int gActiveBuyId = 0;
int gPlannedNeed[5]{};  // per slot after meso split

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};

constexpr DWORD kBagPollMs = 2500;
constexpr DWORD kGotoTimeoutMs = 180000;
constexpr DWORD kWaitOpenTimeoutMs = 90000;
constexpr DWORD kSellTimeoutMs = 120000;
constexpr DWORD kBuyTimeoutMs = 120000;
constexpr DWORD kReturnTimeoutMs = 180000;
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
constexpr DWORD kCloseShopRetryMs = 400;
constexpr int kMaxCloseShopAttempts = 8;
constexpr DWORD kScrollWaitMs = 8000;
constexpr int kMaxScrollTries = 2;
constexpr int kInvConsume = 2;

std::mutex gTownMu;
std::unordered_set<int> gTownIds;
bool gTownTried = false;

void LoadTownIds() {
    std::lock_guard<std::mutex> lock(gTownMu);
    if (gTownTried) return;
    gTownTried = true;
    std::string path = runtime::GetBinDir() ? runtime::GetBinDir() : "";
    if (!path.empty() && path.back() != '\\') path += '\\';
    path += "dataservice\\map_info.tsv";
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t t0 = line.find('\t');
        if (t0 == std::string::npos) continue;
        const int mapId = atoi(line.c_str());
        const size_t t1 = line.find('\t', t0 + 1);
        const size_t t2 = line.find('\t', t1 == std::string::npos ? line.size() : t1 + 1);
        const size_t t3 = line.find('\t', t2 == std::string::npos ? line.size() : t2 + 1);
        const size_t t4 = line.find('\t', t3 == std::string::npos ? line.size() : t3 + 1);
        if (t3 == std::string::npos || t4 == std::string::npos) continue;
        const int town = atoi(line.c_str() + t3 + 1);
        if (town == 1 && mapId > 0) gTownIds.insert(mapId);
    }
    runtime::LogI("AutoSupply", "map_info towns loaded n=%zu", gTownIds.size());
}

bool IsTownMapIdHeuristic(int mapId) {
    if (mapId <= 0) return false;
    // 大区室外主城：后 6 位为 0
    if (mapId % 1000000 == 0) return true;
    LoadTownIds();
    std::lock_guard<std::mutex> lock(gTownMu);
    return gTownIds.count(mapId) != 0;
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
    case Phase::ClosingShop:
    case Phase::Returning: st.state = xcat::kAutoSupplyStateReturning; break;
    case Phase::Cooldown: st.state = xcat::kAutoSupplyStateTripDone; break;
    default: st.state = xcat::kAutoSupplyStateTripTrading; break;
    }
    strncpy_s(st.message, gStatus.message, _TRUNCATE);
    strncpy_s(st.lastFarmMapName, gLastFarmMap, _TRUNCATE);
    st.pendingReturnFarm = gPendingReturnFarm ? 1u : 0u;
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

void FailTrip(const char* why) {
    travel::RequestStop();
    sellbag::Abort(why);
    ResumeSystems();
    gManualTrip = false;
    gPendingReturnFarm = false;
    gReturnStableSince = 0;
    ClearTripTravelArm();
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
bool TryUseReturnScroll(consumable::FindResult& outFr, int& outUsedId) {
    outFr = {};
    outUsedId = 0;
    const char* codes[] = {
        xcat::kAutoSupplyDefaultReturnScrollCode,
        xcat::kAutoSupplyAltReturnScrollCode,
    };
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
        // FindAndUseByItemId 已打 not_found / use_fail / list_miss；这里补 AutoSupply 层摘要
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
    gLastBuyAttempt = 0;
    memset(gPlannedNeed, 0, sizeof(gPlannedNeed));
    Enter(Phase::Buying, "补货中…");
    return true;
}

void PlanRefillsWithMeso() {
    memset(gPlannedNeed, 0, sizeof(gPlannedNeed));
    struct Want {
        int slot = -1;
        int id = 0;
        int need = 0;
        int price = 0;
    };
    Want wants[5]{};
    int n = 0;
    auto push = [&](BuySlot slot, bool en, const char* code, int buyTo) {
        if (!en || buyTo <= 0 || n >= 5) return;
        const int id = ParseItemCode(code);
        if (id <= 0) return;
        const int have = CountConsume(id);
        if (have >= buyTo) return;
        bool inShop = false;
        int price = 0;
        if (!shop::QueryShopBuyOffer(id, inShop, price) || !inShop || price <= 0) return;
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
    if (n == 0) return;

    long long totalCost = 0;
    for (int i = 0; i < n; ++i) totalCost += 1LL * wants[i].need * wants[i].price;
    const int64_t meso = shop::QueryMeso();
    double scale = 1.0;
    if (meso >= 0 && totalCost > meso && totalCost > 0)
        scale = static_cast<double>(meso) / static_cast<double>(totalCost);
    for (int i = 0; i < n; ++i) {
        int need = static_cast<int>(wants[i].need * scale);
        if (need < 0) need = 0;
        if (scale < 1.0 && need == 0 && wants[i].need > 0 && meso >= wants[i].price) need = 1;
        while (need > 0 && 1LL * need * wants[i].price > meso) --need;
        if (wants[i].slot >= 0 && wants[i].slot < 5) gPlannedNeed[wants[i].slot] = need;
    }
    runtime::LogI("AutoSupply", "refill plan meso=%lld totalCost=%lld scale=%.3f n=%d",
                  static_cast<long long>(meso), totalCost, scale, n);
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
        int need = (slotIdx >= 0 && slotIdx < 5) ? gPlannedNeed[slotIdx] : 0;
        if (need <= 0) continue;
        const int id = ParseItemCode(code);
        if (id <= 0) continue;
        const int have = CountConsume(id);
        // planned was relative to start; re-clamp
        if (have >= need && slotIdx < 4) {
            // if plan said need X of buyTo gap, already counted; skip if bag now enough vs buyTo
        }
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
        if (!ports::world::IsPlayReady() || sellbag::IsBusy() || travel::IsActive()) return;
        char msg[96]{};
        if (!ResolveShopTarget(msg, sizeof(msg))) {
            gTripReq.store(false, std::memory_order_release);
            Publish(notify::NotificationKind::Warning, "auto-supply-trip", "无法启动",
                    msg[0] ? msg : "自动寻店失败；可手填杂货地图");
            return;
        }
        gTripReq.store(false, std::memory_order_release);
        // 用户显式开趟：取消崩溃续跑，避免先回挂机再卖
        if (gPendingReturnFarm) {
            runtime::LogI("AutoSupply", "manual trip overrides pendingReturn farm=%s",
                          gLastFarmMap[0] ? gLastFarmMap : "-");
            gPendingReturnFarm = false;
            PublishStatusIni();
        }
        gManualTrip = true;
        gShopExclude[0] = 0;
        gCooldownUntil = 0;
        travel::RequestStop();  // 清掉上次 already_there 等陈旧 failKind
        runtime::LogI("AutoSupply", "手动一趟 shop=%s (%s)", gShopMap, msg);
        Publish(notify::NotificationKind::Info, "auto-supply-trip", "补给已接单",
                "先开趟冷却，再赶路卖装");
        Enter(Phase::Pause, "手动补给：停手并记下挂机图…");
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

    if (!gDesired.load()) return;
    if (now < gCooldownUntil) return;
    if (gLastBagPoll && now - gLastBagPoll < kBagPollMs) return;
    gLastBagPoll = now;

    if (!ports::world::IsPlayReady()) return;
    if (sellbag::IsBusy() || travel::IsActive()) return;

    int used = 0, cap = 0;
    if (!EquipTriggerMet(used, cap)) return;

    char msg[96]{};
    if (!ResolveShopTarget(msg, sizeof(msg))) {
        SetMsg(msg[0] ? msg : "自动寻店失败");
        return;
    }
    gShopExclude[0] = 0;
    runtime::LogI("AutoSupply", "装备触发 %d/%d thr=%d → shop=%s", used, cap, gEquipTrigger,
                  gShopMap);
    gManualTrip = false;
    Enter(Phase::Pause, "停手并记下挂机图…");
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
    gScrollPendingLand = false;
    gScrollMapAtUse[0] = 0;
    gPreferDirect = ShouldPreferDirectShop(gLastFarmMap, gShopMap);
    if (MapMatchesTarget(gShopMap)) {
        // 已在店图：冷却主要用于停战斗瞬移；对话不依赖 Doing
        Enter(Phase::OpeningShop, "已在店图，尝试对话开店");
        gWaitOpenNotified = 0;
        gLastTalkAttempt = 0;
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
        gLastMenuAttempt = 0;
        gShopReadySince = 0;
        return;
    }

    if (!TripTravelReady(now)) return;

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

void TickOpeningShop(DWORD now) {
    if (now - gPhaseSince > kWaitOpenTimeoutMs) {
        // 轻量改道：排除本店图再解析
        if (gShopMap[0] && !gCfg.shopMapName[0]) {
            strncpy_s(gShopExclude, gShopMap, _TRUNCATE);
            char msg[96]{};
            if (ResolveShopTarget(msg, sizeof(msg)) && !MapMatchesTarget(gShopMap)) {
                runtime::LogW("AutoSupply", "开店超时，改道 %s", gShopMap);
                Enter(Phase::GoingTown, "改道其他杂货店…");
                gTriedScroll = true;
                gPreferDirect = true;
                return;
            }
        }
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
        Enter(Phase::Selling, "自动卖出中…");
        return;
    }
    gShopReadySince = 0;

    if (!gLastTalkAttempt || now - gLastTalkAttempt >= kTalkRetryMs) {
        gLastTalkAttempt = now;
        const int tpl = gResolvedNpc[0] ? atoi(gResolvedNpc) : 0;
        const bool talked = shop::TryTalkNearestNpc(0.f, tpl);
        if (!talked) {
            // 与改版前一致：Talk 失败再用 FuncKey 兜底（经典版可远距开店）
            (void)shop::TryNpcTalkFuncKey();
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
    if (gLastBuyAttempt && now - gLastBuyAttempt < kBuyRetryMs) return;

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
        if (scrollId > 0 && shop::QueryShopBuyOffer(scrollId, inShop, price) && inShop) {
            std::string err;
            if (!shop::BuyItem(scrollId, 1, err)) {
                if (err == "SHOP_BUSY") return;
                runtime::LogW("AutoSupply", "补回城卷 fail %s → skip", err.c_str());
            } else {
                runtime::LogI("AutoSupply", "补回城卷 ok");
            }
        } else {
            runtime::LogI("AutoSupply", "店内无回城卷，跳过");
        }
        gBuyStep = BuyStep::Charge;
        return;
    }

    if (gBuyStep == BuyStep::Charge) {
        gLastBuyAttempt = now;
        if (!gCfg.rechargeStarsEnabled) {
            gBuyStep = BuyStep::PlanRefills;
            return;
        }
        int charged = 0, skipMeso = 0, skipOther = 0;
        std::string err;
        (void)shop::RechargeShurikensInOpenShop(charged, skipMeso, skipOther, err);
        if (err == "SHOP_BUSY" || err == "LIST_STALE") {
            SetMsg("飞镖充值等待店务…");
            return;
        }
        if (charged > 0) {
            SetMsg("飞镖充值已发包，继续…");
            return;  // 下一拍再扫：忙清后充下一格 / 确认已满
        }
        if (skipMeso > 0)
            SetMsg("飞镖充值金币不足，跳过");
        else if (err == "NO_SHOP" || err == "UNBOUND" || err == "NO_RPC" || err == "MAIN_TIMEOUT")
            SetMsg("飞镖充值失败，跳过");
        else
            SetMsg("飞镖充值处理完毕");
        gBuyStep = BuyStep::PlanRefills;
        return;
    }

    if (gBuyStep == BuyStep::PlanRefills) {
        PlanRefillsWithMeso();
        gBuySlot = BuySlot::Hp;
        gActiveBuyId = 0;
        gBuyNeed = 0;
        gBuyStep = BuyStep::BuyRefills;
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
            char msg[96]{};
            snprintf(msg, sizeof(msg), "%s #%d 还需%d", label, id, need);
            SetMsg(msg);
        }
        gLastBuyAttempt = now;
        const int batch = gBuyNeed > 100 ? 100 : gBuyNeed;
        std::string err;
        if (!shop::BuyItem(gActiveBuyId, batch, err)) {
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
        gBuyNeed -= batch;
        if (gBuyNeed <= 0) {
            gActiveBuyId = 0;
            gBuyNeed = 0;
        }
        return;
    }

    StartReturnOrDone();
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
    // 冷却中点「立即一趟 / 回挂机」：立刻让出，勿干等 45s
    if (gTripReq.load(std::memory_order_acquire) || gReturnReq.load(std::memory_order_acquire)) {
        gCooldownUntil = 0;
        Enter(Phase::Idle, "空闲");
        SetMsg("");
        return;
    }
    if (now >= gCooldownUntil) {
        Enter(Phase::Idle, "空闲");
        SetMsg("");
    }
}

void TickManualCmds() {
    if (gAbortReq.exchange(false, std::memory_order_acq_rel)) {
        if (gPhase != Phase::Idle && gPhase != Phase::Cooldown) {
            FailTrip(gAbortWhy[0] ? gAbortWhy : "用户中止行程");
            return;
        }
        gPendingReturnFarm = false;
        PublishStatusIni();
    }

    if (gReturnReq.exchange(false, std::memory_order_acq_rel)) {
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

bool DiskBusyState(uint32_t state) {
    return state == xcat::kAutoSupplyStateProbeShopUi || state == xcat::kAutoSupplyStateBuying ||
           state == xcat::kAutoSupplyStateSelling || state == xcat::kAutoSupplyStateGoingTown ||
           state == xcat::kAutoSupplyStateOpeningShop || state == xcat::kAutoSupplyStateTripTrading ||
           state == xcat::kAutoSupplyStateReturning;
}

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

    const bool on = (gCfg.enabled != 0) || (gCfg.autoSellOnBagFullEnabled != 0);
    gDesired.store(on, std::memory_order_release);
    gEquipTrigger = gCfg.sellFreeSlotsAtOrBelow;

    // Init 已保证 bootstrapped。若仍未就绪：只能按 0 对齐，禁止用「当前磁盘 seq」收养，
    // 否则会把用户第一下刚写入的 manualTrip 当成 pre-inject 旧命令吞掉。
    if (!gManualSeqBootstrapped) BootstrapManualSeq(0, "reload-guard");

    if (gCfg.manualSeq != gHandledManualSeq) {
        gHandledManualSeq = gCfg.manualSeq;
        if (gCfg.manualKind == xcat::kAutoSupplyManualTrip) {
            gTripReq.store(true, std::memory_order_release);
            runtime::LogI("AutoSupply", "accept manualTrip seq=%u", gHandledManualSeq);
        } else if (gCfg.manualKind == xcat::kAutoSupplyManualReturnFarm) {
            gReturnReq.store(true, std::memory_order_release);
            runtime::LogI("AutoSupply", "accept manualReturn seq=%u", gHandledManualSeq);
        } else if (gCfg.manualKind == xcat::kAutoSupplyManualStop) {
            gDesired.store(false, std::memory_order_release);
            AbortTrip("用户停止动作");
            gCfg.enabled = 0;
            gCfg.autoSellOnBagFullEnabled = 0;
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
    gShopMap[0] = 0;
    gShopExclude[0] = 0;
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
            const bool on = (gCfg.enabled != 0) || (gCfg.autoSellOnBagFullEnabled != 0);
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
