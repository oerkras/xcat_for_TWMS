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

// Class hashes remounted 2026-08-06（旧 08-04：MobPool=f4afa0ce… Mob=a803dc63… MapData=a08e1596…）。
// 交叉：Mob TDI:1507；MobPool→Dictionary<int,Mob>@+0x10；字段偏移未漂（deadType@0x1B4 等）。
// 误用其它 Map* 壳会通过 FindClass 但 ObjKlassIs 失败 → M/mapId 全废。
constexpr char kMobPoolClass[] =
    "d7149fc16969fb71084fcbbf282b1165ef1778b86b8b5e0f2984145af2bcf9c";
constexpr char kMobClass[] =
    "dd472cd442d83d6a25198278fa3e0a09d3a98a6db2e05d1b12d508d43db4cb5";
constexpr char kMapDataClass[] =
    "dd4cd32d9aef89b376fffeab50e0a12fd9bae83dbecfb9ee663b0962b06f87b";
// UIHpTag（dump 属性 "UIHpTag"；绝对 cur/max 缓存）
constexpr char kUiHpTagClass[] =
    "d262ca123c6942dd62c6c8c153d7fdafcc54de8155ee30008182d4a81cdf441";

// Unity FindAll → x::runtime::il2cpp::kRvaFindObjectsOfTypeAll（il2cpp_bind.h SSOT）

// MobPool
constexpr size_t kFbPoolDict = 0x10;  // Dictionary<int,Mob>
constexpr char kHashPoolDict[] =
    "dc73f061c2088f396c4d4e145efecc56e03a47a2cf8692b559701b05d4aefd8";
size_t gOffPoolDict = kFbPoolDict;
#define kOffPoolDict (gOffPoolDict)

// FieldActorBase / VecCtrlOwner / Mob：hash → field_get_offset（dump fallback）
constexpr size_t kOffCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr

constexpr char kHashVecCtrl[] =
    "<a53533cecb18cbe1cddb1ee6c9adf83e4397ed25de00154905063e15bc1d11f>k__BackingField";
constexpr char kHashPos[] =
    "dfe2e5f14ea04b0ca19e131b66bb4d53e06ca0c22744c299a991cd322e915e4";
constexpr char kHashTemplateId[] =
    "bee51a176937269df4a90c615347dd2e18ef03f7c18eee84fe5bae831864aaa";
constexpr char kHashIsReady[] =
    "<aace0a900d1fc1dcf8cb668ac52fea91c20518721e2d49c812f64e6f4f65459>k__BackingField";
constexpr char kHashPvcActive[] =
    "f9752616061ab01c21cd1ba0f5a74425326a63f722336107a3f40815af2da14";
constexpr char kHashMobId[] =
    "b22341ca9332866c7c3e33e2065a3cb5a770b369cb30df35de9660865554c19";
constexpr char kHashDeadType[] =
    "f39f5054cb70f572827ac8ec20afde4a3f1fba7e82ec349b450030ae698f3d4";
constexpr char kHashDamageInfoList[] =
    "eef68f7ad84fa5427a0d7ea541ddf37a6761761b2cffc02c12076f154756cb1";
constexpr char kHashLastHitted[] =
    "e7785b4c36ce35d0d6583c62c95578e641043701649845d1fcc53383579d926";
constexpr char kHashHpPct[] =
    "<b39844d84499f5c38f07790ddb72dcde756a83b054393a1fd1bb4cb123a1d81>k__BackingField";
constexpr char kHashMobCtrlState[] =
    "cbcb0a8a8c82850097efae235b269bd49aead8a604be412b785ea2850eded52";
// FindHit 同构门参考：inViewSplit@0x100 / suspended@0x240（08-13：旧 0x1B8 bool 已搬走）。
// suspended 仍挡入榜；inView 只写入 MobLite（出刀归 FindHit，不挡 n）。
constexpr char kHashInViewSplit[] =
    "ac5703a0e95cc0497f043bd6d0c9dbed9069c78afc6927facaff10bdf194665";
constexpr char kHashSuspended[] =
    "<a057b020e8767ff0a11a5eef0c39037f931b5946be6575015990d818c036edd>k__BackingField";

constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kFbPos = 0x64;
constexpr size_t kFbTemplateId = 0xB0;
constexpr size_t kFbIsReady = 0xEC;
constexpr size_t kFbPvcActive = 0xF0;
constexpr size_t kFbInViewSplit = 0x100;
constexpr size_t kFbMobId = 0x134;
constexpr size_t kFbDeadType = 0x1B4;
constexpr size_t kFbSuspended = 0x240;
constexpr size_t kFbDamageInfoList = 0x1D8;
constexpr size_t kFbLastHitted = 0x200;
constexpr size_t kFbHpPct = 0x238;
constexpr size_t kFbMobCtrlState = 0xE8;

size_t gOffVecCtrl = kFbVecCtrl;
size_t gOffPos = kFbPos;
size_t gOffTemplateId = kFbTemplateId;
size_t gOffIsReady = kFbIsReady;
size_t gOffPvcActive = kFbPvcActive;
size_t gOffInViewSplit = kFbInViewSplit;
size_t gOffMobId = kFbMobId;
size_t gOffDeadType = kFbDeadType;
size_t gOffSuspended = kFbSuspended;
size_t gOffDamageInfoList = kFbDamageInfoList;
size_t gOffLastHitted = kFbLastHitted;
size_t gOffHpPct = kFbHpPct;
size_t gOffMobCtrlState = kFbMobCtrlState;

#define kOffVecCtrl (gOffVecCtrl)
#define kOffPos (gOffPos)
#define kOffTemplateId (gOffTemplateId)
#define kOffIsReady (gOffIsReady)
#define kOffPvcActive (gOffPvcActive)
#define kOffInViewSplit (gOffInViewSplit)
#define kOffMobId (gOffMobId)
#define kOffDeadType (gOffDeadType)
#define kOffSuspended (gOffSuspended)
#define kOffDamageInfoList (gOffDamageInfoList)
#define kOffLastHitted (gOffLastHitted)
#define kOffHpPct (gOffHpPct)
#define kOffMobCtrlState (gOffMobCtrlState)

// 地图特殊体（日志 tpl=9999999）：不计入活怪 n / 不进 combat 缓存。
constexpr int32_t kSpecialTplExclude = 9999999;
// 与 player_combat 同口径：池化复用槽偶发 NaN / INT_MIN 级脏坐标（BIN land_miss）。
constexpr float kMaxPosAbs = 100000.f;

// DamageInfo / VecCtrl.AbsPos：hash → field_get_offset
// Remount 2026-08-06：VecCtrl TDI 1596 / AbsPos@0x98（与 drop/player_combat 同钉）
constexpr char kDamageInfoClass[] =
    "a59db15a033aca47e02b97b940cb3b342d965e1823605ee7cbc64c28febbf91";
constexpr char kVecCtrlClass[] =
    "fb50f6a1736ed7dc2ae31fe0164df2ac21372ae1ba8c5f346fa63e05fbeff6a";
constexpr char kHashDiDelayed[] =
    "cf46756b4be41d01e9eb7a90e08e876eda2962ba6a160540706f69d310be96d";  // DelayedProcess@0x10
constexpr char kHashDiCharId[] =
    "d81a9f380c8367b4a5696857a282bdbdc8ab8fe00d445845739dde3f5fb7628";
constexpr char kHashDiSkillId[] =
    "dfd6ac61bac96dcf2698f7967b9845a1d8819c4036f67184b2d8a8f69eacaea";
constexpr char kHashDiHitAction[] =
    "e05ede9103a67e2dc106a20831196dafa9f1e64cae2a0dda8958f6c5f169c14";
constexpr char kHashDiDamage[] =
    "d08e7360c3c603d694bfa80d2b3896dd48a3a615bbb3006f61c8447ec94d799";
constexpr char kHashDiAttackIdx[] =
    "e0e859e3cc84ba484063b21063dd0b83fda01c17c9cdd3ad8f6a09f360f9c6c";
constexpr char kHashDiMoveType[] =
    "f69eee43a6b1c55c7283e290c50d6a96d287c102b6c0113be4f4b60d58f3056";
constexpr char kHashVcAp[] =
    "e399633b16dbf327df9b459015caf617aff8e505e4f1fb694acf87a011d4259";  // AbsPos start; Y=+8
