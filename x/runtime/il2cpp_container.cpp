#include "il2cpp_container.h"

#include "il2cpp_bind.h"
#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::runtime::il2cpp_container {
namespace {

constexpr size_t kFbDictBuckets = 0x10;
constexpr size_t kFbDictEntries = 0x18;
constexpr size_t kFbDictCount = 0x20;
constexpr size_t kFbDictFreeList = 0x24;
constexpr size_t kFbDictFreeCount = 0x28;
constexpr size_t kFbDictVersion = 0x2C;
constexpr size_t kFbListItems = 0x10;
constexpr size_t kFbListSize = 0x18;
constexpr size_t kFbQueueArray = 0x10;
constexpr size_t kFbQueueHead = 0x18;
constexpr size_t kFbQueueTail = 0x1C;
constexpr size_t kFbQueueSize = 0x20;
constexpr size_t kFbQueueVersion = 0x24;
constexpr size_t kFbStackArray = 0x10;
constexpr size_t kFbStackSize = 0x18;
constexpr size_t kFbStackVersion = 0x1C;
constexpr size_t kFbHsBuckets = 0x10;
constexpr size_t kFbHsSlots = 0x18;
constexpr size_t kFbHsCount = 0x20;
constexpr size_t kFbHsLastIndex = 0x24;
constexpr size_t kFbHsFreeList = 0x28;
constexpr size_t kFbHsVersion = 0x38;

constexpr char kNs[] = "System.Collections.Generic";
constexpr char kDictName[] = "Dictionary`2";
constexpr char kListName[] = "List`1";
constexpr char kQueueName[] = "Queue`1";
constexpr char kStackName[] = "Stack`1";
constexpr char kHashSetName[] = "HashSet`1";

constexpr char kFldBuckets[] = "_buckets";
constexpr char kFldEntries[] = "_entries";
constexpr char kFldCount[] = "_count";
constexpr char kFldFreeList[] = "_freeList";
constexpr char kFldFreeCount[] = "_freeCount";
constexpr char kFldVersion[] = "_version";
constexpr char kFldItems[] = "_items";
constexpr char kFldSize[] = "_size";
constexpr char kFldArray[] = "_array";
constexpr char kFldHead[] = "_head";
constexpr char kFldTail[] = "_tail";
constexpr char kFldSlots[] = "_slots";
constexpr char kFldLastIndex[] = "_lastIndex";

size_t gOffDictBuckets = kFbDictBuckets;
size_t gOffDictEntries = kFbDictEntries;
size_t gOffDictCount = kFbDictCount;
size_t gOffDictFreeList = kFbDictFreeList;
size_t gOffDictFreeCount = kFbDictFreeCount;
size_t gOffDictVersion = kFbDictVersion;
size_t gOffListItems = kFbListItems;
size_t gOffListSize = kFbListSize;
size_t gOffQueueArray = kFbQueueArray;
size_t gOffQueueHead = kFbQueueHead;
size_t gOffQueueTail = kFbQueueTail;
size_t gOffQueueSize = kFbQueueSize;
size_t gOffQueueVersion = kFbQueueVersion;
size_t gOffStackArray = kFbStackArray;
size_t gOffStackSize = kFbStackSize;
size_t gOffStackVersion = kFbStackVersion;
size_t gOffHsBuckets = kFbHsBuckets;
size_t gOffHsSlots = kFbHsSlots;
size_t gOffHsCount = kFbHsCount;
size_t gOffHsLastIndex = kFbHsLastIndex;
size_t gOffHsFreeList = kFbHsFreeList;
size_t gOffHsVersion = kFbHsVersion;

std::atomic<bool> gTried{false};
std::atomic<bool> gDictLiveRefined{false};
std::atomic<bool> gListLiveRefined{false};
std::atomic<bool> gQueueLiveRefined{false};
std::atomic<bool> gStackLiveRefined{false};
std::atomic<bool> gHashSetLiveRefined{false};
char gPath[96]{};

bool PlausibleDictOff(size_t off) { return off >= 0x10 && off < 0x80; }
bool PlausibleListOff(size_t off) { return off >= 0x10 && off < 0x40; }
bool PlausibleQueueOff(size_t off) { return off >= 0x10 && off < 0x40; }
bool PlausibleStackOff(size_t off) { return off >= 0x10 && off < 0x40; }
bool PlausibleHsOff(size_t off) { return off >= 0x10 && off < 0x80; }

size_t FieldOff(void* klass, const char* name) {
    if (!klass || !name || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return 0;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        field = nullptr;
    }
    if (!field) return 0;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        off = 0;
    }
    return off;
}

