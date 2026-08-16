// Classic TWMS — F6 武装期永久禁挂台（CollisionDetect / CollisionDetectFloat）。
//
// BIN 证据：持续硬卸 CurFh + Impact → 205；根因是引擎下一帧 Float 重挂与上报不一致。
// 现改为在挂台虚函数入口拦截（LocalUser 限定），武装期不重挂。
//
// 锚点（GameAssembly IDB imagebase 0x7FF848C80000）：
//   VecCtrl_WorkUpdateActive @ RVA 0x11C52B0
//   有台：call [klass+0x208]  CollisionDetect     (r9=0)
//   无台：call [klass+0x218]  CollisionDetectFloat (r9=1)
// 安装：改 klass 上对应 methodPtr（VirtualProtect）。
// MemoryCrc.RpmScan 灭火默认关（禁台只改虚表，非 GA .text）；应急：XCAT_FH_BAN_CRC=1。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fly_fh_ban.h"

#include "ground_spoof.h"
#include "player_combat_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::ports::fly_fh_ban {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// 与 teleport_port / foothold_port 同源 fb（Ensure 时再读一次 LocalUser）。
constexpr size_t kFbUserVecCtrl = 0x50;
constexpr size_t kFbVcCurFh = 0x28;
constexpr size_t kFbVcLastFh = 0x30;
constexpr size_t kFbVcLadderOrRope = 0x40;

// Il2CppClass 内 VirtualInvokeData 绝对偏移（WorkUpdateActive 实锤）。
constexpr size_t kKlassOffCdMethodPtr = 0x208;
constexpr size_t kKlassOffCdfMethodPtr = 0x218;

// grap-core MemoryCrc.RpmScan — 与 ga_text_probe 同 RVA。默认不碰；仅 XCAT_FH_BAN_CRC=1 时 early-ret。
constexpr uint32_t kRvaMemoryCrcRpmScan = 0x102D610;
constexpr uint8_t kRpmScanPrologueExpect[] = {0x41, 0x57, 0x41, 0x56};
constexpr uint8_t kEarlyRet[] = {0x33, 0xC0, 0xC3};  // xor eax,eax ; ret

bool EnvCrcExtinguishOn() {
    char buf[16]{};
    return GetEnvironmentVariableA("XCAT_FH_BAN_CRC", buf, sizeof(buf)) > 0 && buf[0] == '1';
}

// (this, a2, a3, flag) → bool；MI 由调用方放栈，不进寄存器。
using FnDetect = uint8_t(__fastcall*)(void* self, void* a2, void* a3, uint8_t flag);

std::atomic<bool> gBan{false};
std::atomic<unsigned> gBanMask{0};  // BanSource 位或；非 0 ≡ BAN ON
std::atomic<bool> gInstalled{false};
std::atomic<void*> gLocalVc{nullptr};

void* gKlass = nullptr;
void** gSlotCd = nullptr;
void** gSlotCdf = nullptr;
FnDetect gOrigCd = nullptr;
FnDetect gOrigCdf = nullptr;
bool gCrcExtinguished = false;

