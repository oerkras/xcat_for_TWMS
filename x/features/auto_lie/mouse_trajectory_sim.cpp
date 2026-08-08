#include "mouse_trajectory_sim.h"

#include "anti_macro_follower.h"
#include "anti_macro_port.h"
#include "mouse_region_overlay.h"
#include "mouse_sim_fixture.inc"

#include "../simple_combat/simple_combat.h"
#include "../titlebar/titlebar_win.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x::features::auto_lie::mouse_trajectory_sim {
namespace {

constexpr DWORD kSampleHz = 33;
constexpr DWORD kFrameMs = (1000u + kSampleHz - 1u) / kSampleHz;
constexpr int kReadyTicks = 165;  // 与 NonFinite START_SOLVING_FRAME 同量级准备窗
constexpr int kReprobeEvery = 33;

struct MappedPoint {
    long x = 0;
    long y = 0;
};

std::atomic<bool> g_running{false};
std::atomic<uint32_t> g_lastSeq{0};
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
SRWLOCK g_threadLock = SRWLOCK_INIT;

// 泵线程夹光标状态（与真题 follower 同线程语义）。
std::atomic<long> g_targetX{0};
std::atomic<long> g_targetY{0};
std::atomic<bool> g_haveTarget{false};
std::atomic<uint32_t> g_pulseOk{0};
std::atomic<uint32_t> g_pulseFail{0};
std::atomic<uint32_t> g_pulseRuns{0};
bool g_pumpClipActive = false;
bool g_pumpHaveSavedClip = false;
RECT g_pumpSavedClip{};
POINT g_pumpSavedCursor{};
bool g_loggedFirstPulse = false;

MappedPoint UvToDesktop(float u, float v, const RECT& panel) {
    const double w = static_cast<double>(panel.right - panel.left);
    const double h = static_cast<double>(panel.bottom - panel.top);
    return MappedPoint{
        static_cast<long>(std::lround(panel.left + u * w)),
        static_cast<long>(std::lround(panel.top + v * h)),
    };
}

void RemapAll(MappedPoint* out, const RECT& panel) {
    for (int i = 0; i < kMouseSimPointCount; ++i) {
        out[i] = UvToDesktop(kMouseSimUv[i][0], kMouseSimUv[i][1], panel);
    }
}

bool ResolveSyntheticPanel(RECT& out) {
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) hwnd = x::features::titlebar::win::FindGameWindow();
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return false;
    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    if (cw < 200 || ch < 200) return false;

    const double sx = static_cast<double>(cw) / static_cast<double>(kMouseSimDumpClientW);
    const double sy = static_cast<double>(ch) / static_cast<double>(kMouseSimDumpClientH);
    out.left = origin.x + static_cast<long>(std::lround(kMouseSimPanelClientL * sx));
    out.top = origin.y + static_cast<long>(std::lround(kMouseSimPanelClientT * sy));
    out.right = origin.x + static_cast<long>(std::lround(kMouseSimPanelClientR * sx));
    out.bottom = origin.y + static_cast<long>(std::lround(kMouseSimPanelClientB * sy));
    // 夹进客户区，避免合成面板甩出窗口外。
    out.left = (std::max)(out.left, origin.x);
    out.top = (std::max)(out.top, origin.y);
    out.right = (std::min)(out.right, origin.x + cw);
    out.bottom = (std::min)(out.bottom, origin.y + ch);
    if (out.right - out.left < 80 || out.bottom - out.top < 80) return false;
    return true;
}

