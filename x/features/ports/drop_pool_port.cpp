// drop_pool_port �?Classic TWMS DropPool + pet vacuum pickup.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "drop_pool_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"

#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"
#include "pet_port.h"
#include "world_port.h"

#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_set>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::drop {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaPetTryPickUpDrop = 0xF96240;  // remapped 2026-08-03
constexpr uint32_t kRvaDropTryPickUpDrop = 0xF508D0;  // remapped 2026-08-03  // DropPool.TryPickUpDrop(in Vector2)
constexpr uint32_t kRvaDropTryPickUpDropByPet = 0xF528E0;  // remapped 2026-08-03 · IDA xref from Pet.TryPickUpDrop
constexpr uint32_t kRvaPetGetUpgradePetSkill = 0xF55F40;  // remapped 2026-08-03
constexpr uint32_t kRvaPetGetItemSlot = 0xF54B60;  // remapped 2026-08-03 · ByPet 内 call；→ ItemSlotPet
constexpr uint32_t kRvaPetIsInExceptionList = 0xF4AC20;  // remapped 2026-08-03
constexpr uint32_t kRvaPetSendDropPickUp = 0xF54F90;  // remapped 2026-08-03   // Pet.SendDropPickUpRequest
constexpr uint32_t kRvaPoolSendDropPickUp = 0xF51F70;  // remapped 2026-08-03  // DropPool.SendDropPickUpRequest
// ByPet Contains 真源（.rdata，非 CollisionCheck / _rcPet）：
//   int32 offX,offY @ +0 ; float w,h @ +0x10
//   rect = (petPos - (offX,offY), w, h)；原生 (25,10)+(50,60)
//   IDA：ByPet@0xF528E0 → psubd/movsd xref；旧 0x5574990
constexpr uint32_t kRvaByPetRectPack = 0x55736B0;  // remapped 2026-08-03

constexpr char kHashPetTryPickUp[] =
    "f4696b4f56aec53585d7854084fb52af688919097505ea393df9f2204f48251";
constexpr char kHashDropTryPickUp[] =
    "d12a250ed4fe660f55f2f9f691e3a55a3bf795136f39fb13349e21cd8c6b9ce";
constexpr char kHashDropTryPickUpByPet[] =
    "bba8465246ffc18f2695ae66e462f0a46c8a072bf0980412ff2eee9e15eccbc";
constexpr char kHashPetGetUpgradeSkill[] =
    "da504f6019c1828761e5254b9dece007bd7e1746f2c9292cb9f326caf2e588b";
// Pet.GetItemSlot：IDA 名 …$$d7bfb7329cc79f1e4bf50183fe6f410674dfb80b6721b7d63db2c2455d7aaf2
constexpr char kHashPetGetItemSlot[] =
    "d7bfb7329cc79f1e4bf50183fe6f410674dfb80b6721b7d63db2c2455d7aaf2";
constexpr char kHashPetIsInException[] =
    "f20a58d0e5ca03c3fe0fcc6d3c4f7855e2ebb4b282d4b99f0cde459ec46515f";
constexpr char kHashPetSendDropPickUp[] =
    "d779339ccf7d143bdeb7f07f5c0046414106886f8acaf427ae63c6dfbee3f71";
constexpr char kHashPoolSendDropPickUp[] =
    "d8757aa736a4495cea79af070f614d069f8dabb40f099fa92b6706cae51abf5";

constexpr char kDropPoolClass[] =
    "c4424063dbd2cca808b166c49a26ca6db164c394978e11ee28e30fdf7c7184a";
// UserLocal → il2cpp_shape::ResolveUserLocalKlass
constexpr char kCollisionCheckClass[] =
    "be8cdb1b8f248e5aee9d45202597c55f2f09596da765a1bae30c334b6bcec1a";  // remounted 2026-08-03 (was e431509c…)
constexpr char kPetClass[] =
    "ce17438d8a1e0a4be3b7a36b24c864280c664caf0a91505de70e167307d5258";

constexpr size_t kOffApPet = 0x2B0;  // TW User.m_apPet 2026-08-03
constexpr size_t kOffWmMyUser = 0x28;
constexpr size_t kOffPoolDict = 0x20;  // TW DropPool._mapDrop OK
constexpr size_t kOffPetRc = 0x100;  // TW Pet._rcPet OK
constexpr size_t kOffPetExceptionList = 0x90;  // TW Pet.ExceptionList OK
constexpr size_t kOffVecCtrl = 0x50;       // VecCtrlOwner.VecCtrl
constexpr size_t kOffFieldPos = 0x64;      // FieldActorBase.Pos — 非脚边真源（见 teleport/P0c）
constexpr size_t kOffCurPos = 0x240;       // TW LocalUser.CurPos（CMS User@0x228 +0x18）
constexpr size_t kOffVcApX = 0x98;         // VecCtrl.AbsPos.X (double)
constexpr size_t kOffVcApY = 0xA0;         // VecCtrl.AbsPos.Y (double)
// Pet 位姿：仍可用 Field Pos，失败再 CurPos（宠与 User 布局不同）
constexpr size_t kOffVisPos = kOffFieldPos;
constexpr size_t kOffLogicalPos = kOffCurPos;
constexpr size_t kOffDropId = 0x30;
constexpr size_t kOffDropOwnType = 0x3C;
// TW dump（restored.B）相对 CMS：0x40 起多了 4 字节，IsMoney/Info 后移
// CMS: IsMoney@0x40 Info@0x44 | TW: int@0x40 IsMoney@0x44 Info@0x48
constexpr size_t kOffDropIsMoney = 0x44;
constexpr size_t kOffDropInfo = 0x48;
constexpr size_t kOffDropPt1 = 0x20;  // Drop.Pt1 = System.Drawing.Point (int x, int y)；TW Drop=f9a2bf4d… OK
constexpr size_t kOffDropEndPara = 0x7C;  // Drop.EndParabolicMotion
constexpr size_t kOffDropLastTry = 0x80;  // Drop.LastTryPickUp
// ByPet 成功 Send 后会盖戳；冷却阈值对照 DropPool.PickUpInterval=3000
constexpr size_t kOffDropPickStamp = 0x88;
// ItemSlotPet.usPetSkill（CMS/TW 同为 +0x3C）；ByPet 先 GetItemSlot(pet) 再读技能
constexpr size_t kOffItemSlotPetSkill = 0x3C;
constexpr size_t kOffCollisionRcPet = 0x20;  // static fields blob +0x20

