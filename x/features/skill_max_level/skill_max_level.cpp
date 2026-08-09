// TWMS Classic — skill_max_level
// A: SkillRecord 等级→满级；B: Hook UserLocal + SkillInfo GetSkillLevel/GetPure。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "skill_max_level.h"

#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"
#include "xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace x::features::skill_max_level {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr size_t kFbLevelDataList = 0x120;
// remounted 2026-08-06（与 skill_port 钉值一致；GetMaxLevel RVA 仍待 BIN 确认）
constexpr uint32_t kRvaGetMaxLevel = 0x1563BD0;
constexpr uint32_t kRvaUlGetSkillLevel = 0x106D470;
// SkillInfo.GetPureSkillLevel / GetSkillLevel(ref CD, id, ref SE)
// 注意：同邻域 GetShootSkillRange(ref CD, skillId, weaponType) 返回射程——禁止当学级钩。
// Pure 与 Level 一并抬满：本功能要客户端「已学即满级」；若需保留纯加点真值再拆。
constexpr uint32_t kRvaSiGetPureSkillLevel = 0x1579560;
constexpr uint32_t kRvaSiGetSkillLevel = 0x1579820;
constexpr char kHashGetMaxLevel[] =
    "f353c9960f752b7026e569baad29cdf41deeddf7c47497dc199ce8aabe0c7d7";
constexpr char kHashSkillEntry[] =
    "cf6d6169272f7c4a4dbb084cc7786a67fed9c03d7376babdcb5e5ecdde00eef";
constexpr char kHashUlGetSkillLevel[] =
    "e3f94beec124905fdceee7be877c5006e976256d338613487b632a8015ff251";
constexpr char kHashSiGetPureSkillLevel[] =
    "adf769b01abe7aa717b10da02614e83f638bfb44f42495e4c2dc2bf9ea27aed";
constexpr char kHashSiGetSkillLevel[] =
    "b12045572f903e6050a9be87539beeb84cf880a1dae7f76e42e4444665ac201";
constexpr char kHashSkillInfoClass[] =
    "e4c1bb085eea897cbd36c2ecc9a50b9316187a7ed2fbb7654ad8e162c289c39";

constexpr DWORD kTickMsOn = 400;
constexpr DWORD kTickMsOff = 900;
constexpr DWORD kLogMs = 5000;
constexpr DWORD kJobWaitMs = 120;
constexpr int kMaxSkillLevelCap = 60;

