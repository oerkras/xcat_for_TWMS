// Classic TWMS — 吸怪平行检测族探针 v2。证据见
// docs/features/security/吸怪平行检测族-拉怪密度死点.md
// IDB imagebase 0x7ffd60830000（与 AtRva 同一套 RVA）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_inspect_probe.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x::features::ports::mob_inspect_probe {
namespace {

using x::runtime::il2cpp::AtRva;

// 2026-08-22 runtime IDB。abs-jmp 要 12 字节且不能切开 RIP-relative；
// 序言不够 12 或后面紧跟 lea [rip] 的，改 E9+近桩（steal 落在指令边界）。

constexpr uint32_t kRvaPullKick = 0x1C1D430;   // 6244D430
constexpr uint32_t kRvaPullSend = 0x1C1B5A0;   // 6244B5A0
constexpr uint32_t kRvaDensity = 0x1C20360;    // 62450360
constexpr uint32_t kRvaDeadPos = 0x1C14720;    // 62444720
constexpr uint32_t kRvaHub408 = 0xD10600;      // 61540600
constexpr uint32_t kRvaSched = 0xBF5BB0;       // 61425BB0 → tail jmp tick
constexpr uint32_t kRvaDispatch = 0x1C19BA0;   // 62449BA0 Inspect 分发
constexpr uint32_t kRvaTick = 0x1C275E0;       // 624575E0 密度/死点 tick
constexpr uint32_t kRvaDensWrap = 0x1C1F730;   // 6244F730
constexpr uint32_t kRvaDeadWrap = 0x1C13E00;   // 62443E00
constexpr uint32_t kRvaUnwrap = 0xB2FDF0;      // 6135FDF0 GetInt 解盒

constexpr uint8_t kSigPullKick[] = {0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
                                    0x81, 0xEC, 0xC8, 0x02, 0x00, 0x00};
constexpr uint8_t kSigPullSend[] = {0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
                                    0x81, 0xEC, 0xA8, 0x01, 0x00, 0x00};
constexpr uint8_t kSigDensity[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55,
                                   0x53, 0x48, 0x81, 0xEC, 0xA0, 0x02, 0x00, 0x00};
constexpr uint8_t kSigDeadPos[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56,
                                   0x57, 0x53, 0x48, 0x81, 0xEC, 0xF8, 0x07, 0x00, 0x00};
constexpr uint8_t kSigHub408[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55,
                                  0x53, 0x48, 0x81, 0xEC, 0xA0, 0x01, 0x00, 0x00};
constexpr uint8_t kSigSched[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x70};
constexpr uint8_t kSigDispatch[] = {0x56, 0x57, 0x48, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00};
constexpr uint8_t kSigTick[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53};
constexpr uint8_t kSigDensWrap[] = {0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
                                    0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00};
constexpr uint8_t kSigDeadWrap[] = {0x56, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x01, 0x00, 0x00};

constexpr int kLvMiss = -999999;
constexpr DWORD kHbMs = 2000;
constexpr DWORD kSnapMs = 15000;
constexpr DWORD kEdgeLogMs = 250;

enum Site : int {
    kPullKick = 0,
    kPullSend,
    kDensity,
    kDead,
    kHub408,
    kSched,
    kDispatch,
    kTick,
    kDensWrap,
    kDeadWrap,
    kSiteN
};

enum HookKind : int { kAbs12 = 0, kRel5 = 1 };

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    void* nearStub = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

using FnFwd = void(__fastcall*)(void* a, void* b, void* c, void* d);
using FnThunk = intptr_t(__fastcall*)(int defVal, void* mi);
using FnUnwrap = intptr_t(__fastcall*)(void* a, intptr_t wrapped, int z);

std::atomic<bool> gWant{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<bool> gHooksOn{false};
std::atomic<bool> gRefuse{false};

AbsHookState gAbs[kSiteN]{};
FnFwd gTramp[kSiteN]{};
std::atomic<uint32_t> gHit[kSiteN]{};
std::atomic<DWORD> gLastEdge[kSiteN]{};

struct LvThunk {
    int id;
    uint32_t rva;
};

// docs §3 已验种子的专用 GetInt thunk。不 FindClass（TW 哈希名会 miss）。
constexpr LvThunk kLvThunks[] = {
    {220, 0x1700BD0}, {408, 0x1722BF0}, {411, 0x1723480}, {412, 0x1723750},
    {525, 0x17387E0}, {710, 0x1759A20}, {781, 0x17669E0}, {926, 0x1781490},
    {927, 0x17817D0}, {928, 0x1781A60}, {929, 0x1781D50}, {930, 0x1782050},
    {932, 0x17825E0}, {933, 0x17828A0}, {934, 0x1782C30}, {935, 0x1782FB0},
    {936, 0x1783220}, {937, 0x17834D0}, {938, 0x17837F0}, {939, 0x1783AE0},
    {940, 0x1783E10}, {941, 0x1784120},
};

const char* SiteTag(int i) {
    switch (i) {
        case kPullKick:
            return "pullKick";
        case kPullSend:
            return "pullSend";
        case kDensity:
            return "density";
        case kDead:
            return "deadPos";
        case kHub408:
            return "hub408";
        case kSched:
            return "sched";
        case kDispatch:
            return "dispatch";
        case kTick:
            return "tick";
        case kDensWrap:
            return "densWrap";
        case kDeadWrap:
            return "deadWrap";
        default:
            return "?";
    }
}

void Note(int site) {
    gHit[site].fetch_add(1, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    DWORD prev = gLastEdge[site].load(std::memory_order_relaxed);
    if (prev && now - prev < kEdgeLogMs) return;
    gLastEdge[site].store(now, std::memory_order_relaxed);
    x::runtime::LogI("MobInspect", "edge %s n=%u", SiteTag(site),
                     gHit[site].load(std::memory_order_relaxed));
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool WriteRelJmp5(void* at, void* to, size_t steal) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    const intptr_t rel =
        reinterpret_cast<uint8_t*>(to) - (reinterpret_cast<uint8_t*>(at) + 5);
    if (rel < static_cast<intptr_t>(INT32_MIN) || rel > static_cast<intptr_t>(INT32_MAX)) {
        return false;
    }
    p[0] = 0xE9;
    const auto r32 = static_cast<int32_t>(rel);
    memcpy(p + 1, &r32, 4);
    for (size_t i = 5; i < steal; ++i) p[i] = 0x90;
    return true;
}

void* AllocNear(void* target, size_t size) {
    if (!target || size == 0) return nullptr;
    constexpr uintptr_t kGran = 0x10000;
    constexpr uintptr_t kReach = 0x70000000ull;
    const uintptr_t t = reinterpret_cast<uintptr_t>(target) & ~(kGran - 1);
    MEMORY_BASIC_INFORMATION mbi{};
    for (int dir = 0; dir < 2; ++dir) {
        for (uintptr_t off = kGran; off < kReach; off += kGran) {
            uintptr_t p = 0;
            if (dir == 0) {
                p = t + off;
            } else {
                if (t < off) break;
                p = t - off;
            }
            if (p < 0x10000) continue;
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_FREE) continue;
            void* r = VirtualAlloc(reinterpret_cast<void*>(p), size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (!r) continue;
            const intptr_t rel =
                reinterpret_cast<uint8_t*>(r) - (reinterpret_cast<uint8_t*>(target) + 5);
            if (rel >= static_cast<intptr_t>(INT32_MIN) &&
                rel <= static_cast<intptr_t>(INT32_MAX)) {
                return r;
            }
            VirtualFree(r, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

enum ProbeEq : int { kReadFail = 0, kMismatch = 1, kMatch = 2 };

ProbeEq BytesEq(void* p, const uint8_t* sig, size_t n) {
    if (!p || !sig || n == 0) return kReadFail;
    __try {
        return memcmp(p, sig, n) == 0 ? kMatch : kMismatch;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kReadFail;
    }
}

void RemoveAll();

bool ProtectWrite(void* p, size_t n, DWORD* old) {
    return VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, old) != 0;
}

bool InstallAbs(AbsHookState& st, void* target, void* hook, const uint8_t* sig, size_t steal) {
    if (st.active) return true;
    if (!target || !hook || steal < 12 || steal > sizeof(st.saved)) return false;
    if (BytesEq(target, sig, steal) != kMatch) return false;
    void* tramp =
        VirtualAlloc(nullptr, steal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(st.saved, target, steal);
    memcpy(tramp, target, steal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + steal,
                reinterpret_cast<uint8_t*>(target) + steal);
    DWORD old = 0;
    if (!ProtectWrite(target, steal, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hook);
    for (size_t i = 12; i < steal; ++i) reinterpret_cast<uint8_t*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    VirtualProtect(target, steal, old, &old);
    st.target = target;
    st.trampoline = tramp;
    st.nearStub = nullptr;
    st.stolen = steal;
    st.active = true;
    return true;
}

bool InstallRel5(AbsHookState& st, void* target, void* hook, const uint8_t* sig, size_t steal) {
    if (st.active) return true;
    if (!target || !hook || steal < 5 || steal > sizeof(st.saved)) return false;
    if (BytesEq(target, sig, steal) != kMatch) return false;
    void* tramp =
        VirtualAlloc(nullptr, steal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    void* nearStub = AllocNear(target, 16);
    if (!nearStub) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    memcpy(st.saved, target, steal);
    memcpy(tramp, target, steal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + steal,
                reinterpret_cast<uint8_t*>(target) + steal);
    WriteAbsJmp(nearStub, hook);
    DWORD old = 0;
    if (!ProtectWrite(target, steal, &old)) {
        VirtualFree(nearStub, 0, MEM_RELEASE);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    if (!WriteRelJmp5(target, nearStub, steal)) {
        memcpy(target, st.saved, steal);
        FlushInstructionCache(GetCurrentProcess(), target, steal);
        VirtualProtect(target, steal, old, &old);
        VirtualFree(nearStub, 0, MEM_RELEASE);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    FlushInstructionCache(GetCurrentProcess(), nearStub, 16);
    VirtualProtect(target, steal, old, &old);
    st.target = target;
    st.trampoline = tramp;
    st.nearStub = nearStub;
    st.stolen = steal;
    st.active = true;
    return true;
}

void RemoveAbs(AbsHookState& st) {
    if (!st.active || !st.target) return;
    DWORD old = 0;
    if (ProtectWrite(st.target, st.stolen, &old)) {
        memcpy(st.target, st.saved, st.stolen);
        FlushInstructionCache(GetCurrentProcess(), st.target, st.stolen);
        VirtualProtect(st.target, st.stolen, old, &old);
    }
    if (st.trampoline) VirtualFree(st.trampoline, 0, MEM_RELEASE);
    if (st.nearStub) VirtualFree(st.nearStub, 0, MEM_RELEASE);
    st.trampoline = nullptr;
    st.nearStub = nullptr;
    st.target = nullptr;
    st.stolen = 0;
    st.active = false;
}

void __fastcall HookPullKick(void* a, void* b, void* c, void* d) {
    Note(kPullKick);
    gTramp[kPullKick](a, b, c, d);
}
void __fastcall HookPullSend(void* a, void* b, void* c, void* d) {
    Note(kPullSend);
    gTramp[kPullSend](a, b, c, d);
}
void __fastcall HookDensity(void* a, void* b, void* c, void* d) {
    Note(kDensity);
    gTramp[kDensity](a, b, c, d);
}
void __fastcall HookDead(void* a, void* b, void* c, void* d) {
    Note(kDead);
    gTramp[kDead](a, b, c, d);
}
void __fastcall HookHub408(void* a, void* b, void* c, void* d) {
    Note(kHub408);
    gTramp[kHub408](a, b, c, d);
}
void __fastcall HookSched(void* a, void* b, void* c, void* d) {
    Note(kSched);
    gTramp[kSched](a, b, c, d);
}
void __fastcall HookDispatch(void* a, void* b, void* c, void* d) {
    Note(kDispatch);
    gTramp[kDispatch](a, b, c, d);
}
void __fastcall HookTick(void* a, void* b, void* c, void* d) {
    Note(kTick);
    gTramp[kTick](a, b, c, d);
}
void __fastcall HookDensWrap(void* a, void* b, void* c, void* d) {
    Note(kDensWrap);
    gTramp[kDensWrap](a, b, c, d);
}
void __fastcall HookDeadWrap(void* a, void* b, void* c, void* d) {
    Note(kDeadWrap);
    gTramp[kDeadWrap](a, b, c, d);
}

bool SiteRequired(int site) { return site <= kHub408; }

bool InstallOne(int site, uint32_t rva, const uint8_t* sig, size_t steal, void* hook,
                HookKind kind) {
    void* t = AtRva<void*>(rva);
    const ProbeEq eq = BytesEq(t, sig, steal);
    const bool req = SiteRequired(site);
    if (eq == kReadFail) {
        x::runtime::LogW("MobInspect", "unread %s rva=0x%X", SiteTag(site), (unsigned)rva);
        return !req;
    }
    if (eq == kMismatch) {
        uint8_t b0 = 0;
        __try {
            if (t) b0 = *reinterpret_cast<uint8_t*>(t);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        x::runtime::LogW("MobInspect", "%s %s rva=0x%X b0=%02X", req ? "refuse" : "skip",
                         SiteTag(site), (unsigned)rva, b0);
        if (req) gRefuse.store(true, std::memory_order_relaxed);
        return !req;
    }
    const bool ok = (kind == kAbs12) ? InstallAbs(gAbs[site], t, hook, sig, steal)
                                     : InstallRel5(gAbs[site], t, hook, sig, steal);
    if (!ok) {
        x::runtime::LogW("MobInspect", "install fail %s kind=%d", SiteTag(site), (int)kind);
        return !req;
    }
    gTramp[site] = reinterpret_cast<FnFwd>(gAbs[site].trampoline);
    return true;
}

bool InstallAll() {
    if (gHooksOn.load(std::memory_order_relaxed)) return true;
    if (gRefuse.load(std::memory_order_relaxed)) return false;
    if (!x::runtime::il2cpp::GaBase()) return false;

    struct One {
        int site;
        uint32_t rva;
        const uint8_t* sig;
        size_t steal;
        void* hook;
        HookKind kind;
    };
    const One ones[] = {
        {kPullKick, kRvaPullKick, kSigPullKick, sizeof(kSigPullKick),
         reinterpret_cast<void*>(&HookPullKick), kAbs12},
        {kPullSend, kRvaPullSend, kSigPullSend, sizeof(kSigPullSend),
         reinterpret_cast<void*>(&HookPullSend), kAbs12},
        {kDensity, kRvaDensity, kSigDensity, sizeof(kSigDensity),
         reinterpret_cast<void*>(&HookDensity), kAbs12},
        {kDead, kRvaDeadPos, kSigDeadPos, sizeof(kSigDeadPos), reinterpret_cast<void*>(&HookDead),
         kAbs12},
        {kHub408, kRvaHub408, kSigHub408, sizeof(kSigHub408), reinterpret_cast<void*>(&HookHub408),
         kAbs12},
        {kSched, kRvaSched, kSigSched, sizeof(kSigSched), reinterpret_cast<void*>(&HookSched),
         kRel5},
        {kDispatch, kRvaDispatch, kSigDispatch, sizeof(kSigDispatch),
         reinterpret_cast<void*>(&HookDispatch), kRel5},
        {kTick, kRvaTick, kSigTick, sizeof(kSigTick), reinterpret_cast<void*>(&HookTick), kRel5},
        {kDensWrap, kRvaDensWrap, kSigDensWrap, sizeof(kSigDensWrap),
         reinterpret_cast<void*>(&HookDensWrap), kAbs12},
        {kDeadWrap, kRvaDeadWrap, kSigDeadWrap, sizeof(kSigDeadWrap),
         reinterpret_cast<void*>(&HookDeadWrap), kRel5},
    };

    int ok = 0;
    for (const One& o : ones) {
        if (SiteRequired(o.site)) {
            if (!InstallOne(o.site, o.rva, o.sig, o.steal, o.hook, o.kind)) {
                RemoveAll();
                return false;
            }
            ++ok;
        } else {
            if (InstallOne(o.site, o.rva, o.sig, o.steal, o.hook, o.kind) && gAbs[o.site].active) {
                ++ok;
            }
        }
        if (gRefuse.load(std::memory_order_relaxed)) {
            RemoveAll();
            return false;
        }
    }
    gHooksOn.store(true, std::memory_order_relaxed);
    x::runtime::LogI(
        "MobInspect",
        "hooks on n=%d pullKick=0x%X pullSend=0x%X dens=0x%X dead=0x%X hub=0x%X "
        "sched=0x%X disp=0x%X tick=0x%X densW=0x%X deadW=0x%X extra=%d%d%d%d%d",
        ok, (unsigned)kRvaPullKick, (unsigned)kRvaPullSend, (unsigned)kRvaDensity,
        (unsigned)kRvaDeadPos, (unsigned)kRvaHub408, (unsigned)kRvaSched, (unsigned)kRvaDispatch,
        (unsigned)kRvaTick, (unsigned)kRvaDensWrap, (unsigned)kRvaDeadWrap,
        gAbs[kSched].active ? 1 : 0, gAbs[kDispatch].active ? 1 : 0, gAbs[kTick].active ? 1 : 0,
        gAbs[kDensWrap].active ? 1 : 0, gAbs[kDeadWrap].active ? 1 : 0);
    return true;
}

void RemoveAll() {
    for (int i = 0; i < kSiteN; ++i) {
        RemoveAbs(gAbs[i]);
        gTramp[i] = nullptr;
    }
    gHooksOn.store(false, std::memory_order_relaxed);
}

int CallGetIntThunk(uint32_t rva) {
    void* p = AtRva<void*>(rva);
    if (!p) return kLvMiss;
    auto fn = reinterpret_cast<FnThunk>(p);
    intptr_t raw = kLvMiss;
    __try {
        raw = fn(kLvMiss, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobInspect.thunk");
        return kLvMiss;
    }
    const intptr_t mag = raw < 0 ? -raw : raw;
    if (mag <= 10000000) return static_cast<int>(raw);

    auto un = AtRva<FnUnwrap>(kRvaUnwrap);
    intptr_t v = kLvMiss;
    __try {
        v = un(nullptr, raw, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("MobInspect.unwrap");
        x::runtime::LogW("MobInspect", "unwrap except rva=0x%X raw=%p", (unsigned)rva,
                         reinterpret_cast<void*>(raw));
        return kLvMiss;
    }
    if (v > 10000000 || v < -10000000) {
        x::runtime::LogW("MobInspect", "lv-raw rva=0x%X raw=%p un=%p", (unsigned)rva,
                         reinterpret_cast<void*>(raw), reinterpret_cast<void*>(v));
        return kLvMiss;
    }
    return static_cast<int>(v);
}

void SnapshotOnPump(void*) {
    char buf[768];
    int n = snprintf(buf, sizeof(buf), "lv");
    for (const LvThunk& t : kLvThunks) {
        const int v = CallGetIntThunk(t.rva);
        if (n > 0 && static_cast<size_t>(n) + 24 < sizeof(buf)) {
            n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %d=%d", t.id, v);
        }
    }
    x::runtime::LogI("MobInspect", "%s", buf);
}

void Heartbeat() {
    x::runtime::LogI(
        "MobInspect",
        "hb sched=%u disp=%u tick=%u densW=%u deadW=%u pullKick=%u pullSend=%u "
        "density=%u deadPos=%u hub408=%u hooks=%d",
        gHit[kSched].load(std::memory_order_relaxed),
        gHit[kDispatch].load(std::memory_order_relaxed),
        gHit[kTick].load(std::memory_order_relaxed),
        gHit[kDensWrap].load(std::memory_order_relaxed),
        gHit[kDeadWrap].load(std::memory_order_relaxed),
        gHit[kPullKick].load(std::memory_order_relaxed),
        gHit[kPullSend].load(std::memory_order_relaxed),
        gHit[kDensity].load(std::memory_order_relaxed),
        gHit[kDead].load(std::memory_order_relaxed),
        gHit[kHub408].load(std::memory_order_relaxed),
        gHooksOn.load(std::memory_order_relaxed) ? 1 : 0);
}

bool EnvOn() {
    char env[8]{};
    const DWORD n = GetEnvironmentVariableA("MOB_INSPECT_PROBE", env, sizeof(env));
    return n > 0 && env[0] == '1';
}

DWORD WINAPI Worker(LPVOID) {
    DWORD lastHb = 0;
    DWORD lastSnap = 0;
    bool snapOnce = false;
    while (!gStop.load(std::memory_order_relaxed)) {
        if (!gWant.load(std::memory_order_relaxed)) {
            Sleep(400);
            continue;
        }
        if (!gRefuse.load(std::memory_order_relaxed) && !gHooksOn.load(std::memory_order_relaxed)) {
            InstallAll();
        }
        const DWORD now = GetTickCount();
        if (!lastHb || now - lastHb >= kHbMs) {
            lastHb = now;
            Heartbeat();
        }
        if (!snapOnce || now - lastSnap >= kSnapMs) {
            if (x::runtime::main_thread::IsInstalled()) {
                lastSnap = now;
                snapOnce = true;
                x::runtime::main_thread::InvokeAndWait(&SnapshotOnPump, nullptr, 1500,
                                                       x::runtime::main_thread::JobPrio::Low);
            }
        }
        Sleep(200);
    }
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false, std::memory_order_relaxed);
    gRefuse.store(false, std::memory_order_relaxed);
    gWant.store(EnvOn(), std::memory_order_relaxed);
    if (!gWant.load(std::memory_order_relaxed)) {
        x::runtime::LogI("MobInspect", "disabled (set MOB_INSPECT_PROBE=1 to enable)");
        return;
    }
    x::runtime::LogI("MobInspect", "armed v2 (abs-jmp + rel5 parents + thunk GetInt)");
}

void StopWorker() {
    gStop.store(true, std::memory_order_relaxed);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 4000);
        CloseHandle(th);
    }
}

void Shutdown() {
    StopWorker();
    RemoveAll();
}

void StartWorker() {
    if (!gWant.load(std::memory_order_relaxed)) return;
    gStop.store(false, std::memory_order_relaxed);
    if (gWorker.load(std::memory_order_relaxed)) return;
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("MobInspect", "CreateThread fail");
        return;
    }
    gWorker.store(th, std::memory_order_release);
}

}  // namespace x::features::ports::mob_inspect_probe
