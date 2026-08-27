// hit_pin_port — AbsHook FindHitMobInRect；hit_rotate 模式下滤命中列表到锁 oid。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "hit_pin_port.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::ports::hit_pin {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// dump.cs：d7f12f4cc34eb8c408869db0c4459a3e53597364a87582f89ee7301faa99859
// CMS：FindHitMobInRect(Rect, ref List<Mob>, maxCount, except, wishMobId, …)
constexpr uint32_t kRvaFindHitMobInRect = 0xF89C40;
constexpr size_t kFbMobId = 0x134;

// 与 melee_veto 近战/射击同一套 12 字节序言。
constexpr uint8_t kProlog[12] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41,
                                 0x55, 0x41, 0x54, 0x56, 0x57, 0x53};
constexpr size_t kSteal = 12;

// Win64 调用约定（近战 / Trapezoid / Summoned 多处 call site 实锤）：
//   rcx=this  rdx=Rect*  r8=List<Mob>**  r9=maxCount
//   [rsp+20]=except  +28=wishMobId  +30=poison  +38=wishTemplateId
//   +40=includeDazzled(byte)  +48=startIndex  +50=MethodInfo*
using FnFindHit = int32_t(__fastcall*)(void* self, void* rect, void* mobsRef, int32_t maxCount,
                                       void* except, int32_t wishMobId, int32_t poison,
                                       int32_t wishTemplateId, int32_t includeDazzled,
                                       int32_t startIndex, void* methodInfo);

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