size_t Pick(size_t got, size_t fb, bool* hit) {
    if (got) {
        if (hit) *hit = true;
        return got;
    }
    return fb;
}

void ApplyOne(void* klass, const char* nm, size_t fb, size_t* out, bool (*ok)(size_t), int* hits) {
    if (!klass || !out || !hits) return;
    size_t got = FieldOff(klass, nm);
    if (got && ok && !ok(got)) got = 0;
    bool h = false;
    *out = Pick(got, fb, &h);
    if (h) ++(*hits);
}

void ApplyDictKlass(void* dictKlass, int* hits) {
    ApplyOne(dictKlass, kFldBuckets, kFbDictBuckets, &gOffDictBuckets, PlausibleDictOff, hits);
    ApplyOne(dictKlass, kFldEntries, kFbDictEntries, &gOffDictEntries, PlausibleDictOff, hits);
    ApplyOne(dictKlass, kFldCount, kFbDictCount, &gOffDictCount, PlausibleDictOff, hits);
    ApplyOne(dictKlass, kFldFreeList, kFbDictFreeList, &gOffDictFreeList, PlausibleDictOff, hits);
    ApplyOne(dictKlass, kFldFreeCount, kFbDictFreeCount, &gOffDictFreeCount, PlausibleDictOff, hits);
    ApplyOne(dictKlass, kFldVersion, kFbDictVersion, &gOffDictVersion, PlausibleDictOff, hits);
}

void ApplyListKlass(void* listKlass, int* hits) {
    ApplyOne(listKlass, kFldItems, kFbListItems, &gOffListItems, PlausibleListOff, hits);
    ApplyOne(listKlass, kFldSize, kFbListSize, &gOffListSize, PlausibleListOff, hits);
}

void ApplyQueueKlass(void* qKlass, int* hits) {
    ApplyOne(qKlass, kFldArray, kFbQueueArray, &gOffQueueArray, PlausibleQueueOff, hits);
    ApplyOne(qKlass, kFldHead, kFbQueueHead, &gOffQueueHead, PlausibleQueueOff, hits);
    ApplyOne(qKlass, kFldTail, kFbQueueTail, &gOffQueueTail, PlausibleQueueOff, hits);
    ApplyOne(qKlass, kFldSize, kFbQueueSize, &gOffQueueSize, PlausibleQueueOff, hits);
    ApplyOne(qKlass, kFldVersion, kFbQueueVersion, &gOffQueueVersion, PlausibleQueueOff, hits);
}

void ApplyStackKlass(void* sKlass, int* hits) {
    ApplyOne(sKlass, kFldArray, kFbStackArray, &gOffStackArray, PlausibleStackOff, hits);
    ApplyOne(sKlass, kFldSize, kFbStackSize, &gOffStackSize, PlausibleStackOff, hits);
    ApplyOne(sKlass, kFldVersion, kFbStackVersion, &gOffStackVersion, PlausibleStackOff, hits);
}

void ApplyHashSetKlass(void* hsKlass, int* hits) {
    ApplyOne(hsKlass, kFldBuckets, kFbHsBuckets, &gOffHsBuckets, PlausibleHsOff, hits);
    ApplyOne(hsKlass, kFldSlots, kFbHsSlots, &gOffHsSlots, PlausibleHsOff, hits);
    ApplyOne(hsKlass, kFldCount, kFbHsCount, &gOffHsCount, PlausibleHsOff, hits);
    ApplyOne(hsKlass, kFldLastIndex, kFbHsLastIndex, &gOffHsLastIndex, PlausibleHsOff, hits);
    ApplyOne(hsKlass, kFldFreeList, kFbHsFreeList, &gOffHsFreeList, PlausibleHsOff, hits);
    ApplyOne(hsKlass, kFldVersion, kFbHsVersion, &gOffHsVersion, PlausibleHsOff, hits);
}

