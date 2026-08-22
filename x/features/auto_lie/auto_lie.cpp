#include "auto_lie.h"
#include "anti_macro_follower.h"
#include "anti_macro_port.h"
#include "lie_log.h"
#include "lie_stats.h"
#include "mouse_trajectory_sim.h"

#include "../notify/notify.h"
#include "../simple_combat/simple_combat.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include "xcat_sound.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

namespace x::features::auto_lie {
namespace {

constexpr DWORD kPollMs = 400;
constexpr DWORD kAnsPollMs = 500;
constexpr DWORD kSubmitDelayMinMs = 1500;
constexpr DWORD kSubmitDelayMaxMs = 4000;
constexpr DWORD kLlmWaitMs = 25000;
constexpr DWORD kAlarmSoundMs = 3000;  // 与对照仓一致：进行中/测试报警均约每 3s
constexpr uint32_t kAlarmNotifyTtlMs = 4500;
constexpr DWORD kAlarmNotifyRefreshMs = 2000;
constexpr DWORD kAlarmTestNotifyRefreshMs = 2000;
constexpr DWORD kDumpRetryMs = 1500;
constexpr DWORD kStatusWriteMs = 1000;
constexpr DWORD kInfraRefreshMs = 2000;
constexpr DWORD kAlarmTestDurationMs = 12000;
constexpr DWORD kMouseSmokeDurationMs = 3000;
// lie_events 归档：防测谎截图只增不删把盘/Standby 顶满
constexpr int kLieEventsMaxFiles = 40;
constexpr ULONGLONG kLieEventsMaxBytes = 32ull * 1024ull * 1024ull;

std::atomic<bool> gEnabled{false};
std::atomic<bool> gDryRun{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gBusy{false};
std::atomic<DWORD> gAlarmTestUntil{0};
std::atomic<DWORD> gLastAlarmTestSound{0};
std::atomic<DWORD> gLastAlarmTestNotify{0};
std::atomic<DWORD> gMouseSmokeUntil{0};
// 答题中途被压下的 autoLie=0 的到达时刻。SetEnabled 在 IPC 线程、落实在 worker 的 TickImpl，
// 所以得是原子量。0 = 没有待落实的关闭。
std::atomic<DWORD> gPendingOffMs{0};
// 压得再久也要放手：万一跟随卡住不退出，别让开关永远关不掉。
constexpr DWORD kDeferOffMaxMs = 30000;

DWORD gLastPoll = 0;
DWORD gLastAnsPoll = 0;
DWORD gLastAlarm = 0;
DWORD gLastAlarmNotify = 0;
DWORD gLastDumpAttempt = 0;
DWORD gLastStatusWrite = 0;
DWORD gLastInfraRefresh = 0;

std::string gPendingId;
DWORD gPendingDumpTick = 0;
DWORD gSubmitAt = 0;
bool gHaveAns = false;
std::string gAnsText;
bool gSubmitted = false;
bool gTimeoutLogged = false;
bool gWorldPaused = false;
bool gAlarmActive = false;
bool gAlarmSuppressedUntilUiGone = false;
bool gLieUiWasOpen = false;
std::string gAlarmBody;
std::string gPhase = "idle";  // idle|dump|wait_llm|submit|done|timeout|mouse|alarm_test|mouse_smoke
std::string gLastError;

anti_macro_port::BindReady gBind{};
bool gDirsOk = false;
bool gInfraOk = false;
bool gInfraFull = false;  // infraOk && encodePng
POINT gMouseSmokeOrigin{};
bool gMouseSmokeHaveOrigin = false;

bool IsInfraPhase(const std::string& phase) {
    return phase == "alarm_test" || phase == "mouse_smoke";
}

bool IsProtectedPhase(const std::string& phase) {
    return phase == "mouse" || IsInfraPhase(phase);
}

void Log(const char* fmt, ...) {
    char buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lie_log::Line("AutoLie", buf);
}

void EnsureDir(const std::string& path) {
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

std::string StateRoot() {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return {};
    return std::string(bin) + "state\\";
}

std::string ReqDir() { return StateRoot() + "lie_ai\\req"; }
std::string AnsDir() { return StateRoot() + "lie_ai\\ans"; }
std::string EventsDir() { return StateRoot() + "lie_events"; }
std::string StatusPath() { return StateRoot() + "lie_ai\\status.txt"; }

void PruneLieEvents() {
    const std::string dir = EventsDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    const std::string pattern = dir + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    struct Ent {
        std::string path;
        FILETIME ft{};
        ULONGLONG bytes = 0;
    };
    std::vector<Ent> ents;
    ULONGLONG total = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* name = fd.cFileName;
        if (!name || !name[0] || name[0] == '.') continue;
        const size_t len = strlen(name);
        const bool img = (len >= 4 && (_stricmp(name + len - 4, ".jpg") == 0 ||
                                       _stricmp(name + len - 4, ".png") == 0));
        if (!img) continue;
        Ent e;
        e.path = dir + "\\" + name;
        e.ft = fd.ftLastWriteTime;
        e.bytes = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) |
                  static_cast<ULONGLONG>(fd.nFileSizeLow);
        total += e.bytes;
        ents.push_back(std::move(e));
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (ents.empty()) return;
    if (static_cast<int>(ents.size()) <= kLieEventsMaxFiles && total <= kLieEventsMaxBytes) return;

    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
        return CompareFileTime(&a.ft, &b.ft) < 0;  // oldest first
    });

