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
    "a2eca01ad670b3c4b4ba85636ed1dfa4dc01ac28c03a91163597f3e0c9f4cf1";
constexpr char kPortalManagerClass[] =
    "e67f9ad37cf404d09bae32979e6e59ad535a364232b52dcf9a1d59458a0ed91";

constexpr char kHashWmMapData[] =
    "ca7537d2fae3d15edc28a3fb9695328e94aedb3358c69a3c33807338687b5b9";
constexpr char kHashMapId[] =
    "def0153e61596369af412ae296b171c485855ee731bd538ee084ba06b0941f5";
constexpr char kHashMapLifeList[] =
    "b8a14178bfe053c2a322b093bbc5981662b218e07cdd0ff9ad530b0cfa6c81d";
constexpr char kHashMapPortals[] =
    "fd46f6cf03736393416ecf39a9284a47155912fcea3f192097bc0aed108a6a6";
constexpr char kHashFootholdMap[] =
    "fc9707e184114f2061674e97895e121630f15b7fbe350c65e5b818ce6e93db3";
constexpr char kHashLadderRopes[] =
    "ec25cffe14add4082b214d1d08d52ff5ab8ae874c1094febd7423b7e288f89a";
constexpr char kHashPmPortalList[] =
    "a854c7581e07105f1d46cc9dccd4cffa4a8d312f6e94ff5632d2ad30c2d8229";

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
