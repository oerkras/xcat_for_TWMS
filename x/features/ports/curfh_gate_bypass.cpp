// Classic TWMS — CurFh 地面门 .text 旁路（详见 curfh_gate_bypass.h）。
// 故意不碰 grap MemoryCrc.RpmScan：脏 GA 可能被扫到，但绝不主动拆检测器。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "curfh_gate_bypass.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::ports::curfh_gate_bypass {
namespace {

// remounted 2026-08-06 GA；与 dump.cs.restored / IDA 实锤一致。
constexpr uint32_t kRvaMagicJnz = 0x1091E42;      // 75 07 → EB 07
constexpr uint32_t kRvaShootJnz = 0x10599F0;      // 75 07 → EB 07
constexpr uint32_t kRvaPrepareSetnz = 0x10B353B;  // 0F 95 C2 → B2 01 90

constexpr uint8_t kJnzExpect[] = {0x75, 0x07};
constexpr uint8_t kJmpPatch[] = {0xEB, 0x07};
constexpr uint8_t kSetnzExpect[] = {0x0F, 0x95, 0xC2};
constexpr uint8_t kSetnzPatch[] = {0xB2, 0x01, 0x90};  // mov dl,1 ; nop

std::atomic<bool> gWant{false};
std::atomic<bool> gInstalled{false};

uint8_t gBakMagic[sizeof(kJmpPatch)]{};
uint8_t gBakShoot[sizeof(kJmpPatch)]{};
uint8_t gBakPrepare[sizeof(kSetnzPatch)]{};
bool gHaveBakMagic = false;
bool gHaveBakShoot = false;
bool gHaveBakPrepare = false;

bool ProtectWrite(void* addr, size_t n, const uint8_t* src, uint8_t* backup) {
    if (!addr || !src || n == 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        if (backup) memcpy(backup, addr, n);
        memcpy(addr, src, n);
        FlushInstructionCache(GetCurrentProcess(), addr, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(addr, n, old, &old);
    return ok;
}

bool BytesEq(const void* p, const uint8_t* expect, size_t n) {
    __try {
        return memcmp(p, expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PatchOne(uint32_t rva, const uint8_t* expect, const uint8_t* patch, size_t n,
              uint8_t* bak, bool* haveBak, const char* tag) {
    auto* p = x::runtime::il2cpp::AtRva<uint8_t*>(rva);
    if (!p) {
        x::runtime::LogW("CurFhGateBypass", "%s: GA base 0", tag);
        return false;
    }
    // 已是补丁态：幂等成功。
    if (BytesEq(p, patch, n)) {
        if (haveBak) *haveBak = true;
        return true;
    }
    if (!BytesEq(p, expect, n)) {
        x::runtime::LogW("CurFhGateBypass", "%s: unexpected bytes @ga+0x%X", tag, rva);
        return false;
    }
    if (!ProtectWrite(p, n, patch, bak)) {
        x::runtime::LogW("CurFhGateBypass", "%s: VirtualProtect/write fail @ga+0x%X", tag, rva);
        return false;
    }
    if (haveBak) *haveBak = true;
    x::runtime::LogI("CurFhGateBypass", "%s: patched ga+0x%X", tag, rva);
    return true;
}

bool RestoreOne(uint32_t rva, const uint8_t* patch, const uint8_t* bak, size_t n, bool haveBak,
                const char* tag) {
    if (!haveBak) return true;
    auto* p = x::runtime::il2cpp::AtRva<uint8_t*>(rva);
    if (!p) return false;
    // 仍是我们的补丁才还原；别人改过则不动。
    if (!BytesEq(p, patch, n)) {
        x::runtime::LogW("CurFhGateBypass", "%s: skip restore (bytes changed) @ga+0x%X", tag, rva);
        return true;
    }
    if (!ProtectWrite(p, n, bak, nullptr)) {
        x::runtime::LogW("CurFhGateBypass", "%s: restore fail @ga+0x%X", tag, rva);
        return false;
    }
    x::runtime::LogI("CurFhGateBypass", "%s: restored ga+0x%X", tag, rva);
    return true;
}

void Uninstall() {
    if (!gInstalled.load(std::memory_order_acquire) && !gHaveBakMagic && !gHaveBakShoot &&
        !gHaveBakPrepare) {
        return;
    }
    (void)RestoreOne(kRvaMagicJnz, kJmpPatch, gBakMagic, sizeof(kJmpPatch), gHaveBakMagic,
                     "Magic");
    (void)RestoreOne(kRvaShootJnz, kJmpPatch, gBakShoot, sizeof(kJmpPatch), gHaveBakShoot,
                     "Shoot");
    (void)RestoreOne(kRvaPrepareSetnz, kSetnzPatch, gBakPrepare, sizeof(kSetnzPatch),
                     gHaveBakPrepare, "Prepare");
    gHaveBakMagic = gHaveBakShoot = gHaveBakPrepare = false;
    gInstalled.store(false, std::memory_order_release);
    x::runtime::LogI("CurFhGateBypass", "uninstalled");
}

bool TryInstall() {
    if (!x::runtime::il2cpp::GaBase()) return false;
    const bool m =
        PatchOne(kRvaMagicJnz, kJnzExpect, kJmpPatch, sizeof(kJmpPatch), gBakMagic,
                 &gHaveBakMagic, "Magic");
    const bool s =
        PatchOne(kRvaShootJnz, kJnzExpect, kJmpPatch, sizeof(kJmpPatch), gBakShoot,
                 &gHaveBakShoot, "Shoot");
    const bool p =
        PatchOne(kRvaPrepareSetnz, kSetnzExpect, kSetnzPatch, sizeof(kSetnzPatch), gBakPrepare,
                 &gHaveBakPrepare, "Prepare");
    const bool ok = m && s && p;
    gInstalled.store(ok, std::memory_order_release);
    if (ok) {
        x::runtime::LogI("CurFhGateBypass",
                         "installed Magic+Shoot+Prepare (.text; no RpmScan extinguish)");
    } else {
        x::runtime::LogW("CurFhGateBypass", "partial/fail m=%d s=%d p=%d — rolling back", m ? 1 : 0,
                         s ? 1 : 0, p ? 1 : 0);
        Uninstall();
    }
    return ok;
}

}  // namespace

void SetEnabled(bool on) {
    gWant.store(on, std::memory_order_release);
    if (!on) {
        Uninstall();
        return;
    }
    if (gInstalled.load(std::memory_order_acquire)) return;
    if (!TryInstall()) {
        x::runtime::LogW("CurFhGateBypass",
                         "install deferred (GA not ready or bytes mismatch); will retry on next apply");
    }
}

bool IsEnabled() { return gWant.load(std::memory_order_acquire); }

bool IsInstalled() { return gInstalled.load(std::memory_order_acquire); }

}  // namespace x::features::ports::curfh_gate_bypass
