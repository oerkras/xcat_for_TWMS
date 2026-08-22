// Classic TWMS — WorldManager SSOT + scene gate (read-only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "world_port.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::features::ports::world {
namespace {

namespace il2 = x::runtime::il2cpp;

// WM klass：il2cpp_shape::ResolveWorldManagerKlass（hash acda742a… + shape 兜底）
// SceneState / Field / CharacterId / FieldKey：hash → field_get_offset（2026-08-06 remount）
// CharacterData / CharacterStat：SSOT = x::ui::player
constexpr char kWorldManagerClass[] =
    "f05b942aeb569b2c37916e7ee710b3ba74011550adcb611a0b26981331a8321";
constexpr char kHashWmSceneState[] =
    "d4bd05dbb2655f7dacb545dce0bdc8a51a13f9682ad4d65b5d187a7241598d6";
constexpr char kHashWmField[] =
    "c8849d22f0cbc0cc1e7f1ce767b0308a40ba75442d2649f305e941110da76c8";
constexpr char kHashWmCharacterId[] =
    "<f99d001e380c3ec741d1c660cad024f15a56ba67b0dd200f54a3918c47a65a9>k__BackingField";
// FieldKey 现挂在 WM 本体（byte@_fieldKey），不再走 SceneField+0x98
constexpr char kHashWmFieldKey[] =
    "e7c850e510712d210dbd4bd49f31bc2b3b21a069cb08ac04ecc53acbdb571e0";
// CharacterRegDate backing（08-20 dump；紧挨 QuestTimers@+0x240）
constexpr char kHashWmCharacterRegDate[] =
    "<a4dde0132cd8b2253290dee45468a3f949136e790a7c024b44d053ff293a6ce>k__BackingField";

constexpr size_t kFbWmSceneState = 0x34;
constexpr size_t kFbWmField = 0x58;
constexpr size_t kFbWmCharacterId = 0x90;  // was 0x98
constexpr size_t kFbWmFieldKey = 0x80;     // was Field+0x98
constexpr size_t kFbWmCharacterRegDate = 0x238;
size_t gOffWmSceneState = kFbWmSceneState;
size_t gOffWmField = kFbWmField;
size_t gOffWmCharacterId = kFbWmCharacterId;
size_t gOffFieldKey = kFbWmFieldKey;
size_t gOffWmCharacterRegDate = kFbWmCharacterRegDate;
#define kOffWmSceneState (gOffWmSceneState)
#define kOffWmField (gOffWmField)
#define kOffWmCharacterId (gOffWmCharacterId)
#define kOffFieldKey (gOffFieldKey)
#define kOffWmCharacterRegDate (gOffWmCharacterRegDate)
#define kOffWmMapData (x::runtime::il2cpp_mapdata::OffWmMapData())
#define kOffMapDataId (x::runtime::il2cpp_mapdata::OffMapId())
#define kOffWmCharacterData (x::ui::player::OffWmCharacterData())
#define kOffCdCharacterStat (x::ui::player::OffCdCharacterStat())
bool gWmFieldTried = false;

bool PlausibleWmOff(size_t off) { return off >= 0x20 && off < 0x200; }
bool PlausibleWmDateOff(size_t off) { return off >= 0x1C0 && off < 0x2C0; }

bool WmFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out,
                   bool (*plausible)(size_t) = PlausibleWmOff) {
    *out = fb;
    if (!klass || !hash || !il2::Ensure()) return false;
    const auto& e = il2::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, hash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (plausible(off)) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}

