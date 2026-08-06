#include "xcat_map_bounds.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace xcat {
namespace {

std::string JoinPath(const char* binDir, const char* rel) {
    std::string out = binDir ? binDir : "";
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out += '\\';
    out += rel ? rel : "";
    return out;
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

void TrimTrailing(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> cols;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t t = line.find('\t', start);
        if (t == std::string::npos) {
            cols.push_back(line.substr(start));
            break;
        }
        cols.push_back(line.substr(start, t - start));
        start = t + 1;
    }
    return cols;
}

bool ParseIntCell(const std::string& s, int* out) {
    if (!out) return false;
    *out = 0;
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return false;
    *out = static_cast<int>(v);
    return true;
}

bool RectLooksValid(const MapVrRect& r) {
    return r.left < r.right && r.top < r.bottom;
}

}  // namespace

bool TryLoadMapBoundsPack(const char* payloadBinDir, MapBoundsPack& out) {
    out = {};
    if (!payloadBinDir || !payloadBinDir[0]) return false;
    const std::string path = JoinPath(payloadBinDir, "dataservice\\map_info.tsv");
    if (!FileExists(path)) return false;

    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;

    int idxTop = -1, idxLeft = -1, idxBottom = -1, idxRight = -1;
    std::string line;
    bool headerSeen = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            auto cols = SplitTabs(line);
            if (!cols.empty() && !cols[0].empty() && cols[0][0] == '#') {
                cols[0].erase(0, 1);
                TrimTrailing(cols[0]);
                while (!cols[0].empty() && (cols[0][0] == ' ' || cols[0][0] == '\t'))
                    cols[0].erase(0, 1);
            }
            for (size_t i = 0; i < cols.size(); ++i) {
                TrimTrailing(cols[i]);
                if (cols[i] == "VRTop")
                    idxTop = static_cast<int>(i);
                else if (cols[i] == "VRLeft")
                    idxLeft = static_cast<int>(i);
                else if (cols[i] == "VRBottom")
                    idxBottom = static_cast<int>(i);
                else if (cols[i] == "VRRight")
                    idxRight = static_cast<int>(i);
            }
            headerSeen = true;
            continue;
        }
        if (!headerSeen) continue;
        if (idxTop < 0 || idxLeft < 0 || idxBottom < 0 || idxRight < 0) {
            // 旧表无 VR 列：加载成功但 byId 空（mtime 热载后可换成新表）。
            out.loaded = true;
            return true;
        }

        auto cols = SplitTabs(line);
        if (cols.empty()) continue;
        int mapId = 0;
        if (!ParseIntCell(cols[0], &mapId) || mapId < 0) continue;
        const int need = (std::max)({idxTop, idxLeft, idxBottom, idxRight});
        if (static_cast<int>(cols.size()) <= need) continue;

        MapVrRect vr{};
        int top = 0, left = 0, bottom = 0, right = 0;
        const bool okT = ParseIntCell(cols[static_cast<size_t>(idxTop)], &top);
        const bool okL = ParseIntCell(cols[static_cast<size_t>(idxLeft)], &left);
        const bool okB = ParseIntCell(cols[static_cast<size_t>(idxBottom)], &bottom);
        const bool okR = ParseIntCell(cols[static_cast<size_t>(idxRight)], &right);
        if (!(okT && okL && okB && okR)) continue;
        vr.top = top;
        vr.left = left;
        vr.bottom = bottom;
        vr.right = right;
        vr.valid = RectLooksValid(vr);
        if (!vr.valid) continue;
        out.byId[mapId] = vr;
    }

    out.loaded = true;
    return true;
}

const MapBoundsPack& GetSharedMapBounds(const char* payloadBinDir) {
    static std::mutex mu;
    static MapBoundsPack pack;
    static DWORD lastCheckMs = 0;
    static std::string binKey;
    static std::filesystem::file_time_type fileMtime{};
    static bool haveMtime = false;

    std::lock_guard<std::mutex> lock(mu);
    const char* bin = payloadBinDir ? payloadBinDir : "";
    const DWORD now = GetTickCount();
    const bool binChanged = (binKey != bin);
    // 已加载也每 ~3s 探一次 mtime，避免 OTA 换表后永久 latch。
    const bool due =
        binChanged || lastCheckMs == 0 || (now - lastCheckMs) >= 3000u;
    if (!due) return pack;
    lastCheckMs = now;
    binKey = bin;

    if (!bin[0]) return pack;
    const std::string path = JoinPath(bin, "dataservice\\map_info.tsv");
    if (!FileExists(path)) {
        // 文件暂缺：保留旧 pack，不把 loaded 打假。
        return pack;
    }

    std::error_code ec;
    const auto mt = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (!ec && haveMtime && !binChanged && pack.loaded && mt == fileMtime) {
        return pack;
    }

    MapBoundsPack next{};
    if (TryLoadMapBoundsPack(bin, next)) {
        pack = std::move(next);
        if (!ec) {
            fileMtime = mt;
            haveMtime = true;
        }
    }
    return pack;
}

bool TryGetOfflineMapVr(const MapBoundsPack& pack, int mapId, MapVrRect* out) {
    if (out) *out = {};
    if (!pack.loaded || mapId <= 0 || !out) return false;
    const auto it = pack.byId.find(mapId);
    if (it == pack.byId.end() || !it->second.valid) return false;
    *out = it->second;
    return true;
}

bool PointInVr(const MapVrRect& vr, float x, float y, int marginPx) {
    if (!vr.valid || !std::isfinite(x) || !std::isfinite(y)) return false;
    const int m = marginPx > 0 ? marginPx : 0;
    const float L = static_cast<float>(vr.left + m);
    const float R = static_cast<float>(vr.right - m);
    const float T = static_cast<float>(vr.top + m);
    const float B = static_cast<float>(vr.bottom - m);
    if (!(L < R && T < B)) return false;
    return x >= L && x <= R && y >= T && y <= B;
}

}  // namespace xcat
