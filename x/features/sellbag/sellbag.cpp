#include "sellbag.h"

#include "../notify/notify.h"
#include "../ports/shop_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"
#include "xcat_item_catalog.h"
#include "xcat_payload_status.h"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace x::features::sellbag {
namespace {

namespace shop = ports::shop;

xcat::SellbagConfig g_cfg{};
uint64_t g_cfgTick = 0;
DWORD g_lastCfgPoll = 0;
uint32_t g_lastManualSeq = 0;
bool g_manualSeqInit = false;
uint32_t g_lastAbortSeq = 0;
bool g_abortSeqInit = false;

std::atomic<uint32_t> g_pendingMask{0};
DWORD g_pendingSinceMs = 0;
bool g_suppressNotify = false;

enum class State { Idle, Selling };
State g_state = State::Idle;
uint32_t g_activeMask = 0;
bool g_curEquipBag = false;
DWORD g_lastStep = 0;
DWORD g_flowStartMs = 0;
DWORD g_lastShopReadyPoll = 0;
int g_sellGuard = 0;
int64_t g_roundMesoStart = 0;
bool g_roundMesoOk = false;
int g_listStaleRetries = 0;  // LIST_STALE / SHOP_BUSY 同项重试计数（切 TAB 后投影延迟）

constexpr DWORD kStepMs = 180;  // 略大于店内请求回包；SHOP_BUSY 时同步进重试
constexpr DWORD kFlowHardTimeoutMs = 90000;
constexpr int kSellMax = 512;
constexpr int kScanMax = 256;
constexpr int kListStaleMax = 12;  // ~2s@180ms；TAB 切完后列表仍空则放弃
constexpr DWORD kConfirmSoftTimeoutMs = 2500;
constexpr DWORD kConfirmHardTimeoutMs = 10000;
constexpr DWORD kCfgPollMs = 500;
constexpr DWORD kShopReadyPollMs = 1000;

struct Pending {
    bool active = false;
    int itemId = 0;
    int countBefore = 0;
    int invType = 0;
    int64_t mesoBefore = 0;
    DWORD firedAt = 0;
};
Pending g_pending{};

std::unordered_map<int, int> g_skip;

// 离线 catalog 把不卖关键词展开成 itemId（对照 pet_loot skip 解析）。
// key = (itemId)；value = 命中的 targetMask（equip/etc/all）。
std::unordered_map<int, uint32_t> g_keepIds;
bool g_keepIdsDirty = true;

void EndFlow(const char* msg, bool error);  // 前向声明：abort 轮询会调用

struct QueueItem {
    int pos = 0;
    int itemId = 0;
    int count = 1;
    int invType = 0;
    char name[64]{};
};
std::vector<QueueItem> g_queue;
size_t g_queueIndex = 0;

Status g_status{};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};

constexpr size_t kMaxIdsPerNameKey = 0;  // 0=不限；白名单截断会误卖

std::string NormalizeText(const char* s) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
        if (std::isspace(*p)) continue;
        out.push_back(static_cast<char>(std::tolower(*p)));
    }
    return out;
}

bool TextContains(const char* haystack, const char* needle) {
    const std::string h = NormalizeText(haystack);
    const std::string n = NormalizeText(needle);
    return !h.empty() && !n.empty() && h.find(n) != std::string::npos;
}

const xcat::ItemCatalogPack& Catalog() {
    return xcat::GetSharedItemCatalog(runtime::GetBinDir());
}

const char* OfflineName(int itemId) {
    if (itemId <= 0) return "";
    char code[32]{};
    snprintf(code, sizeof(code), "%d", itemId);
    return xcat::ItemCatalogLookupName(Catalog(), code);
}