    size_t keepFrom = 0;
    if (static_cast<int>(ents.size()) > kLieEventsMaxFiles)
        keepFrom = ents.size() - static_cast<size_t>(kLieEventsMaxFiles);

    ULONGLONG keptBytes = 0;
    for (size_t i = keepFrom; i < ents.size(); ++i) keptBytes += ents[i].bytes;
    while (keepFrom < ents.size() && keptBytes > kLieEventsMaxBytes) {
        keptBytes -= ents[keepFrom].bytes;
        ++keepFrom;
    }

    for (size_t i = 0; i < keepFrom; ++i) DeleteFileA(ents[i].path.c_str());
}

void ClearPending() {
    gPendingId.clear();
    gPendingDumpTick = 0;
    gSubmitAt = 0;
    gHaveAns = false;
    gAnsText.clear();
    gSubmitted = false;
    gTimeoutLogged = false;
    // 保留轨迹跟 / 基建烟测 phase，避免被清 pending 冲掉
    if (!IsProtectedPhase(gPhase)) gPhase = "idle";
}

void SetWorldPause(bool on) {
    gWorldPaused = on;
    // 只走 follower 聚合口（quiz|following|ui），勿直接 SetHardPause，避免 Abort 互踩。
    anti_macro_follower::SetQuizWorldPaused(on);
}

void Notify(x::features::notify::NotificationKind kind, const char* key, const char* title,
            const char* body, uint32_t ttlMs = 5200) {
    x::features::notify::PublishNotification(
        x::features::notify::NotificationEvent{kind, key, title, body, ttlMs});
}

void StartAlarm(DWORD now, const char* kindTag) {
    if (gAlarmSuppressedUntilUiGone) return;
    gAlarmActive = true;
    gLastAlarm = now;
    gLastAlarmNotify = now;
    gAlarmBody = kindTag && kindTag[0]
                     ? (std::string("检测到测谎（") + kindTag + "），正在自动测谎中。")
                     : "检测到测谎 UI，正在自动测谎中。";
    Notify(x::features::notify::NotificationKind::Danger, "auto-lie-detected",
           "测谎警报：正在自动测谎中", gAlarmBody.c_str(), kAlarmNotifyTtlMs);
    // 面板不再叠播 Alarm：首响由 payload 立刻给出。
    xcat::sound::PlayAsync(xcat::sound::Id::Alarm);
    Log("alarm started kind=%s", kindTag && kindTag[0] ? kindTag : "unknown");
}

void RefreshAlarmNotification(DWORD now) {
    if (!gAlarmActive || gAlarmSuppressedUntilUiGone) return;
    if (gLastAlarmNotify != 0 && now - gLastAlarmNotify < kAlarmNotifyRefreshMs) return;
    gLastAlarmNotify = now;
    Notify(x::features::notify::NotificationKind::Danger, "auto-lie-detected",
           "测谎警报：正在自动测谎中", gAlarmBody.c_str(), kAlarmNotifyTtlMs);
}

void PulseAlarm(DWORD now) {
    if (gAlarmSuppressedUntilUiGone) return;
    if (!gAlarmActive) StartAlarm(now, nullptr);
    else RefreshAlarmNotification(now);
    if (gLastAlarm && now - gLastAlarm < kAlarmSoundMs) return;
    gLastAlarm = now;
    // 对照仓：程序化 Alarm PCM（高低交替四拍），非系统 Beep。
    xcat::sound::PlayAsync(xcat::sound::Id::Alarm);
}

void StopAlarm(bool suppressUntilUiGone) {
    const bool wasActive = gAlarmActive;
    gAlarmActive = false;
    gLastAlarm = 0;
    gLastAlarmNotify = 0;
    gAlarmBody.clear();
    if (suppressUntilUiGone) gAlarmSuppressedUntilUiGone = true;
    if (wasActive || suppressUntilUiGone) {
        if (wasActive) xcat::sound::CancelPlayback();
        x::features::notify::DismissNotification("auto-lie-detected");
        Log("alarm stopped%s", suppressUntilUiGone ? " (suppressed until UI gone)" : "");
    }
}

bool WriteText(const std::string& path, const std::string& text);

void RefreshInfra(DWORD now, bool force = false) {
    if (!force && gLastInfraRefresh && now - gLastInfraRefresh < kInfraRefreshMs) return;
    gLastInfraRefresh = now;
    EnsureDir(ReqDir());
    EnsureDir(AnsDir());
    EnsureDir(EventsDir());
    EnsureDir(ReqDir() + "\\done");
    EnsureDir(ReqDir() + "\\failed");
    const std::string probe = StatusPath() + ".write_probe";
    gDirsOk = WriteText(probe, "ok\n");
    if (gDirsOk) DeleteFileA(probe.c_str());
    gBind = anti_macro_port::ProbeBindReady();
    gInfraOk = gDirsOk && gBind.ok;
    gInfraFull = gInfraOk && gBind.encodePng;
}

void TickAlarmTest(DWORD now) {
    const DWORD until = gAlarmTestUntil.load();
    if (!until) return;
    if (static_cast<LONG>(now - until) >= 0) {
        gAlarmTestUntil.store(0);
        gLastAlarmTestSound.store(0);
        gLastAlarmTestNotify.store(0);
        xcat::sound::CancelPlayback();
        x::features::notify::DismissNotification("auto-lie-alarm-test");
        Log("alarm test finished");
        if (gPhase == "alarm_test") gPhase = "idle";
        return;
    }
    gPhase = "alarm_test";
    const DWORD last = gLastAlarmTestSound.load();
    if (now - last >= kAlarmSoundMs) {
        gLastAlarmTestSound.store(now);
        xcat::sound::PlayAsync(xcat::sound::Id::Alarm);
    }
    const DWORD lastNotify = gLastAlarmTestNotify.load();
    if (now - lastNotify >= kAlarmTestNotifyRefreshMs) {
        gLastAlarmTestNotify.store(now);
        Notify(x::features::notify::NotificationKind::Danger, "auto-lie-alarm-test",
               "测试测谎警报：正在自动测谎中", "这是 12 秒报警测试，不会答题或提交答案。",
               kAlarmNotifyTtlMs);
    }
}

void TickMouseSmoke(DWORD now) {
    const DWORD until = gMouseSmokeUntil.load();
    if (!until) {
        if (gMouseSmokeHaveOrigin) {
            // 遗留路径：旧版曾 ClipCursor；确保释放。
            ClipCursor(nullptr);
            gMouseSmokeHaveOrigin = false;
            anti_macro_follower::RefreshAutoLieHardPauseFromOutside();
        }
        return;
    }
    if (static_cast<LONG>(now - until) >= 0) {
        ClipCursor(nullptr);
        gMouseSmokeUntil.store(0);
        gMouseSmokeHaveOrigin = false;
        // 结束硬闸，交回 follower 聚合口（quiz|following|ui|sim）。
        anti_macro_follower::RefreshAutoLieHardPauseFromOutside();
        Log("mouse smoke finished");
        if (gPhase == "mouse_smoke") gPhase = "idle";
        return;
    }
    gPhase = "mouse_smoke";
    // 烟测期间硬闸战斗：避免 Clip/光标抖动与 OnFuncKey/face 抢主线程泵 → 旋翼卡死。
    x::features::simple_combat::SetHardPause(
        x::features::simple_combat::HardPauseHolder::AutoLie, true);

    // 仅游戏前台时挪光标；失焦跳过（不锁桌面）。
    // 故意不用 ClipCursor：实机已证锁光标 + 战斗仍开火 → face/OnFuncKey pump timeout、heli stale。
    if (!anti_macro_port::IsGameForeground()) {
        return;
    }
    if (!gMouseSmokeHaveOrigin) {
        GetCursorPos(&gMouseSmokeOrigin);
        gMouseSmokeHaveOrigin = true;
        Log("mouse smoke cursor origin=(%ld,%ld) (SetCursorPos only, no ClipCursor)",
            gMouseSmokeOrigin.x, gMouseSmokeOrigin.y);
    }
    const int step = static_cast<int>((now / 180) % 4);
    const int dx[] = {-36, 36, 36, -36};
    const int dy[] = {-36, -36, 36, 36};
    SetCursorPos(gMouseSmokeOrigin.x + dx[step], gMouseSmokeOrigin.y + dy[step]);
}

bool WriteBytes(const std::string& path, const std::vector<uint8_t>& data) {
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) EnsureDir(path.substr(0, slash));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    return f.good();
}

