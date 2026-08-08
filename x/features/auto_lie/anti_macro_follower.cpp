#include "anti_macro_follower.h"
#include "anti_macro_port.h"
#include "mouse_region_overlay.h"
#include "mouse_trajectory_sim.h"

#include "../simple_combat/simple_combat.h"
#include "../titlebar/titlebar_win.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace x::features::auto_lie::anti_macro_follower {
namespace {

// Classic NonFinite：准备窗 START_SOLVING_FRAME=150，作答最多 POS_COUNT=330（P0a）。
constexpr int kStartSolvingFrame = 150;
constexpr int kPosCount = 330;
constexpr DWORD kPlanRetryMs = 800;
constexpr DWORD kFrameReassertMs = 30;
constexpr DWORD kProgressLogMs = 1000;
constexpr DWORD kFocusBringEveryMs = 400;
constexpr int kMapFailBudget = 3;
// 闭环节：桌面点位误差（GetCursorPos vs 计划）；对照 Artale local 3px，桌面放宽到 8。
constexpr float kCalibDesktopMaxErrPx = 8.f;

struct PhysicalPlan {
    void* instance = nullptr;
    void* mouseList = nullptr;
    int pointCount = 0;
    POINT screenPoints[kPosCount]{};
    bool havePanelCorners = false;
    POINT panelCorners[4]{};
    // 闭环节用：首/中/末的题面局部（已 Decrypt）
    anti_macro_port::Vec2 verifyLocal[3]{};
    int verifyIndices[3]{};
};

enum class CalibPhase : uint8_t {
    Idle = 0,
    Move0,
    Sample0,
    Move1,
    Sample1,
    Move2,
    Sample2,
    Passed,
    Failed,
};

std::atomic<bool> gEnabled{false};
std::atomic<bool> gFollowing{false};
std::atomic<bool> gUiVisible{false};
std::atomic<bool> gQuizPaused{false};
std::atomic<bool> gFocusLost{false};
std::atomic<bool> gPlayback{false};
std::atomic<bool> gOverlayPref{false};
// 采满 / 原生即将交卷：UI 仍可见也不再硬闸打怪（对照 Artale answer-sent）。
std::atomic<bool> gAnswerDone{false};

PhysicalPlan gPlans[2]{};
std::atomic<unsigned> gPlanSeq{0};
std::atomic<unsigned> gPublishedPlan{0};  // 0 = none；否则 token

std::atomic<int> gPulseLastSample{-1};
std::atomic<DWORD> gPulseLastMoveMs{0};
std::atomic<uint32_t> gPulseMoves{0};
std::atomic<uint32_t> gPulseMissed{0};
std::atomic<uint32_t> gPulseFails{0};
std::atomic<uint32_t> gPulseReasserts{0};

std::mutex gCursorMtx;
bool gClipActive = false;
RECT gSavedClip{};
bool gHaveSavedClip = false;
POINT gSavedCursor{};
DWORD gLastPlanAttempt = 0;
DWORD gLastProgressLog = 0;
DWORD gLastFocusBringMs = 0;
bool gWasNonFiniteOpen = false;
// 求解起点强制终映射一次（准备窗窗口可能移动；短轨也可能刚补满）。
bool gForceRemapOnce = false;
bool gSolvingRemapDone = false;
void* gBuildingInstance = nullptr;

CalibPhase gCalibPhase = CalibPhase::Idle;
unsigned gCalibPlanToken = 0;
float gCalibMaxErr = 0.f;
POINT gCalibRestoreCursor{};
bool gCalibHaveRestore = false;

// 真题路径取证：一读到 rawPosList 就落盘（MapBatch 失败也要留），供事后静态分析 / 换模拟夹具。
void* gPathDumpInst = nullptr;
bool gPathDumpMapped = false;
char gPathDumpId[48]{};

void Log(const char* fmt, ...) {
    char buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    x::runtime::LogI("AutoLieMouse", "%s", buf);
}

void EnsureDirRecursive(const std::string& path) {
    if (path.empty()) return;
    char tmp[MAX_PATH]{};
    snprintf(tmp, sizeof(tmp), "%s", path.c_str());
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            CreateDirectoryA(tmp, nullptr);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, nullptr);
}

bool WriteTextFile(const std::string& path, const std::string& body) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return n == body.size();
}

