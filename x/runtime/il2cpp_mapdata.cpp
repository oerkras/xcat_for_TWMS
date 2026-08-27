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
    "eca01f9d2fbb16afddb74e0a6128167dde4d2e423f84433d39f4654aef4a767";
constexpr char kPortalManagerClass[] =
    "f57372a0245b31de065eae5b3f27e196c2d3a536ae939b1a517e9fefa3aba09";

constexpr char kHashWmMapData[] =
    "b2dffc39394d96600313685b903b5892fa881485fc9ddb4db7156ccbc149402";
constexpr char kHashMapId[] =
    "a3aea2b4853180be40ac38082e43fd4d10c985a6de97344573cdb2908f63261";
constexpr char kHashMapLifeList[] =
    "afda327a1d994a5198e4c3eacf8a6df149406b6e38dbdbede279a894a735958";
constexpr char kHashMapPortals[] =
    "e1eeea933fc36003e666be0e9e48726a71016db39c2b0d8c680223540a2e879";
constexpr char kHashFootholdMap[] =
    "a7643bf54df625354a20a24a8b62af75dde88382e4ba196aed59dbcff0eac4a";
constexpr char kHashLadderRopes[] =
    "c27e53b14287ad15e813f0275e8887e46d082b9a75e1fe894c4260ee69e9205";
constexpr char kHashPmPortalList[] =
    "ed7c45820e8ecca4411511fa5e0bffce763a1f4afb1e41bb3e707558f0980fc";

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