// VecCtrl.Active@0x80：SetRemoteMob 置 false 后常留池（P0a §7.6）→ 不计入 n
constexpr char kHashVcActive[] =
    "<af1b5b42e79782209fe4a1b3b8991c9ae2267caf27316a66d737327bd79f885>k__BackingField";

constexpr size_t kFbDiDelayed = 0x10;
constexpr size_t kFbDiCharId = 0x14;
constexpr size_t kFbDiSkillId = 0x18;
constexpr size_t kFbDiHitAction = 0x1C;
constexpr size_t kFbDiDamage = 0x24;
constexpr size_t kFbDiAttackIdx = 0x2C;
constexpr size_t kFbDiMoveType = 0x34;
constexpr size_t kFbVcActive = 0x80;
constexpr size_t kFbVcAp = 0x98;  // AbsPos; Y = Ap+8

size_t gOffDiDelayed = kFbDiDelayed;
size_t gOffDiCharId = kFbDiCharId;
size_t gOffDiSkillId = kFbDiSkillId;
size_t gOffDiHitAction = kFbDiHitAction;
size_t gOffDiDamage = kFbDiDamage;
size_t gOffDiAttackIdx = kFbDiAttackIdx;
size_t gOffDiMoveType = kFbDiMoveType;
size_t gOffVcActive = kFbVcActive;
size_t gOffVcAp = kFbVcAp;

#define kOffDiDelayed (gOffDiDelayed)
#define kOffDiCharId (gOffDiCharId)
#define kOffDiSkillId (gOffDiSkillId)
#define kOffDiHitAction (gOffDiHitAction)
#define kOffDiDamage (gOffDiDamage)
#define kOffDiAttackIdx (gOffDiAttackIdx)
#define kOffDiMoveType (gOffDiMoveType)
#define kOffVcActive (gOffVcActive)
#define kOffVcApX (gOffVcAp)
#define kOffVcApY (gOffVcAp + 8)

// UIHpTag（相对 CMS +8：基类多一截）
// UIHpTag 绝对血量槽（hash → field_get_offset；dump fallback）
constexpr size_t kFbUiHpTagMobId = 0xC8;
constexpr size_t kFbUiHpTagCurHp = 0xD4;
constexpr size_t kFbUiHpTagMaxHp = 0xD8;
constexpr char kHashUiHpTagMobId[] =
    "f76099fb060a861ce2ce12bf87902db6fc128180242bc587b374fabc763a2d4";
constexpr char kHashUiHpTagCurHp[] =
    "e7b0a907277feaa744ae7e1fdf6d3c47638643fca867e822b08bdba73564647";
constexpr char kHashUiHpTagMaxHp[] =
    "cc6b72b1c396041a5130a298db2c17508054d72ed7c4061ce9451cc4a04e923";
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
    "cdb411df9e4657c68f35d2afebb2eb6a70a5de9eea5918aa7283a96a9cb648b";
constexpr char kHashLifeType[] =
    "<bcb26b22743e85de9b74a2e293021d28c832a3443424f111c87072d3eee696d>k__BackingField";
constexpr char kHashLifeX[] =
    "<ad753db403be75d9d25ad7138c870e8b21da280deb86656ec689a588514e962>k__BackingField";
constexpr char kHashLifeY[] =
    "<c85b34c5582146bba4cb69bb9427d650605fddce25843184a17582d033a2ca5>k__BackingField";
constexpr char kHashLifeRx0[] =
    "<eeeb9e9bec1ee51b30e0755ff7f07719654e3cf1bad7579a4b7ffc05d3e9037>k__BackingField";
constexpr char kHashLifeRx1[] =
    "<ee556b7fdc58df031c19d9faf78cc766165de2bd4ffe07c528e640790c5f0db>k__BackingField";