// phase: have-path（仅 local）| mapped（含 screen，成功）| map-fail（local + 半成品 screen）
// gPathDumpMapped 仅在 phase=="mapped" 时置位；map-fail 不得挡后续成功升级。
void DumpMousePathEvidence(void* inst, const std::vector<anti_macro_port::Vec2>& path,
                           const std::vector<POINT>* screen, const POINT* panelCorners4,
                           bool havePanelCorners, const char* phase, const char* failWhy) {
    if (!inst || path.empty()) return;
    const bool mapped = screen && screen->size() == path.size();
    // 同实例：已有成功映射档 → 跳过；仅有 local 档且本次仍无 screen → 跳过；有 screen → 可升级覆写。
    if (gPathDumpInst == inst && gPathDumpId[0]) {
        if (gPathDumpMapped) return;
        if (!mapped) return;
    }

    const DWORD now = GetTickCount();
    if (gPathDumpInst != inst || !gPathDumpId[0]) {
        snprintf(gPathDumpId, sizeof(gPathDumpId), "%08lX%04X",
                 static_cast<unsigned long>(now),
                 static_cast<unsigned>((reinterpret_cast<uintptr_t>(inst) >> 4) & 0xFFFFu));
        gPathDumpInst = inst;
        gPathDumpMapped = false;
    }

    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) {
        Log("path dump skip: no binDir");
        return;
    }
    const std::string dir =
        std::string(bin) + "state\\lie_events\\mouse_" + gPathDumpId;
    EnsureDirRecursive(dir);

    float minLx = path[0].x, maxLx = path[0].x, minLy = path[0].y, maxLy = path[0].y;
    for (const auto& p : path) {
        minLx = (std::min)(minLx, p.x);
        maxLx = (std::max)(maxLx, p.x);
        minLy = (std::min)(minLy, p.y);
        maxLy = (std::max)(maxLy, p.y);
    }
    const float spanX = (std::max)(maxLx - minLx, 1e-3f);
    const float spanY = (std::max)(maxLy - minLy, 1e-3f);

    int clientW = 0, clientH = 0;
    long originX = 0, originY = 0;
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) hwnd = x::features::titlebar::win::FindGameWindow();
    if (hwnd && IsWindow(hwnd)) {
        RECT client{};
        POINT origin{};
        if (GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin)) {
            clientW = client.right - client.left;
            clientH = client.bottom - client.top;
            originX = origin.x;
            originY = origin.y;
        }
    }

    long panelL = 0, panelT = 0, panelR = 0, panelB = 0;
    bool haveDesktopPanel = false;
    if (havePanelCorners && panelCorners4) {
        panelL = panelR = panelCorners4[0].x;
        panelT = panelB = panelCorners4[0].y;
        for (int i = 1; i < 4; ++i) {
            panelL = (std::min)(panelL, panelCorners4[i].x);
            panelT = (std::min)(panelT, panelCorners4[i].y);
            panelR = (std::max)(panelR, panelCorners4[i].x);
            panelB = (std::max)(panelB, panelCorners4[i].y);
        }
        haveDesktopPanel = (panelR - panelL) >= 8 && (panelB - panelT) >= 8;
    }

    // path_local.jsonl
    {
        std::string body;
        body.reserve(path.size() * 48);
        for (size_t i = 0; i < path.size(); ++i) {
            char line[96]{};
            snprintf(line, sizeof(line), "{\"i\":%zu,\"x\":%.6f,\"y\":%.6f}\n", i, path[i].x,
                     path[i].y);
            body += line;
        }
        (void)WriteTextFile(dir + "\\path_local.jsonl", body);
    }

    // uv.jsonl：相对 local AABB（无桌面映射也能复刻形状到任意面板）
    {
        std::string body;
        body.reserve(path.size() * 48);
        for (size_t i = 0; i < path.size(); ++i) {
            const float u = (path[i].x - minLx) / spanX;
            const float v = (path[i].y - minLy) / spanY;
            char line[96]{};
            snprintf(line, sizeof(line), "{\"i\":%zu,\"u\":%.8f,\"v\":%.8f}\n", i, u, v);
            body += line;
        }
        (void)WriteTextFile(dir + "\\uv.jsonl", body);
    }

    if (mapped) {
        std::string body;
        body.reserve(screen->size() * 40);
        for (size_t i = 0; i < screen->size(); ++i) {
            char line[80]{};
            snprintf(line, sizeof(line), "{\"i\":%zu,\"x\":%ld,\"y\":%ld}\n", i,
                     (*screen)[i].x, (*screen)[i].y);
            body += line;
        }
        (void)WriteTextFile(dir + "\\path_screen.jsonl", body);
    }

    // 可直接替换 mouse_sim_fixture.inc 的 UV 表（相对 local AABB）
    {
        std::string inc;
        inc.reserve(path.size() * 36 + 512);
        char hdr[512]{};
        snprintf(hdr, sizeof(hdr),
                 "// Auto-generated from live NonFinite dump id=%s phase=%s\n"
                 "// Product=经典版 TWMS. Local AABB UV (not Artale desktop UV).\n"
                 "// localAABB=(%.3f,%.3f)-(%.3f,%.3f) pts=%zu mapped=%d\n"
                 "constexpr int kMouseSimPointCount = %zu;\n"
                 "constexpr long kMouseSimPanelL = %ld;\n"
                 "constexpr long kMouseSimPanelT = %ld;\n"
                 "constexpr long kMouseSimPanelR = %ld;\n"
                 "constexpr long kMouseSimPanelB = %ld;\n"
                 "constexpr int kMouseSimDumpClientW = %d;\n"
                 "constexpr int kMouseSimDumpClientH = %d;\n"
                 "constexpr float kMouseSimPanelClientL = %.1ff;\n"
                 "constexpr float kMouseSimPanelClientT = %.1ff;\n"
                 "constexpr float kMouseSimPanelClientR = %.1ff;\n"
                 "constexpr float kMouseSimPanelClientB = %.1ff;\n"
                 "constexpr float kMouseSimUv[kMouseSimPointCount][2] = {\n",
                 gPathDumpId, phase ? phase : "?", minLx, minLy, maxLx, maxLy, path.size(),
                 mapped ? 1 : 0, path.size(),
                 haveDesktopPanel ? panelL : 0L, haveDesktopPanel ? panelT : 0L,
                 haveDesktopPanel ? panelR : 0L, haveDesktopPanel ? panelB : 0L,
                 clientW > 0 ? clientW : 1280, clientH > 0 ? clientH : 720,
                 haveDesktopPanel && clientW > 0
                     ? static_cast<float>(panelL - originX)
                     : 389.0f,
                 haveDesktopPanel && clientH > 0
                     ? static_cast<float>(panelT - originY)
                     : 187.5f,
                 haveDesktopPanel && clientW > 0
                     ? static_cast<float>(panelR - originX)
                     : 889.0f,
                 haveDesktopPanel && clientH > 0
                     ? static_cast<float>(panelB - originY)
                     : 521.5f);
        inc += hdr;
        for (size_t i = 0; i < path.size(); ++i) {
            const float u = (path[i].x - minLx) / spanX;
            const float v = (path[i].y - minLy) / spanY;
            char line[64]{};
            snprintf(line, sizeof(line), "    {%.8ff, %.8ff}%s\n", u, v,
                     i + 1 < path.size() ? "," : "");
            inc += line;
        }
        inc += "};\n";
        (void)WriteTextFile(dir + "\\mouse_sim_fixture.inc", inc);
    }

    {
        char meta[1024]{};
        snprintf(meta, sizeof(meta),
                 "kind=nonfinite-mouse\n"
                 "id=%s\n"
                 "phase=%s\n"
                 "failWhy=%s\n"
                 "tick=%lu\n"
                 "pts=%zu\n"
                 "mapped=%d\n"
                 "havePanelCorners=%d\n"
                 "localAABB=%.6f,%.6f,%.6f,%.6f\n"
                 "client=%dx%d origin=(%ld,%ld)\n"
                 "panelDesktop=LTRB(%ld,%ld,%ld,%ld)\n"
                 "firstLocal=%.6f,%.6f\n"
                 "lastLocal=%.6f,%.6f\n",
                 gPathDumpId, phase ? phase : "?", failWhy ? failWhy : "",
                 static_cast<unsigned long>(now), path.size(), mapped ? 1 : 0,
                 havePanelCorners ? 1 : 0, minLx, minLy, maxLx, maxLy, clientW, clientH,
                 originX, originY, panelL, panelT, panelR, panelB, path.front().x,
                 path.front().y, path.back().x, path.back().y);
        (void)WriteTextFile(dir + "\\meta.txt", meta);
    }

    // 仅成功「mapped」阶段才锁档；valid-low / map-fail 的半成品 screen 不得挡后续升级。
    if (mapped && phase && std::strcmp(phase, "mapped") == 0) gPathDumpMapped = true;
    Log("path dump id=%s phase=%s pts=%zu mapped=%d dir=%s", gPathDumpId,
        phase ? phase : "?", path.size(), mapped ? 1 : 0, dir.c_str());
}

