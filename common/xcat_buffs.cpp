#include "xcat_buffs.h"

#include "xcat_config_ini.h"
#include "xcat_skill_names.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace xcat {
namespace {

constexpr uint32_t kBuffsIniVersion = 1u;

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%sstate", binDir);
    CreateDirectoryA(dir, nullptr);
    return true;
}

constexpr uint32_t kBuffsRuntimeShmMagic = 0x58434252u;  // 'XCBR'
constexpr uint32_t kBuffsRuntimeShmVersion = 2u;

struct BuffsRuntimeShared {
    uint32_t      magic = kBuffsRuntimeShmMagic;
    uint32_t      version = kBuffsRuntimeShmVersion;
    uint32_t      size = 0;
    volatile LONG seq = 0;
    BuffsRuntimeSnapshot snapshot{};
};

uint32_t HashBinDirForShm(const char* binDir) {
    // 与 PayloadStatus/Notify 一致：统一小写、/→\、去尾部 \，避免 launcher「XCat_data」与
    // payload「XCat_data\」哈希分裂导致面板永远读不到 BUFF runtime 快照。
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

std::wstring BuffsRuntimeMapName(const char* binDir) {
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\XCatBuffsRuntime_v2_%08X", HashBinDirForShm(binDir));
    return name;
}

struct BuffsRuntimeMappingCache {
    uint32_t hash = 0;
    HANDLE h = nullptr;
    BuffsRuntimeShared* view = nullptr;
};

BuffsRuntimeMappingCache g_readMap{};
BuffsRuntimeMappingCache g_writeMap{};

void CloseBuffsRuntimeMap(BuffsRuntimeMappingCache& m) {
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

BuffsRuntimeShared* OpenBuffsRuntimeMapForRead(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_readMap.view && g_readMap.hash == hash) return g_readMap.view;
    CloseBuffsRuntimeMap(g_readMap);

    const std::wstring name = BuffsRuntimeMapName(binDir);
    g_readMap.h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!g_readMap.h) return nullptr;
    g_readMap.view = static_cast<BuffsRuntimeShared*>(
        MapViewOfFile(g_readMap.h, FILE_MAP_READ, 0, 0, sizeof(BuffsRuntimeShared)));
    if (!g_readMap.view) {
        CloseBuffsRuntimeMap(g_readMap);
        return nullptr;
    }
    g_readMap.hash = hash;
    return g_readMap.view;
}

BuffsRuntimeShared* OpenBuffsRuntimeMapForWrite(const char* binDir) {
    const uint32_t hash = HashBinDirForShm(binDir);
    if (g_writeMap.view && g_writeMap.hash == hash) return g_writeMap.view;
    CloseBuffsRuntimeMap(g_writeMap);

    const std::wstring name = BuffsRuntimeMapName(binDir);
    g_writeMap.h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(BuffsRuntimeShared), name.c_str());
    if (!g_writeMap.h) return nullptr;
    g_writeMap.view = static_cast<BuffsRuntimeShared*>(
        MapViewOfFile(g_writeMap.h, FILE_MAP_WRITE, 0, 0, sizeof(BuffsRuntimeShared)));
    if (!g_writeMap.view) {
        CloseBuffsRuntimeMap(g_writeMap);
        return nullptr;
    }
    g_writeMap.hash = hash;
    return g_writeMap.view;
}

bool ValidBuffsRuntimePayload(const BuffsRuntimeSnapshot& snap) {
    return snap.magic == kBuffsRuntimeMagic && snap.version == kBuffsRuntimeVersion;
}

