// auto_stat — Classic TWMS 自动加点。
// 策略对照 Maplecat UseAP（权重总和=5、贪心每次 +1、属性 +1 确认）。
// 发包：泵上直调官方 UIStat.ed6479da(uint flag)，不造包、不开属性窗。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "auto_stat.h"

#include "../../ipc/payload_auto_stat.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"
#include "../auto_lie/auto_lie.h"
#include "../ports/world_port.h"

#include "xcat_auto_stat.h"

#include <atomic>
#include <cstring>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x::features::auto_stat {
namespace {

constexpr DWORD kCooldownMs = 900;
constexpr DWORD kPollMs = 400;
constexpr DWORD kIdleSleepMs = 500;
constexpr DWORD kActiveSleepMs = 50;
constexpr DWORD kJobWaitMs = 800;
constexpr int kMaxFail = 5;

constexpr char kUiStatClassHash[] =
    "d66bc81d6a7f262b21a014fb50217f67d576c23fb110baecd6a2be282380705";
constexpr char kSendApMethodHash[] =
    "ed6479da555ecd19988ffcc43eaffa328cfdb7cba294ce4da2f95c668b4f236";
constexpr uint32_t kRvaSendAp = 0x64A200;

// CharacterStatFlag（UIStat.OnButtonClicked 立即数）
constexpr uint32_t kFlagStr = 0x40;
constexpr uint32_t kFlagDex = 0x80;
constexpr uint32_t kFlagInt = 0x100;
constexpr uint32_t kFlagLuk = 0x200;

constexpr int kStatCount = 4;
constexpr const char* kStatName[kStatCount] = {"STR", "DEX", "INT", "LUK"};
constexpr uint32_t kStatFlag[kStatCount] = {kFlagStr, kFlagDex, kFlagInt, kFlagLuk};

using FnSendAp = void (*)(void* self, uint32_t flag, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer = nullptr;
    void* virtualMethodPointer = nullptr;
};

xcat::AutoStatConfig gCfg{};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

void* gKlassUiStat = nullptr;
MethodInfoHead* gMiSendAp = nullptr;
FnSendAp gFnSendAp = nullptr;

int64_t gAlloc[kStatCount]{};
uint32_t gLastCharId = 0;
DWORD gLastUseMs = 0;
DWORD gLastPoll = 0;
int gFailStreak = 0;
bool gPendingVerify = false;
int gLastPick = -1;
int64_t gStatBefore = 0;
bool gLoggedResolve = false;
bool gLoggedOffs = false;
bool gLieBusy = false;

struct SendJobCtx {
    uint32_t flag = 0;
    bool invoked = false;  // 已真正调到官方函数（不含泵拒/解析失败）
    bool seh = false;
};

enum class SendResult { Transient = 0, HardFail = 1, Ok = 2 };

int RatioAt(int idx) {
    switch (idx) {
        case 0:
            return static_cast<int>(gCfg.str);
        case 1:
            return static_cast<int>(gCfg.dex);
        case 2:
            return static_cast<int>(gCfg.intel);
        case 3:
            return static_cast<int>(gCfg.luk);
        default:
            return 0;
    }
}

int16_t BaseStatByIdx(const x::ui::player::BaseApStats& st, int idx) {
    switch (idx) {
        case 0:
            return st.str;
        case 1:
            return st.dex;
        case 2:
            return st.intel;
        case 3:
            return st.luk;
        default:
            return 0;
    }
}

void ResetPending() {
    gPendingVerify = false;
    gLastPick = -1;
    gStatBefore = 0;
}

void ResetFail() { gFailStreak = 0; }

void ResetAlloc() {
    for (int i = 0; i < kStatCount; ++i) gAlloc[i] = 0;
}

void PersistDisabled(const char* why) {
    gCfg.enabled = 0;
    ResetPending();
    gCfg.writeTickMs = GetTickCount64();
    if (!xcat::WriteAutoStat(x::runtime::GetBinDir(), gCfg)) {
        x::runtime::LogW("AutoStat", "停机写盘失败 (%s)", why ? why : "?");
    } else {
        x::runtime::LogW("AutoStat", "已停机并写回 ini (%s)", why ? why : "?");
    }
}

bool ResolveSendOnMain() {
    if (gFnSendAp) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;

    if (!gKlassUiStat) {
        gKlassUiStat = x::runtime::il2cpp::FindClass("", kUiStatClassHash);
        if (!gKlassUiStat) gKlassUiStat = x::runtime::il2cpp::FindClass("", "UIStat");
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kShape{1, TypeKind::Void, false, true, {TypeKind::U32}};

    if (gKlassUiStat) {
        const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            gKlassUiStat, kRvaSendAp, kShape, nullptr, kSendApMethodHash);
        if (mr.method) {
            gMiSendAp = reinterpret_cast<MethodInfoHead*>(mr.method);
            if (gMiSendAp && gMiSendAp->methodPointer) {
                gFnSendAp = reinterpret_cast<FnSendAp>(gMiSendAp->methodPointer);
            }
        }
    }
    if (!gFnSendAp) {
        gFnSendAp = x::runtime::il2cpp::AtRva<FnSendAp>(kRvaSendAp);
    }
    if (gFnSendAp && !gLoggedResolve) {
        gLoggedResolve = true;
        x::runtime::LogI("AutoStat", "SendAp fn=%p MI=%p klass=%p rva=0x%X",
                         reinterpret_cast<void*>(gFnSendAp), static_cast<void*>(gMiSendAp),
                         gKlassUiStat, kRvaSendAp);
    }
    return gFnSendAp != nullptr;
}

void SendJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<SendJobCtx*>(user);
    if (!ctx) return;
    ctx->invoked = false;
    ctx->seh = false;
    __try {
        if (!ResolveSendOnMain() || !gFnSendAp) return;
        // 组包不用 this；官方 OnButtonClicked 也只把 flag 传下去。MI 与喝药一致传 null。
        gFnSendAp(nullptr, ctx->flag, nullptr);
        ctx->invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("auto_stat.SendAp");
        ctx->seh = true;
        ctx->invoked = false;
        x::runtime::LogW("AutoStat", "SendAp SEH flag=0x%X", ctx->flag);
    }
}

