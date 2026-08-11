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
    "acda742ab51e7e2e3003fd2b44fbc00eababde4300ef17ac35b5f4fd01bee68";
constexpr char kHashWmSceneState[] =
    "d190e0af58c4288d25573fdf91171036e93c69653c7357f9a1e4c7b7efd0077";
constexpr char kHashWmField[] =
    "cd05676f83eff32a7a754d6c6287f124ca239987ddd9c57667f9cce26502a0e";
constexpr char kHashWmCharacterId[] =
    "<b33ad98e8d2b26e76c74ed2a5a5fe8ca99561c5ac3fc68e2962ea770f11acdf>k__BackingField";
// FieldKey 现挂在 WM 本体（byte@_fieldKey），不再走 SceneField+0x98
constexpr char kHashWmFieldKey[] =
    "b4ba3b6c23175b5e7a2099cdc465f07c421273042694938c0193ea5be2a924c";

constexpr size_t kFbWmSceneState = 0x34;
constexpr size_t kFbWmField = 0x58;
constexpr size_t kFbWmCharacterId = 0x90;  // was 0x98
constexpr size_t kFbWmFieldKey = 0x80;     // was Field+0x98
size_t gOffWmSceneState = kFbWmSceneState;
size_t gOffWmField = kFbWmField;
size_t gOffWmCharacterId = kFbWmCharacterId;
size_t gOffFieldKey = kFbWmFieldKey;
#define kOffWmSceneState (gOffWmSceneState)
#define kOffWmField (gOffWmField)
#define kOffWmCharacterId (gOffWmCharacterId)
#define kOffFieldKey (gOffFieldKey)
#define kOffWmMapData (x::runtime::il2cpp_mapdata::OffWmMapData())
#define kOffMapDataId (x::runtime::il2cpp_mapdata::OffMapId())
#define kOffWmCharacterData (x::ui::player::OffWmCharacterData())
#define kOffCdCharacterStat (x::ui::player::OffCdCharacterStat())
bool gWmFieldTried = false;

bool PlausibleWmOff(size_t off) { return off >= 0x20 && off < 0x200; }

bool WmFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
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
            if (PlausibleWmOff(off)) {
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
    x::runtime::LogI("WorldPort",
                     "wm/field slots path=%s hits=%d/4 scene=0x%zX field=0x%zX charId=0x%zX "
                     "fkey=0x%zX",
                     hits == 4 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                     gOffWmSceneState, gOffWmField, gOffWmCharacterId, gOffFieldKey);
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

int GetMapId() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return 0;
    void* mapData = il2::ReadPtr(wm, kOffWmMapData);
    if (!il2::LooksLikeHeapPtr(mapData)) return 0;
    const int id = static_cast<int>(ReadU32(mapData, kOffMapDataId));
    return id > 0 ? id : 0;
}

uint32_t GetCharacterId() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return 0;
    return ReadU32(wm, kOffWmCharacterId);
}

}  // namespace x::features::ports::world

// Character 链防漂 API：实现放在 TU 尾，避免 world_port↔player_vitals 头文件环依赖。
#include "../../ui/player_vitals.h"

namespace x::features::ports::world {

void* GetCharacterData() { return x::ui::player::LocalCharacterData(); }
void* GetCharacterStat() { return x::ui::player::LocalCharacterStat(); }
int64_t ReadMoney() { return x::ui::player::ReadMoney(); }
void* GetItemSlotList(int invType) { return x::ui::player::GetItemSlotList(invType); }

}  // namespace x::features::ports::world
