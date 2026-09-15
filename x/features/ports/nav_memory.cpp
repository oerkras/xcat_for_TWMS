#include "nav_memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

namespace x::features::ports::nav_memory {
namespace {

constexpr DWORD kRespawnSaveGapMs = 30000;

std::mutex gMu;
MapMemory gCur{};          // 当前图的记忆（合并了文件 + 本会话学到的）
bool gCurLoaded = false;
bool gCurDirty = false;
DWORD gCurSavedMs = 0;

std::string Dir() {
    const char* bin = x::runtime::GetBinDir();
    std::string d = bin ? bin : "";
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    return d + "state\\navmem";
}

bool EnsureDir() {
    const std::string root = Dir();
    // state\ 可能也不存在：逐级建。
    const size_t cut = root.find_last_of('\\');
    if (cut != std::string::npos) CreateDirectoryA(root.substr(0, cut).c_str(), nullptr);
    CreateDirectoryA(root.c_str(), nullptr);
    const DWORD attr = GetFileAttributesA(root.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string MapPath(int32_t mapId) {
    char name[64];
    snprintf(name, sizeof(name), "\\map_%d.txt", mapId);
    return Dir() + name;
}

std::string EtaPath() { return Dir() + "\\eta_scale.txt"; }
std::string JumpLatPath() { return Dir() + "\\jump_lat.txt"; }

// 整文件写：先 .tmp 再替换，半截文件不会被下次读到。
bool WriteAtomic(const std::string& path, const std::string& body) {
    if (!EnsureDir()) return false;
    const std::string tmp = path + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
    const size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (n != body.size()) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

bool ReadAll(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > 256 * 1024) break;  // 记忆文件不该这么大；防呆
    }
    fclose(f);
    return true;
}

// 逐行 key=value；value 里的空格分隔子字段由调用方拆。
template <typename Fn>
void ForEachLine(const std::string& text, Fn&& fn) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        fn(line.substr(0, eq), line.substr(eq + 1));
    }
}

bool LoadUnlocked(int32_t mapId, MapMemory* out) {
    *out = MapMemory{};
    out->mapId = mapId;
    std::string text;
    if (!ReadAll(MapPath(mapId), text)) return false;
    ForEachLine(text, [&](const std::string& k, const std::string& v) {
        if (k == "respawn_ema_ms") {
            out->respawnEmaMs = static_cast<float>(atof(v.c_str()));
        } else if (k == "respawn_samples") {
            out->respawnSamples = atoi(v.c_str());
        } else if (k == "dead_edge") {
            unsigned from = 0, to = 0, kind = 0;
            if (sscanf_s(v.c_str(), "%u %u %u", &from, &to, &kind) == 3 && from && to &&
                out->deadN < kDeadEdgeCap) {
                out->dead[out->deadN++] = DeadEdgeRec{from, to, static_cast<uint8_t>(kind)};
            }
        }
    });
    if (out->respawnEmaMs < 0.f || out->respawnEmaMs > 600000.f) out->respawnEmaMs = 0.f;
    if (out->respawnSamples < 0) out->respawnSamples = 0;
    return true;
}

bool SaveUnlocked(const MapMemory& m) {
    if (!m.mapId) return false;
    std::string body;
    char line[128];
    snprintf(line, sizeof(line), "# xcat navmem map=%d\n", m.mapId);
    body += line;
    if (m.respawnEmaMs > 0.f) {
        snprintf(line, sizeof(line), "respawn_ema_ms=%.0f\nrespawn_samples=%d\n", m.respawnEmaMs,
                 m.respawnSamples);
        body += line;
    }
    for (int i = 0; i < m.deadN; ++i) {
        snprintf(line, sizeof(line), "dead_edge=%u %u %u\n", m.dead[i].from, m.dead[i].to,
                 (unsigned)m.dead[i].kind);
        body += line;
    }
    return WriteAtomic(MapPath(m.mapId), body);
}

// 切到另一张图：先把上一张图没落盘的写掉，再读新图。
void SwitchUnlocked(int32_t mapId) {
    if (gCurLoaded && gCur.mapId == mapId) return;
    if (gCurLoaded && gCurDirty) {
        (void)SaveUnlocked(gCur);
        gCurDirty = false;
    }
    (void)LoadUnlocked(mapId, &gCur);
    gCurLoaded = true;
    gCurDirty = false;
    gCurSavedMs = GetTickCount();
}

}  // namespace

bool Load(int32_t mapId, MapMemory* out) {
    if (!out || !mapId) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (gCurLoaded && gCur.mapId == mapId) {
        *out = gCur;  // 本会话已学到的也算
        return true;
    }
    return LoadUnlocked(mapId, out);
}

