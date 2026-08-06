#include "attach_inject.h"

#include "inject_after_launch.h"
#include "process_util.h"
#include "xcat_log.h"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace xcat::app::attach_inject {
namespace {

constexpr wchar_t kClassicExe[] = L"Maplestory_Classic.exe";
constexpr DWORD kWatchPollMs = 1000;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

std::wstring LaunchModePathRoot() { return ExeDir() + L"\\launch_mode.txt"; }

std::wstring g_prefsStateDir;  // …/XCat_data/state（可空）

std::wstring LaunchModePathState() {
    if (g_prefsStateDir.empty()) return {};
    return g_prefsStateDir + L"\\launch_mode.txt";
}

std::string NarrowUtf8(const std::wstring& w) { return xcat::WideToUtf8(w); }

void EnsureParentDir(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = filePath.substr(0, slash);
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
}

void WriteLaunchModeFile(const std::wstring& path, LaunchMode mode) {
    if (path.empty()) return;
    EnsureParentDir(path);
    const char* v = "attach_watch";
    if (mode == LaunchMode::GamaPassAuto) v = "gama_pass_auto";
    else if (mode == LaunchMode::OneClickLogin) v = "one_click";
    std::ofstream f(NarrowUtf8(path), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << v;
}

void SaveLaunchModeToDisk(LaunchMode mode) {
    // 双写：state（更新保留）+ 安装根（兼容旧路径/人工查看）
    WriteLaunchModeFile(LaunchModePathState(), mode);
    WriteLaunchModeFile(LaunchModePathRoot(), mode);
}

LaunchMode ParseLaunchModeRaw(std::string raw) {
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
        raw.pop_back();
    for (char& c : raw) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (raw == "gama_pass_auto" || raw == "gamapass_auto" || raw == "gama_pass" ||
        raw == "gamapass" || raw == "2") {
        return LaunchMode::GamaPassAuto;
    }
    if (raw == "one_click" || raw == "oneclick" || raw == "1" || raw == "login") {
        return LaunchMode::OneClickLogin;
    }
    if (raw == "attach_watch" || raw == "attach" || raw == "0" || raw == "watch") {
        return LaunchMode::AttachWatch;
    }
    return LaunchMode::AttachWatch;
}

bool TryReadLaunchModeFile(const std::wstring& path, LaunchMode* out) {
    if (!out || path.empty()) return false;
    std::ifstream f(NarrowUtf8(path), std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.empty()) return false;
    *out = ParseLaunchModeRaw(std::move(raw));
    return true;
}

LaunchMode LoadLaunchModeFromDisk() {
    LaunchMode mode = LaunchMode::AttachWatch;
    const std::wstring statePath = LaunchModePathState();
    const std::wstring rootPath = LaunchModePathRoot();
    // 优先 state（更新白名单），其次安装根兼容文件
    if (TryReadLaunchModeFile(statePath, &mode) || TryReadLaunchModeFile(rootPath, &mode)) {
        SaveLaunchModeToDisk(mode);
        return mode;
    }
    SaveLaunchModeToDisk(LaunchMode::AttachWatch);
    return LaunchMode::AttachWatch;
}

struct State {
    std::mutex mu;
    std::mutex threadMu;       // watchThread / asyncJoinThread
    LogFn log;
    LaunchMode mode = LaunchMode::AttachWatch;
    std::atomic<bool> watching{false};
    std::atomic<bool> stopWatch{false};
    std::atomic<bool> injectBusy{false};
    std::atomic<DWORD> lastHandledPid{0};
    std::atomic<DWORD> lastSeenPid{0};
    std::string status = "未监视";
    std::thread watchThread;
    std::thread asyncJoinThread;  // UI 侧 StopWatch 异步收尸
    HANDLE wakeEvent = nullptr;   // manual-reset：停监视时立刻打断 Sleep
};

State g;

void EnsureWakeEvent() {
    if (g.wakeEvent) return;
    HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE prev = static_cast<HANDLE>(InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g.wakeEvent), created, nullptr));
    if (prev != nullptr && created) CloseHandle(created);
}

void WakeWatch() {
    EnsureWakeEvent();
    if (g.wakeEvent) SetEvent(g.wakeEvent);
}

void ResetWake() {
    EnsureWakeEvent();
    if (g.wakeEvent) ResetEvent(g.wakeEvent);
}

