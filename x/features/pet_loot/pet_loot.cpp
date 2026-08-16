// Classic TWMS — pet_loot vacuum (.rdata ByPet rect pack → TryPickUpDrop).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pet_loot.h"

#include "../ports/attack_input_port.h"
#include "../ports/drop_pool_port.h"
#include "../ports/pet_port.h"
#include "../ports/world_port.h"
#include "../auto_enter/auto_enter.h"
#include "../simple_combat/simple_combat.h"
#include "../notify/notify.h"
#include "../../ipc/payload_pet_loot.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"

#include "xcat_item_catalog.h"
#include "xcat_pet_loot.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x {
namespace features {
namespace pet_loot {
namespace {

constexpr DWORD kIdleSleepMs = 40;
constexpr DWORD kForceLogMs = 5000;      // probe：状态变化立即打；无变化最长间隔
constexpr DWORD kProbeIdleMs = 30000;    // probe 稳态（drops=0 等）心跳
constexpr DWORD kDetailLogMs = 8000;     // petmap 明细节流（聚合 absorb）
constexpr DWORD kErrLogMs = 8000;        // seh / no_rect 等硬错
constexpr DWORD kMissLogMs = 15000;
constexpr DWORD kNoPetGateMs = 2000;
constexpr DWORD kNoSkillLogMs = 8000;
// 退避集合持续偏大 ≈ 服端在批量拒收 → 多为背包某栏已满
constexpr DWORD kInvFullHintMs = 30000;
constexpr int kInvFullHintStall = 16;
// 名称关键词展开不设上限（0=全量）；「卷」~670、「色」~三千，固定 cap 会漏黑名单
constexpr size_t kMaxIdsPerNameKey = 0;

xcat::PetLootConfig gCfg{};
ports::drop::SkipIds gSkipResolved{};
bool gSkipDirty = true;
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

DWORD gNoPetSince = 0;
DWORD gLastNoSkillLog = 0;

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &self) ||
        !self)
        return L".";
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L".";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return s.substr(0, slash);
}

void OpenLog() {
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
}

void LogLine(const char* fmt, ...) {
    char body[1400];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1600];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    OpenLog();
    (void)x::runtime::AppendDbgLog(ModuleDir() + L"\\logs\\petloot.log", buf, (DWORD)n);
    x::runtime::LogI("PetLoot", "%s", body);
}

// 低频事件才打 ODS（调试器下 OutputDebugString 很贵）；明细走文件+LogI
void LogLineOd(const char* fmt, ...) {
    char body[1400];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1600];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    OpenLog();
    (void)x::runtime::AppendDbgLog(ModuleDir() + L"\\logs\\petloot.log", buf, (DWORD)n);
    OutputDebugStringA(buf);
    x::runtime::LogI("PetLoot", "%s", body);
}

bool SkipPush(ports::drop::SkipIds& skip, int id) { return skip.insert(id); }

void RebuildSkipIds() {
    gSkipResolved.clear();
    if (!gCfg.skipFilterEnabled) {
        gSkipDirty = false;
        if (gCfg.skipRuleCount > 0) {
            static DWORD s_lastOffLog = 0;
            const DWORD now = GetTickCount();
            if (!s_lastOffLog || now - s_lastOffLog >= 15000u) {
                s_lastOffLog = now;
                LogLine("skip resolve filter=off rules=%u (勾选「启用拾取黑名单」后才过滤)",
                        gCfg.skipRuleCount);
            }
        }
        return;
    }

    const xcat::ItemCatalogPack& pack = xcat::GetSharedItemCatalog(x::runtime::GetBinDir());
    int fromId = 0, fromExact = 0, fromSub = 0, missName = 0;

    for (uint32_t i = 0; i < gCfg.skipRuleCount; ++i) {
        const xcat::PetLootSkipRule& r = gCfg.skipRules[i];
        if (!r.enabled) continue;

        if (r.itemId != 0) {
            if (SkipPush(gSkipResolved, static_cast<int>(r.itemId))) ++fromId;
            continue;
        }
        if (!r.nameKey[0]) continue;

        if (!pack.loaded) {
            ++missName;
            LogLine("skip miss(no-catalog) key=\"%s\"", r.nameKey);
            continue;
        }

        std::vector<std::string> keys;
        xcat::SplitKeywordList(r.nameKey, keys);
        if (keys.empty()) {
            ++missName;
            LogLine("skip miss key=\"%s\"", r.nameKey);
            continue;
        }

        for (const std::string& key : keys) {
            std::vector<std::string> codes;
            const size_t total = xcat::ItemCatalogCollectCodesByNameContains(
                pack, key.c_str(), codes, kMaxIdsPerNameKey);
            if (total == 0) {
                ++missName;
                LogLine("skip miss key=\"%s\"", key.c_str());
                continue;
            }

            const bool exactOne =
                (total == 1 && codes.size() == 1 &&
                 xcat::ItemCatalogLookupCodeByExactName(pack, key.c_str())[0] != '\0');
            for (const std::string& code : codes) {
                char* end = nullptr;
                const long v = strtol(code.c_str(), &end, 10);
                if (!end || *end != '\0' || v <= 0) continue;
                if (!SkipPush(gSkipResolved, static_cast<int>(v))) continue;
                if (exactOne)
                    ++fromExact;
                else
                    ++fromSub;
            }
        }
    }

    gSkipDirty = false;
    char idBuf[160]{};
    size_t idOff = 0;
    int shown = 0;
    for (int id : gSkipResolved.ids) {
        if (shown >= 8) break;
        const int n = snprintf(idBuf + idOff, sizeof(idBuf) - idOff, "%s%d", shown ? "," : "", id);
        if (n < 0) break;
        idOff += (size_t)n;
        if (idOff >= sizeof(idBuf)) break;
        ++shown;
    }
    LogLine(
        "skip resolve filter=1 catalog=%d rules=%u → ids=%zu [%s%s] (itemId=%d exact=%d substr=%d "
        "miss=%d)",
        pack.loaded ? 1 : 0, gCfg.skipRuleCount, gSkipResolved.size(), idBuf,
        gSkipResolved.size() > 8 ? ",..." : "", fromId, fromExact, fromSub, missName);
}