constexpr size_t kFbLifeType = 0x20;
constexpr size_t kFbLifeX = 0x24;
constexpr size_t kFbLifeY = 0x28;
constexpr size_t kFbLifeRx0 = 0x38;
constexpr size_t kFbLifeRx1 = 0x3C;
size_t gOffLifeType = kFbLifeType;
size_t gOffLifeX = kFbLifeX;
size_t gOffLifeY = kFbLifeY;
size_t gOffLifeRx0 = kFbLifeRx0;
size_t gOffLifeRx1 = kFbLifeRx1;
#define kOffLifeType (gOffLifeType)
#define kOffLifeX (gOffLifeX)
#define kOffLifeY (gOffLifeY)
#define kOffLifeRx0 (gOffLifeRx0)
#define kOffLifeRx1 (gOffLifeRx1)
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
    hit(kHashInViewSplit, kFbInViewSplit, &gOffInViewSplit);
    hit(kHashMobId, kFbMobId, &gOffMobId);
    hit(kHashDeadType, kFbDeadType, &gOffDeadType);
    hit(kHashSuspended, kFbSuspended, &gOffSuspended);
    hit(kHashDamageInfoList, kFbDamageInfoList, &gOffDamageInfoList);
    hit(kHashLastHitted, kFbLastHitted, &gOffLastHitted);
    hit(kHashHpPct, kFbHpPct, &gOffHpPct);
    hit(kHashMobCtrlState, kFbMobCtrlState, &gOffMobCtrlState);
    constexpr int kExpect = 13;
    x::runtime::LogI(
        "MobPool",
        "mob fields path=%s hits=%d/%d tpl=0x%zX id=0x%zX hp=0x%zX ctrl=0x%zX di=0x%zX vc=0x%zX "
        "inView=0x%zX sus=0x%zX",
        hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
        gOffTemplateId, gOffMobId, gOffHpPct, gOffMobCtrlState, gOffDamageInfoList, gOffVecCtrl,
        gOffInViewSplit, gOffSuspended);

    // DamageInfo + VecCtrl.AbsPos / Active
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
    diHit(vcKlass, kHashVcActive, kFbVcActive, &gOffVcActive);
    x::runtime::LogI("MobPool", "di/vcAp path=%s hits=%d/9 dmg=0x%zX vcAp=0x%zX vcAct=0x%zX",
                     diHits == 9 ? "meta" : (diHits ? "meta-partial" : "fallback"), diHits,
                     gOffDiDamage, gOffVcAp, gOffVcActive);

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

// FillLite 失败码：供 Collect 做 raw-n 归因。
enum class LiteFail : uint8_t {
    Ok = 0,
    Other,      // 野指针 / 错 klass / id=0 / 特殊 tpl
    NotReady,
    DeadType,
    Hp,
    Suspended,
    DirtyPos,
};

struct LiteDiag {
    int id = 0;
    int tpl = 0;
    int hpPct = 0;
    int deadType = 0;
    float x = 0.f;
    float y = 0.f;
    uint8_t ready = 0;
    uint8_t inView = 0;
    uint8_t suspended = 0;
};

char LiteFailTag(LiteFail f) {
    switch (f) {
        case LiteFail::NotReady: return 'R';
        case LiteFail::DeadType: return 'D';
        case LiteFail::Hp: return 'H';
        case LiteFail::Suspended: return 'S';
        case LiteFail::DirtyPos: return 'P';
        default: return 'O';
    }
}

bool SampleLooksAlive(const FillRejectSample& s) {
    return s.ready && s.deadType == 0 && s.hpPct > 0 && s.id != 0;
}

void NoteFillReject(Snapshot& snap, LiteFail why, const LiteDiag& d) {
    switch (why) {
        case LiteFail::NotReady: ++snap.rejNotReady; break;
        case LiteFail::DeadType: ++snap.rejDeadType; break;
        case LiteFail::Hp: ++snap.rejHp; break;
        case LiteFail::Suspended: ++snap.rejSuspended; break;
        case LiteFail::DirtyPos: ++snap.rejDirty; break;
        default: ++snap.rejOther; break;
    }
    FillRejectSample cand{};
    cand.id = d.id;
    cand.tpl = d.tpl;
    cand.hpPct = d.hpPct;
    cand.deadType = d.deadType;
    cand.x = d.x;
    cand.y = d.y;
    cand.ready = d.ready;
    cand.inView = d.inView;
    cand.suspended = d.suspended;
    cand.why = LiteFailTag(why);
    if (snap.rejSampleN < kMaxFillRejectSamples) {
        snap.rejSamples[snap.rejSampleN++] = cand;
        return;
    }
    // 槽满：优先留像活怪的拒样（ready+有血），少被尸体占满。
    if (!SampleLooksAlive(cand)) return;
    for (int i = 0; i < snap.rejSampleN; ++i) {
        if (!SampleLooksAlive(snap.rejSamples[i])) {
            snap.rejSamples[i] = cand;
            return;
        }
    }
}