void RebuildKeepIds() {
    g_keepIds.clear();
    g_keepIdsDirty = false;
    const auto& pack = Catalog();
    int fromSub = 0, miss = 0, trunc = 0;

    for (uint32_t i = 0; i < g_cfg.keepRuleCount &&
                        i < static_cast<uint32_t>(xcat::kSellbagMaxKeepRules); ++i) {
        const auto& r = g_cfg.keepRules[i];
        if (!r.enabled || !r.nameKey[0]) continue;
        if (!pack.loaded) {
            ++miss;
            continue;
        }
        std::vector<std::string> codes;
        const size_t total =
            xcat::ItemCatalogCollectCodesByNameContains(pack, r.nameKey, codes, kMaxIdsPerNameKey);
        if (total == 0) {
            ++miss;
            runtime::LogW("Sellbag", "保留词未命中离线表 key=\"%s\"", r.nameKey);
            continue;
        }
        if (total > codes.size()) ++trunc;
        const uint32_t mask = (r.targetMask & xcat::kSellbagBagAll)
                                  ? (r.targetMask & xcat::kSellbagBagAll)
                                  : xcat::kSellbagBagAll;
        for (const std::string& code : codes) {
            char* end = nullptr;
            const long v = strtol(code.c_str(), &end, 10);
            if (!end || *end != '\0' || v <= 0) continue;
            g_keepIds[static_cast<int>(v)] |= mask;
            ++fromSub;
        }
    }
    runtime::LogI("Sellbag",
                  "保留解析 catalog=%d rules=%u → ids=%zu (expand=%d miss=%d trunc=%d)",
                  pack.loaded ? 1 : 0, g_cfg.keepRuleCount, g_keepIds.size(), fromSub, miss, trunc);
}

void SetMessage(const char* msg) {
    strncpy_s(g_status.message, msg ? msg : "", _TRUNCATE);
}

void PublishSellNotify(notify::NotificationKind kind, const char* key, const char* title,
                       const char* body, uint32_t ttlMs) {
    notify::PublishNotification(notify::NotificationEvent{kind, key, title, body, ttlMs});
}

void ReloadConfigIfChanged() {
    xcat::SellbagConfig c{};
    if (!xcat::ReadSellbag(runtime::GetBinDir(), c)) {
        if (g_cfgTick != 0) {
            xcat::SellbagSetDefaults(g_cfg);
            g_cfgTick = 0;
            g_keepIdsDirty = true;
        }
        return;
    }
    if (c.writeTickMs == g_cfgTick) return;
    g_cfgTick = c.writeTickMs;
    if (c.keepRuleCount > static_cast<uint32_t>(xcat::kSellbagMaxKeepRules))
        c.keepRuleCount = xcat::kSellbagMaxKeepRules;
    g_cfg = c;
    g_keepIdsDirty = true;
    runtime::LogI("Sellbag", "config 载入: 保留规则=%u", g_cfg.keepRuleCount);
}

void PollExternalManualCommand(DWORD now) {
    if (g_pendingMask.load(std::memory_order_acquire) != 0 && g_state == State::Idle) {
        // pending 已有，仍轮询 abort
    }
    if (g_lastCfgPoll && now - g_lastCfgPoll < kCfgPollMs) return;
    g_lastCfgPoll = now;

    xcat::SellbagConfig c{};
    if (!xcat::ReadSellbag(runtime::GetBinDir(), c)) return;

    if (!g_manualSeqInit) {
        g_manualSeqInit = true;
        g_lastManualSeq = c.manualSeq;
    }
    if (!g_abortSeqInit) {
        g_abortSeqInit = true;
        g_lastAbortSeq = c.abortSeq;
    }
    if (c.writeTickMs != g_cfgTick) {
        g_cfgTick = c.writeTickMs;
        if (c.keepRuleCount > static_cast<uint32_t>(xcat::kSellbagMaxKeepRules))
            c.keepRuleCount = xcat::kSellbagMaxKeepRules;
        g_cfg = c;
        g_keepIdsDirty = true;
    }

    if (c.abortSeq != 0 && c.abortSeq != g_lastAbortSeq) {
        g_lastAbortSeq = c.abortSeq;
        g_pendingMask.store(0, std::memory_order_release);
        if (g_state == State::Selling) {
            EndFlow("用户中止", true);
        } else {
            g_status.state = 3u;
            SetMessage("已中止");
        }
        runtime::LogI("Sellbag", "收到 abortSeq=%u", c.abortSeq);
        return;
    }

    if (g_state != State::Idle) return;
    if (g_pendingMask.load(std::memory_order_acquire) != 0) return;

    const uint32_t mask = c.manualMask & xcat::kSellbagBagAll;
    if (c.manualSeq != 0 && c.manualSeq != g_lastManualSeq && mask != 0) {
        g_lastManualSeq = c.manualSeq;
        g_pendingMask.store(mask, std::memory_order_release);
        g_pendingSinceMs = now;
        g_status.state = 1u;
        SetMessage("已排队，等待开卖…");
        runtime::LogI("Sellbag", "收到外部一键卖出 seq=%u mask=0x%x", c.manualSeq, mask);
    }
}