bool WriteText(const std::string& path, const std::string& text) {
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) EnsureDir(path.substr(0, slash));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

bool ReadText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void WriteStatus(DWORD now, bool force = false) {
    if (!force && gLastStatusWrite && now - gLastStatusWrite < kStatusWriteMs) return;
    gLastStatusWrite = now;
    const DWORD alarmUntil = gAlarmTestUntil.load();
    const DWORD smokeUntil = gMouseSmokeUntil.load();
    char body[1024]{};
    snprintf(body, sizeof(body),
             "enabled=%d\n"
             "dryRun=%d\n"
             "busy=%d\n"
             "phase=%s\n"
             "pendingId=%s\n"
             "mouseUi=%d\n"
             "mouseFollow=%d\n"
             "infraOk=%d\n"
             "infraFull=%d\n"
             "dirsOk=%d\n"
             "il2cpp=%d\n"
             "gaBase=%d\n"
             "klassUtil=%d\n"
             "klassText=%d\n"
             "klassNonFinite=%d\n"
             "inputSetText=%d\n"
             "getTransform=%d\n"
             "encodePng=%d\n"
             "quizOk=%d\n"
             "mouseOk=%d\n"
             "bindOk=%d\n"
             "alarmTest=%d\n"
             "mouseSmoke=%d\n"
             "lastError=%s\n"
             "writeTickMs=%llu\n",
             gEnabled.load() ? 1 : 0, gDryRun.load() ? 1 : 0, gBusy.load() ? 1 : 0, gPhase.c_str(),
             gPendingId.c_str(), anti_macro_follower::IsUiVisible() ? 1 : 0,
             anti_macro_follower::IsFollowing() ? 1 : 0, gInfraOk ? 1 : 0, gInfraFull ? 1 : 0,
             gDirsOk ? 1 : 0, gBind.il2cpp ? 1 : 0, gBind.gaBase ? 1 : 0, gBind.klassUtil ? 1 : 0,
             gBind.klassText ? 1 : 0, gBind.klassNonFinite ? 1 : 0, gBind.inputSetText ? 1 : 0,
             gBind.getTransform ? 1 : 0, gBind.encodePng ? 1 : 0, gBind.quizOk ? 1 : 0,
             gBind.mouseOk ? 1 : 0, gBind.ok ? 1 : 0, alarmUntil ? 1 : 0, smokeUntil ? 1 : 0,
             gLastError.c_str(), static_cast<unsigned long long>(GetTickCount64()));
    (void)WriteText(StatusPath(), body);
}