bool TryReadBuffsRuntimeShared(const char* binDir, BuffsRuntimeSnapshot& out) {
    BuffsRuntimeShared* view = OpenBuffsRuntimeMapForRead(binDir);
    if (!view) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = view->seq;
        if (s0 & 1) {
            Sleep(0);
            continue;
        }
        MemoryBarrier();
        BuffsRuntimeSnapshot copy = view->snapshot;
        MemoryBarrier();
        const LONG s1 = view->seq;
        if (s0 == s1 && !(s1 & 1) && view->magic == kBuffsRuntimeShmMagic &&
            view->version == kBuffsRuntimeShmVersion &&
            view->size == sizeof(BuffsRuntimeShared) && ValidBuffsRuntimePayload(copy)) {
            if (copy.count > kBuffsRuntimeMaxSkills)
                copy.count = static_cast<uint32_t>(kBuffsRuntimeMaxSkills);
            out = copy;
            return true;
        }
    }
    return false;
}

bool TryWriteBuffsRuntimeShared(const char* binDir, const BuffsRuntimeSnapshot& snapshot) {
    BuffsRuntimeShared* view = OpenBuffsRuntimeMapForWrite(binDir);
    if (!view) return false;

    BuffsRuntimeSnapshot disk = snapshot;
    if (disk.count > kBuffsRuntimeMaxSkills)
        disk.count = static_cast<uint32_t>(kBuffsRuntimeMaxSkills);

    if (!(view->seq & 1)) InterlockedIncrement(&view->seq);
    MemoryBarrier();
    view->magic = kBuffsRuntimeShmMagic;
    view->version = kBuffsRuntimeShmVersion;
    view->size = sizeof(BuffsRuntimeShared);
    view->snapshot = disk;
    MemoryBarrier();
    const LONG done = InterlockedIncrement(&view->seq);
    if (done & 1) InterlockedIncrement(&view->seq);
    return true;
}

bool ReadBuffsBinLegacy(const char* binDir, BuffsConfig& out) {
    BuffsSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    const std::string path = BuffsPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    BuffsConfig disk{};
    const size_t n = fread(&disk, 1, sizeof(BuffsConfig), f);
    fclose(f);
    if (n != sizeof(BuffsConfig)) return false;
    if (disk.magic != kBuffsMagic || disk.version != kBuffsVersion) return false;

    out = disk;
    return true;
}

bool ReadBuffsIni(const char* binDir, BuffsConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    uint32_t version = 0;
    if (!IniGetU32(ini, "buffs", "version", version) || version != kBuffsIniVersion) return false;

    BuffsSetDefaults(out);
    bool master = false;
    if (IniGetBool(ini, "buffs", "masterEnabled", master)) out.masterEnabled = master ? 1u : 0u;

    for (size_t i = 0; i < kBuffSlotCount; ++i) {
        char prefix[32]{};
        snprintf(prefix, sizeof(prefix), "slot.%zu.", i + 1);
        BuffSlotConfig& slot = out.slots[i];

        std::string key = std::string(prefix) + "enabled";
        bool enabled = false;
        if (IniGetBool(ini, "buffs", key.c_str(), enabled)) slot.enabled = enabled ? 1u : 0u;
        key = std::string(prefix) + "kind";
        IniGetU32(ini, "buffs", key.c_str(), slot.kind);
        key = std::string(prefix) + "code";
        std::string code;
        if (IniGetString(ini, "buffs", key.c_str(), code))
            strncpy_s(slot.code, code.c_str(), _TRUNCATE);
        key = std::string(prefix) + "intervalSec";
        IniGetU32(ini, "buffs", key.c_str(), slot.intervalSec);
        key = std::string(prefix) + "strategy";
        IniGetU32(ini, "buffs", key.c_str(), slot.strategy);
    }

    IniGetU32(ini, "buffs", "refreshSeq", out.refreshSeq);
    if (outWriteTick) IniGetU64(ini, "buffs", "writeTickMs", *outWriteTick);
    return true;
}