bool ShouldKeep(const shop::BagItem& item, uint32_t bagBit) {
    if (g_keepIdsDirty) RebuildKeepIds();

    // ① 离线表：关键词 → itemId 集合（含异体字归一，见 ItemCatalogCollectCodesByNameContains）
    const auto idIt = g_keepIds.find(item.itemId);
    if (idIt != g_keepIds.end() && (idIt->second & bagBit) != 0) {
        ++g_status.kept;
        const char* offline = OfflineName(item.itemId);
        runtime::LogI("Sellbag", "保留(离线id) id=%d name=%s", item.itemId,
                      item.name[0] ? item.name : offline);
        return true;
    }

    // ② 运行时/离线名子串（与枫星契约一致；catalog 取名失败时仍可对槽位名兜底）
    for (uint32_t i = 0; i < g_cfg.keepRuleCount &&
                        i < static_cast<uint32_t>(xcat::kSellbagMaxKeepRules); ++i) {
        const auto& r = g_cfg.keepRules[i];
        if (!r.enabled || !r.nameKey[0]) continue;
        if ((r.targetMask & bagBit) == 0) continue;
        const char* offline = OfflineName(item.itemId);
        if (TextContains(item.name, r.nameKey) || TextContains(offline, r.nameKey)) {
            ++g_status.kept;
            runtime::LogI("Sellbag", "保留(名模糊) id=%d name=%s rule=%s", item.itemId,
                          item.name[0] ? item.name : offline, r.nameKey);
            return true;
        }
    }
    return false;
}

bool BuildQueueForCurrentBag() {
    g_queue.clear();
    g_queueIndex = 0;
    const uint32_t bagBit = g_curEquipBag ? xcat::kSellbagBagEquip : xcat::kSellbagBagEtc;
    if (g_keepIdsDirty) RebuildKeepIds();

    shop::BagItem items[kScanMax]{};
    int n = 0;
    if (!shop::ScanBag(g_curEquipBag, items, kScanMax, n)) return false;

    g_queue.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto& it = items[i];
        if (!it.sellable || it.pos <= 0 || it.itemId <= 0) continue;
        if (g_skip.count(it.itemId)) continue;
        if (ShouldKeep(it, bagBit)) continue;

        QueueItem q{};
        q.pos = it.pos;
        q.itemId = it.itemId;
        q.count = it.count;
        q.invType = it.invType;
        strncpy_s(q.name, it.name, _TRUNCATE);
        g_queue.push_back(q);
    }
    runtime::LogI("Sellbag", "队列生成 %s: candidates=%zu scanned=%d",
                  g_curEquipBag ? "装备" : "其他", g_queue.size(), n);
    return true;
}

void ResetRoundState() {
    g_pending = {};
    g_skip.clear();
    g_queue.clear();
    g_queueIndex = 0;
    g_sellGuard = 0;
    g_listStaleRetries = 0;
    g_status.equipSold = 0;
    g_status.etcSold = 0;
    g_status.kept = 0;
    g_status.failed = 0;
    g_status.mesoGained = 0;
    g_status.mesoGainedValid = 0;
}