LiteFail FillLiteEx(void* mob, MobLite& out, LiteDiag* diag) {
    if (!UnityObjectAlive(mob)) return LiteFail::Other;
    if (gMobKlass && !ObjKlassIs(mob, gMobKlass)) return LiteFail::Other;

    const int id = ReadI32(mob, kOffMobId);
    if (id == 0) return LiteFail::Other;

    const int deadType = ReadI32(mob, kOffDeadType);
    const int hpPct = ReadI32(mob, kOffHpPct);
    const bool ready = ReadU8(mob, kOffIsReady) != 0;
    const uint8_t inView = ReadU8(mob, kOffInViewSplit);
    const uint8_t sus = ReadU8(mob, kOffSuspended);
    float x = 0.f, y = 0.f;
    ReadMobPos(mob, x, y);
    if (diag) {
        diag->id = id;
        diag->tpl = ReadI32(mob, kOffTemplateId);
        diag->hpPct = hpPct;
        diag->deadType = deadType;
        diag->x = x;
        diag->y = y;
        diag->ready = ready ? 1 : 0;
        diag->inView = inView != 0 ? 1 : 0;
        diag->suspended = sus != 0 ? 1 : 0;
    }

    // 未就绪 / 尸体 / 空血：不入活怪榜（曾被乱码注释吞掉 ready 门 → 池脏坐标贴飞）。
    if (!ready) return LiteFail::NotReady;
    if (deadType != 0) return LiteFail::DeadType;
    if (hpPct <= 0) return LiteFail::Hp;

    // suspended：Init/暂挂中，不进 n。
    // 注意：不要用 VecCtrl.Active@0x80——SetRemote 会置 false，但怪仍可被 FindHit 命中；
    // BIN 1000002：加 Active 后门 → raw=9 n=0，打怪全 miss。
    if (sus != 0) return LiteFail::Suspended;

    // ★ inView 不再挡入榜。FindHitMobInRect 要 inView——那是**出刀**门，不是「场上有没有怪」。
    // BIN 2026-08-08：上层清空后下层满血怪 inView=0 → n=0 → 旋翼宽限落地；同一只稍后仍可杀。
    // 选怪/旋翼认活怪；命中仍交给官方 FindHit（inView 恢复或贴近后再中）。

    // 池槽复用瞬间坐标可能未种好：NaN / 超界 / 原点 → 贴过去会把人甩出图。
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > kMaxPosAbs ||
        std::fabs(y) > kMaxPosAbs || (std::fabs(x) < kMinPosAbs && std::fabs(y) < kMinPosAbs)) {
        static DWORD sDirty = 0;
        const DWORD now = GetTickCount();
        if (!sDirty || now - sDirty > 3000) {
            sDirty = now;
            x::runtime::LogW("MobPool",
                             "skip dirty pool pos id=%d tpl=%d ready=%d hp=%d pos=(%.0f,%.0f)", id,
                             diag ? diag->tpl : ReadI32(mob, kOffTemplateId), ready ? 1 : 0, hpPct, x,
                             y);
        }
        return LiteFail::DirtyPos;
    }

    out.ptr = mob;
    out.id = id;
    out.templateId = diag ? diag->tpl : ReadI32(mob, kOffTemplateId);
    if (out.templateId == kSpecialTplExclude) return LiteFail::Other;
    out.hpPct = hpPct;
    out.lastHitted = ReadI32(mob, kOffLastHitted);
    out.deadType = deadType;
    out.ctrl = ReadI32(mob, kOffMobCtrlState);
    out.x = x;
    out.y = y;
    out.ready = ready;
    out.inView = inView != 0;
    return LiteFail::Ok;
}

bool FillLite(void* mob, MobLite& out) { return FillLiteEx(mob, out, nullptr) == LiteFail::Ok; }

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
        LiteDiag diag{};
        const LiteFail fail = FillLiteEx(mob, lite, &diag);
        if (fail != LiteFail::Ok) {
            NoteFillReject(snap, fail, diag);
            continue;
        }
        if (!PushLite(snap, lite)) {
            if (snap.truncated) break;
            continue;
        }
        if (!lite.inView) ++snap.nInView0;
    }
    snap.rawDict = raw;
    return snap.count;
}

