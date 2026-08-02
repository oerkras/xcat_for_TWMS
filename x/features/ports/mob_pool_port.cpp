// Classic TWMS ??MobPool read-only port (P1).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_pool_port.h"

#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace x::features::ports::mob {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Class hashes remounted 2026-08-03 (client update).
// MapData：必须以 WM._currentMapData@+0x88 的字段类型为准（bb2af058…）；
// 误用 b6fed812… 会通过 FindClass 但 ObjKlassIs 失败 → M/mapId 全废。
constexpr char kMobPoolClass[] =
    "d8a4e9e1cd748ad0e8b74931ab3f4f590033561dcccab87aaab6bd28fbc8010";
constexpr char kMobClass[] =
    "a6c2b4310ec35fad4d0e6553a05ba23450dc09133c50f7ead6dd2b30d46369f";
constexpr char kMapDataClass[] =
    "bb2af0589464ae5ebed8746710fe5df221e7119ae389d8da192c0a46140dff1";

// UnityEngine.Object.FindObjectsOfTypeAll（与 invuln 同 RVA）。
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03

// MobPool
constexpr size_t kOffPoolDict = 0x10;

// FieldActorBase / VecCtrlOwner
constexpr size_t kOffVecCtrl = 0x50;
constexpr size_t kOffPos = 0x64;
constexpr size_t kOffCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr

// Mob (TW dump ??CMS layout for these fields)
constexpr size_t kOffTemplateId = 0xB0;
constexpr size_t kOffIsReady = 0xEC;
constexpr size_t kOffPvcActive = 0xF0;
// 地图特殊体（日志 tpl=9999999）：不计入活怪 n / 不进 combat 缓存。
constexpr int32_t kSpecialTplExclude = 9999999;
constexpr size_t kOffMobId = 0x134;
constexpr size_t kOffDeadType = 0x1B4;
constexpr size_t kOffHpPct = 0x240;
constexpr size_t kOffMobCtrlState = 0xE8;  // MobCtrlType; where MobChangeController lands

// VecCtrl AbsPos (doubles) ??fallback when Pos??
constexpr size_t kOffVcApX = 0x98;
constexpr size_t kOffVcApY = 0xA0;

// Dictionary<int,Mob*> typical IL2CPP layout
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kOffDictFreeCount = 0x2C;
constexpr size_t kEntrySize = 0x18;
constexpr size_t kOffEntryHash = 0x00;
constexpr size_t kOffEntryValue = 0x10;

// WorldManager / MapData / MapLifeData offsets (TW dump).
constexpr size_t kOffWmCurrentMapData = 0x88;
constexpr size_t kOffMapId = 0x10;
constexpr size_t kOffMapLifeList = 0x38;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffLifeType = 0x20;  // MapLifeData.LifeType；Mob=1（枚举 0/1/2）
constexpr int kLifeTypeMob = 1;

constexpr float kMinPosAbs = 0.5f;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassGetType gClassGetType = nullptr;
FnTypeGetObject gTypeGetObject = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;

void* gMobPoolKlass = nullptr;
void* gMobKlass = nullptr;
void* gMobTypeObj = nullptr;
void* gMobPool = nullptr;
void* gMapDataKlass = nullptr;

// Peak spawn fallback (fengxing UpdateSpawnPeak).
int gPeakMapId = 0;
int gPeakAlive = 0;
std::atomic<int> gLastSpawnSlots{-1};

std::atomic<bool> gBound{false};
Snapshot gCacheA{};
Snapshot gCacheB{};
std::atomic<int> gCacheIdx{0};  // 0?A, 1?B published

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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

