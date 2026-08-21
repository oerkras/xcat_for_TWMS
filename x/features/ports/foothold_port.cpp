// Classic TWMS — MapData foothold / ladder-rope enumerate (read-only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foothold_port.h"

#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace x::features::ports::foothold {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

#define kOffWmMapData (x::runtime::il2cpp_mapdata::OffWmMapData())
#define kOffMapId (x::runtime::il2cpp_mapdata::OffMapId())
#define kOffFootholdMap (x::runtime::il2cpp_mapdata::OffFootholdMap())
#define kOffLadderRopes (x::runtime::il2cpp_mapdata::OffLadderRopes())

// Dictionary / List / Entry / Array → il2cpp_container（旧 freeCount@0x2C 误读 _version，已纠正）
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictFreeCount (x::runtime::il2cpp_container::OffDictFreeCount())
#define kEntrySize (x::runtime::il2cpp_container::DictEntryStrideIntPtr())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())
#define kOffEntryKey (x::runtime::il2cpp_container::OffDictEntryKey())
#define kOffEntryValue (x::runtime::il2cpp_container::OffDictEntryValuePtr())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())

#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())

// StaticFoothold / LadderOrRope / User.VecCtrl / VecCtrl.CurFh：hash → field_get_offset
// remounted 2026-08-06（与 teleport_port 同源；字段偏移未漂）
constexpr char kFhClass[] =
    "fe25c5d024ca8723d80f44ca4b281d98b03876a90fefe6d8d040e1e4f264686";
constexpr char kLrClass[] =
    "b816b7b84910a22ff54e1c94e85d12f89cf82da59a2a6ce5fc78eb9d18bd05a";
constexpr char kActorBaseClass[] =
    "dbfdaecfa6ec49a19a019f06093eac445b3ca6082f2b874b489646c98303d44";
constexpr char kVecCtrlClass[] =
    "c59d7aae5606ba613a8f11b7cb1b87f531d6779f531d032ae48cb63b641a919";

constexpr char kHashFhId[] =
    "<e8a3c25028bf37bb0a46ea3a6330758b7d1e488c08a79d9b86a0b479049308e>k__BackingField";
constexpr char kHashFhX1[] =
    "<bb9d1bd98bf3093201f93d1633f1bbe817baef78562a80a284a93860fd898cf>k__BackingField";
constexpr char kHashFhY1[] =
    "<bf5cbd5978017d4b22621636c6458155ea90d22baa7d8a28e433dd7a2ce8a77>k__BackingField";
constexpr char kHashFhX2[] =
    "<a5aadced346701bbf11c4ba38f5940728e5ec8e3c4d8b8a8314fca26d97fff6>k__BackingField";
constexpr char kHashFhY2[] =
    "<bac46f67b464ce7651fead276c96d71a4d41a6dd67a3888ea7ddbe4d8dd1369>k__BackingField";
constexpr char kHashFhPrev[] =
    "<dfd52a64cd064e93a9a0418537075d86f6c656559534028f775be0fcb7700e4>k__BackingField";
constexpr char kHashFhNext[] =
    "<b3104de95fb0b6e8c7d70dd79fd741a686f9d3b345165fbcb6ae0ac3e927789>k__BackingField";
constexpr char kHashFhForbid[] =
    "<d859d760feae8c7d922fc6a955781be880309a85a3f04bc01336c8c9220dadd>k__BackingField";
constexpr char kHashFhLayer[] =
    "<df693dfd0b557559ca3883058f1f8a0eaa2c5e6e66d33ea577b8b9a30fd4b5f>k__BackingField";
constexpr char kHashFhPage[] =
    "d56261fdd5e520bd16c7aa1b6cbafcb097cf8aef10b25443a0c3241f9dee5af";
constexpr char kHashFhZMass[] =
    "f931188ce68774c65d639bcedb5aa4182a4f9e2cf5b441974588271464cef6e";

constexpr char kHashLrId[] =
    "<d3783d548ed72e7f395daf98abcf619e4396a5c939743d347f565b67e3324f5>k__BackingField";
constexpr char kHashLrX[] =
    "<e2fbbf4edf2679d030f17f93a6fd3690f15644fba84409b505de2d69bd68143>k__BackingField";
constexpr char kHashLrY1[] =
    "<ccf05844b6fb01dd8dd52bf22fe6a24a84a6987518898679105c79ea370f292>k__BackingField";
constexpr char kHashLrY2[] =
    "<ca28f996935afe73c29cc659941c0337cee7287e218b2c780f81e9c51fb86ef>k__BackingField";