const ports::drop::SkipIds* CurrentSkipIds() {
    if (gSkipDirty) RebuildSkipIds();
    return gSkipResolved.empty() ? nullptr : &gSkipResolved;
}

void TickHighValueDropNotify(DWORD now) {
    if (!gCfg.scrollDropNotify) return;
    static DWORD s_last = 0;
    if (s_last && now - s_last < 200) return;
    s_last = now;

    ports::drop::HighValueDropAlert hits[8]{};
    const int n = ports::drop::CollectNewHighValueDropAlerts(hits, 8);

    const xcat::ItemCatalogPack& pack = xcat::GetSharedItemCatalog(x::runtime::GetBinDir());
    auto fillName = [&](int itemId, char* buf, size_t buflen) {
        char code[16]{};
        snprintf(code, sizeof(code), "%d", itemId);
        const char* name = xcat::ItemCatalogLookupName(pack, code);
        if (name && name[0])
            snprintf(buf, buflen, "%s（%d）", name, itemId);
        else
            snprintf(buf, buflen, "itemId=%d", itemId);
    };

    if (n > 0) {
    auto publishGroup = [&](int kind, const char* keyPrefix, const char* title,
                            const char* logTag) {
        ports::drop::HighValueDropAlert group[8]{};
        int gn = 0;
        for (int i = 0; i < n && gn < 8; ++i) {
            if (hits[i].kind != kind) continue;
            group[gn++] = hits[i];
        }
        if (gn <= 0) return;
        if (gn == 1) {
            char key[48]{};
            snprintf(key, sizeof(key), "%s-%d", keyPrefix, group[0].dropId);
            char body[128]{};
            fillName(group[0].itemId, body, sizeof(body));
            notify::PublishNotification(notify::NotificationEvent{
                notify::NotificationKind::Success, key, title, body, 4500});
            LogLineOd("%s notify dropId=%d itemId=%d", logTag, group[0].dropId, group[0].itemId);
            return;
        }
        char key[48]{};
        snprintf(key, sizeof(key), "%s-batch-%u", keyPrefix, (unsigned)now);
        char body[256]{};
        size_t off = 0;
        off += (size_t)snprintf(body + off, sizeof(body) - off, "×%d　", gn);
        for (int i = 0; i < gn && off + 8 < sizeof(body); ++i) {
            char piece[96]{};
            fillName(group[i].itemId, piece, sizeof(piece));
            const int wrote =
                snprintf(body + off, sizeof(body) - off, "%s%s", i ? "、" : "", piece);
            if (wrote < 0) break;
            off += (size_t)wrote;
        }
        notify::PublishNotification(notify::NotificationEvent{
            notify::NotificationKind::Success, key, title, body, 5000});
        LogLineOd("%s notify batch n=%d", logTag, gn);
    };

    // 雷之鏢单独气泡（kind=3）；204 卷走「掉落卷軸」
    publishGroup(3, "petloot-scroll", "掉落雷之鏢", "thunderDart");
    publishGroup(2, "petloot-scroll", "掉落卷軸", "scrollDrop");
    }

    ports::drop::HighValueDropAlert gone[8]{};
    const int gn = ports::drop::CollectGoneHighValueDrops(gone, 8);
    if (gn <= 0) return;
    const bool lootOn = gCfg.enabled || gCfg.footEnabled || gCfg.charVacEnabled ||
                        gCfg.nativeVacEnabled;
    if (!lootOn) return;

    auto publishPicked = [&]() {
        const bool dartOnly = [&]() {
            for (int i = 0; i < gn; ++i) {
                if (gone[i].kind != 3) return false;
            }
            return gn > 0;
        }();
        const char* title = dartOnly ? "拾取雷之鏢" : "拾取成功";
        const char* logTag = dartOnly ? "dartPick" : "scrollPick";
        if (gn == 1) {
            char key[48]{};
            snprintf(key, sizeof(key), dartOnly ? "petloot-picked-dart-%d" : "petloot-picked-%d",
                     gone[0].dropId);
            char body[128]{};
            fillName(gone[0].itemId, body, sizeof(body));
            notify::PublishNotification(notify::NotificationEvent{
                notify::NotificationKind::Success, key, title, body, 3500});
            LogLineOd("%s ok dropId=%d itemId=%d", logTag, gone[0].dropId, gone[0].itemId);
            return;
        }
        char key[48]{};
        snprintf(key, sizeof(key), dartOnly ? "petloot-picked-dart-batch-%u" : "petloot-picked-batch-%u",
                 (unsigned)now);
        char body[256]{};
        size_t off = 0;
        off += (size_t)snprintf(body + off, sizeof(body) - off, "×%d　", gn);
        for (int i = 0; i < gn && off + 8 < sizeof(body); ++i) {
            char piece[96]{};
            fillName(gone[i].itemId, piece, sizeof(piece));
            const int wrote =
                snprintf(body + off, sizeof(body) - off, "%s%s", i ? "、" : "", piece);
            if (wrote < 0) break;
            off += (size_t)wrote;
        }
        notify::PublishNotification(notify::NotificationEvent{
            notify::NotificationKind::Success, key, title, body, 4000});
        LogLineOd("%s ok batch n=%d", logTag, gn);
    };
    publishPicked();
}

