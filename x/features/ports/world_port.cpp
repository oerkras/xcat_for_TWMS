// Classic TWMS — WorldManager SSOT + scene gate (read-only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "world_port.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::features::ports::world {
namespace {

namespace il2 = x::runtime::il2cpp;

// WM klass：il2cpp_shape::ResolveWorldManagerKlass（hash ab85c0a9… + shape 兜底）

constexpr size_t kOffWmSceneState = 0x34;
constexpr size_t kOffWmField = 0x58;
constexpr size_t kOffWmMapData = 0x88;
constexpr size_t kOffWmCharacterId = 0x98;
constexpr size_t kOffWmCharacterData = 0xE0;

constexpr size_t kOffFieldKey = 0x98;
constexpr size_t kOffMapDataId = 0x10;
constexpr size_t kOffCdCharacterStat = 0x10;

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
}

SceneState GetSceneState() {
    void* wm = GetWorldManager();
    return ReadSceneStateRaw(wm);
}

bool IsInMapScene() {
    void* wm = GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return false;

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
    return inMap;
}

bool IsPlayReady() { return IsInMapScene() && IsAlive(); }

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
