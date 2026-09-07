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
    "e7deb27fdaac1d8fec9cc2277febc6c35b2687979aebe9df5877ff29dc16129";
constexpr char kPortalManagerClass[] =
    "a772207dc761a88203ba79b3d090cea9136601f47b526fb34af27454d998e9d";

constexpr char kHashWmMapData[] =
    "b562aba780b2f942a4381660b4aa344dec0608179cf56523d2753c464a2e884";
constexpr char kHashMapId[] =
    "ff8f7e28e89db9cd0cb552ccb242d37840814548c4f8d192a9231dda3bdeb71";
constexpr char kHashMapLifeList[] =
    "e6ef98849288eed1ff4370ed270d3254ef796145582cf497de0c930d2fc92df";
constexpr char kHashMapPortals[] =
    "aa326f55ee7a2f3b7b2636836d963281bdf15500af623545201c7807e9ab8d6";
constexpr char kHashFootholdMap[] =
    "ea750672ff09ba8ca693327617757e438a7216d358e03f9726b13b174515e79";
constexpr char kHashLadderRopes[] =
    "ed77c5acc4b59b259fc374c6ef29f1bd4b7a0022724682a00d53fa13a90394c";
constexpr char kHashPmPortalList[] =
    "d4f0e522b0d707b423aaf4e4d342178a690416c74fff74ab09a5e38718661cc";

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