// DropOwnType（CMS）：User=0 Party=1 No=2 ExplosiveNoOwn=3 UserOwnMoney=4
constexpr int kDropOwnNo = 2;
// IDA ByPet：cmp [drop+0x7C], 3 / cmovz 才继续；写 0 会重置抛物线（近图飞落根因）
constexpr int kEndParaReady = 3;
// 黑名单挡 ByPet：必须 !=3；禁止写 0（会重置抛物线飞落）
constexpr int kEndParaSkipHold = 4;
constexpr int kLastTrySkipStamp = 0x7FFFFFFF;
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kOffDictFreeCount = 0x28;
constexpr size_t kEntrySize = 0x18;
constexpr size_t kOffEntryHash = 0x0;
constexpr size_t kOffEntryValue = 0x10;

constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;

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
using FnPetTryPickUp = void (*)(void* pet, const void* methodInfo);
using FnDropTryPickUp = void (*)(void* pool, const float* posXy /* in Vector2 */, const void* methodInfo);
using FnPetGetSkill = uint16_t (*)(void* pet, const void* methodInfo);
using FnPetGetItemSlot = void* (*)(void* pet, const void* methodInfo);
using FnPetInEx = bool (*)(void* pet, int itemId, const void* methodInfo);
// ByPet 对 Pet.Send 多为直接 call；MI swap 只捕走 methodPointer 的调用。dropsΔ 才是硬证据。
using FnPetSend = bool(__fastcall*)(void* self, uint64_t ptPacked, int dropId, uint32_t crc1,
                                    uint32_t crc2, const void* methodInfo);
using FnPoolSend = void(__fastcall*)(void* self, const void* ptIn, int dropId, uint32_t crc,
                                     const void* methodInfo);

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

void* gDropPoolKlass = nullptr;
void* gDropPool = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gPetKlass = nullptr;
void* gCollisionKlass = nullptr;
MethodInfoHead* gMiTryPickUp = nullptr;
MethodInfoHead* gMiFootTryPickUp = nullptr;
MethodInfoHead* gMiGetSkill = nullptr;
MethodInfoHead* gMiGetItemSlot = nullptr;
MethodInfoHead* gMiInEx = nullptr;
MethodInfoHead* gMiPetSend = nullptr;
MethodInfoHead* gMiPoolSend = nullptr;
void* gOrigPetSend = nullptr;
void* gOrigPoolSend = nullptr;
std::atomic<uint32_t> gPetSendHits{0};
std::atomic<uint32_t> gPoolSendHits{0};
std::atomic<bool> gSendProbeInstalled{false};

DWORD gLastLuRebind = 0;
DWORD gLastPoolRebind = 0;

struct VacJob {
    float vacuumW = 300.f;
    float vacuumH = 200.f;
    SkipIds skip{};
    VacuumResult result{};
    bool done = false;
};

struct FootJob {
    float halfW = 100.f;
    float halfH = 80.f;
    SkipIds skip{};
    FootResult result{};
    bool done = false;
};

std::atomic<bool> gJobPending{false};
VacJob gJob{};
std::atomic<bool> gFootPending{false};
FootJob gFoot{};

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

uint16_t ReadU16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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

void WriteF32(void* obj, size_t off, float v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteI32(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Drop.Pt1：CMS/TW 均为 System.Drawing.Point（int x/y），禁止当 float 读。
bool ReadDropPt(void* drop, float& x, float& y) {
    x = y = 0.f;
    if (!drop) return false;
    x = static_cast<float>(ReadI32(drop, kOffDropPt1));
    y = static_cast<float>(ReadI32(drop, kOffDropPt1 + 4));
    return true;
}

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGA) return nullptr;
    const void* want = AtRva<void*>(rva);
    void* iter = nullptr;
    __try {
        while (true) {
            void* mi = gClassGetMethods(klass, &iter);
            if (!mi) break;
            auto* head = reinterpret_cast<MethodInfoHead*>(mi);
            if (head->methodPointer == want || head->virtualMethodPointer == want) return head;
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

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (plain) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plain, shape.arity)) return mi;
    }
    if (hash) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hash, shape.arity)) return mi;
    }
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("DropPort", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plain ? plain : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    *outOrig = mi->methodPointer;
    mi->methodPointer = hook;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return true;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    mi->methodPointer = orig;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

bool __fastcall HookPetSend(void* self, uint64_t ptPacked, int dropId, uint32_t crc1, uint32_t crc2,
                            const void* methodInfo) {
    gPetSendHits.fetch_add(1, std::memory_order_relaxed);
    auto* fn = reinterpret_cast<FnPetSend>(gOrigPetSend);
    return fn ? fn(self, ptPacked, dropId, crc1, crc2, methodInfo) : false;
}

void __fastcall HookPoolSend(void* self, const void* ptIn, int dropId, uint32_t crc,
                             const void* methodInfo) {
    gPoolSendHits.fetch_add(1, std::memory_order_relaxed);
    auto* fn = reinterpret_cast<FnPoolSend>(gOrigPoolSend);
    if (fn) fn(self, ptIn, dropId, crc, methodInfo);
}

void EnsureSendProbe() {
    if (gSendProbeInstalled.load(std::memory_order_acquire)) return;
    if (!gGA || !gClassGetMethods) return;
    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
    bool okPet = false;
    bool okPool = false;
    if (gPetKlass && !gMiPetSend) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        // bool(Point,int,uint,uint) 唯一。
        constexpr MethodShape kPetSend{
            4, TypeKind::Bool, true, false,
            {TypeKind::Any, TypeKind::I32, TypeKind::U32, TypeKind::U32}};
        gMiPetSend = ResolveMi(gPetKlass, kRvaPetSendDropPickUp, kPetSend, "SendDropPickUpRequest",
                               kHashPetSendDropPickUp);
        if (gMiPetSend &&
            PatchMethodInfo(gMiPetSend, reinterpret_cast<void*>(&HookPetSend), &gOrigPetSend)) {
            okPet = true;
        }
    } else if (gMiPetSend && gOrigPetSend) {
        okPet = true;
    }
    if (gDropPoolKlass && !gMiPoolSend) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        // void(in Point,int,uint) 唯一。
        constexpr MethodShape kPoolSend{
            3, TypeKind::Void, true, false, {TypeKind::Any, TypeKind::I32, TypeKind::U32}};
        gMiPoolSend = ResolveMi(gDropPoolKlass, kRvaPoolSendDropPickUp, kPoolSend,
                                "SendDropPickUpRequest", kHashPoolSendDropPickUp);
        if (gMiPoolSend &&
            PatchMethodInfo(gMiPoolSend, reinterpret_cast<void*>(&HookPoolSend), &gOrigPoolSend)) {
            okPool = true;
        }
    } else if (gMiPoolSend && gOrigPoolSend) {
        okPool = true;
    }
    if (okPet || okPool) {
        gSendProbeInstalled.store(true, std::memory_order_release);
        x::runtime::LogI("DropPort",
                         "SendProbe MI pet=%d pool=%d (direct-call ByPet 可能不经 MI；以 dropsΔ 为准)",
                         okPet ? 1 : 0, okPool ? 1 : 0);
    }
}