std::atomic<bool> gWant{false};
std::atomic<bool> gAuxWant{false};
std::atomic<bool> gHooksWanted{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<bool> gRefuse{false};
std::atomic<int32_t> gWishOid{0};
std::atomic<BeforeFindHitFn> gBefore{nullptr};
std::atomic<AfterFindHitFn> gAfter{nullptr};
AbsHookState gHit{};
FnFindHit gTramp = nullptr;

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WritePtr(void* obj, size_t off, void* v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool SigMatch(void* target, const uint8_t* sig, size_t n) {
    if (!target) return false;
    for (size_t i = 0; i < n; ++i) {
        if (reinterpret_cast<uint8_t*>(target)[i] != sig[i]) return false;
    }
    return true;
}

bool InstallAbs(AbsHookState& st, void* target, void* hook) {
    if (st.active) return true;
    if (!target || !hook) return false;
    if (!SigMatch(target, kProlog, kSteal)) return false;
    void* tramp =
        VirtualAlloc(nullptr, kSteal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(st.saved, target, kSteal);
    memcpy(tramp, target, kSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kSteal,
                reinterpret_cast<uint8_t*>(target) + kSteal);
    DWORD old = 0;
    if (!VirtualProtect(target, kSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hook);
    FlushInstructionCache(GetCurrentProcess(), target, kSteal);
    VirtualProtect(target, kSteal, old, &old);
    st.target = target;
    st.trampoline = tramp;
    st.stolen = kSteal;
    st.active = true;
    return true;
}

void RemoveAbs(AbsHookState& st) {
    if (!st.active || !st.target) return;
    DWORD old = 0;
    if (VirtualProtect(st.target, st.stolen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(st.target, st.saved, st.stolen);
        FlushInstructionCache(GetCurrentProcess(), st.target, st.stolen);
        VirtualProtect(st.target, st.stolen, old, &old);
    }
    st.trampoline = nullptr;
    st.target = nullptr;
    st.stolen = 0;
    st.active = false;
}

bool EnsurePatchEnv() {
    char env[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", env, sizeof(env));
    if (n > 0 && env[0] == '1') return true;
    if (!SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", "1")) {
        x::runtime::LogW("HitPin", "无法设置 XCAT_ALLOW_TEXT_PATCH=1 err=%lu", GetLastError());
        return false;
    }
    return true;
}

void* ListFromRef(void* mobsRef) {
    if (!mobsRef) return nullptr;
    // TryDoingShootAttack 等 call site：lea r8, [rbp+local] = List**。
    // LooksLikeHeapPtr 对用户栈也是 true（0x10000..0x7FFE），绝不能把 r8 本身当 List*。
    void* p = ReadPtr(mobsRef, 0);
    if (LooksLikeHeapPtr(p)) return p;
    return nullptr;
}

// 返回：1=只留 wish；0=已清空（盒内无锁目标）；-1=列表解不出，原样交给官方返回值。
int32_t PinListToOid(void* mobsRef, int32_t wish) {
    void* list = ListFromRef(mobsRef);
    if (!LooksLikeHeapPtr(list)) return -1;
    x::runtime::il2cpp_container::Ensure();
    const int n = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    if (n < 0 || n > 64) return -1;
    if (n == 0) return 0;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    if (!LooksLikeHeapPtr(items)) return -1;
    const uintptr_t alen = x::runtime::il2cpp::ArrayLen(items);
    const int lim = n < static_cast<int>(alen) ? n : static_cast<int>(alen);
    void* keep = nullptr;
    int keepAt = -1;
    for (int i = 0; i < lim; ++i) {
        void* mob = x::runtime::il2cpp::ArrayAt(items, static_cast<uintptr_t>(i));
        if (!LooksLikeHeapPtr(mob)) continue;
        if (ReadI32(mob, kFbMobId) == wish) {
            keep = mob;
            keepAt = i;
            break;
        }
    }
    if (!keep) {
        WriteI32(list, x::runtime::il2cpp_container::OffListSize(), 0);
        return 0;
    }
    if (keepAt != 0 || n != 1) {
        WritePtr(items, x::runtime::il2cpp_container::OffArrayData(), keep);
        WriteI32(list, x::runtime::il2cpp_container::OffListSize(), 1);
    }
    return 1;
}

int32_t __fastcall HookFindHit(void* self, void* rect, void* mobsRef, int32_t maxCount,
                               void* except, int32_t wishMobId, int32_t poison,
                               int32_t wishTemplateId, int32_t includeDazzled, int32_t startIndex,
                               void* methodInfo) {
    const FnFindHit o = gTramp;
    if (!o) return 0;
    const int32_t wish = gWishOid.load(std::memory_order_relaxed);
    // 官方 wish 语义是「oid 匹配走优先分支」，不是「只打这一只」。BIN 里塞 wish
    // 之后 TryDoingShootAttack 拿到 eax=0 直接空包。盒扫描仍走原参数，钉锁只滤列表。
    int32_t mc = maxCount;
    const BeforeFindHitFn before = gBefore.load(std::memory_order_acquire);
    if (before) {
        __try {
            before(rect, &mc, startIndex);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x::runtime::LogW("HitPin", "before seh");
        }
    }
    int32_t n = 0;
    __try {
        n = o(self, rect, mobsRef, mc, except, wishMobId, poison, wishTemplateId,
              includeDazzled, startIndex, methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("HitPin", "findhit seh wish=%d", wish);
        return 0;
    }
    const AfterFindHitFn after = gAfter.load(std::memory_order_acquire);
    if (after) {
        __try {
            after(rect, mobsRef, n, mc, startIndex);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x::runtime::LogW("HitPin", "after seh");
        }
    }
    if (wish <= 0) return n;
    const int32_t kept = PinListToOid(mobsRef, wish);
    static DWORD sLog = 0;
    static int sFail = 0;
    const DWORD now = GetTickCount();
    const bool fail = kept <= 0;
    if ((fail && sFail < 12) || !sLog || now - sLog > 800) {
        if (fail) ++sFail;
        sLog = now;
        x::runtime::LogI("HitPin", "findhit wish=%d officialN=%d keep=%d", wish, n, kept);
    }
    return kept < 0 ? n : kept;
}

void PumpApply(void*) {
    const bool want = gHooksWanted.load(std::memory_order_acquire);
    if (!want) {
        if (gHit.active) {
            RemoveAbs(gHit);
            gTramp = nullptr;
            x::runtime::LogI("HitPin", "disarm");
        }
        return;
    }
    if (gHit.active || gRefuse.load(std::memory_order_relaxed)) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    void* target = x::runtime::il2cpp::AtRva<void*>(kRvaFindHitMobInRect);
    if (!SigMatch(target, kProlog, kSteal)) {
        gRefuse.store(true, std::memory_order_relaxed);
        x::runtime::LogW("HitPin", "refuse: prolog mismatch @%p rva=0x%X", target,
                         (unsigned)kRvaFindHitMobInRect);
        return;
    }
    if (!InstallAbs(gHit, target, reinterpret_cast<void*>(&HookFindHit))) {
        x::runtime::LogW("HitPin", "install failed @%p", target);
        return;
    }
    gTramp = reinterpret_cast<FnFindHit>(gHit.trampoline);
    x::runtime::LogI("HitPin", "arm=1 target=%p rva=0x%X", target,
                     (unsigned)kRvaFindHitMobInRect);
}

void RequestApply() {
    if (!x::runtime::main_thread::WaitUntilInstalled(0)) return;
    (void)x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 3000,
                                                x::runtime::main_thread::JobPrio::High);
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("HitPin", "worker start");
    while (!gStop.load(std::memory_order_acquire)) {
        const bool need = gHooksWanted.load(std::memory_order_acquire) && !gHit.active &&
                          !gRefuse.load(std::memory_order_relaxed);
        if (need) RequestApply();
        Sleep(need ? 400 : 2000);
    }
    x::runtime::LogI("HitPin", "worker exit");
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false, std::memory_order_release);
    gWant.store(false, std::memory_order_release);
    gAuxWant.store(false, std::memory_order_release);
    gHooksWanted.store(false, std::memory_order_release);
    gRefuse.store(false, std::memory_order_relaxed);
    gWishOid.store(0, std::memory_order_release);
    gBefore.store(nullptr, std::memory_order_release);
    gAfter.store(nullptr, std::memory_order_release);
}

void Shutdown() {
    StopWorker();
    gWant.store(false, std::memory_order_release);
    gAuxWant.store(false, std::memory_order_release);
    gBefore.store(nullptr, std::memory_order_release);
    gAfter.store(nullptr, std::memory_order_release);
    gHooksWanted.store(false, std::memory_order_release);
    gWishOid.store(0, std::memory_order_release);
    if (gHit.active) {
        if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
            x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 2000);
        else {
            RemoveAbs(gHit);
            gTramp = nullptr;
        }
    }
}

void StartWorker() {
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("HitPin", "CreateThread failed");
        return;
    }
    HANDLE prev = nullptr;
    if (!gWorker.compare_exchange_strong(prev, th, std::memory_order_acq_rel)) CloseHandle(th);
}

void StopWorker() {
    gStop.store(true, std::memory_order_release);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

void SetWanted(bool on) {
    if (on && !EnsurePatchEnv()) {
        gWant.store(false, std::memory_order_release);
        return;
    }
    const bool was = gWant.exchange(on, std::memory_order_acq_rel);
    if (!on) {
        gWishOid.store(0, std::memory_order_release);
        return;
    }
    gRefuse.store(false, std::memory_order_relaxed);
    gHooksWanted.store(true, std::memory_order_release);
    if (!was || !gHit.active) RequestApply();
}

void SetAuxWanted(bool on) {
    if (on && !EnsurePatchEnv()) {
        gAuxWant.store(false, std::memory_order_release);
        return;
    }
    const bool was = gAuxWant.exchange(on, std::memory_order_acq_rel);
    if (!on) return;
    gRefuse.store(false, std::memory_order_relaxed);
    gHooksWanted.store(true, std::memory_order_release);
    if (!was || !gHit.active) RequestApply();
}

void SetBeforeFindHit(BeforeFindHitFn fn) { gBefore.store(fn, std::memory_order_release); }

void SetAfterFindHit(AfterFindHitFn fn) { gAfter.store(fn, std::memory_order_release); }

bool IsArmed() { return gHit.active; }

void SetWishOid(int32_t oid) { gWishOid.store(oid > 0 ? oid : 0, std::memory_order_release); }

int32_t WishOid() { return gWishOid.load(std::memory_order_acquire); }

}  // namespace x::features::ports::hit_pin