void EnsureWmFieldOff() {
    if (gWmFieldTried) return;
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { EnsureWmFieldOff(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return;
    }
    if (!il2::Ensure()) return;
    gWmFieldTried = true;
    void* wm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!wm) wm = il2::FindClass("", kWorldManagerClass);
    int hits = 0;
    if (WmFieldOffHit(wm, kHashWmSceneState, kFbWmSceneState, &gOffWmSceneState)) ++hits;
    if (WmFieldOffHit(wm, kHashWmField, kFbWmField, &gOffWmField)) ++hits;
    if (WmFieldOffHit(wm, kHashWmCharacterId, kFbWmCharacterId, &gOffWmCharacterId)) ++hits;
    if (WmFieldOffHit(wm, kHashWmFieldKey, kFbWmFieldKey, &gOffFieldKey)) ++hits;
    if (WmFieldOffHit(wm, kHashWmCharacterRegDate, kFbWmCharacterRegDate, &gOffWmCharacterRegDate,
                      PlausibleWmDateOff))
        ++hits;
    x::runtime::LogI("WorldPort",
                     "wm/field slots path=%s hits=%d/5 scene=0x%zX field=0x%zX charId=0x%zX "
                     "fkey=0x%zX regDate=0x%zX",
                     hits == 5 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                     gOffWmSceneState, gOffWmField, gOffWmCharacterId, gOffFieldKey,
                     gOffWmCharacterRegDate);
}

constexpr DWORD kWmRebindMs = 3000;

void* gWmTypeObj = nullptr;
void* gWorldManager = nullptr;
DWORD gLastWmBindMs = 0;
std::atomic<int> gLastLoggedScene{-999};
std::atomic<DWORD> gLastSceneLogMs{0};