SendResult SendAp(int idx) {
    if (idx < 0 || idx >= kStatCount) return SendResult::Transient;
    SendJobCtx ctx{};
    ctx.flag = kStatFlag[idx];
    if (!x::runtime::main_thread::InvokeAndWait(&SendJobOnMain, &ctx, kJobWaitMs,
                                                x::runtime::main_thread::JobPrio::Low)) {
        x::runtime::LogWThrottled(230, 3000, "AutoStat", "SendAp pump timeout/reject %s",
                                  kStatName[idx]);
        return SendResult::Transient;
    }
    if (ctx.seh) return SendResult::HardFail;
    if (!ctx.invoked) {
        x::runtime::LogWThrottled(231, 3000, "AutoStat", "SendAp resolve miss %s", kStatName[idx]);
        return SendResult::Transient;
    }
    return SendResult::Ok;
}

void Tick(DWORD now) {
    if (!gCfg.enabled) return;
    if (!xcat::AutoStatRatioOk(gCfg)) return;
    // 离图 / 测谎只跳过：保留 pending，避免过门或轨迹踢期间清确认态导致连发。
    if (!ports::world::IsPlayReady()) return;
    if (auto_lie::IsBusy()) {
        if (!gLieBusy) {
            gLieBusy = true;
            x::runtime::LogI("AutoStat", "测谎中，暂停加点");
        }
        return;
    }
    if (gLieBusy) {
        gLieBusy = false;
        x::runtime::LogI("AutoStat", "测谎结束，恢复加点");
    }
    if (gLastPoll && static_cast<int>(now - gLastPoll) < static_cast<int>(kPollMs)) return;
    gLastPoll = now;
    if (gLastUseMs && static_cast<int>(now - gLastUseMs) < static_cast<int>(kCooldownMs)) return;

    x::ui::player::BaseApStats st{};
    if (!x::ui::player::ReadBaseApStats(st) || !st.ok) return;

    if (!gLoggedOffs) {
        gLoggedOffs = true;
        x::runtime::LogI("AutoStat", "read ok cid=%u ap=%d STR=%d DEX=%d INT=%d LUK=%d",
                         st.characterId, st.ap, st.str, st.dex, st.intel, st.luk);
    }

    if (st.characterId != 0 && gLastCharId != 0 && st.characterId != gLastCharId) {
        ResetAlloc();
        ResetPending();
        ResetFail();
        x::runtime::LogI("AutoStat", "换角色 cid %u → %u，配比计数清零", gLastCharId,
                         st.characterId);
    }
    if (st.characterId != 0) gLastCharId = st.characterId;

    if (gPendingVerify) {
        const int64_t cur = BaseStatByIdx(st, gLastPick);
        if (cur > gStatBefore) {
            gPendingVerify = false;
            ResetFail();
        } else {
            ++gFailStreak;
            gLastUseMs = now;  // 确认失败也走 900ms，避免 400ms 连加停机
            x::runtime::LogW("AutoStat", "上次 +%s 后属性未 +1（%lld→%lld）streak=%d",
                             (gLastPick >= 0 && gLastPick < kStatCount) ? kStatName[gLastPick] : "?",
                             static_cast<long long>(gStatBefore), static_cast<long long>(cur),
                             gFailStreak);
            if (gFailStreak >= kMaxFail) PersistDisabled("确认超时");
            return;
        }
    }

    if (st.ap <= 0) return;

    int pick = -1;
    double best = 0.0;
    for (int i = 0; i < kStatCount; ++i) {
        const int ratio = RatioAt(i);
        if (ratio <= 0) continue;
        const double score = static_cast<double>(gAlloc[i]) / static_cast<double>(ratio);
        if (pick < 0 || score < best) {
            best = score;
            pick = i;
        }
    }
    if (pick < 0) return;

    gLastUseMs = now;  // 含瞬时失败：避免泵堵时 400ms 连打 InvokeAndWait
    gLastPick = pick;
    gStatBefore = BaseStatByIdx(st, pick);
    const SendResult sent = SendAp(pick);
    if (sent == SendResult::Ok) {
        ++gAlloc[pick];
        gPendingVerify = true;
        x::runtime::LogI("AutoStat",
                         "+1 %s（ap=%d 累计 S%lld/D%lld/I%lld/L%lld）", kStatName[pick], st.ap,
                         static_cast<long long>(gAlloc[0]), static_cast<long long>(gAlloc[1]),
                         static_cast<long long>(gAlloc[2]), static_cast<long long>(gAlloc[3]));
        return;
    }
    ResetPending();
    if (sent == SendResult::Transient) return;
    ++gFailStreak;
    if (gFailStreak >= kMaxFail) PersistDisabled("连续发包失败");
}

