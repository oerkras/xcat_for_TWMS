// Shared player vitals — Classic TWMS.
// 真源唯一：WorldManager→CharacterData→CharacterStat（WM+0xE8 BasicStat）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "player_vitals.h"

#include "../features/ports/world_port.h"
#include "../runtime/log.h"
#include "../runtime/managed_main.h"
#include "../runtime/il2cpp_bind.h"
#include "../runtime/il2cpp_shape.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace x::ui::player {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// UserLocal → il2cpp_shape::ResolveUserLocalKlass

constexpr size_t kOffWmMyUser = 0x28;
constexpr size_t kOffWmCharacterData = 0xE0;
constexpr size_t kOffWmBasicStat = 0xE8;
constexpr size_t kOffCdCharacterStat = 0x10;
constexpr size_t kOffCsName = 0x18;
constexpr size_t kOffCsLevel = 0x38;
constexpr size_t kOffCsJob = 0x3A;
constexpr size_t kOffCsHp = 0x44;
constexpr size_t kOffCsMhp = 0x46;
constexpr size_t kOffCsMp = 0x48;
constexpr size_t kOffCsMmp = 0x4A;
constexpr size_t kOffCsExp = 0x50;
constexpr size_t kOffCsMoney = 0x58;
constexpr size_t kOffCsNextLevel = 0x80;
constexpr size_t kOffBsNmhp = 0x30;
constexpr size_t kOffBsNmmp = 0x34;
constexpr size_t kOffNextArr = 0x10;
constexpr size_t kOffLuName = 0x1B8;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);

FnFindAll gFindAll = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
bool gHaveValid = false;

std::atomic<int> gReadyStreak{0};
std::atomic<bool> gReadyLatched{false};