// 可中断等待：StopWatch 置停旗并 SetEvent 后立刻返回，避免 UI join 卡满整秒 poll。
bool WaitWatchOrStop(DWORD ms) {
    if (g.stopWatch.load(std::memory_order_acquire)) return true;
    EnsureWakeEvent();
    if (!g.wakeEvent) {
        Sleep(ms);
        return g.stopWatch.load(std::memory_order_acquire);
    }
    WaitForSingleObject(g.wakeEvent, ms);
    return g.stopWatch.load(std::memory_order_acquire);
}

void JoinAsyncJoinThread() {
    std::thread joiner;
    {
        std::lock_guard<std::mutex> lock(g.threadMu);
        if (g.asyncJoinThread.joinable()) joiner = std::move(g.asyncJoinThread);
    }
    if (joiner.joinable()) joiner.join();
}

void JoinWatchThreadHandle() {
    std::thread watch;
    {
        std::lock_guard<std::mutex> lock(g.threadMu);
        if (g.watchThread.joinable()) watch = std::move(g.watchThread);
    }
    if (watch.joinable()) watch.join();
}

// StartWatch / Shutdown：确保上一轮监视（含异步 Stop）已彻底结束。
void DrainAllWatchActivity() {
    JoinAsyncJoinThread();
    JoinWatchThreadHandle();
}

void Emit(const std::wstring& line) {
    LogFn fn;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        fn = g.log;
    }
    if (fn) fn(line);
}

void SetStatus(std::string s) {
    std::lock_guard<std::mutex> lock(g.mu);
    g.status = std::move(s);
}

bool DoInject(DWORD pid) {
    if (!pid) return false;
    if (g.injectBusy.exchange(true)) {
        Emit(L"[Attach] 注入进行中，跳过");
        return false;
    }
    bool ok = false;
    try {
        xcat::log::Info("Attach", "inject begin pid=%lu", static_cast<unsigned long>(pid));
        Emit(L"[…] 附着注入：等待 GameAssembly 后 LoadLibraryW… PID=" + std::to_wstring(pid));
        xcat::twms_inject::Options iopt;
        iopt.pid = pid;
        auto ir = xcat::twms_inject::InjectIntoClassic(
            iopt, [](const std::wstring& line) {
                Emit(line);
                // 同步进 launcher.jsonl，避免只卡在 UI「注入中」却看不见进度。
                xcat::log::Info("Attach", "%s", xcat::WideToUtf8(line).c_str());
            });
        if (ir.ok) {
            ok = true;
            // 仅成功记 PID，失败可重试（等 GA / 提权后再试）。
            g.lastHandledPid.store(pid, std::memory_order_release);
            Emit(L"[OK] 附着注入完成");
            SetStatus("已注入 PID=" + std::to_string(pid));
            xcat::log::Ok("Attach", "inject ok pid=%lu msg=%s",
                          static_cast<unsigned long>(pid), ir.message.c_str());
        } else {
            Emit(L"[FAIL] 附着注入未完成：" + xcat::Utf8ToWide(ir.message));
            SetStatus("注入失败，将重试 PID=" + std::to_string(pid));
            xcat::log::Warn("Attach", "inject fail pid=%lu msg=%s (will retry)",
                            static_cast<unsigned long>(pid), ir.message.c_str());
        }
    } catch (...) {
        Emit(L"[FAIL] 附着注入异常");
        SetStatus("注入异常，将重试");
        xcat::log::Warn("Attach", "inject exception pid=%lu", static_cast<unsigned long>(pid));
    }
    g.injectBusy.store(false, std::memory_order_release);
    return ok;
}

void WatchLoop() {
    xcat::log::Info("Attach", "watch loop start");
    Emit(L"[Attach] 开始监视游戏进程");
    SetStatus("监视中：等待游戏进程…");
    while (!g.stopWatch.load(std::memory_order_acquire)) {
        const DWORD pid = xcat::FindProcessIdByName(kClassicExe);
        g.lastSeenPid.store(pid, std::memory_order_release);
        if (!pid) {
            if (g.lastHandledPid.load(std::memory_order_acquire) != 0) {
                g.lastHandledPid.store(0, std::memory_order_release);
                SetStatus("监视中：等待游戏进程…");
                Emit(L"[Attach] 游戏进程已退出，继续等待…");
            }
            if (WaitWatchOrStop(kWatchPollMs)) break;
            continue;
        }

        const DWORD handled = g.lastHandledPid.load(std::memory_order_acquire);
        if (handled == pid || g.injectBusy.load(std::memory_order_acquire)) {
            if (!g.injectBusy.load(std::memory_order_acquire) && handled == pid) {
                SetStatus("已注入 PID=" + std::to_string(pid));
            }
            if (WaitWatchOrStop(kWatchPollMs)) break;
            continue;
        }

        // 停旗已立则不再开注入，避免切模式后还卡在 InjectIntoClassic。
        if (g.stopWatch.load(std::memory_order_acquire)) break;

        // 新 PID 或尚未成功注入：尝试注入（已加载 payload 时 InjectIntoClassic 会跳过）。
        SetStatus("发现游戏 PID=" + std::to_string(pid) + "，注入中…");
        Emit(L"[Attach] 发现游戏进程 PID=" + std::to_wstring(pid));
        xcat::log::Info("Attach", "found classic pid=%lu", static_cast<unsigned long>(pid));
        const bool ok = DoInject(pid);
        if (g.stopWatch.load(std::memory_order_acquire)) break;
        // 失败后稍等再试，避免立刻打满 OpenProcess；成功则下一轮靠 lastHandledPid 跳过。
        if (WaitWatchOrStop(ok ? kWatchPollMs : 5000)) break;
    }
    SetStatus("已停止监视");
    Emit(L"[Attach] 监视已停止");
    xcat::log::Info("Attach", "watch loop stop");
}

}  // namespace

