#include "auto_lie.h"
#include "anti_macro_follower.h"
#include "anti_macro_port.h"

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

std::atomic<bool> gEnabled{false};
std::atomic<bool> gDryRun{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gBusy{false};
std::atomic<DWORD> gAlarmTestUntil{0};
std::atomic<DWORD> gLastAlarmTestSound{0};
std::atomic<DWORD> gLastAlarmTestNotify{0};
std::atomic<DWORD> gMouseSmokeUntil{0};

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
    x::runtime::LogI("AutoLie", "%s", buf);
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
    if (gWorldPaused == on) return;
    gWorldPaused = on;
    x::features::simple_combat::SetExternalPause(on);
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
            ClipCursor(nullptr);
            gMouseSmokeHaveOrigin = false;
        }
        return;
    }
    if (static_cast<LONG>(now - until) >= 0) {
        ClipCursor(nullptr);
        gMouseSmokeUntil.store(0);
        gMouseSmokeHaveOrigin = false;
        Log("mouse smoke finished");
        if (gPhase == "mouse_smoke") gPhase = "idle";
        return;
    }
    gPhase = "mouse_smoke";
    // 仅游戏前台时锁光标；失焦释放，计时继续（避免锁死桌面）
    if (!anti_macro_port::IsGameForeground()) {
        if (gMouseSmokeHaveOrigin) {
            ClipCursor(nullptr);
            gMouseSmokeHaveOrigin = false;
        }
        return;
    }
    if (!gMouseSmokeHaveOrigin) {
        GetCursorPos(&gMouseSmokeOrigin);
        gMouseSmokeHaveOrigin = true;
        RECT clip{gMouseSmokeOrigin.x - 48, gMouseSmokeOrigin.y - 48, gMouseSmokeOrigin.x + 48,
                  gMouseSmokeOrigin.y + 48};
        ClipCursor(&clip);
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
    TickAlarmTest(now);
    TickMouseSmoke(now);
    RefreshInfra(now);

    if (!gEnabled.load()) {
        if (gWorldPaused) SetWorldPause(false);
        // 关自动测谎：立刻停报警音/撤气泡（测试报警基建仍可独立跑）。
        if (gAlarmActive || gAlarmSuppressedUntilUiGone) {
            StopAlarm(/*suppressUntilUiGone=*/false);
            gAlarmSuppressedUntilUiGone = false;
        }
        ClearPending();
        gBusy.store(false);
        if (!IsInfraPhase(gPhase)) gPhase = "idle";
        anti_macro_follower::SetEnabled(false);
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
    const bool mouseOpen = anti_macro_port::IsNonFiniteOpen();
    const bool anyOpen =
        anti_macro_port::IsOpenAntiMacro() || textOpen || mouseOpen;

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

}  // namespace

void Init() {
    anti_macro_follower::Init();
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
    if (!th) return;
    const DWORD wait = WaitForSingleObject(th, 5000);
    if (wait == WAIT_TIMEOUT) {
        Log("StopWorker wait timeout; thread may still be exiting");
    }
    CloseHandle(th);
}

void SetEnabled(bool on) {
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
}

bool IsDryRun() { return gDryRun.load(); }

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
    ClipCursor(nullptr);
    gMouseSmokeHaveOrigin = false;
    gMouseSmokeUntil.store(now + kMouseSmokeDurationMs);
    gPhase = "mouse_smoke";
    Log("mouse smoke started duration=%lums",
        static_cast<unsigned long>(kMouseSmokeDurationMs));
    WriteStatus(now, true);
}

void Tick(DWORD now) { TickImpl(now); }

bool IsBusy() { return gBusy.load() || anti_macro_follower::IsFollowing(); }

}  // namespace x::features::auto_lie