int ReadPoolDropCount(void* pool) {
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    const int count = ReadI32(dict, kOffDictCount);
    const int freeCount = ReadI32(dict, kOffDictFreeCount);
    if (count < 0 || count > 4096) return 0;
    const int live = count - freeCount;
    return live >= 0 ? live : count;
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

bool BindIl2Cpp() {
    if (gGA) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gClassGetMethods = e.classGetMethods;
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gRuntimeClassInit = e.runtimeClassInit;
    gCompGo = e.compGo;
    gObjName = e.objName;
    return gGA != nullptr;
}

bool ReadIl2CppString(void* str, char* out, size_t outCap) {
    out[0] = 0;
    if (!str || outCap < 2) return false;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 512) return false;
        const auto* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            if (c < 0x80) out[n++] = static_cast<char>(c);
            else if (c < 0x800 && n + 2 < outCap) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            } else if (n + 3 < outCap) {
                out[n++] = static_cast<char>(0xE0 | (c >> 12));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            }
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

bool IsMyUserGo(void* user) {
    char name[96]{};
    return GetGoName(user, name, sizeof(name)) && _stricmp(name, "MyUser") == 0;
}

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

// LocalUser / Component：+0x10 是 Unity m_CachedPtr（原生句柄），不能当托管堆指针校验。
bool LocalUserStillAlive(void* user) {
    if (!LooksLikeHeapPtr(user)) return false;
    __try {
        if (!ReadPtr(user, 0)) return false;
        const void* cached = ReadPtr(user, 0x10);
        if (!cached) return false;  // 已销毁
        return IsMyUserGo(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// DropPool 等纯托管 Singleton：只看 klass 指针。
bool ManagedAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) != nullptr;
}

// Pet 等 Unity 对象：m_CachedPtr@+0x10 非 0 即存活（原生句柄，勿 LooksLikeHeapPtr）。
bool UnityObjectAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    if (!ReadPtr(obj, 0)) return false;
    return ReadPtr(obj, 0x10) != nullptr;
}

// 解冻权：仅玩法就绪时清 freeze；大厅绑定 MyUser 不抢 auto_enter 的登录冻结。
void MaybeClearLoginFreeze() {
    if (!x::runtime::managed_main::IsLoginFrozen()) return;
    if (world::IsPlayReady()) x::runtime::managed_main::SetLoginFreeze(false);
}

bool ResolveLocalUser(DWORD now) {
    // 热路径：只看 Unity m_CachedPtr；名字校验放到真正 rebind。
    // 换图：WM.MyUser 指针变了立刻失效，不受 kRebindMs 保护。
    if (gLocalUser && now - gLastLuRebind < kRebindMs) {
        void* wm = world::PeekWorldManager();
        void* myUser = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (LooksLikeHeapPtr(myUser) && myUser != gLocalUser) {
            gLocalUser = nullptr;
            gLastLuRebind = 0;  // 强制 fall-through 立刻重绑
        } else if (!LooksLikeHeapPtr(myUser)) {
            // 换图空窗 MyUser 暂空：丢掉旧缓存，勿继续当活的用。
            gLocalUser = nullptr;
            gLastLuRebind = 0;
        } else if (LooksLikeHeapPtr(gLocalUser) && ReadPtr(gLocalUser, 0) &&
                   ReadPtr(gLocalUser, 0x10)) {
            MaybeClearLoginFreeze();
            return true;
        } else {
            gLocalUser = nullptr;
        }
    }
    if (gLastLuRebind && now - gLastLuRebind < kRebindMs && !gLocalUser) return false;
    gLastLuRebind = now;
    gLocalUser = nullptr;
    if (!BindIl2Cpp()) return false;

    void* wm = world::PeekWorldManager();
    if (!wm) wm = world::GetWorldManager();
    void* myUser = ReadPtr(wm, kOffWmMyUser);
    if (LooksLikeHeapPtr(myUser) && IsMyUserGo(myUser)) {
        gLocalUser = myUser;
        x::runtime::LogI("DropPort", "LocalUser ACCEPT wm.MyUser=%p", gLocalUser);
        MaybeClearLoginFreeze();
        return true;
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
    for (uintptr_t i = 0; i < n && i < 64; ++i) {
        void* cand = ArrayAt(arr, i);
        if (!LooksLikeHeapPtr(cand) || !IsMyUserGo(cand)) continue;
        gLocalUser = cand;
        x::runtime::LogI("DropPort", "LocalUser ACCEPT FindAll=%p", gLocalUser);
        MaybeClearLoginFreeze();
        return true;
    }
    return false;
}

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

bool LooksLikeDropPool(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gDropPoolKlass && !ObjKlassIs(cand, gDropPoolKlass)) return false;
    void* dict = ReadPtr(cand, kOffPoolDict);
    // 允许 dict 尚未填充（刚进图）；但若有指针必须像堆。
    if (!dict) return true;
    return LooksLikeHeapPtr(dict);
}