bool WriteBuffsIni(const char* binDir, const BuffsConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;

    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "buffs", "version", kBuffsIniVersion);
        IniSetU64(ini, "buffs", "writeTickMs", writeTickMs);
        IniSetU32(ini, "buffs", "refreshSeq", cfg.refreshSeq);
        IniSetBool(ini, "buffs", "masterEnabled", cfg.masterEnabled != 0);

        // 槽位数下调时清掉旧 slot.N.*，避免读到越界孤儿。
        IniEraseKeysWithPrefix(ini, "buffs", "slot.");
        for (size_t i = 0; i < kBuffSlotCount; ++i) {
            const BuffSlotConfig& slot = cfg.slots[i];
            char prefix[32]{};
            snprintf(prefix, sizeof(prefix), "slot.%zu.", i + 1);
            IniSetBool(ini, "buffs", (std::string(prefix) + "enabled").c_str(), slot.enabled != 0);
            IniSetU32(ini, "buffs", (std::string(prefix) + "kind").c_str(), slot.kind);
            IniSetString(ini, "buffs", (std::string(prefix) + "code").c_str(), slot.code);
            IniSetU32(ini, "buffs", (std::string(prefix) + "intervalSec").c_str(), slot.intervalSec);
            IniSetU32(ini, "buffs", (std::string(prefix) + "strategy").c_str(), slot.strategy);
        }

    });
}

bool WriteBuffsLkgFileImpl(const char* binDir, const BuffsConfig& cfg) {
    if (!binDir || !binDir[0]) return false;
    if (cfg.magic != kBuffsMagic || cfg.version != kBuffsVersion) return false;
    if (!EnsureStateDir(binDir)) return false;

    BuffsConfig normalized = cfg;
    BuffsNormalizeMasterEnabled(normalized);

    char tmp[MAX_PATH]{};
    char path[MAX_PATH]{};
    snprintf(tmp, sizeof(tmp), "%sstate\\buffs.lkg.tmp", binDir);
    snprintf(path, sizeof(path), "%sstate\\buffs.lkg", binDir);

    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") != 0 || !f) return false;
    const size_t n = fwrite(&normalized, 1, sizeof(BuffsConfig), f);
    const int flushed = fflush(f);
    fclose(f);
    if (n != sizeof(BuffsConfig) || flushed != 0) {
        DeleteFileA(tmp);
        return false;
    }

    DeleteFileA(path);
    if (!MoveFileA(tmp, path)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

}  // namespace

void BuffsSetDefaults(BuffsConfig& out) {
    out = {};
    out.magic = kBuffsMagic;
    out.version = kBuffsVersion;
    out.masterEnabled = 0;
    for (size_t i = 0; i < kBuffSlotCount; ++i) {
        out.slots[i].enabled = 0;
        out.slots[i].kind = kBuffKindSkill;
        out.slots[i].code[0] = '\0';
        out.slots[i].intervalSec = 180;
        out.slots[i].strategy = kBuffRenewByPresence;
    }
}

bool BuffsAnySlotEnabled(const BuffsConfig& cfg) {
    for (size_t i = 0; i < kBuffSlotCount; ++i) {
        if (cfg.slots[i].enabled != 0 && cfg.slots[i].code[0]) return true;
    }
    return false;
}

void BuffsNormalizeMasterEnabled(BuffsConfig& cfg) {
    // Master 与 slot 启用相互独立：用户可关总开关暂停续航，同时保留各槽配置。
    // 旧实现在「有启用槽且 master=0」时强制 master=1，导致 UI/写盘后总开关永远关不掉。
    (void)cfg;
}

std::string BuffsRelPath() { return "state\\buffs.bin"; }

std::string BuffsPath(const char* binDir) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%s%s", binDir ? binDir : "", BuffsRelPath().c_str());
    return path;
}

bool ReadBuffs(const char* binDir, BuffsConfig& out) {
    BuffsSetDefaults(out);
    if (!binDir || !binDir[0]) {
        BuffsNormalizeMasterEnabled(out);
        return false;
    }

    uint64_t iniTick = 0;
    BuffsConfig iniCfg{};
    const bool iniOk = ReadBuffsIni(binDir, iniCfg, &iniTick);

    BuffsConfig bin{};
    const bool binOk = ReadBuffsBinLegacy(binDir, bin);

    if (iniOk && binOk) {
        if (iniTick >= bin.writeTickMs) {
            out = iniCfg;
            out.writeTickMs = iniTick;
        } else {
            out = bin;
            WriteBuffsIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
        }
    } else if (iniOk) {
        out = iniCfg;
        out.writeTickMs = iniTick;
    } else if (binOk) {
        out = bin;
        WriteBuffsIni(binDir, out, out.writeTickMs ? out.writeTickMs : GetTickCount64());
    } else {
        BuffsNormalizeMasterEnabled(out);
        return false;
    }

    BuffsNormalizeMasterEnabled(out);
    return true;
}

