// Classic TWMS — 抓「在 il2cpp 引擎代码内部发生」的首次异常。
//
// 为什么需要它：
// 2026-08-09 多次卡死都指向同一条链——某个 XCat 工作线程调 il2cpp 元数据 API，API 内部
// 抛异常，XCat 的 __except 把它吞掉。展开时跨过了 il2cpp 手写递归锁的解锁代码，锁永久
// 泄漏；更糟的是 il2cpp 的元数据状态机停在半更新状态，光把锁归还只能把死锁换成活锁
// （05:40 实测：主线程在 GameAssembly+0x3f5663 一带 100% 空转）。
//
// 逐个给 __except 插桩追不完：全仓七百多处，各 feature 还各抄了一份私有包装。所以改在
// 异常发生的那一刻抓——VEH 是首次异常，早于任何 __except，谁都绕不过去。
//
// ★ VEH 里能做什么，是这个文件唯一的难点 ★
// 06:01 那一版在处理函数里直接调了 LogW，客户端 LOADING 时当场 abort：事件日志记的是
// 异常码 0xE06D7363、故障模块 KERNELBASE.dll，即**未处理的 C++ 异常**。LOADING 阶段
// il2cpp 本来就在密集抛托管异常（在 il2cpp 里就是 C++ 异常），我们在别人的异常派发过程
// 中又抛一个新的（LogW 会加锁、分配内存），直接 std::terminate。
//
// 所以处理函数现在守三条铁律：
//   1. 不分配、不加锁、不调任何可能抛异常的东西——只往预分配的环形缓冲里填 POD；
//   2. 不碰加载器（GetModuleHandle / GetModuleFileName 一律不调）。LOADING 时加载器锁
//      正被别人持着，在 VEH 里去拿必死；模块地址一律由后台线程预先解析好缓存；
//   3. 整个函数体裹在 __try 里兜底，并有线程局部重入闩。
// 格式化、模块名解析、落盘全部由后台 flusher 线程做，那里怎么抛都不影响异常派发。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "il2cpp_fault_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bin_dir.h"
#include "il2cpp_bind.h"
#include "il2cpp_metadata_lock.h"
#include "log.h"

