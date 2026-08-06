// Classic TWMS ??MobPool read-only port (P1).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_pool_port.h"

#include "world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace x::features::ports::mob {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Class hashes remounted 2026-08-04（旧 08-03：MobPool=d8a4e9e1… Mob=a6c2b431… MapData=bb2af058…）。
// 交叉：Mob TDI:1507；MobPool→Dictionary<int,Mob>@+0x10；WM._currentMapData@+0x88 类型=a08e1596…。
// 误用其它 Map* 壳会通过 FindClass 但 ObjKlassIs 失败 → M/mapId 全废。
constexpr char kMobPoolClass[] =
    "f4afa0ce542b9322a09ad954f69cc727eae4b5c550c7c68a1ba450d0d13ec02";
constexpr char kMobClass[] =
    "a803dc6312a27204244f43331114818db6b799117c7970d93707d2051ade498";
constexpr char kMapDataClass[] =
    "a08e159696c821e2f934c073a759701d4e6b402cd2f0070450066102602e91e";
// UIHpTag（dump 属性 "UIHpTag"；绝对 cur/max 缓存）
constexpr char kUiHpTagClass[] =
    "f901834d899eb081fc8ce3e858778d0ba708e7b8933ced899d849de0a8e6168";

// ResourcesAPIInternal.FindObjectsOfTypeAll（与 invuln / il2cpp_bind 同 RVA）。
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E4A610;  // remounted 2026-08-04

// MobPool
constexpr size_t kFbPoolDict = 0x10;  // Dictionary<int,Mob>
constexpr char kHashPoolDict[] =
    "c2ded4dd44371180b3ce86d02f8d1a31e0783a21067a2dde4e695705d12d939";
size_t gOffPoolDict = kFbPoolDict;
#define kOffPoolDict (gOffPoolDict)

// FieldActorBase / VecCtrlOwner / Mob：hash → field_get_offset（dump fallback）
constexpr size_t kOffCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr

constexpr char kHashVecCtrl[] =
    "<dc76f5c9e250bc9a327a219b39e16c345cdabf7b01ad5c60b568045069c9120>k__BackingField";
constexpr char kHashPos[] =
    "c9d7ef4393802ebe9fdf9ebe7eaf7245d5cef3eeaa2a8d052fb4ad4883e34dc";
constexpr char kHashTemplateId[] =
    "dc1ccd4a18d416557e6bb006e3356c9b3fdd4aa7731611315c8c724dde15714";
constexpr char kHashIsReady[] =
    "<b050a5a54bc530da78ec88f910471010011ca733a2592df48e9ed46e10e28ee>k__BackingField";
constexpr char kHashPvcActive[] =
    "dd4c058e1a627d9c1177b6369307c3fdc5165d7b6b7249523e08ad05f82b0c3";
constexpr char kHashMobId[] =
    "f73c913a570542915da0198d3b1e4d1ae2abfb1a207a9fa20c354425d9a1d46";
constexpr char kHashDeadType[] =
    "dbb11ab37acba92ed72bd4103ce95f35ebc477bdebda50edc6995b7755ac498";
constexpr char kHashDamageInfoList[] =
    "a7c525933ff370ab7ba6c7730a38b596ccf7f780b9e3f061d12f8793ae26113";
constexpr char kHashLastHitted[] =
    "d5c28c21074ed3e934ddbe8599d3e12929464f06defc1db41548f21ccad6a0d";
constexpr char kHashHpPct[] =
    "<e3ca08a73919587d7ae0a1efb2663f4517077a09c4edaeffbdc659d3df43dd6>k__BackingField";
constexpr char kHashMobCtrlState[] =
    "cbd2d3b42231000972fd104ced908765e86304eb015f6f888c2e58e9280ce95";

constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kFbPos = 0x64;
constexpr size_t kFbTemplateId = 0xB0;
constexpr size_t kFbIsReady = 0xEC;
constexpr size_t kFbPvcActive = 0xF0;
constexpr size_t kFbMobId = 0x134;
constexpr size_t kFbDeadType = 0x1B4;
constexpr size_t kFbDamageInfoList = 0x1D8;
constexpr size_t kFbLastHitted = 0x208;
constexpr size_t kFbHpPct = 0x240;
constexpr size_t kFbMobCtrlState = 0xE8;

