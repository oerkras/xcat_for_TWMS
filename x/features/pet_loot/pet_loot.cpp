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

        std::vector<std::string> codes;
        const size_t total =
            xcat::ItemCatalogCollectCodesByNameContains(pack, r.nameKey, codes, kMaxIdsPerNameKey);
        if (total == 0) {
            ++missName;
            LogLine("skip miss key=\"%s\"", r.nameKey);
            continue;
        }

        const bool exactOne = (total == 1 && codes.size() == 1 &&
                               xcat::ItemCatalogLookupCodeByExactName(pack, r.nameKey)[0] != '\0');
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

    gSkipDirty = false;
    LogLine(
        "skip resolve filter=1 catalog=%d rules=%u → ids=%zu (itemId=%d exact=%d substr=%d miss=%d)",
        pack.loaded ? 1 : 0, gCfg.skipRuleCount, gSkipResolved.size(), fromId, fromExact, fromSub,
        missName);
}

const ports::drop::SkipIds* CurrentSkipIds() {
    if (gSkipDirty) RebuildSkipIds();
    return gSkipResolved.empty() ? nullptr : &gSkipResolved;
}

void Tick(DWORD now) {
    if (!gCfg.enabled && !gCfg.footEnabled && !gCfg.charVacEnabled) return;

    // 与自动打怪/瞬移共用 MainPump（drainBudget=2 + JobPrio）。泵拥堵时让路，否则吸物会把出刀饿死
    // （upload 211841：interval=50 + 全盒清闸 → combat fires 连续 40s 归零）。
    if (x::runtime::main_thread::IsCongested()) {
        static DWORD sCongLog = 0;
        if (!sCongLog || now - sCongLog > 2000) {
            sCongLog = now;
            LogLineOd("yield pump_congested q=%d (defer loot for combat)",
                      x::runtime::main_thread::QueuedJobCount());
        }
        return;
    }

    // 挂机时分复用：不出刀就吸；仅 Aim/Firing/Recover 让路（含拟人走路 MoveTo）。
    if (!simple_combat::IsLootPulseActive()) {
        static DWORD sFireLog = 0;
        if (!sFireLog || now - sFireLog > 2000) {
            sFireLog = now;
            LogLineOd("yield combat_fire_window (defer loot for Aim/Fire/Recover)");
        }
        return;
    }

    const ports::drop::SkipIds* skip = CurrentSkipIds();

    // 人物直吸 = 宠吸控制面，主体换成角色；盒子为近身可达范围（非宠吸全图）；
    // 官方 Send 不写 LastTry，拒收必须靠 sentDropId AddStall；burst 跟面板（1–5）。
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
    if (!ports::pet::ReadState(pst) || !pst.hasLocalUser) return;

    if (pst.activatedCount <= 0) {
        if (!gNoPetSince) gNoPetSince = now;
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
        ok = ports::drop::TryPetVacuum(vacW, vacH, skip, one);
        ++calls;
        vr = one;
        if (one.why && std::strcmp(one.why, "ok_absorbed") == 0) ++absorbedN;
        if (!ok && one.why && std::strcmp(one.why, "no_skill") == 0) break;
        if (one.why && (std::strcmp(one.why, "no_rect_patch") == 0 ||
                        std::strcmp(one.why, "seh") == 0 || std::strcmp(one.why, "no_pet") == 0 ||
                        std::strcmp(one.why, "no_lu") == 0))
            break;
        if (one.why && std::strcmp(one.why, "ok_empty") == 0) break;
        if (one.nearCount == 0 && !(one.why && std::strcmp(one.why, "ok_absorbed") == 0)) break;
    }

    if (!ok && vr.why && std::strcmp(vr.why, "no_skill") == 0) {
        if (!gLastNoSkillLog || now - gLastNoSkillLog >= kNoSkillLogMs) {
            gLastNoSkillLog = now;
            LogLineOd("mode=petmap pets=1 skill=0x%X skillSlot=0x%X why=no_skill (need PickupItem)",
                      (unsigned)vr.petSkill, (unsigned)vr.petSkillSlot);
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
    static int s_sentSameAcc = 0;
    static int s_sendTouchMax = 0;
    static int s_sentItemWhileMoney = 0;

    s_absorbAcc += absorbedN;
    ++s_tickAcc;
    if (vr.nearCount > s_nearMax) s_nearMax = vr.nearCount;
    if (vr.nearMoney > s_moneyMax) s_moneyMax = vr.nearMoney;
    if (vr.nearItem > s_itemMax) s_itemMax = vr.nearItem;
    if (vr.sentButPoolSame) ++s_sentSameAcc;
    if (vr.sendTouch > s_sendTouchMax) s_sendTouchMax = vr.sendTouch;
    if (vr.sentButPoolSame && vr.sendTouchMoney == 0 && vr.nearMoney > 0) ++s_sentItemWhileMoney;

    const bool hardErr =
        vr.why && (std::strcmp(vr.why, "seh") == 0 || std::strcmp(vr.why, "no_rect_patch") == 0);
    const bool errDue = hardErr && (!s_lastErr || now - s_lastErr >= kErrLogMs);
    const bool detailDue = !s_lastDetail || (now - s_lastDetail >= kDetailLogMs);
    // nearMax  alone 不算信号：站在吸不动的堆上会每间隔刷一行。
    const bool hasSignal = s_absorbAcc > 0 || s_sentSameAcc > 0 || hardErr;

    if (errDue) {
        s_lastErr = now;
        s_lastDetail = now;
        LogLineOd(
            "mode=petmap pets=%d skill=0x%X skillSlot=0x%X box=%.0fx%.0f mapVac=%d burst=%u/%u "
            "absorbed=%d drops=%d→%d Δ=%d fell=%d near=%d money=%d item=%d "
            "sampMoney=%d sampInfo=%d sendTouch=%d touchMoney=%d sentSame=%d "
            "gates=%d skipStamp=%d stall=%d/%d/%d own=%d lastTry=%d endPara=%d "
            "called=%d why=%s petSendΔ=%u poolSendΔ=%u skipN=%d",
            pst.activatedCount, (unsigned)vr.petSkill, (unsigned)vr.petSkillSlot, vacW, vacH,
            gCfg.mapVacuumEnabled ? 1 : 0, calls, burst, absorbedN, vr.dropCount, vr.dropCountAfter,
            vr.dropsDelta, vr.poolFellSinceLast ? 1 : 0, vr.nearCount, vr.nearMoney, vr.nearItem,
            vr.sampleIsMoney, vr.sampleInfo, vr.sendTouch, vr.sendTouchMoney, vr.sentButPoolSame,
            vr.gatesCleared, vr.skipStamped, vr.stallHeld, vr.stallStamped, vr.stallRestored,
            vr.sampleOwnType, vr.sampleLastTry, vr.sampleEndPara, vr.called ? 1 : 0,
            vr.why ? vr.why : "?", vr.petSendDelta, vr.poolSendDelta,
            skip ? (int)skip->size() : 0);
        s_absorbAcc = 0;
        s_tickAcc = 0;
        s_nearMax = s_moneyMax = s_itemMax = 0;
        s_sentSameAcc = s_sendTouchMax = s_sentItemWhileMoney = 0;
    } else if (detailDue && hasSignal) {
        s_lastDetail = now;
        LogLine(
            "mode=petmap pets=%d skill=0x%X skillSlot=0x%X box=%.0fx%.0f mapVac=%d burst=%u/%u "
            "absorbed=%d ticks=%d nearMax=%d moneyMax=%d itemMax=%d "
            "sentSame=%d touchMax=%d itemWhileMoney=%d "
            "drops=%d→%d Δ=%d fell=%d near=%d money=%d item=%d "
            "sampMoney=%d sampInfo=%d sendTouch=%d touchMoney=%d "
            "gates=%d skipStamp=%d stall=%d/%d/%d own=%d lastTry=%d endPara=%d "
            "called=%d why=%s petSendΔ=%u poolSendΔ=%u skipN=%d",
            pst.activatedCount, (unsigned)vr.petSkill, (unsigned)vr.petSkillSlot, vacW, vacH,
            gCfg.mapVacuumEnabled ? 1 : 0, calls, burst, s_absorbAcc, s_tickAcc, s_nearMax,
            s_moneyMax, s_itemMax, s_sentSameAcc, s_sendTouchMax, s_sentItemWhileMoney, vr.dropCount,
            vr.dropCountAfter, vr.dropsDelta, vr.poolFellSinceLast ? 1 : 0, vr.nearCount,
            vr.nearMoney, vr.nearItem, vr.sampleIsMoney, vr.sampleInfo, vr.sendTouch,
            vr.sendTouchMoney, vr.gatesCleared, vr.skipStamped, vr.stallHeld, vr.stallStamped,
            vr.stallRestored, vr.sampleOwnType, vr.sampleLastTry, vr.sampleEndPara,
            vr.called ? 1 : 0, vr.why ? vr.why : "?", vr.petSendDelta, vr.poolSendDelta,
            skip ? (int)skip->size() : 0);
        s_absorbAcc = 0;
        s_tickAcc = 0;
        s_nearMax = s_moneyMax = s_itemMax = 0;
        s_sentSameAcc = s_sendTouchMax = s_sentItemWhileMoney = 0;
    }

    // 大量掉落被服端持续拒收：宠吸已跳过它们继续吸别的，但用户看到的是「吸不动」，给一条人话提示
    static DWORD s_lastInvHint = 0;
    if (vr.stallHeld >= kInvFullHintStall &&
        (!s_lastInvHint || now - s_lastInvHint >= kInvFullHintMs)) {
        s_lastInvHint = now;
        LogLine("mode=petmap hint=inv_full_suspect stall=%d near=%d money=%d item=%d "
                "(服端持续拒收，多为背包某栏已满；已跳过塞不进的道具继续吸其余)",
                vr.stallHeld, vr.nearCount, vr.nearMoney, vr.nearItem);
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

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        x::ipc::PayloadPetLoot_Poll();

        const uint32_t cfgSig = (gCfg.enabled ? 1u : 0u) | (gCfg.footEnabled ? 2u : 0u) |
                                (gCfg.mapVacuumEnabled ? 4u : 0u) |
                                (gCfg.charVacEnabled ? 8u : 0u) | (gCfg.skipRuleCount << 8) |
                                (gCfg.skipFilterEnabled ? 0x80000000u : 0);
        const uint32_t cfgSig2 = (gCfg.intervalMs & 0xFFFFu) | (gCfg.burstPerTick << 16);
        if (cfgSig != lastCfgSig || cfgSig2 != lastCfgSig2) {
            lastCfgSig = cfgSig;
            lastCfgSig2 = cfgSig2;
            float vacW = 0.f, vacH = 0.f;
            xcat::PetLootEffectiveVacuum(gCfg, vacW, vacH);
            float charHW = 0.f, charHH = 0.f;
            xcat::PetLootEffectiveCharHalf(gCfg, charHW, charHH);
            LogLineOd("config pet=%d foot=%d charVac=%d mapVac=%d interval=%u burst=%u "
                      "box=%.0fx%.0f(near) footBox=50x60(native) charBox=%.0fx%.0f filters=0x%X "
                      "skipFilter=%d skipRules=%u",
                    gCfg.enabled ? 1 : 0, gCfg.footEnabled ? 1 : 0, gCfg.charVacEnabled ? 1 : 0,
                    gCfg.mapVacuumEnabled ? 1 : 0, gCfg.intervalMs, gCfg.burstPerTick, vacW, vacH,
                    charHW * 2.f, charHH * 2.f, gCfg.filterFlags, gCfg.skipFilterEnabled ? 1 : 0,
                    gCfg.skipRuleCount);
        }

        // 脚边 / 人物直吸：DropPool+MyUser；宠吸：额外 pet_port
        const bool wantFoot = gCfg.footEnabled != 0;
        const bool wantPet = gCfg.enabled != 0;
        const bool wantChar = gCfg.charVacEnabled != 0;
        if (!wantFoot && !wantPet && !wantChar) {
            Sleep(kIdleSleepMs);
            continue;
        }

        const bool dropOk = ports::drop::EnsureBound();
        const bool petOk = !wantPet || ports::pet::EnsureBound();
        const bool anyReady = dropOk && (wantFoot || wantChar || (wantPet && petOk));
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

        const DWORD interval = xcat::PetLootClampIntervalMs(gCfg.intervalMs);
        // 脉冲边沿（Settling 武装 / 干等从关到开）：无视 interval 立刻吸一拍，吃满短落地窗。
        static uint32_t sPulseGen = 0;
        const uint32_t pulseGen = simple_combat::LootPulseGeneration();
        const bool pulseEdge = pulseGen != sPulseGen;
        if (pulseEdge) sPulseGen = pulseGen;
        const bool due = !lastTick || (now - lastTick >= interval) ||
                         (pulseEdge && simple_combat::IsLootPulseActive());
        if (due) {
            lastTick = now;
            Tick(now);
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