namespace x::runtime::il2cpp_fault_probe {
namespace {

namespace il2 = x::runtime::il2cpp;

// 32 帧不是拍脑袋：06:11 那轮用 12 帧，四次「持锁时抛异常」的栈里一个 xcat.dll 都没照到，
// 而 il2cpp 的类初始化链（class_init → .cctor → 一串托管调用）轻松就超过十几帧。
constexpr int kFrames = 32;
constexpr int kRing = 64;

// 两套彼此独立的额度。2026-08-09 05:52 的教训：单一的 24 条总额被 final_attack_force
// 那个每秒重复几十次的空指针读刷爆（05:52:01 用完），真正泄漏锁的异常发生在 05:52:04，
// 正好被挤掉。判据是「异常发生时元数据锁在不在本线程名下」。
constexpr int kMaxOwnedReports = 64;
constexpr int kMaxNoiseReports = 24;
constexpr int kMaxPerSignature = 3;
constexpr int kMaxSignatures = 16;

struct Rec {
    std::atomic<uint8_t> state{0};  // 0=空 1=正在写 2=可读
    DWORD code = 0;
    DWORD tid = 0;
    uint64_t faultAt = 0;
    uint64_t target = 0;
    uint64_t op = 0;
    uint32_t word = 0;
    uint32_t recursion = 0;
    uint8_t owned = 0;
    uint8_t nframes = 0;
    uint64_t frames[kFrames]{};
};

Rec gRing[kRing];
std::atomic<uint32_t> gWriteSeq{0};
std::atomic<uint32_t> gDropped{0};

std::atomic<int> gOwnedReported{0};
std::atomic<int> gNoiseReported{0};
// 未截断的总数：持锁时抛异常若是 LOADING 期的常态，光看前 64 条会误判频次。
std::atomic<uint32_t> gOwnedSeen{0};

std::atomic<bool> gRunning{false};
std::atomic<bool> gStop{false};
void* gHandle = nullptr;
HANDLE gFlusher = nullptr;
std::atomic<DWORD> gTlsGuard{TLS_OUT_OF_INDEXES};

// 这三段范围由后台线程解析好，处理函数只读，绝不在 VEH 里碰加载器。
std::atomic<uint64_t> gGaBase{0};
std::atomic<uint64_t> gGaEnd{0};

bool InRange(uint64_t a, const std::atomic<uint64_t>& b, const std::atomic<uint64_t>& e) {
    const uint64_t lo = b.load(std::memory_order_relaxed);
    const uint64_t hi = e.load(std::memory_order_acquire);
    return lo && a >= lo && a < hi;
}

// 噪声去重表：同一个 (异常码, 出错 RVA) 只报前几次。
struct Signature {
    std::atomic<uint64_t> key{0};
    std::atomic<int> count{0};
};
Signature gSignatures[kMaxSignatures];

bool NoiseBudgetOk(DWORD code, uint64_t rva) {
    const uint64_t key = (static_cast<uint64_t>(code) << 32) ^ (rva | 1);
    for (auto& s : gSignatures) {
        uint64_t cur = s.key.load(std::memory_order_acquire);
        if (cur == 0) {
            uint64_t expect = 0;
            cur = s.key.compare_exchange_strong(expect, key, std::memory_order_acq_rel) ? key
                                                                                        : expect;
        }
        if (cur != key) continue;
        return s.count.fetch_add(1, std::memory_order_relaxed) < kMaxPerSignature;
    }
    return false;
}

// 用异常上下文的副本往回走几帧。x64 有完整的展开表，不必读栈帧链；RtlVirtualUnwind 正是
// 系统异常派发自己走的那套，在 VEH 里调是安全的。
int Unwind(const CONTEXT& src, uint64_t* out, int max) {
    CONTEXT ctx = src;
    int n = 0;
    bool leafHopUsed = false;
    while (n < max && ctx.Rip) {
        out[n++] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!rf) {
            // 没有 .pdata = 叶子函数。x64 调用约定下它的返回地址就在 [RSP]，跳过去接着走。
            // 06:16 那次实测：出错点 GameAssembly+0x3dfbd0 正是这种函数，少了这一跳整条栈
            // 就只剩一帧，等于白抓。只在栈顶兜这一次——再往深处猜就是在编故事了。
            if (leafHopUsed || !ctx.Rsp) break;
            leafHopUsed = true;
            uint64_t ret = 0;
            __try {
                ret = *reinterpret_cast<const uint64_t*>(ctx.Rsp);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (!ret) break;
            ctx.Rip = ret;
            ctx.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf, &ctx, &handlerData,
                         &establisher, nullptr);
        if (!ctx.Rip) break;
    }
    return n;
}

// 栈在外面先走完再进来占格子：万一展开自己踩空被外层 __try 兜住，也不会留下一个永远
// 停在「正在写」状态的死格子，把环形缓冲越用越小。
void Record(EXCEPTION_POINTERS* ep, bool owned, const x::runtime::il2cpp_metadata_lock::State& lk,
            uint64_t faultAt, const uint64_t* frames, int nframes) {
    const uint32_t i = gWriteSeq.fetch_add(1, std::memory_order_relaxed) % kRing;
    Rec& r = gRing[i];
    uint8_t expect = 0;
    if (!r.state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) {
        gDropped.fetch_add(1, std::memory_order_relaxed);  // flusher 还没跟上，丢这条
        return;
    }
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    r.code = er->ExceptionCode;
    r.tid = GetCurrentThreadId();
    r.faultAt = faultAt;
    r.op = er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0;
    r.target = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
    r.word = lk.word;
    r.recursion = lk.recursion;
    r.owned = owned ? 1 : 0;
    if (nframes > kFrames) nframes = kFrames;
    for (int k = 0; k < nframes; ++k) r.frames[k] = frames[k];
    r.nframes = static_cast<uint8_t>(nframes < 0 ? 0 : nframes);
    r.state.store(2, std::memory_order_release);
}

void HandlerImpl(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // 单步与断点是 kick_sniff 硬件断点的日常流量，直接放行，别在那条热路上加活。
    if (code == EXCEPTION_SINGLE_STEP || code == EXCEPTION_BREAKPOINT) return;
    if (!gGaEnd.load(std::memory_order_acquire)) return;  // 范围还没解析好

    const auto faultAt = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    const bool inGa = InRange(faultAt, gGaBase, gGaEnd);

    const auto lk = x::runtime::il2cpp_metadata_lock::Read();
    const bool owned = lk.read && lk.plausible && lk.ownerTid &&
                       lk.ownerTid == static_cast<uint64_t>(GetCurrentThreadId());

    if (!owned) {
        if (!inGa) return;  // 不是引擎内部炸的，与我们无关
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
            code != EXCEPTION_DATATYPE_MISALIGNMENT) {
            return;
        }
        if (gNoiseReported.fetch_add(1, std::memory_order_relaxed) >= kMaxNoiseReports) return;
        if (!NoiseBudgetOk(code, faultAt - gGaBase.load(std::memory_order_relaxed))) return;
        uint64_t frames[kFrames]{};
        const int n = Unwind(*ep->ContextRecord, frames, kFrames);
        Record(ep, false, lk, faultAt, frames, n);
        return;
    }