void EndFlow(const char* msg, bool error) {
    g_state = State::Idle;
    g_activeMask = 0;
    g_pending = {};
    g_skip.clear();
    g_queue.clear();
    g_queueIndex = 0;
    g_lastShopReadyPoll = 0;
    g_status.busy = false;
    g_status.state = error ? 3u : 2u;
    g_status.lastRunTickMs = GetTickCount64();

    if (!error && g_roundMesoOk) {
        const int64_t mesoEnd = shop::QueryMeso();
        if (mesoEnd >= 0) {
            g_status.mesoGained = mesoEnd - g_roundMesoStart;
            g_status.mesoGainedValid = 1u;
        }
    }
    g_roundMesoOk = false;

    SetMessage(msg);
    runtime::LogI("Sellbag", "流程结束: %s (装备%u 其他%u 保留%u 失败%u meso=%lld)",
                  msg ? msg : "?", g_status.equipSold, g_status.etcSold, g_status.kept,
                  g_status.failed,
                  static_cast<long long>(g_status.mesoGainedValid ? g_status.mesoGained : 0));

    if (!g_suppressNotify) {
        char body[256]{};
        if (!error) {
            if (g_status.mesoGainedValid && g_status.mesoGained > 0) {
                snprintf(body, sizeof(body),
                         "卖装备 %u 件，卖其他 %u 件，保留 %u 件，获得金币 %lld。",
                         g_status.equipSold, g_status.etcSold, g_status.kept,
                         static_cast<long long>(g_status.mesoGained));
            } else {
                snprintf(body, sizeof(body), "卖装备 %u 件，卖其他 %u 件，保留 %u 件%s。",
                         g_status.equipSold, g_status.etcSold, g_status.kept,
                         g_status.failed ? "（部分未确认）" : "");
            }
            PublishSellNotify(notify::NotificationKind::Success, "sellbag-result", "一键卖出完成",
                              body, 6000);
        } else {
            snprintf(body, sizeof(body), "%s（已卖装备 %u 件、其他 %u 件）",
                     msg ? msg : "卖出中止", g_status.equipSold, g_status.etcSold);
            PublishSellNotify(notify::NotificationKind::Warning, "sellbag-result", "一键卖出中止",
                              body, 6500);
        }
    }
    g_suppressNotify = false;
}

bool PickNextBag() {
    if (g_activeMask & xcat::kSellbagBagEquip) {
        g_curEquipBag = true;
        return true;
    }
    if (g_activeMask & xcat::kSellbagBagEtc) {
        g_curEquipBag = false;
        return true;
    }
    return false;
}

bool ConfirmPending(DWORD now) {
    if (!g_pending.active) return true;
    bool present = true;
    int count = 0;
    const bool queried =
        shop::QueryItemPresent(g_pending.invType, g_pending.itemId, present, count);
    const bool gone = queried && !present;
    const bool reduced = queried && present && count < g_pending.countBefore;
    if (gone || reduced) {
        if (g_curEquipBag)
            ++g_status.equipSold;
        else
            ++g_status.etcSold;
        g_pending = {};
        return true;
    }

    // 槽位未变：看 meso 是否上涨（部分服槽位刷新慢）
    if (g_pending.mesoBefore >= 0) {
        const int64_t mesoNow = shop::QueryMeso();
        if (mesoNow > g_pending.mesoBefore) {
            if (g_curEquipBag)
                ++g_status.equipSold;
            else
                ++g_status.etcSold;
            g_pending = {};
            return true;
        }
    }

    const DWORD elapsed = now - g_pending.firedAt;
    if (elapsed >= kConfirmHardTimeoutMs ||
        (queried && elapsed >= kConfirmSoftTimeoutMs)) {
        ++g_status.failed;
        g_skip[g_pending.itemId] = 1;
        runtime::LogW("Sellbag", "确认超时 id=%d elapsed=%u", g_pending.itemId, elapsed);
        g_pending = {};
        return true;
    }
    return false;
}

