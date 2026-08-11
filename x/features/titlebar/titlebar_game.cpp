#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "titlebar_game.h"

#include "../ports/world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../ui/player_vitals.h"
#include "xcat_item_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace x::features::titlebar::game {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// HP/MP 真源在 x::ui::player（WM→CS）；LocalUser → il2cpp_shape::ResolveUserLocalKlass
// ItemDataManager：08-06 remount（TypeDef 2027；dataTable@+0x18 / bundleMap@+0x38）
constexpr char kItemDataManagerClass[] =
    "de1c111ab825716f33574ba3f3978092564030384a1f03c2ea2676e99ea71c2";
constexpr char kItemDataClass[] =
    "f621ae15dbc3cb71335f3e8c226da19a11ca3bd1bbfa67913aecd861a8875e4";
constexpr char kItemBundleClass[] =
    "c2f3c9349695dedb3b3a45795c1e60bb7a5c97ddda6b2736643de8ce66b4db7";
// ItemData.info 实际类型（restored Info · TypeDef 2031），不是旧 ItemInfo 名
constexpr char kItemInfoClass[] =
    "b6c2d6b5bde207d472a7e8ea0798b51fda0d25068579cfac9a8a467feded236";

// IDM / ItemData / Bundle / Info 价位：hash → field_get_offset（dump fallback）
constexpr char kHashIdmDataTable[] =
    "a96e655147f3659f9a24e3836052e342ab9db997dd3647c5db2d15ac376972a";
constexpr char kHashIdmBundleMap[] =
    "bf0d6d387c3598d051d5089e0932cea80521839a69083ab04b283b5acddb3e0";
constexpr char kHashBundleSellPrice[] =
    "a10749449fb7a2c19504ec7092ab2dc668cdfc31f4b436fdffa6c48ebe14392";
constexpr char kHashItemDataInfo[] =
    "d85749d6426c0e4657ff23f4fc1ca8a12961891ab0f4aec5b7824271ee1a5a5";
constexpr char kHashInfoPrice[] =
    "b96bdf76a975afaa84ebc6c1ca0090fab5acc5eff8b7627ce67d13a7b8f7c73";
constexpr char kHashInfoNotSale[] =
    "d11cfcde5d2e4b572c68d7357431e7316f0f2362442b8d5a017703a8e006e77";

constexpr size_t kFbIdmDataTable = 0x18;
constexpr size_t kFbIdmBundleMap = 0x38;
constexpr size_t kFbBundleSellPrice = 0x38;
constexpr size_t kFbItemDataInfo = 0x18;
constexpr size_t kFbInfoPrice = 0x54;
constexpr size_t kFbInfoNotSale = 0x68;
size_t gOffIdmDataTable = kFbIdmDataTable;
size_t gOffIdmBundleMap = kFbIdmBundleMap;
size_t gOffBundleSellPrice = kFbBundleSellPrice;
size_t gOffItemDataInfo = kFbItemDataInfo;
size_t gOffInfoPrice = kFbInfoPrice;
size_t gOffInfoNotSale = kFbInfoNotSale;
#define kOffIdmDataTable (gOffIdmDataTable)
#define kOffIdmBundleMap (gOffIdmBundleMap)
#define kOffBundleSellPrice (gOffBundleSellPrice)
#define kOffItemDataInfo (gOffItemDataInfo)
#define kOffInfoPrice (gOffInfoPrice)
#define kOffInfoNotSale (gOffInfoNotSale)
bool gPriceFieldTried = false;

// WM.MyUser / CD.ItemSlots / Slot.ItemId|nNumber → x::ui::player（hash SSOT）
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffDictBuckets (x::runtime::il2cpp_container::OffDictBuckets())
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictFreeCount (x::runtime::il2cpp_container::OffDictFreeCount())
#define kEntrySize (x::runtime::il2cpp_container::DictEntryStrideIntPtr())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())
#define kOffEntryNext (x::runtime::il2cpp_container::OffDictEntryNext())
#define kOffEntryKey (x::runtime::il2cpp_container::OffDictEntryKey())
#define kOffEntryValue (x::runtime::il2cpp_container::OffDictEntryValuePtr())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())

constexpr int kInvTiConsume = 2;
constexpr int kInvTiInstall = 3;
constexpr int kInvTiEtc = 4;

using FnFindAll = void* (*)(void*, void*);
using FnCompGo = void* (*)(void*, void*);
using FnObjName = void* (*)(void*, void*);

