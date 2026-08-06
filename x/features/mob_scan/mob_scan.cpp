// Classic TWMS — mob scan worker (P1 probe for simple_combat).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_scan.h"

#include "../../../common/xcat_payload_control.h"
#include "../ports/foothold_port.h"
#include "../ports/foothold_path.h"
#include "../ports/mob_pool_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
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
namespace mob_scan {
namespace {

// BIN：攻击加速下缓存 360ms 会拖尸体空砍；打怪开启时用面板周期（默认 50）对齐同行瞬切。
constexpr DWORD kScanIntervalIdleMs = 360;
constexpr DWORD kIdleSleepMs = 15;
constexpr DWORD kForceLogMs = 5000;
constexpr DWORD kCountLogMs = 500;  // count 变：短摘要写盘上限
constexpr DWORD kMissLogMs = 15000;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<uint32_t> gCombatIntervalMs{xcat::kMobScanIntervalDefaultMs};
HANDLE gLog = INVALID_HANDLE_VALUE;

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
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"mobscan.log");
}

// 高频扫描摘要只写 mobscan.log；低频事件才 LogI→x.jsonl。
void LogToFile(const char* fmt, ...) {
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
    x::runtime::LogI("MobScan", "%s", body);
}

DWORD WINAPI Worker(LPVOID) {
    timeBeginPeriod(1);
    OpenLog();
    LogLine("mob_scan worker start idle=%ums combat=%ums", (unsigned)kScanIntervalIdleMs,
            (unsigned)gCombatIntervalMs.load(std::memory_order_acquire));

    DWORD lastScan = 0;
    DWORD lastForceLog = 0;
    DWORD lastCountLog = 0;
    DWORD lastMissLog = 0;
    int lastCount = -1;
    int lastSlots = -999;
    int lastFhMapId = -1;
    bool lastCombat = false;
    DWORD lastCombatInterval = 0;

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        if (!ports::mob::EnsureBound()) {
            if (now - lastMissLog >= kMissLogMs) {
                lastMissLog = now;
                LogLine("mobscan wait: GameAssembly / class bind");
            }
            Sleep(kIdleSleepMs);
            continue;
        }

        if (!ports::world::IsPlayReady()) {
            lastFhMapId = -1;  // 离开地图后重进同图也要再 dump
            if (now - lastMissLog >= kMissLogMs) {
                lastMissLog = now;
                const int st = static_cast<int>(ports::world::GetSceneState());
                LogLine("mobscan wait: not play ready scene=%d", st);
            }
            Sleep(kIdleSleepMs);
            continue;
        }

        const bool combatOn = simple_combat::IsEnabled();
        const DWORD combatInterval = static_cast<DWORD>(
            xcat::ClampMobScanIntervalMs(gCombatIntervalMs.load(std::memory_order_acquire)));
        const DWORD interval = combatOn ? combatInterval : kScanIntervalIdleMs;
        if (combatOn != lastCombat || (combatOn && combatInterval != lastCombatInterval)) {
            lastCombat = combatOn;
            lastCombatInterval = combatInterval;
            LogLine("mobscan pace %s interval=%ums", combatOn ? "combat" : "idle",
                    (unsigned)interval);
            lastScan = 0;  // 立刻扫一帧，避免切模式/改速空窗
        }
        if (lastScan && now - lastScan < interval) {
            Sleep(kIdleSleepMs);
            continue;
        }
        lastScan = now;

        // 换图：堆缓存枚举 foothold / 绳子（勿栈 Snapshot）
        {
            ports::foothold::SnapshotMeta fh{};
            if (ports::foothold::CollectToCache(&fh) && fh.mapId > 0 && fh.mapId != lastFhMapId) {
                lastFhMapId = fh.mapId;
                ports::foothold::DumpCachedLog();
                ports::foothold_path::GraphMeta gm{};
                const bool gOk = ports::foothold_path::EnsureGraph() &&
                                 ports::foothold_path::GetGraphMeta(&gm);
                LogLine("foothold map=%d n=%d ladders=%d curFh=%u mismatch=%d "
                        "graph=%d nodes=%d walk=%d climb=%d fall=%d ropeLink=%d (see foothold.log)",
                        fh.mapId, fh.footholdN, fh.ladderN, fh.curFhId, fh.idMismatch,
                        gOk ? 1 : 0, gm.nodes, gm.walkEdges, gm.climbEdges, gm.fallEdges,
                        gm.ropeLinked);
            }
        }

        ports::mob::Snapshot snap{};
        if (!ports::mob::Collect(snap)) {
            if (now - lastMissLog >= kMissLogMs) {
                lastMissLog = now;
                LogLine("mobscan miss: pool/findall empty");
            }
            continue;
        }

        // 日志与扫描解耦：count 变 → 短摘要（仅文件，≥kCountLogMs）；
        // 每 kForceLogMs → 带样例（文件+x.jsonl）。
        const bool countChanged = snap.count != lastCount || snap.spawnSlots != lastSlots;
        const bool force = !lastForceLog || (now - lastForceLog >= kForceLogMs);
        const bool countLog =
            countChanged && (!lastCountLog || now - lastCountLog >= kCountLogMs);
        if (!countLog && !force) {
            if (countChanged) {
                lastCount = snap.count;
                lastSlots = snap.spawnSlots;
            }
            continue;
        }

        lastCount = snap.count;
        lastSlots = snap.spawnSlots;
        const int fk = ports::world::GetMapSceneKey();

        if (force) {
            lastForceLog = now;
            lastCountLog = now;
            char sample[512]{};
            int sn = 0;
            const int show = snap.count < 5 ? snap.count : 5;
            for (int i = 0; i < show; ++i) {
                const auto& m = snap.mobs[i];
                const int left = (int)sizeof(sample) - sn;
                if (left < 48) break;
                sn += snprintf(sample + sn, (size_t)left,
                               " [%d id=%d tpl=%d hp=%d%% (%.0f,%.0f)]", i, m.id, m.templateId,
                               m.hpPct, m.x, m.y);
            }
            LogLine("mobscan n=%d/M=%d map=%d lifeMob=%d lifeAll=%d raw=%d trunc=%d mapKey=%d%s",
                    snap.count, snap.spawnSlots, snap.mapId, snap.lifeMob, snap.lifeAll,
                    snap.rawDict, snap.truncated ? 1 : 0, fk, sample);
        } else {
            lastCountLog = now;
            LogToFile("mobscan n=%d/M=%d map=%d lifeMob=%d lifeAll=%d raw=%d trunc=%d mapKey=%d",
                      snap.count, snap.spawnSlots, snap.mapId, snap.lifeMob, snap.lifeAll,
                      snap.rawDict, snap.truncated ? 1 : 0, fk);
        }
    }

    LogLine("mob_scan worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    LogLine("Init");
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
    // DllMain detach: never join (loader lock). Just signal.
    if (th) CloseHandle(th);
}

void SetCombatIntervalMs(uint32_t ms) {
    const uint32_t v = xcat::ClampMobScanIntervalMs(ms ? ms : xcat::kMobScanIntervalDefaultMs);
    const uint32_t prev = gCombatIntervalMs.exchange(v, std::memory_order_acq_rel);
    if (prev != v) LogLine("SetCombatIntervalMs %u (prev=%u)", (unsigned)v, (unsigned)prev);
}

uint32_t GetCombatIntervalMs() {
    return gCombatIntervalMs.load(std::memory_order_acquire);
}

}  // namespace mob_scan
}  // namespace features
}  // namespace x