size_t gOffVecCtrl = kFbVecCtrl;
size_t gOffPos = kFbPos;
size_t gOffTemplateId = kFbTemplateId;
size_t gOffIsReady = kFbIsReady;
size_t gOffPvcActive = kFbPvcActive;
size_t gOffMobId = kFbMobId;
size_t gOffDeadType = kFbDeadType;
size_t gOffDamageInfoList = kFbDamageInfoList;
size_t gOffLastHitted = kFbLastHitted;
size_t gOffHpPct = kFbHpPct;
size_t gOffMobCtrlState = kFbMobCtrlState;

#define kOffVecCtrl (gOffVecCtrl)
#define kOffPos (gOffPos)
#define kOffTemplateId (gOffTemplateId)
#define kOffIsReady (gOffIsReady)
#define kOffPvcActive (gOffPvcActive)
#define kOffMobId (gOffMobId)
#define kOffDeadType (gOffDeadType)
#define kOffDamageInfoList (gOffDamageInfoList)
#define kOffLastHitted (gOffLastHitted)
#define kOffHpPct (gOffHpPct)
#define kOffMobCtrlState (gOffMobCtrlState)

// 地图特殊体（日志 tpl=9999999）：不计入活怪 n / 不进 combat 缓存。
constexpr int32_t kSpecialTplExclude = 9999999;
// 与 player_combat 同口径：池化复用槽偶发 NaN / INT_MIN 级脏坐标（BIN land_miss）。
constexpr float kMaxPosAbs = 100000.f;

// DamageInfo / VecCtrl.AbsPos：hash → field_get_offset
constexpr char kDamageInfoClass[] =
    "ab37e6988658c1e57b709c657a38963f6b47e8672507990fc654ba17acd6a04";
constexpr char kVecCtrlClass[] =
    "ef24024acbe225bcc90ca332f3e00aff5800daa32a769057d2e830eeac776bb";
constexpr char kHashDiDelayed[] =
    "a05d904d2c5027897723dcd5fbfe3c371e112045774dabd3b4baf0f390e0545";
constexpr char kHashDiCharId[] =
    "ef09b0abe40f1fa0ca990a125c0122e4263c6bf8958be58a8598e722903e754";
constexpr char kHashDiSkillId[] =
    "eee5aff238bc25df81528628c584a98d3b9951eba38a5b10d1234cbe27ab3f9";
constexpr char kHashDiHitAction[] =
    "ff84e3bf831e6524c87f58cea6b3236349c6a3f55648a3f73a63142e65510dc";
constexpr char kHashDiDamage[] =
    "eaeca82de91a1f5d7f2eaa50416084d7c5c74757a2ca8852e2701e041f5e9b4";
constexpr char kHashDiAttackIdx[] =
    "bdd56d5f45cfe627617f7794a1162a997ec0ee481bb4d1eb14907dfc4a04df7";
constexpr char kHashDiMoveType[] =
    "f2723c40a21e303f3368d9de930c42f4bed7d5b7d485af23601e9347dae4f1b";
constexpr char kHashVcAp[] =
    "a860e652f11e3e8846eaf4dfb600e319058d3e0e9e79b3fd7a3447344d98bb9";  // AbsPos start; Y=+8

constexpr size_t kFbDiDelayed = 0x10;
constexpr size_t kFbDiCharId = 0x14;
constexpr size_t kFbDiSkillId = 0x18;
constexpr size_t kFbDiHitAction = 0x1C;
constexpr size_t kFbDiDamage = 0x24;
constexpr size_t kFbDiAttackIdx = 0x2C;
constexpr size_t kFbDiMoveType = 0x34;
constexpr size_t kFbVcAp = 0x98;  // AbsPos; Y = Ap+8

size_t gOffDiDelayed = kFbDiDelayed;
size_t gOffDiCharId = kFbDiCharId;
size_t gOffDiSkillId = kFbDiSkillId;
size_t gOffDiHitAction = kFbDiHitAction;
size_t gOffDiDamage = kFbDiDamage;
size_t gOffDiAttackIdx = kFbDiAttackIdx;
size_t gOffDiMoveType = kFbDiMoveType;
size_t gOffVcAp = kFbVcAp;