int DropPoolScore(void* cand) {
    if (!LooksLikeDropPool(cand)) return -1;
    void* dict = ReadPtr(cand, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    const int count = ReadI32(dict, kOffDictCount);
    if (count < 0 || count > 4096) return 0;
    return count > 0 ? (1000 + (count > 500 ? 500 : count)) : 1;
}

void* ResolveDropPool(DWORD now) {
    if (gDropPool && LooksLikeDropPool(gDropPool) && ManagedAlive(gDropPool) &&
        now - gLastPoolRebind < kRebindMs)
        return gDropPool;
    if (gLastPoolRebind && now - gLastPoolRebind < kRebindMs && !gDropPool) return nullptr;
    gLastPoolRebind = now;
    gDropPool = nullptr;
    if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
    if (!gDropPoolKlass) {
        x::runtime::LogWThrottled(21, 15000, "DropPort", "DropPool klass miss");
        return nullptr;
    }

    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gDropPoolKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* staticsKlass = gDropPoolKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gDropPoolKlass);
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
    if (!statics) statics = KlassStaticFields(gDropPoolKlass);

    void* best = nullptr;
    int bestScore = -1;
    if (statics) {
        // Singleton<T>.Lazy 通常在 parent statics +0；扫宽一点。
        for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
            void* lazy = ReadPtr(statics, s);
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (LooksLikeHeapPtr(cand) && ObjKlassIs(cand, gDropPoolKlass)) {
                const int sc = DropPoolScore(cand);
                if (sc > bestScore) {
                    bestScore = sc;
                    best = cand;
                    if (sc >= 1000) break;
                }
            }
        }
    }

    // Fallback：FindAll(DropPool) — Lazy 未初始化或 static 槽扫空时兜底。
    if (!best) {
        void* typeObj = FindClassTypeObject(kDropPoolClass);
        if (typeObj && gFindAll) {
            void* arr = nullptr;
            __try {
                arr = x::runtime::managed_main::FindAll(gFindAll, typeObj, 2000);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                arr = nullptr;
            }
            const uintptr_t n = ArrayLen(arr);
            for (uintptr_t i = 0; i < n && i < 8; ++i) {
                void* cand = ArrayAt(arr, i);
                const int sc = DropPoolScore(cand);
                if (sc > bestScore) {
                    bestScore = sc;
                    best = cand;
                }
            }
            if (best) {
                x::runtime::LogI("DropPort", "DropPool FindAll hit %p score=%d n=%llu", best,
                                 bestScore, (unsigned long long)n);
            }
        }
    }

    if (best) {
        gDropPool = best;
        x::runtime::LogI("DropPort", "DropPool bind %p score=%d", gDropPool, bestScore);
        return gDropPool;
    }
    x::runtime::LogWThrottled(22, 15000, "DropPort", "DropPool resolve miss (statics=%p klass=%p)",
                              statics, gDropPoolKlass);
    return nullptr;
}

void ReadPetPos(void* pet, float& x, float& y) {
    x = ReadF32(pet, kOffVisPos);
    y = ReadF32(pet, kOffVisPos + 4);
    if (std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs) return;
    x = ReadF32(pet, kOffLogicalPos);
    y = ReadF32(pet, kOffLogicalPos + 4);
}

Rect4 ReadRect(void* base, size_t off) {
    Rect4 r{};
    r.x = ReadF32(base, off);
    r.y = ReadF32(base, off + 4);
    r.w = ReadF32(base, off + 8);
    r.h = ReadF32(base, off + 12);
    return r;
}

void* FirstActivePet() {
    if (!gLocalUser) return nullptr;
    void* arr = ReadPtr(gLocalUser, kOffApPet);
    if (!LooksLikeHeapPtr(arr)) return nullptr;
    const uintptr_t n = ArrayLen(arr);
    if (n == 0 || n > 8) return nullptr;
    for (uintptr_t i = 0; i < n; ++i) {
        void* pet = ArrayAt(arr, i);
        if (LooksLikeHeapPtr(pet) && UnityObjectAlive(pet)) return pet;
    }
    return nullptr;
}