void Init(LogFn log) { Init(std::move(log), {}); }

void Init(LogFn log, const std::string& prefsBinDir) {
    std::lock_guard<std::mutex> lock(g.mu);
    g.log = std::move(log);
    if (!prefsBinDir.empty()) {
        g_prefsStateDir = xcat::Utf8ToWide(prefsBinDir + "\\state");
    } else {
        g_prefsStateDir.clear();
    }
    g.mode = LoadLaunchModeFromDisk();
    xcat::log::Info("Attach", "init mode=%s state=%s", LaunchModeLabel(g.mode),
                    g_prefsStateDir.empty() ? "(root-only)" : NarrowUtf8(g_prefsStateDir).c_str());
}

void Shutdown() {
    StopWatch();
    DrainAllWatchActivity();
    if (g.wakeEvent) {
        CloseHandle(g.wakeEvent);
        g.wakeEvent = nullptr;
    }
    std::lock_guard<std::mutex> lock(g.mu);
    g.log = {};
}

LaunchMode GetLaunchMode() {
    std::lock_guard<std::mutex> lock(g.mu);
    return g.mode;
}

void SetLaunchMode(LaunchMode mode) {
    {
        std::lock_guard<std::mutex> lock(g.mu);
        g.mode = mode;
    }
    SaveLaunchModeToDisk(mode);
    Emit(std::wstring(L"[OK] 启动模式已切换为 ") +
         xcat::Utf8ToWide(LaunchModeLabel(mode)));
    xcat::log::Info("Attach", "mode -> %s", LaunchModeLabel(mode));
}

const char* LaunchModeLabel(LaunchMode mode) {
    switch (mode) {
    case LaunchMode::GamaPassAuto:
        return "GAMA PASS自动登录";
    case LaunchMode::OneClickLogin:
        return "gamania (HK)";
    case LaunchMode::AttachWatch:
    default:
        return "手动启动并注入";
    }
}

bool StartWatch() {
    if (g.watching.load(std::memory_order_acquire)) return true;
    // 等上一轮异步 Stop 收尸完，再开新监视（可能短暂阻塞；正常路径几乎瞬时）。
    DrainAllWatchActivity();
    g.stopWatch.store(false, std::memory_order_release);
    ResetWake();
    try {
        g.watching.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g.threadMu);
        g.watchThread = std::thread([] {
            WatchLoop();
            g.watching.store(false, std::memory_order_release);
        });
    } catch (...) {
        g.watching.store(false, std::memory_order_release);
        SetStatus("监视线程启动失败");
        Emit(L"[FAIL] 无法启动监视线程");
        return false;
    }
    return true;
}

void StopWatch() {
    // UI 线程安全：只置停旗 + 唤醒 poll，join 放到后台，避免 ImGui 切模式卡 1s+。
    g.stopWatch.store(true, std::memory_order_release);
    WakeWatch();
    g.watching.store(false, std::memory_order_release);

    std::thread watch;
    {
        std::lock_guard<std::mutex> lock(g.threadMu);
        if (g.watchThread.joinable()) watch = std::move(g.watchThread);
        if (!watch.joinable()) return;
        // 串行化异步收尸：若上一轮 joiner 还在，先接到它后面再 join 本轮。
        if (g.asyncJoinThread.joinable()) {
            std::thread prev = std::move(g.asyncJoinThread);
            g.asyncJoinThread = std::thread([prev = std::move(prev), watch = std::move(watch)]() mutable {
                if (prev.joinable()) prev.join();
                if (watch.joinable()) watch.join();
            });
        } else {
            g.asyncJoinThread = std::thread([watch = std::move(watch)]() mutable {
                if (watch.joinable()) watch.join();
            });
        }
    }
}