void* KlassOf(void* obj) {
    if (!obj || !x::runtime::il2cpp::Ensure()) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.objectGetClass) return nullptr;
    void* k = nullptr;
    __try {
        k = e.objectGetClass(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        k = nullptr;
    }
    return k;
}

}  // namespace

void Ensure() {
    if (gTried.load(std::memory_order_acquire)) return;
    gTried.store(true, std::memory_order_release);
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(gPath, sizeof(gPath), "fallback");
        x::runtime::LogW("Il2CppContainer", "field off: bind miss — dump fallback");
        return;
    }

    int hits = 0;
    void* dictKlass = x::runtime::il2cpp::FindClass(kNs, kDictName);
    void* listKlass = x::runtime::il2cpp::FindClass(kNs, kListName);
    void* queueKlass = x::runtime::il2cpp::FindClass(kNs, kQueueName);
    void* stackKlass = x::runtime::il2cpp::FindClass(kNs, kStackName);
    void* hsKlass = x::runtime::il2cpp::FindClass(kNs, kHashSetName);
    ApplyDictKlass(dictKlass, &hits);
    ApplyListKlass(listKlass, &hits);
    ApplyQueueKlass(queueKlass, &hits);
    ApplyStackKlass(stackKlass, &hits);
    ApplyHashSetKlass(hsKlass, &hits);

    constexpr int kExpect = 22;  // 6+2+5+3+6
    snprintf(gPath, sizeof(gPath), "%s",
             hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"));
    x::runtime::LogI(
        "Il2CppContainer",
        "field off path=%s hits=%d/%d dict={ent=0x%zX free=0x%zX} list={items=0x%zX size=0x%zX} "
        "queue={arr=0x%zX head=0x%zX size=0x%zX} stack={arr=0x%zX size=0x%zX} "
        "hs={slots=0x%zX cnt=0x%zX}",
        gPath, hits, kExpect, gOffDictEntries, gOffDictFreeCount, gOffListItems, gOffListSize,
        gOffQueueArray, gOffQueueHead, gOffQueueSize, gOffStackArray, gOffStackSize, gOffHsSlots,
        gOffHsCount);
}

void RefineFromDictInstance(void* dict) {
    Ensure();
    if (gDictLiveRefined.load(std::memory_order_acquire)) return;
    void* k = KlassOf(dict);
    if (!k) return;
    int hits = 0;
    ApplyDictKlass(k, &hits);
    if (hits > 0) {
        gDictLiveRefined.store(true, std::memory_order_release);
        x::runtime::LogI("Il2CppContainer", "refine dict hits=%d ent=0x%zX free=0x%zX klass=%p", hits,
                         gOffDictEntries, gOffDictFreeCount, k);
    }
}

void RefineFromListInstance(void* list) {
    Ensure();
    if (gListLiveRefined.load(std::memory_order_acquire)) return;
    void* k = KlassOf(list);
    if (!k) return;
    int hits = 0;
    ApplyListKlass(k, &hits);
    if (hits > 0) {
        gListLiveRefined.store(true, std::memory_order_release);
        x::runtime::LogI("Il2CppContainer", "refine list hits=%d items=0x%zX size=0x%zX klass=%p",
                         hits, gOffListItems, gOffListSize, k);
    }
}

void RefineFromQueueInstance(void* queue) {
    Ensure();
    if (gQueueLiveRefined.load(std::memory_order_acquire)) return;
    void* k = KlassOf(queue);
    if (!k) return;
    int hits = 0;
    ApplyQueueKlass(k, &hits);
    if (hits > 0) {
        gQueueLiveRefined.store(true, std::memory_order_release);
        x::runtime::LogI("Il2CppContainer",
                         "refine queue hits=%d arr=0x%zX head=0x%zX size=0x%zX klass=%p", hits,
                         gOffQueueArray, gOffQueueHead, gOffQueueSize, k);
    }
}