void RefreshAutoLieHardPause() {
    // answer-sent 后 UI 淡出期不再因 gUiVisible 锁打怪（对照 Artale SetEnvironmentPaused(false)）。
    const bool uiHold =
        gUiVisible.load(std::memory_order_acquire) &&
        !gAnswerDone.load(std::memory_order_acquire);
    const bool on =
        gQuizPaused.load(std::memory_order_acquire) ||
        gFollowing.load(std::memory_order_acquire) || uiHold ||
        mouse_trajectory_sim::IsRunning();
    x::features::simple_combat::SetHardPause(x::features::simple_combat::HardPauseHolder::AutoLie,
                                             on);
}

void ReleaseCursorClipOnly() {
    std::lock_guard<std::mutex> lk(gCursorMtx);
    if (!gClipActive) return;
    if (gHaveSavedClip)
        ClipCursor(&gSavedClip);
    else
        ClipCursor(nullptr);
    gClipActive = false;
    gHaveSavedClip = false;
}

void ReleaseCursor(bool restorePos) {
    std::lock_guard<std::mutex> lk(gCursorMtx);
    if (gClipActive) {
        if (gHaveSavedClip)
            ClipCursor(&gSavedClip);
        else
            ClipCursor(nullptr);
        gClipActive = false;
        gHaveSavedClip = false;
    }
    if (restorePos && (gSavedCursor.x || gSavedCursor.y)) {
        SetCursorPos(gSavedCursor.x, gSavedCursor.y);
        gSavedCursor = {};
    }
}

const PhysicalPlan* PublishedPlan(unsigned* outToken = nullptr) {
    const unsigned token = gPublishedPlan.load(std::memory_order_acquire);
    if (outToken) *outToken = token;
    if (!token) return nullptr;
    return &gPlans[(token - 1u) & 1u];
}

void ClearCalibState(bool restoreCursor) {
    if (restoreCursor && gCalibHaveRestore) {
        SetCursorPos(gCalibRestoreCursor.x, gCalibRestoreCursor.y);
    }
    gCalibPhase = CalibPhase::Idle;
    gCalibPlanToken = 0;
    gCalibMaxErr = 0.f;
    gCalibHaveRestore = false;
    gCalibRestoreCursor = {};
}

void ClearPublishedPlan() {
    gPlayback.store(false, std::memory_order_release);
    gPublishedPlan.store(0, std::memory_order_release);
    gPulseLastSample.store(-1, std::memory_order_release);
    gPulseLastMoveMs.store(0, std::memory_order_release);
    gBuildingInstance = nullptr;
    ClearCalibState(false);
}

void PublishOverlayWaiting(const char* label) {
    if (mouse_trajectory_sim::IsRunning()) return;
    if (!mouse_region_overlay::IsEnabled()) return;
    mouse_region_overlay::Snapshot snap{};
    // 无真题计划时也画合成面板青框（与模拟 SYNTH 同几何），避免「只有左上角字、看不见区域」。
    RECT panel{};
    bool live = false;
    if (mouse_trajectory_sim::ResolveTestPanel(panel, &live) &&
        panel.right - panel.left >= 40 && panel.bottom - panel.top >= 40) {
        snap.valid = true;
        snap.corners[0] = {panel.left, panel.bottom};
        snap.corners[1] = {panel.right, panel.bottom};
        snap.corners[2] = {panel.right, panel.top};
        snap.corners[3] = {panel.left, panel.top};
        snap.center.x = (panel.left + panel.right) / 2;
        snap.center.y = (panel.top + panel.bottom) / 2;
        if (label && label[0]) {
            strncpy_s(snap.label, label, _TRUNCATE);
        } else {
            strncpy_s(snap.label, live ? "waiting — LIVE panel" : "waiting — SYNTH panel",
                      _TRUNCATE);
        }
    } else if (label && label[0]) {
        strncpy_s(snap.label, label, _TRUNCATE);
    }
    mouse_region_overlay::SetSnapshot(snap);
}

void PublishOverlayFromPlan(const PhysicalPlan& plan, int liveSamples, const char* label) {
    if (mouse_trajectory_sim::IsRunning()) return;
    if (!mouse_region_overlay::IsEnabled()) return;
    mouse_region_overlay::Snapshot snap{};
    snap.valid = plan.pointCount >= 2;
    if (!snap.valid) {
        PublishOverlayWaiting(label ? label : "plan incomplete");
        return;
    }

    if (plan.havePanelCorners) {
        std::memcpy(snap.corners, plan.panelCorners, sizeof(snap.corners));
    } else {
        long minX = plan.screenPoints[0].x;
        long minY = plan.screenPoints[0].y;
        long maxX = minX;
        long maxY = minY;
        for (int i = 1; i < plan.pointCount; ++i) {
            const POINT& p = plan.screenPoints[i];
            minX = (std::min)(minX, p.x);
            minY = (std::min)(minY, p.y);
            maxX = (std::max)(maxX, p.x);
            maxY = (std::max)(maxY, p.y);
        }
        snap.corners[0] = {minX, maxY};
        snap.corners[1] = {maxX, maxY};
        snap.corners[2] = {maxX, minY};
        snap.corners[3] = {minX, minY};
    }
    snap.center.x = (snap.corners[0].x + snap.corners[1].x + snap.corners[2].x +
                     snap.corners[3].x) /
                    4;
    snap.center.y = (snap.corners[0].y + snap.corners[1].y + snap.corners[2].y +
                     snap.corners[3].y) /
                    4;

    snap.answerCount = (std::min)(plan.pointCount, mouse_region_overlay::kMaxTrailPoints);
    for (int i = 0; i < snap.answerCount; ++i) snap.answerTrail[i] = plan.screenPoints[i];

    const int live =
        (std::max)(0, (std::min)(liveSamples, snap.answerCount));
    snap.liveCount = live;
    for (int i = 0; i < live; ++i) snap.liveTrail[i] = plan.screenPoints[i];

    if (live < snap.answerCount) {
        snap.plannedValid = true;
        snap.planned = plan.screenPoints[live];
    }
    if (label && label[0]) strncpy_s(snap.label, label, _TRUNCATE);
    mouse_region_overlay::SetSnapshot(snap);
}

