// Classic TWMS — pointblank_shoot：不挥弓（贴身仍射箭）。
//
// 三层（均按弓/弩门控；职业兜底）：
// 1) CED7E0 → false：主动技贴身早闸留在射箭分支
// 2) TryDoingShootAttack → isMortalBlow=1：Melee 内探测/MB 路径
// 3) TryDoingMeleeAttack 入口改道 → 只调 Shoot(MB=1)，禁止落入挥弓 Encode(50)
//
// BIN 2026-08-09：钩已 arm 仍挥弓 → 门控未命中或 Melee 失败落点仍 bonk。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pointblank_shoot.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::pointblank_shoot {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaTryDoingShootAttack = 0x103D2A0;
constexpr uint32_t kRvaCedGate = 0x106D7E0;
constexpr uint32_t kRvaTryDoingMeleeAttack = 0x10B0BB0;
constexpr uint32_t kRvaGetWeaponType = 0x1418B10;

// Shoot：55 41 57 41 56 41 55 41 54 56 57 53
constexpr uint8_t kShootSig[12] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41,
                                   0x55, 0x41, 0x54, 0x56, 0x57, 0x53};
constexpr size_t kShootSteal = 12;

// CED：41 57 41 56 56 57 53 48 81 EC 70 02 00 00
constexpr uint8_t kCedSig[14] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
                                 0x81, 0xEC, 0x70, 0x02, 0x00, 0x00};
constexpr size_t kCedSteal = 14;

// Melee：41 57 41 56 41 55 41 54 56 57 55 53（12B push，abs-jmp 对齐）
constexpr uint8_t kMeleeSig[12] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                   0x41, 0x54, 0x56, 0x57, 0x55, 0x53};
constexpr size_t kMeleeSteal = 12;

constexpr int kWtBow = 45;
constexpr int kWtCrossBow = 46;
constexpr int kBodyPartWeapon = 11;
constexpr int kInvTiEquip = 1;
constexpr size_t kFbCdEquipped = 0x28;
constexpr size_t kFbCdEquipped2 = 0x30;
constexpr DWORD kWeaponCacheMs = 400;
constexpr DWORD kHeartMs = 5000;

using FnShoot = uint8_t(__fastcall*)(void* self, void* skill, int32_t skillLevel, void* shootRange,
                                     int32_t isMortalBlow, int32_t timeKeyDown, uint32_t randMb,
                                     void* methodInfo);
using FnCed = uint8_t(__fastcall*)(void* self, void* methodInfo);
using FnMelee = uint8_t(__fastcall*)(void* self, void* skill, int32_t skillLevel, void* shootRange,
                                     int32_t serialSkill, int32_t lastMob, int32_t timeKeyDown,
                                     void* grenade, void* methodInfo);
using FnGetWeaponType = int(__fastcall*)(int itemId, void* methodInfo);

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