uint32_t ReadU32(void* base, size_t off) {
    if (!base) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t ReadU8(void* base, size_t off) {
    if (!base) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int64_t ReadI64(void* base, size_t off) {
    if (!base) return 0;
    __try {
        return *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool WorldManagerLooksAlive(void* wm) {
    if (!il2::LooksLikeHeapPtr(wm)) return false;
    void* cd = il2::ReadPtr(wm, kOffWmCharacterData);
    if (!il2::LooksLikeHeapPtr(cd)) return false;
    void* cs = il2::ReadPtr(cd, kOffCdCharacterStat);
    return il2::LooksLikeHeapPtr(cs);
}

int ScoreWmCandidate(void* wm) {
    if (!il2::LooksLikeHeapPtr(wm)) return -1;
    void* cd = il2::ReadPtr(wm, kOffWmCharacterData);
    if (!il2::LooksLikeHeapPtr(cd)) return 0;
    void* cs = il2::ReadPtr(cd, kOffCdCharacterStat);
    if (il2::LooksLikeHeapPtr(cs)) return 3;
    if (il2::LooksLikeHeapPtr(il2::ReadPtr(wm, kOffWmMapData))) return 2;
    return 1;
}

bool ResolveWorldManager(bool force) {
    if (!force && WorldManagerLooksAlive(gWorldManager)) return true;

    const DWORD now = GetTickCount();
    // Login / char-select: WM often exists with score=0 (no CharacterData yet).
    // Do NOT null it + reset throttle — that caused FindAll every frame → beep/crash.
    if (!force && gWorldManager && !il2::LooksLikeHeapPtr(gWorldManager)) {
        gWorldManager = nullptr;
    }
    if (!force && gLastWmBindMs && (now - gLastWmBindMs < kWmRebindMs)) {
        return false;
    }

    gLastWmBindMs = now;
    if (!il2::Ensure()) return false;
    EnsureWmFieldOff();
    x::runtime::il2cpp_shape::LogResolveSelfCheck();
    // login-freeze 默认开：场景 SSOT 必须 bypass，否则 Titlebar 永远解不了冻 → 全端口死锁。
    if (!gWmTypeObj) {
        void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
        gWmTypeObj = il2::ClassTypeObject(wmKlass, true);
    }
    const auto& exp = il2::Get();
    if (!gWmTypeObj || !exp.findAll) return false;

    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(exp.findAll, gWmTypeObj, 2000, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uintptr_t n = il2::ArrayLen(arr);
    void* best = nullptr;
    int bestScore = -1;
    for (uintptr_t i = 0; i < n && i < 8; ++i) {
        void* wm = il2::ArrayAt(arr, i);
        const int sc = ScoreWmCandidate(wm);
        if (sc > bestScore) {
            bestScore = sc;
            best = wm;
            if (sc >= 3) break;
        }
    }
    const void* prev = gWorldManager;
    gWorldManager = best;
    const bool alive = WorldManagerLooksAlive(gWorldManager);
    if (gWorldManager && gWorldManager != prev) {
        x::runtime::LogI("WorldPort", "WM bind %p score=%d alive=%d (prev=%p)", gWorldManager,
                         bestScore, alive ? 1 : 0, prev);
    }
    return alive;
}

SceneState ReadSceneStateRaw(void* wm) {
    if (!il2::LooksLikeHeapPtr(wm)) return SceneState::Unknown;
    __try {
        const int v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wm) + kOffWmSceneState);
        if (v < 0 || v > 5) return SceneState::Unknown;
        return static_cast<SceneState>(v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SceneState::Unknown;
    }
}

void* gLastPublishedWm = nullptr;
int gLastPublishedInMap = -1;
DWORD gLastPublishMs = 0;

// Live hint for external tools (krw_hp_watch --auto). Overwritten when in-map + alive.
void PublishWmLive(bool inMap) {
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%sstate\\wm_live.txt", x::runtime::GetBinDir());
    const bool alive = inMap && WorldManagerLooksAlive(gWorldManager);
    const DWORD now = GetTickCount();
    if (!alive) {
        if (gLastPublishedInMap != 0) {
            DeleteFileA(path);
            gLastPublishedWm = nullptr;
            gLastPublishedInMap = 0;
        }
        return;
    }
    if (gWorldManager == gLastPublishedWm && gLastPublishedInMap == 1 && gLastPublishMs &&
        (now - gLastPublishMs < 2000)) {
        return;
    }
    gLastPublishedWm = gWorldManager;
    gLastPublishedInMap = 1;
    gLastPublishMs = now;
    char body[256]{};
    std::snprintf(body, sizeof(body),
                  "pid=%lu\n"
                  "wm=0x%p\n"
                  "inMap=1\n",
                  static_cast<unsigned long>(GetCurrentProcessId()), gWorldManager);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, body, static_cast<DWORD>(std::strlen(body)), &wr, nullptr);
    CloseHandle(h);
}

void MaybeLogScene(SceneState st, bool inMap) {
    const DWORD now = GetTickCount();
    const int prev = gLastLoggedScene.load(std::memory_order_relaxed);
    const DWORD last = gLastSceneLogMs.load(std::memory_order_relaxed);
    PublishWmLive(inMap);
    if (static_cast<int>(st) == prev && last && (now - last < 15000)) return;
    gLastLoggedScene.store(static_cast<int>(st), std::memory_order_relaxed);
    gLastSceneLogMs.store(now, std::memory_order_relaxed);
    x::runtime::LogI("WorldPort", "scene=%d inMap=%d wm=%p mapScene=%p", static_cast<int>(st),
                     inMap ? 1 : 0, gWorldManager, il2::ReadPtr(gWorldManager, kOffWmField));
}

}  // namespace

bool EnsureBound() {
    if (!il2::Ensure()) return false;
    return ResolveWorldManager(false);
}

void* GetWorldManager() {
    if (!EnsureBound()) return nullptr;
    return WorldManagerLooksAlive(gWorldManager) ? gWorldManager : nullptr;
}

void* PeekWorldManager() {
    return WorldManagerLooksAlive(gWorldManager) ? gWorldManager : nullptr;
}

bool Rebind(bool force) {
    if (!il2::Ensure()) return false;
    return ResolveWorldManager(force);
}

bool IsAlive() { return WorldManagerLooksAlive(gWorldManager); }

void Invalidate() {
    gWorldManager = nullptr;
    gLastWmBindMs = 0;
    PublishWmLive(false);
    // Explicit leave-map: do not wait for the next IsPlayReady poll.
    x::runtime::main_thread::SetPumpPhase(x::runtime::main_thread::PumpPhase::Bootstrap);
    x::runtime::managed_main::SetMapTransitBlock(true);
}

SceneState GetSceneState() {
    void* wm = GetWorldManager();
    return ReadSceneStateRaw(wm);
}

bool IsInMapScene() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) {
        // No WM → cannot be play-ready; drop pump phase without waiting for IsPlayReady.
        if (x::runtime::main_thread::GetPumpPhase() == x::runtime::main_thread::PumpPhase::InMap) {
            x::runtime::main_thread::SetPumpPhase(x::runtime::main_thread::PumpPhase::Bootstrap);
        }
        return false;
    }

    const SceneState st = ReadSceneStateRaw(wm);
    void* mapScene = il2::ReadPtr(wm, kOffWmField);
    const bool mapOk = il2::LooksLikeHeapPtr(mapScene);
    const bool cdOk = WorldManagerLooksAlive(wm);

    bool inMap = false;
    if (st == SceneState::MapScene) {
        inMap = true;
    } else if (st == SceneState::Unknown) {
        inMap = mapOk && cdOk;
    } else {
        inMap = false;
    }

    MaybeLogScene(st, inMap);
    // Soft sync on leave (InterStage / CashShop / migrate): Bootstrap immediately.
    // Enter InMap only via IsPlayReady (needs IsAlive).
    if (!inMap &&
        x::runtime::main_thread::GetPumpPhase() == x::runtime::main_thread::PumpPhase::InMap) {
        x::runtime::main_thread::SetPumpPhase(x::runtime::main_thread::PumpPhase::Bootstrap);
    }
    return inMap;
}

