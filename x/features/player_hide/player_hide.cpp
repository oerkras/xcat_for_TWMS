// Classic TWMS — hide other players on the same map (client render only).
// 1) UserPool remotes → UserBase._avatarRoot → GameObject.SetActive(false)
// 2) MethodInfo swap（禁止 GA .text）：
//    - LocalUser.ShowSkill*：跳过非 UserLocal（远程攻击特效）
//    - Mob Slot14（damage tick）：调用前从 _damageInfo 列表剔除远程 CharacterId
//      （OnHit/ShowDamage/EffectHp 均为直接 call，MI 换钩捕不到；Slot14 仅 data xref→走 MI）
// Never touches UserLocal avatar. No GameAssembly .text patch.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "player_hide.h"

#include "../ports/user_pool_port.h"
#include "../ports/world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::player_hide {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::AtRva;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// —— AvatarRoot SetActive ——
constexpr size_t kFbAvatarRoot = 0x80;
constexpr char kFldAvatarRoot[] = "_avatarRoot";
constexpr uint32_t kRvaGoSetActive = 0x4E58B00;  // remount 2026-08-04

// —— 远程伤字 / 技能特效（dump remount 2026-08-04）——
constexpr uint32_t kRvaShowSkillEffect = 0xFE81B0;         // LocalUser
constexpr uint32_t kRvaShowSkillAffected = 0xFED6B0;       // LocalUser
constexpr uint32_t kRvaShowSkillPrepare = 0xFEDA90;        // LocalUser
constexpr uint32_t kRvaShowSkillSpecialEffect = 0xFEDF20;  // LocalUser
// Mob Slot14 damage-process tick（仅 MethodInfo 引用，无 code xref）
constexpr uint32_t kRvaMobDamageTick = 0xF0A520;
constexpr size_t kFbMobDamageInfoList = 0x1D8;             // Mob._damageInfo List<DamageInfo>
constexpr size_t kOffDamageInfoCharacterId = 0x14;         // DamageInfo.CharacterId

constexpr int kMaxUsers = 128;
constexpr int kMaxDamageInfos = 256;
constexpr DWORD kTickMsOn = 250;
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kJobWaitMs = 800;
constexpr DWORD kLogMs = 5000;
constexpr DWORD kFxInstallRetryMs = 3000;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

using FnGoSetActive = void (*)(void* self, bool value, const void* method);
using FnShowSkill5 = void (*)(void* self, void* a1, int a2, int a3, uint8_t a4, int a5,
                              const void* method);
using FnShowSkill2 = void (*)(void* self, void* a1, int a2, const void* method);
using FnShowSkillPrepare = void (*)(void* self, void* a1, int a2, uint8_t a3, uint8_t a4,
                                    const void* method);
using FnShowSkillSpecial = void (*)(void* self, void* a1, int a2, void* grenade,
                                    const void* method);
using FnMobDamageTick = void (*)(void* self, const void* method);