constexpr char kHashLrPage[] =
    "<b21e597ae95e8855a024ffd3de1532fcd16cf67e0799a8548509e7e33cbf27b>k__BackingField";
constexpr char kHashLrIsLadder[] =
    "<bcbc6e4219988511533047b4f2fb99dc5f8caf24189bc5eba4d5ef3aa35bd52>k__BackingField";
constexpr char kHashLrIsUpper[] =
    "<fd9406ac84eae73f5d92552e9cb5ca09652d07f6562649dd96bf35e61b32f5e>k__BackingField";

constexpr char kHashUserVecCtrl[] =
    "<fa431c5c26278a9e2fbcd0e091bb230983ceea3ac8ef75a7a90d5cbe5764964>k__BackingField";
constexpr char kHashVcCurFh[] =
    "<a88f4a31e6dd49b8a2f9d5e9f54aed5a8cb0ab6ab51c1bfadf11d254b938ed6>k__BackingField";

constexpr size_t kFbFhId = 0x10, kFbFhX1 = 0x14, kFbFhY1 = 0x18, kFbFhX2 = 0x1C, kFbFhY2 = 0x20;
constexpr size_t kFbFhPrev = 0x24, kFbFhNext = 0x28, kFbFhForbid = 0x38, kFbFhLayer = 0x44;
constexpr size_t kFbFhPage = 0x70, kFbFhZMass = 0x74;
constexpr size_t kFbLrId = 0x18, kFbLrX = 0x1C, kFbLrY1 = 0x20, kFbLrY2 = 0x24;
constexpr size_t kFbLrPage = 0x30, kFbLrIsLadder = 0x4C, kFbLrIsUpper = 0x4D;
constexpr size_t kFbUserVecCtrl = 0x50, kFbVcCurFh = 0x28;

size_t gOffFhId = kFbFhId, gOffFhX1 = kFbFhX1, gOffFhY1 = kFbFhY1, gOffFhX2 = kFbFhX2,
       gOffFhY2 = kFbFhY2, gOffFhPrev = kFbFhPrev, gOffFhNext = kFbFhNext,
       gOffFhForbid = kFbFhForbid, gOffFhLayer = kFbFhLayer, gOffFhPage = kFbFhPage,
       gOffFhZMass = kFbFhZMass;
size_t gOffLrId = kFbLrId, gOffLrX = kFbLrX, gOffLrY1 = kFbLrY1, gOffLrY2 = kFbLrY2,
       gOffLrPage = kFbLrPage, gOffLrIsLadder = kFbLrIsLadder, gOffLrIsUpper = kFbLrIsUpper;
size_t gOffUserVecCtrl = kFbUserVecCtrl, gOffVcCurFh = kFbVcCurFh;
#define kOffFhId (gOffFhId)
#define kOffFhX1 (gOffFhX1)
#define kOffFhY1 (gOffFhY1)
#define kOffFhX2 (gOffFhX2)
#define kOffFhY2 (gOffFhY2)
#define kOffFhPrev (gOffFhPrev)
#define kOffFhNext (gOffFhNext)
#define kOffFhForbid (gOffFhForbid)
#define kOffFhLayer (gOffFhLayer)
#define kOffFhPage (gOffFhPage)
#define kOffFhZMass (gOffFhZMass)
#define kOffLrId (gOffLrId)
#define kOffLrX (gOffLrX)
#define kOffLrY1 (gOffLrY1)
#define kOffLrY2 (gOffLrY2)
#define kOffLrPage (gOffLrPage)
#define kOffLrIsLadder (gOffLrIsLadder)
#define kOffLrIsUpper (gOffLrIsUpper)
#define kOffUserVecCtrl (gOffUserVecCtrl)
#define kOffVcCurFh (gOffVcCurFh)
bool gFhFieldTried = false;

