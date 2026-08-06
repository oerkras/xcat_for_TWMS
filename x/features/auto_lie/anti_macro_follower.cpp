#include "anti_macro_follower.h"
#include "anti_macro_port.h"

#include "../simple_combat/simple_combat.h"
#include "../../runtime/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace x::features::auto_lie::anti_macro_follower {
namespace {

constexpr int kStartSolvingFrame = 150;
constexpr int kPosCount = 330;
constexpr DWORD kPlanRetryMs = 800;
constexpr int kMapFailBudget = 3;

std::atomic<bool> gEnabled{false};
std::atomic<bool> gFollowing{false};
std::atomic<bool> gUiVisible{false};
std::atomic<bool> gQuizPaused{false};
std::atomic<bool> gFocusLost{false};

bool gClipActive = false;
RECT gSavedClip{};
bool gHaveSavedClip = false;
POINT gSavedCursor{};
int gLastTargetIndex = -1;
int gMapFailStreak = 0;
DWORD gLastPlanAttempt = 0;
void* gPlanInstance = nullptr;
std::vector<POINT> gScreenPlan;

void Log(const char* fmt, ...) {
    char buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    x::runtime::LogI("AutoLieMouse", "%s", buf);
}

// AutoLie 硬闸唯一写口：quiz / following / UI 任一成立即置位，避免 Abort 误清。
void RefreshAutoLieHardPause() {
    const bool on =
        gQuizPaused.load(std::memory_order_acquire) || gFollowing.load(std::memory_order_acquire) ||
        gUiVisible.load(std::memory_order_acquire);
    x::features::simple_combat::SetHardPause(x::features::simple_combat::HardPauseHolder::AutoLie,
                                             on);
}

void ReleaseCursor() {
    if (gClipActive) {
        if (gHaveSavedClip)
            ClipCursor(&gSavedClip);
        else
            ClipCursor(nullptr);
        gClipActive = false;
        gHaveSavedClip = false;
    }
    // 恢复原光标位置（仅当曾保存过）
    if (gSavedCursor.x || gSavedCursor.y) {
        SetCursorPos(gSavedCursor.x, gSavedCursor.y);
        gSavedCursor = {};
    }
}

void ClearPlan() {
    gScreenPlan.clear();
    gPlanInstance = nullptr;
    gLastTargetIndex = -1;
    gMapFailStreak = 0;
}

void Abort(const char* reason) {
    if (gFollowing.load() || gClipActive) Log("abort: %s", reason ? reason : "?");
    ReleaseCursor();
    gFollowing.store(false);
    gFocusLost.store(false);
    ClearPlan();
    RefreshAutoLieHardPause();
}

bool MoveToScreen(const POINT& pt) {
    if (pt.x == 0 && pt.y == 0) return false;
    RECT clip{pt.x, pt.y, pt.x + 1, pt.y + 1};
    if (!gClipActive) {
        gHaveSavedClip = GetClipCursor(&gSavedClip) != FALSE;
        GetCursorPos(&gSavedCursor);
        gClipActive = true;
    }
    if (!ClipCursor(&clip)) return false;
    return SetCursorPos(pt.x, pt.y) != FALSE;
}

bool EnsureScreenPlan(void* inst, DWORD now) {
    if (gPlanInstance == inst && !gScreenPlan.empty()) return true;
    if (gLastPlanAttempt && now - gLastPlanAttempt < kPlanRetryMs) return false;
    gLastPlanAttempt = now;

    std::vector<anti_macro_port::Vec2> path;
    if (!anti_macro_port::ReadRawPosList(inst, path) || path.empty()) {
        Log("plan fail: no-path");
        return false;
    }
    void* rect = anti_macro_port::ReadNonFiniteTargetRect(inst);
    if (!rect) {
        Log("plan fail: no-rect");
        return false;
    }
    std::vector<POINT> screen;
    if (!anti_macro_port::MapWinCursorBatch(rect, path, screen) || screen.size() != path.size()) {
        Log("plan fail: map-batch");
        return false;
    }
    // 有效点过少则拒收
    int valid = 0;
    for (const auto& p : screen)
        if (p.x != 0 || p.y != 0) ++valid;
    if (valid < static_cast<int>(screen.size()) / 2) {
        Log("plan fail: valid=%d / %zu", valid, screen.size());
        return false;
    }
    gScreenPlan = std::move(screen);
    gPlanInstance = inst;
    gLastTargetIndex = -1;
    Log("plan ready pts=%zu valid=%d", gScreenPlan.size(), valid);
    return true;
}

}  // namespace