#define kOffDiDelayed (gOffDiDelayed)
#define kOffDiCharId (gOffDiCharId)
#define kOffDiSkillId (gOffDiSkillId)
#define kOffDiHitAction (gOffDiHitAction)
#define kOffDiDamage (gOffDiDamage)
#define kOffDiAttackIdx (gOffDiAttackIdx)
#define kOffDiMoveType (gOffDiMoveType)
#define kOffVcApX (gOffVcAp)
#define kOffVcApY (gOffVcAp + 8)

// UIHpTag（相对 CMS +8：基类多一截）
// UIHpTag 绝对血量槽（hash → field_get_offset；dump fallback）
constexpr size_t kFbUiHpTagMobId = 0xC8;
constexpr size_t kFbUiHpTagCurHp = 0xD4;
constexpr size_t kFbUiHpTagMaxHp = 0xD8;
constexpr char kHashUiHpTagMobId[] =
    "b6b2c77dc67ae0206b675bbe969de1fc78ff1c2756ceaa4ca1f74a70f31af24";
constexpr char kHashUiHpTagCurHp[] =
    "eeb20f926bc76e943c09f70217ee6f8500d3455d7aafe4821dac5680fc51be1";
constexpr char kHashUiHpTagMaxHp[] =
    "ef320d557020494b697218b80c868e76066453ae7ce22456953f7f864cb4747";
size_t gOffUiHpTagMobId = kFbUiHpTagMobId;
size_t gOffUiHpTagCurHp = kFbUiHpTagCurHp;
size_t gOffUiHpTagMaxHp = kFbUiHpTagMaxHp;
#define kOffUiHpTagMobId (gOffUiHpTagMobId)
#define kOffUiHpTagCurHp (gOffUiHpTagCurHp)
#define kOffUiHpTagMaxHp (gOffUiHpTagMaxHp)


// Dictionary / List → il2cpp_container（旧 freeCount@0x2C 误读 _version，已纠正）
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictFreeCount (x::runtime::il2cpp_container::OffDictFreeCount())
#define kEntrySize (x::runtime::il2cpp_container::DictEntryStrideIntPtr())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())
#define kOffEntryValue (x::runtime::il2cpp_container::OffDictEntryValuePtr())

// WorldManager / MapData / MapLifeData → il2cpp_mapdata SSOT
#define kOffWmCurrentMapData (x::runtime::il2cpp_mapdata::OffWmMapData())
#define kOffMapId (x::runtime::il2cpp_mapdata::OffMapId())
#define kOffMapLifeList (x::runtime::il2cpp_mapdata::OffMapLifeList())
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
// MapLifeData.LifeType（hash 防漂）；Mob=1（枚举 0/1/2）
constexpr char kMapLifeClass[] =
    "f7d49ea8e32a285bbc61a352badc8bd0e281d43a8e2079b81e7b4e97fffaf3c";
constexpr char kHashLifeType[] =
    "<b2635419913810c13f74a02a2fe009f6fe627c51860f1bb46c5ac4290233e6d>k__BackingField";
constexpr size_t kFbLifeType = 0x20;
size_t gOffLifeType = kFbLifeType;
#define kOffLifeType (gOffLifeType)
constexpr int kLifeTypeMob = 1;
std::atomic<bool> gUiLifeFieldResolved{false};

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
void* gUiHpTagKlass = nullptr;
void* gUiHpTagTypeObj = nullptr;

// Peak spawn fallback (fengxing UpdateSpawnPeak).
int gPeakMapId = 0;
int gPeakAlive = 0;
std::atomic<int> gLastSpawnSlots{-1};

std::atomic<bool> gBound{false};
std::atomic<bool> gPoolDictResolved{false};
std::atomic<bool> gMobFieldResolved{false};
Snapshot gCacheA{};
Snapshot gCacheB{};
std::atomic<int> gCacheIdx{0};  // 0?A, 1?B published

