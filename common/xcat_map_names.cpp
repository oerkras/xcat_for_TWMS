#include "xcat_map_names.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace xcat {
namespace {

void TrimTrailing(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
}

void TrimLeading(std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s.erase(0, i);
}

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

bool LooksLikeMapDesc(const std::string& s) {
    // 世界地图常把简介塞进「名」：含句号/逗号且偏长 → 当 desc，不当短名模糊匹配源。
    if (s.size() < 24) return false;
    return s.find("。") != std::string::npos || s.find("，") != std::string::npos ||
           s.find('.') != std::string::npos;
}

}  // namespace

std::string MapNamesPadKey(const std::string& raw) {
    if (raw.empty()) return {};
    bool digits = true;
    for (char c : raw) {
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
    }
    if (!digits) return raw;
    if (raw.size() >= 9) return raw;
    return std::string(9 - raw.size(), '0') + raw;
}

bool TryLoadMapNamesPack(const char* payloadBinDir, MapNamesPack& out) {
    out = {};
    if (!payloadBinDir || !payloadBinDir[0]) return false;
    const std::string path = JoinPath(payloadBinDir, "dataservice\\map_names.tsv");
    if (!FileExists(path)) return false;

    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t t0 = line.find('\t');
        if (t0 == std::string::npos) continue;
        const size_t t1 = line.find('\t', t0 + 1);
        const size_t t2 = line.find('\t', t1 == std::string::npos ? line.size() : t1 + 1);
        const size_t t3 = (t2 == std::string::npos) ? std::string::npos : line.find('\t', t2 + 1);

        std::string code = line.substr(0, t0);
        TrimTrailing(code);
        std::string street;
        std::string mapName;
        std::string desc;
        if (t1 != std::string::npos) {
            street = line.substr(t0 + 1, t1 - (t0 + 1));
            mapName = line.substr(t1 + 1, (t2 == std::string::npos ? line.size() : t2) - (t1 + 1));
            if (t2 != std::string::npos) {
                desc = line.substr(t2 + 1, (t3 == std::string::npos ? line.size() : t3) - (t2 + 1));
            }
        } else {
            street = line.substr(t0 + 1);
        }
        TrimTrailing(street);
        TrimTrailing(mapName);
        TrimTrailing(desc);

        const std::string key = MapNamesPadKey(code);
        if (key.empty()) continue;

        if (!mapName.empty()) {
            out.nameByKey[key] = mapName;
            if (out.keyByName.find(mapName) == out.keyByName.end()) out.keyByName[mapName] = key;
        }
        if (!desc.empty() && out.keyByDesc.find(desc) == out.keyByDesc.end()) {
            out.keyByDesc[desc] = key;
        }

        int mapId = 0;
        try {
            mapId = std::stoi(code);
        } catch (...) {
            mapId = 0;
        }
        if (mapId >= 0 && (!street.empty() || !mapName.empty())) {
            std::string label;
            if (!street.empty() && !mapName.empty())
                label = street + "·" + mapName;
            else if (!mapName.empty())
                label = mapName;
            else
                label = street;
            out.labelById[mapId] = std::move(label);
        }
    }

    out.loaded = !out.labelById.empty() || !out.nameByKey.empty();
    return out.loaded;
}

const MapNamesPack& GetSharedMapNames(const char* payloadBinDir) {
    static std::mutex s_mu;
    static MapNamesPack s_pack;
    static DWORD s_lastTryMs = 0;
    static std::string s_bin;

    std::lock_guard<std::mutex> lock(s_mu);
    const char* bin = payloadBinDir ? payloadBinDir : "";
    if (s_pack.loaded && s_bin == bin) return s_pack;

    const DWORD now = GetTickCount();
    if (s_lastTryMs != 0 && (now - s_lastTryMs) < 3000u && s_bin == bin) return s_pack;

    s_lastTryMs = now;
    s_bin = bin;
    MapNamesPack tmp;
    if (TryLoadMapNamesPack(bin, tmp)) s_pack = std::move(tmp);
    return s_pack;
}

std::string MapNamesLabelById(const MapNamesPack& pack, int mapId) {
    if (mapId < 0) return {};
    const auto it = pack.labelById.find(mapId);
    if (it != pack.labelById.end() && !it->second.empty()) return it->second;
    return std::string("圖") + std::to_string(mapId);
}

std::string MapNamesResolveQuery(const MapNamesPack& pack, const std::string& rawIn) {
    std::string s = rawIn;
    TrimLeading(s);
    TrimTrailing(s);
    if (s.empty() || !pack.loaded) return {};

    bool digits = true;
    for (char c : s) {
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
    }
    if (digits) return MapNamesPadKey(s);

    auto it = pack.keyByName.find(s);
    if (it != pack.keyByName.end()) return it->second;

    auto id = pack.keyByDesc.find(s);
    if (id != pack.keyByDesc.end()) return id->second;

    // 短查询才允许子串（防简介整句误撞）
    if (!LooksLikeMapDesc(s) && s.size() <= 48) {
        for (const auto& kv : pack.keyByName) {
            if (kv.first.find(s) != std::string::npos) return kv.second;
        }
    }
    return {};
}

std::string MapNamesResolveExact(const MapNamesPack& pack, const std::string& rawIn) {
    std::string s = rawIn;
    TrimLeading(s);
    TrimTrailing(s);
    if (s.empty() || !pack.loaded) return {};

    bool digits = true;
    for (char c : s) {
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
    }
    if (digits) {
        const std::string key = MapNamesPadKey(s);
        if (key.empty()) return {};
        int id = 0;
        try {
            id = std::stoi(key);
        } catch (...) {
            id = 0;
        }
        if (id > 0 && MapNamesHasId(pack, id)) return key;
        if (pack.nameByKey.find(key) != pack.nameByKey.end()) return key;
        return {};
    }

    auto it = pack.keyByName.find(s);
    if (it != pack.keyByName.end()) return it->second;
    auto id = pack.keyByDesc.find(s);
    if (id != pack.keyByDesc.end()) return id->second;
    return {};
}

bool MapNamesHasId(const MapNamesPack& pack, int mapId) {
    if (mapId <= 0 || !pack.loaded) return false;
    const auto it = pack.labelById.find(mapId);
    return it != pack.labelById.end() && !it->second.empty();
}

void CopyUtf8Truncate(char* dst, size_t dstCap, const char* src) {
    if (!dst || dstCap == 0) return;
    dst[0] = '\0';
    if (!src || !src[0] || dstCap < 2) return;

    const size_t maxBytes = dstCap - 1;
    size_t di = 0;
    size_t si = 0;
    while (src[si]) {
        const unsigned char c = static_cast<unsigned char>(src[si]);
        size_t charLen = 1;
        if ((c & 0x80u) == 0) {
            charLen = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            charLen = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            charLen = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            charLen = 4;
        } else {
            break;
        }
        if (di + charLen > maxBytes) break;
        bool intact = true;
        for (size_t k = 0; k < charLen; ++k) {
            if (!src[si + k]) {
                intact = false;
                break;
            }
        }
        if (!intact) break;
        for (size_t k = 0; k < charLen; ++k) dst[di + k] = src[si + k];
        di += charLen;
        si += charLen;
    }
    dst[di] = '\0';
}

}  // namespace xcat