void Init() {
    gEnabled.store(false);
    Abort("init");
}

void SetEnabled(bool enabled) {
    gEnabled.store(enabled);
    if (!enabled) {
        Abort("disabled");
        // Abort 已 Refresh；quiz 仍持有则硬闸保留
    }
}

void SetQuizWorldPaused(bool paused) {
    gQuizPaused.store(paused, std::memory_order_release);
    RefreshAutoLieHardPause();
}

void Stop() { Abort("stop"); }

void Shutdown() {
    gQuizPaused.store(false, std::memory_order_release);
    gUiVisible.store(false, std::memory_order_release);
    Abort("shutdown");
}

bool IsFollowing() { return gFollowing.load(); }
bool IsUiVisible() { return gUiVisible.load(); }

void Tick(DWORD now) {
    if (!gEnabled.load()) {
        gUiVisible.store(false);
        if (gFollowing.load() || gClipActive) Abort("disabled-tick");
        else RefreshAutoLieHardPause();
        return;
    }
    if (!anti_macro_port::Ensure()) {
        // 解析瞬败：不改 flags，按上次 quiz|following|ui 重钉，防漏闸。
        RefreshAutoLieHardPause();
        return;
    }

    const bool open = anti_macro_port::IsNonFiniteOpen();
    gUiVisible.store(open);
    if (!open) {
        if (gFollowing.load() || gClipActive || gPlanInstance) {
            Abort("ui-closed");
        } else {
            RefreshAutoLieHardPause();
        }
        return;
    }

    // UI 已开：先按 ui 置硬闸，再取实例（inst 空也不漏拍）。
    RefreshAutoLieHardPause();

    void* inst = anti_macro_port::GetNonFinite();
    if (!inst) return;

    if (!anti_macro_port::IsGameForeground()) {
        // 失焦：释放 ClipCursor，保留计划，回前台后继续（勿整段 Abort 丢计划）
        if (!gFocusLost.load()) {
            Log("focus lost — pause cursor");
            gFocusLost.store(true);
        }
        ReleaseCursor();
        gFollowing.store(false);
        RefreshAutoLieHardPause();
        return;
    }
    if (gFocusLost.exchange(false)) Log("focus restored");

    const int frame = anti_macro_port::ReadNonFiniteTickFrame(inst);
    if (frame < kStartSolvingFrame) {
        // 准备窗：提前建计划，求解一开就能跟
        if (frame >= 0) (void)EnsureScreenPlan(inst, now);
        return;
    }

    if (!EnsureScreenPlan(inst, now) || gScreenPlan.empty()) return;

    const int samples = anti_macro_port::ReadMouseSampleCount(inst);
    int idx = samples;
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(gScreenPlan.size()))
        idx = static_cast<int>(gScreenPlan.size()) - 1;
    if (idx >= kPosCount) idx = kPosCount - 1;

    if (!gFollowing.load()) {
        gFollowing.store(true);
        RefreshAutoLieHardPause();
        Log("follow start frame=%d plan=%zu samples=%d", frame, gScreenPlan.size(), samples);
    }

    if (idx == gLastTargetIndex) return;
    gLastTargetIndex = idx;

    const POINT& screen = gScreenPlan[static_cast<size_t>(idx)];
    if (!MoveToScreen(screen)) {
        ++gMapFailStreak;
        if (gMapFailStreak >= kMapFailBudget) {
            Abort("setcursor-fail");
        }
        return;
    }
    gMapFailStreak = 0;
}

}  // namespace x::features::auto_lie::anti_macro_follower