std::string MakeId(DWORD now) {
    char buf[64]{};
    snprintf(buf, sizeof(buf), "lie_%08lx_%lu", static_cast<unsigned long>(now),
             static_cast<unsigned long>(GetCurrentProcessId()));
    return buf;
}

DWORD RandSubmitDelay() {
    const DWORD span = kSubmitDelayMaxMs - kSubmitDelayMinMs;
    return kSubmitDelayMinMs + (span ? (static_cast<DWORD>(rand()) % (span + 1)) : 0);
}

bool ParseAnsFile(const std::string& body, std::string& answer, bool& okFlag) {
    answer.clear();
    okFlag = false;
    std::string line;
    for (size_t i = 0; i <= body.size(); ++i) {
        if (i < body.size() && body[i] != '\n') {
            line.push_back(body[i]);
            continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ok=", 0) == 0) okFlag = (line.substr(3) == "1");
        if (line.rfind("answer=", 0) == 0) {
            answer = line.substr(7);
            while (!answer.empty() && (answer.back() == ' ' || answer.back() == '\t'))
                answer.pop_back();
        }
        line.clear();
    }
    return okFlag && !answer.empty();
}

bool DumpCaptchaReq(DWORD now, std::string& outId) {
    std::vector<uint8_t> image;
    anti_macro_port::CaptchaImageKind kind = anti_macro_port::CaptchaImageKind::Unknown;
    if (!anti_macro_port::DumpTextCaptchaImage(image, kind) || image.empty()) {
        gLastError = "dump image failed";
        Log("%s", gLastError.c_str());
        return false;
    }
    const char* ext = (kind == anti_macro_port::CaptchaImageKind::Jpeg) ? ".jpg" : ".png";
    outId = MakeId(now);
    EnsureDir(ReqDir());
    EnsureDir(AnsDir());
    EnsureDir(EventsDir());
    const std::string imgPath = ReqDir() + "\\" + outId + ext;
    const std::string txt = ReqDir() + "\\" + outId + ".txt";
    const std::string arch = EventsDir() + "\\" + outId + ext;
    if (!WriteBytes(imgPath, image)) {
        gLastError = "write image failed";
        return false;
    }
    (void)WriteBytes(arch, image);
    PruneLieEvents();
    const std::string probe =
        "kind=textcaptcha\n"
        "q=识别图中扭曲验证码文字，只输出答案本身\n"
        "o1=\n"
        "o2=\n"
        "o3=\n"
        "o4=\n";
    if (!WriteText(txt, probe)) {
        gLastError = "write probe txt failed";
        return false;
    }
    gLastError.clear();
    Log("dumped %s %s size=%zu", outId.c_str(), ext, image.size());
    return true;
}

void TickQuiz(DWORD now) {
    const bool open = anti_macro_port::IsTextCaptchaOpen();
    if (!open) {
        if (!gPendingId.empty()) {
            Log("textcaptcha closed, clear pending id=%s", gPendingId.c_str());
            ClearPending();
        }
        return;
    }

    SetWorldPause(true);
    if (!gAlarmActive) StartAlarm(now, "text");
    else PulseAlarm(now);
    gBusy.store(true);

    if (gPendingId.empty()) {
        if (gLastDumpAttempt && now - gLastDumpAttempt < kDumpRetryMs) return;
        gLastDumpAttempt = now;
        gPhase = "dump";
        std::string id;
        if (!DumpCaptchaReq(now, id)) {
            WriteStatus(now, true);
            return;
        }
        gPendingId = id;
        gPendingDumpTick = now;
        gSubmitAt = 0;
        gHaveAns = false;
        gSubmitted = false;
        gTimeoutLogged = false;
        gPhase = "wait_llm";
        WriteStatus(now, true);
        return;
    }

    if (gSubmitted) {
        gPhase = "done";
        return;
    }

    if (!gHaveAns) {
        if (now - gPendingDumpTick > kLlmWaitMs) {
            gPhase = "timeout";
            if (!gTimeoutLogged) {
                gTimeoutLogged = true;
                gLastError = "LLM timeout " + gPendingId;
                Log("%s (keep alarm until UI closes)", gLastError.c_str());
                // 对照仓：超时用 AlarmTimeout，与进行中 Alarm 一听可辨。
                xcat::sound::PlayAsync(xcat::sound::Id::AlarmTimeout);
                lie_stats::RecordOutcome(lie_stats::Kind::Text, lie_stats::Outcome::Timeout);
                Notify(x::features::notify::NotificationKind::Warning, "auto-lie-solver-timeout",
                       "测谎识别超时",
                       "AI 未在限时内返回答案，请检查启动器、网络或 LLM 服务。", 5200);
                WriteStatus(now, true);
            }
            return;
        }
        if (gLastAnsPoll && now - gLastAnsPoll < kAnsPollMs) return;
        gLastAnsPoll = now;
        const std::string ansPath = AnsDir() + "\\" + gPendingId + ".ans";
        std::string body;
        if (!ReadText(ansPath, body)) return;
        std::string answer;
        bool ok = false;
        if (!ParseAnsFile(body, answer, ok)) {
            gLastError = "bad ans file " + gPendingId;
            Log("%s", gLastError.c_str());
            // 坏 ans 不当作成功；等 UI 关或人工
            return;
        }
        gAnsText = answer;
        gHaveAns = true;
        gSubmitAt = now + RandSubmitDelay();
        gPhase = "submit";
        gLastError.clear();
        Log("got ans=%s submitIn=%lu", gAnsText.c_str(),
            static_cast<unsigned long>(gSubmitAt - now));
        WriteStatus(now, true);
        return;
    }

    if (now < gSubmitAt) return;
    if (gDryRun.load()) {
        Log("dryRun skip OnOk ans=%s", gAnsText.c_str());
        gSubmitted = true;
        gPhase = "done";
        gLastError.clear();
        // 干跑也播通过音，便于离线对听；并抑制后续 Alarm 直至窗关。
        StopAlarm(/*suppressUntilUiGone=*/true);
        xcat::sound::PlayInterrupt(xcat::sound::Id::LiePass);
        Notify(x::features::notify::NotificationKind::Success, "auto-lie-result", "测谎干跑完成",
               "已拿到答案但未 OnOk（干跑）。", 4500);
        WriteStatus(now, true);
        return;
    }
    if (anti_macro_port::SubmitTextCaptchaAnswer(gAnsText)) {
        Log("submitted OnOk ans=%s", gAnsText.c_str());
        gSubmitted = true;
        gPhase = "done";
        gLastError.clear();
        // 对照仓：掐断报警队列后插播 LiePass；窗未关前不再 PulseAlarm。
        StopAlarm(/*suppressUntilUiGone=*/true);
        xcat::sound::PlayInterrupt(xcat::sound::Id::LiePass);
        lie_stats::RecordOutcome(lie_stats::Kind::Text, lie_stats::Outcome::Answered);
        Notify(x::features::notify::NotificationKind::Success, "auto-lie-result", "测谎已提交",
               gAnsText.c_str(), 4500);
        WriteStatus(now, true);
    } else {
        gLastError = "submit OnOk failed";
        Log("%s", gLastError.c_str());
        gSubmitAt = now + 800;
    }
}

void TickImpl(DWORD now) {
    // 模拟线程结束后清掉误用的 mouse_smoke phase
    static bool s_simWas = false;
    const bool simNow = mouse_trajectory_sim::IsRunning();
    if (s_simWas && !simNow && gPhase == "mouse_smoke") {
        gPhase = "idle";
        WriteStatus(now, true);
    }
    s_simWas = simNow;

    TickAlarmTest(now);
    TickMouseSmoke(now);
    RefreshInfra(now);

    // 之前压下的 autoLie=0：答题收了尾就落实（等太久也放手，别让卡住的跟随把开关锁死）。
    const DWORD pendingOff = gPendingOffMs.load();
    if (pendingOff &&
        (!anti_macro_follower::IsFollowing() || now - pendingOff >= kDeferOffMaxMs)) {
        const DWORD held = now - pendingOff;
        gPendingOffMs.store(0);
        Log("deferred autoLie=0 applied after %lums", static_cast<unsigned long>(held));
        SetEnabled(false);
        return;  // 本拍收手，下一拍走关闭路径
    }

    if (!gEnabled.load()) {
        if (gWorldPaused) SetWorldPause(false);
        // 关自动测谎：立刻停报警音/撤气泡（测试报警基建仍可独立跑）。
        if (gAlarmActive || gAlarmSuppressedUntilUiGone) {
            StopAlarm(/*suppressUntilUiGone=*/false);
            gAlarmSuppressedUntilUiGone = false;
        }
        ClearPending();
        anti_macro_follower::SetEnabled(false);
        anti_macro_follower::Tick(now);  // 题目区域显示可在 autoLie 关时独立刷新
        // 关自动测谎也要让路：轨迹题/知识题面板开着时加点、加技能点不许抢独占包。
        bool quizOpen = false;
        if (anti_macro_port::Ensure()) {
            quizOpen = anti_macro_port::IsOpenAntiMacro() || anti_macro_port::IsTextCaptchaOpen() ||
                       anti_macro_port::IsNonFiniteOpen();
        }
        gBusy.store(quizOpen);
        if (!gBusy.load() && !IsInfraPhase(gPhase)) gPhase = "idle";
        WriteStatus(now);
        return;
    }

    if (!anti_macro_port::Ensure()) {
        gLastError = "il2cpp/classes not ready";
        WriteStatus(now);
        return;
    }

    anti_macro_follower::SetEnabled(true);
    anti_macro_follower::Tick(now);

    const bool textOpen = anti_macro_port::IsTextCaptchaOpen();
    // 轨迹题以 follower 为准：IsInstantiated 但还没轨迹的空壳不算开题（BIN 7bb1b7）。
    const bool mouseOpen =
        anti_macro_follower::IsUiVisible() || anti_macro_follower::IsFollowing();
    const bool anyOpen = textOpen || mouseOpen;

    // 知识题记账（轨迹题在 follower 里埋）；关窗时若没提交过答案，闩会补记 missed。
    static bool s_textWasOpen = false;
    if (textOpen != s_textWasOpen) {
        s_textWasOpen = textOpen;
        if (textOpen)
            lie_stats::RecordSeen(lie_stats::Kind::Text,
                                  reinterpret_cast<uint64_t>(anti_macro_port::GetTextCaptcha()));
        else
            lie_stats::NotifyClosed(lie_stats::Kind::Text);
    }

    // 实机测谎 UI 优先；基建 phase 仅在无 UI 时保留
    if (mouseOpen && !IsInfraPhase(gPhase)) gPhase = "mouse";

    if (!anyOpen) {
        if (gLieUiWasOpen) {
            // 未走 LiePass 抑制路径时掐断残余 Alarm；已插播胜利音则勿再 Cancel。
            if (!gAlarmSuppressedUntilUiGone) StopAlarm(/*suppressUntilUiGone=*/false);
            else {
                gAlarmSuppressedUntilUiGone = false;
                gAlarmActive = false;
                gLastAlarm = 0;
                gLastAlarmNotify = 0;
                gAlarmBody.clear();
            }
            gLieUiWasOpen = false;
        }
        ClearPending();
        if (!anti_macro_follower::IsFollowing()) SetWorldPause(false);
        gBusy.store(anti_macro_follower::IsFollowing() || anti_macro_follower::IsUiVisible());
        if (!gBusy.load() && !IsInfraPhase(gPhase)) gPhase = "idle";
        WriteStatus(now);
        return;
    }

    gLieUiWasOpen = true;
    gBusy.store(true);

    // 鼠标轨迹题：TickQuiz 不跑，这里维持报警气泡 + 音效。
    if (mouseOpen && !textOpen) {
        if (!gAlarmActive) StartAlarm(now, "mouse");
        else PulseAlarm(now);
    }

    // 轨迹跟随已高频 Tick；知识题节流
    if (gLastPoll && now - gLastPoll < kPollMs) {
        if (textOpen) TickQuiz(now);
        WriteStatus(now);
        return;
    }
    gLastPoll = now;

    if (textOpen) {
        TickQuiz(now);
    } else if (!gPendingId.empty()) {
        ClearPending();
    }

    WriteStatus(now);
}

DWORD WINAPI Worker(LPVOID) {
    srand(static_cast<unsigned>(GetTickCount() ^ GetCurrentProcessId()));
    Log("worker start");
    EnsureDir(ReqDir());
    EnsureDir(AnsDir());
    while (!gWorkerStop.load()) {
        x::ipc::PayloadControl_Poll();
        TickImpl(GetTickCount());
        Sleep(30);
    }
    anti_macro_follower::Stop();
    SetWorldPause(false);
    if (gAlarmActive || gAlarmSuppressedUntilUiGone) {
        StopAlarm(/*suppressUntilUiGone=*/false);
        gAlarmSuppressedUntilUiGone = false;
    }
    xcat::sound::CancelPlayback();
    gPhase = "idle";
    WriteStatus(GetTickCount(), true);
    Log("worker stop");
    return 0;
}

// missed 取证（知识题）。只会在 worker 线程被回调——Text 的 seen / outcome / closed 全部从
// TickImpl 触发，与下面这些状态的写方同线程，所以读 gPhase / gLastError 是安全的。
// 轨迹题那份在 anti_macro_follower.cpp 里，两者互不相干。
void FillMissedSnapshot(char* out, int outSz) {
    if (!out || outSz <= 0) return;
    snprintf(out, static_cast<size_t>(outSz),
             "auto_lie phase=%.24s busy=%d dryRun=%d worldPaused=%d\r\n"
             "text pendingId=%.32s haveAns=%d ansLen=%d submitted=%d timeoutLogged=%d\r\n"
             "infra dirsOk=%d infraOk=%d infraFull=%d\r\n"
             "lastError=%.160s",
             gPhase.c_str(), gBusy.load() ? 1 : 0, gDryRun.load() ? 1 : 0, gWorldPaused ? 1 : 0,
             gPendingId.c_str(), gHaveAns ? 1 : 0, static_cast<int>(gAnsText.size()),
             gSubmitted ? 1 : 0, gTimeoutLogged ? 1 : 0, gDirsOk ? 1 : 0, gInfraOk ? 1 : 0,
             gInfraFull ? 1 : 0, gLastError.empty() ? "(none)" : gLastError.c_str());
}

}  // namespace

