// pet_port — Classic TWMS pet read-state + activate (P0c).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pet_port.h"

#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/anchor_lamps.h"
#include "../../ui/player_vitals.h"

#include <Psapi.h>
#include <cstring>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::pet {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E4A610;  // remounted 2026-08-04
constexpr uint32_t kRvaCompGetGo = 0x4E53330;              // remounted 2026-08-04
constexpr uint32_t kRvaObjGetName = 0x4E60290;             // remounted 2026-08-04
constexpr uint32_t kRvaSendActivatePetRequest = 0xC5A960;  // remounted 2026-08-04
constexpr char kHashSendActivatePet[] =
    "b2c39570b0512792466530313a22b91bb9929131024081d578d55bb1778aa00";

// UserLocal → il2cpp_shape::ResolveUserLocalKlass
constexpr char kCashItemManagerClass[] =
    "da9619a468e583349c55afeb25a23df1e101d6b8d1e8f4c222ce18f7ec8f878";
constexpr char kUserClass[] =
    "d9ad004bbff1a41ca96697c8e44ed3175dae9846fb772898fd54ec65040348b";  // TDI:1560

// dump fallback；运行时 hash + field_get_offset 优先
constexpr size_t kFbWmMyUser = 0x28;
constexpr size_t kFbApPet = 0x2B0;
constexpr size_t kFbLogicalPos = 0x240;
constexpr size_t kFbVisPos = 0x64;
constexpr char kHashFldWmMyUser[] =
    "<d4428e1b7aab1a1fca5b6951009bf64f2c5cfb39f9a183f582fed4ff3a1aaaa>k__BackingField";
constexpr char kHashFldApPet[] =
    "a3e632d00632a74fdc95dc470f10e1a8979e81b032d7d8eac27dfeb6a13074c";
constexpr char kHashFldCurPos[] =
    "b992bfa57dd45d484f39e25a6290a95d76e19fc1059423bff8fb0c9507dbda7";
constexpr char kHashFldFieldPos[] =
    "c9d7ef4393802ebe9fdf9ebe7eaf7245d5cef3eeaa2a8d052fb4ad4883e34dc";
constexpr char kVecCtrlOwnerClass[] =
    "ddef6db860cfa2bea6dca39e201bf3065a897797f86009fb4d6104830143d94";

struct PetFieldOff {
    size_t wmMyUser = kFbWmMyUser;
    size_t apPet = kFbApPet;
    size_t logicalPos = kFbLogicalPos;
    size_t visPos = kFbVisPos;
    bool tried = false;
    const char* path = "fallback";
};
PetFieldOff gOff{};

#define kOffWmMyUser (gOff.wmMyUser)
#define kOffApPet (gOff.apPet)
#define kOffLogicalPos (gOff.logicalPos)
#define kOffVisPos (gOff.visPos)

// 背包列表：SSOT = player_vitals（ItemSlots hash）；本文件不再钉 CD 偏移。
// Pet / ItemSlotPet：hash → field_get_offset
constexpr char kPetClass[] =
    "f5be2907a4e45eab8f0728f4335609468c882e48c120846124031faddb9b9f2";
constexpr char kItemSlotPetClass[] =
    "c4f17c2d5bd81b5d8e01da93b92b81b91ea9237ecaa08791d6be71784fe6d41";
constexpr char kHashPetRepleteness[] =
    "cdd2e2ec01a3de26f3f2146ca483c5e23abef3110d60b6712108078bd2d717d";
constexpr char kHashSlotRepleteness[] =
    "cf0c3076077ad451da3c44894bcaeed751a8127ead64c82d09fb5e8007fc830";
constexpr char kHashDateDead[] =
    "ca1ab0f8f44b56398566e7d287c7e67fd423775448fcfb0b5173d29686803e1";
constexpr char kHashRemainLife[] =
    "c71dc99cf198b0e7287e04be21a34864d6be4051106ab969fb79b472624aca4";
constexpr char kHashActiveState[] =
    "a6be7648d376879ba30ca97a67f4c803d34746e2c906510d188f7cc4dc6bc48";
