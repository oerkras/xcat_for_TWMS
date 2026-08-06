#include "xcat_anchor_lamps.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace xcat {
namespace {

constexpr uint32_t kShmMagic = 0x58434153u;  // 'XCAS'
constexpr uint32_t kShmVersion = 2u;

struct AnchorLampsShared {
    uint32_t magic = kShmMagic;
    uint32_t version = kShmVersion;
    uint32_t size = 0;
    volatile LONG seq = 0;
    AnchorLampsStatus status{};
};

uint32_t HashBinDirForShm(const char* binDir) {
    char norm[1024]{};
    size_t n = 0;
    if (binDir) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(binDir); *p && n + 1 < sizeof(norm);
             ++p) {
            unsigned char c = *p;
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c == '/') c = '\\';
            norm[n++] = static_cast<char>(c);
        }
    }
    while (n > 0 && (norm[n - 1] == '\\' || norm[n - 1] == '/')) --n;
    norm[n] = 0;

    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(norm[i]);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

std::wstring MapName(const char* binDir) {
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\XCatAnchorLamps_v%u_%08X", kShmVersion, HashBinDirForShm(binDir));
    return name;
}

struct MappingCache {
    uint32_t hash = 0;
    HANDLE h = nullptr;
    AnchorLampsShared* view = nullptr;
};

MappingCache g_readMap{};
MappingCache g_writeMap{};

void CloseMap(MappingCache& m) {
    if (m.view) {
        UnmapViewOfFile(m.view);
        m.view = nullptr;
    }
    if (m.h) {
        CloseHandle(m.h);
        m.h = nullptr;
    }
    m.hash = 0;
}

AnchorLampsShared* OpenForRead(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_readMap.view && g_readMap.hash == hash) return g_readMap.view;
    CloseMap(g_readMap);

    const std::wstring name = MapName(binDir);
    g_readMap.h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!g_readMap.h) return nullptr;
    g_readMap.view = static_cast<AnchorLampsShared*>(
        MapViewOfFile(g_readMap.h, FILE_MAP_READ, 0, 0, sizeof(AnchorLampsShared)));
    if (!g_readMap.view) {
        CloseMap(g_readMap);
        return nullptr;
    }
    g_readMap.hash = hash;
    return g_readMap.view;
}

AnchorLampsShared* OpenForWrite(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_writeMap.view && g_writeMap.hash == hash) return g_writeMap.view;
    CloseMap(g_writeMap);

    const std::wstring name = MapName(binDir);
    g_writeMap.h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(AnchorLampsShared), name.c_str());
    if (!g_writeMap.h) return nullptr;
    g_writeMap.view = static_cast<AnchorLampsShared*>(
        MapViewOfFile(g_writeMap.h, FILE_MAP_WRITE, 0, 0, sizeof(AnchorLampsShared)));
    if (!g_writeMap.view) {
        CloseMap(g_writeMap);
        return nullptr;
    }
    g_writeMap.hash = hash;
    return g_writeMap.view;
}

bool ValidStatus(const AnchorLampsStatus& st) {
    return st.magic == kAnchorLampsMagic && st.version == kAnchorLampsVersion &&
           st.count <= kAnchorLampMax;
}

uint64_t NowMs() { return GetTickCount64(); }

}  // namespace

void AnchorLampsSetDefaults(AnchorLampsStatus& out) { out = AnchorLampsStatus{}; }

bool ReadAnchorLamps(const char* binDir, AnchorLampsStatus& out) {
    AnchorLampsSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    AnchorLampsShared* view = OpenForRead(binDir);
    if (!view) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = view->seq;
        if (s0 & 1) {
            Sleep(0);
            continue;
        }
        MemoryBarrier();
        AnchorLampsStatus copy = view->status;
        MemoryBarrier();
        const LONG s1 = view->seq;
        if (s0 == s1 && !(s1 & 1) && view->magic == kShmMagic && view->version == kShmVersion &&
            view->size == sizeof(AnchorLampsShared) && ValidStatus(copy)) {
            out = copy;
            return true;
        }
    }
    return false;
}

bool WriteAnchorLamps(const char* binDir, const AnchorLampsStatus& status) {
    if (!binDir || !binDir[0]) return false;

    AnchorLampsShared* view = OpenForWrite(binDir);
    if (!view) return false;

    AnchorLampsStatus disk = status;
    disk.magic = kAnchorLampsMagic;
    disk.version = kAnchorLampsVersion;
    if (disk.count > kAnchorLampMax) disk.count = static_cast<uint32_t>(kAnchorLampMax);
    if (!disk.writeTickMs) disk.writeTickMs = NowMs();

    InterlockedIncrement(&view->seq);
    MemoryBarrier();
    view->magic = kShmMagic;
    view->version = kShmVersion;
    view->size = sizeof(AnchorLampsShared);
    view->status = disk;
    MemoryBarrier();
    InterlockedIncrement(&view->seq);
    return true;
}

bool AnchorLampsFresh(const AnchorLampsStatus& st, uint64_t nowMs, uint64_t maxAgeMs) {
    if (!ValidStatus(st)) return false;
    if (!st.writeTickMs) return false;
    if (nowMs < st.writeTickMs) return true;
    return (nowMs - st.writeTickMs) <= maxAgeMs;
}

}  // namespace xcat