void HoldSkipWhileYielding() {
    // 原生宠 50x60 不看面板档位。拾物 Radio=关闭时 enabled/charVac/foot 全 0，
    // 这里再 return 会把 LiveSkip 留空：TryPick MI 钩装着也不会盖戳，箭矢照舔。
    if (!gCfg.skipFilterEnabled && !gCfg.enabled && !gCfg.charVacEnabled && !gCfg.footEnabled &&
        !gCfg.nativeVacEnabled)
        return;
    const ports::drop::SkipIds* skip = CurrentSkipIds();
    if (!gCfg.skipFilterEnabled || !skip || skip->empty()) {
        (void)ports::drop::HoldSkipDrops(nullptr, 2.f, 2.f);
        return;
    }
    float vacW = 0.f, vacH = 0.f;
    xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);
    const int n = ports::drop::HoldSkipDrops(skip, vacW * 0.5f, vacH * 0.5f);
    if (n <= 0) return;
    static DWORD sHoldLog = 0;
    const DWORD now = GetTickCount();
    if (!sHoldLog || now - sHoldLog > 2000) {
        sHoldLog = now;
        LogLineOd("skip-hold native-foot stamp=%d skipN=%d pet=%d charVac=%d foot=%d nativeVac=%d "
                  "(block 50x60)",
                  n, (int)skip->size(), gCfg.enabled ? 1 : 0, gCfg.charVacEnabled ? 1 : 0,
                  gCfg.footEnabled ? 1 : 0, gCfg.nativeVacEnabled ? 1 : 0);
    }
}

void TickDropFallBoost() {
    if (!gCfg.dropSnapLand && !gCfg.dropAccelFall) return;
    int snapN = 0;
    int accelN = 0;
    int skipHoldN = 0;
    const ports::drop::SkipIds* skip =
        gCfg.skipFilterEnabled ? CurrentSkipIds() : nullptr;
    const int n = ports::drop::BoostDropFall(gCfg.dropSnapLand != 0, gCfg.dropAccelFall != 0, skip,
                                            &snapN, &accelN, &skipHoldN);
    if (n <= 0) return;
    static DWORD sFallLog = 0;
    const DWORD now = GetTickCount();
    if (!sFallLog || now - sFallLog > 2000) {
        sFallLog = now;
        LogLineOd("drop-fall snap=%d accel=%d skipHold=%d flags(snap=%d accel=%d skip=%d)", snapN,
                  accelN, skipHoldN, gCfg.dropSnapLand ? 1 : 0, gCfg.dropAccelFall ? 1 : 0,
                  skip ? (int)skip->size() : 0);
    }
}

void TickNativeVacHold() {
    if (!gCfg.nativeVacEnabled) {
        ports::drop::ReleaseByPetRectPack();
        return;
    }
    float vacW = 0.f, vacH = 0.f;
    xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);
    static DWORD sMiss = 0;
    if (ports::drop::HoldByPetRectPack(vacW, vacH)) return;
    const DWORD now = GetTickCount();
    if (!sMiss || now - sMiss > 2000) {
        sMiss = now;
        LogLineOd("native-vac hold miss box=%.0fx%.0f", vacW, vacH);
    }
}