constexpr size_t kFbPetRepleteness = 0xBC, kFbSlotRepleteness = 0x38, kFbDateDead = 0x40;
constexpr size_t kFbRemainLife = 0x48, kFbActiveState = 0x4E;
size_t gOffPetRepleteness = kFbPetRepleteness, gOffSlotRepleteness = kFbSlotRepleteness;
size_t gOffDateDead = kFbDateDead, gOffRemainLife = kFbRemainLife, gOffActiveState = kFbActiveState;
#define kOffPetRepleteness (gOffPetRepleteness)
#define kOffSlotRepleteness (gOffSlotRepleteness)
#define kOffDateDead (gOffDateDead)
#define kOffRemainLife (gOffRemainLife)
#define kOffActiveState (gOffActiveState)
// .NET DateTime：低 62 bit = ticks；永久宠 dateDead 常落在 2078/2079。
constexpr int64_t kDateTimeTicksMask = 0x3FFFFFFFFFFFFFFFLL;
constexpr int64_t kPetPermanentTicksFloor = 653000000000000000LL;
constexpr size_t kOffCachedPtr = 0x10;

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
MethodInfoHead* gMiActivate = nullptr;

DWORD gLastLuRebind = 0;
DWORD gLastCashRebind = 0;

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
    return ReadI32(list, x::runtime::il2cpp_container::OffListSize());
}

void* ListAt(void* list, int i) {
    if (!list || i < 0) return nullptr;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
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

bool FieldOffOrFb(void* klass, const char* fieldHash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !fieldHash) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, fieldHash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (off < 0x10 || off >= 0x1000) return false;
    *out = off;
    return true;
}

void EnsureFieldOffsets() {
    if (gOff.tried) return;
    gOff.tried = true;
    if (!x::runtime::il2cpp::Ensure()) return;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return;

    void* userKlass = FindClass("", kUserClass);
    void* vcoKlass = FindClass("", kVecCtrlOwnerClass);
    void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    void* petKlass = FindClass("", kPetClass);
    void* slotKlass = FindClass("", kItemSlotPetClass);
    int hits = 0;
    if (FieldOffOrFb(wmKlass, kHashFldWmMyUser, kFbWmMyUser, &gOff.wmMyUser)) ++hits;
    if (FieldOffOrFb(userKlass, kHashFldApPet, kFbApPet, &gOff.apPet)) ++hits;
    if (FieldOffOrFb(userKlass, kHashFldCurPos, kFbLogicalPos, &gOff.logicalPos)) ++hits;
    if (FieldOffOrFb(vcoKlass, kHashFldFieldPos, kFbVisPos, &gOff.visPos)) ++hits;
    if (FieldOffOrFb(petKlass, kHashPetRepleteness, kFbPetRepleteness, &gOffPetRepleteness)) ++hits;
    if (FieldOffOrFb(slotKlass, kHashSlotRepleteness, kFbSlotRepleteness, &gOffSlotRepleteness))
        ++hits;
    if (FieldOffOrFb(slotKlass, kHashDateDead, kFbDateDead, &gOffDateDead)) ++hits;
    if (FieldOffOrFb(slotKlass, kHashRemainLife, kFbRemainLife, &gOffRemainLife)) ++hits;
    if (FieldOffOrFb(slotKlass, kHashActiveState, kFbActiveState, &gOffActiveState)) ++hits;
    constexpr int kExpect = 9;
    gOff.path = hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback");
    x::runtime::LogI("PetPort",
                     "field offsets path=%s hits=%d/%d apPet=0x%zx slotFull=0x%zx dead=0x%zx",
                     gOff.path, hits, kExpect, gOff.apPet, gOffSlotRepleteness, gOffDateDead);
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !rva) return nullptr;
    const auto& ex = x::runtime::il2cpp::Get();
    if (!ex.classGetMethods) return nullptr;
    HMODULE ga = gGA ? gGA : ex.ga;
    if (!ga) return nullptr;
    void* target = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(ga) + rva);
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* miRaw = ex.classGetMethods(cur, &iter);
                if (!miRaw) break;
                auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
                void* mp = nullptr;
                void* vp = nullptr;
                __try {
                    mp = mi->methodPointer;
                    vp = mi->virtualMethodPointer;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
                if (mp == target || vp == target) return mi;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!ex.classParent) break;
        void* parent = nullptr;
        __try {
            parent = ex.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        const int tryArgc[] = {argc, -1};
        for (int ac : tryArgc) {
            __try {
                mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, ac));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mi = nullptr;
            }
            if (mi && mi->methodPointer) return mi;
        }
    }
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
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    if (!klass) return nullptr;
    // hash → plain → RVA/kind；void(int) 同形 → unique=false。
    constexpr MethodShape kAct{1, TypeKind::Void, false, false, {TypeKind::I32}};
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        klass, kRvaSendActivatePetRequest, kAct, "SendActivatePetRequest", kHashSendActivatePet);
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        x::runtime::LogI("PetPort", "methods path=%s hits=%d/1",
                         mr.path == ResolvePath::Hash ? "meta"
                         : (mr.path != ResolvePath::Miss ? "meta-partial" : "fallback"),
                         mr.path == ResolvePath::Hash ? 1 : 0);
    }
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