    // 持锁时抛的异常一律详记。曾经这里还要求「栈里有 xcat.dll」——本意是滤掉引擎自抛自接
    // 的托管异常，怕 LOADING 时刷屏。实测一整局只有 4 次，量根本不是问题，那道过滤反而把
    // 唯一的线索全挡了（06:11 那轮 详记 0 条）。宁可多记，也不能再漏。
    gOwnedSeen.fetch_add(1, std::memory_order_relaxed);
    // 挂号（两个原子写，VEH 里安全）。真泄漏了，后台线程一秒出头就会把锁抢修回来，
    // 不必再等主泵僵死 12 秒。
    x::runtime::il2cpp_metadata_lock::NoteExceptionWhileOwned(GetCurrentThreadId());
    if (gOwnedReported.fetch_add(1, std::memory_order_relaxed) >= kMaxOwnedReports) return;
    uint64_t frames[kFrames]{};
    const int n = Unwind(*ep->ContextRecord, frames, kFrames);
    Record(ep, true, lk, faultAt, frames, n);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    // 重入闩：Record / Unwind 若自己踩了空，别让处理函数递归下去把栈吃光。
    // 用 TlsAlloc 的裸槽而不是 thread_local——动态加载的 DLL 里 thread_local 走的是按需
    // TLS 初始化，第一次访问会分配内存。在 VEH 里分配正是上一版 abort 的同一类错误。
    const DWORD slot = gTlsGuard.load(std::memory_order_acquire);
    if (slot == TLS_OUT_OF_INDEXES) return EXCEPTION_CONTINUE_SEARCH;
    if (TlsGetValue(slot)) return EXCEPTION_CONTINUE_SEARCH;
    TlsSetValue(slot, reinterpret_cast<LPVOID>(1));
    __try {
        HandlerImpl(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 探针自己出问题，绝不能影响本来的异常派发。
    }
    TlsSetValue(slot, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;  // 只旁观，交回原本的处理流程
}

// ——— 以下都跑在后台线程，可以随便分配 / 加锁 / 落盘 ———

uintptr_t ImageSize(uintptr_t base) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void ResolveRanges() {
    if (!gGaEnd.load(std::memory_order_acquire)) {
        const uintptr_t base = il2::GaBase();
        if (base) {
            if (const uintptr_t size = ImageSize(base)) {
                // 先把锁地址的解析在这个安全上下文里跑热：Read() 首次调用会去解析
                // GameAssembly 的 PE 头，那种活不能落到 VEH 里。
                (void)x::runtime::il2cpp_metadata_lock::Read();
                gGaBase.store(base, std::memory_order_relaxed);
                gGaEnd.store(base + size, std::memory_order_release);
            }
        }
    }
}

// 只用后台线程做：会碰加载器。
int DescribeAddr(uint64_t a, char* out, size_t cap) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &mod) &&
        mod) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(mod, path, sizeof(path))) {
            const char* slash = strrchr(path, '\\');
            const char* name = slash ? slash + 1 : path;
            return snprintf(out, cap, "%s+0x%llx", name,
                            static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
        }
    }
    return snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(a));
}

void Emit(const Rec& r) {
    char stack[2560];  // 32 帧 × 「模块名+0x偏移 < 」，留够
    stack[0] = '\0';
    int used = 0;
    // snprintf 截断时返回的是「本该写多长」，直接累加会让 used 冲出缓冲区、
    // 下一轮 sizeof-used 反绕成天文数字。每步都夹回来。
    const int cap = static_cast<int>(sizeof(stack));
    for (int i = 0; i < r.nframes && used < cap - 80; ++i) {
        if (i) used += snprintf(stack + used, cap - used, " < ");
        if (used >= cap - 1) break;
        used += DescribeAddr(r.frames[i], stack + used, static_cast<size_t>(cap - used));
        if (used >= cap - 1) {
            used = cap - 1;
            break;
        }
    }
    stack[cap - 1] = '\0';

    char at[96];
    DescribeAddr(r.faultAt, at, sizeof(at));

    const char* what = r.code == EXCEPTION_ACCESS_VIOLATION
                           ? (r.op == 0 ? "读" : (r.op == 1 ? "写" : "执行"))
                           : "—";

    if (r.owned) {
        x::runtime::LogW("Il2cppFault",
                         "★持锁时抛异常★ code=0x%08lX %s 0x%llx 于 %s tid=%lu recursion=%u "
                         "—— 栈里有 xcat.dll，若外层 __except 吞掉它这把锁就永久泄漏、"
                         "全客户端黑屏。栈: %s",
                         r.code, what, static_cast<unsigned long long>(r.target), at, r.tid,
                         r.recursion, stack);
    } else {
        x::runtime::LogW("Il2cppFault",
                         "引擎内部异常 code=0x%08lX %s 0x%llx 于 %s tid=%lu "
                         "持锁=否(word=%u recursion=%u) 栈: %s",
                         r.code, what, static_cast<unsigned long long>(r.target), at, r.tid,
                         r.word, r.recursion, stack);
    }
}

