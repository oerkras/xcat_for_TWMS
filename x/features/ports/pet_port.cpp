// pet_port ?Classic TWMS pet read-state + activate (P0c).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pet_port.h"

#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"

#include <Psapi.h>
#include <atomic>
#include <cstring>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::pet {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaCompGetGo = 0x4E47E00;  // remapped 2026-08-03
constexpr uint32_t kRvaObjGetName = 0x4E54D60;  // remapped 2026-08-03
constexpr uint32_t kRvaSendWillRenderCanvases = 0x5239AB0;  // remapped 2026-08-03
constexpr uint32_t kRvaSendActivatePetRequest = 0xC56910;  // remapped 2026-08-03
constexpr char kHashSendActivatePet[] =
    "bda41ac00e6e562f7370087756222c00497dcb55dd3fb99c4d9f96c436649a9";

// UserLocal → il2cpp_shape::ResolveUserLocalKlass
constexpr char kCashItemManagerClass[] =
    "edfa536aa4e0251d8e7ae705fd533c40eb0a4a55ae0a8d596e09f711d527f11";

constexpr size_t kOffWmMyUser = 0x28;
constexpr size_t kOffWmCharacterData = 0xE0;
constexpr size_t kOffCdItemSlots = 0x40;
constexpr size_t kOffApPet = 0x2B0;  // TW User.m_apPet 2026-08-03
constexpr size_t kOffPetRepleteness = 0xBC;
constexpr size_t kOffItemId = 0x10;
constexpr size_t kOffBundleNumber = 0x28;
constexpr size_t kOffSlotRepleteness = 0x38;
constexpr size_t kOffDateDead = 0x40;  // DateTime?ticks + kind?
constexpr size_t kOffRemainLife = 0x48;
constexpr size_t kOffActiveState = 0x4E;
// .NET DateTime?? 62 bit = ticks???? dateDead ? 2078/2079?????????
constexpr int64_t kDateTimeTicksMask = 0x3FFFFFFFFFFFFFFFLL;
// ~? 2070????????/??????????????????/??
constexpr int64_t kPetPermanentTicksFloor = 653000000000000000LL;
constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kOffVisPos = 0x64;
constexpr size_t kOffLogicalPos = 0x240;

constexpr int kItemTypeConsume = 2;
constexpr int kItemTypeCash = 5;
constexpr int kDefaultFoodCode = 2120000;
constexpr int kFoodIdMin = 2120000;
constexpr int kFoodIdMax = 2129999;
constexpr int kPetIdMin = 5000000;
constexpr int kPetIdMax = 5010000;

