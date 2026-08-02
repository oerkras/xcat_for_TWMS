#include "xcat_payload_notify.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xcat {
namespace {

CRITICAL_SECTION g_notifyCs{};
volatile long g_notifyCsInit = 0;

constexpr uint32_t kNotifyShmMagic = 0x58434654u;  // 'XCFT'
constexpr uint32_t kNotifyShmVersion = 1u;

struct PayloadNotifyShared {
    uint32_t      magic = kNotifyShmMagic;
    uint32_t      version = kNotifyShmVersion;
    uint32_t      size = 0;
    volatile LONG seq = 0;
    PayloadNotifyQueue queue{};
};

void EnsureNotifyCs() {
    if (InterlockedCompareExchange(&g_notifyCsInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_notifyCs);
        InterlockedExchange(&g_notifyCsInit, 2);
        return;
    }
    while (InterlockedCompareExchange(&g_notifyCsInit, 2, 2) != 2) Sleep(0);
}

struct NotifyLock {
    NotifyLock() {
        EnsureNotifyCs();
        EnterCriticalSection(&g_notifyCs);
    }
    ~NotifyLock() { LeaveCriticalSection(&g_notifyCs); }
};

uint32_t HashBinDirForShm(const char* binDir) {
    // 与 TWMS PayloadStatus 一致：统一小写、/→\、去尾部 \，避免 launcher「XCat_data」与
    // payload「XCat_data\」哈希分裂（对照仓旧实现未去尾部斜杠）。
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

std::wstring NotifyMapName(const char* binDir) {
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\XCatNotify_%08X", HashBinDirForShm(binDir));
    return name;
}

struct NotifyMappingCache {
    uint32_t hash = 0;
    HANDLE h = nullptr;
    PayloadNotifyShared* view = nullptr;
};

NotifyMappingCache g_readMap{};
NotifyMappingCache g_writeMap{};

void CloseNotifyMap(NotifyMappingCache& m) {
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

PayloadNotifyShared* OpenNotifyMapForRead(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_readMap.view && g_readMap.hash == hash) return g_readMap.view;
    CloseNotifyMap(g_readMap);

    const std::wstring name = NotifyMapName(binDir);
    g_readMap.h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!g_readMap.h) return nullptr;
    g_readMap.view = static_cast<PayloadNotifyShared*>(
        MapViewOfFile(g_readMap.h, FILE_MAP_READ, 0, 0, sizeof(PayloadNotifyShared)));
    if (!g_readMap.view) {
        CloseNotifyMap(g_readMap);
        return nullptr;
    }
    g_readMap.hash = hash;
    return g_readMap.view;
}

PayloadNotifyShared* OpenNotifyMapForWrite(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_writeMap.view && g_writeMap.hash == hash) return g_writeMap.view;
    CloseNotifyMap(g_writeMap);

    const std::wstring name = NotifyMapName(binDir);
    g_writeMap.h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(PayloadNotifyShared), name.c_str());
    if (!g_writeMap.h) return nullptr;
    g_writeMap.view = static_cast<PayloadNotifyShared*>(
        MapViewOfFile(g_writeMap.h, FILE_MAP_WRITE, 0, 0, sizeof(PayloadNotifyShared)));
    if (!g_writeMap.view) {
        CloseNotifyMap(g_writeMap);
        return nullptr;
    }
    g_writeMap.hash = hash;
    return g_writeMap.view;
}

bool ValidNotifyQueuePayload(const PayloadNotifyQueue& q) {
    return q.magic == kPayloadNotifyMagic && q.version == kPayloadNotifyVersion;
}