bool FhFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t lo, size_t hi) {
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
            if (off >= lo && off < hi) {
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

void EnsureFhFieldOff() {
    if (gFhFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gFhFieldTried = true;
    void* fh = x::runtime::il2cpp::FindClass("", kFhClass);
    void* lr = x::runtime::il2cpp::FindClass("", kLrClass);
    void* actor = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    void* vc = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    int hits = 0;
    auto hit = [&](bool ok) {
        if (ok) ++hits;
    };
    hit(FhFieldOffHit(fh, kHashFhId, kFbFhId, &gOffFhId, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhX1, kFbFhX1, &gOffFhX1, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhY1, kFbFhY1, &gOffFhY1, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhX2, kFbFhX2, &gOffFhX2, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhY2, kFbFhY2, &gOffFhY2, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhPrev, kFbFhPrev, &gOffFhPrev, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhNext, kFbFhNext, &gOffFhNext, 0x10, 0x80));
    hit(FhFieldOffHit(fh, kHashFhForbid, kFbFhForbid, &gOffFhForbid, 0x20, 0x80));
    hit(FhFieldOffHit(fh, kHashFhLayer, kFbFhLayer, &gOffFhLayer, 0x20, 0x80));
    hit(FhFieldOffHit(fh, kHashFhPage, kFbFhPage, &gOffFhPage, 0x40, 0x100));
    hit(FhFieldOffHit(fh, kHashFhZMass, kFbFhZMass, &gOffFhZMass, 0x40, 0x100));
    hit(FhFieldOffHit(lr, kHashLrId, kFbLrId, &gOffLrId, 0x10, 0x80));
    hit(FhFieldOffHit(lr, kHashLrX, kFbLrX, &gOffLrX, 0x10, 0x80));
    hit(FhFieldOffHit(lr, kHashLrY1, kFbLrY1, &gOffLrY1, 0x10, 0x80));
    hit(FhFieldOffHit(lr, kHashLrY2, kFbLrY2, &gOffLrY2, 0x10, 0x80));
    hit(FhFieldOffHit(lr, kHashLrPage, kFbLrPage, &gOffLrPage, 0x20, 0x80));
    hit(FhFieldOffHit(lr, kHashLrIsLadder, kFbLrIsLadder, &gOffLrIsLadder, 0x30, 0x80));
    hit(FhFieldOffHit(lr, kHashLrIsUpper, kFbLrIsUpper, &gOffLrIsUpper, 0x30, 0x80));
    hit(FhFieldOffHit(actor, kHashUserVecCtrl, kFbUserVecCtrl, &gOffUserVecCtrl, 0x40, 0x100));
    hit(FhFieldOffHit(vc, kHashVcCurFh, kFbVcCurFh, &gOffVcCurFh, 0x10, 0x80));
    x::runtime::LogI("Foothold",
                     "fh/lr/vc slots path=%s hits=%d/20 id=0x%zX page=0x%zX lrId=0x%zX vc=0x%zX "
                     "curFh=0x%zX",
                     hits == 20 ? "meta" : (hits ? "meta-partial" : "fallback"), hits, gOffFhId,
                     gOffFhPage, gOffLrId, gOffUserVecCtrl, gOffVcCurFh);
}

std::mutex gCacheMu;
Snapshot* gCache = nullptr;  // heap；首次 Collect 分配
std::atomic<bool> gHaveCache{false};

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint32_t ReadU32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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

uintptr_t ArrayLen(void* arr) {
    if (!arr) return 0;
    __try {
        return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) +
                                             x::runtime::il2cpp_container::OffArrayMaxLength());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* ArrayAtPtr(void* arr, uintptr_t i) {
    if (!arr) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
                                         i * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool FillFh(void* fh, FootholdLite& out) {
    if (!LooksLikeHeapPtr(fh)) return false;
    out.id = ReadU32(fh, kOffFhId);
    if (out.id == 0) return false;
    out.x1 = ReadI32(fh, kOffFhX1);
    out.y1 = ReadI32(fh, kOffFhY1);
    out.x2 = ReadI32(fh, kOffFhX2);
    out.y2 = ReadI32(fh, kOffFhY2);
    out.prev = ReadU32(fh, kOffFhPrev);
    out.next = ReadU32(fh, kOffFhNext);
    out.forbidFall = ReadU8(fh, kOffFhForbid) != 0;
    out.layer = ReadI32(fh, kOffFhLayer);
    out.page = ReadI32(fh, kOffFhPage);
    out.zMass = ReadI32(fh, kOffFhZMass);
    return true;
}

bool FillLr(void* lr, LadderLite& out) {
    if (!LooksLikeHeapPtr(lr)) return false;
    out.id = ReadI32(lr, kOffLrId);
    out.x = ReadI32(lr, kOffLrX);
    out.y1 = ReadI32(lr, kOffLrY1);
    out.y2 = ReadI32(lr, kOffLrY2);
    out.page = ReadI32(lr, kOffLrPage);
    out.isLadder = ReadU8(lr, kOffLrIsLadder) != 0;
    out.isUpperFh = ReadU8(lr, kOffLrIsUpper) != 0;
    return out.y1 != out.y2 || out.x != 0;
}

uint32_t ReadPlayerCurFhId() {
    EnsureFhFieldOff();
    player_combat::CombatCtx ctx{};
    if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok || !LooksLikeHeapPtr(ctx.localUser))
        return 0;
    void* vc = ReadPtr(ctx.localUser, kOffUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return 0;
    void* fh = ReadPtr(vc, kOffVcCurFh);
    if (!LooksLikeHeapPtr(fh)) return 0;
    return ReadU32(fh, kOffFhId);
}

Snapshot* EnsureCacheUnlocked() {
    if (!gCache) gCache = new Snapshot{};
    return gCache;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &self) ||
        !self)
        return L".";
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L".";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return s.substr(0, slash);
}

void WriteFileOnly(const char* fmt, ...) {
    char body[1400];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1600];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    (void)x::runtime::AppendDbgLog(dir + L"\\foothold.log", buf, (DWORD)n);
}

void FillMeta(const Snapshot& s, SnapshotMeta& m) {
    m.ok = s.ok;
    m.mapId = s.mapId;
    m.footholdN = s.footholdN;
    m.ladderN = s.ladderN;
    m.curFhId = s.curFhId;
    m.idMismatch = s.idMismatch;
}

}  // namespace