uint16_t ReadPetSkill(void* pet) {
    if (!pet || !gGA) return 0;
    if (!gMiGetSkill && gPetKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kSk{0, TypeKind::Any, true, false, {}};
        gMiGetSkill = ResolveMi(gPetKlass, kRvaPetGetUpgradePetSkill, kSk, "GetUpgradePetSkill",
                                kHashPetGetUpgradeSkill);
    }
    auto fn = FnFromMi<FnPetGetSkill>(gMiGetSkill, kRvaPetGetUpgradePetSkill);
    if (!fn) return 0;
    uint16_t skill = 0;
    __try {
        skill = fn(pet, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skill = 0;
    }
    return skill;
}

// ByPet 真源：GetItemSlot(pet) → ItemSlotPet.usPetSkill@+0x3C（勿再读 Pet+0x428）
uint16_t ReadPetSkillSlot(void* pet) {
    if (!pet || !gGA) return 0;
    if (!gMiGetItemSlot && gPetKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kSlot{0, TypeKind::Any, true, false, {}};
        gMiGetItemSlot =
            ResolveMi(gPetKlass, kRvaPetGetItemSlot, kSlot, "GetItemSlot", kHashPetGetItemSlot);
    }
    auto fn = FnFromMi<FnPetGetItemSlot>(gMiGetItemSlot, kRvaPetGetItemSlot);
    if (!fn) return 0;
    void* slot = nullptr;
    __try {
        slot = fn(pet, gMiGetItemSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    if (!LooksLikeHeapPtr(slot)) return 0;
    return ReadU16(slot, kOffItemSlotPetSkill);
}

// 只读：probe 对照原生托管矩形；ByPet Contains 不读这里。
Rect4 ReadCollisionRcPet() {
    Rect4 empty{};
    if (!gCollisionKlass) gCollisionKlass = FindClass(kCollisionCheckClass);
    if (!gCollisionKlass) return empty;
    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gCollisionKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void* statics = KlassStaticFields(gCollisionKlass);
    if (!LooksLikeHeapPtr(statics)) return empty;
    return ReadRect(statics, kOffCollisionRcPet);
}

// ByPet 用 .rdata 常量组 Contains 矩形；扩 _rcPet/CollisionCheck 对这条链无效。
struct ByPetRectBackup {
    int32_t offX = 25;
    int32_t offY = 10;
    float w = 50.f;
    float h = 60.f;
    bool ok = false;
    DWORD oldProtect = 0;
    uint8_t* base = nullptr;
};

bool PatchByPetRectPack(float vacuumW, float vacuumH, ByPetRectBackup& bak) {
    bak = {};
    if (!gGA || vacuumW < 1.f || vacuumH < 1.f) return false;
    bak.base = AtRva<uint8_t*>(kRvaByPetRectPack);
    if (!bak.base) return false;
    __try {
        bak.offX = *reinterpret_cast<int32_t*>(bak.base + 0);
        bak.offY = *reinterpret_cast<int32_t*>(bak.base + 4);
        bak.w = *reinterpret_cast<float*>(bak.base + 0x10);
        bak.h = *reinterpret_cast<float*>(bak.base + 0x14);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!VirtualProtect(bak.base, 0x20, PAGE_READWRITE, &bak.oldProtect)) return false;
    const int32_t offX = static_cast<int32_t>(vacuumW * 0.5f);
    const int32_t offY = static_cast<int32_t>(vacuumH * 0.5f);
    *reinterpret_cast<int32_t*>(bak.base + 0) = offX;
    *reinterpret_cast<int32_t*>(bak.base + 4) = offY;
    *reinterpret_cast<float*>(bak.base + 0x10) = vacuumW;
    *reinterpret_cast<float*>(bak.base + 0x14) = vacuumH;
    bak.ok = true;
    return true;
}

void RestoreByPetRectPack(ByPetRectBackup& bak) {
    if (!bak.ok || !bak.base) return;
    __try {
        *reinterpret_cast<int32_t*>(bak.base + 0) = bak.offX;
        *reinterpret_cast<int32_t*>(bak.base + 4) = bak.offY;
        *reinterpret_cast<float*>(bak.base + 0x10) = bak.w;
        *reinterpret_cast<float*>(bak.base + 0x14) = bak.h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DWORD tmp = 0;
    VirtualProtect(bak.base, 0x20, bak.oldProtect, &tmp);
    bak.ok = false;
}

// 宠吸拍前门控（IDA ByPet 实锤）：
// - EndParabolicMotion 必须 == 3 才继续；绝不能写 0（重置抛物线 → 全体飞落）
// - +0x88 为 Send 盖戳冷却（对照 PickUpInterval=3000）
// - LastTry / 异常 OwnType 仍清
// - 黑名单 drop 一律不碰（否则每拍清 LastTry + 写 EndPara=3 → ByPet 空吸动画「打转」）
bool DropMatchesSkip(void* drop, const SkipIds& skip);

int ClearPickupGatesNear(void* pool, float cx, float cy, float halfW, float halfH,
                         int* outSampleOwn, int* outSampleLast, int* outSampleEnd,
                         const SkipIds* skip) {
    if (outSampleOwn) *outSampleOwn = -1;
    if (outSampleLast) *outSampleLast = 0;
    if (outSampleEnd) *outSampleEnd = 0;
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int cleared = 0;
    bool sampled = false;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;

        const int own = ReadI32(drop, kOffDropOwnType);
        const int last = ReadI32(drop, kOffDropLastTry);
        const int endp = ReadI32(drop, kOffDropEndPara);
        const int stamp = ReadI32(drop, kOffDropPickStamp);
        if (!sampled) {
            sampled = true;
            if (outSampleOwn) *outSampleOwn = own;
            if (outSampleLast) *outSampleLast = last;
            if (outSampleEnd) *outSampleEnd = endp;
        }
        bool touched = false;
        // ByPet：EndPara != 3 → 直接跳过该 drop（日志常见 sampleEndPara=1）
        if (endp != kEndParaReady) {
            WriteI32(drop, kOffDropEndPara, kEndParaReady);
            touched = true;
        }
        if (stamp != 0) {
            WriteI32(drop, kOffDropPickStamp, 0);
            touched = true;
        }
        // 空成功会盖 LastTry 冷却；只在非 0 时清，避免无意义写
        if (last != 0) {
            WriteI32(drop, kOffDropLastTry, 0);
            touched = true;
        }
        // TW 实机 OwnType 常读到异常大数（非 0–4 枚举），ByPet 会直接跳过；写成 No=任何人可捡
        if (own < 0 || own > 4) {
            WriteI32(drop, kOffDropOwnType, kDropOwnNo);
            touched = true;
        }
        if (touched) ++cleared;
    }
    return cleared;
}

int CountDropsNear(void* pool, float cx, float cy, float halfW, float halfH, int* outTotal,
                   int* outNearMoney = nullptr, int* outNearItem = nullptr,
                   int* outSampleIsMoney = nullptr, int* outSampleInfo = nullptr) {
    if (outTotal) *outTotal = 0;
    if (outNearMoney) *outNearMoney = 0;
    if (outNearItem) *outNearItem = 0;
    if (outSampleIsMoney) *outSampleIsMoney = -1;
    if (outSampleInfo) *outSampleInfo = 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int total = 0, nearN = 0, nearMoney = 0, nearItem = 0;
    bool haveMoneySample = false;
    bool haveAnySample = false;
    int sampleMoneyFlag = -1;
    int sampleInfo = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        ++total;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        ++nearN;
        const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
        const int info = ReadI32(drop, kOffDropInfo);
        if (money) {
            ++nearMoney;
            if (!haveMoneySample) {
                haveMoneySample = true;
                haveAnySample = true;
                sampleMoneyFlag = 1;
                sampleInfo = info;
            }
        } else {
            ++nearItem;
            if (!haveAnySample) {
                haveAnySample = true;
                sampleMoneyFlag = 0;
                sampleInfo = info;
            }
        }
    }
    if (outTotal) *outTotal = total;
    if (outNearMoney) *outNearMoney = nearMoney;
    if (outNearItem) *outNearItem = nearItem;
    if (outSampleIsMoney) *outSampleIsMoney = sampleMoneyFlag;
    if (outSampleInfo) *outSampleInfo = sampleInfo;
    return nearN;
}

// 拍前 ClearPickupGates 已清 LastTry/PickStamp；拍后非 0 = ByPet 本拍碰过（多为 Send）
int CountPostSendTouchesNear(void* pool, float cx, float cy, float halfW, float halfH,
                             int* outTouchMoney) {
    if (outTouchMoney) *outTouchMoney = 0;
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int touch = 0, touchMoney = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        const int last = ReadI32(drop, kOffDropLastTry);
        const int stamp = ReadI32(drop, kOffDropPickStamp);
        if (last == 0 && stamp == 0) continue;
        ++touch;
        if (ReadU8(drop, kOffDropIsMoney) != 0) ++touchMoney;
    }
    if (outTouchMoney) *outTouchMoney = touchMoney;
    return touch;
}

bool EnsureExceptionIds(void* pet, const SkipIds& skip) {
    if (!pet || skip.empty()) return true;
    void* list = ReadPtr(pet, kOffPetExceptionList);
    if (!LooksLikeHeapPtr(list)) return false;
    void* items = ReadPtr(list, kOffListItems);
    int size = ReadI32(list, kOffListSize);
    if (!LooksLikeHeapPtr(items) || size < 0 || size > 512) return false;
    const uintptr_t cap = ArrayLen(items);
    if (cap == 0 || cap > 1024) return false;

    // 已有表 → set，避免宽词三千次线性扫表（读写走 ReadI32/WriteI32，勿在本函数内 __try）
    std::unordered_set<int> have;
    have.reserve(static_cast<size_t>(size) + 16);
    for (int i = 0; i < size; ++i) {
        const int v = ReadI32(items, 0x20 + (uintptr_t)i * 4);
        if (v > 0) have.insert(v);
    }

    // 托管 ExceptionList 容量很小；主挡捡靠 LastTry+EndPara。此处只尽量补满 cap。
    for (const int id : skip.ids) {
        if (id <= 0) continue;
        if (have.find(id) != have.end()) continue;
        if ((uintptr_t)size >= cap) break;
        WriteI32(items, 0x20 + (uintptr_t)size * 4, id);
        if (ReadI32(items, 0x20 + (uintptr_t)size * 4) != id) return false;
        ++size;
        WriteI32(list, kOffListSize, size);
        have.insert(id);
    }
    return true;
}

// 仅在 skip 集合或宠指针变化时同步 ExceptionList（宽词每拍全量扫会卡主线程）
size_t HashSkipIds(const SkipIds& skip) {
    size_t h = skip.size();
    for (const int id : skip.ids) {
        h ^= static_cast<size_t>(id) + 0x9e3779b9u + (h << 6) + (h >> 2);
    }
    return h;
}

bool EnsureExceptionIdsIfNeeded(void* pet, const SkipIds& skip) {
    if (!pet || skip.empty()) return true;
    static void* s_lastPet = nullptr;
    static size_t s_lastHash = 0;
    const size_t h = HashSkipIds(skip);
    if (pet == s_lastPet && h == s_lastHash) return true;
    const bool ok = EnsureExceptionIds(pet, skip);
    if (ok) {
        s_lastPet = pet;
        s_lastHash = h;
    }
    return ok;
}

int StampSkippedDropsNear(void* pool, float cx, float cy, float halfW, float halfH,
                          const SkipIds& skip, int* outNear, int* outWant, int* outTotal);

void RunVacuumOnMain() {
    VacuumResult& r = gJob.result;
    r = {};
    r.why = "fail";

    const DWORD now = GetTickCount();
    if (!ResolveLocalUser(now)) {
        r.why = "no_lu";
        return;
    }
    void* pool = ResolveDropPool(now);

    void* pet = FirstActivePet();
    if (!pet) {
        r.why = "no_pet";
        return;
    }

    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    EnsureSendProbe();
    const uint16_t skill = ReadPetSkill(pet);
    r.petSkill = skill;
    r.petSkillSlot = ReadPetSkillSlot(pet);
    if ((skill & kPetSkillPickupItem) == 0) {
        r.why = "no_skill";
        return;
    }

    float px = 0.f, py = 0.f;
    ReadPetPos(pet, px, py);
    r.dropCount = ReadPoolDropCount(pool);
    if (pool) {
        int totalEnum = 0;
        r.nearCount = CountDropsNear(pool, px, py, gJob.vacuumW * 0.5f, gJob.vacuumH * 0.5f,
                                    &totalEnum, &r.nearMoney, &r.nearItem, &r.sampleIsMoney,
                                    &r.sampleInfo);
        if (r.dropCount <= 0 && totalEnum > 0) r.dropCount = totalEnum;
        r.gatesCleared =
            ClearPickupGatesNear(pool, px, py, gJob.vacuumW * 0.5f, gJob.vacuumH * 0.5f,
                                 &r.sampleOwnType, &r.sampleLastTry, &r.sampleEndPara,
                                 gJob.skip.empty() ? nullptr : &gJob.skip);
        // 黑名单：脚边同款盖 LastTry=INT_MAX（ExceptionList 容量/官方门控不可靠；宽词截断曾漏 紅寶殼）
        if (gJob.skip.size() > 0) {
            int nearIgn = 0, wantIgn = 0, totalIgn = 0;
            r.skipStamped =
                StampSkippedDropsNear(pool, px, py, gJob.vacuumW * 0.5f, gJob.vacuumH * 0.5f,
                                      gJob.skip, &nearIgn, &wantIgn, &totalIgn);
        }
    }

    if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);

    // 只读诊断：托管 _rcPet / CollisionCheck 对 ByPet Contains 无效，不再写入。
    r.beforeRc = ReadRect(pet, kOffPetRc);

    Rect4 vacuum{};
    vacuum.x = -gJob.vacuumW * 0.5f;
    vacuum.y = -gJob.vacuumH * 0.5f;
    vacuum.w = gJob.vacuumW;
    vacuum.h = gJob.vacuumH;
    // 日志 rc= 表示本拍意图真空尺寸（实际写入 .rdata 矩形包）
    r.afterRc = vacuum;

    ByPetRectBackup rectBak{};
    const bool rectPatched = PatchByPetRectPack(gJob.vacuumW, gJob.vacuumH, rectBak);

    if (!gMiTryPickUp && gPetKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kTry{0, TypeKind::Void, true, false, {}};
        gMiTryPickUp = ResolveMi(gPetKlass, kRvaPetTryPickUpDrop, kTry, "TryPickUpDrop",
                                 kHashPetTryPickUp);
    }
    auto fn = FnFromMi<FnPetTryPickUp>(gMiTryPickUp, kRvaPetTryPickUpDrop);
    const uint32_t petHits0 = gPetSendHits.load(std::memory_order_relaxed);
    const uint32_t poolHits0 = gPoolSendHits.load(std::memory_order_relaxed);
    bool ok = false;
    __try {
        if (fn && rectPatched) {
            // 官方脚边同类调用 MethodInfo=nullptr；传 MI 在 CFF 下偶发早退
            fn(pet, nullptr);
            ok = true;
        } else if (fn && !rectPatched) {
            r.why = "no_rect_patch";
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        r.why = "seh";
    }

    RestoreByPetRectPack(rectBak);

    r.dropCountAfter = ReadPoolDropCount(pool);
    r.dropsDelta = r.dropCountAfter - r.dropCount;
    r.petSendHits = gPetSendHits.load(std::memory_order_relaxed);
    r.poolSendHits = gPoolSendHits.load(std::memory_order_relaxed);
    r.petSendDelta = r.petSendHits - petHits0;
    r.poolSendDelta = r.poolSendHits - poolHits0;

    // 服端异步删 drop：同拍前后常仍相等；用跨拍池下降作真吸证据
    static int s_prevDropAfter = -1;
    static int s_prevNear = 0;
    static int s_prevGates = 0;
    static DWORD s_prevTick = 0;
    static bool s_prevOk = false;
    const DWORD nowTick = GetTickCount();
    const bool recentPrev =
        s_prevOk && s_prevTick != 0 && (nowTick - s_prevTick) <= 3000u;
    const bool poolFell = recentPrev && s_prevDropAfter > 0 && r.dropCount < s_prevDropAfter &&
                          (s_prevNear > 0 || s_prevGates > 0);
    r.poolFellSinceLast = poolFell;

    // 仅在「调用成功且本拍池未同步下降」时轻扫盖戳，避免成功吸收再多走一趟
    if (ok && pool && r.nearCount > 0 && r.dropsDelta >= 0 && !poolFell) {
        r.sendTouch =
            CountPostSendTouchesNear(pool, px, py, gJob.vacuumW * 0.5f, gJob.vacuumH * 0.5f,
                                     &r.sendTouchMoney);
        const bool probeSend = r.petSendDelta > 0 || r.poolSendDelta > 0;
        if (r.sendTouch > 0 || probeSend) r.sentButPoolSame = 1;
    }

    r.called = ok;
    r.ok = ok;
    if (ok) {
        if (r.dropsDelta < 0 || poolFell)
            r.why = "ok_absorbed";
        else if (r.nearCount > 0)
            r.why = "ok";
        else
            r.why = "ok_empty";
    }

    if (ok) {
        s_prevDropAfter = r.dropCountAfter;
        s_prevNear = r.nearCount;
        s_prevGates = r.gatesCleared;
        s_prevTick = nowTick;
        s_prevOk = true;
    }
}

void VacJobThunk(void*) { RunVacuumOnMain(); }

bool SkipHas(const SkipIds& skip, int id) { return skip.contains(id); }

bool DropMatchesSkip(void* drop, const SkipIds& skip) {
    if (!drop || skip.empty()) return false;
    const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
    if (money) return SkipHas(skip, kMesoSkipId);
    const int info = ReadI32(drop, kOffDropInfo);
    return info > 0 && SkipHas(skip, info);
}

bool ReadUserPos(float& x, float& y) {
    x = y = 0.f;
    if (!gLocalUser) return false;

    // 脚边 TryPickUpDrop 与 Drop.Pt1 同属身体/逻辑空间。
    // 禁止优先读 FieldActorBase.Pos@0x64（镜头/镜像会漂，贴脸仍 near=0 / ok_far）。
    // 顺序：CurPos@0x240 → VecCtrl.Ap → Pos@0x64 兜底。
    x = ReadF32(gLocalUser, kOffCurPos);
    y = ReadF32(gLocalUser, kOffCurPos + 4);
    if (std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs) return true;

    void* vc = ReadPtr(gLocalUser, kOffVecCtrl);
    if (LooksLikeHeapPtr(vc)) {
        const double ax = ReadF64(vc, kOffVcApX);
        const double ay = ReadF64(vc, kOffVcApY);
        if (std::fabs(ax) >= kMinPosAbs || std::fabs(ay) >= kMinPosAbs) {
            x = static_cast<float>(ax);
            y = static_cast<float>(ay);
            return true;
        }
    }

    x = ReadF32(gLocalUser, kOffFieldPos);
    y = ReadF32(gLocalUser, kOffFieldPos + 4);
    return std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs;
}

// 黑名单：LastTry=INT_MAX + EndPara=SkipHold(!=3) → ByPet 早退，避免空吸打转
int StampSkippedDropsNear(void* pool, float cx, float cy, float halfW, float halfH,
                          const SkipIds& skip, int* outNear, int* outWant, int* outTotal) {
    if (outNear) *outNear = 0;
    if (outWant) *outWant = 0;
    if (outTotal) *outTotal = 0;
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int total = 0, nearN = 0, want = 0, stamped = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        ++total;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        const float dx = px - cx;
        const float dy = py - cy;
        if (std::fabs(dx) > halfW || std::fabs(dy) > halfH) continue;
        ++nearN;
        if (DropMatchesSkip(drop, skip)) {
            const int last = ReadI32(drop, kOffDropLastTry);
            const int endp = ReadI32(drop, kOffDropEndPara);
            bool touched = false;
            if (last != kLastTrySkipStamp) {
                WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
                touched = true;
            }
            // Ready(3) 会被 ByPet 吸入动画；改成 SkipHold，且绝不写 0
            if (endp == kEndParaReady) {
                WriteI32(drop, kOffDropEndPara, kEndParaSkipHold);
                touched = true;
            }
            if (touched) ++stamped;
            continue;
        }
        ++want;
    }
    if (outNear) *outNear = nearN;
    if (outWant) *outWant = want;
    if (outTotal) *outTotal = total;
    return stamped;
}