constexpr DWORD kRebindMs = 3000;
constexpr DWORD kJobWaitMs = 1500;
constexpr float kMinPosAbs = 1.0f;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);
using FnSendWill = void (*)(const void* methodInfo);
using FnActivatePet = void (*)(void* self, int nPos, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;

void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gCashMgrKlass = nullptr;
void* gCashMgr = nullptr;
void* gKlassCanvas = nullptr;
MethodInfoHead* gMiSendWill = nullptr;
MethodInfoHead* gMiActivate = nullptr;
FnSendWill gOrigSendWill = nullptr;
std::atomic<bool> gPumpInstalled{false};
std::atomic<bool> gInPump{false};

DWORD gLastLuRebind = 0;
DWORD gLastCashRebind = 0;

std::atomic<bool> gJobPending{false};
std::atomic<bool> gJobDone{false};
std::atomic<bool> gJobOk{false};
std::atomic<uint32_t> gJobSerial{0};  // timeout 后晚到的泵不得再发包
int gJobPos = 0;
uint32_t gJobArmedSerial = 0;  // 与 gJobPos 同受 gJobCs 保护
CRITICAL_SECTION gJobCs{};
bool gJobCsInit = false;

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

uint16_t ReadU16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int ListSize(void* list) {
    if (!list) return 0;
    return ReadI32(list, 0x18);
}

void* ListAt(void* list, int i) {
    if (!list || i < 0) return nullptr;
    void* items = ReadPtr(list, 0x10);
    if (!items) return nullptr;
    return ArrayAt(items, (uintptr_t)i);
}

bool PosLooksAliveXY(float x, float y) {
    return (x > kMinPosAbs || x < -kMinPosAbs || y > kMinPosAbs || y < -kMinPosAbs);
}

bool ReadIl2CppString(void* str, char* out, size_t outCap) {
    if (!str || !out || outCap < 2) return false;
    __try {
        const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            out[n++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetGoName(void* comp, char* out, size_t outCap) {
    out[0] = 0;
    if (!comp || !gCompGo || !gObjName) return false;
    void* go = nullptr;
    __try {
        go = gCompGo(comp, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!go) return false;
    void* nameObj = nullptr;
    __try {
        nameObj = gObjName(go, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ReadIl2CppString(nameObj, out, outCap);
}

void* FindClass(const char* ns, const char* name) {
    return x::runtime::il2cpp::FindClass(ns, name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGA) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(gGA) + rva;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = gClassGetMethods(klass, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            if (reinterpret_cast<uintptr_t>(mi->methodPointer) == want) return mi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
    }
    if (mi && mi->methodPointer) return mi;
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            const char* nm = e.methodGetName(raw);
            if (nm && strcmp(nm, name) == 0) {
                mi = reinterpret_cast<MethodInfoHead*>(raw);
                if (mi && mi->methodPointer) return mi;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

MethodInfoHead* ResolveActivateMi(void* klass) {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (MethodInfoHead* mi =
            FindMethodByName(klass, "SendActivatePetRequest", 1))
        return mi;
    if (MethodInfoHead* mi = FindMethodByName(klass, kHashSendActivatePet, 1)) return mi;
    // void(int) 同形 x3 → kind 只验；哈希优先。
    constexpr MethodShape kAct{1, TypeKind::Void, true, false, {TypeKind::I32}};
    const auto mr =
        x::runtime::il2cpp_method::FindMethodCached(klass, kRvaSendActivatePetRequest, kAct);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("PetPort", "Activate MethodInfo via kind");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, kRvaSendActivatePetRequest);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    *outOrig = mi->methodPointer;
    mi->methodPointer = hook;
    if (mi->virtualMethodPointer) mi->virtualMethodPointer = hook;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return true;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    mi->methodPointer = orig;
    if (mi->virtualMethodPointer) mi->virtualMethodPointer = orig;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

bool BindApis() {
    if (gGA && gFindAll && gCompGo && gObjName && gClassGetMethods) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gCompGo = e.compGo;
    gObjName = e.objName;
    gClassGetMethods = e.classGetMethods;
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gRuntimeClassInit = e.runtimeClassInit;
    return gFindAll && gCompGo && gObjName && gClassGetMethods;
}

void EnsureCs() {
    if (gJobCsInit) return;
    InitializeCriticalSection(&gJobCs);
    gJobCsInit = true;
}

bool LocalUserStillAlive() {
    if (!gLocalUser) return false;
    __try {
        if (!*reinterpret_cast<void**>(gLocalUser)) return false;
        const intptr_t cached =
            *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffCachedPtr);
        if (cached == 0) return false;
        void* wm = world::PeekWorldManager();
        void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (LooksLikeHeapPtr(mu) && mu != gLocalUser) return false;
        char name[96]{};
        if (!GetGoName(gLocalUser, name, sizeof(name))) return false;
        if (_stricmp(name, "MyUser") != 0) return false;
        const float visX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos);
        const float visY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos + 4);
        const float logX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos);
        const float logY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos + 4);
        if (!PosLooksAliveXY(visX, visY) && !PosLooksAliveXY(logX, logY)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryResolveLocalUser(DWORD now) {
    if (LocalUserStillAlive()) return true;

    bool forceRebind = false;
    if (gLocalUser) {
        void* wm = world::PeekWorldManager();
        void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (LooksLikeHeapPtr(mu) && mu != gLocalUser) forceRebind = true;
    }
    gLocalUser = nullptr;
    if (!forceRebind && gLastLuRebind && now - gLastLuRebind < kRebindMs) return false;
    gLastLuRebind = now;
    if (!BindApis()) return false;

    void* wm = world::PeekWorldManager();
    if (!wm) wm = world::GetWorldManager();
    void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
    if (LooksLikeHeapPtr(mu)) {
        char name[96]{};
        if (GetGoName(mu, name, sizeof(name)) && _stricmp(name, "MyUser") == 0) {
            gLocalUser = mu;
            x::runtime::LogI("PetPort", "LocalUser ACCEPT wm.MyUser=%p", gLocalUser);
            return true;
        }
    }

    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType || !gFindAll) return false;

    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    void* best = nullptr;
    for (uintptr_t i = 0; i < n && i < 64; ++i) {
        void* obj = ArrayAt(arr, i);
        if (!obj) continue;
        char name[96]{};
        GetGoName(obj, name, sizeof(name));
        if (name[0] && _stricmp(name, "MyUser") == 0) {
            best = obj;
            break;
        }
    }
    gLocalUser = best;
    if (gLocalUser) x::runtime::LogI("PetPort", "LocalUser ACCEPT lu=%p", gLocalUser);
    return gLocalUser != nullptr;
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

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

bool LooksLikeCashMgr(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gCashMgrKlass && !ObjKlassIs(cand, gCashMgrKlass)) return false;
    // instance fields start with Dictionary* @+0x10
    void* d0 = ReadPtr(cand, 0x10);
    return !d0 || LooksLikeHeapPtr(d0);
}

bool ResolveCashItemManager(DWORD now) {
    if (gCashMgr && LooksLikeCashMgr(gCashMgr)) return true;
    gCashMgr = nullptr;
    if (gLastCashRebind && now - gLastCashRebind < kRebindMs) return false;
    gLastCashRebind = now;
    if (!BindApis()) return false;
    if (!gCashMgrKlass) gCashMgrKlass = FindClass("", kCashItemManagerClass);
    if (!gCashMgrKlass) return false;

    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gCashMgrKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* staticsKlass = gCashMgrKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gCashMgrKlass);
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
    if (!statics) statics = KlassStaticFields(gCashMgrKlass);
    if (!statics) return false;

    for (size_t s = 0; s < 4; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeCashMgr(cand)) continue;
        gCashMgr = cand;
        break;
    }
    if (gCashMgr) {
        x::runtime::LogI("PetPort", "CashItemManager bind %p", gCashMgr);
        if (!gMiActivate) gMiActivate = ResolveActivateMi(gCashMgrKlass);
    }
    return gCashMgr != nullptr;
}

void* GetSlotList(int itemType) {
    void* wm = world::GetWorldManager();
    if (!wm) return nullptr;
    void* cd = ReadPtr(wm, kOffWmCharacterData);
    if (!LooksLikeHeapPtr(cd)) return nullptr;
    void* slotsArr = ReadPtr(cd, kOffCdItemSlots);
    if (!LooksLikeHeapPtr(slotsArr)) return nullptr;
    const uintptr_t n = ArrayLen(slotsArr);
    if (n <= (uintptr_t)itemType) return nullptr;
    return ArrayAt(slotsArr, (uintptr_t)itemType);
}

bool IsPetItemId(int id) { return id >= kPetIdMin && id < kPetIdMax; }
bool IsFoodItemId(int id) { return id >= kFoodIdMin && id <= kFoodIdMax; }

// FILETIME(1601-01-01) → .NET DateTime ticks(0001-01-01).
// 注意：621355968000000000 是 Unix(1970) 偏移，误用会把「现在」推到 ~2395，
// 导致一切合理 dateDead 都被判死（BIN LOG: dateTicks=2026-10-28, now=2395）。
int64_t NowNetTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t fileTicks =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
    constexpr uint64_t kFileTimeEpochToDateTime = 504911232000000000ULL;  // DateTime(1601,1,1).Ticks
    return static_cast<int64_t>(fileTicks + kFileTimeEpochToDateTime);
}

// 死宠：dateDead 落在「合理历法区间」且早于现在。永久宠≈2078/79；脏/未初始化小值不当死。
bool IsPetSlotDead(void* item) {
    if (!item) return true;
    const int64_t ticks = ReadI64(item, kOffDateDead) & kDateTimeTicksMask;
    if (ticks <= 0) return false;
    if (ticks >= kPetPermanentTicksFloor) return false;
    // ~2000-01-01；更早多半是错读/未写字段，不当过期
    constexpr int64_t kMinPlausibleDead = 630822816000000000LL;
    if (ticks < kMinPlausibleDead) return false;
    return ticks < NowNetTicks();
}

int ItemQty(void* item) {
    if (!item) return 0;
    const int n = (int)ReadU16(item, kOffBundleNumber);
    if (n > 0) return n;
    return 1;
}

void ScanFood(PetCareState& out) {
    void* list = GetSlotList(kItemTypeConsume);
    if (!list) return;
    const int n = ListSize(list);
    if (n <= 0 || n > 512) return;
    const bool oneBased = (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);

    int bestPos = 0, bestId = 0, bestQty = 0;
    bool preferHit = false;
    for (int i = 0; i < n; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        const int id = ReadI32(item, kOffItemId);
        if (!IsFoodItemId(id)) continue;
        const int qty = ItemQty(item);
        if (qty <= 0) continue;
        const int pos = oneBased ? i : (i + 1);
        if (pos <= 0) continue;
        const bool isPrefer = (id == kDefaultFoodCode);
        if (!preferHit) {
            if (isPrefer || bestPos == 0) {
                bestPos = pos;
                bestId = id;
                bestQty = qty;
                if (isPrefer) preferHit = true;
            }
        } else if (isPrefer && qty > bestQty) {
            bestPos = pos;
            bestId = id;
            bestQty = qty;
        }
    }
    if (bestPos > 0) {
        out.hasFood = true;
        out.foodPos = bestPos;
        out.foodItemId = bestId;
        out.foodQty = bestQty;
    }
}

void ScanCashPets(PetCareState& out) {
    void* list = GetSlotList(kItemTypeCash);
    if (!list) return;
    const int n = ListSize(list);
    if (n <= 0 || n > 512) return;
    const bool oneBased = (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);

    int summonPos = 0, dead = 0, cashPets = 0, slotMinFull = -1;
    int livingAnyPos = 0;  // 未过期（含 active!=0），供场上空宠时的 active 粘滞回退
    for (int i = 0; i < n; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        const int id = ReadI32(item, kOffItemId);
        if (!IsPetItemId(id)) continue;
        ++cashPets;
        const int remain = ReadI32(item, kOffRemainLife);
        const uint8_t active = ReadU8(item, kOffActiveState);
        const int full = (int)ReadU8(item, kOffSlotRepleteness);
        const bool deadSlot = IsPetSlotDead(item);
        const int pos = oneBased ? i : (i + 1);
        if (cashPets == 1) {
            out.probeRemainLife = remain;
            out.probeActiveState = (int)active;
            out.probeDeadByDate = deadSlot ? 1 : 0;
        }
        if (deadSlot) {
            ++dead;
            continue;
        }
        if (pos > 0 && livingAnyPos == 0) livingAnyPos = pos;
        if (active != 0) {
            if (full >= 0 && full <= 100) {
                if (slotMinFull < 0 || full < slotMinFull) slotMinFull = full;
            }
            continue;
        }
        if (summonPos == 0 && pos > 0) summonPos = pos;
        (void)remain;
    }
    // 真过期宠不再 ignore-dead。
    // 仅当场上 m_apPet 已空、但 Cash 槽仍 active!=0（粘滞）时回退，避免误填 summonPos。
    if (summonPos == 0 && livingAnyPos > 0 && out.activatedCount == 0) {
        summonPos = livingAnyPos;
        static DWORD sLastActiveStuckLog = 0;
        const DWORD nowMs = GetTickCount();
        if (!sLastActiveStuckLog || nowMs - sLastActiveStuckLog >= 5000) {
            sLastActiveStuckLog = nowMs;
            x::runtime::LogW("PetPort",
                             "summon fallback active-stuck pos=%d cash=%d dead=%d active=%d",
                             summonPos, cashPets, dead, out.probeActiveState);
        }
    }
    out.cashPetCount = cashPets;
    out.deadPetCount = dead;
    out.summonPetPos = summonPos;
    if (out.minRepleteness < 0 && slotMinFull >= 0) out.minRepleteness = slotMinFull;
}

void ReadFieldPets(PetCareState& out) {
    if (!gLocalUser) return;
    void* arr = ReadPtr(gLocalUser, kOffApPet);
    if (!LooksLikeHeapPtr(arr)) return;
    const uintptr_t n = ArrayLen(arr);
    if (n == 0 || n > 8) return;
    int activated = 0, minFull = -1;
    for (uintptr_t i = 0; i < n; ++i) {
        void* pet = ArrayAt(arr, i);
        if (!LooksLikeHeapPtr(pet)) continue;
        ++activated;
        const int full = ReadI32(pet, kOffPetRepleteness);
        if (full < 0 || full > 100) continue;
        if (minFull < 0 || full < minFull) minFull = full;
    }
    out.activatedCount = activated;
    if (minFull >= 0) out.minRepleteness = minFull;
}

void RunActivateOnMain() {
    EnsureCs();
    EnterCriticalSection(&gJobCs);
    const int pos = gJobPos;
    const uint32_t serial = gJobArmedSerial;
    const bool armed = gJobPending.load(std::memory_order_relaxed) && serial != 0;
    LeaveCriticalSection(&gJobCs);
    if (!armed || pos <= 0) return;

    bool ok = false;
    __try {
        const DWORD now = GetTickCount();
        if (!ResolveCashItemManager(now) || !gCashMgr) {
            x::runtime::LogW("PetPort", "Activate: no CashItemManager");
        } else {
            if (!gMiActivate && gCashMgrKlass)
                gMiActivate = ResolveActivateMi(gCashMgrKlass);
            auto fn = (gMiActivate && gMiActivate->methodPointer)
                          ? reinterpret_cast<FnActivatePet>(gMiActivate->methodPointer)
                          : AtRva<FnActivatePet>(kRvaSendActivatePetRequest);
            if (fn) {
                fn(gCashMgr, pos, gMiActivate);
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        x::runtime::LogW("PetPort", "Activate SEH pos=%d", pos);
    }

    EnterCriticalSection(&gJobCs);
    // timeout 已作废本 serial 则丢弃结果，禁止晚到发包被视为成功/再次触发
    if (gJobArmedSerial == serial && gJobPending.load(std::memory_order_relaxed)) {
        gJobOk.store(ok, std::memory_order_relaxed);
        gJobDone.store(true, std::memory_order_release);
        gJobPending.store(false, std::memory_order_relaxed);
        gJobArmedSerial = 0;
    }
    LeaveCriticalSection(&gJobCs);
}

void HookSendWill(const void* methodInfo) {
    if (!gInPump.exchange(true)) {
        if (gJobPending.load() && !gJobDone.load()) RunActivateOnMain();
        gInPump.store(false);
    }
    if (gOrigSendWill) gOrigSendWill(methodInfo);
}

bool InstallPump() {
    if (gPumpInstalled.load()) return true;
    if (!BindApis()) return false;
    if (!gKlassCanvas) gKlassCanvas = FindClass("UnityEngine", "Canvas");
    if (!gKlassCanvas) {
        x::runtime::LogW("PetPort", "Canvas klass miss");
        return false;
    }
    gMiSendWill = FindMethodByRva(gKlassCanvas, kRvaSendWillRenderCanvases);
    if (!gMiSendWill) {
        auto getName = x::runtime::il2cpp::Get().methodGetName;
        void* iter = nullptr;
        __try {
            for (;;) {
                void* miRaw = gClassGetMethods(gKlassCanvas, &iter);
                if (!miRaw) break;
                if (!getName) break;
                const char* nm = getName(miRaw);
                if (nm && strcmp(nm, "SendWillRenderCanvases") == 0) {
                    gMiSendWill = reinterpret_cast<MethodInfoHead*>(miRaw);
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            gMiSendWill = nullptr;
        }
    }
    if (!gMiSendWill) {
        x::runtime::LogW("PetPort", "SendWill MI miss");
        return false;
    }
    if (gMiSendWill->methodPointer == reinterpret_cast<void*>(&HookSendWill)) {
        gPumpInstalled.store(true);
        return true;
    }
    void* orig = nullptr;
    if (!PatchMethodInfo(gMiSendWill, reinterpret_cast<void*>(&HookSendWill), &orig)) {
        x::runtime::LogW("PetPort", "SendWill patch fail");
        return false;
    }
    gOrigSendWill = reinterpret_cast<FnSendWill>(orig);
    gPumpInstalled.store(true);
    x::runtime::LogI("PetPort", "main-thread pump installed MI=%p orig=%p", (void*)gMiSendWill,
                     orig);
    return true;
}

void UninstallPump() {
    if (!gPumpInstalled.exchange(false)) return;
    if (gMiSendWill && gOrigSendWill &&
        gMiSendWill->methodPointer == reinterpret_cast<void*>(&HookSendWill)) {
        RestoreMethodInfo(gMiSendWill, (void*)gOrigSendWill);
    }
    gMiSendWill = nullptr;
    gOrigSendWill = nullptr;
}

}  // namespace

void Init() {
    EnsureCs();
    gLocalUser = nullptr;
    gCashMgr = nullptr;
    gLastLuRebind = gLastCashRebind = 0;
    gJobPending.store(false);
    gJobDone.store(true);
    gJobArmedSerial = 0;
    x::runtime::LogI("PetPort", "pet_port ready (P0c summon)");
}

void Shutdown() {
    UninstallPump();
    gLocalUser = nullptr;
    gCashMgr = nullptr;
}

bool EnsureBound() {
    const DWORD now = GetTickCount();
    if (!BindApis()) return false;
    (void)world::EnsureBound();
    // 召唤/读态都依赖 MyUser；仅 WM 就绪不算 bound（避免空转扫背包）
    return TryResolveLocalUser(now) && LocalUserStillAlive();
}

bool ReadState(PetCareState& out) {
    out = {};
    const DWORD now = GetTickCount();
    if (!BindApis()) return false;
    const bool luOk = TryResolveLocalUser(now);
    out.hasLocalUser = luOk && LocalUserStillAlive();
    (void)world::EnsureBound();
    if (out.hasLocalUser) ReadFieldPets(out);
    ScanFood(out);
    ScanCashPets(out);
    return out.hasLocalUser || out.hasFood || out.cashPetCount > 0;
}

bool TryActivatePet(int nPos) {
    if (nPos <= 0) return false;
    if (!InstallPump()) return false;
    EnsureCs();
    const uint32_t serial = gJobSerial.fetch_add(1, std::memory_order_relaxed) + 1;
    EnterCriticalSection(&gJobCs);
    gJobPos = nPos;
    gJobArmedSerial = serial;
    gJobDone.store(false, std::memory_order_relaxed);
    gJobOk.store(false, std::memory_order_relaxed);
    gJobPending.store(true, std::memory_order_release);
    LeaveCriticalSection(&gJobCs);

    const DWORD start = GetTickCount();
    while (!gJobDone.load(std::memory_order_acquire)) {
        if (GetTickCount() - start > kJobWaitMs) {
            EnterCriticalSection(&gJobCs);
            if (gJobArmedSerial == serial) {
                gJobPending.store(false, std::memory_order_relaxed);
                gJobArmedSerial = 0;
                gJobDone.store(true, std::memory_order_release);
            }
            LeaveCriticalSection(&gJobCs);
            x::runtime::LogW("PetPort", "Activate timeout pos=%d serial=%u", nPos, serial);
            return false;
        }
        Sleep(5);
    }
    const bool ok = gJobOk.load(std::memory_order_relaxed);
    x::runtime::LogI("PetPort", "Activate %s pos=%d", ok ? "ok" : "fail", nPos);
    return ok;
}

bool TryFeed() { return false; }

}  // namespace x::features::ports::pet
