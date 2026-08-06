// TWMS Classic — skill_max_level
// A: SkillRecord 等级→满级；B: Hook UserLocal.GetSkillLevel 作 fallback。
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
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace x::features::skill_max_level {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr size_t kFbCdSkillMasterLevel = 0x60;
constexpr size_t kFbLevelDataList = 0x120;
constexpr uint32_t kRvaGetMaxLevel = 0x1560D90;
constexpr uint32_t kRvaGetSkillLevel = 0x106B600;
constexpr char kHashGetSkillLevel[] =
    "edc26b11d81b1fa077ad2675981f80527a412dcfacfd3874470fe2876a5eb7c";

constexpr DWORD kTickMsOn = 400;
constexpr DWORD kTickMsOff = 900;
constexpr DWORD kLogMs = 5000;
constexpr DWORD kJobWaitMs = 120;
constexpr int kMaxSkillLevelCap = 60;

using FnGetMaxLevel = int (*)(void* self, const void* methodInfo);
using FnGetSkillLevel = int (*)(void* self, int skillId, const void* methodInfo);

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
std::atomic<bool> gHookInstalled{false};

MethodInfoHead* gMiGetSkillLevel = nullptr;
FnGetSkillLevel gOrigGetSkillLevel = nullptr;

std::mutex gOrigMu;
std::unordered_map<int, int> gOrigLevel;  // skillId → 开启前原等级

std::mutex gMaxMu;
std::unordered_map<int, int> gMaxCache;  // skillId → 满级（hook 热路径）

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

int CallGetMaxLevel(void* entry) {
    if (!LooksLikeHeapPtr(entry)) return 0;
    auto fn = x::runtime::il2cpp::AtRva<FnGetMaxLevel>(kRvaGetMaxLevel);
    int mx = 0;
    if (fn) {
        __try {
            mx = fn(entry, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mx = 0;
        }
    }
    if (mx <= 0 || mx > kMaxSkillLevelCap) mx = LevelDataListSize(entry);
    return (mx > 0 && mx <= kMaxSkillLevelCap) ? mx : 0;
}

void CacheMax(int skillId, int maxLv) {
    if (skillId <= 0 || maxLv <= 0) return;
    std::lock_guard<std::mutex> lock(gMaxMu);
    gMaxCache[skillId] = maxLv;
}

int CachedMax(int skillId) {
    std::lock_guard<std::mutex> lock(gMaxMu);
    auto it = gMaxCache.find(skillId);
    return it == gMaxCache.end() ? 0 : it->second;
}

void ClearMaxCache() {
    std::lock_guard<std::mutex> lock(gMaxMu);
    gMaxCache.clear();
}

int ResolveMaxForSkill(int skillId, void* masterDict) {
    if (skillId <= 0) return 0;
    if (const int c = CachedMax(skillId)) return c;

    void* entry = x::features::ports::skill::GetSkillEntry(skillId);
    int maxLv = CallGetMaxLevel(entry);
    if (maxLv <= 0) return 0;

    if (LooksLikeHeapPtr(masterDict)) {
        x::runtime::il2cpp_container::Ensure();
        x::runtime::il2cpp_container::RefineFromDictInstance(masterDict);
        void* entries = ReadPtr(masterDict, x::runtime::il2cpp_container::OffDictEntries());
        if (entries) {
            const size_t offHash = x::runtime::il2cpp_container::OffDictEntryHash();
            const size_t offKey = x::runtime::il2cpp_container::OffDictEntryKey();
            const size_t strides[] = {x::runtime::il2cpp_container::DictEntryStrideIntIntTight(),
                                      x::runtime::il2cpp_container::DictEntryStrideIntIntAlign()};
            const size_t valOffs[] = {x::runtime::il2cpp_container::OffDictEntryValueIntTight(),
                                     x::runtime::il2cpp_container::OffDictEntryValueIntAlign()};
            const int len = ReadI32(entries, x::runtime::il2cpp_container::OffArrayMaxLength());
            for (int pass = 0; pass < 2 && len > 0; ++pass) {
                for (int i = 0; i < len && i < 4096; ++i) {
                    uint8_t* e =
                        x::runtime::il2cpp_container::DictEntryAt(entries, i, strides[pass]);
                    if (!e) continue;
                    __try {
                        if (*reinterpret_cast<int*>(e + offHash) < 0) continue;
                        if (*reinterpret_cast<int*>(e + offKey) != skillId) continue;
                        const int master = *reinterpret_cast<int*>(e + valOffs[pass]);
                        if (master > 0 && master < maxLv) maxLv = master;
                        goto cached;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                }
            }
        }
    }
cached:
    CacheMax(skillId, maxLv);
    return maxLv;
}

int MasterLevelOf(void* masterDict, int skillId) {
    // ResolveMaxForSkill 内已含 master 封顶；此处仅给 dict 路径单独用。
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
    int hookOn = 0;
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

int Hook_GetSkillLevel(void* self, int skillId, const void* methodInfo) {
    int lv = 0;
    if (gOrigGetSkillLevel) {
        __try {
            lv = gOrigGetSkillLevel(self, skillId, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lv = 0;
        }
    }
    if (!gDesired.load(std::memory_order_relaxed)) return lv;
    if (lv <= 0 || !LooksLikePlayerSkillId(skillId)) return lv;

    int maxLv = CachedMax(skillId);
    if (maxLv <= 0) {
        // 热路径尽量轻：无 masterDict 时先 GetMaxLevel；master 封顶由 A 路径缓存补齐
        void* entry = x::features::ports::skill::GetSkillEntry(skillId);
        maxLv = CallGetMaxLevel(entry);
        if (maxLv > 0) CacheMax(skillId, maxLv);
    }
    if (maxLv > lv) {
        gHookBoostHits.fetch_add(1, std::memory_order_relaxed);
        return maxLv;
    }
    return lv;
}

bool EnsureHookMi() {
    if (gMiGetSkillLevel) return true;
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (!ul) return false;

    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 1;
    shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
    auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        ul, kRvaGetSkillLevel, shape, "GetSkillLevel", kHashGetSkillLevel);
    if (mr.method) {
        gMiGetSkillLevel = reinterpret_cast<MethodInfoHead*>(mr.method);
        return true;
    }
    // RVA 扫 MI
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods || !e.ga) return false;
    const uintptr_t want = reinterpret_cast<uintptr_t>(e.ga) + kRvaGetSkillLevel;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = e.classGetMethods(ul, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            void* mp = nullptr;
            __try {
                mp = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mp = nullptr;
            }
            if (reinterpret_cast<uintptr_t>(mp) == want) {
                gMiGetSkillLevel = mi;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool InstallHook() {
    if (gHookInstalled.load(std::memory_order_acquire)) return true;
    if (!EnsureHookMi()) return false;
    void* orig = nullptr;
    if (!PatchMi(gMiGetSkillLevel, reinterpret_cast<void*>(&Hook_GetSkillLevel), &orig)) {
        return false;
    }
    gOrigGetSkillLevel = reinterpret_cast<FnGetSkillLevel>(orig);
    gHookInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("SkillMax", "hook GetSkillLevel installed mi=%p orig=%p",
                     (void*)gMiGetSkillLevel, orig);
    return true;
}

void UninstallHook() {
    if (!gHookInstalled.load(std::memory_order_acquire)) return;
    if (gMiGetSkillLevel && gOrigGetSkillLevel) {
        RestoreMi(gMiGetSkillLevel, reinterpret_cast<void*>(gOrigGetSkillLevel));
    }
    gOrigGetSkillLevel = nullptr;
    gHookInstalled.store(false, std::memory_order_release);
    x::runtime::LogI("SkillMax", "hook GetSkillLevel removed");
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
            void* entry = x::features::ports::skill::GetSkillEntry(key);
            maxLv = CallGetMaxLevel(entry);
            const int master = MasterLevelOf(masterDict, key);
            if (master > 0 && master < maxLv) maxLv = master;
            if (maxLv > 0) CacheMax(key, maxLv);
        }
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
    void* master = ReadPtr(cd, kFbCdSkillMasterLevel);
    const bool on = gDesired.load(std::memory_order_relaxed);

    if (on) {
        if (InstallHook()) st->hookOn = 1;
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
    if (x::runtime::main_thread::IsCongested()) return;

    TickStats st{};
    if (!x::runtime::main_thread::InvokeAndWait(&TickOnMain, &st, kJobWaitMs,
                                               x::runtime::main_thread::JobPrio::Normal)) {
        return;
    }

    static DWORD sLastLog = 0;
    const DWORD now = GetTickCount();
    if (now - sLastLog < kLogMs) return;
    sLastLog = now;
    const bool on = gDesired.load(std::memory_order_relaxed);
    if (!on && st.restored == 0 && st.scanned == 0 && !st.err) return;
    x::runtime::LogI(
        "SkillMax",
        "%s src=%s scanned=%d patched=%d restored=%d already=%d skip=%d dictOk=%d "
        "hook=%d patchHits=%u restoreHits=%u hookBoosts=%u err=%s",
        on ? "force-max" : "restore", st.src ? st.src : "?", st.scanned, st.patched, st.restored,
        st.already, st.skipped, st.dictOk, st.hookOn, gPatchHits.load(std::memory_order_relaxed),
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

void SetDesired(bool on) { gDesired.store(on, std::memory_order_relaxed); }

bool IsDesired() { return gDesired.load(std::memory_order_relaxed); }

}  // namespace x::features::skill_max_level