void RunFootOnMain() {
    FootResult& r = gFoot.result;
    r = {};
    r.why = "fail";

    const DWORD now = GetTickCount();
    if (!ResolveLocalUser(now)) {
        r.why = "no_lu";
        return;
    }
    void* pool = ResolveDropPool(now);
    if (!pool) {
        r.why = "no_pool";
        return;
    }
    EnsureSendProbe();

    float ux = 0.f, uy = 0.f;
    if (!ReadUserPos(ux, uy)) {
        r.why = "no_pos";
        return;
    }
    r.userX = ux;
    r.userY = uy;

    const int poolBefore = ReadPoolDropCount(pool);
    r.stamped = StampSkippedDropsNear(pool, ux, uy, gFoot.halfW, gFoot.halfH, gFoot.skip,
                                      &r.nearCount, &r.nearWant, &r.dropCount);
    if (poolBefore > 0) r.dropCount = poolBefore;

    // 枚举失败/空池时仍调用官方 TryPickUpDrop：盖戳只是黑名单辅助，不能当「有没有掉落」门禁。
    if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
    if (!gMiFootTryPickUp && gDropPoolKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        // void(in Vector2) 唯一。
        constexpr MethodShape kFoot{1, TypeKind::Void, true, false, {TypeKind::Any}};
        gMiFootTryPickUp = ResolveMi(gDropPoolKlass, kRvaDropTryPickUpDrop, kFoot, "TryPickUpDrop",
                                     kHashDropTryPickUp);
    }
    auto fn = FnFromMi<FnDropTryPickUp>(gMiFootTryPickUp, kRvaDropTryPickUpDrop);
    float pos[2] = {ux, uy};
    const uint32_t petHits0 = gPetSendHits.load(std::memory_order_relaxed);
    const uint32_t poolHits0 = gPoolSendHits.load(std::memory_order_relaxed);
    bool ok = false;
    __try {
        if (fn) {
            fn(pool, pos, gMiFootTryPickUp);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        r.why = "seh";
    }
    r.dropCountAfter = ReadPoolDropCount(pool);
    r.dropsDelta = r.dropCountAfter - r.dropCount;
    r.petSendHits = gPetSendHits.load(std::memory_order_relaxed);
    r.poolSendHits = gPoolSendHits.load(std::memory_order_relaxed);
    r.petSendDelta = r.petSendHits - petHits0;
    r.poolSendDelta = r.poolSendHits - poolHits0;
    r.called = ok;
    r.ok = ok;

    static int s_footPrevAfter = -1;
    static int s_footPrevNearWant = 0;
    static DWORD s_footPrevTick = 0;
    static bool s_footPrevOk = false;
    const DWORD nowTick = GetTickCount();
    const bool recentPrev =
        s_footPrevOk && s_footPrevTick != 0 && (nowTick - s_footPrevTick) <= 3000u;
    const bool poolFell = recentPrev && s_footPrevAfter > 0 && r.dropCount < s_footPrevAfter &&
                          s_footPrevNearWant > 0;

    if (ok) {
        if (r.dropsDelta < 0 || poolFell) r.why = "ok_absorbed";
        else if (r.nearWant > 0) r.why = "ok";
        else if (r.nearCount > 0) r.why = "ok_all_skip";
        else if (r.dropCount > 0) r.why = "ok_far";
        else r.why = "ok_empty";
    } else if (!r.why || !r.why[0] || strcmp(r.why, "fail") == 0) {
        r.why = "no_fn";
    }

    if (ok) {
        s_footPrevAfter = r.dropCountAfter;
        s_footPrevNearWant = r.nearWant;
        s_footPrevTick = nowTick;
        s_footPrevOk = true;
    }
}

void FootJobThunk(void*) { RunFootOnMain(); }

}  // namespace