void BeginFlow(uint32_t mask, DWORD now) {
    bool ready = false;
    if (!shop::ShopReady(ready) || !ready) {
        g_status.state = 3u;
        SetMessage("请先打开 NPC 商店");
        runtime::LogW("Sellbag", "未开店，拒绝开卖");
        if (!g_suppressNotify) {
            PublishSellNotify(notify::NotificationKind::Warning, "sellbag-need-shop",
                              "请先打开 NPC 商店", "打开杂货店后再点一键卖。", 5500);
        }
        g_suppressNotify = false;
        return;
    }
    ReloadConfigIfChanged();
    ResetRoundState();
    g_activeMask = mask & xcat::kSellbagBagAll;
    g_status.lastBagMask = g_activeMask;
    g_status.busy = true;
    g_status.state = 1u;
    g_flowStartMs = now;
    g_lastStep = 0;
    g_lastShopReadyPoll = now;
    g_roundMesoStart = shop::QueryMeso();
    g_roundMesoOk = g_roundMesoStart >= 0;
    g_state = State::Selling;
    SetMessage("卖出中…");
    if (!PickNextBag() || !BuildQueueForCurrentBag()) {
        EndFlow("建队失败", true);
    }
}

void TickSelling(DWORD now) {
    if (now - g_flowStartMs > kFlowHardTimeoutMs) {
        EndFlow("卖出超时", true);
        return;
    }

    if (!g_lastShopReadyPoll || now - g_lastShopReadyPoll >= kShopReadyPollMs) {
        g_lastShopReadyPoll = now;
        bool ready = false;
        if (shop::ShopReady(ready) && !ready) {
            EndFlow("商店已关闭", true);
            return;
        }
    }

    if (!ConfirmPending(now)) return;
    if (now - g_lastStep < kStepMs) return;
    g_lastStep = now;

    if (g_queueIndex >= g_queue.size()) {
        if (g_curEquipBag)
            g_activeMask &= ~xcat::kSellbagBagEquip;
        else
            g_activeMask &= ~xcat::kSellbagBagEtc;
        if (!PickNextBag()) {
            EndFlow("一键卖出完成", false);
            return;
        }
        if (!BuildQueueForCurrentBag()) {
            EndFlow("建队失败", true);
            return;
        }
        if (g_queue.empty()) {
            if (g_curEquipBag)
                g_activeMask &= ~xcat::kSellbagBagEquip;
            else
                g_activeMask &= ~xcat::kSellbagBagEtc;
            if (!PickNextBag()) {
                EndFlow("一键卖出完成", false);
                return;
            }
            if (!BuildQueueForCurrentBag()) {
                EndFlow("建队失败", true);
                return;
            }
        }
    }

    if (g_sellGuard >= kSellMax) {
        EndFlow("单轮卖出次数上限", true);
        return;
    }

    const QueueItem& q = g_queue[g_queueIndex];
    std::string err;
    const int64_t mesoBefore = shop::QueryMeso();
    if (!shop::SellItem(q.invType, q.pos, q.itemId, q.count, err)) {
        // SHOP_BUSY：店内请求未清，不跳过、不消耗队列下标，下步再试
        if (err.find("SHOP_BUSY") != std::string::npos) {
            runtime::LogW("Sellbag", "店忙重试 id=%d pos=%d", q.itemId, q.pos);
            return;
        }
        // LIST_STALE：刚切 TAB / 卖栏投影未就绪，同帧常空；限次重试后再记失败
        if (err.find("LIST_STALE") != std::string::npos) {
            ++g_listStaleRetries;
            if (g_listStaleRetries < kListStaleMax) {
                runtime::LogW("Sellbag", "列表未就绪重试 id=%d pos=%d n=%d/%d", q.itemId, q.pos,
                              g_listStaleRetries, kListStaleMax);
                return;
            }
            runtime::LogW("Sellbag", "列表未就绪耗尽 id=%d pos=%d", q.itemId, q.pos);
        }
        ++g_queueIndex;
        ++g_status.failed;
        g_skip[q.itemId] = 1;
        g_listStaleRetries = 0;
        runtime::LogW("Sellbag", "卖出失败 id=%d pos=%d err=%s", q.itemId, q.pos, err.c_str());
        return;
    }
    ++g_queueIndex;
    ++g_sellGuard;
    g_listStaleRetries = 0;
    g_pending.active = true;
    g_pending.itemId = q.itemId;
    g_pending.countBefore = q.count;
    g_pending.invType = q.invType;
    g_pending.mesoBefore = mesoBefore;
    g_pending.firedAt = now;
    runtime::LogI("Sellbag", "已发包 id=%d pos=%d qty=%d ti=%d name=%s", q.itemId, q.pos, q.count,
                  q.invType, q.name);
}