void Init() {
    anti_macro_follower::Init();
    mouse_trajectory_sim::Init();
    lie_stats::Init();
    lie_stats::SetSnapshotProvider(lie_stats::Kind::Text, &FillMissedSnapshot);
    ClearPending();
    gEnabled.store(false);
    gDryRun.store(false);
    gBusy.store(false);
    gPhase = "idle";
    RefreshInfra(GetTickCount(), true);
    WriteStatus(GetTickCount(), true);
}

void Shutdown() {
    StopWorker();
    mouse_trajectory_sim::Shutdown();
    anti_macro_follower::Shutdown();
    SetWorldPause(false);
    ClipCursor(nullptr);
    gAlarmTestUntil.store(0);
    gMouseSmokeUntil.store(0);
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!th) {
        Log("CreateThread failed");
        return;
    }
    gWorkerThread.store(th);
}

void StopWorker() {
    gWorkerStop.store(true);
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) {
        const DWORD wait = WaitForSingleObject(th, 5000);
        if (wait == WAIT_TIMEOUT) {
            Log("StopWorker wait timeout; thread may still be exiting");
        }
        CloseHandle(th);
    }
    // 本 worker 是谓词的唯一提问方，停完它再停刷新线程，顺序不能反。
    // 挂在这里而不是 Shutdown()：卸载路径走的是 StopAllFeatureWorkers → StopWorker，
    // auto_lie::Shutdown() 全仓没有调用方。
    anti_macro_port::StopRefresher();
}