void Init() {
    BindIl2Cpp();
    x::runtime::LogI("DropPort", "init pet_loot port (formal=pet vacuum; foot optional)");
}

void Shutdown() {
    if (gMiPetSend && gOrigPetSend) RestoreMethodInfo(gMiPetSend, gOrigPetSend);
    if (gMiPoolSend && gOrigPoolSend) RestoreMethodInfo(gMiPoolSend, gOrigPoolSend);
    gMiPetSend = nullptr;
    gMiPoolSend = nullptr;
    gOrigPetSend = nullptr;
    gOrigPoolSend = nullptr;
    gSendProbeInstalled.store(false, std::memory_order_release);
    gDropPool = nullptr;
    gLocalUser = nullptr;
}

bool EnsureBound() {
    if (!BindIl2Cpp()) return false;
    const DWORD now = GetTickCount();
    const bool lu = ResolveLocalUser(now);
    void* pool = ResolveDropPool(now);
    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    if (lu && pool) EnsureSendProbe();
    return lu && pool != nullptr;
}

void* PeekLocalUser() { return gLocalUser; }
void* PeekDropPool() { return gDropPool; }

bool CollectProbe(ProbeSnapshot& out, float nearHalfW, float nearHalfH) {
    out = {};
    if (!EnsureBound()) return false;
    const DWORD now = GetTickCount();
    void* pool = ResolveDropPool(now);
    void* pet = FirstActivePet();
    out.hasPet = pet != nullptr;
    out.pet = pet;
    if (pet) {
        out.petSkill = ReadPetSkill(pet);
        out.petRc = ReadRect(pet, kOffPetRc);
        ReadPetPos(pet, out.petX, out.petY);
    }
    out.collisionRcPet = ReadCollisionRcPet();
    if (pool && pet) {
        out.nearCount =
            CountDropsNear(pool, out.petX, out.petY, nearHalfW, nearHalfH, &out.dropCount);
    } else if (pool) {
        CountDropsNear(pool, 0.f, 0.f, 1e9f, 1e9f, &out.dropCount);
    }
    out.ok = true;
    return true;
}

