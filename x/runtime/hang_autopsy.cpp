#include "hang_autopsy.h"

#include <TlHelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bin_dir.h"
#include "il2cpp_metadata_lock.h"
#include "log.h"
#include "main_thread_pump.h"

namespace x::runtime::hang_autopsy {
namespace {

// 主泵连续这么久没有真 tick 就认定卡死。换场景/登录时主泵合法静默数秒，
// 阈值取得比那长一截；真死锁是永久的，多等几秒不影响取证。
constexpr DWORD kStallMs = 12000;
constexpr DWORD kPollMs = 1000;
// 一次运行最多抓这么多份，防止反复卡死把磁盘写满。
constexpr int kMaxCapturesPerSession = 4;
constexpr int kMaxFrames = 64;

// il2cpp 的「类方法查找锁」——2026-08-09 卡死现场里主线程就死在这上面。地址解析、
// 合理性校验与泄漏归还都在 il2cpp_metadata_lock 里，这边只负责把状态写进报告。
using MethodLockState = x::runtime::il2cpp_metadata_lock::State;

struct ModuleRange {
    uintptr_t base = 0;
    uintptr_t end = 0;
    char name[64]{};
};

struct ThreadStack {
    DWORD tid = 0;
    bool opened = false;
    bool suspended = false;
    bool gotContext = false;
    int n = 0;
    uint64_t frames[kMaxFrames]{};
};

std::atomic_bool gRunning{false};
std::atomic_bool gStop{false};
HANDLE gThread = nullptr;
std::atomic_int gCaptures{0};

std::vector<ModuleRange> SnapshotModules() {
    std::vector<ModuleRange> out;
    const HANDLE snap =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleRange r;
            r.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            r.end = r.base + me.modBaseSize;
            WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, r.name, sizeof(r.name) - 1, nullptr,
                                nullptr);
            out.push_back(r);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

std::vector<DWORD> SnapshotThreadIds() {
    std::vector<DWORD> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    const DWORD self = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                                 sizeof(te.th32OwnerProcessID) &&
                te.th32OwnerProcessID == self)
                out.push_back(te.th32ThreadID);
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out;
}

// 目标线程处于挂起态时调用。全程不分配、不取锁、不写日志——被挂起的线程随时
// 可能正持有 CRT 堆锁或日志锁，在这里碰它们会把 XCat 自己一起锁死。
int UnwindSuspended(CONTEXT& ctx, uint64_t* out, int maxFrames) {
    int n = 0;
    __try {
        while (n < maxFrames && ctx.Rip) {
            out[n++] = ctx.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                // 没有 unwind 信息（叶子函数 / il2cpp 之外的动态代码）：
                // 按 x64 约定弹一次返回地址continue，弹不动就收工。
                if (!ctx.Rsp) break;
                const DWORD64 ret = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += sizeof(DWORD64);
                if (!ret) break;
                ctx.Rip = ret;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData,
                             &establisher, nullptr);
            if (!ctx.Rip) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 栈走出了可读范围：保留已收到的帧，别把整份取证赔进去。
    }
    return n;
}

const ModuleRange* FindModule(const std::vector<ModuleRange>& mods, uint64_t addr) {
    for (const ModuleRange& m : mods) {
        if (addr >= m.base && addr < m.end) return &m;
    }
    return nullptr;
}

void AppendFrame(std::string& text, const std::vector<ModuleRange>& mods, uint64_t addr) {
    char line[160];
    const ModuleRange* m = FindModule(mods, addr);
    if (m) {
        snprintf(line, sizeof(line), "    %016llx  %s+0x%llx\n",
                 static_cast<unsigned long long>(addr), m->name,
                 static_cast<unsigned long long>(addr - m->base));
    } else {
        snprintf(line, sizeof(line), "    %016llx  <unknown>\n",
                 static_cast<unsigned long long>(addr));
    }
    text += line;
}

std::string LogDir() {
    std::string dir = GetBinDir();  // …/bin/XCat_data/
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    dir += "logs";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\hang";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

bool WriteCapture(const std::string& text, std::string* outPath) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "\\hang_%04u%02u%02u_%02u%02u%02u.txt", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::string path = LogDir() + leaf;
    const HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    if (outPath) *outPath = path;
    return ok != FALSE && written == text.size();
}

bool Capture(const char* reason) {
    // ── A. 准备：还没挂起任何人，可以放心分配 ────────────────────────────
    const std::vector<ModuleRange> mods = SnapshotModules();
    const std::vector<DWORD> tids = SnapshotThreadIds();
    if (tids.empty()) return false;

    const DWORD self = GetCurrentThreadId();
    const DWORD pumpTid = main_thread::PumpThreadId();
    const DWORD tickAge = main_thread::LastRealTickAgeMs();
    const MethodLockState methodLock = x::runtime::il2cpp_metadata_lock::Read();

    std::vector<ThreadStack> stacks(tids.size());

    // 预热 RtlLookupFunctionEntry 的惰性初始化，别把它留到挂起别人之后再触发。
    {
        DWORD64 imageBase = 0;
        (void)RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&Capture), &imageBase, nullptr);
    }

    // ── B. 采集：每个线程的挂起窗口里只做 GetThreadContext + 回溯 ──────────
    size_t k = 0;
    for (const DWORD tid : tids) {
        if (tid == self) continue;
        ThreadStack& ts = stacks[k++];
        ts.tid = tid;
        const HANDLE th = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th) continue;
        ts.opened = true;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            ts.suspended = true;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                ts.gotContext = true;
                ts.n = UnwindSuspended(ctx, ts.frames, kMaxFrames);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    stacks.resize(k);

    // ── C. 落盘：所有线程都已恢复，随便分配 ────────────────────────────────
    size_t withContext = 0;
    for (const ThreadStack& ts : stacks) {
        if (ts.gotContext) ++withContext;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char head[512];
    snprintf(head, sizeof(head),
             "XCat hang autopsy\n"
             "time      : %04u-%02u-%02u %02u:%02u:%02u.%03u\n"
             "reason    : %s\n"
             "pid       : %lu\n"
             "pump tid  : %lu (0 = 主泵未记录过宿主线程)\n"
             "tick age  : %lu ms\n"
             "threads   : %zu (含 %zu 个成功取到上下文)\n"
             "modules   : %zu\n\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             reason ? reason : "?", GetCurrentProcessId(), pumpTid, tickAge, stacks.size(),
             withContext, mods.size());

    std::string text = head;

    // il2cpp 类方法查找锁：持有者 tid 直接点名，省得靠栈形状猜。
    {
        char lockText[512];
        if (!methodLock.read) {
            snprintf(lockText, sizeof(lockText),
                     "il2cpp method lock: 读取失败（GameAssembly 未加载或 RVA 越界）\n\n");
        } else if (!methodLock.plausible) {
            snprintf(lockText, sizeof(lockText),
                     "il2cpp method lock: 取值不合理（word=0x%x owner=0x%llx）——RVA 很可能已随"
                     "客户端更新漂移，本节作废\n\n",
                     methodLock.word, static_cast<unsigned long long>(methodLock.ownerTid));
        } else {
            const char* state = methodLock.word == 0   ? "空闲"
                                : methodLock.word == 1 ? "已持有（无等待者）"
                                                       : "已持有（有等待者在 futex 上）";
            const DWORD ownerTid = static_cast<DWORD>(methodLock.ownerTid);
            bool ownerIsXcat = false;
            bool ownerSeen = false;
            for (const ThreadStack& ts : stacks) {
                if (ts.tid != ownerTid) continue;
                ownerSeen = true;
                for (int i = 0; i < ts.n; ++i) {
                    const ModuleRange* m = FindModule(mods, ts.frames[i]);
                    if (m && _stricmp(m->name, "xcat.dll") == 0) ownerIsXcat = true;
                }
            }
            snprintf(lockText, sizeof(lockText),
                     "il2cpp method lock: %s  word=%u recursion=%u\n"
                     "  持有者 tid : %lu%s%s\n"
                     "  （持有者若是 xcat.dll 线程 = XCat 的锅；若是游戏线程 = 客户端自身死锁）\n\n",
                     state, methodLock.word, methodLock.recursion, ownerTid,
                     ownerSeen ? "" : "  [该线程已不在快照里]",
                     ownerIsXcat ? "  ← 栈里有 xcat.dll" : "");
        }
        text += lockText;
    }

    // 排序：主泵线程 → 锁持有者 → 其余。前两者是本次取证的主角。
    const DWORD ownerTid =
        (methodLock.read && methodLock.plausible) ? static_cast<DWORD>(methodLock.ownerTid) : 0;
    for (int pass = 0; pass < 3; ++pass) {
        for (const ThreadStack& ts : stacks) {
            const bool isPump = pumpTid != 0 && ts.tid == pumpTid;
            const bool isOwner = ownerTid != 0 && ts.tid == ownerTid && !isPump;
            const int bucket = isPump ? 0 : (isOwner ? 1 : 2);
            if (bucket != pass) continue;

            char hdr[192];
            snprintf(hdr, sizeof(hdr), "--- tid %lu%s%s%s (%d frames)\n", ts.tid,
                     isPump ? "  [PUMP / Unity main]" : "",
                     isOwner ? "  [HOLDS il2cpp method lock]" : "",
                     ts.gotContext ? "" : (ts.opened ? "  [context unavailable]" : "  [open failed]"),
                     ts.n);
            text += hdr;
            for (int i = 0; i < ts.n; ++i) AppendFrame(text, mods, ts.frames[i]);
            text += '\n';
        }
    }

    text += "--- modules\n";
    for (const ModuleRange& m : mods) {
        char line[192];
        snprintf(line, sizeof(line), "  %016llx-%016llx  %s\n",
                 static_cast<unsigned long long>(m.base), static_cast<unsigned long long>(m.end),
                 m.name);
        text += line;
    }

    std::string path;
    const bool ok = WriteCapture(text, &path);
    if (ok)
        LogW("HangAutopsy", "卡死取证已落盘 reason=%s threads=%zu -> %s", reason ? reason : "?",
             stacks.size(), path.c_str());
    else
        LogE("HangAutopsy", "卡死取证落盘失败 reason=%s", reason ? reason : "?");
    return ok;
}