void SetEnabled(bool on) {
    // 答题进行中收到 autoLie=0 先别照办。IPC 每拍下发全量配置，实机见过面板侧这一位短暂
    // 掉 0 又弹回来（BIN aa29bc 08-11 05:24 / 05:26 / 05:35，各 117 / 641 / 182 ms）。
    // 那三次都落在空闲期所以无害，可要是落进答题窗口，follower 的 Abort 会把光标弹回答题前
    // 的位置、游戏那几帧照采，轨迹里就多一段人为瞬移 —— 与谓词 stale 误判是同一种伤。
    // 真想关的话不会漏：答题收尾（或 kDeferOffMaxMs 到点）后由 TickImpl 落实。
    if (!on && gEnabled.load() && anti_macro_follower::IsFollowing()) {
        DWORD expected = 0;
        DWORD mark = GetTickCount();
        if (!mark) mark = 1;
        if (gPendingOffMs.compare_exchange_strong(expected, mark))
            Log("autoLie=0 arrived mid-follow — defer off until quiz ends");
        return;
    }
    gPendingOffMs.store(0);
    const bool prev = gEnabled.exchange(on);
    if (prev != on) Log("enabled=%d", on ? 1 : 0);
    if (!on) {
        ClearPending();
        anti_macro_follower::SetEnabled(false);
        SetWorldPause(false);
        if (gAlarmActive || gAlarmSuppressedUntilUiGone) {
            StopAlarm(/*suppressUntilUiGone=*/false);
            gAlarmSuppressedUntilUiGone = false;
        }
        gBusy.store(false);
        if (!IsInfraPhase(gPhase)) gPhase = "idle";
        WriteStatus(GetTickCount(), true);
    } else {
        RefreshInfra(GetTickCount(), true);
        WriteStatus(GetTickCount(), true);
    }
}