bool WriteBuffs(const char* binDir, const BuffsConfig& cfg) {
    if (!binDir || !binDir[0]) return false;
    if (cfg.magic != kBuffsMagic || cfg.version != kBuffsVersion) return false;

    BuffsConfig normalized = cfg;
    BuffsNormalizeMasterEnabled(normalized);
    const uint64_t tick = normalized.writeTickMs ? normalized.writeTickMs : GetTickCount64();
    normalized.writeTickMs = tick;

    if (!WriteBuffsIni(binDir, normalized, tick)) return false;
    // 权威写成功后顺带刷新磁盘 LKG：冷启动 / [buffs] 节缺失时仍可 heal。
    (void)WriteBuffsLkgFile(binDir, normalized);
    return true;
}

std::string BuffsLkgRelPath() { return "state\\buffs.lkg"; }

std::string BuffsLkgPath(const char* binDir) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%s%s", binDir ? binDir : "", BuffsLkgRelPath().c_str());
    return path;
}

bool ReadBuffsLkgFile(const char* binDir, BuffsConfig& out) {
    BuffsSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    const std::string path = BuffsLkgPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    BuffsConfig disk{};
    const size_t n = fread(&disk, 1, sizeof(BuffsConfig), f);
    fclose(f);
    if (n != sizeof(BuffsConfig)) return false;
    if (disk.magic != kBuffsMagic || disk.version != kBuffsVersion) return false;

    out = disk;
    BuffsNormalizeMasterEnabled(out);
    return true;
}

bool WriteBuffsLkgFile(const char* binDir, const BuffsConfig& cfg) {
    return WriteBuffsLkgFileImpl(binDir, cfg);
}

void BuffsRuntimeSnapshotSetDefaults(BuffsRuntimeSnapshot& out) {
    out = {};
    out.magic = kBuffsRuntimeMagic;
    out.version = kBuffsRuntimeVersion;
}

std::string BuffsRuntimeSnapshotRelPath() { return "state\\buffs_runtime.bin"; }

std::string BuffsRuntimeSnapshotPath(const char* binDir) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%s%s", binDir ? binDir : "",
             BuffsRuntimeSnapshotRelPath().c_str());
    return path;
}

bool ReadBuffsRuntimeSnapshotDisk(const char* binDir, BuffsRuntimeSnapshot& out) {
    BuffsRuntimeSnapshotSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    const std::string path = BuffsRuntimeSnapshotPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    BuffsRuntimeSnapshot disk{};
    const size_t n = fread(&disk, 1, sizeof(BuffsRuntimeSnapshot), f);
    fclose(f);
    if (n != sizeof(BuffsRuntimeSnapshot)) return false;
    if (disk.magic != kBuffsRuntimeMagic || disk.version != kBuffsRuntimeVersion) return false;
    if (disk.count > kBuffsRuntimeMaxSkills) disk.count = static_cast<uint32_t>(kBuffsRuntimeMaxSkills);

    out = disk;
    return true;
}

