#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "titlebar_game.h"

#include "../ports/world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
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
// ItemDataManager：08-03 后无明文名；以 dump 类哈希为准（dataTable@+0x18 / bundleMap@+0x38）。
constexpr char kItemDataManagerClass[] =
    "cd80f688c990f0dd0aafd2b78602618c46a424e5b4c34d18172867f24c782ec";

constexpr size_t kOffLuName = 0x1B8;
constexpr size_t kOffWmMyUser = 0x28;
constexpr size_t kOffWmCharacterData = 0xE0;
constexpr size_t kOffCdItemSlots = 0x40;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffSlotItemId = 0x10;       // ItemSlot 基类
constexpr size_t kOffBundleNumber = 0x28;     // ItemSlotBundle(子类) ushort；基类无此字段
constexpr size_t kOffIdmDataTable = 0x18;
constexpr size_t kOffIdmBundleMap = 0x38;
constexpr size_t kOffBundleSellPrice = 0x38;  // ItemBundle.nSellPrice
constexpr size_t kOffItemDataInfo = 0x18;
constexpr size_t kOffInfoPrice = 0x54;
constexpr size_t kOffInfoNotSale = 0x68;
constexpr size_t kOffDictBuckets = 0x10;
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kOffDictFreeCount = 0x2C;
constexpr size_t kEntrySize = 0x18;
constexpr size_t kOffEntryHash = 0x00;
constexpr size_t kOffEntryNext = 0x04;
constexpr size_t kOffEntryKey = 0x08;
constexpr size_t kOffEntryValue = 0x10;

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
void* gIdmType = nullptr;
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

int ArrayI32At(void* array, uintptr_t index) {
    if (!array) return -1;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(array) + 0x20 + index * sizeof(int));
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
                uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 +
                                 static_cast<uintptr_t>(index) * kEntrySize;
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
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + index * kEntrySize;
        if (ReadI32(entry, kOffEntryHash) < 0 || ReadI32(entry, kOffEntryKey) != key) continue;
        void* value = ReadPtr(entry, kOffEntryValue);
        return LooksLikeHeapPtr(value) ? value : nullptr;
    }
    return nullptr;
}

bool SnapshotInventory(std::unordered_map<int, unsigned long long>& out) {
    out.clear();
    void* worldManager = ports::world::GetWorldManager();
    void* characterData = ReadPtr(worldManager, kOffWmCharacterData);
    void* slotsArray = ReadPtr(characterData, kOffCdItemSlots);
    const uintptr_t typeCount = ArrayLen(slotsArray);
    if (!LooksLikeHeapPtr(worldManager) || !LooksLikeHeapPtr(characterData) ||
        !LooksLikeHeapPtr(slotsArray) || typeCount == 0 || typeCount > 16) {
        return false;
    }
    for (const int type : {kInvTiConsume, kInvTiInstall, kInvTiEtc}) {
        if (static_cast<uintptr_t>(type) >= typeCount) continue;
        void* list = ArrayAt(slotsArray, static_cast<uintptr_t>(type));
        void* items = ReadPtr(list, kOffListItems);
        const int size = ReadI32(list, kOffListSize);
        if (!LooksLikeHeapPtr(list) || !LooksLikeHeapPtr(items) || size <= 0 || size > 512) continue;
        const int count = (std::min)(size, static_cast<int>(ArrayLen(items)));
        for (int index = 0; index < count; ++index) {
            void* slot = ArrayAt(items, static_cast<uintptr_t>(index));
            const int itemId = ReadI32(slot, kOffSlotItemId);
            if (!LooksLikeHeapPtr(slot) || itemId <= 0) continue;
            // TI 2/3/4 槽位运行时多为 ItemSlotBundle；数量在子类 +0x28 ushort。
            unsigned long long quantity = ReadU16(slot, kOffBundleNumber);
            out[itemId] += quantity ? quantity : 1;
        }
    }
    return true;
}

int LookupOfflineSellPrice(int itemId) {
    char code[32]{};
    snprintf(code, sizeof(code), "%d", itemId);
    const xcat::ItemCatalogPack& pack = xcat::GetSharedItemCatalog(x::runtime::GetBinDir());
    return xcat::ItemCatalogLookupSellPrice(pack, code);
}

int LookupSellPrice(int itemId) {
    if (itemId <= 0) return 0;
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
    x::runtime::LogI("Titlebar", "BindApis ok GA=%p FindAll=%p", exports.ga, gFindAll);
    return true;
}

void* LocalCharacterStat() { return x::ui::player::LocalCharacterStat(); }

bool LocalUserLooksOk() {
    if (!LooksLikeHeapPtr(gLocalUser)) return false;
    void* wm = ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) return false;
    void* myUser = ReadPtr(wm, kOffWmMyUser);
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
        void* myUser = LooksLikeHeapPtr(wm) ? ReadPtr(wm, kOffWmMyUser) : nullptr;
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
        if (!ports::world::IsPlayReady()) return;
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

bool TryResolveItemDataManager() {
    if (LooksLikeHeapPtr(gItemDataManager)) return true;
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    if (!gIdmType) gIdmType = x::runtime::il2cpp::FindClassTypeObject(kItemDataManagerClass);
    if (!gIdmType || !gFindAll) {
        x::runtime::LogWThrottled(912, 30000, "Titlebar", "ItemDataManager type miss hash=%s",
                                  kItemDataManagerClass);
        return false;
    }
    struct Context { bool ok; } context{false};
    auto task = [](void* raw) {
        auto* ctx = static_cast<Context*>(raw);
        void* array = nullptr;
        __try { array = gFindAll(gIdmType, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        for (uintptr_t index = 0, count = ArrayLen(array); index < count && index < 4; ++index) {
            void* manager = ArrayAt(array, index);
            if (!LooksLikeHeapPtr(manager)) continue;
            if (LooksLikeHeapPtr(ReadPtr(manager, kOffIdmDataTable)) ||
                LooksLikeHeapPtr(ReadPtr(manager, kOffIdmBundleMap))) {
                gItemDataManager = manager;
                ctx->ok = true;
                return;
            }
            if (!gItemDataManager) gItemDataManager = manager;
        }
        ctx->ok = LooksLikeHeapPtr(gItemDataManager);
    };
    return x::runtime::managed_main::Call(+task, &context, 2500) && context.ok;
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