std::atomic<bool> gWant{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
AbsHookState gShoot{};
AbsHookState gCed{};
AbsHookState gMelee{};
FnShoot gShootTramp = nullptr;
FnCed gCedTramp = nullptr;
FnMelee gMeleeTramp = nullptr;
std::atomic<uint32_t> gForceMbHits{0};
std::atomic<uint32_t> gForceCedHits{0};
std::atomic<uint32_t> gMeleeRedirectHits{0};
std::atomic<uint32_t> gPassHits{0};
std::atomic<bool> gShootRefuse{false};
std::atomic<bool> gCedRefuse{false};
std::atomic<bool> gMeleeRefuse{false};
std::atomic<int> gLastWt{0};
std::atomic<int> gLastJob{0};
bool gWeSetPatchEnv = false;

int ReadI32(void* obj, size_t off) {
    int v = 0;
    __try {
        v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

void* ArrayAtPtr(void* arr, int index) {
    if (!LooksLikeHeapPtr(arr) || index < 0) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    int len = 0;
    __try {
        len = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(arr) + offLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (index >= len) return nullptr;
    void* elem = nullptr;
    __try {
        elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                         static_cast<size_t>(index) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(elem) ? elem : nullptr;
}

void* ListAt(void* list, int index) {
    if (!LooksLikeHeapPtr(list) || index < 0) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const size_t offItems = x::runtime::il2cpp_container::OffListItems();
    const size_t offSize = x::runtime::il2cpp_container::OffListSize();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    void* items = nullptr;
    int size = 0;
    __try {
        items = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(list) + offItems);
        size = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(list) + offSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!LooksLikeHeapPtr(items) || index >= size) return nullptr;
    void* elem = nullptr;
    __try {
        elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + offData +
                                         static_cast<size_t>(index) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(elem) ? elem : nullptr;
}

int WeaponTypeFromItemId(int itemId) {
    if (itemId < 1000000) return 0;
    const int t = (itemId / 10000) % 100;
    if (t >= 30 && t <= 49) return t;
    return 0;
}

int CallGetWeaponType(int itemId) {
    if (itemId <= 0) return 0;
    auto fn = x::runtime::il2cpp::AtRva<FnGetWeaponType>(kRvaGetWeaponType);
    if (!fn) return 0;
    int wt = 0;
    __try {
        wt = fn(itemId, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wt = 0;
    }
    if (wt >= 30 && wt <= 49) return wt;
    return 0;
}

int ResolveWeaponType(int itemId) {
    const int fromId = WeaponTypeFromItemId(itemId);
    if (fromId) return fromId;
    return CallGetWeaponType(itemId);
}

int SlotItemId(void* slot) {
    if (!LooksLikeHeapPtr(slot)) return 0;
    const size_t offId = x::ui::player::OffSlotItemId();
    if (!offId) return 0;
    return ReadI32(slot, offId);
}

int TryWtFromSlot(void* slot) {
    const int id = SlotItemId(slot);
    if (id <= 0) return 0;
    return ResolveWeaponType(id);
}

int ReadEquippedWeaponType() {
    void* cd = x::ui::player::LocalCharacterData();
    if (LooksLikeHeapPtr(cd)) {
        const size_t eqOffs[2] = {kFbCdEquipped, kFbCdEquipped2};
        for (int ei = 0; ei < 2; ++ei) {
            void* arr = ReadPtr(cd, eqOffs[ei]);
            if (!LooksLikeHeapPtr(arr)) continue;
            const int wt = TryWtFromSlot(ArrayAtPtr(arr, kBodyPartWeapon));
            if (wt) return wt;
        }
    }
    void* list = x::ui::player::GetItemSlotList(kInvTiEquip);
    if (!LooksLikeHeapPtr(list)) return 0;
    const int preferIdx[2] = {kBodyPartWeapon, kBodyPartWeapon + 1};
    for (int i = 0; i < 2; ++i) {
        const int wt = TryWtFromSlot(ListAt(list, preferIdx[i]));
        if (wt) return wt;
    }
    x::runtime::il2cpp_container::Ensure();
    const int size = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    const int n = size > 96 ? 96 : size;
    for (int i = 0; i < n; ++i) {
        const int wt = TryWtFromSlot(ListAt(list, i));
        if (wt == kWtBow || wt == kWtCrossBow) return wt;
    }
    return 0;
}

bool IsArcherJob(int job) {
    if (job <= 0) return false;
    // 冒险家弓系 300–322；狂狼/夜行者外的弓系：风灵使者 1300–1312；狂豹猎人 3300–3312
    if (job >= 300 && job < 400) return true;
    if (job >= 1300 && job < 1400) return true;
    if (job >= 3300 && job < 3400) return true;
    return false;
}

// 弓/弩才强制；wt 未知时用职业兜底。已知近战武器绝不强制。
bool ShouldForceNoBonk() {
    if (!gWant.load(std::memory_order_relaxed)) return false;

    static DWORD sLastMs = 0;
    static int sWt = 0;
    static int sJob = 0;
    static bool sForce = false;
    const DWORD now = GetTickCount();
    if (sLastMs && (now - sLastMs) < kWeaponCacheMs) return sForce;

    int wt = 0;
    int job = 0;
    __try {
        wt = ReadEquippedWeaponType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wt = 0;
    }
    x::ui::player::Vitals v{};
    if (x::ui::player::Read(v) && v.ok) job = v.job;

    bool force = false;
    if (wt == kWtBow || wt == kWtCrossBow) {
        force = true;
    } else if (wt >= 30 && wt <= 49) {
        force = false;  // 明确其它武器
    } else {
        force = IsArcherJob(job);  // wt 未知 → 职业兜底
    }

    sWt = wt;
    sJob = job;
    sForce = force;
    sLastMs = now;
    gLastWt.store(wt, std::memory_order_relaxed);
    gLastJob.store(job, std::memory_order_relaxed);
    return force;
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
    for (size_t i = 0; i < n; ++i) {
        if (reinterpret_cast<uint8_t*>(target)[i] != sig[i]) return false;
    }
    return true;
}

bool InstallAbs(AbsHookState& st, void* target, void* hook, const uint8_t* sig, size_t steal) {
    if (st.active) return true;
    if (!target || !hook || steal < 12 || steal > sizeof(st.saved)) return false;
    if (!SigMatch(target, sig, steal)) return false;
    void* tramp = VirtualAlloc(nullptr, steal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(st.saved, target, steal);
    memcpy(tramp, target, steal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + steal,
                reinterpret_cast<uint8_t*>(target) + steal);
    DWORD old = 0;
    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hook);
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    VirtualProtect(target, steal, old, &old);
    st.target = target;
    st.trampoline = tramp;
    st.stolen = steal;
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
    if (st.trampoline) VirtualFree(st.trampoline, 0, MEM_RELEASE);
    st.trampoline = nullptr;
    st.target = nullptr;
    st.stolen = 0;
    st.active = false;
}

uint8_t __fastcall HookShoot(void* self, void* skill, int32_t skillLevel, void* shootRange,
                             int32_t isMortalBlow, int32_t timeKeyDown, uint32_t randMb,
                             void* methodInfo) {
    const FnShoot o = gShootTramp;
    if (!o) return 0;
    if (ShouldForceNoBonk()) {
        if (!isMortalBlow) gForceMbHits.fetch_add(1, std::memory_order_relaxed);
        isMortalBlow = 1;
    } else {
        gPassHits.fetch_add(1, std::memory_order_relaxed);
    }
    __try {
        return o(self, skill, skillLevel, shootRange, isMortalBlow, timeKeyDown, randMb,
                 methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t __fastcall HookCed(void* self, void* methodInfo) {
    if (ShouldForceNoBonk()) {
        gForceCedHits.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const FnCed o = gCedTramp;
    if (!o) return 0;
    __try {
        return o(self, methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 弓/弩：禁止进 Melee 挥弓落点，改只走 Shoot(MB=1)。
uint8_t __fastcall HookMelee(void* self, void* skill, int32_t skillLevel, void* shootRange,
                             int32_t serialSkill, int32_t lastMob, int32_t timeKeyDown,
                             void* grenade, void* methodInfo) {
    if (ShouldForceNoBonk()) {
        const FnShoot shoot = gShootTramp;
        if (shoot) {
            gMeleeRedirectHits.fetch_add(1, std::memory_order_relaxed);
            __try {
                return shoot(self, skill, skillLevel, shootRange, /*isMortalBlow=*/1, timeKeyDown,
                             0, methodInfo);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }
        // Shoot 钩未就绪时仍拒绝落入原 Melee（宁可空挥也不 bonk）
        return 0;
    }
    const FnMelee o = gMeleeTramp;
    if (!o) return 0;
    __try {
        return o(self, skill, skillLevel, shootRange, serialSkill, lastMob, timeKeyDown, grenade,
                 methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void ReportLamp(x::runtime::anchor_lamps::AnchorLampCode code, const char* detail) {
    x::runtime::anchor_lamps::Set("PbShoot", code, detail);
}

bool TryArmOne(AbsHookState& st, std::atomic<bool>& refuse, uint32_t rva, const uint8_t* sig,
               size_t steal, void* hook, const char* name, void** outTramp) {
    if (st.active || refuse.load(std::memory_order_relaxed)) return st.active;
    void* target = x::runtime::il2cpp::AtRva<void*>(rva);
    if (!SigMatch(target, sig, steal)) {
        refuse.store(true, std::memory_order_relaxed);
        x::runtime::LogW("PbShoot", "%s refuse: sig mismatch @%p b0=%02X", name, target,
                         reinterpret_cast<uint8_t*>(target)[0]);
        return false;
    }
    if (!InstallAbs(st, target, hook, sig, steal)) return false;
    if (outTramp) *outTramp = st.trampoline;
    x::runtime::LogI("PbShoot", "%s arm=1 target=%p rva=0x%X", name, target, (unsigned)rva);
    return true;
}

void PumpApply(void*) {
    const bool want = gWant.load(std::memory_order_acquire);
    if (want) {
        if (!x::runtime::il2cpp::Ensure()) {
            ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "no-il2cpp");
            return;
        }
        void* shootTramp = gShootTramp;
        void* cedTramp = gCedTramp;
        void* meleeTramp = gMeleeTramp;
        TryArmOne(gShoot, gShootRefuse, kRvaTryDoingShootAttack, kShootSig, kShootSteal,
                  reinterpret_cast<void*>(&HookShoot), "Shoot", &shootTramp);
        gShootTramp = reinterpret_cast<FnShoot>(shootTramp);
        TryArmOne(gCed, gCedRefuse, kRvaCedGate, kCedSig, kCedSteal,
                  reinterpret_cast<void*>(&HookCed), "CED", &cedTramp);
        gCedTramp = reinterpret_cast<FnCed>(cedTramp);
        TryArmOne(gMelee, gMeleeRefuse, kRvaTryDoingMeleeAttack, kMeleeSig, kMeleeSteal,
                  reinterpret_cast<void*>(&HookMelee), "Melee", &meleeTramp);
        gMeleeTramp = reinterpret_cast<FnMelee>(meleeTramp);

        const bool ok = gShoot.active && gCed.active && gMelee.active;
        ReportLamp(ok ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                      : (gShoot.active || gCed.active || gMelee.active)
                            ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                            : x::runtime::anchor_lamps::AnchorLampCode::Miss,
                   ok ? "armed" : "partial");
        x::runtime::LogI("PbShoot", "arm shoot=%d ced=%d melee=%d wt=%d job=%d",
                         gShoot.active ? 1 : 0, gCed.active ? 1 : 0, gMelee.active ? 1 : 0,
                         gLastWt.load(std::memory_order_relaxed),
                         gLastJob.load(std::memory_order_relaxed));
    } else if (gShoot.active || gCed.active || gMelee.active) {
        RemoveAbs(gShoot);
        gShootTramp = nullptr;
        RemoveAbs(gCed);
        gCedTramp = nullptr;
        RemoveAbs(gMelee);
        gMeleeTramp = nullptr;
        x::runtime::LogI("PbShoot", "disarm mb=%u ced=%u meleeRedir=%u pass=%u",
                         gForceMbHits.load(std::memory_order_relaxed),
                         gForceCedHits.load(std::memory_order_relaxed),
                         gMeleeRedirectHits.load(std::memory_order_relaxed),
                         gPassHits.load(std::memory_order_relaxed));
        ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "off");
    }
}

bool EnsurePatchEnv(bool on) {
    if (on) {
        char env[8]{};
        const DWORD n = GetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", env, sizeof(env));
        const bool allowed = (n > 0 && env[0] == '1');
        if (!allowed) {
            if (!SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", "1")) {
                x::runtime::LogW("PbShoot", "无法设置 XCAT_ALLOW_TEXT_PATCH=1 err=%lu",
                                 GetLastError());
                return false;
            }
            gWeSetPatchEnv = true;
            x::runtime::LogI("PbShoot", "auto XCAT_ALLOW_TEXT_PATCH=1");
        }
        return true;
    }
    if (gWeSetPatchEnv) {
        SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", nullptr);
        gWeSetPatchEnv = false;
        x::runtime::LogI("PbShoot", "cleared XCAT_ALLOW_TEXT_PATCH");
    }
    return true;
}

void RequestApply() {
    if (!x::runtime::main_thread::WaitUntilInstalled(0)) return;
    (void)x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 3000,
                                                 x::runtime::main_thread::JobPrio::High);
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("PbShoot", "worker start");
    DWORD lastHeart = 0;
    while (!gStop.load(std::memory_order_acquire)) {
        const bool want = gWant.load(std::memory_order_acquire);
        const bool need =
            want && ((!gShoot.active && !gShootRefuse.load(std::memory_order_relaxed)) ||
                     (!gCed.active && !gCedRefuse.load(std::memory_order_relaxed)) ||
                     (!gMelee.active && !gMeleeRefuse.load(std::memory_order_relaxed)));
        if (need) RequestApply();
        const DWORD now = GetTickCount();
        if (want && (!lastHeart || now - lastHeart >= kHeartMs)) {
            lastHeart = now;
            (void)ShouldForceNoBonk();  // refresh wt/job cache
            x::runtime::LogI(
                "PbShoot",
                "heart want=1 shoot=%d ced=%d melee=%d force=%d wt=%d job=%d mb=%u cedH=%u "
                "redir=%u pass=%u",
                gShoot.active ? 1 : 0, gCed.active ? 1 : 0, gMelee.active ? 1 : 0,
                ShouldForceNoBonk() ? 1 : 0, gLastWt.load(std::memory_order_relaxed),
                gLastJob.load(std::memory_order_relaxed),
                gForceMbHits.load(std::memory_order_relaxed),
                gForceCedHits.load(std::memory_order_relaxed),
                gMeleeRedirectHits.load(std::memory_order_relaxed),
                gPassHits.load(std::memory_order_relaxed));
        }
        Sleep(want ? 1000 : 2000);
    }
    x::runtime::LogI("PbShoot", "worker exit");
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false, std::memory_order_release);
    gShootRefuse.store(false, std::memory_order_relaxed);
    gCedRefuse.store(false, std::memory_order_relaxed);
    gMeleeRefuse.store(false, std::memory_order_relaxed);
    ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "init");
}

void Shutdown() {
    StopWorker();
    gWant.store(false, std::memory_order_release);
    if (gShoot.active || gCed.active || gMelee.active) {
        if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
            x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 2000);
        else {
            RemoveAbs(gShoot);
            gShootTramp = nullptr;
            RemoveAbs(gCed);
            gCedTramp = nullptr;
            RemoveAbs(gMelee);
            gMeleeTramp = nullptr;
        }
    }
    EnsurePatchEnv(false);
}

void StartWorker() {
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("PbShoot", "CreateThread failed");
        return;
    }
    HANDLE prev = nullptr;
    if (!gWorker.compare_exchange_strong(prev, th, std::memory_order_acq_rel)) {
        CloseHandle(th);
    }
}

void StopWorker() {
    gStop.store(true, std::memory_order_release);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

void SetEnabled(bool on) {
    if (!EnsurePatchEnv(on)) {
        gWant.store(false, std::memory_order_release);
        return;
    }
    if (on) {
        gShootRefuse.store(false, std::memory_order_relaxed);
        gCedRefuse.store(false, std::memory_order_relaxed);
        gMeleeRefuse.store(false, std::memory_order_relaxed);
    }
    const bool prev = gWant.exchange(on, std::memory_order_acq_rel);
    const bool fullyOn = gShoot.active && gCed.active && gMelee.active;
    if (prev == on && ((on && fullyOn) || (!on && !gShoot.active && !gCed.active && !gMelee.active)))
        return;
    x::runtime::LogI("PbShoot", "want=%d shoot=%d ced=%d melee=%d", on ? 1 : 0,
                     gShoot.active ? 1 : 0, gCed.active ? 1 : 0, gMelee.active ? 1 : 0);
    RequestApply();
}

bool IsEnabled() { return gWant.load(std::memory_order_acquire); }
bool IsActive() { return gShoot.active || gCed.active || gMelee.active; }

}  // namespace x::features::pointblank_shoot
