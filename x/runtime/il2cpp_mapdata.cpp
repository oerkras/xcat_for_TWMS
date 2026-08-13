#include "il2cpp_mapdata.h"

#include "il2cpp_bind.h"
#include "il2cpp_shape.h"
#include "log.h"

#include <atomic>
#include <cstdio>

namespace x::runtime::il2cpp_mapdata {
namespace {

constexpr size_t kFbWmMapData = 0x88;
constexpr size_t kFbMapId = 0x10;
constexpr size_t kFbMapLifeList = 0x38;
constexpr size_t kFbMapPortals = 0x40;
constexpr size_t kFbFootholdMap = 0xE0;
constexpr size_t kFbLadderRopes = 0x108;
constexpr size_t kFbPmPortalList = 0x10;

// remounted 2026-08-06（TypeDef MapData=2067 PortalManager=1520；字段偏移未漂）
constexpr char kMapDataClass[] =
    "ae73c6f358ac48dd9bdcadbff6e6bf7991739ace4c41cc7d43e49afa5020905";
constexpr char kPortalManagerClass[] =
    "e9b546b967ec6df510bfa47a9e6604b3551a412c124f7b956f603ef8878dede";

constexpr char kHashWmMapData[] =
    "c721e31af565a590a794bd25addb031bd407242fcb9348ce92e729fa8896427";
constexpr char kHashMapId[] =
    "cf7f80e0825b3d8dc14048c78de447fff89c792624fe4b7a95f5b3dabd29ec7";
constexpr char kHashMapLifeList[] =
    "e20bea5b8e609529d58c593515af97c820200537e226aeac8ab232fd2f73acb";
constexpr char kHashMapPortals[] =
    "d096f129db27598b538b38dae8e3719ac402ee891544cc267adb6a345c58f26";
constexpr char kHashFootholdMap[] =
    "dcf92a929e6ecc874f9c979e654d8de8e0524536a16e1ab2233e760009c432c";
constexpr char kHashLadderRopes[] =
    "aa2770578aee1bc26ab6f3c38d1a16097efa23466e10c6ff9a07af70fb11256";
constexpr char kHashPmPortalList[] =
    "f005e38ee02b1952801e5e6cd87c804f1787d61a4ef6c8b7b01ad03eb89b92a";

size_t gOffWmMapData = kFbWmMapData;
size_t gOffMapId = kFbMapId;
size_t gOffMapLifeList = kFbMapLifeList;
size_t gOffMapPortals = kFbMapPortals;
size_t gOffFootholdMap = kFbFootholdMap;
size_t gOffLadderRopes = kFbLadderRopes;
size_t gOffPmPortalList = kFbPmPortalList;

std::atomic<bool> gTried{false};
char gPath[64]{};

bool PlausibleWm(size_t off) { return off >= 0x40 && off < 0x200; }
bool PlausibleMd(size_t off) { return off >= 0x10 && off < 0x200; }
bool PlausiblePm(size_t off) { return off >= 0x10 && off < 0x80; }

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

void ApplyOne(void* klass, const char* nm, size_t fb, size_t* out, bool (*ok)(size_t), int* hits) {
    if (!klass || !out || !hits) return;
    size_t got = FieldOff(klass, nm);
    if (got && ok && !ok(got)) got = 0;
    if (got) {
        *out = got;
        ++(*hits);
    } else {
        *out = fb;
    }
}

}  // namespace

void Ensure() {
    if (gTried.load(std::memory_order_acquire)) return;
    gTried.store(true, std::memory_order_release);
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(gPath, sizeof(gPath), "fallback");
        x::runtime::LogW("Il2CppMapData", "field off: bind miss — dump fallback");
        return;
    }

    void* wm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    void* md = x::runtime::il2cpp::FindClass("", kMapDataClass);
    void* pm = x::runtime::il2cpp::FindClass("", kPortalManagerClass);
    int hits = 0;
    ApplyOne(wm, kHashWmMapData, kFbWmMapData, &gOffWmMapData, PlausibleWm, &hits);
    ApplyOne(md, kHashMapId, kFbMapId, &gOffMapId, PlausibleMd, &hits);
    ApplyOne(md, kHashMapLifeList, kFbMapLifeList, &gOffMapLifeList, PlausibleMd, &hits);
    ApplyOne(md, kHashMapPortals, kFbMapPortals, &gOffMapPortals, PlausibleMd, &hits);
    ApplyOne(md, kHashFootholdMap, kFbFootholdMap, &gOffFootholdMap, PlausibleMd, &hits);
    ApplyOne(md, kHashLadderRopes, kFbLadderRopes, &gOffLadderRopes, PlausibleMd, &hits);
    ApplyOne(pm, kHashPmPortalList, kFbPmPortalList, &gOffPmPortalList, PlausiblePm, &hits);

    constexpr int kExpect = 7;
    snprintf(gPath, sizeof(gPath), "%s",
             hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"));
    x::runtime::LogI(
        "Il2CppMapData",
        "field off path=%s hits=%d/%d wm.md=0x%zX map={id=0x%zX life=0x%zX portals=0x%zX "
        "fh=0x%zX lr=0x%zX} pm.list=0x%zX",
        gPath, hits, kExpect, gOffWmMapData, gOffMapId, gOffMapLifeList, gOffMapPortals,
        gOffFootholdMap, gOffLadderRopes, gOffPmPortalList);
}

size_t OffWmMapData() {
    Ensure();
    return gOffWmMapData;
}
size_t OffMapId() {
    Ensure();
    return gOffMapId;
}
size_t OffMapLifeList() {
    Ensure();
    return gOffMapLifeList;
}
size_t OffMapPortals() {
    Ensure();
    return gOffMapPortals;
}
size_t OffFootholdMap() {
    Ensure();
    return gOffFootholdMap;
}
size_t OffLadderRopes() {
    Ensure();
    return gOffLadderRopes;
}
size_t OffPmPortalList() {
    Ensure();
    return gOffPmPortalList;
}

}  // namespace x::runtime::il2cpp_mapdata
