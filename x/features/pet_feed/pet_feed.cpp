// Classic TWMS — pet_feed P0c auto-summon (official feed handles fullness).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pet_feed.h"

#include "../ports/pet_port.h"
#include "../ports/world_port.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"

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
constexpr DWORD kHavePetPollMs = 10000;
constexpr DWORD kIdleSleepMs = 80;
constexpr DWORD kForceLogMs = 5000;
constexpr DWORD kMissLogMs = 15000;
constexpr DWORD kNotifyMs = 60000;
constexpr DWORD kPendingMs = 5000;

std::atomic<bool> gDesired{false};
std::atomic<bool> gRequireFood{true};  // 与 common 默认一致：有粮才召，防召出即饿
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
HANDLE gLog = INVALID_HANDLE_VALUE;

DWORD gPendingUntil = 0;
DWORD gLastNotifyFood = 0;
DWORD gLastNotifyPet = 0;
DWORD gLastSummonAt = 0;

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

void Tick(DWORD now) {
    if (!gDesired.load(std::memory_order_relaxed)) {
        gPendingUntil = 0;
        return;
    }
    if (!ports::world::IsPlayReady()) return;
    if (!ports::pet::EnsureBound()) return;

    ports::pet::PetCareState st{};
    if (!ports::pet::ReadState(st) || !st.hasLocalUser) return;

    if (st.activatedCount >= 1) {
        gPendingUntil = 0;
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
        const DWORD interval =
            (got && st.hasLocalUser && st.activatedCount == 0) ? kEmptyPollMs : kHavePetPollMs;

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

void SetDesired(bool on) { gDesired.store(on, std::memory_order_relaxed); }

void SetRequireFood(bool on) { gRequireFood.store(on, std::memory_order_relaxed); }

}  // namespace pet_feed
}  // namespace features
}  // namespace x