void WritePtrSeh(void* base, size_t off, void* val) {
    if (!base) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off) = val;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* ReadPtrSeh(void* base, size_t off) {
    if (!base) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool PatchSlot(void** slot, void* hook, FnDetect* outOrig) {
    if (!slot || !hook || !outOrig) return false;
    void* cur = nullptr;
    __try {
        cur = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!cur) return false;
    if (*outOrig && reinterpret_cast<void*>(*outOrig) != cur && cur != hook) {
        // 已被别人改过且不是我们的钩——拒装。
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        if (!*outOrig) *outOrig = reinterpret_cast<FnDetect>(cur);
        *slot = hook;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return ok;
}

void RestoreSlot(void** slot, FnDetect orig) {
    if (!slot || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    __try {
        if (*slot != reinterpret_cast<void*>(orig)) *slot = reinterpret_cast<void*>(orig);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

void ClearFhOnVc(void* vc) {
    if (!LooksLikeHeapPtr(vc)) return;
    // 清掉之前先把这块台的 ID 留给「站立伪装」——起飞那一帧走的就是这里，
    // 之后 CurFh 恒为空，再没别的地方能拿到一块本图的合法台。
    ground_spoof::NoticeFhObject(ReadPtrSeh(vc, kFbVcCurFh));
    WritePtrSeh(vc, kFbVcCurFh, nullptr);
    WritePtrSeh(vc, kFbVcLastFh, nullptr);
    WritePtrSeh(vc, kFbVcLadderOrRope, nullptr);
}

bool IsLocalVc(void* self) {
    if (!self) return false;
    void* cached = gLocalVc.load(std::memory_order_acquire);
    if (cached && cached == self) return true;
    // 缓存未命中：轻量再解析（物理帧热路径；失败则不当本地）。
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtrSeh(lu, kFbUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    gLocalVc.store(vc, std::memory_order_release);
    return vc == self;
}

void ExtinguishMemoryCrcIfPresent() {
    if (gCrcExtinguished) return;
    HMODULE grap = GetModuleHandleW(L"grap-core64.aes");
    if (!grap) grap = GetModuleHandleW(L"grap-core64.dll");
    if (!grap) {
        // 枚举一次（与 ga_text_probe 同策略，缩略）
        HMODULE mods[256]{};
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            const DWORD n = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < n && i < 256; ++i) {
                wchar_t path[MAX_PATH]{};
                if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
                const wchar_t* leaf = wcsrchr(path, L'\\');
                leaf = leaf ? leaf + 1 : path;
                if (_wcsicmp(leaf, L"grap-core64.aes") == 0 ||
                    _wcsicmp(leaf, L"grap-core64.dll") == 0) {
                    grap = mods[i];
                    break;
                }
            }
        }
    }
    if (!grap) return;
    auto* p = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(grap) + kRvaMemoryCrcRpmScan);
    __try {
        if (memcmp(p, kRpmScanPrologueExpect, sizeof(kRpmScanPrologueExpect)) != 0) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(kEarlyRet), PAGE_EXECUTE_READWRITE, &old)) return;
    __try {
        memcpy(p, kEarlyRet, sizeof(kEarlyRet));
        FlushInstructionCache(GetCurrentProcess(), p, sizeof(kEarlyRet));
        gCrcExtinguished = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(p, sizeof(kEarlyRet), old, &old);
    if (gCrcExtinguished) {
        x::runtime::LogI("FlyFhBan", "MemoryCrc.RpmScan early-ret @grap+0x%X", kRvaMemoryCrcRpmScan);
    }
}

uint8_t __fastcall HookCollisionDetect(void* self, void* a2, void* a3, uint8_t flag) {
    if (gBan.load(std::memory_order_acquire) && IsLocalVc(self)) {
        ClearFhOnVc(self);
        return 0;
    }
    if (!gOrigCd) return 0;
    return gOrigCd(self, a2, a3, flag);
}

uint8_t __fastcall HookCollisionDetectFloat(void* self, void* a2, void* a3, uint8_t flag) {
    if (gBan.load(std::memory_order_acquire) && IsLocalVc(self)) {
        // 不调用原函数 → 不落台；顺手清残附着。
        ClearFhOnVc(self);
        return 0;
    }
    if (!gOrigCdf) return 0;
    return gOrigCdf(self, a2, a3, flag);
}

struct InstallJob {
    bool ok = false;
    char why[48]{};
};

void InstallJobFn(void* p) {
    auto* job = static_cast<InstallJob*>(p);
    if (!job) return;
    if (gInstalled.load(std::memory_order_acquire)) {
        job->ok = true;
        strncpy_s(job->why, "already", _TRUNCATE);
        return;
    }
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
        strncpy_s(job->why, "no_lu", _TRUNCATE);
        return;
    }
    void* vc = ReadPtrSeh(lu, kFbUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) {
        strncpy_s(job->why, "no_vc", _TRUNCATE);
        return;
    }
    void* klass = ReadPtrSeh(vc, 0);
    if (!klass) {
        strncpy_s(job->why, "no_klass", _TRUNCATE);
        return;
    }
    void** slotCd = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + kKlassOffCdMethodPtr);
    void** slotCdf = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + kKlassOffCdfMethodPtr);
    void* pCd = nullptr;
    void* pCdf = nullptr;
    __try {
        pCd = *slotCd;
        pCdf = *slotCdf;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy_s(job->why, "slot_seh", _TRUNCATE);
        return;
    }
    if (!pCd || !pCdf) {
        strncpy_s(job->why, "null_mp", _TRUNCATE);
        return;
    }
    if (EnvCrcExtinguishOn()) {
        ExtinguishMemoryCrcIfPresent();
    } else {
        x::runtime::LogI("FlyFhBan", "skip MemoryCrc extinguish (set XCAT_FH_BAN_CRC=1 to enable)");
    }
    if (!PatchSlot(slotCd, reinterpret_cast<void*>(&HookCollisionDetect), &gOrigCd)) {
        strncpy_s(job->why, "patch_cd", _TRUNCATE);
        return;
    }
    if (!PatchSlot(slotCdf, reinterpret_cast<void*>(&HookCollisionDetectFloat), &gOrigCdf)) {
        RestoreSlot(slotCd, gOrigCd);
        gOrigCd = nullptr;
        strncpy_s(job->why, "patch_cdf", _TRUNCATE);
        return;
    }
    gKlass = klass;
    gSlotCd = slotCd;
    gSlotCdf = slotCdf;
    gLocalVc.store(vc, std::memory_order_release);
    gInstalled.store(true, std::memory_order_release);
    job->ok = true;
    strncpy_s(job->why, "ok", _TRUNCATE);
    x::runtime::LogI("FlyFhBan",
                     "installed klass=%p cd=%p cdf=%p crc=%d (armed-ban ready)", klass, pCd, pCdf,
                     gCrcExtinguished ? 1 : 0);
}