float ReadF32(void* obj, size_t off) {
    if (!obj) return 0.f;
    __try {
        return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

bool UnityObjectAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    void* cached = ReadPtr(obj, kOffCachedPtr);
    return LooksLikeHeapPtr(cached);
}

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

void* TryLazyValue(void* lazy) {
    if (!lazy || !LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    if (gClassStaticData) {
        __try {
            void* p = gClassStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

bool LooksLikeMobPool(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gMobPoolKlass && !ObjKlassIs(cand, gMobPoolKlass)) return false;
    void* dict = ReadPtr(cand, kOffPoolDict);
    if (!dict) return true;  // empty map ok
    return LooksLikeHeapPtr(dict);
}

void* ResolveMobPoolSingleton() {
    if (gMobPool && LooksLikeMobPool(gMobPool)) return gMobPool;
    gMobPool = nullptr;
    if (!gMobPoolKlass) gMobPoolKlass = FindClass(kMobPoolClass);
    if (!gMobPoolKlass) return nullptr;

    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gMobPoolKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* staticsKlass = gMobPoolKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gMobPoolKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) {
                __try {
                    gRuntimeClassInit(parent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            staticsKlass = parent;
        }
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gMobPoolKlass);
    if (!statics) return nullptr;

    void* best = nullptr;
    for (size_t s = 0; s < 4; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeMobPool(cand)) continue;
        best = cand;
        break;
    }
    if (best) gMobPool = best;
    return gMobPool;
}

void ReadMobPos(void* mob, float& x, float& y) {
    x = ReadF32(mob, kOffPos);
    y = ReadF32(mob, kOffPos + 4);
    if (std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs) return;

    void* vc = ReadPtr(mob, kOffPvcActive);
    if (!LooksLikeHeapPtr(vc)) vc = ReadPtr(mob, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return;
    const double ax = ReadF64(vc, kOffVcApX);
    const double ay = ReadF64(vc, kOffVcApY);
    if (std::fabs(ax) >= kMinPosAbs || std::fabs(ay) >= kMinPosAbs) {
        x = static_cast<float>(ax);
        y = static_cast<float>(ay);
    }
}

bool FillLite(void* mob, MobLite& out) {
    if (!UnityObjectAlive(mob)) return false;
    if (gMobKlass && !ObjKlassIs(mob, gMobKlass)) return false;

    const int id = ReadI32(mob, kOffMobId);
    if (id == 0) return false;

    const int deadType = ReadI32(mob, kOffDeadType);
    const int hpPct = ReadI32(mob, kOffHpPct);
    const bool ready = ReadU8(mob, kOffIsReady) != 0;

    // 未就绪 / 尸体 / 空血：不入活怪榜（曾被乱码注释吞掉 ready 门 → 池脏坐标贴飞）。
    if (!ready) return false;
    if (deadType != 0) return false;
    if (hpPct <= 0) return false;

    float x = 0.f, y = 0.f;
    ReadMobPos(mob, x, y);

    out.ptr = mob;
    out.id = id;
    out.templateId = ReadI32(mob, kOffTemplateId);
    if (out.templateId == kSpecialTplExclude) return false;
    out.hpPct = hpPct;
    out.deadType = deadType;
    out.ctrl = ReadI32(mob, kOffMobCtrlState);
    out.x = x;
    out.y = y;
    out.ready = ready;
    return true;
}

bool PushLite(Snapshot& snap, const MobLite& m) {
    if (snap.count >= kMaxLiteMobs) {
        snap.truncated = true;
        return false;
    }
    snap.mobs[snap.count++] = m;
    return true;
}

int CollectFromDict(void* pool, Snapshot& snap) {
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;

    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    const int freeCount = ReadI32(dict, kOffDictFreeCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;

    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    const int liveHint = count - freeCount;
    (void)liveHint;

    int raw = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;  // free slot
        void* mob = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(mob)) continue;
        ++raw;
        MobLite lite{};
        if (!FillLite(mob, lite)) continue;
        if (!PushLite(snap, lite) && snap.truncated) break;
    }
    snap.rawDict = raw;
    return snap.count;
}

int CollectFromFindAll(Snapshot& snap) {
    if (!gFindAll) return 0;
    if (!gMobTypeObj) gMobTypeObj = FindClassTypeObject(kMobClass);
    if (!gMobTypeObj) return 0;

    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gMobTypeObj, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!arr) return 0;

    const uintptr_t n = ArrayLen(arr);
    int raw = 0;
    for (uintptr_t i = 0; i < n && i < 512; ++i) {
        void* mob = ArrayAt(arr, i);
        if (!LooksLikeHeapPtr(mob)) continue;
        ++raw;
        MobLite lite{};
        if (!FillLite(mob, lite)) continue;
        if (!PushLite(snap, lite) && snap.truncated) break;
    }
    snap.rawDict = raw;
    return snap.count;
}

void Publish(const Snapshot& snap) {
    const int next = 1 - gCacheIdx.load(std::memory_order_relaxed);
    if (next == 0)
        gCacheA = snap;
    else
        gCacheB = snap;
    gCacheIdx.store(next, std::memory_order_release);
    gLastSpawnSlots.store(snap.spawnSlots, std::memory_order_release);
}

void UpdateSpawnPeak(int mapId, int alive) {
    if (mapId <= 0 || alive < 0) return;
    if (mapId != gPeakMapId) {
        gPeakMapId = mapId;
        gPeakAlive = 0;
    }
    if (alive > gPeakAlive) gPeakAlive = alive;
}

int PeakForMap(int mapId) {
    if (mapId <= 0 || mapId != gPeakMapId || gPeakAlive <= 0) return -1;
    return gPeakAlive;
}

// Count Mob entries in MapData.LifeList; -1 on failure.
int CountLifeMobSlotsFromMapData(void* mapData, int* outAll, int* outMapId) {
    if (outAll) *outAll = -1;
    if (outMapId) *outMapId = 0;
    if (!LooksLikeHeapPtr(mapData)) return -1;
    // klass ????????????????MapData
    if (gMapDataKlass && !ObjKlassIs(mapData, gMapDataKlass)) return -1;

    const int mapId = ReadI32(mapData, kOffMapId);
    if (outMapId) *outMapId = mapId;

    void* list = ReadPtr(mapData, kOffMapLifeList);
    if (!LooksLikeHeapPtr(list)) return -1;
    void* items = ReadPtr(list, kOffListItems);
    const int size = ReadI32(list, kOffListSize);
    if (!LooksLikeHeapPtr(items) || size < 0 || size > 4096) return -1;

    int mobN = 0;
    int allN = 0;
    for (int i = 0; i < size; ++i) {
        void* life = ArrayAt(items, (uintptr_t)i);
        if (!LooksLikeHeapPtr(life)) continue;
        ++allN;
        const int ty = ReadI32(life, kOffLifeType);
        if (ty == kLifeTypeMob) ++mobN;
    }
    if (outAll) *outAll = allN;
    return mobN;
}

// Fill spawnSlots from LifeList; fallback to peak.
void FillSpawnSlots(Snapshot& out) {
    out.spawnSlots = -1;
    out.mapId = 0;
    out.lifeMob = -1;
    out.lifeAll = -1;

    void* wm = world::GetWorldManager();
    if (!wm) return;
    void* mapData = ReadPtr(wm, kOffWmCurrentMapData);
    if (!LooksLikeHeapPtr(mapData)) {
        world::Rebind(true);
        wm = world::GetWorldManager();
        mapData = wm ? ReadPtr(wm, kOffWmCurrentMapData) : nullptr;
    }

    int lifeAll = -1;
    int mapId = 0;
    const int lifeMob = CountLifeMobSlotsFromMapData(mapData, &lifeAll, &mapId);
    out.mapId = mapId;
    out.lifeMob = lifeMob;
    out.lifeAll = lifeAll;

    UpdateSpawnPeak(mapId, out.count);

    if (lifeMob >= 0) {
        out.spawnSlots = lifeMob;
        return;
    }
    out.spawnSlots = PeakForMap(mapId);
}

bool BindApis() {
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gClassGetType = e.classGetType;
    gTypeGetObject = e.typeGetObject;
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gRuntimeClassInit = e.runtimeClassInit;

    if (!gGA) return false;

    gMobPoolKlass = FindClass(kMobPoolClass);
    gMobKlass = FindClass(kMobClass);
    gMapDataKlass = FindClass(kMapDataClass);
    if (gMobKlass && gClassGetType && gTypeGetObject) {
        __try {
            void* t = gClassGetType(gMobKlass);
            if (t) gMobTypeObj = x::runtime::managed_main::TypeGetObject(gTypeGetObject, t, 2000);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return gMobPoolKlass != nullptr || gMobKlass != nullptr;
}

}  // namespace

const char* CtrlName(int32_t ctrl) {
    switch (ctrl) {
        case kMobCtrlPassive: return "Passive";
        case kMobCtrlPassive0: return "Passive0";
        case kMobCtrlPassive1: return "Passive1";
        case kMobCtrlActiveInt: return "ActiveInt";
        case kMobCtrlActiveReq: return "ActiveReq";
        case kMobCtrlActivePerm0: return "ActivePerm0";
        case kMobCtrlActivePerm1: return "ActivePerm1";
        default: return "?";
    }
}

bool EnsureBound() {
    if (gBound.load(std::memory_order_acquire) && (gMobPoolKlass || gMobKlass)) return true;
    if (!BindApis()) {
        gBound.store(false, std::memory_order_release);
        return false;
    }
    gBound.store(true, std::memory_order_release);
    x::runtime::LogI("MobPort",
                     "bound poolKlass=%p mobKlass=%p mapDataKlass=%p FindAll=%p (WM=world_port)",
                     gMobPoolKlass, gMobKlass, gMapDataKlass, gFindAll);
    return true;
}

bool Collect(Snapshot& out) {
    out = Snapshot{};
    out.tickMs = GetTickCount64();
    if (!EnsureBound()) return false;

    void* pool = ResolveMobPoolSingleton();
    if (pool) {
        CollectFromDict(pool, out);
        out.ok = true;
    } else {
        // Fallback?FindAll(Mob) ???? Singleton ???????????????
        CollectFromFindAll(out);
        out.ok = out.count > 0 || gMobTypeObj != nullptr;
    }

    if (out.ok) {
        FillSpawnSlots(out);
        Publish(out);
    }
    return out.ok;
}

bool TryFillLive(void* mob, int32_t expectId, MobLite& out) {
    out = MobLite{};
    if (!LooksLikeHeapPtr(mob)) return false;
    if (!EnsureBound()) return false;
    MobLite lite{};
    if (!FillLite(mob, lite)) return false;
    if (expectId != 0 && lite.id != expectId) return false;
    out = lite;
    return true;
}

bool GetCached(Snapshot& out) {
    const int idx = gCacheIdx.load(std::memory_order_acquire);
    out = (idx == 0) ? gCacheA : gCacheB;
    return out.ok;
}

int GetCachedAliveCount() {
    Snapshot s{};
    if (!GetCached(s)) return -1;
    return s.count;
}

int GetCachedSpawnSlots() {
    return gLastSpawnSlots.load(std::memory_order_acquire);
}

int CountMapMobLifeSlots() {
    if (!EnsureBound()) return -1;
    void* wm = world::GetWorldManager();
    if (!wm) return -1;
    void* mapData = ReadPtr(wm, kOffWmCurrentMapData);
    int all = -1;
    int mapId = 0;
    const int mobN = CountLifeMobSlotsFromMapData(mapData, &all, &mapId);
    if (mobN >= 0) return mobN;
    return PeakForMap(mapId);
}

int GetSpawnPeak() {
    return gPeakAlive > 0 ? gPeakAlive : -1;
}

}  // namespace x::features::ports::mob