void ReportPetActLamp() {
    if (gMiActivate && gMiActivate->methodPointer) {
        x::runtime::anchor_lamps::Set("PetAct", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                     "Activate MI");
    } else if (gCashMgrKlass) {
        x::runtime::anchor_lamps::Set("PetAct", x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                     "RVA fallback");
    } else {
        x::runtime::anchor_lamps::Set("PetAct", x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                     "pending");
    }
}

bool BindApis() {
    if (gGA && gFindAll && gCompGo && gObjName && gClassGetMethods) {
        EnsureFieldOffsets();
        return true;
    }
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
    if (gFindAll && gCompGo && gObjName && gClassGetMethods) {
        EnsureFieldOffsets();
        return true;
    }
    return false;
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

    if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gCashMgrKlass);

    void* staticsKlass = gCashMgrKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gCashMgrKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
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
        if (gMiActivate) {
            x::runtime::LogI("PetPort", "Activate MI bind ok mi=%p rva=0x%X", gMiActivate,
                             kRvaSendActivatePetRequest);
        } else {
            x::runtime::LogW("PetPort", "Activate MI miss — will use RVA 0x%X",
                             kRvaSendActivatePetRequest);
        }
        ReportPetActLamp();
    }
    return gCashMgr != nullptr;
}

void* GetSlotList(int itemType) { return x::ui::player::GetItemSlotList(itemType); }

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
    const int n = (int)ReadU16(item, x::ui::player::OffSlotBundleNumber());
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
        const int id = ReadI32(item, x::ui::player::OffSlotItemId());
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
        const int id = ReadI32(item, x::ui::player::OffSlotItemId());
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

struct ActivateJob {
    int pos = 0;
    bool ok = false;
};

void ActivateJobOnMain(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("pet.Activate");
    auto* job = reinterpret_cast<ActivateJob*>(user);
    if (!job || job->pos <= 0) return;
    job->ok = false;
    __try {
        const DWORD now = GetTickCount();
        if (!ResolveCashItemManager(now) || !gCashMgr) {
            x::runtime::LogW("PetPort", "Activate: no CashItemManager");
            return;
        }
        if (!gMiActivate && gCashMgrKlass) gMiActivate = ResolveActivateMi(gCashMgrKlass);
        if (gMiActivate) ReportPetActLamp();
        auto fn = (gMiActivate && gMiActivate->methodPointer)
                      ? reinterpret_cast<FnActivatePet>(gMiActivate->methodPointer)
                      : AtRva<FnActivatePet>(kRvaSendActivatePetRequest);
        if (!fn) {
            ReportPetActLamp();
            return;
        }
        fn(gCashMgr, job->pos, gMiActivate);
        job->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
        x::runtime::LogW("PetPort", "Activate SEH pos=%d", job->pos);
    }
}

}  // namespace

void Init() {
    gLocalUser = nullptr;
    gCashMgr = nullptr;
    gLastLuRebind = gLastCashRebind = 0;
    x::runtime::LogI("PetPort", "pet_port ready (P0c summon)");
}

void Shutdown() {
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
    if (!BindApis()) return false;
    ActivateJob job{};
    job.pos = nPos;
    if (!x::runtime::main_thread::InvokeAndWait(&ActivateJobOnMain, &job, kJobWaitMs)) {
        x::runtime::LogW("PetPort", "Activate pump fail pos=%d", nPos);
        return false;
    }
    x::runtime::LogI("PetPort", "Activate %s pos=%d", job.ok ? "ok" : "fail", nPos);
    return job.ok;
}

bool TryFeed() { return false; }

}  // namespace x::features::ports::pet