bool IsPlayReady() {
    const bool ready = IsInMapScene() && IsAlive();
    // Phase: InMap drains on WM.FixedUpdate/Update once MI patched; else Bootstrap hooks.
    x::runtime::main_thread::SetPumpPhase(ready ? x::runtime::main_thread::PumpPhase::InMap
                                                : x::runtime::main_thread::PumpPhase::Bootstrap);
    // 仓级契约：!PlayReady ⇒ managed_main::FindAll 默认拒（bypassFreeze 除外）。
    x::runtime::managed_main::SetMapTransitBlock(!ready);
    return ready;
}

void* GetMapScene() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return nullptr;
    void* mapScene = il2::ReadPtr(wm, kOffWmField);
    return il2::LooksLikeHeapPtr(mapScene) ? mapScene : nullptr;
}

int GetMapSceneKey() {
    // FieldKey 在 WM@0x80（2026-08-06），不再挂在 SceneField 上
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return -1;
    return static_cast<int>(ReadU8(wm, kOffFieldKey));
}

bool HasMapData() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return false;
    void* mapData = il2::ReadPtr(wm, kOffWmMapData);
    return il2::LooksLikeHeapPtr(mapData);
}

int GetMapId() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return 0;
    void* mapData = il2::ReadPtr(wm, kOffWmMapData);
    if (!il2::LooksLikeHeapPtr(mapData)) return 0;
    // 出生图 id=0（000000000 / 菇菇村訓練所入口）。禁止再写成 id>0 才返回。
    return static_cast<int>(ReadU32(mapData, kOffMapDataId));
}

uint32_t GetCharacterId() {
    // CharacterStat.CharacterID@+0x10（CMS）——战斗/DamageInfo 等同槽。
    // User.CharacterId 真源 +0x1B0（IDA mov eax,[rcx+1B0h]）；旧 0x198 是 Camera*。
    constexpr size_t kFbCsCharacterId = 0x10;
    constexpr size_t kFbUserCharacterId = 0x1B0;
    constexpr uint32_t kCidPlausibleMax = 0x04000000u;

    auto plausible = [](uint32_t id) { return id != 0 && id < kCidPlausibleMax; };

    uint32_t csCid = 0;
    void* cs = x::ui::player::LocalCharacterStat();
    if (il2::LooksLikeHeapPtr(cs)) csCid = ReadU32(cs, kFbCsCharacterId);

    uint32_t userCid = 0;
    void* mu = x::ui::player::LocalMyUser();
    if (il2::LooksLikeHeapPtr(mu)) userCid = ReadU32(mu, kFbUserCharacterId);

    uint32_t wmCid = 0;
    void* wm = GetWorldManager();
    if (il2::LooksLikeHeapPtr(wm)) {
        // dump 把对象槽标成 CharacterId@0x90；uint getter 实读 +0x98
        const uint32_t wm98 = ReadU32(wm, 0x98);
        const uint32_t wmOff = ReadU32(wm, kOffWmCharacterId);
        if (plausible(wm98))
            wmCid = wm98;
        else if (plausible(wmOff))
            wmCid = wmOff;
    }

    if (plausible(csCid)) return csCid;
    if (plausible(userCid)) return userCid;
    if (plausible(wmCid)) return wmCid;
    return csCid ? csCid : (userCid ? userCid : wmCid);
}