int16_t ReadI16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int64_t ReadI64(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t ReadU8(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadIl2CppStringUtf8(void* str, char* out, size_t outSz) {
    if (!str || !out || outSz == 0) return false;
    out[0] = 0;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const wchar_t* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        const int n = WideCharToMultiByte(CP_UTF8, 0, chars, len, out, (int)outSz - 1, nullptr,
                                          nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BindFindAll() {
    if (gFindAll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    gFindAll = x::runtime::il2cpp::Get().findAll;
    return gFindAll != nullptr;
}

int ReadNextLevelExp(void* cs, int level) {
    void* next = ReadPtr(cs, kOffCsNextLevel);
    if (!next || level < 1) return 0;
    void* arr = ReadPtr(next, kOffNextArr);
    if (!arr) return 0;
    const uintptr_t n = ArrayLen(arr);
    if (n == 0 || (uintptr_t)level >= n) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                           (uintptr_t)level * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void TryResolveLocalUser() {
    // SSOT = WM.MyUser@+0x28。禁止「有缓存就永不重绑」——换图后旧指针会错乱。
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    void* myUser = LooksLikeHeapPtr(wm) ? ReadPtr(wm, kOffWmMyUser) : nullptr;
    if (LooksLikeHeapPtr(myUser)) {
        gLocalUser = myUser;
        return;
    }
    gLocalUser = nullptr;
    if (!x::features::ports::world::IsPlayReady()) return;
    if (!BindFindAll()) return;
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType || !gFindAll) return;
    void* luArr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 2000);
    const uintptr_t ln = ArrayLen(luArr);
    void* luBest = nullptr;
    for (uintptr_t i = 0; i < ln && i < 8; ++i) {
        void* lu = ArrayAt(luArr, i);
        if (!lu) continue;
        void* name = ReadPtr(lu, kOffLuName);
        char tmp[64]{};
        if (name && ReadIl2CppStringUtf8(name, tmp, sizeof(tmp))) {
            luBest = lu;
            break;
        }
        if (!luBest) luBest = lu;
    }
    gLocalUser = luBest;
}

void EnrichFromBasicStat(Vitals& out) {
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    void* bs = LooksLikeHeapPtr(wm) ? ReadPtr(wm, kOffWmBasicStat) : nullptr;
    if (!LooksLikeHeapPtr(bs)) return;
    out.mhp = (std::max)(out.mhp, ReadI32(bs, kOffBsNmhp));
    out.mmp = (std::max)(out.mmp, ReadI32(bs, kOffBsNmmp));
}

bool ReadFromCharacterStat(Vitals& out, void* cs) {
    out = {};
    if (!LooksLikeHeapPtr(cs)) return false;
    out.level = static_cast<int>(ReadU8(cs, kOffCsLevel));
    out.job = static_cast<int>(ReadI16(cs, kOffCsJob));
    out.hp = static_cast<int>(ReadI16(cs, kOffCsHp));
    out.mhp = static_cast<int>(ReadI16(cs, kOffCsMhp));
    out.mp = static_cast<int>(ReadI16(cs, kOffCsMp));
    out.mmp = static_cast<int>(ReadI16(cs, kOffCsMmp));
    out.exp = ReadI32(cs, kOffCsExp);
    out.meso = ReadI64(cs, kOffCsMoney);
    out.maxExp = ReadNextLevelExp(cs, out.level);

    EnrichFromBasicStat(out);

    void* nameObj = ReadPtr(cs, kOffCsName);
    if (!nameObj || !ReadIl2CppStringUtf8(nameObj, out.name, sizeof(out.name))) {
        TryResolveLocalUser();
        if (gLocalUser) {
            void* luName = ReadPtr(gLocalUser, kOffLuName);
            (void)ReadIl2CppStringUtf8(luName, out.name, sizeof(out.name));
        }
    }
    out.ok = out.level >= 1 && out.mhp > 0;
    return out.ok;
}

bool IsTrusted(const Vitals& v) {
    if (!v.ok) return false;
    if (v.hp == 0 && v.mp == 0) return false;
    if (v.hp <= 0 && v.mhp > 0) return false;  // dead
    if (v.mhp < 10) return false;
    return true;
}

}  // namespace

void Init() {
    gReadyStreak.store(0);
    gReadyLatched.store(false);
    gLocalUser = nullptr;
    gHaveValid = false;
    x::runtime::LogI("Vitals", "player_vitals ready (WM→CharacterStat)");
}

void Shutdown() {
    ClearBind();
    ClearReadyLatch();
}

void* LocalCharacterStat() {
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    void* cd = ReadPtr(wm, kOffWmCharacterData);
    void* cs = ReadPtr(cd, kOffCdCharacterStat);
    return LooksLikeHeapPtr(cs) ? cs : nullptr;
}

bool Read(Vitals& out) {
    out = {};
    void* cs = LocalCharacterStat();
    if (!ReadFromCharacterStat(out, cs)) {
        if (gHaveValid) {
            ClearBind();
            ClearReadyLatch();
        }
        return false;
    }
    gHaveValid = true;
    return true;
}

bool ResolveAndRead(Vitals& out, DWORD /*now*/, bool /*forceRebind*/) { return Read(out); }

bool ReadCached(Vitals& out) { return Read(out); }

void ClearBind() {
    gLocalUser = nullptr;
    gHaveValid = false;
}

bool HasBind() { return LocalCharacterStat() != nullptr; }

void NoteSample(const Vitals& v, DWORD /*now*/) {
    if (!IsTrusted(v)) {
        gReadyStreak.store(0);
        gReadyLatched.store(false);
        return;
    }
    const int streak = gReadyStreak.fetch_add(1) + 1;
    if (streak >= 3) gReadyLatched.store(true);
}

void ClearReadyLatch() {
    gReadyStreak.store(0);
    gReadyLatched.store(false);
}

bool IsReadyLatched() { return gReadyLatched.load(); }

int HpPct(const Vitals& v) {
    if (v.mhp <= 0) return -1;
    return (int)((int64_t)v.hp * 100 / v.mhp);
}

int MpPct(const Vitals& v) {
    if (v.mmp <= 0) return -1;
    return (int)((int64_t)v.mp * 100 / v.mmp);
}

bool IsDead(const Vitals& v) { return v.ok && v.mhp > 0 && v.hp <= 0; }

}  // namespace x::ui::player
