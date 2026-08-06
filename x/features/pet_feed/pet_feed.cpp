// Classic TWMS — pet_feed P0c auto-summon (official feed handles fullness).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pet_feed.h"

#include "../ports/pet_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/world_port.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../ui/player_vitals.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x {
namespace features {
namespace pet_feed {
namespace {

constexpr DWORD kEmptyPollMs = 2500;
constexpr DWORD kEmptyPollFastMs = 400;  // 进图后等宠窗：加快首召，赶在打怪武装前
constexpr DWORD kHavePetPollMs = 10000;
constexpr DWORD kIdleSleepMs = 80;
constexpr DWORD kForceLogMs = 5000;
constexpr DWORD kMissLogMs = 15000;
constexpr DWORD kNotifyMs = 60000;
constexpr DWORD kPendingMs = 5000;
    // 与 simple_combat 协调：开召宠时打怪最多等这么久；超时放行以免卡死挂机。
    // 预算从「打怪侧真正询问让路」起算；未落地/警戒 defer 会 PauseHoldBudget 清零。
    constexpr DWORD kHoldCombatMaxMs = 20000;
// UserBase 短 IsAlertMode：LocalUser alert stamp > 0（与 drop_alert 同字段）
// hash → field_get_offset；dump fallback 0x114
constexpr char kUserAlertClass[] =
    "c99c0bcb0549788a98e73a02acc1cf7e5476d3f920f9a4f5f69a76490798a16";
constexpr char kHashAlertAt[] =
    "c469c323e5afda2bab68c386c87ea8b571b3fd726ece08d92aa459848a6d351";
constexpr size_t kFbAlertAt = 0x114;
size_t gOffAlertAt = kFbAlertAt;
bool gAlertFieldTried = false;

bool AlertFieldOffHit(void* klass, const char* nameHash, size_t fb, size_t* out) {
    if (!klass || !nameHash || !out || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, nameHash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (off >= 0x10 && off < 0x800) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    *out = fb;
    return false;
}

void EnsureAlertFieldOff() {
    if (gAlertFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gAlertFieldTried = true;
    void* klass = x::runtime::il2cpp::FindClass("", kUserAlertClass);
    if (!klass) klass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    const bool hit = AlertFieldOffHit(klass, kHashAlertAt, kFbAlertAt, &gOffAlertAt);
    x::runtime::LogI("PetFeed", "alert field path=%s off=0x%zX", hit ? "meta" : "fallback",
                     gOffAlertAt);
}

std::atomic<bool> gDesired{false};
std::atomic<bool> gRequireFood{true};  // 与 common 默认一致：有粮才召，防召出即饿
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
HANDLE gLog = INVALID_HANDLE_VALUE;

DWORD gPendingUntil = 0;
DWORD gLastNotifyFood = 0;
DWORD gLastNotifyPet = 0;
DWORD gLastNotifyAlert = 0;
DWORD gLastNotifyLand = 0;
DWORD gLastSummonAt = 0;

// 供 simple_combat 只读：场上宠态 / 是否仍值得等召。
std::atomic<int> gActivatedCount{-1};       // -1=尚未读到
std::atomic<int> gCanSummonNow{0};          // 1=有可召宠且（粮条件满足）
std::atomic<int> gPermanentSkip{0};         // 1=无宠/无粮等永久跳过
std::atomic<DWORD> gHoldCombatSince{0};     // 开始要求打怪让路的 tick；0=未持门

void WriteLogHandle(HANDLE h, const char* buf, int n) {
    if (h == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, nullptr);
}

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
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"petfeed.log");
}

void LogLine(const char* fmt, ...) {
    char body[1200];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1400];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteLogHandle(gLog, buf, n);
    OutputDebugStringA(buf);
    x::runtime::LogI("PetFeed", "%s", body);
}

bool ReadAlertMode() {
    EnsureAlertFieldOff();
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) return false;
    int stamp = 0;
    __try {
        stamp = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(lu) + gOffAlertAt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // IsAlertMode：field > (seed+IMM)=0 → 非 0 即警戒
    return stamp > 0;
}

void PublishHoldState(const ports::pet::PetCareState& st, DWORD now, bool got) {
    if (!got || !st.hasLocalUser) return;
    gActivatedCount.store(st.activatedCount, std::memory_order_relaxed);
    const bool reqFood = gRequireFood.load(std::memory_order_relaxed);
    const bool can =
        st.activatedCount < 1 && st.summonPetPos > 0 && (!reqFood || st.hasFood);
    gCanSummonNow.store(can ? 1 : 0, std::memory_order_relaxed);
    const bool permanent =
        st.activatedCount < 1 &&
        (st.summonPetPos <= 0 || (reqFood && !st.hasFood));
    gPermanentSkip.store(permanent ? 1 : 0, std::memory_order_relaxed);

    if (!gDesired.load(std::memory_order_relaxed) || st.activatedCount >= 1 || permanent) {
        gHoldCombatSince.store(0, std::memory_order_relaxed);
    }
    // 不在此处起算 hold 预算：须等 simple_combat 真正让路时再计（防 F5 关着/arm 窗空耗 20s）。
    (void)now;
}

// 召唤侧 defer（未落地/警戒）：暂停预算，避免时钟空跑后打怪放行又抢开火。
void PauseHoldBudget() { gHoldCombatSince.store(0, std::memory_order_relaxed); }

void ClearHoldPublish() {
    gActivatedCount.store(-1, std::memory_order_relaxed);
    gCanSummonNow.store(0, std::memory_order_relaxed);
    gPermanentSkip.store(0, std::memory_order_relaxed);
    gHoldCombatSince.store(0, std::memory_order_relaxed);
}

void Tick(DWORD now) {
    if (!gDesired.load(std::memory_order_relaxed)) {
        gPendingUntil = 0;
        ClearHoldPublish();
        return;
    }
    if (!ports::world::IsPlayReady()) {
        ClearHoldPublish();
        return;
    }
    if (!ports::pet::EnsureBound()) return;

    ports::pet::PetCareState st{};
    if (!ports::pet::ReadState(st) || !st.hasLocalUser) return;
    PublishHoldState(st, now, true);

    if (st.activatedCount >= 1) {
        gPendingUntil = 0;
        return;
    }

    // 落地闩未就绪：再等一会儿，避免进图瞬间接包被拒。
    if (!x::ui::player::IsReadyLatched()) {
        PauseHoldBudget();
        if (!gLastNotifyLand || now - gLastNotifyLand >= 2000) {
            gLastNotifyLand = now;
            LogLine("summon wait: not_landed (readyLatch=0)");
        }
        return;
    }

    // 战斗警戒态：CanPerformAction 会弹「现在无法进行」；等警戒自然落再召。
    if (ReadAlertMode()) {
        PauseHoldBudget();
        if (!gLastNotifyAlert || now - gLastNotifyAlert >= 2000) {
            gLastNotifyAlert = now;
            LogLine("summon wait: alert_mode (defer activate)");
        }
        return;
    }

    if (gPendingUntil && now < gPendingUntil) return;
    gPendingUntil = 0;

    if (gRequireFood.load(std::memory_order_relaxed) && !st.hasFood) {
        if (!gLastNotifyFood || now - gLastNotifyFood >= kNotifyMs) {
            gLastNotifyFood = now;
            LogLine("summon skip: no food (requireFood=1; need Consume 212xxxx; cashPets=%d "
                    "dead=%d summonPos=%d remain=%d active=%d deadByDate=%d)",
                    st.cashPetCount, st.deadPetCount, st.summonPetPos, st.probeRemainLife,
                    st.probeActiveState, st.probeDeadByDate);
        }
        return;
    }
    if (st.summonPetPos <= 0) {
        if (!gLastNotifyPet || now - gLastNotifyPet >= kNotifyMs) {
            gLastNotifyPet = now;
            LogLine("summon skip: no inactive living pet (cash=%d dead=%d)", st.cashPetCount,
                    st.deadPetCount);
        }
        return;
    }

    LogLine("summon try pos=%d foodId=%d qty=%d", st.summonPetPos, st.foodItemId, st.foodQty);
    const bool ok = ports::pet::TryActivatePet(st.summonPetPos);
    gPendingUntil = now + kPendingMs;
    gLastSummonAt = now;
    LogLine("summon %s pos=%d pending=%ums", ok ? "queued" : "fail", st.summonPetPos,
            (unsigned)kPendingMs);
}

DWORD WINAPI Worker(LPVOID) {
    timeBeginPeriod(1);
    OpenLog();
    LogLine("pet_feed P0c summon worker start");
    ports::pet::Init();

    DWORD lastScan = 0;
    DWORD lastForceLog = 0;
    DWORD lastMissLog = 0;
    int lastActivated = -1;
    int lastSummon = -1;
    int lastFood = -1;
    bool lastDesired = false;
    bool lastRequireFood = false;
    bool haveCfgSnap = false;

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        x::ipc::PayloadControl_Poll();

        const bool on = gDesired.load(std::memory_order_relaxed);
        const bool reqFood = gRequireFood.load(std::memory_order_relaxed);
        if (!haveCfgSnap || on != lastDesired || reqFood != lastRequireFood) {
            haveCfgSnap = true;
            lastDesired = on;
            lastRequireFood = reqFood;
            LogLine("config enabled=%d requireFood=%d", on ? 1 : 0, reqFood ? 1 : 0);
            if (!on) gPendingUntil = 0;
        }

        if (!ports::world::IsPlayReady()) {
            ClearHoldPublish();
            if (on && (!lastMissLog || now - lastMissLog >= kMissLogMs)) {
                lastMissLog = now;
                LogLine("petfeed wait: not_play_ready (scene=%d wmAlive=%d)",
                        static_cast<int>(ports::world::GetSceneState()),
                        ports::world::IsAlive() ? 1 : 0);
            }
            Sleep(kIdleSleepMs);
            continue;
        }

        if (!ports::pet::EnsureBound()) {
            if (now - lastMissLog >= kMissLogMs) {
                lastMissLog = now;
                LogLine("petfeed wait: GameAssembly / MyUser / WorldManager");
            }
            Sleep(kIdleSleepMs);
            continue;
        }

        ports::pet::PetCareState st{};
        const bool got = ports::pet::ReadState(st);
        if (got) PublishHoldState(st, now, true);
        const bool empty =
            got && st.hasLocalUser && st.activatedCount == 0;
        // 等宠窗内加快轮询，缩短「落地→召出」空窗，避免打怪抢先开火。
        const bool fastEmpty =
            empty && on && gCanSummonNow.load(std::memory_order_relaxed) != 0;
        const DWORD interval =
            empty ? (fastEmpty ? kEmptyPollFastMs : kEmptyPollMs) : kHavePetPollMs;

        if (lastScan && now - lastScan < interval) {
            Sleep(kIdleSleepMs);
            continue;
        }
        lastScan = now;

        if (got) {
            const bool changed = st.activatedCount != lastActivated ||
                                 st.summonPetPos != lastSummon || (int)st.hasFood != lastFood;
            const bool force = !lastForceLog || (now - lastForceLog >= kForceLogMs);
            if (changed || force) {
                lastActivated = st.activatedCount;
                lastSummon = st.summonPetPos;
                lastFood = st.hasFood ? 1 : 0;
                lastForceLog = now;
                LogLine(
                    "petfeed on=%d lu=%d act=%d full=%d food=%d foodId=%d summonPos=%d cash=%d "
                    "dead=%d remain=%d active=%d deadByDate=%d pending=%d",
                    on ? 1 : 0, st.hasLocalUser ? 1 : 0, st.activatedCount, st.minRepleteness,
                    st.hasFood ? 1 : 0, st.foodItemId, st.summonPetPos, st.cashPetCount,
                    st.deadPetCount, st.probeRemainLife, st.probeActiveState, st.probeDeadByDate,
                    (gPendingUntil && now < gPendingUntil) ? 1 : 0);
            }
        }

        Tick(now);
    }