using FnGetMaxLevel = int (*)(void* self, const void* methodInfo);
using FnUlGetSkillLevel = int (*)(void* self, int skillId, const void* methodInfo);
using FnSiGetSkillLevel = int (*)(void* self, void* cdRef, int skillId, void* seRef,
                                  const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<bool> gDesired{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gPatchHits{0};
std::atomic<uint32_t> gRestoreHits{0};
std::atomic<uint32_t> gHookBoostHits{0};
std::atomic<bool> gUlHookInstalled{false};
std::atomic<bool> gSiHookInstalled{false};
std::atomic<bool> gSiPureHookInstalled{false};

MethodInfoHead* gMiUlGetSkillLevel = nullptr;
MethodInfoHead* gMiSiGetSkillLevel = nullptr;
MethodInfoHead* gMiSiGetPureSkillLevel = nullptr;
MethodInfoHead* gMiGetMaxLevel = nullptr;
FnGetMaxLevel gGetMaxLevel = nullptr;
FnUlGetSkillLevel gOrigUlGetSkillLevel = nullptr;
FnSiGetSkillLevel gOrigSiGetSkillLevel = nullptr;
FnSiGetSkillLevel gOrigSiGetPureSkillLevel = nullptr;

std::mutex gOrigMu;
std::unordered_map<int, int> gOrigLevel;  // skillId → 开启前原等级

struct MaxCacheEnt {
    int maxLv = 0;
    bool masterOk = false;  // 已做过 Master 封顶（热路径可跳过扫字典）
};
std::mutex gMaxMu;
std::unordered_map<int, MaxCacheEnt> gMaxCache;

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool LooksLikePlayerSkillId(int id) { return id >= 1000 && id <= 99999999; }
bool LooksLikeSkillLevel(int lv) { return lv > 0 && lv <= kMaxSkillLevelCap; }

int LevelDataListSize(void* entry) {
    if (!LooksLikeHeapPtr(entry)) return 0;
    void* list = nullptr;
    __try {
        list = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + kFbLevelDataList);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const int n = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    return (n > 0 && n <= kMaxSkillLevelCap) ? n : 0;
}

void EnsureGetMaxLevel() {
    if (gGetMaxLevel) return;
    void* se = x::runtime::il2cpp::FindClass("", kHashSkillEntry);
    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 0;
    shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
    auto mr = x::runtime::il2cpp_method::FindMethodResolved(se, kRvaGetMaxLevel, shape,
                                                            "GetMaxLevel", kHashGetMaxLevel);
    if (mr.method) {
        gMiGetMaxLevel = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (gMiGetMaxLevel && gMiGetMaxLevel->methodPointer)
            gGetMaxLevel = reinterpret_cast<FnGetMaxLevel>(gMiGetMaxLevel->methodPointer);
    }
    if (!gGetMaxLevel) gGetMaxLevel = x::runtime::il2cpp::AtRva<FnGetMaxLevel>(kRvaGetMaxLevel);
}

int CallGetMaxLevel(void* entry) {
    if (!LooksLikeHeapPtr(entry)) return 0;
    EnsureGetMaxLevel();
    auto fn = gGetMaxLevel;
    int mx = 0;
    if (fn) {
        __try {
            mx = fn(entry, gMiGetMaxLevel);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mx = 0;
        }
    }
    if (mx <= 0 || mx > kMaxSkillLevelCap) mx = LevelDataListSize(entry);
    return (mx > 0 && mx <= kMaxSkillLevelCap) ? mx : 0;
}

void CacheMax(int skillId, int maxLv, bool masterOk) {
    if (skillId <= 0 || maxLv <= 0) return;
    std::lock_guard<std::mutex> lock(gMaxMu);
    gMaxCache[skillId] = MaxCacheEnt{maxLv, masterOk};
}

// outMasterOk：缓存是否已带 Master 封顶；未命中时写 false。
int CachedMax(int skillId, bool* outMasterOk) {
    if (outMasterOk) *outMasterOk = false;
    std::lock_guard<std::mutex> lock(gMaxMu);
    auto it = gMaxCache.find(skillId);
    if (it == gMaxCache.end()) return 0;
    if (outMasterOk) *outMasterOk = it->second.masterOk;
    return it->second.maxLv;
}

void ClearMaxCache() {
    std::lock_guard<std::mutex> lock(gMaxMu);
    gMaxCache.clear();
}

int MasterLevelOf(void* masterDict, int skillId) {
    // Dict<int,int> SkillMasterLevel；无条目则返回 0（不封顶）。
    if (!LooksLikeHeapPtr(masterDict) || skillId <= 0) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromDictInstance(masterDict);
    void* entries = ReadPtr(masterDict, x::runtime::il2cpp_container::OffDictEntries());
    if (!entries) return 0;
    const size_t offHash = x::runtime::il2cpp_container::OffDictEntryHash();
    const size_t offKey = x::runtime::il2cpp_container::OffDictEntryKey();
    const size_t strides[] = {x::runtime::il2cpp_container::DictEntryStrideIntIntTight(),
                              x::runtime::il2cpp_container::DictEntryStrideIntIntAlign()};
    const size_t valOffs[] = {x::runtime::il2cpp_container::OffDictEntryValueIntTight(),
                             x::runtime::il2cpp_container::OffDictEntryValueIntAlign()};
    const int len = ReadI32(entries, x::runtime::il2cpp_container::OffArrayMaxLength());
    for (int pass = 0; pass < 2 && len > 0; ++pass) {
        for (int i = 0; i < len && i < 4096; ++i) {
            uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, strides[pass]);
            if (!e) continue;
            __try {
                if (*reinterpret_cast<int*>(e + offHash) < 0) continue;
                if (*reinterpret_cast<int*>(e + offKey) != skillId) continue;
                const int v = *reinterpret_cast<int*>(e + valOffs[pass]);
                return (v > 0 && v <= kMaxSkillLevelCap) ? v : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    return 0;
}

int CapByMasterDict(int skillId, int maxLv, void* masterDict) {
    if (maxLv <= 0) return 0;
    const int master = MasterLevelOf(masterDict, skillId);
    if (master > 0 && master < maxLv) return master;
    return maxLv;
}

int CapByLocalMaster(int skillId, int maxLv, bool* outApplied) {
    if (outApplied) *outApplied = false;
    if (maxLv <= 0) return 0;
    void* cd = x::ui::player::LocalCharacterData();
    if (!LooksLikeHeapPtr(cd)) return maxLv;
    void* master = ReadPtr(cd, x::ui::player::OffSkillMasterLevel());
    // master 指针可空：仍算「已尝试封顶」，避免热路径反复扫 CD
    if (outApplied) *outApplied = true;
    return CapByMasterDict(skillId, maxLv, master);
}

int ResolveMaxForSkill(int skillId, void* masterDict) {
    if (skillId <= 0) return 0;
    int maxLv = CachedMax(skillId, nullptr);
    if (maxLv <= 0) {
        void* entry = x::features::ports::skill::GetSkillEntry(skillId);
        maxLv = CallGetMaxLevel(entry);
        if (maxLv <= 0) return 0;
    }
    // A 路径每 tick 再夹一次：Master 表变更可纠正；并标记 masterOk 供热路径复用
    maxLv = CapByMasterDict(skillId, maxLv, masterDict);
    CacheMax(skillId, maxLv, true);
    return maxLv;
}

struct DictLayout {
    size_t stride = 0;
    size_t valOff = 0;
};

DictLayout PickIntIntLayout(void* entries, int len) {
    DictLayout out{};
    if (!entries || len <= 0) return out;
    const size_t offHash = x::runtime::il2cpp_container::OffDictEntryHash();
    const size_t offKey = x::runtime::il2cpp_container::OffDictEntryKey();
    const size_t strideTight = x::runtime::il2cpp_container::DictEntryStrideIntIntTight();
    const size_t strideAlign = x::runtime::il2cpp_container::DictEntryStrideIntIntAlign();
    const size_t valTight = x::runtime::il2cpp_container::OffDictEntryValueIntTight();
    const size_t valAlign = x::runtime::il2cpp_container::OffDictEntryValueIntAlign();

    auto score = [&](size_t stride, size_t valOff) -> int {
        int s = 0;
        for (int i = 0; i < len && i < 4096; ++i) {
            uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, stride);
            if (!e) continue;
            __try {
                if (*reinterpret_cast<int*>(e + offHash) < 0) continue;
                const int key = *reinterpret_cast<int*>(e + offKey);
                const int val = *reinterpret_cast<int*>(e + valOff);
                if (LooksLikePlayerSkillId(key) && LooksLikeSkillLevel(val)) ++s;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return s;
    };
    const int st = score(strideTight, valTight);
    const int sa = score(strideAlign, valAlign);
    if (sa > st) {
        out.stride = strideAlign;
        out.valOff = valAlign;
    } else {
        out.stride = strideTight;
        out.valOff = valTight;
    }
    return out;
}

struct TickStats {
    int scanned = 0;
    int patched = 0;
    int restored = 0;
    int already = 0;
    int skipped = 0;
    int dictOk = 0;  // 字典侧已达满级（patched+already）
    int hookOn = 0;  // bit0=UserLocal bit1=SI.GetSkillLevel bit2=SI.GetPure
    const char* err = nullptr;
    const char* src = "none";
};

void RememberOrig(int skillId, int level) {
    // 只记「低于满级」的原值，避免把已满级误当成可还原真源
    std::lock_guard<std::mutex> lock(gOrigMu);
    if (gOrigLevel.find(skillId) == gOrigLevel.end()) gOrigLevel[skillId] = level;
}

int OrigOf(int skillId, int fallback) {
    std::lock_guard<std::mutex> lock(gOrigMu);
    auto it = gOrigLevel.find(skillId);
    return it == gOrigLevel.end() ? fallback : it->second;
}

void ClearOrig() {
    std::lock_guard<std::mutex> lock(gOrigMu);
    gOrigLevel.clear();
}

bool PatchMi(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig) return false;
    if (orig == hook) return *outOrig != nullptr;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        if (mi->virtualMethodPointer == orig) mi->virtualMethodPointer = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return ok;
}

void RestoreMi(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    __try {
        void* cur = mi->methodPointer;
        mi->methodPointer = orig;
        if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

// Hook / skill_port 共用：feature 开且已学时抬到满级。
// Master 封顶：缓存带 masterOk 则热路径不再扫字典；A tick 会刷新。
int ApplyForceMax(int skillId, int rawLevel) {
    if (!gDesired.load(std::memory_order_relaxed)) return rawLevel;
    if (rawLevel <= 0 || !LooksLikePlayerSkillId(skillId)) return rawLevel;

    bool masterOk = false;
    int maxLv = CachedMax(skillId, &masterOk);
    if (maxLv <= 0) {
        void* entry = x::features::ports::skill::GetSkillEntry(skillId);
        maxLv = CallGetMaxLevel(entry);
        masterOk = false;
    }
    if (maxLv > 0 && !masterOk) {
        bool applied = false;
        maxLv = CapByLocalMaster(skillId, maxLv, &applied);
        CacheMax(skillId, maxLv, applied);
    }
    if (maxLv > rawLevel) {
        gHookBoostHits.fetch_add(1, std::memory_order_relaxed);
        return maxLv;
    }
    return rawLevel;
}

int Hook_UlGetSkillLevel(void* self, int skillId, const void* methodInfo) {
    int lv = 0;
    if (gOrigUlGetSkillLevel) {
        __try {
            lv = gOrigUlGetSkillLevel(self, skillId, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lv = 0;
        }
    }
    return ApplyForceMax(skillId, lv);
}

int Hook_SiGetSkillLevel(void* self, void* cdRef, int skillId, void* seRef,
                         const void* methodInfo) {
    int lv = 0;
    if (gOrigSiGetSkillLevel) {
        __try {
            lv = gOrigSiGetSkillLevel(self, cdRef, skillId, seRef, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lv = 0;
        }
    }
    return ApplyForceMax(skillId, lv);
}

int Hook_SiGetPureSkillLevel(void* self, void* cdRef, int skillId, void* seRef,
                             const void* methodInfo) {
    int lv = 0;
    if (gOrigSiGetPureSkillLevel) {
        __try {
            lv = gOrigSiGetPureSkillLevel(self, cdRef, skillId, seRef, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lv = 0;
        }
    }
    return ApplyForceMax(skillId, lv);
}

bool FindMiByRva(void* klass, uint32_t rva, MethodInfoHead** outMi) {
    if (!klass || !outMi || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods || !e.ga) return false;
    const uintptr_t want = reinterpret_cast<uintptr_t>(e.ga) + rva;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = e.classGetMethods(klass, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            void* mp = nullptr;
            __try {
                mp = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mp = nullptr;
            }
            if (reinterpret_cast<uintptr_t>(mp) == want) {
                *outMi = mi;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool EnsureUlHookMi() {
    if (gMiUlGetSkillLevel) return true;
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (!ul) return false;

    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 1;
    shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
    auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        ul, kRvaUlGetSkillLevel, shape, "GetSkillLevel", kHashUlGetSkillLevel);
    if (mr.method) {
        gMiUlGetSkillLevel = reinterpret_cast<MethodInfoHead*>(mr.method);
        return true;
    }
    return FindMiByRva(ul, kRvaUlGetSkillLevel, &gMiUlGetSkillLevel);
}

bool EnsureSiHookMi() {
    if (gMiSiGetSkillLevel && gMiSiGetPureSkillLevel) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    void* si = x::runtime::il2cpp::FindClass("", kHashSkillInfoClass);
    if (!si) return false;

    // 同形 arity=3：靠 hash/RVA 区分 GetPure vs GetSkillLevel
    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 3;
    shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[0] = x::runtime::il2cpp_method::TypeKind::Ptr;
    shape.param[1] = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[2] = x::runtime::il2cpp_method::TypeKind::Ptr;

    if (!gMiSiGetSkillLevel) {
        auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            si, kRvaSiGetSkillLevel, shape, "GetSkillLevel", kHashSiGetSkillLevel);
        if (mr.method) {
            gMiSiGetSkillLevel = reinterpret_cast<MethodInfoHead*>(mr.method);
        } else {
            (void)FindMiByRva(si, kRvaSiGetSkillLevel, &gMiSiGetSkillLevel);
        }
    }
    if (!gMiSiGetPureSkillLevel) {
        auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            si, kRvaSiGetPureSkillLevel, shape, "GetPureSkillLevel", kHashSiGetPureSkillLevel);
        if (mr.method) {
            gMiSiGetPureSkillLevel = reinterpret_cast<MethodInfoHead*>(mr.method);
        } else {
            (void)FindMiByRva(si, kRvaSiGetPureSkillLevel, &gMiSiGetPureSkillLevel);
        }
    }
    return gMiSiGetSkillLevel || gMiSiGetPureSkillLevel;
}

bool InstallUlHook() {
    if (gUlHookInstalled.load(std::memory_order_acquire)) return true;
    if (!EnsureUlHookMi()) return false;
    void* orig = nullptr;
    if (!PatchMi(gMiUlGetSkillLevel, reinterpret_cast<void*>(&Hook_UlGetSkillLevel), &orig)) {
        return false;
    }
    gOrigUlGetSkillLevel = reinterpret_cast<FnUlGetSkillLevel>(orig);
    gUlHookInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("SkillMax", "hook UserLocal.GetSkillLevel mi=%p orig=%p",
                     (void*)gMiUlGetSkillLevel, orig);
    return true;
}

bool InstallSiLevelHook() {
    if (gSiHookInstalled.load(std::memory_order_acquire)) return true;
    if (!EnsureSiHookMi() || !gMiSiGetSkillLevel) return false;
    void* orig = nullptr;
    if (!PatchMi(gMiSiGetSkillLevel, reinterpret_cast<void*>(&Hook_SiGetSkillLevel), &orig)) {
        return false;
    }
    gOrigSiGetSkillLevel = reinterpret_cast<FnSiGetSkillLevel>(orig);
    gSiHookInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("SkillMax", "hook SkillInfo.GetSkillLevel mi=%p orig=%p",
                     (void*)gMiSiGetSkillLevel, orig);
    return true;
}

bool InstallSiPureHook() {
    if (gSiPureHookInstalled.load(std::memory_order_acquire)) return true;
    if (!EnsureSiHookMi() || !gMiSiGetPureSkillLevel) return false;
    void* orig = nullptr;
    if (!PatchMi(gMiSiGetPureSkillLevel, reinterpret_cast<void*>(&Hook_SiGetPureSkillLevel),
                 &orig)) {
        return false;
    }
    gOrigSiGetPureSkillLevel = reinterpret_cast<FnSiGetSkillLevel>(orig);
    gSiPureHookInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("SkillMax", "hook SkillInfo.GetPureSkillLevel mi=%p orig=%p",
                     (void*)gMiSiGetPureSkillLevel, orig);
    return true;
}

void UninstallHook() {
    if (gUlHookInstalled.load(std::memory_order_acquire)) {
        if (gMiUlGetSkillLevel && gOrigUlGetSkillLevel) {
            RestoreMi(gMiUlGetSkillLevel, reinterpret_cast<void*>(gOrigUlGetSkillLevel));
        }
        gOrigUlGetSkillLevel = nullptr;
        gUlHookInstalled.store(false, std::memory_order_release);
        x::runtime::LogI("SkillMax", "hook UserLocal.GetSkillLevel removed");
    }
    if (gSiHookInstalled.load(std::memory_order_acquire)) {
        if (gMiSiGetSkillLevel && gOrigSiGetSkillLevel) {
            RestoreMi(gMiSiGetSkillLevel, reinterpret_cast<void*>(gOrigSiGetSkillLevel));
        }
        gOrigSiGetSkillLevel = nullptr;
        gSiHookInstalled.store(false, std::memory_order_release);
        x::runtime::LogI("SkillMax", "hook SkillInfo.GetSkillLevel removed");
    }
    if (gSiPureHookInstalled.load(std::memory_order_acquire)) {
        if (gMiSiGetPureSkillLevel && gOrigSiGetPureSkillLevel) {
            RestoreMi(gMiSiGetPureSkillLevel, reinterpret_cast<void*>(gOrigSiGetPureSkillLevel));
        }
        gOrigSiGetPureSkillLevel = nullptr;
        gSiPureHookInstalled.store(false, std::memory_order_release);
        x::runtime::LogI("SkillMax", "hook SkillInfo.GetPureSkillLevel removed");
    }
}

void MutateSkillDict(void* dict, void* masterDict, bool forceOn, TickStats* st) {
    if (!LooksLikeHeapPtr(dict) || !st) return;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, x::runtime::il2cpp_container::OffDictEntries());
    if (!entries) return;
    const int len = ReadI32(entries, x::runtime::il2cpp_container::OffArrayMaxLength());
    if (len <= 0) return;
    const DictLayout lay = PickIntIntLayout(entries, len);
    if (!lay.stride || !lay.valOff) return;

    const size_t offHash = x::runtime::il2cpp_container::OffDictEntryHash();
    const size_t offKey = x::runtime::il2cpp_container::OffDictEntryKey();

    for (int i = 0; i < len && i < 4096; ++i) {
        uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, lay.stride);
        if (!e) continue;
        int key = 0;
        int val = 0;
        __try {
            if (*reinterpret_cast<int*>(e + offHash) < 0) continue;
            key = *reinterpret_cast<int*>(e + offKey);
            val = *reinterpret_cast<int*>(e + lay.valOff);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!LooksLikePlayerSkillId(key) || val <= 0) continue;
        ++st->scanned;

        if (!forceOn) {
            const int orig = OrigOf(key, val);
            if (val == orig) {
                ++st->already;
                continue;
            }
            __try {
                *reinterpret_cast<int*>(e + lay.valOff) = orig;
                ++st->restored;
                gRestoreHits.fetch_add(1, std::memory_order_relaxed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ++st->skipped;
            }
            continue;
        }

        int maxLv = ResolveMaxForSkill(key, masterDict);
        if (maxLv <= 0) {
            ++st->skipped;
            continue;
        }

        // 仅在尚未满级时记原值，防止把 max 当成 orig
        if (val < maxLv) RememberOrig(key, val);

        if (val >= maxLv) {
            ++st->already;
            ++st->dictOk;
            continue;
        }
        __try {
            *reinterpret_cast<int*>(e + lay.valOff) = maxLv;
            ++st->patched;
            ++st->dictOk;
            gPatchHits.fetch_add(1, std::memory_order_relaxed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++st->skipped;
        }
    }
}

const char* PickSrc(bool on, const TickStats& st) {
    if (!on) return st.restored > 0 ? "restore" : "off";
    const bool dict = st.dictOk > 0;
    const bool hook = st.hookOn != 0;
    if (dict && hook) return "dict+hook";
    if (dict) return "dict";
    if (hook) return "hook";
    return "fail";
}

void ReportSkillMaxLamp(const TickStats& st) {
    using Code = x::runtime::anchor_lamps::AnchorLampCode;
    if (!gDesired.load(std::memory_order_relaxed)) {
        x::runtime::anchor_lamps::Set("SkillMax", Code::Unknown, "off");
        return;
    }
    if (st.src && strcmp(st.src, "wait") == 0) {
        x::runtime::anchor_lamps::Set("SkillMax", Code::Unknown, "wait");
        return;
    }
    const int nHook = ((st.hookOn & 1) ? 1 : 0) + ((st.hookOn & 2) ? 1 : 0) +
                      ((st.hookOn & 4) ? 1 : 0);
    const bool dict = st.dictOk > 0;
    char d[xcat::kAnchorLampDetailLen]{};
    if (nHook == 3 && dict) {
        snprintf(d, sizeof(d), "dict+3hook");
        x::runtime::anchor_lamps::Set("SkillMax", Code::Ok, d);
        return;
    }
    if (nHook > 0 || dict) {
        snprintf(d, sizeof(d), "h=%d/3 d=%d", nHook, st.dictOk);
        x::runtime::anchor_lamps::Set("SkillMax", Code::Degraded, d);
        return;
    }
    snprintf(d, sizeof(d), "%s", st.err && st.err[0] ? st.err : "fail");
    x::runtime::anchor_lamps::Set("SkillMax", Code::Miss, d);
}

void TickOnMain(void* user) {
    auto* st = reinterpret_cast<TickStats*>(user);
    if (!st) return;
    *st = {};
    st->src = "none";

    if (!x::features::ports::world::IsPlayReady()) {
        st->err = "not play-ready";
        st->src = "wait";
        return;
    }
    if (!x::features::ports::skill::EnsureBound()) {
        st->err = "skill unbound";
        st->src = "fail";
        return;
    }

    void* cd = x::ui::player::LocalCharacterData();
    if (!LooksLikeHeapPtr(cd)) {
        st->err = "no CharacterData";
        st->src = "fail";
        return;
    }

    void* rec = ReadPtr(cd, x::ui::player::OffSkillRecord());
    void* recEx = ReadPtr(cd, x::ui::player::OffSkillRecordEx());
    void* master = ReadPtr(cd, x::ui::player::OffSkillMasterLevel());
    const bool on = gDesired.load(std::memory_order_relaxed);

    if (on) {
        if (InstallUlHook()) st->hookOn |= 1;
        if (InstallSiLevelHook()) st->hookOn |= 2;
        if (InstallSiPureHook()) st->hookOn |= 4;
        MutateSkillDict(rec, master, true, st);
        MutateSkillDict(recEx, master, true, st);
    } else {
        MutateSkillDict(rec, master, false, st);
        MutateSkillDict(recEx, master, false, st);
        UninstallHook();
        ClearOrig();
        ClearMaxCache();
    }
    st->src = PickSrc(on, *st);
}

void TickOnce() {
    if (!x::runtime::main_thread::Ensure()) return;
    if (x::runtime::main_thread::IsCongested()) {
        if (gDesired.load(std::memory_order_relaxed)) {
            x::runtime::anchor_lamps::Set("SkillMax",
                                         x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                         "busy");
        }
        return;
    }

    TickStats st{};
    if (!x::runtime::main_thread::InvokeAndWait(&TickOnMain, &st, kJobWaitMs,
                                               x::runtime::main_thread::JobPrio::Normal)) {
        if (gDesired.load(std::memory_order_relaxed)) {
            x::runtime::anchor_lamps::Set("SkillMax",
                                         x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                         "invoke");
        }
        return;
    }
    ReportSkillMaxLamp(st);

    static DWORD sLastLog = 0;
    const DWORD now = GetTickCount();
    if (now - sLastLog < kLogMs) return;
    sLastLog = now;
    const bool on = gDesired.load(std::memory_order_relaxed);
    if (!on && st.restored == 0 && st.scanned == 0 && !st.err) return;
    x::runtime::LogI(
        "SkillMax",
        "%s src=%s scanned=%d patched=%d restored=%d already=%d skip=%d dictOk=%d "
        "hookUl=%d hookSi=%d hookPure=%d patchHits=%u restoreHits=%u hookBoosts=%u err=%s",
        on ? "force-max" : "restore", st.src ? st.src : "?", st.scanned, st.patched, st.restored,
        st.already, st.skipped, st.dictOk, (st.hookOn & 1) ? 1 : 0, (st.hookOn & 2) ? 1 : 0,
        (st.hookOn & 4) ? 1 : 0, gPatchHits.load(std::memory_order_relaxed),
        gRestoreHits.load(std::memory_order_relaxed),
        gHookBoostHits.load(std::memory_order_relaxed), st.err ? st.err : "-");
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("SkillMax", "worker start desired=%d", gDesired.load() ? 1 : 0);
    while (!gStop.load(std::memory_order_relaxed)) {
        const bool on = gDesired.load(std::memory_order_relaxed);
        TickOnce();
        Sleep(on ? kTickMsOn : kTickMsOff);
    }
    gDesired.store(false, std::memory_order_relaxed);
    TickOnce();  // 还原字典 + 卸钩
    x::runtime::LogI("SkillMax", "worker stop");
    return 0;
}

bool EnvForceOn() {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_SKILL_MAX_LEVEL", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

}  // namespace

void Init() {
    if (!xcat::kSkillMaxLevelUserEnabled) {
        gDesired.store(false, std::memory_order_relaxed);
        x::runtime::LogI("SkillMax", "user gate off — skipped (keep code)");
        return;
    }
    if (EnvForceOn()) {
        gDesired.store(true, std::memory_order_relaxed);
        x::runtime::LogI("SkillMax", "env XCAT_SKILL_MAX_LEVEL → on");
    }
}

void Shutdown() {
    StopWorker();
    UninstallHook();
    ClearOrig();
    ClearMaxCache();
}

void StartWorker() {
    if (!xcat::kSkillMaxLevelUserEnabled) {
        gDesired.store(false, std::memory_order_relaxed);
        x::runtime::anchor_lamps::Set("SkillMax",
                                     x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                     "disabled");
        return;
    }
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_relaxed);
    HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (h) gWorker.store(h, std::memory_order_release);
}

void StopWorker() {
    gStop.store(true, std::memory_order_relaxed);
    HANDLE h = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (h) {
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
    UninstallHook();
}

void SetDesired(bool on) {
    if (!xcat::kSkillMaxLevelUserEnabled) on = false;
    gDesired.store(on, std::memory_order_relaxed);
}

bool IsDesired() { return gDesired.load(std::memory_order_relaxed); }

int AdjustLevelIfForced(int skillId, int rawLevel) { return ApplyForceMax(skillId, rawLevel); }

}  // namespace x::features::skill_max_level
