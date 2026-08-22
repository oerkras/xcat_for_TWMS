// Classic TWMS / 经典版 — 怪 prevpos 举报内联分支旁路（详见 mob_prevpos_patch.h）。
// 照 curfh_gate_bypass 同款 .text 内联补丁模板：GaBase+RVA、expect 字节守卫、幂等、可回滚。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_prevpos_patch.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::ports::mob_prevpos_patch {
namespace {

// 分支决策 @ RVA 0xF5D0DB（内联在 Mob CFF 大函数 sub_7FFD61789D50 里）：
//   cmovz r15, rax  (4C 0F 44 F8)  ->  mov r15, rax; nop  (4C 8B F8 90)
// rax 在前一条 lea 恒为 &正常移动包路径 → 无条件 mov 后每帧每怪永走正常包。
constexpr uint32_t kRvaBranch = 0xF5D0DB;
constexpr uint8_t kExpect[] = {0x4C, 0x0F, 0x44, 0xF8};
constexpr uint8_t kPatch[] = {0x4C, 0x8B, 0xF8, 0x90};

std::atomic<bool> gWant{false};
std::atomic<bool> gInstalled{false};

uint8_t gBak[sizeof(kPatch)]{};
bool gHaveBak = false;

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

bool TryInstall() {
    if (!x::runtime::il2cpp::GaBase()) return false;
    auto* p = x::runtime::il2cpp::AtRva<uint8_t*>(kRvaBranch);
    if (!p) {
        x::runtime::LogW("MobPrevPosPatch", "GA base 0");
        return false;
    }
    // 已是补丁态：幂等成功。
    if (BytesEq(p, kPatch, sizeof(kPatch))) {
        gHaveBak = true;
        gInstalled.store(true, std::memory_order_release);
        return true;
    }
    if (!BytesEq(p, kExpect, sizeof(kExpect))) {
        x::runtime::LogW("MobPrevPosPatch", "unexpected bytes @ga+0x%X — skip", kRvaBranch);
        return false;
    }
    if (!ProtectWrite(p, sizeof(kPatch), kPatch, gBak)) {
        x::runtime::LogW("MobPrevPosPatch", "VirtualProtect/write fail @ga+0x%X", kRvaBranch);
        return false;
    }
    gHaveBak = true;
    gInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("MobPrevPosPatch", "patched ga+0x%X (cmovz r15->mov r15; forces normal move packet)",
                     kRvaBranch);
    return true;
}

void Uninstall() {
    auto* p = x::runtime::il2cpp::AtRva<uint8_t*>(kRvaBranch);
    // 即便本进程没装过：若 GA 仍是我们的补丁字节（上次卸载失败残留），也写回原指令。
    if (p && BytesEq(p, kPatch, sizeof(kPatch))) {
        const uint8_t* src = gHaveBak ? gBak : kExpect;
        if (!ProtectWrite(p, sizeof(kExpect), src, nullptr))
            x::runtime::LogW("MobPrevPosPatch", "restore fail @ga+0x%X", kRvaBranch);
        else
            x::runtime::LogI("MobPrevPosPatch", "restored ga+0x%X", kRvaBranch);
    } else if (gInstalled.load(std::memory_order_acquire) && p) {
        x::runtime::LogW("MobPrevPosPatch", "skip restore (bytes changed) @ga+0x%X", kRvaBranch);
    }
    gHaveBak = false;
    gInstalled.store(false, std::memory_order_release);
}

}  // namespace

void SetEnabled(bool on) {
    // 2026-08-22 硬关：BIN 证伪后永不装补丁。原安装路径留着，kKillSwitched=false 即可回滚。
    constexpr bool kKillSwitched = true;
    if (kKillSwitched) {
        if (on) {
            x::runtime::LogWThrottled(
                1, 30000, "MobPrevPosPatch",
                "kill-switched — refuse install (BIN disproved; see docs §7.12)");
        }
        gWant.store(false, std::memory_order_release);
        Uninstall();
        return;
    }
    gWant.store(on, std::memory_order_release);
    if (!on) {
        Uninstall();
        return;
    }
    if (gInstalled.load(std::memory_order_acquire)) return;
    if (!TryInstall()) {
        x::runtime::LogW("MobPrevPosPatch",
                         "install deferred (GA not ready or bytes mismatch); retry on next apply");
    }
}

bool IsEnabled() { return gWant.load(std::memory_order_acquire); }

bool IsInstalled() { return gInstalled.load(std::memory_order_acquire); }

}  // namespace x::features::ports::mob_prevpos_patch
