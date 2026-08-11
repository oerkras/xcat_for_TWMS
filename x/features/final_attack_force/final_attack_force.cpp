// TWMS Classic — final_attack_force
//
// 根因（upload 0.1.87）：Prop→100 已生效，但 OnFuncKey 普攻路径 FinalAttack 列表常空，
// 掷骰走不到 → 需直接写 UserLocal.FinalAttack@+0x3A4。
//
// 根因（upload 0.1.93）：write-only 写了 SkillID 后 stuck（regHits=1 / wrote=0）。
// StartTick 误用 GetTickCount，而 TryDoingFinalAttack 用 GetUpdateTime 游戏钟做
// 有符号时间窗 → 每帧提前退出且不清理 SkillID。必须写游戏钟并在待发时刷新。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "final_attack_force.h"

#include "../ports/player_combat_port.h"
#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"
#include "xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace x::features::final_attack_force {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// SkillEntry.LevelDataList → List<SkillLevelData>；Prop 与 CMS SkillLevelData 对齐。
constexpr size_t kFbLevelDataList = 0x120;
constexpr size_t kFbProp = 0x84;
constexpr size_t kFbSkillType = 0x28;
constexpr int kSkillTypeFinalAttack = 3;
constexpr int kPropForce = 100;

// UserLocal.FinalAttack valuetype @ +0x3A4（restored dump；CMS 旧档曾为 +0x38C）
constexpr size_t kFbFinalAttack = 0x3A4;
constexpr size_t kFaLastSkill = 0x0;
constexpr size_t kFaSkillId = 0x4;
constexpr size_t kFaWeaponType = 0x8;
constexpr size_t kFaStartTick = 0xC;

constexpr int kBodyPartWeapon = 11;  // CMS BodyPart.Weapon
constexpr int kInvTiEquip = 1;
constexpr size_t kFbCdEquipped = 0x28;   // CharacterData.Equipped[]
constexpr size_t kFbCdEquipped2 = 0x30;  // CharacterData.Equipped2[]（现金）

// GetLevelData(int level) → SkillLevelData*
constexpr uint32_t kRvaGetLevelData = 0x1564A30;
// UserLocal.TryDoingFinalAttack — remount 2026-08-06
// 警告：0x1026050 / a42678b6… 是 TryDoingFallDown，勿再绑。
constexpr uint32_t kRvaTryDoingFinalAttack = 0x109DBA0;
// ItemInfo.GetWeaponType(int) — 与 Doing 内比较同源（edx=MethodInfo 可为 null）
constexpr uint32_t kRvaGetWeaponType = 0x1418B10;
// CharacterData.GetItem(nTI, nPos)
constexpr uint32_t kRvaCdGetItem = 0x12D9AF0;

constexpr char kHashGetLevelData[] =
    "f47191da3f08f5e8a7d4a64a286be8d40e43c4f3f66724aa962070342297d3e";
constexpr char kHashSkillEntry[] =
    "cf6d6169272f7c4a4dbb084cc7786a67fed9c03d7376babdcb5e5ecdde00eef";
constexpr char kHashSkillLevelData[] =
    "f6b2dee44681627d86594e8da90980812d5ffc411c2f9cb20b03555deaace96";
constexpr char kHashProp[] =
    "c91691ae992a129aa72666b399b168db898130e774088e51c3f65ff33e376e8";
constexpr char kHashLevelDataList[] =
    "aebdce14e3d6d5e71cf4c57d2ef33206875605936ef7f51deeb8fa3ad2db120";
constexpr char kHashFinalAttackField[] =
    "bf036898924d1d22c058f3aea8978fd90ad0214d60234d57dd243b750472c62";
constexpr char kHashCdEquipped[] =
    "b6730f554734a2d4bf2d0bfb383efce2305fd749cc5d2d00773f538566c6777";
constexpr char kHashCdEquipped2[] =
    "bc5fe18e2648ed8fff901dd9541262015a345b2ef8c0ec0e4fec41b1d051b54";
constexpr char kHashTryDoingFinalAttack[] =
    "ab75154d87bf59e1cdecfd53266213dc2d2636dcc792c140eb13298dfcb5fb2";
constexpr char kHashGetWeaponType[] =
    "f47003eeea3d63c00dae53d67da83598202c55d335a9e17fce3bd2cc9979241";