    ports::pet::Shutdown();
    LogLine("pet_feed worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    LogLine("Init P0c summon-only");
}

void Shutdown() {
    StopWorker();
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void StartWorker() {
    if (gWorkerThread.load(std::memory_order_acquire)) return;
    gWorkerStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!th) {
        LogLine("StartWorker CreateThread failed");
        return;
    }
    gWorkerThread.store(th, std::memory_order_release);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        // 短 join：对齐 fly/attack；DllMain DETACH 下亦有同仓先例（≤3s）
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

void SetDesired(bool on) {
    gDesired.store(on, std::memory_order_relaxed);
    if (!on) ClearHoldPublish();
}

void SetRequireFood(bool on) { gRequireFood.store(on, std::memory_order_relaxed); }

bool IsDesired() { return gDesired.load(std::memory_order_relaxed); }

bool ShouldHoldCombatForSummon() {
    if (!gDesired.load(std::memory_order_relaxed)) return false;
    if (!ports::world::IsPlayReady()) return false;

    const int act = gActivatedCount.load(std::memory_order_relaxed);
    if (act >= 1) return false;
    if (gPermanentSkip.load(std::memory_order_relaxed) != 0) return false;

    // 尚未读到态：短暂让路；已确认可召：持续让路到召出/超时。
    const bool shouldHold =
        (act < 0) || (gCanSummonNow.load(std::memory_order_relaxed) != 0);
    if (!shouldHold) return false;

    // 预算仅在打怪侧真正询问让路时起算（F5 关着不空耗）。
    const DWORD now = GetTickCount();
    DWORD since = gHoldCombatSince.load(std::memory_order_relaxed);
    if (since == 0) {
        const DWORD start = now ? now : 1;
        DWORD expected = 0;
        if (gHoldCombatSince.compare_exchange_strong(expected, start,
                                                    std::memory_order_relaxed)) {
            since = start;
        } else {
            since = expected ? expected : start;
        }
    }
    if (now - since >= kHoldCombatMaxMs) return false;
    return true;
}

}  // namespace pet_feed
}  // namespace features
}  // namespace x