bool IsEnabled() { return gEnabled.load(); }

void SetDryRun(bool on) {
    const bool prev = gDryRun.exchange(on);
    if (prev != on) Log("dryRun=%d", on ? 1 : 0);
    lie_stats::SetSuppressed(on);
}

bool IsDryRun() { return gDryRun.load(); }

void SetMouseRegionOverlay(bool on) {
    anti_macro_follower::SetRegionOverlayEnabled(on);
}

bool IsMouseRegionOverlay() { return anti_macro_follower::IsRegionOverlayEnabled(); }

void StartAlarmTest() {
    const DWORD now = GetTickCount();
    gAlarmTestUntil.store(now + kAlarmTestDurationMs);
    // 与对照仓一致：起点记 now，约 3s 后首响；气泡立刻推一条。
    gLastAlarmTestSound.store(now);
    gLastAlarmTestNotify.store(now);
    gPhase = "alarm_test";
    Notify(x::features::notify::NotificationKind::Danger, "auto-lie-alarm-test",
           "测试测谎警报：正在自动测谎中", "这是 12 秒报警测试，不会答题或提交答案。",
           kAlarmNotifyTtlMs);
    // 面板不叠播：测试首响立刻由 payload 给出。
    xcat::sound::PlayAsync(xcat::sound::Id::Alarm);
    Log("alarm test started duration=%lums", static_cast<unsigned long>(kAlarmTestDurationMs));
    WriteStatus(now, true);
}

