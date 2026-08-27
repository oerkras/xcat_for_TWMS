// hit_geom_port — 出刀闸复刻 FindHit：afterimage Range ∩ Mob.GetBodyRect
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "hit_geom_port.h"

#include "attack_input_port.h"
#include "../final_attack_force/final_attack_force.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace x::features::ports::hit_geom {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// dump.cs Mob TDI 1507；与 mob_pool_port 同源。
constexpr char kMobClass[] =
    "d8b8258494049523e613374de0bd0539bb7318d4802873cd0c7dfbab192bf96";
constexpr uint32_t kRvaGetBodyRect = 0xF281A0;

// ActionManager 哈希 / 字段：melee_veto 已实机跑通（08-20 dump）。
constexpr char kHashActionManager[] =
    "e35f6343ebf368eebf40fc2ad5feeaeb3b9dc4ac6326ee032d935aadd50d4c5";
constexpr char kHashSingletonInstance[] =
    "c8072d39439eef6a06153eff03c75ee45009876f675c7c7ebc01a78bf7f0856";
constexpr size_t kOffActionMgrAfterImageMap = 0x20;
constexpr size_t kOffAfterImageRange = 0x18;
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kEntryStrideIntRect = 28;
constexpr size_t kEntryKeyOffIntRect = 8;
constexpr size_t kEntryValOffIntRect = 12;
constexpr size_t kEntryStrideStrRef = 24;
constexpr size_t kEntryValOffStrRef = 16;

constexpr int kMaxRange = 32;
constexpr DWORD kBodyCacheMs = 150;
constexpr DWORD kRangeCacheMs = 400;
constexpr DWORD kMapPumpGapMs = 750;  // 空表别每 tick 泵一次
constexpr DWORD kPumpWaitMs = 80;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

// rcx=Mob*  rdx=Rect*  r8b=continuous  r9=MethodInfo*（FindHit 现场 r8=1 r9=0）
using FnGetBodyRect = uint8_t(__fastcall*)(void* mob, void* outRect, uint8_t continuous,
                                           void* methodInfo);

struct RangeEntry {
    int action = -1;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

std::atomic<bool> gStop{false};
std::atomic<bool> gBound{false};
std::atomic<bool> gBindLogged{false};
void* gMiGetBodyRect = nullptr;
FnGetBodyRect gFnGetBodyRect = nullptr;
void* gAfterimageMap = nullptr;

DWORD gBodyAt = 0;
void* gBodyMob = nullptr;
float gBody[4]{};
bool gBodyOk = false;

DWORD gRangeAt = 0;
DWORD gLastMapPump = 0;
int gRangeN = 0;
int gLastMapCount = -1;
RangeEntry gRange[kMaxRange]{};
std::atomic<bool> gRangeReadyLogged{false};
std::atomic<bool> gSkipLogged{false};
std::atomic<bool> gNoRangePardonUsed{false};
DWORD gNoRangePardonAt = 0;
constexpr DWORD kNoRangePardonRetryMs = 2000;

constexpr int32_t kFuncTypeSkill = 1;
constexpr int32_t kFuncTypeBasicAction = 5;
constexpr int32_t kFkmBasicActionAttack = 52;
constexpr int kWtWand = 37;
constexpr int kWtStaff = 38;
constexpr int kWtBow = 45;
constexpr int kWtCrossbow = 46;
constexpr int kWtThrowingGlove = 47;
constexpr int kWtGun = 49;

void ReturnLeakedMetadataLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
}

void NoteRangeReady() {
    gNoRangePardonUsed.store(false, std::memory_order_release);
    gNoRangePardonAt = 0;
}

bool TakeNoRangePardon(DWORD now) {
    if (!gNoRangePardonUsed.load(std::memory_order_acquire)) {
        gNoRangePardonUsed.store(true, std::memory_order_release);
        gNoRangePardonAt = now;
        x::runtime::LogI("HitGeom", "no_range pardon (seed afterimage)");
        return true;
    }
    if (gNoRangePardonAt && now - gNoRangePardonAt >= kNoRangePardonRetryMs) {
        gNoRangePardonAt = now;
        x::runtime::LogI("HitGeom", "no_range pardon retry (table still empty)");
        return true;
    }
    return false;
}