void Abort(const char* reason) {
    if (gFollowing.load() || gClipActive || gPublishedPlan.load(std::memory_order_acquire))
        Log("abort: %s moves=%u missed=%u fails=%u reassert=%u", reason ? reason : "?",
            gPulseMoves.load(std::memory_order_relaxed),
            gPulseMissed.load(std::memory_order_relaxed),
            gPulseFails.load(std::memory_order_relaxed),
            gPulseReasserts.load(std::memory_order_relaxed));
    gPlayback.store(false, std::memory_order_release);
    ReleaseCursor(true);
    gFollowing.store(false);
    gFocusLost.store(false);
    gAnswerDone.store(false, std::memory_order_release);
    ClearPublishedPlan();
    RefreshAutoLieHardPause();
}

// 采满：停跟+松光标；仅当 path 长度与 plan 一致时才放闸（防短 plan 误 resume）。
void SoftStopFollow(const char* reason, int samples, int planPts) {
    Log("%s samples=%d/%d moves=%u missed=%u — stop follow (hold world until path-stable or UI close)",
        reason ? reason : "samples-full", samples, planPts,
        gPulseMoves.load(std::memory_order_relaxed),
        gPulseMissed.load(std::memory_order_relaxed));
    gPlayback.store(false, std::memory_order_release);
    ReleaseCursor(true);
    gFollowing.store(false, std::memory_order_release);
    gFocusLost.store(false, std::memory_order_release);
    // 保留 plan 供叠层；不 ClearPublishedPlan，便于判定 path 是否仍在涨。
    RefreshAutoLieHardPause();
}

void FinishAnswerSent(const char* reason, int samples, int planPts) {
    Log("%s samples=%d/%d moves=%u missed=%u — resume world (ui may still open)",
        reason ? reason : "answer-sent", samples, planPts,
        gPulseMoves.load(std::memory_order_relaxed),
        gPulseMissed.load(std::memory_order_relaxed));
    gPlayback.store(false, std::memory_order_release);
    ReleaseCursor(true);
    gFollowing.store(false, std::memory_order_release);
    gFocusLost.store(false, std::memory_order_release);
    gAnswerDone.store(true, std::memory_order_release);
    ClearPublishedPlan();
    RefreshAutoLieHardPause();
}