bool EnsureBound() { return world::EnsureBound(); }

bool CollectToCache(SnapshotMeta* meta) {
    EnsureFhFieldOff();
    if (meta) *meta = SnapshotMeta{};
    if (!world::IsPlayReady()) return false;
    void* wm = world::GetWorldManager();
    if (!LooksLikeHeapPtr(wm)) return false;
    void* md = ReadPtr(wm, kOffWmMapData);
    if (!LooksLikeHeapPtr(md)) return false;

    const int32_t mapId = ReadI32(md, kOffMapId);
    const uint32_t curFh = ReadPlayerCurFhId();

    std::lock_guard<std::mutex> lock(gCacheMu);
    Snapshot* c = EnsureCacheUnlocked();
    // 就地重建，避免再栈分配 ~100KB Snapshot
    c->ok = false;
    c->mapId = mapId;
    c->curFhId = curFh;
    c->footholdN = 0;
    c->ladderN = 0;
    c->idMismatch = 0;

    void* dict = ReadPtr(md, kOffFootholdMap);
    if (LooksLikeHeapPtr(dict)) {
        x::runtime::il2cpp_container::RefineFromDictInstance(dict);
        void* entries = ReadPtr(dict, kOffDictEntries);
        const int count = ReadI32(dict, kOffDictCount);
        (void)ReadI32(dict, kOffDictFreeCount);
        if (LooksLikeHeapPtr(entries) && count >= 0 && count <= 8192) {
            const uintptr_t arrLen = ArrayLen(entries);
            for (uintptr_t i = 0; i < arrLen && c->footholdN < kMaxFootholds; ++i) {
                uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(
                    entries, static_cast<int>(i), kEntrySize);
                if (!entry) continue;
                const int hash = ReadI32(entry, kOffEntryHash);
                if (hash < 0) continue;
                void* fh = ReadPtr(entry, kOffEntryValue);
                FootholdLite lite{};
                if (!FillFh(fh, lite)) continue;
                const uint32_t key = ReadU32(entry, kOffEntryKey);
                if (key != 0 && key != lite.id) {
                    ++c->idMismatch;
                    lite.id = key;
                }
                c->footholds[c->footholdN++] = lite;
            }
        }
    }

    void* list = ReadPtr(md, kOffLadderRopes);
    if (LooksLikeHeapPtr(list)) {
        x::runtime::il2cpp_container::RefineFromListInstance(list);
        const int n = ReadI32(list, kOffListSize);
        void* items = ReadPtr(list, kOffListItems);
        if (LooksLikeHeapPtr(items) && n > 0 && n <= kMaxLadders) {
            for (int i = 0; i < n && c->ladderN < kMaxLadders; ++i) {
                void* lr = ArrayAtPtr(items, static_cast<uintptr_t>(i));
                LadderLite lite{};
                if (!FillLr(lr, lite)) continue;
                c->ladders[c->ladderN++] = lite;
            }
        }
    }

    c->ok = c->footholdN > 0 || c->ladderN > 0;
    if (!c->ok) {
        gHaveCache.store(false, std::memory_order_release);
        return false;
    }
    gHaveCache.store(true, std::memory_order_release);
    if (meta) FillMeta(*c, *meta);
    return true;
}