void FillNoRange(Snap* s, DWORD now) {
    if (!s) return;
    s->why = "no_range";
    s->rangeN = gRangeN;
    if (TakeNoRangePardon(now)) return;
    s->geom = Geom::Separate;
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Finite4(const float* r) {
    return r && std::isfinite(r[0]) && std::isfinite(r[1]) && std::isfinite(r[2]) &&
           std::isfinite(r[3]);
}

bool RectUsable(const float* r) {
    return Finite4(r) && r[2] > 1.f && r[3] > 1.f;
}

bool UnityRectOverlap(float ax, float ay, float aw, float ah, float bx, float by, float bw,
                      float bh) {
    return (ax + aw) > bx && ax < (bx + bw) && (ay + ah) > by && ay < (by + bh);
}

// Range 是面向左的 XYWH（WZ 默认朝左）。ma bit0=0 朝右时绕原点翻 X。
// 世界盒：AbsPos 原点相加；更大 Y = 更高，不反号。
void LocalToWorld(float lx, float ly, float lw, float lh, float px, float py, bool faceLeft,
                  float* ox, float* oy, float* ow, float* oh) {
    float x = lx;
    if (!faceLeft) x = -(lx + lw);
    *ox = px + x;
    *oy = py + ly;
    *ow = lw;
    *oh = lh;
}

int WalkDictSlots(void* entries) {
    if (!LooksLikeHeapPtr(entries)) return 0;
    uintptr_t n = x::runtime::il2cpp::ArrayLen(entries);
    if (n > 4096) n = 4096;
    return static_cast<int>(n);
}

int WalkRangeInto(RangeEntry* out, int cap, void* map) {
    if (!out || cap <= 0 || !LooksLikeHeapPtr(map)) return 0;
    x::runtime::il2cpp_container::Ensure();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    void* entries = ReadPtr(map, kOffDictEntries);
    const int count = ReadI32(map, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count <= 0) return 0;
    const int slots = WalkDictSlots(entries);
    int n = 0;
    for (int i = 0; i < slots && n < cap; ++i) {
        uint8_t* e = reinterpret_cast<uint8_t*>(entries) + offData +
                     static_cast<size_t>(i) * kEntryStrideStrRef;
        if (ReadI32(e, 0) < 0) continue;
        void* img = ReadPtr(e, kEntryValOffStrRef);
        if (!LooksLikeHeapPtr(img)) continue;
        void* range = ReadPtr(img, kOffAfterImageRange);
        void* rEnt = LooksLikeHeapPtr(range) ? ReadPtr(range, kOffDictEntries) : nullptr;
        const int rCount = LooksLikeHeapPtr(range) ? ReadI32(range, kOffDictCount) : 0;
        if (!LooksLikeHeapPtr(rEnt) || rCount <= 0) continue;
        const int rSlots = WalkDictSlots(rEnt);
        for (int k = 0; k < rSlots && n < cap; ++k) {
            uint8_t* re = reinterpret_cast<uint8_t*>(rEnt) + offData +
                          static_cast<size_t>(k) * kEntryStrideIntRect;
            if (ReadI32(re, 0) < 0) continue;
            RangeEntry& dst = out[n];
            dst.action = ReadI32(re, kEntryKeyOffIntRect);
            __try {
                memcpy(&dst.x, re + kEntryValOffIntRect, 16);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!RectUsable(&dst.x)) continue;
            ++n;
        }
    }
    return n;
}

void* ResolveAfterImageMapOnPump() {
    if (!x::runtime::il2cpp::Ensure()) return nullptr;
    const auto& api = x::runtime::il2cpp::Get();
    if (!api.classParent || !api.classStaticData || !api.classGetFieldFromName ||
        !api.fieldGetOffset || !api.fieldGetType || !api.classFromType) {
        return nullptr;
    }
    void* amKlass = nullptr;
    void* singleton = nullptr;
    __try {
        amKlass = x::runtime::il2cpp::FindClass("", kHashActionManager);
        if (amKlass) singleton = api.classParent(amKlass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/FindClass AM");
        return nullptr;
    }
    if (!amKlass || !singleton) return nullptr;
    (void)x::runtime::il2cpp::RuntimeClassInit(singleton);

    void* fInstance = nullptr;
    __try {
        fInstance = api.classGetFieldFromName(singleton, kHashSingletonInstance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/_instance");
        return nullptr;
    }
    if (!fInstance) return nullptr;
    void* statics = nullptr;
    __try {
        statics = api.classStaticData(singleton);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/statics");
        return nullptr;
    }
    if (!statics) return nullptr;
    const size_t offInst = api.fieldGetOffset(fInstance);
    void* lazy = ReadPtr(statics, offInst);
    if (!LooksLikeHeapPtr(lazy)) return nullptr;

    void* lazyKlass = ReadPtr(lazy, 0);
    if (!LooksLikeHeapPtr(lazyKlass)) {
        __try {
            lazyKlass = api.classFromType(api.fieldGetType(fInstance));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("hit_geom/lazyKlass");
            return nullptr;
        }
    }
    if (!lazyKlass) return nullptr;
    void* fValue = nullptr;
    __try {
        fValue = api.classGetFieldFromName(lazyKlass, "_value");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/_value");
        return nullptr;
    }
    if (!fValue) return nullptr;
    void* mgr = ReadPtr(lazy, api.fieldGetOffset(fValue));
    if (!LooksLikeHeapPtr(mgr)) return nullptr;
    void* map = ReadPtr(mgr, kOffActionMgrAfterImageMap);
    return LooksLikeHeapPtr(map) ? map : nullptr;
}

bool BindGetBodyRectOnPump() {
    if (gFnGetBodyRect && gMiGetBodyRect) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    void* klass = nullptr;
    __try {
        klass = x::runtime::il2cpp::FindClass("", kMobClass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/FindClass Mob");
        return false;
    }
    if (!klass) return false;
    (void)x::runtime::il2cpp::RuntimeClassInit(klass);
    void* mi = nullptr;
    __try {
        mi = x::runtime::il2cpp_method::FindMethodByRva(klass, kRvaGetBodyRect, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/FindMethodByRva");
        mi = nullptr;
    }
    gMiGetBodyRect = mi;
    if (mi) {
        auto* head = reinterpret_cast<MethodInfoHead*>(mi);
        if (head->methodPointer) {
            gFnGetBodyRect = reinterpret_cast<FnGetBodyRect>(head->methodPointer);
        }
    }
    if (!gFnGetBodyRect) {
        gFnGetBodyRect = x::runtime::il2cpp::AtRva<FnGetBodyRect>(kRvaGetBodyRect);
    }
    return gFnGetBodyRect != nullptr;
}

bool CallGetBodyRectOnPump(void* mob, float out[4]) {
    if (!mob || !out || !gFnGetBodyRect) return false;
    uint8_t raw[16]{};
    uint8_t ok = 0;
    __try {
        ok = gFnGetBodyRect(mob, raw, 1, gMiGetBodyRect);
        memcpy(out, raw, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("hit_geom/GetBodyRect");
        return false;
    }
    (void)ok;
    return RectUsable(out);
}

struct PumpJob {
    void* mob = nullptr;
    bool wantBody = false;
    bool bound = false;
    bool bodyOk = false;
    bool mapOk = false;
    int rangeN = 0;
    float body[4]{};
    RangeEntry range[kMaxRange]{};
};

void PumpJobFn(void* p) {
    auto* j = static_cast<PumpJob*>(p);
    if (!j) return;
    j->bound = BindGetBodyRectOnPump();
    void* map = ResolveAfterImageMapOnPump();
    if (map) {
        gAfterimageMap = map;
        j->mapOk = true;
        j->rangeN = WalkRangeInto(j->range, kMaxRange, map);
    }
    if (j->wantBody && j->bound && LooksLikeHeapPtr(j->mob)) {
        j->bodyOk = CallGetBodyRectOnPump(j->mob, j->body);
    }
    int mapCount = -1;
    if (LooksLikeHeapPtr(gAfterimageMap)) mapCount = ReadI32(gAfterimageMap, kOffDictCount);
    gLastMapCount = mapCount;
    if (j->bound && !gBindLogged.exchange(true, std::memory_order_acq_rel)) {
        x::runtime::LogI("HitGeom", "bind GetBodyRect mi=%p map=%p rangeN=%d mapCount=%d",
                         gMiGetBodyRect, gAfterimageMap, j->rangeN, mapCount);
    }
    if (j->rangeN > 0 && !gRangeReadyLogged.exchange(true, std::memory_order_acq_rel)) {
        x::runtime::LogI("HitGeom", "range ready n=%d mapCount=%d", j->rangeN, mapCount);
    }
}

bool PumpOnce(PumpJob* job) {
    if (!job) return false;
    if (!x::runtime::main_thread::Ensure()) return false;
    if (x::runtime::main_thread::IsOnPumpThread()) {
        PumpJobFn(job);
        return true;
    }
    return x::runtime::main_thread::InvokeAndWait(&PumpJobFn, job, kPumpWaitMs,
                                                  x::runtime::main_thread::JobPrio::High);
}

void CacheRangeFromJob(const PumpJob& job, DWORD now) {
    if (job.rangeN <= 0) return;
    const int n = job.rangeN < kMaxRange ? job.rangeN : kMaxRange;
    memcpy(gRange, job.range, sizeof(RangeEntry) * static_cast<size_t>(n));
    gRangeN = n;
    gRangeAt = now;
    NoteRangeReady();
}

bool RefreshRange(DWORD now, bool forcePump) {
    if (gRangeN > 0 && now - gRangeAt < kRangeCacheMs) return true;
    if (LooksLikeHeapPtr(gAfterimageMap)) {
        RangeEntry tmp[kMaxRange]{};
        const int n = WalkRangeInto(tmp, kMaxRange, gAfterimageMap);
        if (n > 0) {
            memcpy(gRange, tmp, sizeof(RangeEntry) * static_cast<size_t>(n));
            gRangeN = n;
            gRangeAt = now;
            NoteRangeReady();
            if (!gRangeReadyLogged.exchange(true, std::memory_order_acq_rel)) {
                x::runtime::LogI("HitGeom", "range ready n=%d (worker walk)", n);
            }
            return true;
        }
    }
    // 表是懒建的：没挥过刀 count 一直是 0。空表不准每 tick 泵，否则卡出刀。
    if (!forcePump && gLastMapPump && now - gLastMapPump < kMapPumpGapMs) return gRangeN > 0;
    PumpJob job{};
    job.wantBody = false;
    gLastMapPump = now;
    if (!PumpOnce(&job)) return gRangeN > 0;
    gBound.store(job.bound, std::memory_order_release);
    CacheRangeFromJob(job, now);
    return gRangeN > 0;
}

bool RefreshBody(void* mob, DWORD now, float out[4]) {
    if (gBodyOk && gBodyMob == mob && now - gBodyAt < kBodyCacheMs) {
        memcpy(out, gBody, 16);
        return true;
    }
    PumpJob job{};
    job.mob = mob;
    job.wantBody = true;
    if (!PumpOnce(&job)) return false;
    gBound.store(job.bound, std::memory_order_release);
    CacheRangeFromJob(job, now);
    if (!job.bodyOk) {
        gBodyOk = false;
        gBodyMob = mob;
        gBodyAt = now;
        return false;
    }
    memcpy(gBody, job.body, 16);
    memcpy(out, job.body, 16);
    gBodyOk = true;
    gBodyMob = mob;
    gBodyAt = now;
    return true;
}

Snap OverlapNow(const float body[4], float px, float py, bool faceLeft, int actionHint) {
    Snap s;
    s.actionHint = actionHint;
    s.rangeN = gRangeN;
    s.bodyX = body[0];
    s.bodyY = body[1];
    s.bodyW = body[2];
    s.bodyH = body[3];
    if (gRangeN <= 0) {
        FillNoRange(&s, GetTickCount());
        return s;
    }
    float uL = 1e9f, uB = 1e9f, uR = -1e9f, uT = -1e9f;
    int hitAction = -1;
    float hitAtk[4]{};
    for (int i = 0; i < gRangeN; ++i) {
        const RangeEntry& e = gRange[i];
        float ax, ay, aw, ah;
        LocalToWorld(e.x, e.y, e.w, e.h, px, py, faceLeft, &ax, &ay, &aw, &ah);
        if (ax < uL) uL = ax;
        if (ay < uB) uB = ay;
        if (ax + aw > uR) uR = ax + aw;
        if (ay + ah > uT) uT = ay + ah;
        if (UnityRectOverlap(ax, ay, aw, ah, body[0], body[1], body[2], body[3])) {
            if (hitAction < 0) {
                hitAction = e.action;
                hitAtk[0] = ax;
                hitAtk[1] = ay;
                hitAtk[2] = aw;
                hitAtk[3] = ah;
            }
        }
    }
    if (hitAction >= 0) {
        s.geom = Geom::Overlap;
        s.actionUsed = hitAction;
        s.atkX = hitAtk[0];
        s.atkY = hitAtk[1];
        s.atkW = hitAtk[2];
        s.atkH = hitAtk[3];
        s.why = "overlap";
        return s;
    }
    s.geom = Geom::Separate;
    s.atkX = uL;
    s.atkY = uB;
    s.atkW = uR - uL;
    s.atkH = uT - uB;
    s.why = "separate";
    return s;
}

bool WeaponSkipsMeleeGeom(int wt) {
    return wt == kWtBow || wt == kWtCrossbow || wt == kWtThrowingGlove || wt == kWtGun ||
           wt == kWtWand || wt == kWtStaff;
}

// Worker 可调。A 槽非普攻 / 远程武器：近战 afterimage 闸会误压官方射击/技能盒。
bool LoadoutSkipsMeleeGeom(int* fkType, int* fkValue, int* weaponType, const char** why) {
    int32_t t = -1;
    int32_t v = -1;
    (void)x::features::ports::attack::PeekAttackBindingCached(&t, &v);
    const int wt = x::features::final_attack_force::QueryEquippedWeaponType();
    if (fkType) *fkType = t;
    if (fkValue) *fkValue = v;
    if (weaponType) *weaponType = wt;
    if (t == kFuncTypeSkill && v > 0) {
        if (why) *why = "a_skill";
        return true;
    }
    if (t >= 0 && !(t == kFuncTypeBasicAction && v == kFkmBasicActionAttack)) {
        if (why) *why = "a_bind";
        return true;
    }
    if (WeaponSkipsMeleeGeom(wt)) {
        if (why) *why = "ranged";
        return true;
    }
    return false;
}

}  // namespace

void Init() {
    gStop.store(false, std::memory_order_release);
    gBound.store(false, std::memory_order_release);
    gBindLogged.store(false, std::memory_order_release);
    gMiGetBodyRect = nullptr;
    gFnGetBodyRect = nullptr;
    gAfterimageMap = nullptr;
    gBodyOk = false;
    gBodyMob = nullptr;
    gBodyAt = 0;
    gRangeN = 0;
    gRangeAt = 0;
    gLastMapPump = 0;
    gLastMapCount = -1;
    gRangeReadyLogged.store(false, std::memory_order_release);
    gSkipLogged.store(false, std::memory_order_release);
    gNoRangePardonUsed.store(false, std::memory_order_release);
    gNoRangePardonAt = 0;
}

void Shutdown() {
    gStop.store(true, std::memory_order_release);
    gBound.store(false, std::memory_order_release);
    gMiGetBodyRect = nullptr;
    gFnGetBodyRect = nullptr;
    gAfterimageMap = nullptr;
    gBodyOk = false;
    gRangeN = 0;
    gRangeReadyLogged.store(false, std::memory_order_release);
    gSkipLogged.store(false, std::memory_order_release);
    gNoRangePardonUsed.store(false, std::memory_order_release);
    gNoRangePardonAt = 0;
}

Snap QueryLockOverlap(void* mob, float px, float py, bool faceLeft, int actionHint) {
    Snap s;
    s.actionHint = actionHint;
    if (gStop.load(std::memory_order_acquire)) {
        s.why = "stop";
        return s;
    }
    {
        const char* skipWhy = nullptr;
        int t = -1, v = -1, wt = 0;
        if (LoadoutSkipsMeleeGeom(&t, &v, &wt, &skipWhy)) {
            s.fkType = t;
            s.fkValue = v;
            s.weaponType = wt;
            s.why = skipWhy ? skipWhy : "loadout";
            if (!gSkipLogged.exchange(true, std::memory_order_acq_rel)) {
                x::runtime::LogI("HitGeom", "skip loadout why=%s t=%d v=%d wt=%d", s.why, t, v,
                                 wt);
            }
            return s;
        }
        s.fkType = t;
        s.fkValue = v;
        s.weaponType = wt;
    }
    if (!LooksLikeHeapPtr(mob)) {
        s.why = "no_mob";
        return s;
    }
    const DWORD now = GetTickCount();
    if (!RefreshRange(now, /*forcePump=*/false)) {
        FillNoRange(&s, now);
        return s;
    }
    float body[4]{};
    if (!RefreshBody(mob, now, body)) {
        s.why = "no_body";
        s.rangeN = gRangeN;
        return s;
    }
    Snap out = OverlapNow(body, px, py, faceLeft, actionHint);
    out.fkType = s.fkType;
    out.fkValue = s.fkValue;
    out.weaponType = s.weaponType;
    return out;
}

}  // namespace x::features::ports::hit_geom