// 最小化/失焦：先还原窗口，再 worker 上 Attach+SFW 抢前台；泵未活则中止。
bool EnsureGameReadyForSim() {
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) hwnd = x::features::titlebar::win::FindGameWindow();
    if (!hwnd || !IsWindow(hwnd)) {
        x::runtime::LogW("AutoLieSim", "abort: no game hwnd");
        return false;
    }

    if (IsIconic(hwnd)) {
        x::runtime::LogW("AutoLieSim", "game minimized — restore + force foreground");
        ShowWindowAsync(hwnd, SW_RESTORE);
    }

    // 模拟线程 = worker：对照仓同款抢前台（绝不进泵）。等待在本线程 Sleep，不堵 Tick。
    anti_macro_port::TryBringGameForeground("sim-start", true);

    for (int i = 0; i < 40; ++i) {  // ≤2s
        if (!IsIconic(hwnd) && x::runtime::main_thread::IsPumpTicking(1500) &&
            anti_macro_port::IsGameForeground()) {
            x::runtime::LogI("AutoLieSim", "ready fg+pump after %dms", i * 50);
            return true;
        }
        if (i == 10 || i == 20 || i == 30) {
            anti_macro_port::TryBringGameForeground("sim-retry", true);
        }
        Sleep(50);
    }

    // 前台弱抢也允许继续，只要已还原且泵在转（光标仍看得见）。
    if (!IsIconic(hwnd) && x::runtime::main_thread::IsPumpTicking(1500)) {
        x::runtime::LogW("AutoLieSim",
                         "continue weak-fg pumpLive=1 iconic=0 (Attach-SFW soft)");
        return true;
    }

    x::runtime::LogW("AutoLieSim",
                     "abort: iconic=%d pumpLive=%d fg=%d after foreground wait",
                     IsIconic(hwnd) ? 1 : 0,
                     x::runtime::main_thread::IsPumpTicking(1500) ? 1 : 0,
                     anti_macro_port::IsGameForeground() ? 1 : 0);
    return false;
}

// 真题同款：必须在泵/前台相关线程上 ClipCursor；worker 调了等于没锁（BIN clip=1 仍可拖）。
void PumpReleaseClip(bool restorePos) {
    if (g_pumpClipActive) {
        if (g_pumpHaveSavedClip)
            ClipCursor(&g_pumpSavedClip);
        else
            ClipCursor(nullptr);
        g_pumpClipActive = false;
    } else {
        ClipCursor(nullptr);
    }
    if (restorePos && (g_pumpSavedCursor.x || g_pumpSavedCursor.y)) {
        SetCursorPos(g_pumpSavedCursor.x, g_pumpSavedCursor.y);
    }
}

bool PumpMoveLocked(long x, long y) {
    if (x == 0 && y == 0) return false;
    if (!g_pumpClipActive) {
        g_pumpHaveSavedClip = GetClipCursor(&g_pumpSavedClip) != FALSE;
        GetCursorPos(&g_pumpSavedCursor);
        g_pumpClipActive = true;
    }
    RECT clip{x, y, x + 1, y + 1};
    if (!ClipCursor(&clip)) return false;
    if (!SetCursorPos(x, y)) return false;
    return true;
}

void PublishOverlayHint(const MappedPoint* mapped, int pointCount, int liveEnd,
                        const RECT& panel, const char* label) {
    mouse_region_overlay::Snapshot snap{};
    snap.valid = true;
    snap.corners[0] = {panel.left, panel.bottom};
    snap.corners[1] = {panel.right, panel.bottom};
    snap.corners[2] = {panel.right, panel.top};
    snap.corners[3] = {panel.left, panel.top};
    snap.center.x = (panel.left + panel.right) / 2;
    snap.center.y = (panel.top + panel.bottom) / 2;

    if (mapped && pointCount > 0) {
        const int answerN = (std::min)(pointCount, mouse_region_overlay::kMaxTrailPoints);
        snap.answerCount = answerN;
        for (int i = 0; i < answerN; ++i) {
            snap.answerTrail[i] = POINT{mapped[i].x, mapped[i].y};
        }
        int liveN = liveEnd;
        if (liveN < 1) liveN = 1;
        if (liveN > answerN) liveN = answerN;
        snap.liveCount = liveN;
        for (int i = 0; i < liveN; ++i) {
            snap.liveTrail[i] = POINT{mapped[i].x, mapped[i].y};
        }
        snap.plannedValid = true;
        snap.planned = POINT{mapped[liveN - 1].x, mapped[liveN - 1].y};
    }
    GetCursorPos(&snap.cursor);
    if (label) strncpy_s(snap.label, label, _TRUNCATE);
    mouse_region_overlay::SetSnapshot(snap);
}