void StartMouseSmoke() {
    const DWORD now = GetTickCount();
    ClipCursor(nullptr);  // 清掉任何残留锁
    gMouseSmokeHaveOrigin = false;
    gMouseSmokeUntil.store(now + kMouseSmokeDurationMs);
    gPhase = "mouse_smoke";
    // 立刻硬闸，不等首 Tick（防止与战斗泵撞车）。
    x::features::simple_combat::SetHardPause(
        x::features::simple_combat::HardPauseHolder::AutoLie, true);
    Log("mouse smoke started duration=%lums (no ClipCursor, hard-pause combat)",
        static_cast<unsigned long>(kMouseSmokeDurationMs));
    WriteStatus(now, true);
}

void StartMouseSim(uint32_t seq) {
    mouse_trajectory_sim::RequestStart(seq);
    // 不占用 mouse_smoke phase（避免与光标烟测戳/状态串味）；sim 自有 IsRunning。
    Log("mouse sim requested seq=%u", seq);
    WriteStatus(GetTickCount(), true);
}

void Tick(DWORD now) { TickImpl(now); }

bool IsBusy() {
    return gBusy.load() || anti_macro_follower::IsFollowing() ||
           anti_macro_follower::IsUiVisible() || mouse_trajectory_sim::IsRunning() ||
           anti_macro_port::IsPredStale();
}

bool IsQuizActive() {
    return gBusy.load() || anti_macro_follower::IsFollowing() ||
           anti_macro_follower::IsUiVisible();
}

}  // namespace x::features::auto_lie