constexpr char kHashCdGetItem[] =
    "e5874bc52cb0f4b4f2e775ab6ef9d96f1196568394d2bff24e04a1d6ad2bc20";
constexpr char kHashItemInfo[] =
    "ab2ffe60bb8f8973293a005d0dc011dce59d8ace10e251ffc4bf48faecb43a9";
constexpr char kHashCharacterData[] =
    "d5453e03707efd1001d8348a46ee270f8117468d2f1504fd0dadd0cc7c10468";

// 经典版 Final Attack 技能表（狂战士剑/斧优先；同 prop 机制一并覆盖）
constexpr int kFinalAttackIds[] = {
    1100002, 1100003,  // Fighter 終極之劍 / 終極之斧
    1200002, 1200003,  // Page
    1300002, 1300003,  // Spearman
    3100001,           // Hunter
    3200001,           // Crossbowman
    11101002,          // Soul Master
    13101002,          // Wind Breaker
};

struct FaWeaponBind {
    int skillId;
    int wtA;
    int wtB;  // 0 = 仅 wtA
};

// WZ finalAttack 武器类型 ↔ FA 技能（经典版）
constexpr FaWeaponBind kFaWeaponBinds[] = {
    {1100002, 30, 40},   // 1h/2h sword
    {1100003, 31, 41},   // 1h/2h axe
    {1200002, 30, 40},
    {1200003, 32, 42},   // Page BW / mace 系
    {1300002, 43, 0},    // spear
    {1300003, 44, 0},    // polearm
    {3100001, 45, 0},    // bow
    {3200001, 46, 0},    // crossbow
    {11101002, 30, 40},
    {13101002, 45, 0},
};

constexpr DWORD kTickMsOn = 250;
constexpr DWORD kTickMsOff = 800;
constexpr DWORD kLogMs = 5000;
constexpr DWORD kForceJobWaitMs = 80;

using FnGetLevelData = void* (*)(void* self, int level, const void* methodInfo);
using FnTryDoingFinalAttack = void (*)(void* self, const void* methodInfo);
using FnGetWeaponType = int (*)(int itemId, const void* methodInfo);
using FnCdGetItem = void* (*)(void* self, int nTI, int nPos, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
};