DWORD WINAPI WatchdogThread(LPVOID) {
    bool armed = false;      // 主泵真正 tick 过之后才开始判死
    bool capturedThisEpisode = false;
    // 元数据锁的持有者连续性：逐个 tick 盯着，换人就重新计时。
    uint32_t lockOwner = 0;
    DWORD lockOwnerSinceMs = 0;

    while (!gStop.load(std::memory_order_acquire)) {
        Sleep(kPollMs);
        if (gStop.load(std::memory_order_acquire)) break;

        if (!main_thread::IsInstalled()) continue;
        if (!armed) {
            if (main_thread::RealTickCount() == 0) continue;
            armed = true;
        }

        {
            const auto lk = x::runtime::il2cpp_metadata_lock::Read();
            const uint32_t owner =
                (lk.read && lk.plausible && lk.word) ? static_cast<uint32_t>(lk.ownerTid) : 0;
            if (owner != lockOwner) {
                lockOwner = owner;
                lockOwnerSinceMs = GetTickCount();
            }
        }

        if (main_thread::IsPumpTicking(kStallMs)) {
            capturedThisEpisode = false;  // 活过来了，下次再卡算新一轮
            continue;
        }
        if (!capturedThisEpisode &&
            gCaptures.load(std::memory_order_relaxed) < kMaxCapturesPerSession) {
            capturedThisEpisode = true;
            gCaptures.fetch_add(1, std::memory_order_relaxed);
            char reason[96];
            snprintf(reason, sizeof(reason), "主泵静默 %lu ms（阈值 %lu）",
                     main_thread::LastRealTickAgeMs(), kStallMs);
            Capture(reason);
        }

        // 取证写完再抢救，保证现场先落盘。抢救不受取证配额限制：取证是为了查因，
        // 这一步是为了让用户能继续玩，抓满 4 份之后仍然要能自救。
        // 只有「同一个非主泵线程把锁攥了整段静默期」才动手：正常元数据查找是微秒级，
        // 十几秒不放只可能是某个 __except 把 il2cpp 内部的异常吞掉、跳过了解锁。
        // 这是兜底网——插桩追不完各 feature 里的私有包装，这里不挑调用点，一律捞回来。
        const DWORD held = GetTickCount() - lockOwnerSinceMs;
        if (lockOwner && lockOwner != main_thread::PumpThreadId() && held >= kStallMs) {
            x::runtime::il2cpp_metadata_lock::ForceReleaseIfOwnedBy(lockOwner, "pump-stall");
        }
    }
    gRunning.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

void Start() {
    bool expected = false;
    if (!gRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    gStop.store(false, std::memory_order_release);
    gThread = CreateThread(nullptr, 0, &WatchdogThread, nullptr, 0, nullptr);
    if (!gThread) {
        gRunning.store(false, std::memory_order_release);
        LogW("HangAutopsy", "看门狗线程创建失败——卡死时不会有取证");
        return;
    }
    LogI("HangAutopsy", "看门狗已启动：主泵静默 %lu ms 即回溯全部线程栈（每次运行最多 %d 份）",
         kStallMs, kMaxCapturesPerSession);
}

void Stop() {
    gStop.store(true, std::memory_order_release);
    if (gThread) {
        CloseHandle(gThread);  // 只放句柄，不 join：DETACH 在加载器锁上
        gThread = nullptr;
    }
}

bool CaptureNow(const char* reason) { return Capture(reason ? reason : "manual"); }

}  // namespace x::runtime::hang_autopsy
