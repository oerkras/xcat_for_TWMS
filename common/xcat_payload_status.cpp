#include "xcat_payload_status.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace xcat {
namespace {

constexpr uint32_t kPayloadStatusShmMagic = 0x58435053u;  // 'XCPS'
constexpr uint32_t kPayloadStatusShmVersion = 19u;

struct PayloadStatusShared {
    uint32_t magic = kPayloadStatusShmMagic;
    uint32_t version = kPayloadStatusShmVersion;
    uint32_t size = 0;
    volatile LONG seq = 0;
    PayloadStatus status{};
};

uint32_t HashBinDirForShm(const char* binDir) {
    // 归一化：统一小写、/→\、去掉尾部 \，避免 launcher「XCat_data」与 payload「XCat_data\」哈希分裂。
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

std::wstring PayloadStatusMapName(const char* binDir) {
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\XCatPayloadStatus_%08X", HashBinDirForShm(binDir));
    return name;
}

struct MappingCache {
    uint32_t hash = 0;
    HANDLE h = nullptr;
    PayloadStatusShared* view = nullptr;
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

PayloadStatusShared* OpenForRead(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_readMap.view && g_readMap.hash == hash) return g_readMap.view;
    CloseMap(g_readMap);

    const std::wstring name = PayloadStatusMapName(binDir);
    g_readMap.h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!g_readMap.h) return nullptr;
    g_readMap.view = static_cast<PayloadStatusShared*>(
        MapViewOfFile(g_readMap.h, FILE_MAP_READ, 0, 0, sizeof(PayloadStatusShared)));
    if (!g_readMap.view) {
        CloseMap(g_readMap);
        return nullptr;
    }
    g_readMap.hash = hash;
    return g_readMap.view;
}

PayloadStatusShared* OpenForWrite(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_writeMap.view && g_writeMap.hash == hash) return g_writeMap.view;
    CloseMap(g_writeMap);

    const std::wstring name = PayloadStatusMapName(binDir);
    g_writeMap.h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(PayloadStatusShared), name.c_str());
    if (!g_writeMap.h) return nullptr;
    g_writeMap.view = static_cast<PayloadStatusShared*>(
        MapViewOfFile(g_writeMap.h, FILE_MAP_WRITE, 0, 0, sizeof(PayloadStatusShared)));
    if (!g_writeMap.view) {
        CloseMap(g_writeMap);
        return nullptr;
    }
    g_writeMap.hash = hash;
    return g_writeMap.view;
}

bool ValidStatus(const PayloadStatus& st) {
    return st.magic == kPayloadStatusMagic && st.version == kPayloadStatusVersion;
}

uint64_t NowMs() {
    return GetTickCount64();
}

}  // namespace

void PayloadStatusSetDefaults(PayloadStatus& out) {
    out = PayloadStatus{};
}

bool ReadPayloadStatus(const char* binDir, PayloadStatus& out) {
    PayloadStatusSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    PayloadStatusShared* view = OpenForRead(binDir);
    if (!view) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = view->seq;
        if (s0 & 1) {
            Sleep(0);
            continue;
        }
        MemoryBarrier();
        PayloadStatus copy = view->status;
        MemoryBarrier();
        const LONG s1 = view->seq;
        if (s0 == s1 && !(s1 & 1) && view->magic == kPayloadStatusShmMagic &&
            view->version == kPayloadStatusShmVersion &&
            view->size == sizeof(PayloadStatusShared) && ValidStatus(copy)) {
            if (copy.writeTickMs) {
                const uint64_t now = NowMs();
                if (now >= copy.writeTickMs)
                    copy.worldChannelAgeSec =
                        static_cast<uint32_t>((now - copy.writeTickMs) / 1000ull);
            }
            out = copy;
            return true;
        }
    }
    return false;
}

bool WritePayloadStatus(const char* binDir, const PayloadStatus& status) {
    if (!binDir || !binDir[0]) return false;
    if (!ValidStatus(status) && status.magic != 0) {
        // Allow writer to fill magic/version for us.
    }

    PayloadStatusShared* view = OpenForWrite(binDir);
    if (!view) return false;

    PayloadStatus disk = status;
    disk.magic = kPayloadStatusMagic;
    disk.version = kPayloadStatusVersion;
    if (!disk.writeTickMs) disk.writeTickMs = NowMs();

    InterlockedIncrement(&view->seq);
    MemoryBarrier();
    view->magic = kPayloadStatusShmMagic;
    view->version = kPayloadStatusShmVersion;
    view->size = sizeof(PayloadStatusShared);
    view->status = disk;
    MemoryBarrier();
    InterlockedIncrement(&view->seq);
    return true;
}

bool PayloadStatusFresh(const PayloadStatus& st, uint64_t nowMs, uint64_t maxAgeMs) {
    if (!ValidStatus(st)) return false;
    if (st.worldChannelOnline < 0) return false;
    if (!st.writeTickMs) return false;
    if (nowMs < st.writeTickMs) return true;
    return (nowMs - st.writeTickMs) <= maxAgeMs;
}

bool PayloadStatusHeartbeatFresh(const PayloadStatus& st, uint64_t nowMs,
                                 uint64_t maxAgeMs) {
    if (!ValidStatus(st)) return false;
    if (!st.writeTickMs) return false;
    if (nowMs < st.writeTickMs) return true;
    return (nowMs - st.writeTickMs) <= maxAgeMs;
}

}  // namespace xcat