bool IsWatching() { return g.watching.load(std::memory_order_acquire); }

bool IsInjectBusy() { return g.injectBusy.load(std::memory_order_acquire); }

bool InjectNow(std::wstring* errOut) {
    const DWORD pid = xcat::FindProcessIdByName(kClassicExe);
    if (!pid) {
        if (errOut) *errOut = L"未找到游戏进程，请先手动启动游戏";
        Emit(L"[FAIL] 立即注入：未找到游戏进程");
        return false;
    }
    if (g.injectBusy.load(std::memory_order_acquire)) {
        if (errOut) *errOut = L"注入进行中，请稍候";
        return false;
    }
    // 允许对同 PID 再试（用户显式点「立即注入」）。
    g.lastHandledPid.store(0, std::memory_order_release);
    std::thread([pid] { (void)DoInject(pid); }).detach();
    return true;
}

bool InjectCustomDll(const std::wstring& dllPath, bool waitGameAssembly, std::wstring* errOut) {
    if (dllPath.empty()) {
        if (errOut) *errOut = L"请先选择 DLL 路径";
        return false;
    }
    std::wstring abs;
    if (!xcat::ResolveAbsolutePath(dllPath, abs) ||
        GetFileAttributesW(abs.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (errOut) *errOut = L"DLL 不存在或路径无效";
        Emit(L"[FAIL] 自定义注入：DLL 无效 " + dllPath);
        return false;
    }
    const DWORD pid = xcat::FindProcessIdByName(kClassicExe);
    if (!pid) {
        if (errOut) *errOut = L"未找到游戏进程 Maplestory_Classic.exe";
        Emit(L"[FAIL] 自定义注入：未找到游戏进程");
        return false;
    }
    if (g.injectBusy.load(std::memory_order_acquire)) {
        if (errOut) *errOut = L"注入进行中，请稍候";
        return false;
    }

    Emit(L"[…] 自定义注入排队：PID=" + std::to_wstring(pid) + L" dll=" + abs +
         (waitGameAssembly ? L"（等 GameAssembly）" : L"（不等 GameAssembly）"));
    xcat::log::Info("Attach", "custom inject begin pid=%lu waitGA=%d dll=%s",
                    static_cast<unsigned long>(pid), waitGameAssembly ? 1 : 0,
                    NarrowUtf8(abs).c_str());

    std::thread([pid, abs, waitGameAssembly] {
        if (g.injectBusy.exchange(true)) {
            Emit(L"[Attach] 自定义注入：与其它注入冲突，跳过");
            return;
        }
        try {
            xcat::twms_inject::Options iopt;
            iopt.pid = pid;
            iopt.dllPath = abs;
            iopt.waitForGameAssembly = waitGameAssembly;
            iopt.registerInjectLog = false;
            iopt.settleMs = waitGameAssembly ? 1000 : 0;
            auto ir = xcat::twms_inject::InjectIntoClassic(
                iopt, [](const std::wstring& line) {
                    Emit(line);
                    xcat::log::Info("Attach", "%s", xcat::WideToUtf8(line).c_str());
                });
            // 故意不写 lastHandledPid：监视线程仍应对正式 xcat.dll 负责。
            if (ir.ok) {
                Emit(L"[OK] 自定义注入完成");
                xcat::log::Ok("Attach", "custom inject ok pid=%lu msg=%s",
                              static_cast<unsigned long>(pid), ir.message.c_str());
            } else {
                Emit(L"[FAIL] 自定义注入未完成：" + xcat::Utf8ToWide(ir.message));
                xcat::log::Warn("Attach", "custom inject fail pid=%lu msg=%s",
                                static_cast<unsigned long>(pid), ir.message.c_str());
            }
        } catch (...) {
            Emit(L"[FAIL] 自定义注入异常");
            xcat::log::Warn("Attach", "custom inject exception pid=%lu",
                            static_cast<unsigned long>(pid));
        }
        g.injectBusy.store(false, std::memory_order_release);
    }).detach();
    return true;
}

DWORD LastHandledPid() { return g.lastHandledPid.load(std::memory_order_acquire); }

std::string StatusBrief() {
    std::lock_guard<std::mutex> lock(g.mu);
    return g.status;
}

}  // namespace xcat::app::attach_inject