void Tick(DWORD now) {
    if (gCfg.nativeVacEnabled) {
        simple_combat::SetHighValueLootUrgent(false);
        TickNativeVacHold();
        return;
    }
    if (!gCfg.enabled && !gCfg.footEnabled && !gCfg.charVacEnabled) {
        simple_combat::SetHighValueLootUrgent(false);
        return;
    }

    // 与自动打怪/瞬移共用 MainPump（drainBudget=2 + JobPrio）。泵拥堵时让路，否则吸物会把出刀饿死
    // （upload 211841：interval=50 + 全盒清闸 → combat fires 连续 40s 归零）。
    if (x::runtime::main_thread::IsCongested()) {
        // 堵泵时吸不到：fail-closed 清紧急，避免 ExternalPause 空挂。
        // 仍盖黑名单戳，否则原生脚边 50x60 会把箭矢舔走。
        simple_combat::SetHighValueLootUrgent(false);
        HoldSkipWhileYielding();
        static DWORD sCongLog = 0;
        if (!sCongLog || now - sCongLog > 2000) {
            sCongLog = now;
            LogLineOd("yield pump_congested q=%d (defer loot for combat; skip-hold on)",
                      x::runtime::main_thread::QueuedJobCount());
        }
        return;
    }

    // 挂机时分复用：不出刀就吸；仅 Firing 让路（Aim/Recover 放行）。
    // 高价值优先：出刀窗前先 Peek（宠真空∩角色半盒）；有可吸装备/卷軸则打断出刀。
    const ports::drop::SkipIds* skip = CurrentSkipIds();
    if (gCfg.highValuePriority && gCfg.enabled) {
        float vacW0 = 0.f, vacH0 = 0.f;
        xcat::PetLootEffectiveVacuum(gCfg, vacW0, vacH0);
        ports::drop::ProbeSnapshot snap{};
        const bool havePet =
            ports::drop::CollectProbe(snap, vacW0 * 0.5f, vacH0 * 0.5f) && snap.hasPet;
        int hvN = 0, hvFull = 0;
        int hvDrop = 0, hvInfo = 0, hvKind = 0;
        // 无宠位 / Peek 失败 → fail-closed 清 urgent（禁止 (0,0) 误扫、禁止 Pause 泄漏）
        if (!havePet ||
            !ports::drop::PeekHighValueActionable(snap.petX, snap.petY, vacW0 * 0.5f,
                                                  vacH0 * 0.5f, skip, hvN, hvFull, &hvDrop,
                                                  &hvInfo, &hvKind)) {
            simple_combat::SetHighValueLootUrgent(false);
        } else {
            simple_combat::SetHighValueLootUrgent(hvN > 0);
            if (hvN > 0) {
                static DWORD sHvLog = 0;
                if (!sHvLog || now - sHvLog > 2000) {
                    sHvLog = now;
                    const char* kind = hvKind == 1   ? "equip"
                                       : hvKind == 2 ? "scroll"
                                       : hvKind == 3 ? "dart"
                                                     : "?";
                    LogLineOd("highValue urgent near=%d skippedFull=%d dropId=%d itemId=%d "
                              "kind=%s (interrupt fire)",
                              hvN, hvFull, hvDrop, hvInfo, kind);
                }
            }
        }
    } else {
        simple_combat::SetHighValueLootUrgent(false);
    }

    if (!simple_combat::IsLootPulseActive()) {
        HoldSkipWhileYielding();
        static DWORD sFireLog = 0;
        if (!sFireLog || now - sFireLog > 2000) {
            sFireLog = now;
            LogLineOd("yield combat_fire_window (defer loot for Firing only; skip-hold on)");
        }
        return;
    }

    // 人吸/脚下 due 拍也要喂 LiveSkip + 盖戳；原生宠 Tick 不跟面板档位走。
    HoldSkipWhileYielding();

    // 人物直吸 = 宠吸控制面，主体换成角色；半盒 = vacuumW/H / 2（与宠吸共用全盒）；
    // 官方 Send 不写 LastTry，拒收必须靠 sentDropId AddStall；burst 跟面板（自设，硬顶 HardCap）。
    // 用户面关闭时 Normalize 已掐 charVac；此处再挡一层，代码路径保留便于重开。
    if (xcat::kPetLootCharVacUserEnabled && gCfg.charVacEnabled && !gCfg.enabled) {
        const uint32_t burst = xcat::PetLootClampBurstPerTick(gCfg.burstPerTick);
        float charHW = 0.f, charHH = 0.f;
        xcat::PetLootEffectiveCharHalf(gCfg, charHW, charHH);
        ports::drop::CharVacResult cr{};
        bool cok = false;
        uint32_t calls = 0;
        int sentAcc = 0;
        int absorbedN = 0;
        int sentSameAcc = 0;
        for (uint32_t bi = 0; bi < burst; ++bi) {
            ports::drop::CharVacResult one{};
            cok = ports::drop::TryCharVacuum(charHW, charHH, /*maxSend=*/1, skip, one);
            ++calls;
            cr = one;
            sentAcc += one.sent;
            if (one.why && std::strcmp(one.why, "ok_absorbed") == 0) ++absorbedN;
            if (one.sentButPoolSame) ++sentSameAcc;
            if (!cok && one.why &&
                (std::strcmp(one.why, "seh") == 0 || std::strcmp(one.why, "no_lu") == 0 ||
                 std::strcmp(one.why, "no_pool") == 0 || std::strcmp(one.why, "no_fn") == 0 ||
                 std::strcmp(one.why, "no_meta") == 0))
                break;
            if (one.why && std::strcmp(one.why, "ok_empty") == 0) break;
            if (one.nearCount == 0 && !(one.why && std::strcmp(one.why, "ok_absorbed") == 0))
                break;
            // 送了池没掉：已 AddStall，勿同拍连打下一件
            if (one.sent > 0 && one.sentButPoolSame) break;
            if (one.sent > 0 && one.why && std::strcmp(one.why, "ok_absorbed") != 0) break;
        }

        static DWORD s_lastChar = 0;
        const bool force = !s_lastChar || (now - s_lastChar >= kDetailLogMs);
        if (force || sentAcc > 0 || sentSameAcc > 0 ||
            (cr.why && std::strcmp(cr.why, "seh") == 0)) {
            s_lastChar = now;
            LogLine("mode=charvac pos=(%.1f,%.1f) box=%.0fx%.0f drops=%d→%d Δ=%d fell=%d near=%d "
                    "want=%d sent=%d calls=%u/%u absorb=%d gates=%d skipStamp=%d "
                    "stall=%d/%d/%d sendTouch=%d same=%d total=%u called=%d why=%s skipN=%d",
                    cr.userX, cr.userY, charHW * 2.f, charHH * 2.f, cr.dropCount,
                    cr.dropCountAfter, cr.dropsDelta, cr.poolFellSinceLast ? 1 : 0, cr.nearCount,
                    cr.nearWant, sentAcc, calls, burst, absorbedN, cr.gatesCleared, cr.skipStamped,
                    cr.stallHeld, cr.stallStamped, cr.stallRestored, cr.sendTouch, sentSameAcc,
                    cr.sentTotal, cr.called ? 1 : 0, cr.why ? cr.why : "?",
                    skip ? (int)skip->size() : 0);
        }
        (void)cok;
    }

    // 脚下：只自动触发原生 DropPool.TryPickUpDrop；与宠吸互斥（Normalize 已保证）
    if (gCfg.footEnabled && !gCfg.enabled && !gCfg.charVacEnabled) {
        ports::drop::FootResult fr{};
        const bool fok = ports::drop::TryFootPickup(fr);
        static DWORD s_lastFoot = 0;
        const bool force = !s_lastFoot || (now - s_lastFoot >= kDetailLogMs);
        if (force || (fr.why && (std::strcmp(fr.why, "seh") == 0))) {
            s_lastFoot = now;
            LogLine(
                "mode=foot native pos=(%.1f,%.1f) drops=%d→%d Δ=%d called=%d why=%s "
                "poolSendΔ=%u poolFell=%d",
                fr.userX, fr.userY, fr.dropCount, fr.dropCountAfter, fr.dropsDelta,
                fr.called ? 1 : 0, fr.why ? fr.why : "?", fr.poolSendDelta,
                fr.poolFellSinceLast ? 1 : 0);
        }
        (void)fok;
    }

    if (!gCfg.enabled) return;

    ports::pet::PetCareState pst{};
    if (!ports::pet::ReadState(pst) || !pst.hasLocalUser) {
        simple_combat::SetHighValueLootUrgent(false);
        return;
    }

    if (pst.activatedCount <= 0) {
        if (!gNoPetSince) gNoPetSince = now;
        // 无活宠吸不到：清紧急，避免 ExternalPause 空挂
        simple_combat::SetHighValueLootUrgent(false);
        if (now - gNoPetSince >= kNoPetGateMs) {
            static DWORD s_lastNoPet = 0;
            if (!s_lastNoPet || now - s_lastNoPet >= kForceLogMs) {
                s_lastNoPet = now;
                LogLine("mode=petmap why=no_active_pet (召出活宠后再吸)");
            }
        }
        return;
    }
    gNoPetSince = 0;

    float vacW = 0.f, vacH = 0.f;
    xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);

    const uint32_t burst = xcat::PetLootClampBurstPerTick(gCfg.burstPerTick);
    ports::drop::VacuumResult vr{};
    bool ok = false;
    uint32_t calls = 0;
    int absorbedN = 0;
    for (uint32_t bi = 0; bi < burst; ++bi) {
        ports::drop::VacuumResult one{};
        ok = ports::drop::TryPetVacuum(vacW, vacH, skip, one, gCfg.highValuePriority != 0);
        ++calls;
        vr = one;
        if (one.why && std::strcmp(one.why, "ok_absorbed") == 0) ++absorbedN;
        if (!ok && one.why && std::strcmp(one.why, "no_skill") == 0) break;
        if (one.why && (std::strcmp(one.why, "no_rect_patch") == 0 ||
                        std::strcmp(one.why, "seh") == 0 || std::strcmp(one.why, "no_pet") == 0 ||
                        std::strcmp(one.why, "no_lu") == 0))
            break;
        if (one.why && std::strcmp(one.why, "ok_empty") == 0) break;
        if (one.why && std::strcmp(one.why, "wait_land") == 0) break;
        if (one.why && std::strcmp(one.why, "ok_own") == 0) break;
        if (one.why && std::strcmp(one.why, "reject_backoff") == 0) break;
        if (one.nearCount == 0 && !(one.why && std::strcmp(one.why, "ok_absorbed") == 0)) break;
        // 根治洪泛：送出一包 / 池未掉即停本拍（人物直吸同口径）。大盒 ByPet 一拍已够。
        if (one.sentButPoolSame) break;
        if (one.why && (std::strcmp(one.why, "ok_sent") == 0 || std::strcmp(one.why, "ok") == 0))
            break;
        // ok_absorbed：池已掉，可同拍再吸下一件（仍受 burst 顶）
    }

    if (gCfg.highValuePriority) {
        // vacuum 未跑完 Scan 的早退：保留 Peek 的 urgent（避免 reject_backoff 抖动清 Pause）
        // 明确吸不了：清 urgent
        const char* why = vr.why ? vr.why : "";
        const bool clearUrgent = std::strcmp(why, "no_pet") == 0 ||
                                 std::strcmp(why, "no_skill") == 0 ||
                                 std::strcmp(why, "no_lu") == 0 ||
                                 std::strcmp(why, "seh") == 0 ||
                                 std::strcmp(why, "ok_far") == 0 ||
                                 std::strcmp(why, "ok_own") == 0 ||
                                 std::strcmp(why, "ok_empty") == 0 ||
                                 std::strcmp(why, "ok_hold") == 0 ||
                                 std::strcmp(why, "wait_land") == 0;
        const bool keepPeekUrgent = std::strcmp(why, "reject_backoff") == 0 ||
                                    std::strcmp(why, "no_pump") == 0 ||
                                    std::strcmp(why, "timeout") == 0 ||
                                    std::strcmp(why, "unbound") == 0;
        if (clearUrgent)
            simple_combat::SetHighValueLootUrgent(false);
        else if (!keepPeekUrgent)
            simple_combat::SetHighValueLootUrgent(vr.highValueUrgent);
        // 诊断：HV 按件选中必打（含 itemId），便于 BIN 核对是否真捡到卷/装
        if (vr.pacedPickRank == 2) {
            LogLineOd("highValue paced dropId=%d itemId=%d rank=%d sampleKind=%d why=%s Δ=%d "
                      "fell=%d",
                      vr.pacedPickDropId, vr.pacedPickInfo, vr.pacedPickRank,
                      vr.highValueSampleKind, why[0] ? why : "?", vr.dropsDelta,
                      vr.poolFellSinceLast ? 1 : 0);
        }
    } else {
        simple_combat::SetHighValueLootUrgent(false);
    }

    if (!ok && vr.why && std::strcmp(vr.why, "no_skill") == 0) {
        if (!gLastNoSkillLog || now - gLastNoSkillLog >= kNoSkillLogMs) {
            gLastNoSkillLog = now;
            LogLineOd("mode=petmap pets=1 skill=0x%X skillSlot=0x%X why=no_skill (need PickupItem)",
                      (unsigned)vr.petSkill, (unsigned)vr.petSkillSlot);
        }
        return;
    }

    if (vr.why && std::strcmp(vr.why, "reject_backoff") == 0) {
        static DWORD s_lastBackoffLog = 0;
        if (!s_lastBackoffLog || now - s_lastBackoffLog >= kForceLogMs) {
            s_lastBackoffLog = now;
            LogLineOd("mode=petmap why=reject_backoff (sentSame streak; %us 内少空转，未必满栏)",
                      5u);
        }
        return;
    }

    // 节流：不再每拍 ok/ok_absorbed 落盘；聚合 absorb/near 后按间隔刷一行
    static DWORD s_lastDetail = 0;
    static DWORD s_lastErr = 0;
    static int s_absorbAcc = 0;
    static int s_tickAcc = 0;
    static int s_nearMax = 0;
    static int s_moneyMax = 0;
    static int s_itemMax = 0;
    static int s_ownSkipMax = 0;
    static int s_sentSameAcc = 0;
    static int s_sendTouchMax = 0;
    static int s_sentItemWhileMoney = 0;

    s_absorbAcc += absorbedN;
    ++s_tickAcc;
    if (vr.nearCount > s_nearMax) s_nearMax = vr.nearCount;
    if (vr.nearMoney > s_moneyMax) s_moneyMax = vr.nearMoney;
    if (vr.nearItem > s_itemMax) s_itemMax = vr.nearItem;
    if (vr.ownSkipped > s_ownSkipMax) s_ownSkipMax = vr.ownSkipped;
    if (vr.sentButPoolSame) ++s_sentSameAcc;
    if (vr.sendTouch > s_sendTouchMax) s_sendTouchMax = vr.sendTouch;
    if (vr.sentButPoolSame && vr.sendTouchMoney == 0 && vr.nearMoney > 0) ++s_sentItemWhileMoney;

    const bool hardErr =
        vr.why && (std::strcmp(vr.why, "seh") == 0 || std::strcmp(vr.why, "no_rect_patch") == 0);
    const bool errDue = hardErr && (!s_lastErr || now - s_lastErr >= kErrLogMs);
    const bool detailDue = !s_lastDetail || (now - s_lastDetail >= kDetailLogMs);
    // near/ownSkip 也算信号：否则归属全跳过时永远不刷 mode=petmap，无法验预筛
    const bool hasSignal =
        s_absorbAcc > 0 || s_sentSameAcc > 0 || hardErr || s_nearMax > 0 || s_ownSkipMax > 0;

    if (errDue) {
        s_lastErr = now;
        s_lastDetail = now;
        LogLineOd(
            "mode=petmap pets=%d skill=0x%X skillSlot=0x%X box=%.0fx%.0f mapVac=%d burst=%u/%u "
            "absorbed=%d drops=%d→%d Δ=%d fell=%d near=%d money=%d item=%d "
            "sampMoney=%d sampInfo=%d sendTouch=%d touchMoney=%d sentSame=%d "
            "gates=%d skipStamp=%d ownSkip=%d remotes=%d stall=%d/%d/%d fly=%d ownType=%d ownRaw=%d "
            "ownerId=%d myCid=%d hv=%d/%d hvDrop=%d hvInfo=%d hvKind=%d "
            "pickDrop=%d pickInfo=%d pickRank=%d "
            "lastTry=%d endPara=%d "
            "called=%d why=%s petSendΔ=%u poolSendΔ=%u skipN=%d",
            pst.activatedCount, (unsigned)vr.petSkill, (unsigned)vr.petSkillSlot, vacW, vacH,
            gCfg.mapVacuumEnabled ? 1 : 0, calls, burst, absorbedN, vr.dropCount, vr.dropCountAfter,
            vr.dropsDelta, vr.poolFellSinceLast ? 1 : 0, vr.nearCount, vr.nearMoney, vr.nearItem,
            vr.sampleIsMoney, vr.sampleInfo, vr.sendTouch, vr.sendTouchMoney, vr.sentButPoolSame,
            vr.gatesCleared, vr.skipStamped, vr.ownSkipped, vr.remoteUsers, vr.stallHeld, vr.stallStamped,
            vr.stallRestored, vr.flyHeld, vr.sampleOwnType, vr.sampleOwnRaw, vr.sampleOwnerId,
            vr.localCharId, vr.highValueNear, vr.highValueSkippedFull, vr.highValueSampleDropId,
            vr.highValueSampleInfo, vr.highValueSampleKind, vr.pacedPickDropId, vr.pacedPickInfo,
            vr.pacedPickRank, vr.sampleLastTry, vr.sampleEndPara, vr.called ? 1 : 0,
            vr.why ? vr.why : "?", vr.petSendDelta, vr.poolSendDelta, skip ? (int)skip->size() : 0);
        s_absorbAcc = 0;
        s_tickAcc = 0;
        s_nearMax = s_moneyMax = s_itemMax = s_ownSkipMax = 0;
        s_sentSameAcc = s_sendTouchMax = s_sentItemWhileMoney = 0;
    } else if (detailDue && hasSignal) {
        s_lastDetail = now;
        LogLine(
            "mode=petmap pets=%d skill=0x%X skillSlot=0x%X box=%.0fx%.0f mapVac=%d burst=%u/%u "
            "absorbed=%d ticks=%d nearMax=%d moneyMax=%d itemMax=%d "
            "sentSame=%d touchMax=%d itemWhileMoney=%d "
            "drops=%d→%d Δ=%d fell=%d near=%d money=%d item=%d "
            "sampMoney=%d sampInfo=%d sendTouch=%d touchMoney=%d "
            "gates=%d skipStamp=%d ownSkip=%d remotes=%d stall=%d/%d/%d fly=%d ownType=%d ownRaw=%d "
            "ownerId=%d myCid=%d hv=%d/%d hvDrop=%d hvInfo=%d hvKind=%d "
            "pickDrop=%d pickInfo=%d pickRank=%d "
            "lastTry=%d endPara=%d "
            "called=%d why=%s petSendΔ=%u poolSendΔ=%u skipN=%d",
            pst.activatedCount, (unsigned)vr.petSkill, (unsigned)vr.petSkillSlot, vacW, vacH,
            gCfg.mapVacuumEnabled ? 1 : 0, calls, burst, s_absorbAcc, s_tickAcc, s_nearMax,
            s_moneyMax, s_itemMax, s_sentSameAcc, s_sendTouchMax, s_sentItemWhileMoney, vr.dropCount,
            vr.dropCountAfter, vr.dropsDelta, vr.poolFellSinceLast ? 1 : 0, vr.nearCount,
            vr.nearMoney, vr.nearItem, vr.sampleIsMoney, vr.sampleInfo, vr.sendTouch,
            vr.sendTouchMoney, vr.gatesCleared, vr.skipStamped, vr.ownSkipped, vr.remoteUsers, vr.stallHeld,
            vr.stallStamped, vr.stallRestored, vr.flyHeld, vr.sampleOwnType, vr.sampleOwnRaw,
            vr.sampleOwnerId, vr.localCharId, vr.highValueNear, vr.highValueSkippedFull,
            vr.highValueSampleDropId, vr.highValueSampleInfo, vr.highValueSampleKind,
            vr.pacedPickDropId, vr.pacedPickInfo, vr.pacedPickRank, vr.sampleLastTry,
            vr.sampleEndPara, vr.called ? 1 : 0, vr.why ? vr.why : "?", vr.petSendDelta,
            vr.poolSendDelta, skip ? (int)skip->size() : 0);
        s_absorbAcc = 0;
        s_tickAcc = 0;
        s_nearMax = s_moneyMax = s_itemMax = s_ownSkipMax = 0;
        s_sentSameAcc = s_sendTouchMax = s_sentItemWhileMoney = 0;
    }

    // 不读背包格数。stallHeld 大只说明「送包后池未降」被登记进软件退避表——
    // 满栏只是常见原因之一；距离/归属/服端异步/同拍误登记也会堆 stall（0b66c7 用户袋空仍刷）。
    // 无 sentSame 信号时不提示，避免挂机饿吸/wait_land 堆表时吓人。
    static DWORD s_lastInvHint = 0;
    if (vr.stallHeld >= kInvFullHintStall && s_sentSameAcc > 0 &&
        (!s_lastInvHint || now - s_lastInvHint >= kInvFullHintMs)) {
        s_lastInvHint = now;
        LogLine("mode=petmap hint=reject_stall_suspect stall=%d near=%d money=%d item=%d "
                "sentSame=%d (未读背包；送包后池未降的退避堆积，满栏只是可能原因之一)",
                vr.stallHeld, vr.nearCount, vr.nearMoney, vr.nearItem, s_sentSameAcc);
    }
    (void)ok;
}