// 地上 Drop.OwnerId 在本地 WM 上扫到的字段（可能 ≠ get_CharacterId 的 +0x98）
size_t gDropOwnerWmFieldOff = 0;

void NoteDropOwnerWmFieldOff(size_t off) {
    if (off < 0x10 || off >= 0x200) return;
    if (gDropOwnerWmFieldOff == off) return;
    gDropOwnerWmFieldOff = off;
}

void ClearDropOwnerWmFieldOff() { gDropOwnerWmFieldOff = 0; }

size_t PeekDropOwnerWmFieldOff() { return gDropOwnerWmFieldOff; }

uint32_t GetDropOwnerCharacterId() {
    // Drop.OwnerId 工程真源（经典版 runtime IDB · imagebase 0x7FF848C80000）：
    // 1) 进包：ReadInt → mov [Drop+34h], eax（服端）
    // 2) ByPet/Near 反汇编 cmp [obj+98h]，但 BIN 实机：WM+0x98 == CS+0x10(194899)
    //    地上 OwnerId=118536 落在 WM+0x114（2026-08-12 scan 钉死）
    // 3) 优先钉死偏移；否则试 +0x114，再试 +0x98（仅当 ≠ CS）
    // ★禁止 User+0x1B0 / CS+0x10
    constexpr size_t kFbWmDropOwnerId = 0x114;  // BIN scan
    constexpr size_t kFbWmUintCharacterId = 0x98;
    constexpr uint32_t kCidPlausibleMax = 0x04000000u;
    auto plausible = [](uint32_t id) { return id != 0 && id < kCidPlausibleMax; };

    void* wm = PeekWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return 0;

    if (gDropOwnerWmFieldOff) {
        const uint32_t pinned = ReadU32(wm, gDropOwnerWmFieldOff);
        if (plausible(pinned)) return pinned;
    }

    const uint32_t csCid = GetCharacterId();
    const uint32_t at114 = ReadU32(wm, kFbWmDropOwnerId);
    // +0x114 = Drop.OwnerId 槽：允许 ==CS（部分角两套 ID 合一，例 195466）
    if (plausible(at114)) return at114;

    // +0x98 = 战斗/CS 系；仅当 ≠CS 才当 Drop 归属用（防毒）
    const uint32_t wm98 = ReadU32(wm, kFbWmUintCharacterId);
    if (plausible(wm98) && !(csCid && wm98 == csCid)) return wm98;
    return 0;
}

}  // namespace x::features::ports::world

// Character 链防漂 API：实现放在 TU 尾，避免 world_port↔player_vitals 头文件环依赖。
#include "../../ui/player_vitals.h"

namespace x::features::ports::world {

void* GetCharacterData() { return x::ui::player::LocalCharacterData(); }
void* GetCharacterStat() { return x::ui::player::LocalCharacterStat(); }
int64_t ReadMoney() { return x::ui::player::ReadMoney(); }
void* GetItemSlotList(int invType) { return x::ui::player::GetItemSlotList(invType); }

bool ReadCharacterRegDateTicks(int64_t* outTicks) {
    if (outTicks) *outTicks = 0;
    EnsureWmFieldOff();
    void* wm = PeekWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return false;

    constexpr int64_t kTicksMask = 0x3FFFFFFFFFFFFFFFLL;
    // .NET DateTime：2000-01-01 .. ~2040-01-01，滤掉未下发的 0 / 砸到 QuestTimers 指针。
    constexpr int64_t kMinTicks = 630822816000000000LL;
    constexpr int64_t kMaxTicks = 646790112000000000LL;

    const int64_t raw = ReadI64(wm, kOffWmCharacterRegDate);
    const int64_t ticks = raw & kTicksMask;
    if (ticks < kMinTicks || ticks > kMaxTicks) return false;
    if (outTicks) *outTicks = ticks;
    return true;
}

}  // namespace x::features::ports::world
