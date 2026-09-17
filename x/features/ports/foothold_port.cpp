// Classic TWMS — MapData foothold / ladder-rope enumerate (read-only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foothold_port.h"

#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/log.h"
#include "xor_cstr.h"

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
    "f170a7675ad228e5a230f827f38e033ea098dfac23c613cb498e5428b4c39d5";
constexpr char kLrClass[] =
    "fea8cc1bb32e2e2006099c182b9b865fc7578ac4018abf187069aa88f389ad7";
constexpr char kActorBaseClass[] =
    "bceba896aaa7328f05716b053a9243bade419f40a12ea561361454a49d42034";
constexpr char kVecCtrlClass[] =
    "b8852488236cbfef306ec9af609517b4001d141a4dfea630f4493ff1dd93c9c";

constexpr char kHashFhId[] =
    "<e8cec24fd7621807a7137c0a6fb08196ee6a61e9789ac847fb2923904d5e66a>k__BackingField";
constexpr char kHashFhX1[] =
    "<c9bd39e47b2570741251866361f511271482116bf503b2d7b44373da352b215>k__BackingField";
constexpr char kHashFhY1[] =
    "<da189ae6a62bd6c6161e0e878adc93e260bdf7997f4c6c067d86da49a155bf3>k__BackingField";
constexpr char kHashFhX2[] =
    "<ad7091b520e1e3843869aece402c7186b90e88d662bc5ee7e4a6ce3725f9c6e>k__BackingField";
constexpr char kHashFhY2[] =
    "<b4e8f3aef577e02d17a811a44fae02ec404ea3215afb5cf5cb4773f5c365272>k__BackingField";
constexpr char kHashFhPrev[] =
    "<fbedf252615cf54bca9dfb8793a53e909d1be50cd8673528ab92c1261e0a100>k__BackingField";
constexpr char kHashFhNext[] =
    "<a5314be58091f1aac0fb3491c64568c8979883273e190edd409480d550c9c1e>k__BackingField";
constexpr char kHashFhForbid[] =
    "<ed90625240ac8cf1170b3d2f4e827076aaf3bcd6a07b8ca99c31e69bf7610e6>k__BackingField";
constexpr char kHashFhLayer[] =
    "<ead98acebe7ba68091259a74510c3441eb503976819571ebb164d86c0610afe>k__BackingField";
constexpr char kHashFhPage[] =
    "fc20eec23d5ed8e9012bb1cad7eb710ff0e330a2f8adf1239247e9d3d3068f3";
constexpr char kHashFhZMass[] =
    "cdafa269edddc7dc1c50944588880123d55051e95817529886ceedcdabe9120";

constexpr char kHashLrId[] =
    "<d4247ef1fc8dc131f82eab17472ebc78db9ac0152212da3c6216c8a13ba6143>k__BackingField";
constexpr char kHashLrX[] =
    "<afd989315a725861078b27d40c8c3cbab2df8baeb9ddcacc6a95ab0aa7c6b8f>k__BackingField";
constexpr char kHashLrY1[] =
    "<d685079c3d9bce3fe07218c16205bee5d81ec9f8bcef927121c1906af4c99be>k__BackingField";
constexpr char kHashLrY2[] =
    "<a37d4835f861efdf7864bbc4e4059e3fe5edfa5593107bb389d21d9f6e92f03>k__BackingField";
constexpr char kHashLrPage[] =
    "<dcf9f9fae694a3f1c0cb9ffedce053724b4f04b3a2a75bd6e3208070b5a53ca>k__BackingField";
constexpr char kHashLrIsLadder[] =
    "<e1c26477b0e086795e1ba5a1f31bc61d24054988e48d696f091f33d388b69aa>k__BackingField";
constexpr char kHashLrIsUpper[] =
    "<b035b09e53b7d374462ffd5cb70870a9647aa91ea0e738d4afd8a26081c8c90>k__BackingField";

constexpr char kHashUserVecCtrl[] =
    "<b4eeb44c75ab914f1491eb1375aede619676f2d39e20fc353d1f677f4ce708b>k__BackingField";
constexpr char kHashVcCurFh[] =
    "<b643d128c94c35a151a28de48a9751015e6dc062e12a4bc7983c99339fdc459>k__BackingField";

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
    HMODULE self = x::runtime::GetImageModule();
    if (!self)
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
    // Field 0 菇菇村訓練所入口是合法图号。禁止把 0 当成没图。
    if (mapId < 0) return false;
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
    return XCAT_ENV_NZ(kEnvFhDump);
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
            WriteFileOnly("dump summary-only map=%d (fh_dump env for fh/lr detail)",
                          meta.mapId);
        }
    }

    x::runtime::LogI("Foothold", "map=%d footholds=%d ladders=%d curFh=%u mismatch=%d (foothold.log)",
                     meta.mapId, meta.footholdN, meta.ladderN, meta.curFhId, meta.idMismatch);
}

}  // namespace x::features::ports::foothold