bool ReadBuffsRuntimeSnapshot(const char* binDir, BuffsRuntimeSnapshot& out) {
    BuffsRuntimeSnapshotSetDefaults(out);
    if (!binDir || !binDir[0]) return false;
    // SHM 每帧可读（memcpy），剩余时间即时；仅磁盘回退 / miss 节流，避免 TAB 每帧 fopen。
    if (TryReadBuffsRuntimeShared(binDir, out)) return true;

    static std::string s_diskMissDir;
    static uint64_t s_diskMissAt = 0;
    static bool s_diskMissCached = false;
    static BuffsRuntimeSnapshot s_diskMissSnap{};
    const uint64_t now = GetTickCount64();
    const bool sameMissDir = !s_diskMissDir.empty() && s_diskMissDir == binDir;
    if (sameMissDir && (now - s_diskMissAt) < 250ull) {
        if (s_diskMissCached) {
            out = s_diskMissSnap;
            return true;
        }
        return false;
    }

    if (ReadBuffsRuntimeSnapshotDisk(binDir, out)) {
        s_diskMissDir = binDir;
        s_diskMissAt = now;
        s_diskMissCached = true;
        s_diskMissSnap = out;
        return true;
    }
    s_diskMissDir = binDir;
    s_diskMissAt = now;
    s_diskMissCached = false;
    return false;
}

bool WriteBuffsRuntimeSnapshotDisk(const char* binDir, const BuffsRuntimeSnapshot& snapshot) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;
    const std::string path = BuffsRuntimeSnapshotPath(binDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const size_t n = fwrite(&snapshot, 1, sizeof(BuffsRuntimeSnapshot), f);
    const int flushRc = fflush(f);
    fclose(f);
    return n == sizeof(BuffsRuntimeSnapshot) && flushRc == 0;
}

bool WriteBuffsRuntimeSnapshot(const char* binDir, const BuffsRuntimeSnapshot& snapshot) {
    if (!binDir || !binDir[0]) return false;
    if (!ValidBuffsRuntimePayload(snapshot)) return false;
    const bool shmOk = TryWriteBuffsRuntimeShared(binDir, snapshot);
    // 磁盘兜底：SHM 名偶发分裂 / 冷读时面板仍可回退。
    const bool diskOk = WriteBuffsRuntimeSnapshotDisk(binDir, snapshot);
    return shmOk || diskOk;
}

bool BuffNameLooksLikeCode(const char* code, const char* name) {
    if (!name || !name[0]) return true;
    if (!code || !code[0]) return false;
    return std::strcmp(name, code) == 0;
}

const char* BuffKnownDisplayName(const char* code) {
    if (!code || !code[0]) return nullptr;
    // 与 buff_effect_redirect / skill_catalog_full 缺口补表对齐（代理效果技 + 剑豪常用 BUFF）
    struct Row {
        const char* code;
        const char* name;
    };
    static constexpr Row kRows[] = {
        {"41001001", "拔刀術"},
        {"41110008", "拔刀術‧心體技"},
        {"42101002", "妖雲召喚"},
        {"42120024", "紫扇仰波‧力"},
        {"41101003", "武神招來"},
        {"41101005", "血咒縛"},
        {"40011002", "連刃斬"},
        {"41001000", "三連斬‧疾"},
    };
    for (const Row& r : kRows) {
        if (std::strcmp(r.code, code) == 0) return r.name;
    }
    return nullptr;
}

void BuffSkillDisplayLabel(const char* code, const char* name, char* out, size_t outSz,
                           const char* payloadBinDir) {
    if (!out || outSz == 0) return;
    out[0] = '\0';
    // offline-first：表命中即用，避免 RUNTIME 偶发韩文/乱码盖过离线繁中。
    if (payloadBinDir && payloadBinDir[0] && code && code[0]) {
        const SkillNamesPack& pack = GetSharedSkillNames(payloadBinDir);
        if (const char* offline = SkillNameLookup(pack, code)) {
            if (offline[0]) {
                strncpy_s(out, outSz, offline, _TRUNCATE);
                return;
            }
        }
    }
    if (!BuffNameLooksLikeCode(code, name)) {
        strncpy_s(out, outSz, name, _TRUNCATE);
        return;
    }
    if (const char* known = BuffKnownDisplayName(code)) {
        strncpy_s(out, outSz, known, _TRUNCATE);
        return;
    }
    if (name && name[0]) {
        strncpy_s(out, outSz, name, _TRUNCATE);
        return;
    }
    if (code && code[0]) strncpy_s(out, outSz, code, _TRUNCATE);
}

}  // namespace xcat