std::atomic<bool> gDesired{false};
std::atomic<bool> gWasOn{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<int> gLastHidden{0};
std::atomic<bool> gFxInstalled{false};

MethodInfoHead* gMiSetActive = nullptr;
bool gBindTried = false;
size_t gOffAvatarRoot = kFbAvatarRoot;
bool gAvatarOffTried = false;
DWORD gLastLog = 0;
DWORD gLastFxInstallTry = 0;

MethodInfoHead* gMiSkillEffect = nullptr;
MethodInfoHead* gMiSkillAffected = nullptr;
MethodInfoHead* gMiSkillPrepare = nullptr;
MethodInfoHead* gMiSkillSpecial = nullptr;
MethodInfoHead* gMiMobDamageTick = nullptr;

FnShowSkill5 gOrigSkillEffect = nullptr;
FnShowSkill2 gOrigSkillAffected = nullptr;
FnShowSkillPrepare gOrigSkillPrepare = nullptr;
FnShowSkillSpecial gOrigSkillSpecial = nullptr;
FnMobDamageTick gOrigMobDamageTick = nullptr;

uint32_t ReadU32(void* base, size_t off) {
    if (!base) return 0;
    uint32_t v = 0;
    __try {
        v = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return v;
}

int32_t ReadI32(void* base, size_t off) {
    return static_cast<int32_t>(ReadU32(base, off));
}

void WriteI32(void* base, size_t off, int32_t v) {
    if (!base) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(base) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WritePtrSlot(void* arr, uintptr_t i, void* v) {
    if (!arr) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) +
                                  x::runtime::il2cpp_container::OffArrayData() +
                                  i * sizeof(void*)) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool IsRemoteUser(void* user) {
    if (!LooksLikeHeapPtr(user)) return false;
    void* local = ports::user_pool::PeekUserLocal();
    if (!local) return true;  // 未绑本地时宁可多藏特效
    return user != local;
}

// 从 Mob._damageInfo 剔除远程玩家条目，避免 Slot14→OnHit→ShowDamage→EffectHp 画字。
int FilterRemoteDamageInfos(void* mob) {
    if (!LooksLikeHeapPtr(mob)) return 0;
    const uint32_t localCid = ports::world::GetCharacterId();
    if (localCid == 0) return 0;

    x::runtime::il2cpp_container::Ensure();
    void* list = ReadPtr(mob, kFbMobDamageInfoList);
    if (!LooksLikeHeapPtr(list)) return 0;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    const int size = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    if (!LooksLikeHeapPtr(items) || size <= 0 || size > kMaxDamageInfos) return 0;

    int kept = 0;
    int dropped = 0;
    for (int i = 0; i < size; ++i) {
        void* di = ArrayAt(items, static_cast<uintptr_t>(i));
        bool keep = true;
        if (LooksLikeHeapPtr(di)) {
            const uint32_t cid = ReadU32(di, kOffDamageInfoCharacterId);
            if (cid != 0 && cid != localCid) {
                keep = false;
                ++dropped;
            }
        }
        if (keep) {
            if (kept != i) WritePtrSlot(items, static_cast<uintptr_t>(kept), di);
            ++kept;
        }
    }
    if (dropped > 0) {
        for (int i = kept; i < size; ++i) WritePtrSlot(items, static_cast<uintptr_t>(i), nullptr);
        WriteI32(list, x::runtime::il2cpp_container::OffListSize(), kept);
    }
    return dropped;
}

void EnsureAvatarRootOffset() {
    if (gAvatarOffTried) return;
    gAvatarOffTried = true;
    if (!x::runtime::il2cpp::Ensure()) return;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return;

    void* ub = x::runtime::il2cpp::FindClass("", "UserBase");
    if (!ub) ub = x::runtime::il2cpp::FindClass("Msc.Game.Object", "UserBase");
    if (!ub) {
        x::runtime::LogWThrottled(73, 15000, "PlayerHide",
                                  "UserBase klass miss — AvatarRoot fb=0x%zX", kFbAvatarRoot);
        return;
    }

    size_t off = 0;
    for (void* k = ub; k; ) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, kFldAvatarRoot);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (off) break;
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        k = parent;
    }
    if (off) {
        gOffAvatarRoot = off;
        x::runtime::LogI("PlayerHide", "AvatarRoot off=0x%zX path=meta", gOffAvatarRoot);
    } else {
        x::runtime::LogW("PlayerHide", "AvatarRoot field miss — fb=0x%zX", kFbAvatarRoot);
    }
}

bool BindSetActive() {
    if (gMiSetActive && gMiSetActive->methodPointer) return true;
    if (gBindTried && gMiSetActive) return gMiSetActive->methodPointer != nullptr;
    gBindTried = true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    EnsureAvatarRootOffset();

    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    if (!goKlass) {
        x::runtime::LogWThrottled(71, 15000, "PlayerHide", "GameObject klass miss");
        return false;
    }

    using namespace x::runtime::il2cpp_method;
    constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Bool}};
    const ResolveResult mr =
        FindMethodResolved(goKlass, kRvaGoSetActive, kSet, "SetActive", nullptr);
    if (!mr.method) {
        x::runtime::LogWThrottled(72, 15000, "PlayerHide", "SetActive MI miss path=%s",
                                  PathName(mr.path));
        return false;
    }
    gMiSetActive = reinterpret_cast<MethodInfoHead*>(mr.method);
    x::runtime::LogI("PlayerHide", "SetActive bound mi=%p mp=%p path=%s", gMiSetActive,
                     gMiSetActive->methodPointer, PathName(mr.path));
    return gMiSetActive->methodPointer != nullptr;
}

FnGoSetActive SetActiveFn() {
    if (gMiSetActive && gMiSetActive->methodPointer)
        return reinterpret_cast<FnGoSetActive>(gMiSetActive->methodPointer);
    return AtRva<FnGoSetActive>(kRvaGoSetActive);
}