void RefineFromStackInstance(void* stack) {
    Ensure();
    if (gStackLiveRefined.load(std::memory_order_acquire)) return;
    void* k = KlassOf(stack);
    if (!k) return;
    int hits = 0;
    ApplyStackKlass(k, &hits);
    if (hits > 0) {
        gStackLiveRefined.store(true, std::memory_order_release);
        x::runtime::LogI("Il2CppContainer", "refine stack hits=%d arr=0x%zX size=0x%zX klass=%p",
                         hits, gOffStackArray, gOffStackSize, k);
    }
}

void RefineFromHashSetInstance(void* set) {
    Ensure();
    if (gHashSetLiveRefined.load(std::memory_order_acquire)) return;
    void* k = KlassOf(set);
    if (!k) return;
    int hits = 0;
    ApplyHashSetKlass(k, &hits);
    if (hits > 0) {
        gHashSetLiveRefined.store(true, std::memory_order_release);
        x::runtime::LogI("Il2CppContainer", "refine hashset hits=%d slots=0x%zX cnt=0x%zX klass=%p",
                         hits, gOffHsSlots, gOffHsCount, k);
    }
}

size_t OffDictBuckets() {
    Ensure();
    return gOffDictBuckets;
}
size_t OffDictEntries() {
    Ensure();
    return gOffDictEntries;
}
size_t OffDictCount() {
    Ensure();
    return gOffDictCount;
}
size_t OffDictFreeList() {
    Ensure();
    return gOffDictFreeList;
}
size_t OffDictFreeCount() {
    Ensure();
    return gOffDictFreeCount;
}
size_t OffDictVersion() {
    Ensure();
    return gOffDictVersion;
}
size_t OffListItems() {
    Ensure();
    return gOffListItems;
}
size_t OffListSize() {
    Ensure();
    return gOffListSize;
}
size_t OffQueueArray() {
    Ensure();
    return gOffQueueArray;
}
size_t OffQueueHead() {
    Ensure();
    return gOffQueueHead;
}
size_t OffQueueTail() {
    Ensure();
    return gOffQueueTail;
}
size_t OffQueueSize() {
    Ensure();
    return gOffQueueSize;
}
size_t OffQueueVersion() {
    Ensure();
    return gOffQueueVersion;
}
size_t OffStackArray() {
    Ensure();
    return gOffStackArray;
}
size_t OffStackSize() {
    Ensure();
    return gOffStackSize;
}
size_t OffStackVersion() {
    Ensure();
    return gOffStackVersion;
}
size_t OffHashSetBuckets() {
    Ensure();
    return gOffHsBuckets;
}
size_t OffHashSetSlots() {
    Ensure();
    return gOffHsSlots;
}
size_t OffHashSetCount() {
    Ensure();
    return gOffHsCount;
}
size_t OffHashSetLastIndex() {
    Ensure();
    return gOffHsLastIndex;
}
size_t OffHashSetFreeList() {
    Ensure();
    return gOffHsFreeList;
}
size_t OffHashSetVersion() {
    Ensure();
    return gOffHsVersion;
}

// Il2CppArray / Entry：ABI 常量（不依赖 Ensure / field meta）。
size_t OffArrayMaxLength() { return 0x18; }
size_t OffArrayData() { return 0x20; }

size_t DictEntryStrideIntPtr() { return 0x18; }
size_t DictEntryStrideIntIntTight() { return 0x10; }
size_t DictEntryStrideIntIntAlign() { return 0x18; }
size_t OffDictEntryHash() { return 0; }
size_t OffDictEntryNext() { return 4; }
size_t OffDictEntryKey() { return 8; }
size_t OffDictEntryValuePtr() { return 0x10; }
size_t OffDictEntryValueIntTight() { return 12; }
size_t OffDictEntryValueIntAlign() { return 16; }

uint8_t* DictEntryAt(void* entries, int index, size_t stride) {
    if (!entries || index < 0 || stride < 16) return nullptr;
    return reinterpret_cast<uint8_t*>(entries) + OffArrayData() +
           static_cast<size_t>(index) * stride;
}

}  // namespace x::runtime::il2cpp_container