bool TryPetVacuum(float vacuumW, float vacuumH, const SkipIds* skipIds, VacuumResult& out) {
    out = {};
    if (!EnsureBound()) {
        out.why = "unbound";
        return false;
    }
    if (!(vacuumW > 1.f) || !(vacuumH > 1.f)) {
        out.why = "bad_box";
        return false;
    }

    gJob.vacuumW = vacuumW;
    gJob.vacuumH = vacuumH;
    gJob.skip = {};
    if (skipIds) gJob.skip = *skipIds;
    gJob.result = {};
    gJob.done = false;

    if (!x::runtime::main_thread::Ensure()) {
        out.why = "no_pump";
        return false;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&VacJobThunk, nullptr, kJobWaitMs)) {
        out.why = "timeout";
        return false;
    }
    out = gJob.result;
    return out.ok;
}

bool TryFootPickup(float halfW, float halfH, const SkipIds* skipIds, FootResult& out) {
    out = {};
    if (!EnsureBound()) {
        out.why = "unbound";
        return false;
    }
    if (!(halfW > 1.f) || !(halfH > 1.f)) {
        out.why = "bad_box";
        return false;
    }

    gFoot.halfW = halfW;
    gFoot.halfH = halfH;
    gFoot.skip = {};
    if (skipIds) gFoot.skip = *skipIds;
    gFoot.result = {};
    gFoot.done = false;

    if (!x::runtime::main_thread::Ensure()) {
        out.why = "no_pump";
        return false;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&FootJobThunk, nullptr, kJobWaitMs)) {
        out.why = "timeout";
        return false;
    }
    out = gFoot.result;
    return out.ok;
}

}  // namespace x::features::ports::drop