int CollectFromFindAll(Snapshot& snap) {
    // InterStage / 卸图：禁 Mob FindAll（主线程扫对象拖黑屏）。
    if (!world::IsPlayReady()) return 0;
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
        LiteDiag diag{};
        const LiteFail fail = FillLiteEx(mob, lite, &diag);
        if (fail != LiteFail::Ok) {
            NoteFillReject(snap, fail, diag);
            continue;
        }
        if (!PushLite(snap, lite)) {
            if (snap.truncated) break;
            continue;
        }
        if (!lite.inView) ++snap.nInView0;
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

void CopySpawnMeta(Snapshot& dst, const Snapshot& src) {
    dst.spawnSlots = src.spawnSlots;
    dst.mapId = src.mapId;
    dst.lifeMob = src.lifeMob;
    dst.lifeAll = src.lifeAll;
    dst.spawnPointN = src.spawnPointN;
    if (src.spawnPointN > 0) {
        const int n = src.spawnPointN < kMaxSpawnPoints ? src.spawnPointN : kMaxSpawnPoints;
        std::memcpy(dst.spawnPoints, src.spawnPoints, sizeof(SpawnPoint) * static_cast<size_t>(n));
        dst.spawnPointN = n;
    }
}

// Count Mob entries in MapData.LifeList; optionally fill spawn XY/Rx. -1 on failure.
int CountLifeMobSlotsFromMapData(void* mapData, int* outAll, int* outMapId, SpawnPoint* pts,
                                 int cap, int* outPts) {
    if (outAll) *outAll = -1;
    if (outMapId) *outMapId = 0;
    if (outPts) *outPts = 0;
    if (!LooksLikeHeapPtr(mapData)) return -1;
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
    int filled = 0;
    for (int i = 0; i < size; ++i) {
        void* life = ArrayAt(items, (uintptr_t)i);
        if (!LooksLikeHeapPtr(life)) continue;
        ++allN;
        const int ty = ReadI32(life, kOffLifeType);
        if (ty != kLifeTypeMob) continue;
        ++mobN;
        if (!pts || cap <= 0 || filled >= cap) continue;
        pts[filled].x = static_cast<float>(ReadI32(life, kOffLifeX));
        pts[filled].y = static_cast<float>(ReadI32(life, kOffLifeY));
        pts[filled].rx0 = ReadI32(life, kOffLifeRx0);
        pts[filled].rx1 = ReadI32(life, kOffLifeRx1);
        ++filled;
    }
    if (outAll) *outAll = allN;
    if (outPts) *outPts = filled;
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
    if (MobFieldOffHit(lifeKlass, kHashLifeX, kFbLifeX, &gOffLifeX)) ++hits;
    if (MobFieldOffHit(lifeKlass, kHashLifeY, kFbLifeY, &gOffLifeY)) ++hits;
    if (MobFieldOffHit(lifeKlass, kHashLifeRx0, kFbLifeRx0, &gOffLifeRx0)) ++hits;
    if (MobFieldOffHit(lifeKlass, kHashLifeRx1, kFbLifeRx1, &gOffLifeRx1)) ++hits;
    constexpr int kExpect = 8;
    x::runtime::LogI(
        "MobPool",
        "uiHp/life fields path=%s hits=%d/%d mobId=0x%zX cur=0x%zX max=0x%zX "
        "lifeTy=0x%zX x=0x%zX y=0x%zX rx0=0x%zX rx1=0x%zX",
        hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
        gOffUiHpTagMobId, gOffUiHpTagCurHp, gOffUiHpTagMaxHp, gOffLifeType, gOffLifeX, gOffLifeY,
        gOffLifeRx0, gOffLifeRx1);
}

// Fill spawnSlots from LifeList; fallback to peak.
void FillSpawnSlots(Snapshot& out) {
    out.spawnSlots = -1;
    out.mapId = 0;
    out.lifeMob = -1;
    out.lifeAll = -1;
    out.spawnPointN = 0;

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
    int nPts = 0;
    const int lifeMob =
        CountLifeMobSlotsFromMapData(mapData, &lifeAll, &mapId, out.spawnPoints, kMaxSpawnPoints,
                                     &nPts);
    out.mapId = mapId;
    out.lifeMob = lifeMob;
    out.lifeAll = lifeAll;
    out.spawnPointN = nPts;

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

bool Collect(Snapshot& out, bool fillSpawnSlots) {
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
        if (fillSpawnSlots) {
            FillSpawnSlots(out);
        } else {
            Snapshot prev{};
            if (GetCached(prev) && prev.ok) {
                CopySpawnMeta(out, prev);
            } else {
                out.spawnSlots = gLastSpawnSlots.load(std::memory_order_acquire);
            }
        }
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
    // 热路径不 ResolveAbsHp（%×表 / 缓存）：出刀只靠 hpPct/lastHitted。
    // 脆皮早切需要 maxHP 时由 simple_combat 锁怪时按需查表。
    out = lite;
    return true;
}

bool StillSameLiveMob(void* mob, int32_t expectId, const char** why) {
    if (!LooksLikeHeapPtr(mob)) {
        if (why) *why = "dead";
        return false;
    }
    if (!gMobFieldResolved.load(std::memory_order_acquire)) {
        if (why) *why = "dead";
        return false;
    }
    MobLite lite{};
    const LiteFail fail = FillLiteEx(mob, lite, nullptr);
    if (fail != LiteFail::Ok) {
        if (why) *why = "dead";
        return false;
    }
    if (expectId != 0 && lite.id != expectId) {
        if (why) *why = "pool";
        return false;
    }
    if (why) *why = "";
    return true;
}

bool GetCached(Snapshot& out) {
    const int idx = gCacheIdx.load(std::memory_order_acquire);
    out = (idx == 0) ? gCacheA : gCacheB;
    return out.ok;
}

uint32_t GetCachedAgeMs() {
    Snapshot s{};
    if (!GetCached(s) || !s.ok || s.tickMs == 0) return 0xFFFFFFFFu;
    const uint64_t now = GetTickCount64();
    if (now < s.tickMs) return 0;
    const uint64_t age = now - s.tickMs;
    return age > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(age);
}

bool TryRefreshCacheLite(Snapshot& out) {
    out = Snapshot{};
    out.tickMs = GetTickCount64();
    // 必须已由 mob_scan 热身：禁 EnsureBound / Resolve（可能 RuntimeClassInit）与 FindAll。
    if (!gMobFieldResolved.load(std::memory_order_acquire)) return false;
    void* pool = gMobPool;
    if (!LooksLikeHeapPtr(pool) || !LooksLikeMobPool(pool)) return false;

    Snapshot prev{};
    const bool hadPrev = GetCached(prev) && prev.ok;

    CollectFromDict(pool, out);
    out.ok = true;
    if (hadPrev) {
        CopySpawnMeta(out, prev);
    } else {
        out.spawnSlots = gLastSpawnSlots.load(std::memory_order_acquire);
    }
    Publish(out);
    return true;
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
    const int mobN = CountLifeMobSlotsFromMapData(mapData, &all, &mapId, nullptr, 0, nullptr);
    if (mobN >= 0) return mobN;
    return PeakForMap(mapId);
}

int GetSpawnPeak() {
    return gPeakAlive > 0 ? gPeakAlive : -1;
}

bool NearMobLifeSlot(float x, float y, const Snapshot& snap, float nearPx, float* outDist) {
    if (outDist) *outDist = 1.e9f;
    if (snap.spawnPointN <= 0) return false;
    if (nearPx < 1.f) nearPx = 1.f;
    const float nearSq = nearPx * nearPx;
    bool hit = false;
    float bestSq = 1.e30f;
    const int n = snap.spawnPointN < kMaxSpawnPoints ? snap.spawnPointN : kMaxSpawnPoints;
    for (int i = 0; i < n; ++i) {
        const SpawnPoint& p = snap.spawnPoints[i];
        const float dx = x - p.x;
        const float dy = y - p.y;
        const float dSq = dx * dx + dy * dy;
        if (dSq < bestSq) bestSq = dSq;
        if (dSq <= nearSq) hit = true;
    }
    if (outDist) *outDist = std::sqrt(bestSq);
    return hit;
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
    if (!world::IsPlayReady()) return false;  // 卸图禁 FindAll
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