DWORD WINAPI SimThread(LPVOID) {
    g_running.store(true, std::memory_order_release);
    g_haveTarget.store(false, std::memory_order_release);
    g_pulseOk.store(0, std::memory_order_relaxed);
    g_pulseFail.store(0, std::memory_order_relaxed);
    g_pulseRuns.store(0, std::memory_order_relaxed);
    g_loggedFirstPulse = false;
    x::runtime::LogI("AutoLieSim", "thread enter");
    const bool keepOverlay = anti_macro_follower::IsRegionOverlayPref();

    auto finish = [&](const char* why) {
        // 顺序硬约束：先松模拟夹 / 放打怪闸，再做 overlay。
        // BIN 21:56：finish 卡在 overlay soft-off 的跨线程 ShowWindow → 永不 Refresh →
        // SimpleCombat 停在 Idle pause。
        // 注意：须在 g_running=false 之前松夹——否则 Pulse 首行直接 return，走不到 g_stop 释放。
        g_haveTarget.store(false, std::memory_order_release);
        g_stop.store(true, std::memory_order_release);
        const bool following = anti_macro_follower::IsFollowing();
        if (following) {
            // 真题已在同泵夹光标：只清模拟侧标志，禁止 ClipCursor/restore 拆掉 follower。
            x::runtime::main_thread::InvokeAndWait(
                [](void*) {
                    g_pumpClipActive = false;
                    g_pumpHaveSavedClip = false;
                    g_pumpSavedCursor = {};
                },
                nullptr, 800);
        } else {
            x::runtime::main_thread::InvokeAndWait(
                [](void*) { ReleaseCursorOnPump(); }, nullptr, 800);
            ClipCursor(nullptr);
        }

        g_running.store(false, std::memory_order_release);
        anti_macro_follower::RefreshAutoLieHardPauseFromOutside();
        if (why && why[0]) x::runtime::LogI("AutoLieSim", "%s", why);
        g_stop.store(false, std::memory_order_release);

        if (keepOverlay) {
            mouse_region_overlay::Snapshot snap{};
            RECT panel{};
            bool live = false;
            if (ResolveTestPanel(panel, &live) && panel.right - panel.left >= 40 &&
                panel.bottom - panel.top >= 40) {
                snap.valid = true;
                snap.corners[0] = {panel.left, panel.bottom};
                snap.corners[1] = {panel.right, panel.bottom};
                snap.corners[2] = {panel.right, panel.top};
                snap.corners[3] = {panel.left, panel.top};
                snap.center.x = (panel.left + panel.right) / 2;
                snap.center.y = (panel.top + panel.bottom) / 2;
            }
            strncpy_s(snap.label, "sim done — waiting NonFinite", _TRUNCATE);
            mouse_region_overlay::SetSnapshot(snap);
            mouse_region_overlay::SetEnabled(true);
        } else {
            mouse_region_overlay::SetEnabled(false);
        }
    };

    if (!EnsureGameReadyForSim()) {
        finish("abort before lock");
        return 0;
    }

    mouse_region_overlay::SetEnabled(true);
    x::features::simple_combat::SetHardPause(
        x::features::simple_combat::HardPauseHolder::AutoLie, true);

    Sleep(30);

    RECT panel{};
    bool live = false;
    x::runtime::LogI("AutoLieSim", "resolving panel…");
    if (!ResolveTestPanel(panel, &live)) {
        x::runtime::LogW("AutoLieSim",
                         "abort: no game window — cannot synthesize test panel");
        finish("abort: no panel");
        return 0;
    }

    MappedPoint mapped[kMouseSimPointCount]{};
    RemapAll(mapped, panel);
    x::runtime::LogI(
        "AutoLieSim",
        "start uv-map points=%d ready=%d hz=%u live=%d panel=(%ld,%ld)-(%ld,%ld) "
        "start=(%ld,%ld) clip=pump-LieFrameTick",
        kMouseSimPointCount, kReadyTicks, kSampleHz, live ? 1 : 0, panel.left, panel.top,
        panel.right, panel.bottom, mapped[0].x, mapped[0].y);

    char label[96];

    auto publishTarget = [&](long x, long y) {
        g_targetX.store(x, std::memory_order_relaxed);
        g_targetY.store(y, std::memory_order_relaxed);
        g_haveTarget.store(true, std::memory_order_release);
        // 不在 worker 上 ClipCursor；只喂点，等泵 LieFrameTick 实锁。
        if (!anti_macro_port::IsGameForeground()) {
            anti_macro_port::TryBringGameForeground("sim-focus", false);
        }
    };

    for (int tick = 0; tick < kReadyTicks && !g_stop.load(std::memory_order_acquire);
         ++tick) {
        if (tick > 0 && (tick % kReprobeEvery) == 0) {
            RECT fresh{};
            bool freshLive = false;
            if (ResolveTestPanel(fresh, &freshLive)) {
                panel = fresh;
                live = freshLive;
                RemapAll(mapped, panel);
            }
        }
        publishTarget(mapped[0].x, mapped[0].y);
        std::snprintf(label, sizeof(label), "SIM READY %d/%d %s @(%ld,%ld)", tick + 1,
                      kReadyTicks, live ? "LIVE" : "SYNTH", mapped[0].x, mapped[0].y);
        PublishOverlayHint(mapped, kMouseSimPointCount, 1, panel, label);
        Sleep(kFrameMs);
    }

    for (int i = 0; i < kMouseSimPointCount && !g_stop.load(std::memory_order_acquire);
         ++i) {
        if (i > 0 && (i % kReprobeEvery) == 0) {
            RECT fresh{};
            bool freshLive = false;
            if (ResolveTestPanel(fresh, &freshLive)) {
                panel = fresh;
                live = freshLive;
                RemapAll(mapped, panel);
            }
        }
        publishTarget(mapped[i].x, mapped[i].y);
        std::snprintf(label, sizeof(label), "SIM PLAY %d/%d %s @(%ld,%ld)", i + 1,
                      kMouseSimPointCount, live ? "LIVE" : "SYNTH", mapped[i].x,
                      mapped[i].y);
        PublishOverlayHint(mapped, kMouseSimPointCount, i + 1, panel, label);
        Sleep(kFrameMs);
    }

    char done[128]{};
    std::snprintf(done, sizeof(done),
                  "finished stop=%d pulseRuns=%u ok=%u fail=%u",
                  g_stop.load(std::memory_order_acquire) ? 1 : 0,
                  g_pulseRuns.load(std::memory_order_relaxed),
                  g_pulseOk.load(std::memory_order_relaxed),
                  g_pulseFail.load(std::memory_order_relaxed));
    finish(done);
    return 0;
}