FnFindAll gFindAll = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gItemDataManager = nullptr;
std::unordered_map<int, unsigned long long> gLootBaseline;
bool gHaveLootBaseline = false;
std::unordered_map<int, int> gPriceCache;

uint16_t ReadU16(void* object, size_t offset) {
    if (!object) return 0;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t ReadI32(void* object, size_t offset) {
    if (!object) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int16_t ReadI16(void* object, size_t offset) {
    if (!object) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t ReadU8(void* object, size_t offset) {
    if (!object) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int64_t ReadI64(void* object, size_t offset) {
    if (!object) return 0;
    __try {
        return *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool PlausiblePriceOff(size_t off) { return off >= 0x10 && off < 0x400; }

bool PriceFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
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
            if (PlausiblePriceOff(off)) {
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

void EnsurePriceFieldOff() {
    if (gPriceFieldTried) return;
    // BIN 11:56 LOADING：Titlebar worker 上 FindClass → GC Fatal（紧挨 price fields 日志）。
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { EnsurePriceFieldOff(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return;
    }
    if (!x::runtime::il2cpp::Ensure()) return;
    gPriceFieldTried = true;
    void* idm = x::runtime::il2cpp::FindClass("", kItemDataManagerClass);
    void* data = x::runtime::il2cpp::FindClass("", kItemDataClass);
    void* bundle = x::runtime::il2cpp::FindClass("", kItemBundleClass);
    void* info = x::runtime::il2cpp::FindClass("", kItemInfoClass);
    int hits = 0;
    if (PriceFieldOffHit(idm, kHashIdmDataTable, kFbIdmDataTable, &gOffIdmDataTable)) ++hits;
    if (PriceFieldOffHit(idm, kHashIdmBundleMap, kFbIdmBundleMap, &gOffIdmBundleMap)) ++hits;
    if (PriceFieldOffHit(bundle, kHashBundleSellPrice, kFbBundleSellPrice, &gOffBundleSellPrice))
        ++hits;
    if (PriceFieldOffHit(data, kHashItemDataInfo, kFbItemDataInfo, &gOffItemDataInfo)) ++hits;
    if (PriceFieldOffHit(info, kHashInfoPrice, kFbInfoPrice, &gOffInfoPrice)) ++hits;
    if (PriceFieldOffHit(info, kHashInfoNotSale, kFbInfoNotSale, &gOffInfoNotSale)) ++hits;
    x::runtime::LogI("Titlebar",
                     "price fields path=%s hits=%d/6 dt=0x%zX bm=0x%zX sell=0x%zX info=0x%zX "
                     "price=0x%zX ns=0x%zX",
                     hits == 6 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                     gOffIdmDataTable, gOffIdmBundleMap, gOffBundleSellPrice, gOffItemDataInfo,
                     gOffInfoPrice, gOffInfoNotSale);
}

int ArrayI32At(void* array, uintptr_t index) {
    if (!array) return -1;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(array) + kOffArrData +
                                       index * sizeof(int));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ReadStringUtf8(void* stringObject, char* out, size_t capacity) {
    if (!stringObject || !out || capacity < 2) return false;
    __try {
        const int32_t length = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(stringObject) + 0x10);
        if (length <= 0 || length > 64) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<uint8_t*>(stringObject) + 0x14);
        const int written = WideCharToMultiByte(CP_UTF8, 0, chars, length, out,
                                                static_cast<int>(capacity) - 1, nullptr, nullptr);
        if (written <= 0) return false;
        out[written] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadStringAscii(void* stringObject, char* out, size_t capacity) {
    if (!stringObject || !out || capacity < 2) return false;
    out[0] = 0;
    __try {
        const int32_t length = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(stringObject) + 0x10);
        if (length <= 0 || length > 96) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<uint8_t*>(stringObject) + 0x14);
        size_t written = 0;
        for (int i = 0; i < length && written + 1 < capacity; ++i) {
            const wchar_t character = chars[i];
            out[written++] = character >= 32 && character < 127 ? static_cast<char>(character) : '?';
        }
        out[written] = 0;
        return written > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetGoName(void* component, char* out, size_t capacity) {
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!component || !gCompGo || !gObjName) return false;
    __try {
        void* go = gCompGo(component, nullptr);
        return go && ReadStringAscii(gObjName(go, nullptr), out, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* DictGetIntPtr(void* dictionary, int key) {
    if (!LooksLikeHeapPtr(dictionary)) return nullptr;
    void* entries = ReadPtr(dictionary, kOffDictEntries);
    const int count = ReadI32(dictionary, kOffDictCount);
    const int freeCount = ReadI32(dictionary, kOffDictFreeCount);
    const uintptr_t entryCount = ArrayLen(entries);
    if (!LooksLikeHeapPtr(entries) || count <= 0 || count > 200000 || entryCount == 0 ||
        entryCount > 400000) {
        return nullptr;
    }

    const int hashCode = key & 0x7fffffff;
    void* buckets = ReadPtr(dictionary, kOffDictBuckets);
    if (LooksLikeHeapPtr(buckets)) {
        const uintptr_t bucketCount = ArrayLen(buckets);
        if (bucketCount > 0 && bucketCount <= 400000) {
            int index = ArrayI32At(buckets, static_cast<uintptr_t>(hashCode) % bucketCount);
            const int limit = count > freeCount ? count - freeCount + 8 : count + 8;
            for (int guard = 0; index >= 0 && guard < limit &&
                                static_cast<uintptr_t>(index) < entryCount; ++guard) {
                uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, index, kEntrySize);
                if (!entry) break;
                if (ReadI32(entry, kOffEntryHash) == hashCode &&
                    ReadI32(entry, kOffEntryKey) == key) {
                    void* value = ReadPtr(entry, kOffEntryValue);
                    return LooksLikeHeapPtr(value) ? value : nullptr;
                }
                index = ReadI32(entry, kOffEntryNext);
            }
            return nullptr;
        }
    }
    for (uintptr_t index = 0; index < entryCount; ++index) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(
            entries, static_cast<int>(index), kEntrySize);
        if (!entry) continue;
        if (ReadI32(entry, kOffEntryHash) < 0 || ReadI32(entry, kOffEntryKey) != key) continue;
        void* value = ReadPtr(entry, kOffEntryValue);
        return LooksLikeHeapPtr(value) ? value : nullptr;
    }
    return nullptr;
}

bool SnapshotInventory(std::unordered_map<int, unsigned long long>& out) {
    out.clear();
    const size_t offItemId = x::ui::player::OffSlotItemId();
    const size_t offBundleNum = x::ui::player::OffSlotBundleNumber();
    bool any = false;
    for (const int type : {kInvTiConsume, kInvTiInstall, kInvTiEtc}) {
        void* list = x::ui::player::GetItemSlotList(type);
        void* items = ReadPtr(list, kOffListItems);
        const int size = list ? ReadI32(list, kOffListSize) : 0;
        if (!LooksLikeHeapPtr(list) || !LooksLikeHeapPtr(items) || size <= 0 || size > 512) continue;
        any = true;
        const int count = (std::min)(size, static_cast<int>(ArrayLen(items)));
        for (int index = 0; index < count; ++index) {
            void* slot = ArrayAt(items, static_cast<uintptr_t>(index));
            const int itemId = ReadI32(slot, offItemId);
            if (!LooksLikeHeapPtr(slot) || itemId <= 0) continue;
            // TI 2/3/4 槽位运行时多为 ItemSlotBundle；数量在子类 nNumber ushort。
            unsigned long long quantity = ReadU16(slot, offBundleNum);
            out[itemId] += quantity ? quantity : 1;
        }
    }
    return any;
}

int LookupOfflineSellPrice(int itemId) {
    char code[32]{};
    snprintf(code, sizeof(code), "%d", itemId);
    const xcat::ItemCatalogPack& pack = xcat::GetSharedItemCatalog(x::runtime::GetBinDir());
    return xcat::ItemCatalogLookupSellPrice(pack, code);
}

int LookupSellPrice(int itemId) {
    if (itemId <= 0) return 0;
    EnsurePriceFieldOff();
    const auto cached = gPriceCache.find(itemId);
    if (cached != gPriceCache.end()) return cached->second > 0 ? cached->second : 0;
    int price = 0;
    if (TryResolveItemDataManager() && LooksLikeHeapPtr(gItemDataManager)) {
        if (void* bundle = DictGetIntPtr(ReadPtr(gItemDataManager, kOffIdmBundleMap), itemId)) {
            price = ReadI32(bundle, kOffBundleSellPrice);
        }
        if (price <= 0) {
            if (void* data = DictGetIntPtr(ReadPtr(gItemDataManager, kOffIdmDataTable), itemId)) {
                if (void* info = ReadPtr(data, kOffItemDataInfo);
                    LooksLikeHeapPtr(info) && ReadI32(info, kOffInfoNotSale) == 0) {
                    price = ReadI32(info, kOffInfoPrice);
                }
            }
        }
    }
    if (price <= 0) price = LookupOfflineSellPrice(itemId);
    gPriceCache[itemId] = price > 0 ? price : -1;
    return price > 0 ? price : 0;
}

}  // namespace

bool BindApis() {
    if (!x::runtime::il2cpp::Ensure()) {
        x::runtime::LogW("Titlebar", "BindApis: no GameAssembly");
        return false;
    }
    const auto& exports = x::runtime::il2cpp::Get();
    gFindAll = exports.findAll;
    gCompGo = exports.compGo;
    gObjName = exports.objName;
    if (!gFindAll || !gCompGo || !gObjName) {
        x::runtime::LogW("Titlebar", "BindApis: missing export/RVA");
        return false;
    }
    EnsurePriceFieldOff();
    x::runtime::LogI("Titlebar", "BindApis ok GA=%p FindAll=%p", exports.ga, gFindAll);
    return true;
}

void WarmForLoginWorkers() {
    // LOGIN 开 Titlebar worker 前：泵上跑完 FindClass，避免 LOADING 期 GC Fatal。
    auto job = [](void*) { EnsurePriceFieldOff(); };
    if (x::runtime::main_thread::IsOnPumpThread()) {
        job(nullptr);
        return;
    }
    if (!x::runtime::main_thread::IsInstalled() ||
        !x::runtime::main_thread::InvokeAndWait(job, nullptr, 2500,
                                                 x::runtime::main_thread::JobPrio::High)) {
        x::runtime::LogW("Titlebar", "WarmForLoginWorkers pump-wait fail");
    }
}

void* LocalCharacterStat() { return x::ui::player::LocalCharacterStat(); }

bool LocalUserLooksOk() {
    if (!LooksLikeHeapPtr(gLocalUser)) return false;
    void* wm = ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) return false;
    void* myUser = ReadPtr(wm, x::ui::player::OffWmMyUser());
    // 换图中 MyUser 可能暂空：缓存一律视为失效，避免沿用旧指针。
    if (!LooksLikeHeapPtr(myUser)) return false;
    return gLocalUser == myUser;
}

void ClearLocalUser() {
    gLocalUser = nullptr;
}

bool TryResolveLocalUser() {
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    (void)ports::world::GetWorldManager();
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    struct Context { bool ok; } context{false};
    auto task = [](void* raw) {
        auto* ctx = static_cast<Context*>(raw);
        auto isMyUser = [](void* user) {
            char name[96]{};
            return GetGoName(user, name, sizeof(name)) && _stricmp(name, "MyUser") == 0;
        };
        void* wm = ports::world::PeekWorldManager();
        void* myUser =
            LooksLikeHeapPtr(wm) ? ReadPtr(wm, x::ui::player::OffWmMyUser()) : nullptr;
        // SSOT = WM.MyUser@+0x28。指针变了必须立刻换；换图空窗直接清缓存。
        if (LooksLikeHeapPtr(myUser)) {
            if (gLocalUser != myUser) {
                x::runtime::LogI("Titlebar", "LocalUser rebind wm.MyUser %p -> %p", gLocalUser,
                                 myUser);
            }
            gLocalUser = myUser;
            ctx->ok = true;
            return;
        }
        gLocalUser = nullptr;
        // 裸 gFindAll：与 invuln / player_combat 同守仓级闸。
        if (x::runtime::managed_main::IsLoginFrozen() ||
            x::runtime::managed_main::IsMapTransitBlocked() ||
            !ports::world::IsPlayReady())
            return;
        if (!gLuType || !gFindAll) return;
        void* array = nullptr;
        __try { array = gFindAll(gLuType, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        for (uintptr_t index = 0, count = ArrayLen(array); index < count && index < 64; ++index) {
            void* candidate = ArrayAt(array, index);
            if (LooksLikeHeapPtr(candidate) && isMyUser(candidate)) {
                gLocalUser = candidate;
                ctx->ok = true;
                return;
            }
        }
    };
    return x::runtime::managed_main::Call(+task, &context, 2500) && context.ok;
}

// ItemDataManager 不是 UnityEngine.Object，Resources.FindObjectsOfTypeAll 会被引擎的类型闸当场
// 拒掉：永远返回不了对象，只会往客户端 Player.log 刷 "The type has to be derived from
// UnityEngine.Object"，同时白占一个 2500ms 的主泵 job。原先整条解析路只有那个必错分支给
// gItemDataManager 赋值，所以物价查询实际上从未工作过——去掉它不损失任何已有功能。
// 要真修好得走 Singleton<T> 的 Lazy 静态槽（参考 travel_port::ResolveSingleton），
// 但 ItemDataManager 是否 Singleton<> 尚未证实，未验证前不猜静态槽地址。
bool TryResolveItemDataManager() {
    EnsurePriceFieldOff();
    if (LooksLikeHeapPtr(gItemDataManager)) return true;
    x::runtime::LogWThrottled(912, 300000, "Titlebar",
                              "ItemDataManager 未解析（FindAll 对非 UnityEngine.Object 无效），"
                              "物价查询停用 hash=%s",
                              kItemDataManagerClass);
    return false;
}

bool ReadVitals(Vitals& out) {
    out = {};
    x::ui::player::Vitals src{};
    if (!x::ui::player::Read(src) || !src.ok) return false;
    out.ok = src.ok;
    out.level = src.level;
    out.job = src.job;
    out.hp = src.hp;
    out.mhp = src.mhp;
    out.mp = src.mp;
    out.mmp = src.mmp;
    out.exp = src.exp;
    out.maxExp = src.maxExp;
    out.meso = src.meso;
    std::memcpy(out.name, src.name, sizeof(out.name));
    return true;
}

double UpdateLootDelta(bool countIntoRate, uint64_t* knownOut, uint64_t* unknownOut) {
    if (knownOut) *knownOut = 0;
    if (unknownOut) *unknownOut = 0;
    std::unordered_map<int, unsigned long long> current;
    if (!SnapshotInventory(current)) return 0.0;
    if (!gHaveLootBaseline) {
        gLootBaseline = std::move(current);
        gHaveLootBaseline = true;
        return 0.0;
    }
    double value = 0.0;
    uint64_t known = 0, unknown = 0;
    for (const auto& [itemId, quantity] : current) {
        const auto previous = gLootBaseline.find(itemId);
        const unsigned long long before = previous == gLootBaseline.end() ? 0ull : previous->second;
        if (quantity <= before) continue;
        const unsigned long long delta = quantity - before;
        const int price = LookupSellPrice(itemId);
        if (price > 0) { value += static_cast<double>(delta) * price; known += delta; }
        else { unknown += delta; }
    }
    gLootBaseline = std::move(current);
    if (knownOut) *knownOut = known;
    if (unknownOut) *unknownOut = unknown;
    return countIntoRate ? value : 0.0;
}

void ResetLootBaseline() {
    gLootBaseline.clear();
    gHaveLootBaseline = false;
}

const char* JobNameTw(int job) {
    switch (job) {
        case 0: return "初心者";
        case 100: return "劍士"; case 110: return "狂戰士"; case 111: return "十字軍"; case 112: return "英雄";
        case 120: return "見習騎士"; case 121: return "騎士"; case 122: return "聖騎士";
        case 130: return "槍騎兵"; case 131: return "龍騎士"; case 132: return "黑騎士";
        case 200: return "法師"; case 210: return "巫師(火/毒)"; case 211: return "魔導士(火/毒)"; case 212: return "大魔導士(火/毒)";
        case 220: return "巫師(冰/雷)"; case 221: return "魔導士(冰/雷)"; case 222: return "大魔導士(冰/雷)";
        case 230: return "僧侶"; case 231: return "祭司"; case 232: return "主教";
        case 300: return "弓箭手"; case 310: return "獵人"; case 311: return "射手"; case 312: return "神射手";
        case 320: return "弩弓手"; case 321: return "遊俠"; case 322: return "箭神";
        case 400: return "盜賊"; case 410: return "刺客"; case 411: return "無影人"; case 412: return "夜使者";
        case 420: return "俠盜"; case 421: return "獨行客"; case 422: return "暗影神偷";
        case 500: return "海盜"; case 510: return "打手"; case 511: return "格鬥家"; case 512: return "拳霸";
        case 520: return "槍手"; case 521: return "神槍手"; case 522: return "槍神";
        default: return nullptr;
    }
}

const char* JobText(int job, char (&buffer)[16]) {
    if (const char* name = JobNameTw(job)) return name;
    snprintf(buffer, sizeof(buffer), "%d", job);
    return buffer;
}

}  // namespace x::features::titlebar::game
