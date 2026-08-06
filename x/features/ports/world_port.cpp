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

// WM klass：il2cpp_shape::ResolveWorldManagerKlass（hash af152981… + shape 兜底）
// SceneState / Field / CharacterId / FieldKey：hash → field_get_offset
// CharacterData / CharacterStat：SSOT = x::ui::player
constexpr char kWorldManagerClass[] =
    "af1529816d3e158e2939f3c03b4fe68c04930802ea39c8d6567d1fb4865b742";
constexpr char kFieldClass[] =
    "cfec8c2698a50442d8c39915b0ace84807a884ba8f42a4d45bdcd80c3d675d0";
constexpr char kHashWmSceneState[] =
    "d26c89da7c7b0b6cc91998e1c9f3c4d791ae04b1a293529ac4b0cb08810fc92";
constexpr char kHashWmField[] =
    "a1466fc5ed9eeb49778c306ae741bfe859e433ccc633df746db2bbf5b36d22d";
constexpr char kHashWmCharacterId[] =
    "<b22c0eb9efe2be1d842a589108889cca3ac4bb5bcdf1fb94cef55dcec6a48bb>k__BackingField";
constexpr char kHashFieldKey[] =
    "<c2521cc6ecfd1d5ca939178fc977a2eab4eaf08c7a72f669d21c8faba6e99c8>k__BackingField";

constexpr size_t kFbWmSceneState = 0x34;
constexpr size_t kFbWmField = 0x58;
constexpr size_t kFbWmCharacterId = 0x98;
constexpr size_t kFbFieldKey = 0x98;
size_t gOffWmSceneState = kFbWmSceneState;
size_t gOffWmField = kFbWmField;
size_t gOffWmCharacterId = kFbWmCharacterId;
size_t gOffFieldKey = kFbFieldKey;
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
    if (!il2::Ensure()) return;
    gWmFieldTried = true;
    void* wm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!wm) wm = il2::FindClass("", kWorldManagerClass);
    void* field = il2::FindClass("", kFieldClass);
    int hits = 0;
    if (WmFieldOffHit(wm, kHashWmSceneState, kFbWmSceneState, &gOffWmSceneState)) ++hits;
    if (WmFieldOffHit(wm, kHashWmField, kFbWmField, &gOffWmField)) ++hits;
    if (WmFieldOffHit(wm, kHashWmCharacterId, kFbWmCharacterId, &gOffWmCharacterId)) ++hits;
    if (WmFieldOffHit(field, kHashFieldKey, kFbFieldKey, &gOffFieldKey)) ++hits;
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
    return ready;
}

void* GetMapScene() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return nullptr;
    void* mapScene = il2::ReadPtr(wm, kOffWmField);
    return il2::LooksLikeHeapPtr(mapScene) ? mapScene : nullptr;
}

int GetMapSceneKey() {
    void* mapScene = GetMapScene();
    if (!mapScene) return -1;
    return static_cast<int>(ReadU8(mapScene, kOffFieldKey));
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