void EnsurePoolDictOffset() {
    if (gPoolDictResolved.load(std::memory_order_acquire)) return;
    gPoolDictResolved.store(true, std::memory_order_release);
    if (!gMobPoolKlass || !x::runtime::il2cpp::Ensure()) return;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(gMobPoolKlass, kHashPoolDict);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        field = nullptr;
    }
    size_t off = 0;
    if (field) {
        __try {
            off = e.fieldGetOffset(field);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            off = 0;
        }
    }
    if (off >= 0x10 && off < 0x80) {
        gOffPoolDict = off;
        x::runtime::LogI("MobPool", "poolDict path=meta off=0x%zX", gOffPoolDict);
    } else {
        gOffPoolDict = kFbPoolDict;
        x::runtime::LogW("MobPool", "poolDict path=fallback off=0x%zX", gOffPoolDict);
    }
}

bool PlausibleMobOff(size_t off) { return off >= 0x10 && off < 0x400; }

bool MobFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
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
            if (PlausibleMobOff(off)) {
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

void EnsureMobFieldOffsets() {
    if (gMobFieldResolved.load(std::memory_order_acquire)) return;
    gMobFieldResolved.store(true, std::memory_order_release);
    if (!x::runtime::il2cpp::Ensure()) return;
    if (!gMobKlass) gMobKlass = x::runtime::il2cpp::FindClass("", kMobClass);
    if (!gMobKlass) {
        x::runtime::LogW("MobPool", "mob fields path=fallback klass miss");
        return;
    }
    int hits = 0;
    auto hit = [&](const char* hash, size_t fb, size_t* slot) {
        if (MobFieldOffHit(gMobKlass, hash, fb, slot)) ++hits;
    };
    hit(kHashVecCtrl, kFbVecCtrl, &gOffVecCtrl);
    hit(kHashPos, kFbPos, &gOffPos);
    hit(kHashTemplateId, kFbTemplateId, &gOffTemplateId);
    hit(kHashIsReady, kFbIsReady, &gOffIsReady);
    hit(kHashPvcActive, kFbPvcActive, &gOffPvcActive);
    hit(kHashMobId, kFbMobId, &gOffMobId);
    hit(kHashDeadType, kFbDeadType, &gOffDeadType);
    hit(kHashDamageInfoList, kFbDamageInfoList, &gOffDamageInfoList);
    hit(kHashLastHitted, kFbLastHitted, &gOffLastHitted);
    hit(kHashHpPct, kFbHpPct, &gOffHpPct);
    hit(kHashMobCtrlState, kFbMobCtrlState, &gOffMobCtrlState);
    constexpr int kExpect = 11;
    x::runtime::LogI(
        "MobPool",
        "mob fields path=%s hits=%d/%d tpl=0x%zX id=0x%zX hp=0x%zX ctrl=0x%zX di=0x%zX vc=0x%zX",
        hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
        gOffTemplateId, gOffMobId, gOffHpPct, gOffMobCtrlState, gOffDamageInfoList, gOffVecCtrl);

    // DamageInfo + VecCtrl.AbsPos
    void* diKlass = x::runtime::il2cpp::FindClass("", kDamageInfoClass);
    void* vcKlass = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    int diHits = 0;
    auto diHit = [&](void* klass, const char* hash, size_t fb, size_t* slot) {
        if (MobFieldOffHit(klass, hash, fb, slot)) ++diHits;
    };
    diHit(diKlass, kHashDiDelayed, kFbDiDelayed, &gOffDiDelayed);
    diHit(diKlass, kHashDiCharId, kFbDiCharId, &gOffDiCharId);
    diHit(diKlass, kHashDiSkillId, kFbDiSkillId, &gOffDiSkillId);
    diHit(diKlass, kHashDiHitAction, kFbDiHitAction, &gOffDiHitAction);
    diHit(diKlass, kHashDiDamage, kFbDiDamage, &gOffDiDamage);
    diHit(diKlass, kHashDiAttackIdx, kFbDiAttackIdx, &gOffDiAttackIdx);
    diHit(diKlass, kHashDiMoveType, kFbDiMoveType, &gOffDiMoveType);
    diHit(vcKlass, kHashVcAp, kFbVcAp, &gOffVcAp);
    x::runtime::LogI("MobPool", "di/vcAp path=%s hits=%d/8 dmg=0x%zX vcAp=0x%zX",
                     diHits == 8 ? "meta" : (diHits ? "meta-partial" : "fallback"), diHits,
                     gOffDiDamage, gOffVcAp);

}

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

    if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gMobPoolKlass);

    void* staticsKlass = gMobPoolKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gMobPoolKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
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
    // 池槽复用瞬间坐标可能未种好：NaN / 超界 / 原点 → 贴过去会把人甩出图。
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > kMaxPosAbs ||
        std::fabs(y) > kMaxPosAbs || (std::fabs(x) < kMinPosAbs && std::fabs(y) < kMinPosAbs)) {
        static DWORD sDirty = 0;
        const DWORD now = GetTickCount();
        if (!sDirty || now - sDirty > 3000) {
            sDirty = now;
            x::runtime::LogW("MobPool",
                             "skip dirty pool pos id=%d tpl=%d ready=%d hp=%d pos=(%.0f,%.0f)", id,
                             ReadI32(mob, kOffTemplateId), ready ? 1 : 0, hpPct, x, y);
        }
        return false;
    }

    out.ptr = mob;
    out.id = id;
    out.templateId = ReadI32(mob, kOffTemplateId);
    if (out.templateId == kSpecialTplExclude) return false;
    out.hpPct = hpPct;
    out.lastHitted = ReadI32(mob, kOffLastHitted);
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
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
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