bool MoveToScreenLocked(const POINT& pt) {
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

int ReadListSizeFast(void* list) {
    if (!list) return -1;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(list) +
                                       x::runtime::il2cpp_container::OffListSize());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// SendWill 帧脉冲：禁止 GC / Invoke / 日志；只读已发布计划 + mouseList.Count。
void LieFramePulse(void* /*user*/) {
    // 模拟与真题共用泵线程夹光标（worker 上 ClipCursor 会被系统清掉 / 无效）。
    if (mouse_trajectory_sim::PulseCursorOnPump()) return;
    if (!gEnabled.load(std::memory_order_acquire) || !gFollowing.load(std::memory_order_acquire) ||
        !gPlayback.load(std::memory_order_acquire)) {
        return;
    }
    unsigned token = 0;
    const PhysicalPlan* plan = PublishedPlan(&token);
    if (!plan || !plan->mouseList || plan->pointCount < 2) return;

    const int sampleCount = ReadListSizeFast(plan->mouseList);
    if (sampleCount < 0 || sampleCount > plan->pointCount) return;
    if (sampleCount >= plan->pointCount) return;

    const int previous = gPulseLastSample.load(std::memory_order_acquire);
    const DWORD now = GetTickCount();
    const DWORD lastMove = gPulseLastMoveMs.load(std::memory_order_acquire);
    const bool reassert = previous == sampleCount;
    if (reassert && lastMove && now - lastMove < kFrameReassertMs) return;

    const POINT target = plan->screenPoints[sampleCount];
    bool moved = false;
    {
        std::lock_guard<std::mutex> lk(gCursorMtx);
        if (!gEnabled.load(std::memory_order_acquire) ||
            !gFollowing.load(std::memory_order_acquire) ||
            !gPlayback.load(std::memory_order_acquire) ||
            gPublishedPlan.load(std::memory_order_acquire) != token) {
            return;
        }
        moved = MoveToScreenLocked(target);
    }
    if (!moved) {
        gPulseFails.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    gPulseLastSample.store(sampleCount, std::memory_order_release);
    gPulseLastMoveMs.store(now, std::memory_order_release);
    gPulseMoves.fetch_add(1, std::memory_order_relaxed);
    if (reassert) gPulseReasserts.fetch_add(1, std::memory_order_relaxed);
    if (previous < 0 && sampleCount > 0) {
        gPulseMissed.fetch_add(static_cast<uint32_t>(sampleCount), std::memory_order_relaxed);
    } else if (previous >= 0 && sampleCount > previous + 1) {
        gPulseMissed.fetch_add(static_cast<uint32_t>(sampleCount - previous - 1),
                               std::memory_order_relaxed);
    }
}

bool BuildAndPublishPlan(void* inst, DWORD now) {
    bool forceRefresh = false;
    if (gForceRemapOnce) {
        gForceRemapOnce = false;
        forceRefresh = true;
        Log("plan final-remap (solving start / preempt)");
    }
    // 已发布计划：rawPos 仍在增长则重建；已 follow 且已开始采样则锁死。
    if (!forceRefresh) {
        if (const PhysicalPlan* existing = PublishedPlan();
            gBuildingInstance == inst && existing) {
            std::vector<anti_macro_port::Vec2> peek;
            const bool grew = anti_macro_port::ReadRawPosList(inst, peek) &&
                              peek.size() > static_cast<size_t>(existing->pointCount);
            if (gFollowing.load(std::memory_order_acquire)) {
                const int samples =
                    existing->mouseList ? ReadListSizeFast(existing->mouseList) : -1;
                // 尚未采到点时仍允许换成长轨，避免半截短 plan 把后续 sample 答丢。
                if (samples > 0 || !grew) return true;
                Log("plan refresh(follow,samples=0): path grew %d -> %zu", existing->pointCount,
                    peek.size());
                forceRefresh = true;
            } else if (!grew) {
                return true;
            } else {
                Log("plan refresh: path grew %d -> %zu", existing->pointCount, peek.size());
                forceRefresh = true;
            }
        }
    }
    if (!forceRefresh && gLastPlanAttempt && now - gLastPlanAttempt < kPlanRetryMs) return false;
    gLastPlanAttempt = now;

    std::vector<anti_macro_port::Vec2> path;
    if (!anti_macro_port::ReadRawPosList(inst, path) || path.empty()) {
        Log("plan fail: no-path");
        return false;
    }
    // 关键证优先：一读到答案轨就落盘，不依赖 MapBatch（E175：pts=330 但 ok=0 仍可事后分析）。
    DumpMousePathEvidence(inst, path, nullptr, nullptr, false, "have-path", nullptr);

    if (static_cast<int>(path.size()) > kPosCount) {
        Log("plan fail: path=%zu > %d", path.size(), kPosCount);
        return false;
    }
    void* rect = anti_macro_port::ReadNonFiniteTargetRect(inst);
    if (!rect) {
        Log("plan fail: no-rect");
        return false;
    }
    void* mouseList = anti_macro_port::PeekNonFiniteMouseList(inst);
    if (!mouseList) {
        Log("plan fail: no-mouseList");
        return false;
    }

    std::vector<POINT> screen;
    POINT panelCorners[4]{};
    bool havePanelCorners = false;
    if (!anti_macro_port::MapWinCursorBatch(rect, path, screen, panelCorners, &havePanelCorners) ||
        screen.size() != path.size()) {
        DumpMousePathEvidence(inst, path, screen.empty() ? nullptr : &screen, nullptr, false,
                              "map-fail", "map-batch-or-collapsed");
        Log("plan fail: map-batch/collapsed (refuse follow — E175 kick guard)");
        return false;
    }

    int valid = 0;
    for (const auto& p : screen)
        if (p.x != 0 || p.y != 0) ++valid;
    if (valid < static_cast<int>(screen.size()) / 2) {
        DumpMousePathEvidence(inst, path, &screen, nullptr, false, "map-fail", "valid-low");
        Log("plan fail: valid=%d / %zu", valid, screen.size());
        return false;
    }

    // 双缓冲：写未发布槽，再 release token（对照 Artale PublishPhysicalPlan）。
    gPlayback.store(false, std::memory_order_release);
    unsigned token = gPlanSeq.fetch_add(1, std::memory_order_acq_rel) + 1u;
    if (!token) token = gPlanSeq.fetch_add(1, std::memory_order_acq_rel) + 1u;
    PhysicalPlan& plan = gPlans[(token - 1u) & 1u];
    plan = {};
    plan.instance = inst;
    plan.mouseList = mouseList;
    plan.pointCount = static_cast<int>(screen.size());
    for (int i = 0; i < plan.pointCount; ++i) plan.screenPoints[i] = screen[static_cast<size_t>(i)];

    // 闭环节：首/中/末题面局部（Decrypt 后）
    const bool alreadyPx = anti_macro_port::RawPathLooksLikeCanvasPixels(path);
    auto toCursor = [&](const anti_macro_port::Vec2& raw) {
        return alreadyPx ? raw : anti_macro_port::RawToCursorLocal(raw);
    };
    plan.verifyIndices[0] = 0;
    plan.verifyIndices[1] = plan.pointCount / 2;
    plan.verifyIndices[2] = plan.pointCount - 1;
    for (int i = 0; i < 3; ++i) {
        const int idx = plan.verifyIndices[i];
        plan.verifyLocal[i] = toCursor(path[static_cast<size_t>(idx)]);
    }

    // 题目区域：优先用面板四角仿射几何（MapBatch 主路径）；否则回退路径 AABB。
    if (havePanelCorners) {
        plan.havePanelCorners = true;
        std::memcpy(plan.panelCorners, panelCorners, sizeof(panelCorners));
    } else {
        float minLx = path[0].x;
        float maxLx = path[0].x;
        float minLy = path[0].y;
        float maxLy = path[0].y;
        for (const auto& p : path) {
            minLx = (std::min)(minLx, p.x);
            maxLx = (std::max)(maxLx, p.x);
            minLy = (std::min)(minLy, p.y);
            maxLy = (std::max)(maxLy, p.y);
        }
        auto cornerLocal = [&](float x, float y) {
            return toCursor(anti_macro_port::Vec2{x, y});
        };
        const auto cL0 = cornerLocal(minLx, minLy);
        const auto cL1 = cornerLocal(maxLx, minLy);
        const auto cL2 = cornerLocal(maxLx, maxLy);
        const auto cL3 = cornerLocal(minLx, maxLy);
        POINT corners[4]{};
        const bool c0 = anti_macro_port::TryMapWinCursor(rect, cL0.x, cL0.y, corners[0]);
        const bool c1 = anti_macro_port::TryMapWinCursor(rect, cL1.x, cL1.y, corners[1]);
        const bool c2 = anti_macro_port::TryMapWinCursor(rect, cL2.x, cL2.y, corners[2]);
        const bool c3 = anti_macro_port::TryMapWinCursor(rect, cL3.x, cL3.y, corners[3]);
        if (c0 && c1 && c2 && c3) {
            plan.havePanelCorners = true;
            std::memcpy(plan.panelCorners, corners, sizeof(corners));
        }
    }

    DumpMousePathEvidence(inst, path, &screen, plan.havePanelCorners ? plan.panelCorners : nullptr,
                          plan.havePanelCorners, "mapped", nullptr);

    gPulseLastSample.store(-1, std::memory_order_release);
    gPulseLastMoveMs.store(0, std::memory_order_release);
    gPulseMoves.store(0, std::memory_order_relaxed);
    gPulseMissed.store(0, std::memory_order_relaxed);
    gPulseFails.store(0, std::memory_order_relaxed);
    gPulseReasserts.store(0, std::memory_order_relaxed);
    gPublishedPlan.store(token, std::memory_order_release);
    gBuildingInstance = inst;
    // 准备窗：新计划重跑闭环节；求解期补映射来不及三拍，标 Passed（依赖 MapBatch verify）。
    ClearCalibState(false);
    gCalibPlanToken = token;
    const int frameNow = anti_macro_port::ReadNonFiniteTickFrame(inst);
    if (frameNow >= kStartSolvingFrame) {
        gCalibPhase = CalibPhase::Passed;
        Log("calib skip (solving-frame) token=%u frame=%d", token, frameNow);
    } else {
        gCalibPhase = CalibPhase::Idle;
    }

    const POINT& a = plan.screenPoints[0];
    const POINT& b = plan.screenPoints[plan.pointCount - 1];
    const POINT& mid = plan.screenPoints[plan.pointCount / 2];
    Log("plan ready pts=%d valid=%d first=(%ld,%ld) mid=(%ld,%ld) last=(%ld,%ld) token=%u",
        plan.pointCount, valid, a.x, a.y, mid.x, mid.y, b.x, b.y, token);
    return true;
}

// 准备窗闭环节：挪到首/中/末计划点，核对 GetCursorPos（+可选 TryGet 前向）。
// 返回 true=已通过或仍进行中可继续等；false=失败已清 plan。
bool TickCalibration(void* inst, DWORD now) {
    (void)now;
    unsigned token = 0;
    const PhysicalPlan* plan = PublishedPlan(&token);
    if (!plan || plan->pointCount < 2) {
        ClearCalibState(false);
        return false;
    }
    if (gCalibPhase == CalibPhase::Passed && gCalibPlanToken == token) return true;
    if (gCalibPhase == CalibPhase::Failed) return false;

    if (gCalibPlanToken != token) {
        ClearCalibState(false);
        gCalibPlanToken = token;
        gCalibPhase = CalibPhase::Idle;
    }

    if (!anti_macro_port::IsGameForeground()) {
        // 失焦：松夹、暂停标定推进（对照 Artale pause playback）
        ReleaseCursorClipOnly();
        return true;
    }

    auto fail = [&](const char* why) {
        Log("calib fail: %s maxErr=%.2f token=%u", why ? why : "?", gCalibMaxErr, token);
        ClearCalibState(true);
        ClearPublishedPlan();
        gLastPlanAttempt = 0;  // 允许尽快重建
        return false;
    };

    auto slotOf = [](CalibPhase p) -> int {
        switch (p) {
            case CalibPhase::Move0:
            case CalibPhase::Sample0:
                return 0;
            case CalibPhase::Move1:
            case CalibPhase::Sample1:
                return 1;
            case CalibPhase::Move2:
            case CalibPhase::Sample2:
                return 2;
            default:
                return -1;
        }
    };

    if (gCalibPhase == CalibPhase::Idle) {
        if (!gCalibHaveRestore) {
            gCalibHaveRestore = GetCursorPos(&gCalibRestoreCursor) != FALSE;
        }
        gCalibMaxErr = 0.f;
        gCalibPhase = CalibPhase::Move0;
        Log("calib start token=%u idx=%d,%d,%d", token, plan->verifyIndices[0],
            plan->verifyIndices[1], plan->verifyIndices[2]);
    }

    const int slot = slotOf(gCalibPhase);
    if (slot < 0) return gCalibPhase == CalibPhase::Passed;

    const int idx = plan->verifyIndices[slot];
    const POINT target = plan->screenPoints[idx];
    const bool isMove = gCalibPhase == CalibPhase::Move0 || gCalibPhase == CalibPhase::Move1 ||
                        gCalibPhase == CalibPhase::Move2;

    if (isMove) {
        if (!MoveToScreenLocked(target)) return fail("move");
        // Move → 下一拍 Sample（给系统一帧消化光标）
        gCalibPhase = static_cast<CalibPhase>(static_cast<uint8_t>(gCalibPhase) + 1);
        return true;
    }

    // Sample：只认桌面落点（GetCursorPos vs 计划）。TryGet 与仿射分歧只打日志，不并进门槛。
    POINT actual{};
    if (!GetCursorPos(&actual)) return fail("get-cursor");
    const float dx = static_cast<float>(actual.x - target.x);
    const float dy = static_cast<float>(actual.y - target.y);
    const float err = std::sqrt(dx * dx + dy * dy);

    void* rect = anti_macro_port::ReadNonFiniteTargetRect(inst);
    POINT tryScreen{};
    float tryErr = -1.f;
    if (rect &&
        anti_macro_port::TryMapWinCursor(rect, plan->verifyLocal[slot].x, plan->verifyLocal[slot].y,
                                         tryScreen)) {
        const float tx = static_cast<float>(tryScreen.x - actual.x);
        const float ty = static_cast<float>(tryScreen.y - actual.y);
        tryErr = std::sqrt(tx * tx + ty * ty);
    }

    Log("calib sample slot=%d desktop=(%ld,%ld) target=(%ld,%ld) err=%.2f tryErr=%.2f", slot,
        actual.x, actual.y, target.x, target.y, err, tryErr);
    if (!std::isfinite(err) || err > kCalibDesktopMaxErrPx) return fail("verify-error");
    if (err > gCalibMaxErr) gCalibMaxErr = err;

    if (slot < 2) {
        gCalibPhase = static_cast<CalibPhase>(static_cast<uint8_t>(gCalibPhase) + 1);  // SampleN→MoveN+1
        return true;
    }

    // 通过：还原光标、松夹，等待求解帧
    ReleaseCursorClipOnly();
    if (gCalibHaveRestore) {
        SetCursorPos(gCalibRestoreCursor.x, gCalibRestoreCursor.y);
        gCalibHaveRestore = false;
    }
    gCalibPhase = CalibPhase::Passed;
    Log("calib passed maxErr=%.2f token=%u", gCalibMaxErr, token);
    return true;
}

}  // namespace

void Init() {
    gEnabled.store(false);
    x::runtime::main_thread::SetLieFrameTick(&LieFramePulse, nullptr);
    Abort("init");
}

void SetEnabled(bool enabled) {
    const bool was = gEnabled.exchange(enabled);
    if (!enabled && was) Abort("disabled");
}

void SetRegionOverlayEnabled(bool enabled) {
    gOverlayPref.store(enabled, std::memory_order_release);
    if (!mouse_trajectory_sim::IsRunning()) mouse_region_overlay::SetEnabled(enabled);
    if (!enabled && !mouse_trajectory_sim::IsRunning()) PublishOverlayWaiting(nullptr);
}

bool IsRegionOverlayPref() { return gOverlayPref.load(std::memory_order_acquire); }

bool TryCopyPublishedPanelRect(RECT& out) {
    const PhysicalPlan* plan = PublishedPlan();
    if (!plan || plan->pointCount < 2) return false;
    long minX = 0;
    long minY = 0;
    long maxX = 0;
    long maxY = 0;
    if (plan->havePanelCorners) {
        minX = maxX = plan->panelCorners[0].x;
        minY = maxY = plan->panelCorners[0].y;
        for (int i = 1; i < 4; ++i) {
            minX = (std::min)(minX, plan->panelCorners[i].x);
            minY = (std::min)(minY, plan->panelCorners[i].y);
            maxX = (std::max)(maxX, plan->panelCorners[i].x);
            maxY = (std::max)(maxY, plan->panelCorners[i].y);
        }
    } else {
        minX = maxX = plan->screenPoints[0].x;
        minY = maxY = plan->screenPoints[0].y;
        for (int i = 1; i < plan->pointCount; ++i) {
            minX = (std::min)(minX, plan->screenPoints[i].x);
            minY = (std::min)(minY, plan->screenPoints[i].y);
            maxX = (std::max)(maxX, plan->screenPoints[i].x);
            maxY = (std::max)(maxY, plan->screenPoints[i].y);
        }
    }
    if (maxX - minX < 40 || maxY - minY < 40) return false;
    out.left = minX;
    out.top = minY;
    out.right = maxX;
    out.bottom = maxY;
    return true;
}

void RefreshAutoLieHardPauseFromOutside() { RefreshAutoLieHardPause(); }

void SetQuizWorldPaused(bool paused) {
    gQuizPaused.store(paused, std::memory_order_release);
    RefreshAutoLieHardPause();
}

void Stop() { Abort("stop"); }

void Shutdown() {
    gQuizPaused.store(false, std::memory_order_release);
    gUiVisible.store(false, std::memory_order_release);
    Abort("shutdown");
    x::runtime::main_thread::SetLieFrameTick(nullptr, nullptr);
    mouse_region_overlay::Shutdown();
}

bool IsFollowing() { return gFollowing.load(); }
bool IsUiVisible() { return gUiVisible.load(); }
bool IsRegionOverlayEnabled() { return gOverlayPref.load(std::memory_order_acquire); }

void Tick(DWORD now) {
    mouse_region_overlay::Tick(now);

    // 真题优先：NonFinite 开着时立刻停模拟，让出泵槽给物理跟随。
    if (mouse_trajectory_sim::IsRunning()) {
        const bool wantReal = gEnabled.load(std::memory_order_acquire) &&
                              anti_macro_port::Ensure() && anti_macro_port::IsNonFiniteOpen();
        if (wantReal) {
            mouse_trajectory_sim::RequestStop("preempt-real-NonFinite");
            // 不 return：下面继续建 plan / follow；Pulse 在 stop 后会让出槽。
        } else {
            RefreshAutoLieHardPause();
            return;
        }
    }

    // 区域显示可在自动测谎关闭时仍用（调试 TAB）；无 NonFinite 时画等待 HUD。
    if (!gEnabled.load()) {
        gUiVisible.store(false);
        if (gFollowing.load() || gClipActive || gPublishedPlan.load(std::memory_order_acquire))
            Abort("disabled-tick");
        else
            RefreshAutoLieHardPause();
        if (mouse_region_overlay::IsEnabled()) {
            if (anti_macro_port::Ensure() && anti_macro_port::IsNonFiniteOpen()) {
                void* inst = anti_macro_port::GetNonFinite();
                if (inst && BuildAndPublishPlan(inst, now)) {
                    const PhysicalPlan* plan = PublishedPlan();
                    const int samples = anti_macro_port::ReadMouseSampleCount(inst);
                    if (plan) PublishOverlayFromPlan(*plan, samples, "overlay-only (autoLie off)");
                } else {
                    PublishOverlayWaiting("NonFinite open — building plan");
                }
            } else {
                PublishOverlayWaiting(nullptr);
            }
        }
        return;
    }
    if (!anti_macro_port::Ensure()) {
        RefreshAutoLieHardPause();
        if (mouse_region_overlay::IsEnabled()) PublishOverlayWaiting("port not ready");
        return;
    }

    const bool open = anti_macro_port::IsNonFiniteOpen();
    gUiVisible.store(open);
    if (!open) {
        gWasNonFiniteOpen = false;
        gForceRemapOnce = false;
        gSolvingRemapDone = false;
        gPathDumpInst = nullptr;
        gPathDumpMapped = false;
        gPathDumpId[0] = '\0';
        gAnswerDone.store(false, std::memory_order_release);
        if (gFollowing.load() || gClipActive || gPublishedPlan.load(std::memory_order_acquire))
            Abort("ui-closed");
        else
            RefreshAutoLieHardPause();
        if (mouse_region_overlay::IsEnabled()) PublishOverlayWaiting(nullptr);
        return;
    }

    // 真题刚弹出：立刻强制游戏前台（对照仓 wantEnsure / 无人值守）。
    if (!gWasNonFiniteOpen) {
        gWasNonFiniteOpen = true;
        gSolvingRemapDone = false;
        gForceRemapOnce = false;
        gAnswerDone.store(false, std::memory_order_release);
        Log("NonFinite open — force foreground (Attach-SFW)");
        anti_macro_port::TryBringGameForeground("lie-open", true);
        gLastFocusBringMs = now;
    }

    RefreshAutoLieHardPause();

    void* inst = anti_macro_port::GetNonFinite();
    if (!inst) {
        if (mouse_region_overlay::IsEnabled()) PublishOverlayWaiting("instance null");
        return;
    }

    // 已交卷：淡出期只维持放闸，勿重建 plan / 再跟（对照 Artale answer-sent return）。
    if (gAnswerDone.load(std::memory_order_acquire)) {
        RefreshAutoLieHardPause();
        if (mouse_region_overlay::IsEnabled())
            PublishOverlayWaiting("answer-done — waiting UI close");
        return;
    }

    // 交卷终态优先：_isResultRecv / POS_COUNT（对照 Artale recvResult / sendResult）。
    {
        const int samplesEarly = anti_macro_port::ReadMouseSampleCount(inst);
        if (anti_macro_port::ReadNonFiniteIsResultRecv(inst)) {
            FinishAnswerSent("answer-received", samplesEarly, kPosCount);
            if (mouse_region_overlay::IsEnabled())
                PublishOverlayWaiting("answer-received — waiting UI close");
            return;
        }
        if (samplesEarly >= kPosCount) {
            FinishAnswerSent("answer-sent", samplesEarly, kPosCount);
            if (mouse_region_overlay::IsEnabled())
                PublishOverlayWaiting("answer-sent — waiting UI close");
            return;
        }
    }

    // 失焦：停播 + 松夹（对照 Artale pause frame playback）；仍建 plan / 标定不硬跟。
    const bool fg = anti_macro_port::IsGameForeground();
    if (!fg) {
        if (!gFocusLost.load()) {
            Log("focus not foreground — pause playback + Attach-SFW retry");
            gFocusLost.store(true, std::memory_order_release);
        }
        gPlayback.store(false, std::memory_order_release);
        ReleaseCursorClipOnly();
        if (now - gLastFocusBringMs >= kFocusBringEveryMs) {
            gLastFocusBringMs = now;
            anti_macro_port::TryBringGameForeground("lie-nf", false);
        }
    } else if (gFocusLost.exchange(false)) {
        Log("focus restored");
    }

    const int frame = anti_macro_port::ReadNonFiniteTickFrame(inst);
    if (frame < kStartSolvingFrame) {
        if (frame >= 0) (void)BuildAndPublishPlan(inst, now);
        gPlayback.store(false, std::memory_order_release);
        if (PublishedPlan()) {
            (void)TickCalibration(inst, now);
        }
        if (mouse_region_overlay::IsEnabled()) {
            const PhysicalPlan* plan = PublishedPlan();
            if (plan) {
                char label[96]{};
                snprintf(label, sizeof(label), "prepare frame=%d calib=%d", frame,
                         static_cast<int>(gCalibPhase));
                PublishOverlayFromPlan(*plan, 0, label);
            } else {
                PublishOverlayWaiting("prepare — waiting plan");
            }
        }
        return;
    }

    // 求解起点：强制终映射一次（补满长轨 + 窗口位移后的 screen）。
    if (!gSolvingRemapDone) {
        gForceRemapOnce = true;
        gLastPlanAttempt = 0;
    }

    if (!BuildAndPublishPlan(inst, now)) {
        if (mouse_region_overlay::IsEnabled()) PublishOverlayWaiting("plan build fail");
        return;
    }
    const PhysicalPlan* plan = PublishedPlan();
    if (!plan || plan->pointCount < 2) {
        if (mouse_region_overlay::IsEnabled()) PublishOverlayWaiting("plan empty");
        return;
    }
    gSolvingRemapDone = true;

    // 准备窗未完成闭环节则补完（晚检出 / 失焦打断后）
    if (gCalibPhase != CalibPhase::Passed || gCalibPlanToken != gPublishedPlan.load()) {
        if (!TickCalibration(inst, now)) {
            if (mouse_region_overlay::IsEnabled()) PublishOverlayWaiting("calib fail");
            return;
        }
        if (gCalibPhase != CalibPhase::Passed) {
            gPlayback.store(false, std::memory_order_release);
            if (mouse_region_overlay::IsEnabled()) {
                char label[96]{};
                snprintf(label, sizeof(label), "calib phase=%d", static_cast<int>(gCalibPhase));
                PublishOverlayFromPlan(*plan, 0, label);
            }
            return;
        }
    }

    // 采满：recv/POS_COUNT 已在上面放闸；此处 path 与 plan 对齐则早恢复，否则 SoftStop 保闸。
    auto tryFinishOrSoftStop = [&](const PhysicalPlan* p) -> bool {
        if (!p) return false;
        const int samplesNow = anti_macro_port::ReadMouseSampleCount(inst);
        if (anti_macro_port::ReadNonFiniteIsResultRecv(inst)) {
            FinishAnswerSent("answer-received", samplesNow, p->pointCount);
            if (mouse_region_overlay::IsEnabled())
                PublishOverlayWaiting("answer-received — waiting UI close");
            return true;
        }
        if (samplesNow >= kPosCount) {
            FinishAnswerSent("answer-sent", samplesNow, p->pointCount);
            if (mouse_region_overlay::IsEnabled())
                PublishOverlayWaiting("answer-sent — waiting UI close");
            return true;
        }
        if (samplesNow < p->pointCount) return false;
        std::vector<anti_macro_port::Vec2> pathNow;
        const bool pathOk = anti_macro_port::ReadRawPosList(inst, pathNow);
        if (pathOk && static_cast<int>(pathNow.size()) == p->pointCount) {
            FinishAnswerSent("answer-sent-path-stable", samplesNow, p->pointCount);
            if (mouse_region_overlay::IsEnabled())
                PublishOverlayWaiting("answer-sent — waiting UI close");
            return true;
        }
        if (gFollowing.load(std::memory_order_acquire) || gPlayback.load(std::memory_order_acquire) ||
            gClipActive) {
            SoftStopFollow("samples-full-path-unstable", samplesNow, p->pointCount);
        }
        if (mouse_region_overlay::IsEnabled()) {
            char label[96]{};
            snprintf(label, sizeof(label), "samples-full path=%zu plan=%d (hold)",
                     pathOk ? pathNow.size() : 0u, p->pointCount);
            PublishOverlayFromPlan(*p, samplesNow, label);
        }
        return true;  // 本拍不再跟光标
    };

    if (tryFinishOrSoftStop(plan)) return;

    if (!fg) {
        // 仍跟题意图，但不播光标
        if (mouse_region_overlay::IsEnabled()) {
            const int samples = anti_macro_port::ReadMouseSampleCount(inst);
            char label[96]{};
            snprintf(label, sizeof(label), "focus-pause samples=%d/%d", samples, plan->pointCount);
            PublishOverlayFromPlan(*plan, samples, label);
        }
        return;
    }

    if (!gFollowing.load()) {
        anti_macro_port::TryBringGameForeground("lie-follow-start", true);
        gLastFocusBringMs = now;
        gFollowing.store(true);
        RefreshAutoLieHardPause();
        const int samples = anti_macro_port::ReadMouseSampleCount(inst);
        Log("follow start frame=%d plan=%d samples=%d fg=%d (SendWill pulse)", frame,
            plan->pointCount, samples, 1);
    }
    gPlayback.store(true, std::memory_order_release);

    if (tryFinishOrSoftStop(plan)) return;

    const int samples = anti_macro_port::ReadMouseSampleCount(inst);
    if (mouse_region_overlay::IsEnabled()) {
        char label[96]{};
        snprintf(label, sizeof(label), "follow frame=%d samples=%d/%d", frame, samples,
                 plan->pointCount);
        PublishOverlayFromPlan(*plan, samples, label);
    }

    if (!gLastProgressLog || now - gLastProgressLog >= kProgressLogMs) {
        gLastProgressLog = now;
        Log("follow progress frame=%d samples=%d/%d moves=%u missed=%u fails=%u", frame, samples,
            plan->pointCount, gPulseMoves.load(std::memory_order_relaxed),
            gPulseMissed.load(std::memory_order_relaxed),
            gPulseFails.load(std::memory_order_relaxed));
        if (gPulseFails.load(std::memory_order_relaxed) >=
            static_cast<uint32_t>(kMapFailBudget) * 10u) {
            Abort("pulse-fail-budget");
        }
    }
}

}  // namespace x::features::auto_lie::anti_macro_follower
