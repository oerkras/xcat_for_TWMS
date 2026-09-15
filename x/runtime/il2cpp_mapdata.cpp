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
    "bc995f86e542046f9d426044ff74cb9beab74ea4a04660ebb5264bf354bf467";
constexpr char kPortalManagerClass[] =
    "b27b4d08eef9e0707b4e40b9cdec68e79df4fa5cb1cd169e18dc52d1ce7c9db";

constexpr char kHashWmMapData[] =
    "b0af05bb6f921156c9af8d3ae528ed3bdd87ee6dbb68ff3a321013c3fd5ceae";
constexpr char kHashMapId[] =
    "c469bc578f01792717595b3482c6dc3378ad0ebff4e133be01e3a45213bf81f";
constexpr char kHashMapLifeList[] =
    "dfeaf6315a2d303fc97b430dcae11e87defb8d481714fdf99a9a753e8ba95bc";
constexpr char kHashMapPortals[] =
    "aae93a65f8d5c8615bccf32bb07c432b7ca7af85823b9cb57f327ea23d47a30";
constexpr char kHashFootholdMap[] =
    "c804251e4dc3148e9633724660bea16bf0c52d560c7e29433982cf92f5c1d41";
constexpr char kHashLadderRopes[] =
    "ce32def0b0072207a1616cb2818a523ab314982681bf26dfbaba687986806e6";
constexpr char kHashPmPortalList[] =
    "eb583ee05eaf89ecf590bd073f4a8353b5d1fc23c5949ee916ebe6ad2944b0d";

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
