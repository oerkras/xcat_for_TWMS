// TWMS Classic — rest_mp_accel（实验·回蓝累加器加速）
//
// 真源：WorldManager.TryRecovery RVA 0xDF27D0。
// 按间隔写满 WM+0x17C（休息 MP）与 +0x180（椅子 MP）——BIN 已证真蓝会动；
// 过密会踢，间隔由用户滑条自调。禁止 GA .text / E9。
// BIN：bin/XCat_data/logs/rest_mp_accel.log
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "rest_mp_accel.h"

#include "../ports/world_port.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace x::features::rest_mp_accel {
namespace {

namespace il2 = x::runtime::il2cpp;
namespace world = x::features::ports::world;

constexpr size_t kFbRestMpDuration = 0x17C;
constexpr size_t kFbRestMpDurationOnPortableChair = 0x180;
constexpr int32_t kForceAccum = 10000;

constexpr size_t kFbWmMyUser = 0x28;
constexpr size_t kFbPortableChairId = 0x304;

constexpr DWORD kIntervalDefaultMs = 2500;
constexpr DWORD kIntervalMinMs = 50;   // 用户自调；过小会踢（曾 16ms 秒踢）
constexpr DWORD kIntervalMaxMs = 10000;
constexpr DWORD kTickMsOff = 500;

std::atomic<bool> gDesired{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gIntervalMs{kIntervalDefaultMs};
std::atomic<uint32_t> gWriteHits{0};
std::atomic<uint32_t> gSkipHits{0};

DWORD gLastWriteMs = 0;
int gLastMp = -1;

HANDLE gLog = INVALID_HANDLE_VALUE;
std::wstring gLogPath;

DWORD ClampInterval(DWORD ms) {
    if (ms < kIntervalMinMs) return kIntervalMinMs;
    if (ms > kIntervalMaxMs) return kIntervalMaxMs;
    return ms;
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

void OpenBinLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLogPath = dir + L"\\rest_mp_accel.log";
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"rest_mp_accel.log");
}

void BinLog(const char* fmt, ...) {
    OpenBinLog();
    char body[512]{};
    va_list ap;
    va_start(ap, fmt);
    const int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn <= 0) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[640]{};
    const int n =
        snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    if (gLog != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(gLog, line, static_cast<DWORD>(n), &w, nullptr);
    } else if (!gLogPath.empty()) {
        x::runtime::AppendDbgLog(gLogPath, line, static_cast<DWORD>(n));
    }
    x::runtime::LogI("RestMpAccel", "%s", body);
}

bool WriteAccum(void* wm, size_t off, int32_t value) {
    if (!il2::LooksLikeHeapPtr(wm)) return false;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(wm) + off) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int32_t ReadAccum(void* wm, size_t off) {
    if (!il2::LooksLikeHeapPtr(wm)) return -1;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(wm) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ReadPortableChairId(void* wm) {
    void* user = il2::ReadPtr(wm, kFbWmMyUser);
    if (!il2::LooksLikeHeapPtr(user)) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(user) + kFbPortableChairId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void ReadMp(int& mp, int& mmp) {
    mp = -1;
    mmp = -1;
    x::ui::player::Vitals v{};
    if (x::ui::player::Read(v) && v.ok) {
        mp = v.mp;
        mmp = v.mmp;
    }
}

void TickOnce(DWORD now) {
    if (!gDesired.load(std::memory_order_relaxed)) return;
    const DWORD interval = ClampInterval(gIntervalMs.load(std::memory_order_relaxed));
    if (gLastWriteMs && now - gLastWriteMs < interval) return;

    if (!world::IsPlayReady()) {
        gSkipHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    void* wm = world::PeekWorldManager();
    if (!wm) wm = world::GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) {
        gSkipHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 最初有效路径：双累加器写满（休息 +0x17C / 椅子 +0x180），无坐椅门控。
    const bool okMp = WriteAccum(wm, kFbRestMpDuration, kForceAccum);
    const bool okChair = WriteAccum(wm, kFbRestMpDurationOnPortableChair, kForceAccum);
    if (okMp || okChair) {
        gWriteHits.fetch_add(1, std::memory_order_relaxed);
        gLastWriteMs = now;
    } else {
        gSkipHits.fetch_add(1, std::memory_order_relaxed);
    }

    const int chairId = ReadPortableChairId(wm);
    const int32_t a17c = ReadAccum(wm, kFbRestMpDuration);
    const int32_t a180 = ReadAccum(wm, kFbRestMpDurationOnPortableChair);
    int mp = -1, mmp = -1;
    ReadMp(mp, mmp);
    const int dMp = (mp >= 0 && gLastMp >= 0) ? (mp - gLastMp) : 0;
    if (mp >= 0) gLastMp = mp;

    BinLog("PULSE okMp=%d okChair=%d chairId=%d a17c=%d a180=%d mp=%d/%d dMp=%+d "
           "intervalMs=%u hits=%u skip=%u",
           okMp ? 1 : 0, okChair ? 1 : 0, chairId, (int)a17c, (int)a180, mp, mmp, dMp,
           (unsigned)interval, gWriteHits.load(), gSkipHits.load());
}

DWORD WINAPI Worker(LPVOID) {
    OpenBinLog();
    BinLog("WORKER_START force +0x17C/+0x180=%d intervalMs=%u (user-tuned; small=kick) "
           "path=logs/rest_mp_accel.log",
           (int)kForceAccum, (unsigned)gIntervalMs.load());
    for (int i = 0; i < 400 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll");
         ++i)
        Sleep(50);
    while (!gStop.load()) {
        const bool on = gDesired.load(std::memory_order_relaxed);
        if (on) TickOnce(GetTickCount());
        // 轮询粒度：开着时按间隔的一小截，至少 16ms，避免空转过密。
        const DWORD interval = ClampInterval(gIntervalMs.load(std::memory_order_relaxed));
        const DWORD poll = on ? (interval < 50 ? 16u : 50u) : kTickMsOff;
        Sleep(poll);
    }
    BinLog("WORKER_STOP writes=%u skip=%u", gWriteHits.load(), gSkipHits.load());
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(false);
    gIntervalMs.store(kIntervalDefaultMs);
    OpenBinLog();
    BinLog("INIT force +0x17C/+0x180→%d; default interval %ums; default off; "
           "tune interval in UI (too small → kick)",
           (int)kForceAccum, (unsigned)kIntervalDefaultMs);
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gWorker.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) gWorker.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gWorker.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void SetEnabled(bool on) {
    const bool prev = gDesired.exchange(on);
    if (prev == on) return;
    if (on) {
        gLastWriteMs = 0;
        gLastMp = -1;
    }
    BinLog("SetEnabled %d intervalMs=%u", on ? 1 : 0, (unsigned)gIntervalMs.load());
}

bool IsEnabled() { return gDesired.load(); }

void SetIntervalMs(unsigned ms) {
    const DWORD clamped = ClampInterval(static_cast<DWORD>(ms));
    const DWORD prev = gIntervalMs.exchange(clamped);
    if (prev == clamped) return;
    BinLog("SetIntervalMs %u", (unsigned)clamped);
}

unsigned IntervalMs() {
    return static_cast<unsigned>(ClampInterval(gIntervalMs.load()));
}

}  // namespace x::features::rest_mp_accel