std::atomic<bool> gDesired{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gPatchHits{0};
std::atomic<uint32_t> gRestoreHits{0};
std::atomic<uint32_t> gForceRegHits{0};

size_t gOffLevelDataList = kFbLevelDataList;
size_t gOffProp = kFbProp;
size_t gOffFinalAttack = kFbFinalAttack;
size_t gOffCdEquipped = kFbCdEquipped;
size_t gOffCdEquipped2 = kFbCdEquipped2;
bool gFieldTried = false;

FnGetLevelData gGetLevelData = nullptr;
MethodInfoHead* gMiGetLevelData = nullptr;
FnTryDoingFinalAttack gTryDoingFinalAttack = nullptr;
MethodInfoHead* gMiTryDoingFinalAttack = nullptr;
FnGetWeaponType gGetWeaponType = nullptr;
MethodInfoHead* gMiGetWeaponType = nullptr;
FnCdGetItem gCdGetItem = nullptr;
MethodInfoHead* gMiCdGetItem = nullptr;
void* gSkillEntryKlass = nullptr;
std::atomic<uint32_t> gDidHits{0};
std::atomic<uint32_t> gRefreshHits{0};

std::mutex gOrigMu;
// key = SkillLevelData* → original Prop（关开关时还原）
std::unordered_map<uintptr_t, int> gOrigProp;

size_t FieldOffsetByHash(void* klass, const char* nameHash) {
    if (!klass || !nameHash || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    for (void* k = klass; k;) {
        if (e.classGetFieldFromName && e.fieldGetOffset) {
            void* field = nullptr;
            __try {
                field = e.classGetFieldFromName(k, nameHash);
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
                if (off) return off;
            }
        }
        if (e.classGetFields && e.fieldGetName && e.fieldGetOffset) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* field = e.classGetFields(k, &iter);
                    if (!field) break;
                    const char* nm = nullptr;
                    __try {
                        nm = e.fieldGetName(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        nm = nullptr;
                    }
                    if (!nm || std::strcmp(nm, nameHash) != 0) continue;
                    size_t off = 0;
                    __try {
                        off = e.fieldGetOffset(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        off = 0;
                    }
                    if (off) return off;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return 0;
}

void EnsureFieldOffsets() {
    if (gFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gFieldTried = true;

    void* se = x::runtime::il2cpp::FindClass("", kHashSkillEntry);
    void* sld = x::runtime::il2cpp::FindClass("", kHashSkillLevelData);
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* cd = x::runtime::il2cpp::FindClass("", kHashCharacterData);
    gSkillEntryKlass = se;
    if (se) {
        const size_t off = FieldOffsetByHash(se, kHashLevelDataList);
        if (off) gOffLevelDataList = off;
    }
    if (sld) {
        const size_t off = FieldOffsetByHash(sld, kHashProp);
        if (off) gOffProp = off;
    }
    if (ul) {
        const size_t off = FieldOffsetByHash(ul, kHashFinalAttackField);
        if (off) gOffFinalAttack = off;
    }
    if (cd) {
        const size_t off = FieldOffsetByHash(cd, kHashCdEquipped);
        if (off) gOffCdEquipped = off;
        const size_t off2 = FieldOffsetByHash(cd, kHashCdEquipped2);
        if (off2) gOffCdEquipped2 = off2;
    }
    x::runtime::LogI("FaForce",
                     "offsets list=0x%zX prop=0x%zX fa=0x%zX eq=0x%zX eq2=0x%zX se=%p sld=%p ul=%p "
                     "cd=%p",
                     gOffLevelDataList, gOffProp, gOffFinalAttack, gOffCdEquipped, gOffCdEquipped2,
                     se, sld, ul, cd);
}

void EnsureMethodApis() {
    EnsureFieldOffsets();
    if (!gSkillEntryKlass)
        gSkillEntryKlass = x::runtime::il2cpp::FindClass("", kHashSkillEntry);

    // GetLevelData 依赖 SkillEntry；其余 API 独立，禁止 SkillEntry miss 时整段早退。
    if (!gGetLevelData && gSkillEntryKlass) {
        x::runtime::il2cpp_method::MethodShape shape{};
        shape.arity = 1;
        shape.ret = x::runtime::il2cpp_method::TypeKind::Ptr;
        shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
        auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            gSkillEntryKlass, kRvaGetLevelData, shape, "GetLevelData", kHashGetLevelData);
        if (mr.method) {
            gMiGetLevelData = reinterpret_cast<MethodInfoHead*>(mr.method);
            if (gMiGetLevelData && gMiGetLevelData->methodPointer)
                gGetLevelData = reinterpret_cast<FnGetLevelData>(gMiGetLevelData->methodPointer);
        }
    }
    if (!gGetLevelData)
        gGetLevelData = x::runtime::il2cpp::AtRva<FnGetLevelData>(kRvaGetLevelData);

    if (!gTryDoingFinalAttack) {
        void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
        x::runtime::il2cpp_method::MethodShape shape{};
        shape.arity = 0;
        shape.ret = x::runtime::il2cpp_method::TypeKind::Void;
        auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            ul, kRvaTryDoingFinalAttack, shape, "TryDoingFinalAttack", kHashTryDoingFinalAttack);
        if (mr.method) {
            gMiTryDoingFinalAttack = reinterpret_cast<MethodInfoHead*>(mr.method);
            if (gMiTryDoingFinalAttack && gMiTryDoingFinalAttack->methodPointer)
                gTryDoingFinalAttack =
                    reinterpret_cast<FnTryDoingFinalAttack>(gMiTryDoingFinalAttack->methodPointer);
        }
        if (!gTryDoingFinalAttack)
            gTryDoingFinalAttack =
                x::runtime::il2cpp::AtRva<FnTryDoingFinalAttack>(kRvaTryDoingFinalAttack);
    }

    if (!gGetWeaponType) {
        void* ii = x::runtime::il2cpp::FindClass("", kHashItemInfo);
        x::runtime::il2cpp_method::MethodShape shape{};
        shape.arity = 1;
        shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
        shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
        auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            ii, kRvaGetWeaponType, shape, "GetWeaponType", kHashGetWeaponType);
        if (mr.method) {
            gMiGetWeaponType = reinterpret_cast<MethodInfoHead*>(mr.method);
            if (gMiGetWeaponType && gMiGetWeaponType->methodPointer)
                gGetWeaponType = reinterpret_cast<FnGetWeaponType>(gMiGetWeaponType->methodPointer);
        }
        if (!gGetWeaponType)
            gGetWeaponType = x::runtime::il2cpp::AtRva<FnGetWeaponType>(kRvaGetWeaponType);
    }

    if (!gCdGetItem) {
        void* cd = x::runtime::il2cpp::FindClass("", kHashCharacterData);
        x::runtime::il2cpp_method::MethodShape shape{};
        shape.arity = 2;
        shape.ret = x::runtime::il2cpp_method::TypeKind::Ptr;
        shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
        shape.param[1] = x::runtime::il2cpp_method::TypeKind::I32;
        auto mr = x::runtime::il2cpp_method::FindMethodResolved(cd, kRvaCdGetItem, shape, "GetItem",
                                                                kHashCdGetItem);
        if (mr.method) {
            gMiCdGetItem = reinterpret_cast<MethodInfoHead*>(mr.method);
            if (gMiCdGetItem && gMiCdGetItem->methodPointer)
                gCdGetItem = reinterpret_cast<FnCdGetItem>(gMiCdGetItem->methodPointer);
        }
        if (!gCdGetItem) gCdGetItem = x::runtime::il2cpp::AtRva<FnCdGetItem>(kRvaCdGetItem);
    }
}

void EnsureGetLevelData() { EnsureMethodApis(); }

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32(void* obj, size_t off, int v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool PatchProp(void* levelData, bool forceOn) {
    if (!LooksLikeHeapPtr(levelData)) return false;
    const int cur = ReadI32(levelData, gOffProp);
    const uintptr_t key = reinterpret_cast<uintptr_t>(levelData);
    if (forceOn) {
        if (cur == kPropForce) return false;
        {
            std::lock_guard<std::mutex> lock(gOrigMu);
            if (gOrigProp.find(key) == gOrigProp.end()) gOrigProp[key] = cur;
        }
        WriteI32(levelData, gOffProp, kPropForce);
        gPatchHits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    int orig = cur;
    {
        std::lock_guard<std::mutex> lock(gOrigMu);
        auto it = gOrigProp.find(key);
        if (it == gOrigProp.end()) return false;
        orig = it->second;
        gOrigProp.erase(it);
    }
    if (cur != orig) {
        WriteI32(levelData, gOffProp, orig);
        gRestoreHits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

int VisitListProps(void* list, bool forceOn) {
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const size_t offItems = x::runtime::il2cpp_container::OffListItems();
    const size_t offSize = x::runtime::il2cpp_container::OffListSize();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    void* items = nullptr;
    int size = 0;
    __try {
        items = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(list) + offItems);
        size = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(list) + offSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!LooksLikeHeapPtr(items) || size <= 0 || size > 64) return 0;
    int n = 0;
    for (int i = 0; i < size; ++i) {
        void* elem = nullptr;
        __try {
            elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + offData +
                                             static_cast<size_t>(i) * sizeof(void*));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (PatchProp(elem, forceOn)) ++n;
    }
    return n;
}

int PatchSkillEntry(void* entry, int level, bool forceOn) {
    if (!LooksLikeHeapPtr(entry)) return 0;
    int n = 0;
    void* list = nullptr;
    __try {
        list = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + gOffLevelDataList);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        list = nullptr;
    }
    n += VisitListProps(list, forceOn);

    EnsureGetLevelData();
    if (gGetLevelData && level > 0) {
        void* ld = nullptr;
        __try {
            ld = gGetLevelData(entry, level, gMiGetLevelData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ld = nullptr;
        }
        if (PatchProp(ld, forceOn)) ++n;
    }
    return n;
}

bool IsKnownFaId(int id) {
    for (int fa : kFinalAttackIds) {
        if (fa == id) return true;
    }
    return false;
}

bool WeaponMatchesFa(int skillId, int weaponType) {
    for (const auto& b : kFaWeaponBinds) {
        if (b.skillId != skillId) continue;
        if (weaponType == b.wtA) return true;
        if (b.wtB && weaponType == b.wtB) return true;
    }
    return false;
}

int WeaponTypeFromItemId(int itemId) {
    if (itemId < 1000000) return 0;
    // 普通武器 13xxxxx–14xxxxx → type 30–49；现金 17xxxxx 走 GetWeaponType API。
    const int cat = itemId / 10000;
    const int t = cat % 100;
    if (t >= 30 && t <= 49) return t;
    return 0;
}

int CallGetWeaponType(int itemId) {
    if (itemId <= 0) return 0;
    EnsureMethodApis();
    auto fn = gGetWeaponType;
    if (!fn) return 0;
    int wt = 0;
    __try {
        wt = fn(itemId, gMiGetWeaponType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wt = 0;
    }
    if (wt >= 30 && wt <= 49) return wt;
    return 0;
}

int ResolveWeaponType(int itemId) {
    const int fromId = WeaponTypeFromItemId(itemId);
    if (fromId) return fromId;
    return CallGetWeaponType(itemId);
}

int SlotItemId(void* slot) {
    if (!LooksLikeHeapPtr(slot)) return 0;
    const size_t offId = x::ui::player::OffSlotItemId();
    if (!offId) return 0;
    return ReadI32(slot, offId);
}

void* ArrayAtPtr(void* arr, int index) {
    if (!LooksLikeHeapPtr(arr) || index < 0) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    int len = 0;
    __try {
        len = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(arr) + offLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (index >= len) return nullptr;
    void* elem = nullptr;
    __try {
        elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                         static_cast<size_t>(index) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(elem) ? elem : nullptr;
}

void* ListAt(void* list, int index) {
    if (!LooksLikeHeapPtr(list) || index < 0) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const size_t offItems = x::runtime::il2cpp_container::OffListItems();
    const size_t offSize = x::runtime::il2cpp_container::OffListSize();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    void* items = nullptr;
    int size = 0;
    __try {
        items = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(list) + offItems);
        size = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(list) + offSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!LooksLikeHeapPtr(items) || index >= size) return nullptr;
    void* elem = nullptr;
    __try {
        elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + offData +
                                         static_cast<size_t>(index) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(elem) ? elem : nullptr;
}

int TryWeaponTypeFromSlot(void* slot, int* outItemId) {
    const int id = SlotItemId(slot);
    if (id <= 0) return 0;
    const int wt = ResolveWeaponType(id);
    if (wt && outItemId) *outItemId = id;
    return wt;
}

void* GetItemEquipSlot(int pos) {
    void* cd = x::ui::player::LocalCharacterData();
    if (!LooksLikeHeapPtr(cd)) return nullptr;
    EnsureMethodApis();
    auto fn = gCdGetItem;
    if (!fn) return nullptr;
    void* slot = nullptr;
    __try {
        slot = fn(cd, kInvTiEquip, pos, gMiCdGetItem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    return LooksLikeHeapPtr(slot) ? slot : nullptr;
}

// 优先 CharacterData.Equipped/Equipped2[Weapon]，再 GetItem(±11)，最后扫背包 List。
int ReadEquippedWeaponType(int* outItemId, int* outEquipSize) {
    if (outItemId) *outItemId = 0;
    if (outEquipSize) *outEquipSize = -1;

    void* cd = x::ui::player::LocalCharacterData();
    if (LooksLikeHeapPtr(cd)) {
        EnsureFieldOffsets();
        void* eq = ReadPtr(cd, gOffCdEquipped);
        void* eq2 = ReadPtr(cd, gOffCdEquipped2);
        for (void* arr : {eq, eq2}) {
            if (!LooksLikeHeapPtr(arr)) continue;
            const int wt = TryWeaponTypeFromSlot(ArrayAtPtr(arr, kBodyPartWeapon), outItemId);
            if (wt) {
                if (outEquipSize) {
                    x::runtime::il2cpp_container::Ensure();
                    *outEquipSize =
                        ReadI32(arr, x::runtime::il2cpp_container::OffArrayMaxLength());
                }
                return wt;
            }
        }
        for (int pos : {-kBodyPartWeapon, kBodyPartWeapon, -(kBodyPartWeapon + 1),
                        kBodyPartWeapon + 1}) {
            const int wt = TryWeaponTypeFromSlot(GetItemEquipSlot(pos), outItemId);
            if (wt) return wt;
        }
    }

    void* list = x::ui::player::GetItemSlotList(kInvTiEquip);
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const int size = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    if (outEquipSize) *outEquipSize = size;

    for (int idx : {kBodyPartWeapon, kBodyPartWeapon + 1}) {
        const int wt = TryWeaponTypeFromSlot(ListAt(list, idx), outItemId);
        if (wt) return wt;
    }
    const int n = size > 96 ? 96 : size;
    for (int i = 0; i < n; ++i) {
        const int wt = TryWeaponTypeFromSlot(ListAt(list, i), outItemId);
        if (wt) return wt;
    }
    return 0;
}

int PickFaSkillForWeapon(int weaponType, int job) {
    if (weaponType <= 0) return 0;
    int fallback = 0;
    for (int id : kFinalAttackIds) {
        if (!WeaponMatchesFa(id, weaponType)) continue;
        const int lv = x::features::ports::skill::GetSkillLevel(id);
        if (lv <= 0) continue;
        if (!fallback) fallback = id;
        if (job > 0) {
            const int jobStem = job / 10;
            const int skillStem = id / 10000;
            if (jobStem == skillStem || job / 100 == id / 1000000) return id;
        }
    }
    return fallback;
}

// 读不到武器时：用已学 FA + 其 WZ 绑定武器类型（剑优先）。
bool PickLearnedFaFallback(int job, int* outSkill, int* outWt) {
    if (!outSkill || !outWt) return false;
    *outSkill = 0;
    *outWt = 0;
    int bestSkill = 0;
    int bestWt = 0;
    int bestScore = -1;
    for (const auto& b : kFaWeaponBinds) {
        const int lv = x::features::ports::skill::GetSkillLevel(b.skillId);
        if (lv <= 0) continue;
        int score = 1;
        if (job > 0) {
            const int jobStem = job / 10;
            const int skillStem = b.skillId / 10000;
            if (jobStem == skillStem) score += 10;
            if (job / 100 == b.skillId / 1000000) score += 5;
        }
        // 狂战士剑系稍优先（常见）
        if (b.skillId == 1100002) score += 2;
        if (score > bestScore) {
            bestScore = score;
            bestSkill = b.skillId;
            bestWt = b.wtA;
        }
    }
    if (bestSkill <= 0) return false;
    *outSkill = bestSkill;
    *outWt = bestWt;
    return true;
}

struct ForceJob {
    int wrote = 0;      // 1=新写 2=刷新 StartTick
    int did = 0;        // 1=已调 TryDoingFinalAttack
    int skillId = 0;
    int weaponType = 0;
    int itemId = 0;
    int equipSize = -1;
    int usedFallback = 0;
    int gameTick = 0;
    int curSkill = 0;
    const char* err = nullptr;
};

void CallTryDoingFinalAttack(void* lu) {
    if (!LooksLikeHeapPtr(lu)) return;
    EnsureMethodApis();
    auto fn = gTryDoingFinalAttack;
    if (!fn) return;
    __try {
        fn(lu, gMiTryDoingFinalAttack);
        gDidHits.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 写/刷新 FinalAttack 待发（游戏钟 StartTick）并主线程触发 TryDoingFinalAttack。
void ForceFaOnMain(void* user) {
    auto* job = reinterpret_cast<ForceJob*>(user);
    if (!job) return;
    job->wrote = 0;
    job->did = 0;
    job->itemId = 0;
    job->equipSize = -1;
    job->usedFallback = 0;
    job->gameTick = 0;
    job->curSkill = 0;
    job->err = nullptr;

    EnsureFieldOffsets();

    void* lu = nullptr;
    if (!x::features::ports::player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
        lu = x::ui::player::LocalMyUser();
    }
    if (!LooksLikeHeapPtr(lu)) {
        job->err = "no LocalUser";
        return;
    }

    int itemId = 0;
    int equipSize = -1;
    int weaponType = ReadEquippedWeaponType(&itemId, &equipSize);
    job->itemId = itemId;
    job->equipSize = equipSize;
    job->weaponType = weaponType;
    x::ui::player::Vitals vit{};
    x::ui::player::Read(vit);
    int faSkill = PickFaSkillForWeapon(weaponType, vit.job);
    if (faSkill <= 0) {
        int fbSkill = 0, fbWt = 0;
        if (PickLearnedFaFallback(vit.job, &fbSkill, &fbWt)) {
            faSkill = fbSkill;
            weaponType = fbWt;
            job->weaponType = weaponType;
            job->usedFallback = 1;
        }
    }
    job->skillId = faSkill;
    if (faSkill <= 0) {
        job->err = weaponType ? "no learned FA for weapon" : "no weapon";
        return;
    }

    const int tick = x::features::ports::skill::GetGameUpdateTimeMs();
    job->gameTick = tick;
    if (tick <= 0) {
        job->err = "no game tick";
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(lu) + gOffFinalAttack;
    int curSkill = 0;
    int curWt = 0;
    __try {
        curSkill = *reinterpret_cast<int*>(base + kFaSkillId);
        curWt = *reinterpret_cast<int*>(base + kFaWeaponType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->err = "fa read fault";
        return;
    }
    job->curSkill = curSkill;

    // 已有待发：若同技能则刷新 StartTick（修 0.1.93 时钟卡死）；否则覆盖。
    const bool samePending = (curSkill == faSkill && curWt == weaponType);
    __try {
        if (!samePending) {
            *reinterpret_cast<int*>(base + kFaLastSkill) = 0;
            *reinterpret_cast<int*>(base + kFaSkillId) = faSkill;
            *reinterpret_cast<int*>(base + kFaWeaponType) = weaponType;
            *reinterpret_cast<int*>(base + kFaStartTick) = tick;
            job->wrote = 1;
            gForceRegHits.fetch_add(1, std::memory_order_relaxed);
        } else {
            *reinterpret_cast<int*>(base + kFaStartTick) = tick;
            job->wrote = 2;
            gRefreshHits.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->err = "fa write fault";
        return;
    }

    CallTryDoingFinalAttack(lu);
    job->did = 1;
}

struct PropJob {
    bool forceOn = false;
    int changed = 0;
};

// 仅主泵：GetSkill* 托管 + Prop 内存补丁。worker 禁止直调（BIN 00:28:36 GA+0x3a0bde TLS AV）。
void TickPropOnMain(void* p) {
    auto* job = static_cast<PropJob*>(p);
    if (!job) return;
    job->changed = 0;
    if (!x::features::ports::world::IsPlayReady()) return;
    if (!x::features::ports::skill::EnsureBound()) return;
    EnsureFieldOffsets();

    for (int id : kFinalAttackIds) {
        const int lv = x::features::ports::skill::GetSkillLevel(id);
        if (job->forceOn && lv <= 0) continue;
        void* entry = x::features::ports::skill::GetSkillEntry(id);
        if (!entry) continue;
        const int st = ReadI32(entry, kFbSkillType);
        if (st != 0 && st != kSkillTypeFinalAttack && !IsKnownFaId(id)) continue;
        job->changed += PatchSkillEntry(entry, lv > 0 ? lv : 1, job->forceOn);
    }
}

int TickProp(bool forceOn) {
    if (!x::features::ports::world::IsPlayReady()) return 0;
    if (!x::features::ports::skill::EnsureBound()) return 0;

    if (x::runtime::main_thread::IsOnPumpThread()) {
        PropJob job{};
        job.forceOn = forceOn;
        TickPropOnMain(&job);
        return job.changed;
    }
    if (!x::runtime::main_thread::Ensure()) return 0;
    if (x::runtime::main_thread::IsCongested()) return 0;

    PropJob job{};
    job.forceOn = forceOn;
    // Normal：不抢 ForceFa High；整批一次避免 N×GetSkill 往返。
    if (!x::runtime::main_thread::InvokeAndWait(&TickPropOnMain, &job, kForceJobWaitMs,
                                               x::runtime::main_thread::JobPrio::Normal)) {
        return 0;
    }
    return job.changed;
}

void TickForce() {
    using Code = x::runtime::anchor_lamps::AnchorLampCode;
    if (!x::features::ports::world::IsPlayReady()) {
        x::runtime::anchor_lamps::Set("FaForce", Code::Unknown, "wait");
        return;
    }
    if (!x::features::ports::skill::EnsureBound()) {
        x::runtime::anchor_lamps::Set("FaForce", Code::Miss, "skill unbound");
        return;
    }
    if (!x::runtime::main_thread::Ensure()) {
        x::runtime::anchor_lamps::Set("FaForce", Code::Miss, "no pump");
        return;
    }
    if (x::runtime::main_thread::IsCongested()) {
        x::runtime::anchor_lamps::Set("FaForce", Code::Degraded, "busy");
        return;
    }

    ForceJob job{};
    if (!x::runtime::main_thread::InvokeAndWait(&ForceFaOnMain, &job, kForceJobWaitMs,
                                               x::runtime::main_thread::JobPrio::High)) {
        x::runtime::anchor_lamps::Set("FaForce", Code::Degraded, "invoke");
        return;
    }

    char d[xcat::kAnchorLampDetailLen]{};
    if (job.err && job.err[0]) {
        snprintf(d, sizeof(d), "%s", job.err);
        x::runtime::anchor_lamps::Set("FaForce", Code::Miss, d);
    } else if (job.did && job.wrote) {
        snprintf(d, sizeof(d), "fa=%d w=%d", job.skillId, job.wrote);
        x::runtime::anchor_lamps::Set("FaForce", Code::Ok, d);
    } else if (job.did || job.wrote) {
        snprintf(d, sizeof(d), "partial did=%d w=%d", job.did, job.wrote);
        x::runtime::anchor_lamps::Set("FaForce", Code::Degraded, d);
    } else {
        x::runtime::anchor_lamps::Set("FaForce", Code::Unknown, "pending");
    }

    static DWORD sLastLog = 0;
    const DWORD now = GetTickCount();
    if (now - sLastLog >= kLogMs) {
        sLastLog = now;
        x::runtime::LogI(
            "FaForce",
            "force fa=%d wt=%d item=%d eqSz=%d fb=%d tick=%d cur=%d wrote=%d did=%d "
            "regHits=%u refresh=%u didHits=%u err=%s",
            job.skillId, job.weaponType, job.itemId, job.equipSize, job.usedFallback, job.gameTick,
            job.curSkill, job.wrote, job.did, gForceRegHits.load(std::memory_order_relaxed),
            gRefreshHits.load(std::memory_order_relaxed), gDidHits.load(std::memory_order_relaxed),
            job.err ? job.err : "-");
    }
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("FaForce", "worker start desired=%d", gDesired.load() ? 1 : 0);
    DWORD lastLog = 0;
    while (!gStop.load(std::memory_order_relaxed)) {
        const bool on = gDesired.load(std::memory_order_relaxed);
        const int changed = TickProp(on);
        if (on) {
            TickForce();
        } else {
            x::runtime::anchor_lamps::Set("FaForce",
                                         x::runtime::anchor_lamps::AnchorLampCode::Unknown, "off");
        }
        const DWORD now = GetTickCount();
        if (changed > 0 && now - lastLog >= kLogMs) {
            lastLog = now;
            x::runtime::LogI("FaForce", "%s rows=%d patchHits=%u restoreHits=%u",
                             on ? "prop→100" : "prop restore", changed,
                             gPatchHits.load(std::memory_order_relaxed),
                             gRestoreHits.load(std::memory_order_relaxed));
        }
        Sleep(on ? kTickMsOn : kTickMsOff);
    }
    TickProp(false);
    x::runtime::anchor_lamps::Set("FaForce", x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                 "off");
    x::runtime::LogI("FaForce", "worker stop");
    return 0;
}

bool EnvForceOn() {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_FINAL_ATTACK_FORCE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

}  // namespace

int QueryEquippedWeaponType() {
    return ReadEquippedWeaponType(nullptr, nullptr);
}

bool EquippedWeaponIsBowFamily() {
    const int wt = QueryEquippedWeaponType();
    // MapleWeaponType：45=弓、46=弩（与 FA 表 / WeaponTypeFromItemId 一致）。
    return wt == 45 || wt == 46;
}

void Init() {
    if (!xcat::kFinalAttackForceUserEnabled) {
        gDesired.store(false, std::memory_order_relaxed);
        x::runtime::LogI("FaForce", "user gate off — skipped (retired; keep code)");
        return;
    }
    if (EnvForceOn()) {
        gDesired.store(true, std::memory_order_relaxed);
        x::runtime::LogI("FaForce", "env XCAT_FINAL_ATTACK_FORCE → on");
    }
}

void Shutdown() {
    StopWorker();
    std::lock_guard<std::mutex> lock(gOrigMu);
    gOrigProp.clear();
}

void StartWorker() {
    if (!xcat::kFinalAttackForceUserEnabled) {
        gDesired.store(false, std::memory_order_relaxed);
        x::runtime::anchor_lamps::Set("FaForce",
                                     x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                     "disabled");
        return;
    }
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_relaxed);
    HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (h) gWorker.store(h, std::memory_order_release);
}

void StopWorker() {
    gStop.store(true, std::memory_order_relaxed);
    HANDLE h = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (h) {
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
}

void SetDesired(bool on) {
    if (!xcat::kFinalAttackForceUserEnabled) on = false;
    gDesired.store(on, std::memory_order_relaxed);
}

bool IsDesired() { return gDesired.load(std::memory_order_relaxed); }

}  // namespace x::features::final_attack_force
