// Classic TWMS — il2cpp 全局元数据锁：读状态 + 泄漏归还。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "il2cpp_metadata_lock.h"

#include <windows.h>

#include <atomic>

#include "il2cpp_bind.h"
#include "log.h"

namespace x::runtime::il2cpp_metadata_lock {
namespace {

namespace il2 = x::runtime::il2cpp;

// il2cpp 的「类方法查找锁」。il2cpp_class_get_method_from_name 只是个 27 字节壳，
// 转调的内部实现手写了一把全局**递归互斥量**，futex 等待超时是 -1（永不返回）：
//
//     tid = Baselib_Thread_GetCurrentThreadId();
//     if (tid == owner) ++recursion;                 // 重入
//     else { CAS(word) ... while (v) Baselib_SystemFutex_Wait(word, 2, -1); owner = tid; }
//
// 解锁：if (recursion) --recursion; else { owner = 0; if (xchg(word,0)==2) notify; }
//
// 这套东西没有任何 SEH 清理路径。XCat 在 __except 里吞掉 il2cpp 内部的访问违例时，
// 展开会直接跨过上面的解锁代码，锁就永久挂在那个工作线程名下——之后全进程（含 Unity
// 主线程）每一次元数据查找都会挂死在 futex 上。2026-08-09 的黑屏卡死就是这么来的，
// 取证见 bin/XCat_data/logs/hang/hang_20260809_042012.txt：
// word=2 recursion=0 owner=tid 36680，而 36680 正躺在自己 worker 循环的 SleepEx 上。
//
// RVA 取自 Dumps/runtime/GameAssembly.dll.i64（imagebase 0x7ff848c80000）：
//   word      0x7FF84FB8D910 → 0x6F0D910
//   owner     0x7FF84FB8D950 → 0x6F0D950
//   recursion 0x7FF84FB8D958 → 0x6F0D958
// 客户端一更新这三个 RVA 就会漂。所以每次读都做合理性校验（锁字只能是 0/1/2、
// 持有者必须落在 32 位 tid 范围内），不通过就当作「读不到」，绝不据此改写内存。
constexpr uintptr_t kWordRva = 0x6F0D910;
constexpr uintptr_t kOwnerRva = 0x6F0D950;
constexpr uintptr_t kRecursionRva = 0x6F0D958;

struct Addrs {
    volatile LONG* word = nullptr;
    volatile uint64_t* owner = nullptr;
    volatile uint32_t* recursion = nullptr;
};

// GameAssembly 的 SizeOfImage，用来卡住「RVA 漂到模块外」。
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

// 结果缓存：il2cpp_fault_probe 会在**每一次首次异常**上问锁状态，那条路径不能每次
// 都去解析一遍 PE 头。GameAssembly 一旦装载就不会再动，算一次就够。
std::atomic<bool> gResolved{false};
Addrs gCached;

// 定点抢修的挂号簿：谁在什么时候「持着锁抛了异常」。
std::atomic<uint32_t> gSuspectTid{0};
std::atomic<DWORD> gSuspectAtMs{0};

bool Resolve(Addrs& out) {
    if (gResolved.load(std::memory_order_acquire)) {
        out = gCached;
        return true;
    }
    const uintptr_t base = il2::GaBase();
    if (!base) return false;
    const uintptr_t size = ImageSize(base);
    if (!size || kRecursionRva + sizeof(uint32_t) > size) return false;
    out.word = reinterpret_cast<volatile LONG*>(base + kWordRva);
    out.owner = reinterpret_cast<volatile uint64_t*>(base + kOwnerRva);
    out.recursion = reinterpret_cast<volatile uint32_t*>(base + kRecursionRva);
    gCached = out;  // 各线程算出来的值必然一致，抢着写没有危害
    gResolved.store(true, std::memory_order_release);
    return true;
}

using WakeByAddressAllFn = void(WINAPI*)(PVOID);

WakeByAddressAllFn WakeAll() {
    // 走 GetProcAddress 而不是链 Synchronization.lib：少一个静态依赖，拿不到就退化成
    // 「只归还不唤醒」——等待者仍会在下一次超时/新的 Notify 时被带出来，总好过硬依赖。
    static WakeByAddressAllFn fn = [] {
        HMODULE h = GetModuleHandleW(L"KernelBase.dll");
        return h ? reinterpret_cast<WakeByAddressAllFn>(GetProcAddress(h, "WakeByAddressAll"))
                 : nullptr;
    }();
    return fn;
}

bool ReadRaw(const Addrs& a, uint32_t& word, uint64_t& owner, uint32_t& recursion) {
    __try {
        word = static_cast<uint32_t>(*a.word);
        owner = *a.owner;
        recursion = *a.recursion;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Plausible(uint32_t word, uint64_t owner) {
    return word <= 2 && owner <= 0xFFFFFFFFull;
}

// 归还顺序必须和 il2cpp 自己的解锁完全一致：先清持有者，再放锁字，最后唤醒等待者。
bool DoRelease(const Addrs& a) {
    LONG prev = 0;
    __try {
        *a.recursion = 0;
        *a.owner = 0;
        prev = InterlockedExchange(a.word, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (prev == 2) {
        // baselib 的 futex 在 Windows 上就是 WaitOnAddress（卡死现场主线程停在
        // KERNELBASE!WaitOnAddress+0x2f 已核对），所以 WakeByAddressAll 能把它们带出来。
        if (WakeByAddressAllFn wake = WakeAll()) wake(const_cast<LONG*>(a.word));
    }
    return true;
}

}  // namespace

State Read() {
    State s;
    Addrs a;
    if (!Resolve(a)) return s;
    if (!ReadRaw(a, s.word, s.ownerTid, s.recursion)) return s;
    s.read = true;
    s.plausible = Plausible(s.word, s.ownerTid);
    return s;
}

bool ReleaseIfOwnedByCurrentThread(const char* where) {
    Addrs a;
    if (!Resolve(a)) return false;

    uint32_t word = 0;
    uint64_t owner = 0;
    uint32_t recursion = 0;
    if (!ReadRaw(a, word, owner, recursion)) return false;
    if (!Plausible(word, owner)) return false;
    if (owner != static_cast<uint64_t>(GetCurrentThreadId())) return false;

    // 走到这里 = 我们刚从 il2cpp 内部被异常弹出来，而锁还记在自己头上。
    if (!DoRelease(a)) return false;
    LogW("Il2cppLock",
         "归还被泄漏的元数据锁 where=%s tid=%lu 归还前 word=%u recursion=%u —— "
         "这次 il2cpp 内部抛了异常，若不归还全进程元数据查找都会永久挂死",
         where ? where : "?", GetCurrentThreadId(), word, recursion);
    return true;
}

bool ForceReleaseIfOwnedBy(uint32_t ownerTid, const char* why) {
    if (!ownerTid) return false;
    Addrs a;
    if (!Resolve(a)) return false;

    uint32_t word = 0;
    uint64_t owner = 0;
    uint32_t recursion = 0;
    if (!ReadRaw(a, word, owner, recursion)) return false;
    if (!Plausible(word, owner)) return false;
    if (!word) return false;  // 已经空闲，没什么可抢救的
    if (owner != static_cast<uint64_t>(ownerTid)) return false;

    if (!DoRelease(a)) return false;
    LogW("Il2cppLock",
         "强行归还卡死的元数据锁 why=%s 原持有者 tid=%lu word=%u recursion=%u —— "
         "该线程长时间持锁不放（泄漏），继续等下去全客户端永久黑屏",
         why ? why : "?", static_cast<unsigned long>(ownerTid), word, recursion);
    return true;
}

void NoteExceptionWhileOwned(uint32_t tid) {
    if (!tid) return;
    // 只留最后一个嫌疑人就够：泄漏一旦发生，锁就再没人能拿到，不会有第二个持有者。
    gSuspectAtMs.store(GetTickCount(), std::memory_order_relaxed);
    gSuspectTid.store(tid, std::memory_order_release);
}

bool RepairAfterExceptionIfStillHeld(uint32_t graceMs) {
    const uint32_t tid = gSuspectTid.load(std::memory_order_acquire);
    if (!tid) return false;
    const DWORD at = gSuspectAtMs.load(std::memory_order_relaxed);
    if (static_cast<int>(GetTickCount() - at) < static_cast<int>(graceMs)) return false;

    Addrs a;
    if (!Resolve(a)) return false;
    uint32_t word = 0;
    uint64_t owner = 0;
    uint32_t recursion = 0;
    if (!ReadRaw(a, word, owner, recursion) || !Plausible(word, owner)) {
        gSuspectTid.store(0, std::memory_order_release);
        return false;
    }
    if (!word || owner != static_cast<uint64_t>(tid)) {
        gSuspectTid.store(0, std::memory_order_release);  // 已经正常还回去了，销案
        return false;
    }

    gSuspectTid.store(0, std::memory_order_release);
    if (!DoRelease(a)) return false;
    LogW("Il2cppLock",
         "定点抢修：tid=%lu 在 %u ms 前持锁时抛了异常，到现在锁仍挂在它名下 "
         "(word=%u recursion=%u) —— 判定为 __except 吞异常导致的泄漏，已归还并唤醒等待者",
         static_cast<unsigned long>(tid), graceMs, word, recursion);
    return true;
}

}  // namespace x::runtime::il2cpp_metadata_lock