void EnsureUiLifeFieldOffsets() {
    if (gUiLifeFieldResolved.load(std::memory_order_acquire)) return;
    gUiLifeFieldResolved.store(true, std::memory_order_release);
    if (!x::runtime::il2cpp::Ensure()) return;
    if (!gUiHpTagKlass) gUiHpTagKlass = FindClass(kUiHpTagClass);
    void* lifeKlass = x::runtime::il2cpp::FindClass("", kMapLifeClass);
    int hits = 0;
    if (MobFieldOffHit(gUiHpTagKlass, kHashUiHpTagMobId, kFbUiHpTagMobId, &gOffUiHpTagMobId))
        ++hits;
    if (MobFieldOffHit(gUiHpTagKlass, kHashUiHpTagCurHp, kFbUiHpTagCurHp, &gOffUiHpTagCurHp))
        ++hits;
    if (MobFieldOffHit(gUiHpTagKlass, kHashUiHpTagMaxHp, kFbUiHpTagMaxHp, &gOffUiHpTagMaxHp))
        ++hits;
    if (MobFieldOffHit(lifeKlass, kHashLifeType, kFbLifeType, &gOffLifeType)) ++hits;
    constexpr int kExpect = 4;
    x::runtime::LogI(
        "MobPool",
        "uiHp/life fields path=%s hits=%d/%d mobId=0x%zX cur=0x%zX max=0x%zX lifeTy=0x%zX",
        hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
        gOffUiHpTagMobId, gOffUiHpTagCurHp, gOffUiHpTagMaxHp, gOffLifeType);
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
    gUiHpTagKlass = FindClass(kUiHpTagClass);
    EnsureMobFieldOffsets();
    EnsureUiLifeFieldOffsets();
    EnsurePoolDictOffset();
    if (gMobKlass && gClassGetType && gTypeGetObject) {
        __try {
            void* t = gClassGetType(gMobKlass);
            if (t) gMobTypeObj = x::runtime::managed_main::TypeGetObject(gTypeGetObject, t, 2000);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (gUiHpTagKlass && gClassGetType && gTypeGetObject) {
        __try {
            void* t = gClassGetType(gUiHpTagKlass);
            if (t) gUiHpTagTypeObj = x::runtime::managed_main::TypeGetObject(gTypeGetObject, t, 2000);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const bool ok = gMobPoolKlass != nullptr || gMobKlass != nullptr;
    if (!ok) {
        // 客户端换包后类哈希 miss 时，一眼能看出是哪颗死了（勿与 field-off 混淆）。
        static DWORD sLastMissLog = 0;
        const DWORD now = GetTickCount();
        if (now - sLastMissLog >= 15000) {
            sLastMissLog = now;
            x::runtime::LogW("MobPort",
                             "FindClass miss pool=%d mob=%d mapData=%d uiHpTag=%d "
                             "(pool=%.12s mob=%.12s)",
                             gMobPoolKlass ? 1 : 0, gMobKlass ? 1 : 0, gMapDataKlass ? 1 : 0,
                             gUiHpTagKlass ? 1 : 0, kMobPoolClass, kMobClass);
        }
    }
    return ok;
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
                     "bound poolKlass=%p mobKlass=%p mapDataKlass=%p uiHpTagKlass=%p FindAll=%p "
                     "(WM=world_port)",
                     gMobPoolKlass, gMobKlass, gMapDataKlass, gUiHpTagKlass, gFindAll);
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

    AbsHp abs{};
    if (ResolveAbsHp(lite.id, lite.templateId, lite.hpPct, abs, /*refreshUi=*/false)) {
        lite.absHp = abs.cur;
        lite.absMaxHp = abs.max;
        lite.absSrc = abs.src;
    }
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

int64_t LookupTemplateMaxHp(int32_t templateId) {
    static std::once_flag once;
    static std::unordered_map<int32_t, int64_t> table;
    std::call_once(once, [] {
        std::string path = x::runtime::GetBinDir() ? x::runtime::GetBinDir() : "";
        if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
        path += "dataservice\\mob_stats.tsv";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            x::runtime::LogW("MobPool", "mob_stats.tsv missing path=%s", path.c_str());
            return;
        }
        std::string line;
        size_t n = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            // mob_id \t level \t maxHP \t ...
            const size_t t0 = line.find('\t');
            if (t0 == std::string::npos) continue;
            const size_t t1 = line.find('\t', t0 + 1);
            if (t1 == std::string::npos) continue;
            const size_t t2 = line.find('\t', t1 + 1);
            const int32_t mid = static_cast<int32_t>(atoi(line.c_str()));
            const char* hpStr = line.c_str() + t1 + 1;
            const int64_t maxHp = _atoi64(hpStr);
            if (mid > 0 && maxHp > 0) {
                table[mid] = maxHp;
                ++n;
            }
            (void)t2;
        }
        x::runtime::LogI("MobPool", "mob_stats loaded n=%zu path=%s", n, path.c_str());
    });
    if (templateId <= 0) return 0;
    const auto it = table.find(templateId);
    return it == table.end() ? 0 : it->second;
}

namespace {

struct AbsHpCacheEntry {
    int64_t cur = 0;
    int64_t max = 0;
    uint64_t tickMs = 0;
    AbsHpSrc src = AbsHpSrc::UiHpTagCache;
};

// Same-map overnight OID churn used to grow without bound; cap + TTL keep it MB-scale.
constexpr size_t kAbsHpCacheCap = 768;
constexpr uint64_t kAbsHpCacheTtlMs = 180000;  // 3 min

std::mutex gAbsHpMu;
std::unordered_map<int32_t, AbsHpCacheEntry> gAbsHpByMob;

void PruneAbsHpCacheUnlocked(uint64_t nowMs) {
    for (auto it = gAbsHpByMob.begin(); it != gAbsHpByMob.end();) {
        if (nowMs > it->second.tickMs && (nowMs - it->second.tickMs) > kAbsHpCacheTtlMs)
            it = gAbsHpByMob.erase(it);
        else
            ++it;
    }
    while (gAbsHpByMob.size() > kAbsHpCacheCap) {
        auto oldest = gAbsHpByMob.begin();
        for (auto it = gAbsHpByMob.begin(); it != gAbsHpByMob.end(); ++it) {
            if (it->second.tickMs < oldest->second.tickMs) oldest = it;
        }
        gAbsHpByMob.erase(oldest);
    }
}

void NoteAbsHpCache(int32_t mobId, int64_t cur, int64_t max, AbsHpSrc src) {
    if (mobId <= 0 || max <= 0 || cur < 0) return;
    if (cur > max) cur = max;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(gAbsHpMu);
    AbsHpCacheEntry& e = gAbsHpByMob[mobId];
    e.cur = cur;
    e.max = max;
    e.tickMs = now;
    e.src = src;
    if (gAbsHpByMob.size() > kAbsHpCacheCap) PruneAbsHpCacheUnlocked(now);
}

bool LookupAbsHpCache(int32_t mobId, int64_t& cur, int64_t& max, AbsHpSrc& src) {
    if (mobId <= 0) return false;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(gAbsHpMu);
    const auto it = gAbsHpByMob.find(mobId);
    if (it == gAbsHpByMob.end()) return false;
    if (now > it->second.tickMs && (now - it->second.tickMs) > kAbsHpCacheTtlMs) {
        gAbsHpByMob.erase(it);
        return false;
    }
    cur = it->second.cur;
    max = it->second.max;
    src = it->second.src;
    return max > 0 && cur >= 0;
}

int64_t AbsHpFromPctLocal(int64_t maxHp, int32_t pct) {
    if (maxHp <= 0 || pct <= 0) return 0;
    if (pct >= 100) return maxHp;
    return (maxHp * static_cast<int64_t>(pct) + 50) / 100;
}

bool EnsureUiHpTagType() {
    if (gUiHpTagTypeObj) return true;
    if (!gUiHpTagKlass) gUiHpTagKlass = FindClass(kUiHpTagClass);
    if (!gUiHpTagKlass || !gClassGetType || !gTypeGetObject) return false;
    __try {
        void* t = gClassGetType(gUiHpTagKlass);
        if (t) gUiHpTagTypeObj = x::runtime::managed_main::TypeGetObject(gTypeGetObject, t, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return gUiHpTagTypeObj != nullptr;
}

}  // namespace

const char* AbsHpSrcName(AbsHpSrc src) {
    switch (src) {
        case AbsHpSrc::UiHpTag:
            return "ui";
        case AbsHpSrc::UiHpTagCache:
            return "cache";
        case AbsHpSrc::PctEstimate:
            return "pct";
        default:
            return "none";
    }
}

void InvalidateAbsHpCache(int32_t mobId) {
    if (mobId <= 0) return;
    std::lock_guard<std::mutex> lock(gAbsHpMu);
    gAbsHpByMob.erase(mobId);
}

void ClearAbsHpCache() {
    std::lock_guard<std::mutex> lock(gAbsHpMu);
    gAbsHpByMob.clear();
}

bool TryReadUiHpTag(int32_t preferMobId, UiHpTagSnap& out) {
    out = UiHpTagSnap{};
    if (!EnsureBound()) return false;
    if (!gFindAll) return false;
    if (!EnsureUiHpTagType()) return false;

    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gUiHpTagTypeObj, 1500);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!arr) return false;

    const uintptr_t n = ArrayLen(arr);
    out.scanned = static_cast<int>(n > 64 ? 64 : n);

    UiHpTagSnap first{};
    UiHpTagSnap prefer{};
    for (uintptr_t i = 0; i < n && i < 64; ++i) {
        void* tag = ArrayAt(arr, i);
        if (!LooksLikeHeapPtr(tag)) continue;
        if (gUiHpTagKlass && !ObjKlassIs(tag, gUiHpTagKlass)) continue;
        if (!UnityObjectAlive(tag)) continue;

        const int32_t mobId = ReadI32(tag, kOffUiHpTagMobId);
        const int32_t cur = ReadI32(tag, kOffUiHpTagCurHp);
        const int32_t maxHp = ReadI32(tag, kOffUiHpTagMaxHp);
        if (mobId == 0 || maxHp <= 0) continue;
        if (cur < 0) continue;

        ++out.valid;
        NoteAbsHpCache(mobId, cur, maxHp, AbsHpSrc::UiHpTagCache);

        UiHpTagSnap snap{};
        snap.ok = true;
        snap.mobId = mobId;
        snap.cachedHp = cur;
        snap.cachedMaxHp = maxHp;
        snap.tagPtr = tag;
        if (!first.ok) first = snap;
        if (preferMobId != 0 && mobId == preferMobId && !prefer.ok) prefer = snap;
    }

    if (prefer.ok) {
        out.ok = true;
        out.mobId = prefer.mobId;
        out.cachedHp = prefer.cachedHp;
        out.cachedMaxHp = prefer.cachedMaxHp;
        out.tagPtr = prefer.tagPtr;
        return true;
    }
    if (first.ok) {
        out.ok = true;
        out.mobId = first.mobId;
        out.cachedHp = first.cachedHp;
        out.cachedMaxHp = first.cachedMaxHp;
        out.tagPtr = first.tagPtr;
        return true;
    }
    return false;
}

bool ResolveAbsHp(int32_t mobId, int32_t templateId, int32_t hpPct, AbsHp& out, bool refreshUi) {
    out = AbsHp{};
    if (refreshUi) {
        UiHpTagSnap tag{};
        if (TryReadUiHpTag(mobId, tag) && tag.ok) {
            if (mobId == 0 || tag.mobId == mobId) {
                out.ok = true;
                out.src = AbsHpSrc::UiHpTag;
                out.cur = tag.cachedHp;
                out.max = tag.cachedMaxHp;
                return true;
            }
            // 扫到别的怪的 tag：缓存已刷新；继续按 mobId 查缓存/估计。
        }
    }

    int64_t cur = 0, max = 0;
    AbsHpSrc cachedSrc = AbsHpSrc::UiHpTagCache;
    if (LookupAbsHpCache(mobId, cur, max, cachedSrc)) {
        out.ok = true;
        out.src = cachedSrc;
        out.cur = cur;
        out.max = max;
        return true;
    }

    const int64_t tplMax = LookupTemplateMaxHp(templateId);
    if (tplMax > 0 && hpPct > 0) {
        out.ok = true;
        out.src = AbsHpSrc::PctEstimate;
        out.max = tplMax;
        out.cur = AbsHpFromPctLocal(tplMax, hpPct);
        return true;
    }
    return false;
}

bool TryReadDamageInfoList(void* mob, DamageInfoSnap& out) {
    out = DamageInfoSnap{};
    if (!LooksLikeHeapPtr(mob)) return false;
    EnsureMobFieldOffsets();
    EnsureUiLifeFieldOffsets();

    void* list = ReadPtr(mob, kOffDamageInfoList);
    if (!LooksLikeHeapPtr(list)) return false;
    void* items = ReadPtr(list, kOffListItems);
    const int size = ReadI32(list, kOffListSize);
    if (!LooksLikeHeapPtr(items) || size < 0 || size > 256) return false;

    out.listSize = size;
    out.ok = true;

    int64_t sum = 0;
    for (int i = 0; i < size; ++i) {
        void* di = ArrayAt(items, static_cast<uintptr_t>(i));
        if (!LooksLikeHeapPtr(di)) continue;
        const int32_t dmg = ReadI32(di, kOffDiDamage);
        if (dmg > 0) sum += dmg;
    }
    out.sumDamage = sum;

    const int take = size < kMaxDamageInfoProbe ? size : kMaxDamageInfoProbe;
    const int start = size - take;
    for (int i = 0; i < take; ++i) {
        void* di = ArrayAt(items, static_cast<uintptr_t>(start + i));
        if (!LooksLikeHeapPtr(di)) continue;
        DamageInfoLite& e = out.items[out.count];
        e.delayed = ReadF32(di, kOffDiDelayed);
        e.charId = static_cast<uint32_t>(ReadI32(di, kOffDiCharId));
        e.skillId = ReadI32(di, kOffDiSkillId);
        e.hitAction = ReadI32(di, kOffDiHitAction);
        e.damage = ReadI32(di, kOffDiDamage);
        e.attackIdx = ReadI32(di, kOffDiAttackIdx);
        e.moveType = ReadI32(di, kOffDiMoveType);
        e.raw[0] = ReadI32(di, 0x14);
        e.raw[1] = ReadI32(di, 0x18);
        e.raw[2] = ReadI32(di, 0x1C);
        e.raw[3] = ReadI32(di, 0x24);
        e.raw[4] = ReadI32(di, 0x2C);
        e.raw[5] = ReadI32(di, 0x34);
        out.lastDamage = e.damage;
        ++out.count;
    }
    return true;
}

}  // namespace x::features::ports::mob