void NoteRespawn(int32_t mapId, float emaMs, int samples) {
    if (!mapId || !(emaMs > 0.f)) return;
    std::lock_guard<std::mutex> lock(gMu);
    SwitchUnlocked(mapId);
    gCur.respawnEmaMs = emaMs;
    gCur.respawnSamples = samples;
    gCurDirty = true;
    const DWORD now = GetTickCount();
    if (now - gCurSavedMs >= kRespawnSaveGapMs) {
        if (SaveUnlocked(gCur)) {
            gCurDirty = false;
            gCurSavedMs = now;
        }
    }
}

void NoteDeadEdge(int32_t mapId, uint32_t from, uint32_t to, uint8_t kind) {
    if (!mapId || !from || !to) return;
    std::lock_guard<std::mutex> lock(gMu);
    SwitchUnlocked(mapId);
    for (int i = 0; i < gCur.deadN; ++i) {
        if (gCur.dead[i].from == from && gCur.dead[i].to == to && gCur.dead[i].kind == kind) return;
    }
    if (gCur.deadN >= kDeadEdgeCap) {
        for (int i = 1; i < gCur.deadN; ++i) gCur.dead[i - 1] = gCur.dead[i];
        gCur.deadN = kDeadEdgeCap - 1;
    }
    gCur.dead[gCur.deadN++] = DeadEdgeRec{from, to, kind};
    gCurDirty = true;
    if (SaveUnlocked(gCur)) {
        gCurDirty = false;
        gCurSavedMs = GetTickCount();
        x::runtime::LogI("NavMem", "dead edge saved map=%d %u->%u kind=%u (n=%d)", mapId, from, to,
                         (unsigned)kind, gCur.deadN);
    }
}

bool LoadEtaScale(float* scale, int* samples, int n) {
    if (!scale || !samples || n <= 0) return false;
    if (n > kEtaKinds) n = kEtaKinds;
    std::lock_guard<std::mutex> lock(gMu);
    std::string text;
    if (!ReadAll(EtaPath(), text)) return false;
    bool any = false;
    ForEachLine(text, [&](const std::string& k, const std::string& v) {
        if (k.rfind("kind", 0) != 0) return;
        const int idx = atoi(k.c_str() + 4);
        if (idx < 0 || idx >= n) return;
        float sc = 1.f;
        int sm = 0;
        if (sscanf_s(v.c_str(), "%f %d", &sc, &sm) >= 1 && sc > 0.2f && sc < 5.f) {
            scale[idx] = sc;
            samples[idx] = sm < 0 ? 0 : sm;
            any = true;
        }
    });
    return any;
}

void SaveEtaScale(const float* scale, const int* samples, int n) {
    if (!scale || !samples || n <= 0) return;
    if (n > kEtaKinds) n = kEtaKinds;
    std::lock_guard<std::mutex> lock(gMu);
    std::string body = "# xcat navmem eta scale: kind<EdgeKind>=<scale> <samples>\n";
    char line[96];
    for (int i = 0; i < n; ++i) {
        snprintf(line, sizeof(line), "kind%d=%.3f %d\n", i, scale[i], samples[i]);
        body += line;
    }
    (void)WriteAtomic(EtaPath(), body);
}

bool LoadJumpLatency(float* ms, int* samples) {
    if (!ms) return false;
    std::lock_guard<std::mutex> lock(gMu);
    std::string text;
    if (!ReadAll(JumpLatPath(), text)) return false;
    bool any = false;
    ForEachLine(text, [&](const std::string& k, const std::string& v) {
        if (k != "jump_lat_ms") return;
        float lat = 0.f;
        int sm = 0;
        if (sscanf_s(v.c_str(), "%f %d", &lat, &sm) >= 1 && lat >= 16.f && lat <= 250.f) {
            *ms = lat;
            if (samples) *samples = sm < 0 ? 0 : sm;
            any = true;
        }
    });
    return any;
}

void SaveJumpLatency(float ms, int samples) {
    std::lock_guard<std::mutex> lock(gMu);
    char line[128];
    snprintf(line, sizeof(line), "# xcat navmem jump key latency EMA (ms) <samples>\njump_lat_ms=%.1f %d\n", ms, samples);
    (void)WriteAtomic(JumpLatPath(), std::string(line));
}

void Flush() {
    std::lock_guard<std::mutex> lock(gMu);
    if (gCurLoaded && gCurDirty && SaveUnlocked(gCur)) {
        gCurDirty = false;
        gCurSavedMs = GetTickCount();
    }
}

}  // namespace x::features::ports::nav_memory