bool GetCachedMeta(SnapshotMeta* out) {
    if (!out) return false;
    *out = SnapshotMeta{};
    if (!gHaveCache.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(gCacheMu);
    if (!gCache || !gCache->ok) return false;
    FillMeta(*gCache, *out);
    return out->ok;
}

bool IsCacheReadyForMap(int32_t mapId) {
    if (mapId <= 0) return false;
    SnapshotMeta meta{};
    if (!GetCachedMeta(&meta) || !meta.ok) return false;
    return meta.mapId == mapId && meta.footholdN > 0;
}

bool TryGetCachedFh(uint32_t id, FootholdLite* out) {
    if (!out || id == 0) return false;
    *out = FootholdLite{};
    if (!gHaveCache.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(gCacheMu);
    if (!gCache || !gCache->ok) return false;
    for (int i = 0; i < gCache->footholdN; ++i) {
        if (gCache->footholds[i].id == id) {
            *out = gCache->footholds[i];
            return true;
        }
    }
    return false;
}

uint32_t PeekCurFhId() { return ReadPlayerCurFhId(); }

void* ResolveFhObject(uint32_t id) {
    if (id == 0) return nullptr;
    if (!world::IsPlayReady()) return nullptr;
    void* wm = world::GetWorldManager();
    if (!LooksLikeHeapPtr(wm)) return nullptr;
    void* md = ReadPtr(wm, kOffWmMapData);
    if (!LooksLikeHeapPtr(md)) return nullptr;
    void* dict = ReadPtr(md, kOffFootholdMap);
    if (!LooksLikeHeapPtr(dict)) return nullptr;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 8192) return nullptr;
    const uintptr_t arrLen = ArrayLen(entries);
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, static_cast<int>(i),
                                                                   kEntrySize);
        if (!entry) continue;
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        const uint32_t key = ReadU32(entry, kOffEntryKey);
        void* fh = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(fh)) continue;
        if (key == id || ReadU32(fh, kOffFhId) == id) return fh;
    }
    return nullptr;
}

bool GetCached(Snapshot& out) {
    if (!gHaveCache.load(std::memory_order_acquire)) {
        out = Snapshot{};
        return false;
    }
    std::lock_guard<std::mutex> lock(gCacheMu);
    if (!gCache || !gCache->ok) {
        out = Snapshot{};
        return false;
    }
    out = *gCache;
    return out.ok;
}

// 默认只写摘要；逐条 fh/lr 需 XCAT_FH_DUMP=1，或 idMismatch 时自动展开。
bool DetailDumpEnabled() {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_FH_DUMP", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] != '\0' && buf[0] != '0';
}

void DumpCachedLog() {
    SnapshotMeta meta{};
    {
        std::lock_guard<std::mutex> lock(gCacheMu);
        if (!gCache || !gCache->ok) {
            x::runtime::LogW("Foothold", "dump skip: empty cache");
            return;
        }
        FillMeta(*gCache, meta);

        WriteFileOnly("map=%d footholds=%d ladders=%d curFh=%u mismatch=%d", meta.mapId,
                      meta.footholdN, meta.ladderN, meta.curFhId, meta.idMismatch);

        const bool detail = DetailDumpEnabled() || meta.idMismatch != 0;
        if (detail) {
            for (int i = 0; i < gCache->footholdN; ++i) {
                const auto& f = gCache->footholds[i];
                WriteFileOnly(
                    "fh id=%u (%d,%d)->(%d,%d) prev=%u next=%u z=%d page=%d layer=%d fallForbid=%d",
                    f.id, f.x1, f.y1, f.x2, f.y2, f.prev, f.next, f.zMass, f.page, f.layer,
                    f.forbidFall ? 1 : 0);
            }
            for (int i = 0; i < gCache->ladderN; ++i) {
                const auto& l = gCache->ladders[i];
                WriteFileOnly("lr id=%d x=%d y=%d..%d page=%d ladder=%d upperFh=%d", l.id, l.x,
                              l.y1, l.y2, l.page, l.isLadder ? 1 : 0, l.isUpperFh ? 1 : 0);
            }
            WriteFileOnly("dump done map=%d", meta.mapId);
        } else {
            WriteFileOnly("dump summary-only map=%d (set XCAT_FH_DUMP=1 for fh/lr detail)",
                          meta.mapId);
        }
    }

    x::runtime::LogI("Foothold", "map=%d footholds=%d ladders=%d curFh=%u mismatch=%d (foothold.log)",
                     meta.mapId, meta.footholdN, meta.ladderN, meta.curFhId, meta.idMismatch);
}

}  // namespace x::features::ports::foothold