void Tick(DWORD now) {
    ReloadConfigIfChanged();
    PollExternalManualCommand(now);

    const uint32_t pending = g_pendingMask.exchange(0, std::memory_order_acq_rel);
    if (pending && g_state == State::Idle) BeginFlow(pending, now);

    if (g_state == State::Selling) TickSelling(now);
}

DWORD WINAPI WorkerProc(LPVOID) {
    runtime::LogI("Sellbag", "worker start");
    while (!gStop.load()) {
        Tick(GetTickCount());
        Sleep(50);
    }
    runtime::LogI("Sellbag", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    xcat::SellbagSetDefaults(g_cfg);
    const auto& pack = Catalog();
    runtime::LogI("Sellbag", "init catalog=%d names=%zu sellPrices=%zu", pack.loaded ? 1 : 0,
                  pack.nameByCode.size(), pack.sellPriceByCode.size());
    ReloadConfigIfChanged();
    if (g_keepIdsDirty) RebuildKeepIds();
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE t = CreateThread(nullptr, 0, &WorkerProc, nullptr, 0, nullptr);
    if (!t) {
        runtime::LogW("Sellbag", "create thread failed");
        return;
    }
    gThread.store(t);
}

void StopWorker() {
    gStop.store(true);
    HANDLE t = gThread.exchange(nullptr);
    if (t) {
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
}

bool RequestSell(uint32_t bagMask) {
    const uint32_t mask = bagMask & xcat::kSellbagBagAll;
    if (!mask) return false;
    if (g_state != State::Idle || g_pendingMask.load() != 0) return false;
    g_suppressNotify = false;
    g_pendingMask.store(mask, std::memory_order_release);
    g_pendingSinceMs = GetTickCount();
    g_status.state = 1u;
    SetMessage("已排队…");
    return true;
}

bool RequestSellQuiet(uint32_t bagMask) {
    const uint32_t mask = bagMask & xcat::kSellbagBagAll;
    if (!mask) return false;
    if (g_state != State::Idle || g_pendingMask.load() != 0) return false;
    g_suppressNotify = true;
    g_pendingMask.store(mask, std::memory_order_release);
    g_pendingSinceMs = GetTickCount();
    g_status.state = 1u;
    SetMessage("已排队…");
    return true;
}

void Abort(const char* reason) {
    g_pendingMask.store(0);
    if (g_state == State::Selling) EndFlow(reason ? reason : "已中止", true);
}

bool IsBusy() { return g_status.busy || g_state == State::Selling; }

void GetStatus(Status& out) { out = g_status; }

void PublishStatusToShm() {
    const char* binDir = runtime::GetBinDir();
    if (!binDir || !binDir[0]) return;
    xcat::PayloadStatus st{};
    (void)xcat::ReadPayloadStatus(binDir, st);
    st.sellbagBusy = g_status.busy ? 1u : 0u;
    st.sellbagState = g_status.state;
    st.sellbagLastBagMask = g_status.lastBagMask;
    st.sellbagEquipSold = g_status.equipSold;
    st.sellbagEtcSold = g_status.etcSold;
    st.sellbagKept = g_status.kept;
    st.sellbagFailed = g_status.failed;
    st.sellbagLastRunTickMs = g_status.lastRunTickMs;
    strncpy_s(st.sellbagMessage, g_status.message, _TRUNCATE);
    st.writeTickMs = GetTickCount64();
    (void)xcat::WritePayloadStatus(binDir, st);
}

}  // namespace x::features::sellbag