DWORD WINAPI Worker(LPVOID) {
    timeBeginPeriod(1);
    OpenLog();
    LogLineOd("pet_loot worker start");
    ports::pet::Init();
    ports::drop::Init();

    DWORD lastTick = 0;
    DWORD lastMiss = 0;
    DWORD lastForceProbe = 0;
    uint32_t lastCfgSig = 0;
    uint32_t lastCfgSig2 = 0;
    uint32_t lastCfgSigVac = 0;

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        x::ipc::PayloadPetLoot_Poll();

        const uint32_t cfgSig = (gCfg.enabled ? 1u : 0u) | (gCfg.footEnabled ? 2u : 0u) |
                                (gCfg.mapVacuumEnabled ? 4u : 0u) |
                                (gCfg.charVacEnabled ? 8u : 0u) |
                                (gCfg.nativeVacEnabled ? 0x10u : 0) | (gCfg.skipRuleCount << 8) |
                                (gCfg.skipFilterEnabled ? 0x80000000u : 0) |
                                (gCfg.highValuePriority ? 0x40000000u : 0) |
                                (gCfg.scrollDropNotify ? 0x20000000u : 0) |
                                (gCfg.dropSnapLand ? 0x10000000u : 0) |
                                (gCfg.dropAccelFall ? 0x08000000u : 0);
        const uint32_t cfgSig2 = (gCfg.intervalMs & 0xFFFFu) | (gCfg.burstPerTick << 16);
        const uint32_t cfgSigVac = (static_cast<uint32_t>(gCfg.vacuumW + 0.5f) & 0xFFFFu) |
                                   ((static_cast<uint32_t>(gCfg.vacuumH + 0.5f) & 0xFFFFu) << 16);
        if (cfgSig != lastCfgSig || cfgSig2 != lastCfgSig2 || cfgSigVac != lastCfgSigVac) {
            lastCfgSig = cfgSig;
            lastCfgSig2 = cfgSig2;
            lastCfgSigVac = cfgSigVac;
            float vacW = 0.f, vacH = 0.f;
            xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);
            float charHW = 0.f, charHH = 0.f;
            xcat::PetLootEffectiveCharHalf(gCfg, charHW, charHH);
            LogLineOd("config pet=%d foot=%d charVac=%d nativeVac=%d mapVac=%d interval=%u burst=%u "
                      "box=%.0fx%.0f(near) footBox=50x60(native) charBox=%.0fx%.0f filters=0x%X "
                      "skipFilter=%d skipRules=%u highValue=%d scrollNotify=%d snapLand=%d "
                      "accelFall=%d",
                    gCfg.enabled ? 1 : 0, gCfg.footEnabled ? 1 : 0, gCfg.charVacEnabled ? 1 : 0,
                    gCfg.nativeVacEnabled ? 1 : 0, gCfg.mapVacuumEnabled ? 1 : 0, gCfg.intervalMs,
                    gCfg.burstPerTick, vacW, vacH, charHW * 2.f, charHH * 2.f, gCfg.filterFlags,
                    gCfg.skipFilterEnabled ? 1 : 0, gCfg.skipRuleCount,
                    gCfg.highValuePriority ? 1 : 0, gCfg.scrollDropNotify ? 1 : 0,
                    gCfg.dropSnapLand ? 1 : 0, gCfg.dropAccelFall ? 1 : 0);
        }

        // 脚边 / 人物直吸：DropPool+MyUser；宠吸：额外 pet_port
        // 卷軸提示音、黑名单可在「拾物全关」时单独跑（原生宠 50x60 仍会舔）
        const bool wantFoot = gCfg.footEnabled != 0;
        const bool wantPet = gCfg.enabled != 0;
        const bool wantChar = gCfg.charVacEnabled != 0;
        const bool wantNativeVac = gCfg.nativeVacEnabled != 0;
        const bool wantScrollNotify = gCfg.scrollDropNotify != 0;
        const bool wantSkip = gCfg.skipFilterEnabled != 0;
        const bool wantFall = gCfg.dropSnapLand != 0 || gCfg.dropAccelFall != 0;
        if (!wantFoot && !wantPet && !wantChar && !wantNativeVac && !wantScrollNotify &&
            !wantSkip && !wantFall) {
            ports::drop::ReleaseByPetRectPack();
            Sleep(kIdleSleepMs);
            continue;
        }

        const bool dropOk = ports::drop::EnsureBound();
        if (wantScrollNotify && dropOk) TickHighValueDropNotify(now);

        if (wantNativeVac) {
            if (dropOk) {
                HoldSkipWhileYielding();
                if (wantFall) TickDropFallBoost();
                TickNativeVacHold();
                static DWORD sNvLog = 0;
                if (!sNvLog || now - sNvLog > 2000) {
                    sNvLog = now;
                    float vacW = 0.f, vacH = 0.f;
                    xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);
                    LogLineOd("mode=nativeVac hold box=%.0fx%.0f skip=%d fall=%d", vacW, vacH,
                              wantSkip ? 1 : 0, wantFall ? 1 : 0);
                }
            }
            Sleep(kIdleSleepMs);
            continue;
        }
        ports::drop::ReleaseByPetRectPack();

        const bool petOk = !wantPet || ports::pet::EnsureBound();
        const bool anyReady = dropOk && (wantFoot || wantChar || (wantPet && petOk));
        if (!wantFoot && !wantPet && !wantChar) {
            // 拾物关闭：仍喂 LiveSkip，否则 TryPick 钩空转、原生宠照捡箭矢。
            // 落地加速与档位无关：先盖戳再加速，避免把黑名单 INT_MAX 冲成 LastTry=0。
            if (wantSkip && dropOk) HoldSkipWhileYielding();
            if (wantFall && dropOk) TickDropFallBoost();
            Sleep(kIdleSleepMs);
            continue;
        }
        if (!anyReady) {
            if (now - lastMiss >= kMissLogMs) {
                lastMiss = now;
                const bool freeze = x::runtime::managed_main::IsLoginFrozen();
                const bool play = ports::world::IsPlayReady();
                const int scene = static_cast<int>(ports::world::GetSceneState());
                LogLine(
                    "petloot wait: drop=%d pet=%d want(f=%d p=%d c=%d) freeze=%d play=%d "
                    "scene=%d autoEnter=%d lu=%p pool=%p",
                    dropOk ? 1 : 0, petOk ? 1 : 0, wantFoot ? 1 : 0, wantPet ? 1 : 0,
                    wantChar ? 1 : 0, freeze ? 1 : 0, play ? 1 : 0, scene,
                    auto_enter::IsDesired() ? 1 : 0,
                    ports::drop::PeekLocalUser(), ports::drop::PeekDropPool());
            }
            if (wantFall && dropOk) TickDropFallBoost();
            Sleep(kIdleSleepMs);
            continue;
        }

        // probe：状态变化立即打；无变化仅 kProbeIdleMs 心跳（避免 drops=0 每 5s 刷盘）。
        if (gCfg.enabled) {
            float vacW = 0.f, vacH = 0.f;
            xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);
            ports::drop::ProbeSnapshot snap{};
            if (ports::drop::CollectProbe(snap, vacW * 0.5f, vacH * 0.5f)) {
                static int s_prevPet = -1;
                static unsigned s_prevSkill = 0;
                static int s_prevDrops = -1;
                static int s_prevNear = -1;
                const bool changed = snap.hasPet != (s_prevPet == 1) ||
                                     (unsigned)snap.petSkill != s_prevSkill ||
                                     snap.dropCount != s_prevDrops || snap.nearCount != s_prevNear;
                const bool due =
                    !lastForceProbe ||
                    (changed && now - lastForceProbe >= kForceLogMs) ||
                    (!changed && now - lastForceProbe >= kProbeIdleMs);
                if (due) {
                    lastForceProbe = now;
                    s_prevPet = snap.hasPet ? 1 : 0;
                    s_prevSkill = (unsigned)snap.petSkill;
                    s_prevDrops = snap.dropCount;
                    s_prevNear = snap.nearCount;
                    if (snap.hasSampleDrop && snap.sampleDropDist >= 0.f) {
                        LogLine(
                            "probe pet=%d skill=0x%X drops=%d near=%d "
                            "petRc=(%.1f,%.1f,%.1f,%.1f) collRc=(%.1f,%.1f,%.1f,%.1f) "
                            "pos=(%.1f,%.1f) drop=(%.1f,%.1f) d=%.1f",
                            snap.hasPet ? 1 : 0, (unsigned)snap.petSkill, snap.dropCount,
                            snap.nearCount, snap.petRc.x, snap.petRc.y, snap.petRc.w, snap.petRc.h,
                            snap.collisionRcPet.x, snap.collisionRcPet.y, snap.collisionRcPet.w,
                            snap.collisionRcPet.h, snap.petX, snap.petY, snap.sampleDropX,
                            snap.sampleDropY, snap.sampleDropDist);
                    } else {
                        LogLine(
                            "probe pet=%d skill=0x%X drops=%d near=%d "
                            "petRc=(%.1f,%.1f,%.1f,%.1f) collRc=(%.1f,%.1f,%.1f,%.1f) "
                            "pos=(%.1f,%.1f)",
                            snap.hasPet ? 1 : 0, (unsigned)snap.petSkill, snap.dropCount,
                            snap.nearCount, snap.petRc.x, snap.petRc.y, snap.petRc.w, snap.petRc.h,
                            snap.collisionRcPet.x, snap.collisionRcPet.y, snap.collisionRcPet.w,
                            snap.collisionRcPet.h, snap.petX, snap.petY);
                    }
                }
            }
        }

        DWORD interval = xcat::PetLootClampIntervalMs(gCfg.intervalMs);
        if (simple_combat::IsHighValueLootUrgent() &&
            interval > xcat::kPetLootHighValueIntervalMs) {
            interval = xcat::kPetLootHighValueIntervalMs;
        }
        // 脉冲边沿（Settling 武装 / 干等从关到开）：无视 interval 立刻吸一拍，吃满短落地窗。
        static uint32_t sPulseGen = 0;
        const uint32_t pulseGen = simple_combat::LootPulseGeneration();
        const bool pulseEdge = pulseGen != sPulseGen;
        if (pulseEdge) sPulseGen = pulseGen;
        const bool due = !lastTick || (now - lastTick >= interval) ||
                         (pulseEdge && simple_combat::IsLootPulseActive()) ||
                         simple_combat::IsHighValueLootUrgent();
        if (due) {
            lastTick = now;
            // 先盖戳再加速再吸：瞬落会写 LastTry=0，必须避开已盖的 INT_MAX。
            HoldSkipWhileYielding();
            TickDropFallBoost();
            Tick(now);
        } else {
            // 真空间隔内原生 50x60 仍在跑；每 40ms 盖戳，不把拦截缩成「只在出刀让路」。
            HoldSkipWhileYielding();
            TickDropFallBoost();
        }
        Sleep(kIdleSleepMs);
    }

    ports::drop::Shutdown();
    ports::pet::Shutdown();
    LogLineOd("pet_loot worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    xcat::PetLootSetDefaults(gCfg);
    LogLineOd("Init pet_loot");
}

void Shutdown() { StopWorker(); }

void ApplyConfig(const xcat::PetLootConfig& cfg) {
    gCfg = cfg;
    xcat::PetLootNormalize(gCfg);
    gSkipDirty = true;
    if (!gCfg.nativeVacEnabled) ports::drop::ReleaseByPetRectPack();
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    gWorkerThread.store(th);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr, std::memory_order_acq_rel);
    if (th) CloseHandle(th);
}

}  // namespace pet_loot
}  // namespace features
}  // namespace x