DWORD WINAPI FlushThread(LPVOID) {
    uint32_t lastOwnedSeen = 0;
    uint32_t lastDropped = 0;
    DWORD lastStatMs = GetTickCount();
    while (!gStop.load(std::memory_order_acquire)) {
        ResolveRanges();
        // 宽限 1200 ms：正常的元数据查找是微秒级，异常处理完锁早该还回来了。
        x::runtime::il2cpp_metadata_lock::RepairAfterExceptionIfStillHeld(1200);
        for (auto& r : gRing) {
            if (r.state.load(std::memory_order_acquire) != 2) continue;
            Emit(r);
            r.state.store(0, std::memory_order_release);
        }
        const DWORD now = GetTickCount();
        if (now - lastStatMs >= 30000) {
            lastStatMs = now;
            const uint32_t seen = gOwnedSeen.load(std::memory_order_relaxed);
            const uint32_t drop = gDropped.load(std::memory_order_relaxed);
            if (seen != lastOwnedSeen || drop != lastDropped) {
                // 「持锁时抛异常」的真实频次。若这个数很大而详记的很少，说明绝大多数是
                // 引擎自抛自接的正常流量，只有带 xcat.dll 栈的那几条才是嫌疑。
                x::runtime::LogI("Il2cppFault", "统计：持锁时抛异常 %u 次（详记 %d 条），丢弃 %u 条",
                                 seen, gOwnedReported.load(std::memory_order_relaxed), drop);
                lastOwnedSeen = seen;
                lastDropped = drop;
            }
        }
        Sleep(300);
    }
    return 0;
}

}  // namespace

// 熄火开关：探针挂在全进程每一次异常上，是本仓风险最高的一块代码（06:01 那版就把客户端
// 送进了 abort）。真再出事时用户没法等我重新构建，放个标记文件就能把它整块停掉。
bool KillSwitchOn() {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return false;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\XCat_data\\state\\no_fault_probe", bin);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

void Start() {
    bool expected = false;
    if (!gRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (KillSwitchOn()) {
        gRunning.store(false, std::memory_order_release);
        x::runtime::LogI("Il2cppFault", "检测到 state\\no_fault_probe，探针已停用");
        return;
    }
    gStop.store(false, std::memory_order_release);
    const DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) {
        gRunning.store(false, std::memory_order_release);
        x::runtime::LogW("Il2cppFault", "TlsAlloc 失败——没有重入闩就不挂 VEH");
        return;
    }
    gTlsGuard.store(slot, std::memory_order_release);
    ResolveRanges();  // 先在安全上下文里解析一次，处理函数只读缓存
    gFlusher = CreateThread(nullptr, 0, &FlushThread, nullptr, 0, nullptr);
    gHandle = AddVectoredExceptionHandler(0, &Handler);
    if (!gHandle) {
        gRunning.store(false, std::memory_order_release);
        x::runtime::LogW("Il2cppFault", "VEH 注册失败——抓不到引擎内部异常");
        return;
    }
    x::runtime::LogI("Il2cppFault",
                     "已开始监视引擎内部异常：持锁时抛的最多详记 %d 条"
                     "（元数据锁泄漏的源头），其余噪声按签名去重、总额 %d 条",
                     kMaxOwnedReports, kMaxNoiseReports);
}

void Stop() {
    if (gHandle) {
        RemoveVectoredExceptionHandler(gHandle);
        gHandle = nullptr;
    }
    gStop.store(true, std::memory_order_release);
    if (gFlusher) {
        WaitForSingleObject(gFlusher, 1500);
        CloseHandle(gFlusher);
        gFlusher = nullptr;
    }
    gRunning.store(false, std::memory_order_release);
}

}  // namespace x::runtime::il2cpp_fault_probe
