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
constexpr char kFhClass[] =
    "f7493895c6355227ba46ff22f0b3d491fac47e4c4ad2e735773a72878d9f860";
constexpr char kLrClass[] =
    "b94ef1fed5bf84dd6e8856229c2592eb46954694fc969d597a1ebd2e1b2ff92";
constexpr char kActorBaseClass[] =
    "ddef6db860cfa2bea6dca39e201bf3065a897797f86009fb4d6104830143d94";
constexpr char kVecCtrlClass[] =
    "ef24024acbe225bcc90ca332f3e00aff5800daa32a769057d2e830eeac776bb";

constexpr char kHashFhId[] =
    "<fc80e4108c6083194f2a7f6abf37ab87633bd47152fe92eea0ba2c7c23abed8>k__BackingField";
constexpr char kHashFhX1[] =
    "<b4b17e3fe55cee84a1cc309d4c6f7cb8f6ba1132cb7b0e3fe2187515f112799>k__BackingField";
constexpr char kHashFhY1[] =
    "<aea9f00b3be599c5d92303bbcce95fefce71cdecb0030dc3e248807b37221e9>k__BackingField";
constexpr char kHashFhX2[] =
    "<be62b8cab6ec790bc8e45378aea447a5e777169435b71c8cdeaf971d16b69ab>k__BackingField";
constexpr char kHashFhY2[] =
    "<e47137381c7954f12ba8667e57b0df9a37338ba57e2f98cd0a4725f2b6e1cb8>k__BackingField";
constexpr char kHashFhPrev[] =
    "<c14b3c900b70c67da47bc032287d7b07e898bf9895c367ebbc79e6935eb2ebc>k__BackingField";
constexpr char kHashFhNext[] =
    "<e1c269a102e7f21e0240fc82d5fe1b79e00a913eaf49d9c369a296b9d0ad06f>k__BackingField";
constexpr char kHashFhForbid[] =
    "<b0e1bb7bccc703dc580a266bf021aabefa9c110fbbe0df9d5877d02c0de79cf>k__BackingField";
constexpr char kHashFhLayer[] =
    "<be66c26dcc8381695f26b25066a8e8ae22165a5d1d5ba4c93bf54bb71bcb21d>k__BackingField";
constexpr char kHashFhPage[] =
    "c28c3f689b0f8b2f204199560e711169bf1a63366fa617342b9959cf670111a";
constexpr char kHashFhZMass[] =
    "ed42e98d30e044ed96f80f025f544a49392c0d8db42f80b54530b21dc5cedf5";

constexpr char kHashLrId[] =
    "<a041edfb843be8c6badddfdd63acf349d16d73eab4af0b12b3c06b50dec07fd>k__BackingField";
constexpr char kHashLrX[] =
    "<f2e95723d7d8359605c3415b44c473a7b8c8c424d3afd82368240a996efc9de>k__BackingField";
constexpr char kHashLrY1[] =
    "<b5aefcdd1326656c4bf3f5c9cc1a426ada1f8bf55e112291d5ae9e11fd916a7>k__BackingField";
constexpr char kHashLrY2[] =
    "<f596c50937b5c853a19b23a67d3a31f6306cddc9bc59e81db7a3de887e782f7>k__BackingField";
constexpr char kHashLrPage[] =
    "<b661e56e7664e11b7097bc9452e019d2911350714659897aa8b5ac602433e79>k__BackingField";
constexpr char kHashLrIsLadder[] =
    "<f9ea601b32028aaf2bf526ed1d25daa29be34a36b0907b5454ec7d5a5878694>k__BackingField";
constexpr char kHashLrIsUpper[] =
    "<d2b28f89df3c373f394ba9d786bdefd37f748aabe9777363e2b04918db05419>k__BackingField";

constexpr char kHashUserVecCtrl[] =
    "<dc76f5c9e250bc9a327a219b39e16c345cdabf7b01ad5c60b568045069c9120>k__BackingField";
constexpr char kHashVcCurFh[] =
    "<b7b98b20290b6ec6221cc7a98ad9113018910968cd1f681f60fe20f109ef629>k__BackingField";

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