bool TryReadNotifyShared(const char* binDir, PayloadNotifyQueue& out) {
    PayloadNotifyShared* view = OpenNotifyMapForRead(binDir);
    if (!view) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = view->seq;
        if (s0 & 1) {
            Sleep(0);
            continue;
        }
        MemoryBarrier();
        PayloadNotifyQueue copy = view->queue;
        MemoryBarrier();
        const LONG s1 = view->seq;
        if (s0 == s1 && !(s1 & 1) && view->magic == kNotifyShmMagic &&
            view->version == kNotifyShmVersion && view->size == sizeof(PayloadNotifyShared) &&
            ValidNotifyQueuePayload(copy)) {
            out = copy;
            if (out.nextSeq == 0) out.nextSeq = 1;
            if (out.epoch == 0) out.epoch = 1;
            if (out.head >= kPayloadNotifyCapacity)
                out.head %= static_cast<uint32_t>(kPayloadNotifyCapacity);
            return true;
        }
    }
    return false;
}

bool TryWriteNotifyShared(const char* binDir, const PayloadNotifyQueue& queue) {
    PayloadNotifyShared* view = OpenNotifyMapForWrite(binDir);
    if (!view) return false;

    if (!(view->seq & 1)) InterlockedIncrement(&view->seq);
    MemoryBarrier();
    view->magic = kNotifyShmMagic;
    view->version = kNotifyShmVersion;
    view->size = sizeof(PayloadNotifyShared);
    view->queue = queue;
    MemoryBarrier();
    const LONG done = InterlockedIncrement(&view->seq);
    if (done & 1) InterlockedIncrement(&view->seq);
    return true;
}

void ClipCopy(char* dst, size_t dstSz, const char* src) {
    if (!dst || dstSz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, dstSz, src, _TRUNCATE);
}

#pragma pack(push, 1)
struct PayloadNotifyQueueV1 {
    uint32_t magic = kPayloadNotifyMagic;
    uint32_t version = 1u;
    uint32_t nextSeq = 1;
    uint32_t head = 0;
    PayloadNotifyEvent slots[kPayloadNotifyCapacity]{};
};
#pragma pack(pop)

bool ReadQueueFile(const char* path, PayloadNotifyQueue& out) {
    out = {};
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;

    uint32_t magic = 0;
    uint32_t version = 0;
    if (fread(&magic, 1, sizeof(magic), f) != sizeof(magic) ||
        fread(&version, 1, sizeof(version), f) != sizeof(version)) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    if (version == 1u) {
        PayloadNotifyQueueV1 v1{};
        const size_t n = fread(&v1, 1, sizeof(PayloadNotifyQueueV1), f);
        fclose(f);
        if (n < sizeof(PayloadNotifyQueueV1)) return false;
        if (v1.magic != kPayloadNotifyMagic) return false;
        out.magic = v1.magic;
        out.version = kPayloadNotifyVersion;
        out.nextSeq = v1.nextSeq;
        out.head = v1.head;
        out.epoch = 1;
        memcpy(out.slots, v1.slots, sizeof(out.slots));
    } else if (version == kPayloadNotifyVersion) {
        const size_t n = fread(&out, 1, sizeof(PayloadNotifyQueue), f);
        fclose(f);
        if (n < sizeof(PayloadNotifyQueue)) return false;
        if (out.magic != kPayloadNotifyMagic) return false;
    } else {
        fclose(f);
        return false;
    }

    if (out.nextSeq == 0) out.nextSeq = 1;
    if (out.epoch == 0) out.epoch = 1;
    if (out.head >= kPayloadNotifyCapacity) out.head %= static_cast<uint32_t>(kPayloadNotifyCapacity);
    return true;
}

bool LoadQueue(const char* binDir, PayloadNotifyQueue& out) {
    if (!binDir || !binDir[0]) return false;
    if (TryReadNotifyShared(binDir, out)) return true;
    const std::string path = PayloadNotifyPath(binDir);
    if (ReadQueueFile(path.c_str(), out)) {
        TryWriteNotifyShared(binDir, out);
        return true;
    }
    out = {};
    return true;
}

bool SaveQueue(const char* binDir, const PayloadNotifyQueue& queue) {
    if (!binDir || !binDir[0]) return false;
    return TryWriteNotifyShared(binDir, queue);
}

}  // namespace

std::string PayloadNotifyRelPath() { return "state\\notify.bin"; }