DWORD WINAPI WorkerProc(void*) {
    timeBeginPeriod(1);
    x::runtime::LogI("AutoStat", "worker start");
    DWORD lastCfgPoll = 0;
    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        const bool want = gCfg.enabled != 0 && xcat::AutoStatRatioOk(gCfg);
        const DWORD cfgGap = want ? kPollMs : kIdleSleepMs;
        if (!lastCfgPoll || static_cast<int>(now - lastCfgPoll) >= static_cast<int>(cfgGap)) {
            lastCfgPoll = now;
            x::ipc::PayloadAutoStat_Poll();
        }
        if (!(gCfg.enabled != 0 && xcat::AutoStatRatioOk(gCfg))) {
            Sleep(kIdleSleepMs);
            continue;
        }
        Tick(GetTickCount());
        Sleep(kActiveSleepMs);
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    xcat::AutoStatSetDefaults(gCfg);
    ResetAlloc();
    ResetPending();
    ResetFail();
    gLastCharId = 0;
    gLastUseMs = 0;
    gLastPoll = 0;
    gKlassUiStat = nullptr;
    gMiSendAp = nullptr;
    gFnSendAp = nullptr;
    gLoggedResolve = false;
    gLoggedOffs = false;
    gLieBusy = false;
    x::runtime::LogI("Feature", "auto_stat ready (off until [auto_stat] enabled + ratio=5)");
}

void Shutdown() { StopWorker(); }

void ApplyConfig(const xcat::AutoStatConfig& cfg) {
    const bool was = gCfg.enabled != 0;
    gCfg = cfg;
    xcat::AutoStatNormalize(gCfg);
    const bool on = gCfg.enabled != 0;
    if (was != on) {
        ResetPending();
        ResetFail();
        x::runtime::LogI("AutoStat", "开关=%s 比例 STR=%u DEX=%u INT=%u LUK=%u",
                         on ? "开" : "关", gCfg.str, gCfg.dex, gCfg.intel, gCfg.luk);
    }
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE h = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    gWorkerThread.store(h);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr, std::memory_order_acq_rel);
    if (th) CloseHandle(th);
}

}  // namespace x::features::auto_stat