void ApplyActive(void* go, bool active) {
    if (!LooksLikeHeapPtr(go)) return;
    auto* fn = SetActiveFn();
    if (!fn) return;
    __try {
        fn(go, active, gMiSetActive);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

MethodInfoHead* FindMiByRva(void* klass, uint32_t rva) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods || !e.ga) return nullptr;
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
            if (reinterpret_cast<uintptr_t>(mp) == want) return mi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
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
    if (orig == hook) {
        return *outOrig != nullptr;
    }
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

void Hook_ShowSkillEffect(void* self, void* a1, int a2, int a3, uint8_t a4, int a5,
                          const void* method) {
    if (gDesired.load(std::memory_order_relaxed) && IsRemoteUser(self)) return;
    if (gOrigSkillEffect) gOrigSkillEffect(self, a1, a2, a3, a4, a5, method);
}

void Hook_ShowSkillAffected(void* self, void* a1, int a2, const void* method) {
    if (gDesired.load(std::memory_order_relaxed) && IsRemoteUser(self)) return;
    if (gOrigSkillAffected) gOrigSkillAffected(self, a1, a2, method);
}

void Hook_ShowSkillPrepare(void* self, void* a1, int a2, uint8_t a3, uint8_t a4,
                           const void* method) {
    if (gDesired.load(std::memory_order_relaxed) && IsRemoteUser(self)) return;
    if (gOrigSkillPrepare) gOrigSkillPrepare(self, a1, a2, a3, a4, method);
}

void Hook_ShowSkillSpecial(void* self, void* a1, int a2, void* grenade, const void* method) {
    if (gDesired.load(std::memory_order_relaxed) && IsRemoteUser(self)) return;
    if (gOrigSkillSpecial) gOrigSkillSpecial(self, a1, a2, grenade, method);
}

void Hook_MobDamageTick(void* self, const void* method) {
    if (gDesired.load(std::memory_order_relaxed)) {
        const int n = FilterRemoteDamageInfos(self);
        if (n > 0) {
            x::runtime::LogWThrottled(77, 5000, "PlayerHide", "drop remote dmgInfo n=%d", n);
        }
    }
    if (gOrigMobDamageTick) gOrigMobDamageTick(self, method);
}

void RestoreAllFxHooks() {
    RestoreMi(gMiSkillEffect, reinterpret_cast<void*>(gOrigSkillEffect));
    RestoreMi(gMiSkillAffected, reinterpret_cast<void*>(gOrigSkillAffected));
    RestoreMi(gMiSkillPrepare, reinterpret_cast<void*>(gOrigSkillPrepare));
    RestoreMi(gMiSkillSpecial, reinterpret_cast<void*>(gOrigSkillSpecial));
    RestoreMi(gMiMobDamageTick, reinterpret_cast<void*>(gOrigMobDamageTick));
    gOrigSkillEffect = nullptr;
    gOrigSkillAffected = nullptr;
    gOrigSkillPrepare = nullptr;
    gOrigSkillSpecial = nullptr;
    gOrigMobDamageTick = nullptr;
}

void UninstallFxHooks() {
    const bool had = gFxInstalled.load(std::memory_order_relaxed) || gOrigMobDamageTick ||
                     gOrigSkillEffect || gOrigSkillAffected || gOrigSkillPrepare ||
                     gOrigSkillSpecial;
    if (!had) return;
    RestoreAllFxHooks();
    gFxInstalled.store(false, std::memory_order_relaxed);
    x::runtime::LogI("PlayerHide", "fx hooks uninstalled");
}

bool InstallFxHooks() {
    if (gFxInstalled.load(std::memory_order_relaxed)) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;

    void* luKlass = x::runtime::il2cpp::FindClass("", "LocalUser");
    if (!luKlass) luKlass = x::runtime::il2cpp::FindClass("Msc.Game.Object", "LocalUser");
    void* mobKlass = x::runtime::il2cpp::FindClass("", "Mob");
    if (!mobKlass) mobKlass = x::runtime::il2cpp::FindClass("Msc.Game.Object", "Mob");

    if (!luKlass || !mobKlass) {
        x::runtime::LogWThrottled(75, 10000, "PlayerHide", "fx klass miss lu=%p mob=%p", luKlass,
                                  mobKlass);
        return false;
    }

    gMiSkillEffect = FindMiByRva(luKlass, kRvaShowSkillEffect);
    gMiSkillAffected = FindMiByRva(luKlass, kRvaShowSkillAffected);
    gMiSkillPrepare = FindMiByRva(luKlass, kRvaShowSkillPrepare);
    gMiSkillSpecial = FindMiByRva(luKlass, kRvaShowSkillSpecialEffect);
    gMiMobDamageTick = FindMiByRva(mobKlass, kRvaMobDamageTick);

    int hits = 0;
    void* orig = nullptr;
    if (gMiSkillEffect &&
        PatchMi(gMiSkillEffect, reinterpret_cast<void*>(&Hook_ShowSkillEffect), &orig)) {
        gOrigSkillEffect = reinterpret_cast<FnShowSkill5>(orig);
        ++hits;
    }
    orig = nullptr;
    if (gMiSkillAffected &&
        PatchMi(gMiSkillAffected, reinterpret_cast<void*>(&Hook_ShowSkillAffected), &orig)) {
        gOrigSkillAffected = reinterpret_cast<FnShowSkill2>(orig);
        ++hits;
    }
    orig = nullptr;
    if (gMiSkillPrepare &&
        PatchMi(gMiSkillPrepare, reinterpret_cast<void*>(&Hook_ShowSkillPrepare), &orig)) {
        gOrigSkillPrepare = reinterpret_cast<FnShowSkillPrepare>(orig);
        ++hits;
    }
    orig = nullptr;
    if (gMiSkillSpecial &&
        PatchMi(gMiSkillSpecial, reinterpret_cast<void*>(&Hook_ShowSkillSpecial), &orig)) {
        gOrigSkillSpecial = reinterpret_cast<FnShowSkillSpecial>(orig);
        ++hits;
    }
    orig = nullptr;
    if (gMiMobDamageTick &&
        PatchMi(gMiMobDamageTick, reinterpret_cast<void*>(&Hook_MobDamageTick), &orig)) {
        gOrigMobDamageTick = reinterpret_cast<FnMobDamageTick>(orig);
        ++hits;
    }

    // 伤字靠 Slot14；技能靠 ShowSkill*。至少一路成功。
    const bool ok = gOrigMobDamageTick || gOrigSkillEffect;
    if (!ok) {
        RestoreAllFxHooks();
        x::runtime::LogWThrottled(76, 10000, "PlayerHide", "fx install fail hits=%d/5", hits);
        return false;
    }
    gFxInstalled.store(true, std::memory_order_relaxed);
    x::runtime::LogI("PlayerHide",
                     "fx hooks ok hits=%d/5 skill=%d dmgTick=%d", hits,
                     gOrigSkillEffect ? 1 : 0, gOrigMobDamageTick ? 1 : 0);
    return true;
}

void EnsureFxHooks(DWORD now) {
    if (!gDesired.load(std::memory_order_relaxed)) {
        UninstallFxHooks();
        return;
    }
    if (gFxInstalled.load(std::memory_order_relaxed)) return;
    if (gLastFxInstallTry && now - gLastFxInstallTry < kFxInstallRetryMs) return;
    gLastFxInstallTry = now;
    (void)InstallFxHooks();
}

struct ApplyJob {
    bool hide = true;
    bool avatar = true;  // false = 只装卸 FX（进图前也可）
    int touched = 0;
    int remote = 0;
    bool ok = false;
};

void ApplyOnMain(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("player_hide.Apply");
    auto* job = reinterpret_cast<ApplyJob*>(user);
    if (!job) return;
    job->ok = false;
    job->touched = 0;
    job->remote = 0;

    // MethodInfo 装卸只在泵线程。
    EnsureFxHooks(GetTickCount());
    if (!job->avatar) {
        job->ok = true;
        return;
    }
    if (!BindSetActive()) return;

    void* users[kMaxUsers]{};
    int n = 0;
    if (!ports::user_pool::EnumRemoteUsers(users, kMaxUsers, &n)) return;
    job->remote = n;

    EnsureAvatarRootOffset();
    void* local = ports::user_pool::PeekUserLocal();
    for (int i = 0; i < n; ++i) {
        void* u = users[i];
        if (!LooksLikeHeapPtr(u) || u == local) continue;
        void* go = ReadPtr(u, gOffAvatarRoot);
        if (!LooksLikeHeapPtr(go)) continue;
        ApplyActive(go, !job->hide);
        ++job->touched;
    }
    job->ok = true;
}

bool InvokeApply(bool hide, bool avatar, int* outTouched, int* outRemote) {
    if (outTouched) *outTouched = 0;
    if (outRemote) *outRemote = 0;
    ApplyJob job{};
    job.hide = hide;
    job.avatar = avatar;
    if (!x::runtime::main_thread::Ensure()) return false;
    if (!x::runtime::main_thread::InvokeAndWait(&ApplyOnMain, &job, kJobWaitMs)) return false;
    if (outTouched) *outTouched = job.touched;
    if (outRemote) *outRemote = job.remote;
    return job.ok;
}

void TickOnce(DWORD now) {
    const bool want = gDesired.load(std::memory_order_relaxed);
    const bool was = gWasOn.load(std::memory_order_relaxed);

    if (!want) {
        if (gFxInstalled.load(std::memory_order_relaxed)) {
            (void)InvokeApply(/*hide=*/false, /*avatar=*/false, nullptr, nullptr);
        }
        if (was) {
            int touched = 0, remote = 0;
            if (!InvokeApply(/*hide=*/false, /*avatar=*/true, &touched, &remote)) {
                x::runtime::LogWThrottled(74, 3000, "PlayerHide", "restore pending (invoke fail)");
                return;
            }
            gWasOn.store(false, std::memory_order_relaxed);
            gLastHidden.store(0, std::memory_order_relaxed);
            x::runtime::LogI("PlayerHide", "restore touched=%d remote=%d", touched, remote);
        }
        return;
    }

    // 关图外也可装钩，避免进图第一刀漏特效/伤字；Avatar 仍要求 PlayReady。
    (void)InvokeApply(/*hide=*/true, /*avatar=*/false, nullptr, nullptr);

    if (!ports::world::IsPlayReady()) return;
    const auto ss = ports::world::GetSceneState();
    if (ss == ports::world::SceneState::CashShop || ss == ports::world::SceneState::Login)
        return;

    int touched = 0, remote = 0;
    if (!InvokeApply(/*hide=*/true, /*avatar=*/true, &touched, &remote)) return;
    gWasOn.store(true, std::memory_order_relaxed);
    gLastHidden.store(touched, std::memory_order_relaxed);

    if (gLastLog == 0 || now - gLastLog >= kLogMs) {
        gLastLog = now;
        x::runtime::LogI("PlayerHide", "hide touched=%d remote=%d fx=%d", touched, remote,
                         gFxInstalled.load() ? 1 : 0);
    }
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("PlayerHide", "worker start");
    while (!gStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        TickOnce(now);
        const bool on = gDesired.load(std::memory_order_relaxed) ||
                        gWasOn.load(std::memory_order_relaxed);
        Sleep(on ? kTickMsOn : kTickMsOff);
    }
    if (gFxInstalled.load(std::memory_order_relaxed)) {
        (void)InvokeApply(/*hide=*/false, /*avatar=*/false, nullptr, nullptr);
    }
    if (gWasOn.load(std::memory_order_relaxed)) {
        int touched = 0, remote = 0;
        for (int i = 0; i < 3; ++i) {
            if (InvokeApply(/*hide=*/false, /*avatar=*/true, &touched, &remote)) break;
            Sleep(50);
        }
        gWasOn.store(false, std::memory_order_relaxed);
        gLastHidden.store(0, std::memory_order_relaxed);
        x::runtime::LogI("PlayerHide", "worker stop restore touched=%d", touched);
    }
    x::runtime::LogI("PlayerHide", "worker exit");
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false);
    gDesired.store(false);
    gWasOn.store(false);
    gLastHidden.store(0);
    gBindTried = false;
    gMiSetActive = nullptr;
    gAvatarOffTried = false;
    gOffAvatarRoot = kFbAvatarRoot;
    gLastLog = 0;
    gLastFxInstallTry = 0;
    gFxInstalled.store(false);
    gOrigSkillEffect = nullptr;
    gOrigSkillAffected = nullptr;
    gOrigSkillPrepare = nullptr;
    gOrigSkillSpecial = nullptr;
    gOrigMobDamageTick = nullptr;
    x::runtime::LogI("PlayerHide",
                     "Init (AvatarRoot + Slot14 dmgFilter + remote SkillFX)");
}

void Shutdown() { StopWorker(); }

void SetEnabled(bool on) {
    gDesired.store(on, std::memory_order_relaxed);
    // 装卸走 worker→泵线程，避免非主线程竞态 MethodInfo。
}

bool IsEnabled() { return gDesired.load(std::memory_order_relaxed); }

int LastHiddenCount() { return gLastHidden.load(std::memory_order_relaxed); }

void Tick(DWORD now) { TickOnce(now); }

void StartWorker() {
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("PlayerHide", "CreateThread failed");
        return;
    }
    gWorker.store(th, std::memory_order_release);
}

void StopWorker() {
    gStop.store(true, std::memory_order_release);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

}  // namespace x::features::player_hide