std::string PayloadNotifyPath(const char* binDir) {
    char path[MAX_PATH]{};
    const char* root = binDir ? binDir : "";
    const size_t len = std::strlen(root);
    const bool needSlash = len > 0 && root[len - 1] != '\\' && root[len - 1] != '/';
    snprintf(path, sizeof(path), "%s%s%s", root, needSlash ? "\\" : "", PayloadNotifyRelPath().c_str());
    return path;
}

bool ResetPayloadNotifyQueue(const char* binDir) {
    NotifyLock lock;
    PayloadNotifyQueue prev{};
    LoadQueue(binDir, prev);
    PayloadNotifyQueue queue{};
    queue.epoch = prev.epoch == 0 ? 1u : prev.epoch + 1u;
    return SaveQueue(binDir, queue);
}

bool PeekPayloadNotifyEpoch(const char* binDir, uint32_t* outEpoch) {
    if (!binDir || !binDir[0] || !outEpoch) return false;
    PayloadNotifyQueue queue{};
    if (!LoadQueue(binDir, queue)) return false;
    *outEpoch = queue.epoch ? queue.epoch : 1u;
    return true;
}

void SkipPayloadNotifyBacklog(const char* binDir, uint32_t* inOutLastSeq) {
    if (!binDir || !binDir[0] || !inOutLastSeq) return;
    PayloadNotifyQueue queue{};
    if (!LoadQueue(binDir, queue)) return;
    uint32_t maxSeq = *inOutLastSeq;
    for (const PayloadNotifyEvent& ev : queue.slots) {
        if (ev.seq > maxSeq) maxSeq = ev.seq;
    }
    *inOutLastSeq = maxSeq;
}

bool EnqueuePayloadNotify(const char* binDir, uint32_t kind, const char* key, const char* title,
                            const char* body, uint32_t ttlMs) {
    if (!binDir || !binDir[0]) return false;
    NotifyLock lock;

    PayloadNotifyQueue queue{};
    LoadQueue(binDir, queue);

    PayloadNotifyEvent ev{};
    ev.seq = queue.nextSeq++;
    ev.kind = kind;
    ev.ttlMs = ttlMs ? ttlMs : 4200u;
    ClipCopy(ev.key, sizeof(ev.key), key);
    ClipCopy(ev.title, sizeof(ev.title), title);
    ClipCopy(ev.body, sizeof(ev.body), body);

    const uint32_t idx = queue.head % static_cast<uint32_t>(kPayloadNotifyCapacity);
    queue.slots[idx] = ev;
    queue.head = (queue.head + 1u) % static_cast<uint32_t>(kPayloadNotifyCapacity);
    return SaveQueue(binDir, queue);
}

size_t DrainPayloadNotify(const char* binDir, uint32_t* inOutLastSeq, PayloadNotifyEvent* out,
                          size_t maxEvents) {
    if (!binDir || !binDir[0] || !inOutLastSeq || !out || maxEvents == 0) return 0;

    PayloadNotifyQueue queue{};
    if (!LoadQueue(binDir, queue)) return 0;

    std::vector<PayloadNotifyEvent> pending;
    pending.reserve(kPayloadNotifyCapacity);
    for (const PayloadNotifyEvent& ev : queue.slots) {
        if (!ev.seq || ev.seq <= *inOutLastSeq) continue;
        pending.push_back(ev);
    }
    if (pending.empty()) return 0;

    std::sort(pending.begin(), pending.end(),
              [](const PayloadNotifyEvent& a, const PayloadNotifyEvent& b) { return a.seq < b.seq; });

    const size_t n = (std::min)(pending.size(), maxEvents);
    uint32_t maxSeq = *inOutLastSeq;
    for (size_t i = 0; i < n; ++i) {
        out[i] = pending[i];
        if (pending[i].seq > maxSeq) maxSeq = pending[i].seq;
    }
    *inOutLastSeq = maxSeq;
    return n;
}

}  // namespace xcat