void JoinThreadUnlocked() {
    if (!g_thread) return;
    g_stop.store(true, std::memory_order_release);
    WaitForSingleObject(g_thread, 20000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_stop.store(false, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
    g_haveTarget.store(false, std::memory_order_release);
    ClipCursor(nullptr);
    // finish 若未跑完（超时杀线程等）也要放打怪闸。
    anti_macro_follower::RefreshAutoLieHardPauseFromOutside();
}

}  // namespace

bool PulseCursorOnPump() {
    if (!g_running.load(std::memory_order_acquire)) return false;
    // 真题抢占 / 结束中：立刻让出 LieFrame 槽，勿挡 NonFinite 跟随。
    if (g_stop.load(std::memory_order_acquire)) {
        if (g_pumpClipActive) PumpReleaseClip(false);
        return false;
    }
    if (!g_haveTarget.load(std::memory_order_acquire)) return true;  // 占住槽，勿跑真题跟随
    const long x = g_targetX.load(std::memory_order_relaxed);
    const long y = g_targetY.load(std::memory_order_relaxed);
    g_pulseRuns.fetch_add(1, std::memory_order_relaxed);
    const bool ok = PumpMoveLocked(x, y);
    if (ok) {
        g_pulseOk.fetch_add(1, std::memory_order_relaxed);
        if (!g_loggedFirstPulse) {
            g_loggedFirstPulse = true;
            POINT cur{};
            GetCursorPos(&cur);
            x::runtime::LogI("AutoLieSim",
                             "pump clip first ok target=(%ld,%ld) actual=(%ld,%ld) fg=%d", x, y,
                             cur.x, cur.y, anti_macro_port::IsGameForeground() ? 1 : 0);
        }
    } else {
        g_pulseFail.fetch_add(1, std::memory_order_relaxed);
        if (!g_loggedFirstPulse) {
            g_loggedFirstPulse = true;
            x::runtime::LogW("AutoLieSim",
                             "pump clip first FAIL target=(%ld,%ld) err=%lu fg=%d", x, y,
                             GetLastError(), anti_macro_port::IsGameForeground() ? 1 : 0);
        }
    }
    return true;
}

void ReleaseCursorOnPump() { PumpReleaseClip(true); }

bool ResolveTestPanel(RECT& out, bool* live) {
    RECT livePanel{};
    if (anti_macro_follower::TryCopyPublishedPanelRect(livePanel)) {
        out = livePanel;
        if (live) *live = true;
        return true;
    }
    if (!ResolveSyntheticPanel(out)) {
        if (live) *live = false;
        return false;
    }
    if (live) *live = false;
    return true;
}

void Init() {
    g_running.store(false, std::memory_order_release);
    g_lastSeq.store(0, std::memory_order_release);
    g_stop.store(false, std::memory_order_release);
}

void RequestStart(uint32_t seq) {
    if (seq == 0) return;
    AcquireSRWLockExclusive(&g_threadLock);
    if (seq <= g_lastSeq.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&g_threadLock);
        return;
    }
    if (g_running.load(std::memory_order_acquire)) {
        // 不改 g_lastSeq：未真正开跑的 seq 留给空闲后重试（见 Apply 侧不写 stamp）。
        x::runtime::LogW("AutoLieSim", "busy, ignore seq=%u (not consumed)", seq);
        ReleaseSRWLockExclusive(&g_threadLock);
        return;
    }
    g_lastSeq.store(seq, std::memory_order_relaxed);
    JoinThreadUnlocked();
    // Join 后若上一轮残留泵夹，清掉再开（避免第二轮 Clip 状态脏）。
    g_pumpClipActive = false;
    g_pumpHaveSavedClip = false;
    g_loggedFirstPulse = false;
    g_haveTarget.store(false, std::memory_order_release);
    g_stop.store(false, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &SimThread, nullptr, 0, nullptr);
    if (!g_thread) {
        x::runtime::LogW("AutoLieSim", "CreateThread failed err=%lu", GetLastError());
    } else {
        x::runtime::LogI("AutoLieSim", "thread launched seq=%u", seq);
    }
    ReleaseSRWLockExclusive(&g_threadLock);
}

void RequestStop(const char* why) {
    if (!g_running.load(std::memory_order_acquire) && !g_thread) return;
    g_haveTarget.store(false, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    x::runtime::LogW("AutoLieSim", "stop requested (%s)", why ? why : "?");
}

bool IsRunning() { return g_running.load(std::memory_order_acquire); }

void Shutdown() {
    AcquireSRWLockExclusive(&g_threadLock);
    JoinThreadUnlocked();
    ReleaseSRWLockExclusive(&g_threadLock);
}

}  // namespace x::features::auto_lie::mouse_trajectory_sim