bool EnsureInstalled() {
    if (gInstalled.load(std::memory_order_acquire)) return true;
    if (!runtime::main_thread::Ensure()) return false;
    InstallJob job{};
    if (!runtime::main_thread::InvokeAndWait(&InstallJobFn, &job, 2000,
                                            runtime::main_thread::JobPrio::High)) {
        x::runtime::LogW("FlyFhBan", "install invoke timeout");
        return false;
    }
    if (!job.ok) {
        x::runtime::LogW("FlyFhBan", "install fail why=%s", job.why);
        return false;
    }
    return true;
}

struct DetachJob {
    bool ok = false;
};

void DetachJobFn(void* p) {
    auto* job = static_cast<DetachJob*>(p);
    if (!job) return;
    void* lu = nullptr;
    if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return;
    void* vc = ReadPtrSeh(lu, kFbUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return;
    ClearFhOnVc(vc);
    gLocalVc.store(vc, std::memory_order_release);
    job->ok = true;
}

}  // namespace

void SetSourceArmed(BanSource source, bool on) {
    const unsigned bit = static_cast<unsigned>(source);
    if (!bit) return;
    unsigned prev = gBanMask.load(std::memory_order_acquire);
    for (;;) {
        const unsigned next = on ? (prev | bit) : (prev & ~bit);
        if (prev == next) return;
        if (!gBanMask.compare_exchange_weak(prev, next, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            continue;
        }
        if (next != 0) {
            if (!EnsureInstalled()) {
                gBanMask.store(0, std::memory_order_release);
                gBan.store(false, std::memory_order_release);
                x::runtime::LogW("FlyFhBan", "source arm install fail bit=0x%x", bit);
                return;
            }
            gBan.store(true, std::memory_order_release);
            // 仅 0→开 卸台；追加 source 不重复 detach。
            if (prev == 0 && runtime::main_thread::Ensure()) {
                DetachJob job{};
                (void)runtime::main_thread::InvokeAndWait(&DetachJobFn, &job, 500,
                                                         runtime::main_thread::JobPrio::High);
                x::runtime::LogI("FlyFhBan", "BAN ON mask=0x%x bit=0x%x detach=%d", next, bit,
                                 job.ok ? 1 : 0);
            } else if (prev == 0) {
                x::runtime::LogI("FlyFhBan", "BAN ON mask=0x%x bit=0x%x (no detach pump)", next,
                                 bit);
            }
        } else {
            gBan.store(false, std::memory_order_release);
            x::runtime::LogI("FlyFhBan", "BAN OFF (last source bit=0x%x)", bit);
        }
        return;
    }
}

void SetArmedBan(bool armed) { SetSourceArmed(BanSource::Fly, armed); }

bool WarmInstall() {
    // 与 SetSourceArmed 共用 EnsureInstalled：只改虚表，不碰 gBanMask / detach。
    return EnsureInstalled();
}

bool IsBanActive() { return gBan.load(std::memory_order_acquire); }
bool IsInstalled() { return gInstalled.load(std::memory_order_acquire); }
unsigned ActiveMask() { return gBanMask.load(std::memory_order_acquire); }

void Shutdown() {
    gBanMask.store(0, std::memory_order_release);
    gBan.store(false, std::memory_order_release);
    if (!gInstalled.load(std::memory_order_acquire)) return;
    if (gSlotCd) RestoreSlot(gSlotCd, gOrigCd);
    if (gSlotCdf) RestoreSlot(gSlotCdf, gOrigCdf);
    gSlotCd = nullptr;
    gSlotCdf = nullptr;
    gOrigCd = nullptr;
    gOrigCdf = nullptr;
    gKlass = nullptr;
    gLocalVc.store(nullptr, std::memory_order_release);
    gInstalled.store(false, std::memory_order_release);
    x::runtime::LogI("FlyFhBan", "shutdown restored slots");
}

}  // namespace x::features::ports::fly_fh_ban
