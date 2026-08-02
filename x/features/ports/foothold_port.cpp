// Classic TWMS — MapData foothold / ladder-rope enumerate (read-only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foothold_port.h"

#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
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

constexpr size_t kOffWmMapData = 0x88;
constexpr size_t kOffMapId = 0x10;
constexpr size_t kOffFootholdMap = 0xE0;
constexpr size_t kOffLadderRopes = 0x108;

constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kOffDictFreeCount = 0x2C;
constexpr size_t kEntrySize = 0x18;
constexpr size_t kOffEntryHash = 0x00;
constexpr size_t kOffEntryKey = 0x08;
constexpr size_t kOffEntryValue = 0x10;

constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;

constexpr size_t kOffFhId = 0x10;
constexpr size_t kOffFhX1 = 0x14;
constexpr size_t kOffFhY1 = 0x18;
constexpr size_t kOffFhX2 = 0x1C;
constexpr size_t kOffFhY2 = 0x20;
constexpr size_t kOffFhPrev = 0x24;
constexpr size_t kOffFhNext = 0x28;
constexpr size_t kOffFhForbid = 0x38;
constexpr size_t kOffFhLayer = 0x44;
constexpr size_t kOffFhPage = 0x70;
constexpr size_t kOffFhZMass = 0x74;

constexpr size_t kOffLrId = 0x18;
constexpr size_t kOffLrX = 0x1C;
constexpr size_t kOffLrY1 = 0x20;
constexpr size_t kOffLrY2 = 0x24;
constexpr size_t kOffLrPage = 0x30;
constexpr size_t kOffLrIsLadder = 0x4C;
constexpr size_t kOffLrIsUpper = 0x4D;

constexpr size_t kOffUserVecCtrl = 0x50;
constexpr size_t kOffVcCurFh = 0x28;

std::mutex gCacheMu;
Snapshot* gCache = nullptr;  // heap；首次 Collect 分配
std::atomic<bool> gHaveCache{false};
HANDLE gLog = INVALID_HANDLE_VALUE;

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
        return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* ArrayAtPtr(void* arr, uintptr_t i) {
    if (!arr) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
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

void EnsureLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"foothold.log");
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
    EnsureLog();
    if (gLog == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(gLog, buf, (DWORD)n, &w, nullptr);
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
        void* entries = ReadPtr(dict, kOffDictEntries);
        const int count = ReadI32(dict, kOffDictCount);
        (void)ReadI32(dict, kOffDictFreeCount);
        if (LooksLikeHeapPtr(entries) && count >= 0 && count <= 8192) {
            const uintptr_t arrLen = ArrayLen(entries);
            for (uintptr_t i = 0; i < arrLen && c->footholdN < kMaxFootholds; ++i) {
                uint8_t* entry =
                    reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
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
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 8192) return nullptr;
    const uintptr_t arrLen = ArrayLen(entries);
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(entries) + 0x20 + i * kEntrySize;
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
        for (int i = 0; i < gCache->footholdN; ++i) {
            const auto& f = gCache->footholds[i];
            WriteFileOnly(
                "fh id=%u (%d,%d)->(%d,%d) prev=%u next=%u z=%d page=%d layer=%d fallForbid=%d",
                f.id, f.x1, f.y1, f.x2, f.y2, f.prev, f.next, f.zMass, f.page, f.layer,
                f.forbidFall ? 1 : 0);
        }
        for (int i = 0; i < gCache->ladderN; ++i) {
            const auto& l = gCache->ladders[i];
            WriteFileOnly("lr id=%d x=%d y=%d..%d page=%d ladder=%d upperFh=%d", l.id, l.x, l.y1,
                          l.y2, l.page, l.isLadder ? 1 : 0, l.isUpperFh ? 1 : 0);
        }
        WriteFileOnly("dump done map=%d", meta.mapId);
    }

    x::runtime::LogI("Foothold", "map=%d footholds=%d ladders=%d curFh=%u mismatch=%d (foothold.log)",
                     meta.mapId, meta.footholdN, meta.ladderN, meta.curFhId, meta.idMismatch);
}

}  // namespace x::features::ports::foothold
