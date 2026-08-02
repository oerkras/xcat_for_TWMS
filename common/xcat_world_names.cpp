#include "xcat_world_names.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace xcat {
namespace {

bool ReadFileUtf8(const std::string& path, std::string& out) {
    std::ifstream f{std::filesystem::path(path), std::ios::binary};
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

void TrimLine(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s.erase(0, i);
}

void SplitLines(const std::string& raw, std::vector<std::string>& lines) {
    lines.clear();
    size_t pos = 0;
    while (pos <= raw.size()) {
        const size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        TrimLine(line);
        if (!line.empty()) lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

void SplitTab(const std::string& line, std::vector<std::string>& cols) {
    cols.clear();
    size_t pos = 0;
    while (pos <= line.size()) {
        const size_t tab = line.find('\t', pos);
        cols.push_back(line.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
        if (tab == std::string::npos) break;
        pos = tab + 1;
    }
}

std::string JoinPath(const std::string& dir, const char* file) {
    if (dir.empty()) return file ? file : "";
    std::string out = dir;
    if (out.back() != '\\' && out.back() != '/') out += '\\';
    out += file ? file : "";
    return out;
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

std::string ResolveDataServiceDir(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return "dataservice\\";
    return JoinPath(payloadBinDir, "dataservice");
}

}  // namespace

bool LoadWorldNamesPack(const char* payloadBinDir, WorldNamesPack& out) {
    out = {};
    const std::string path = JoinPath(ResolveDataServiceDir(payloadBinDir), "world_names.tsv");
    if (!FileExists(path)) return false;

    std::string raw;
    if (!ReadFileUtf8(path, raw)) return false;

    std::vector<std::string> lines;
    std::vector<std::string> cols;
    SplitLines(raw, lines);
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        SplitTab(line, cols);
        if (cols.size() < 2 || cols[0].empty() || cols[1].empty()) continue;
        const std::string& key = cols[0];
        const std::string& name = cols[1];
        if (out.displayByKey.find(key) == out.displayByKey.end()) out.displayByKey[key] = name;
        if (out.keyByDisplay.find(name) == out.keyByDisplay.end()) out.keyByDisplay[name] = key;
    }
    out.loaded = !out.displayByKey.empty();
    return out.loaded;
}

const WorldNamesPack& GetSharedWorldNames(const char* payloadBinDir) {
    static WorldNamesPack s_pack;
    static std::once_flag s_once;
    std::call_once(s_once, [&] { LoadWorldNamesPack(payloadBinDir, s_pack); });
    return s_pack;
}

const char* WorldNameLookupDisplay(const WorldNamesPack& pack, const char* keyOrName) {
    if (!keyOrName || !keyOrName[0] || !pack.loaded) return "";
    const auto it = pack.displayByKey.find(keyOrName);
    return it != pack.displayByKey.end() ? it->second.c_str() : "";
}

const char* WorldNameLookupKey(const WorldNamesPack& pack, const char* displayOrKey) {
    if (!displayOrKey || !displayOrKey[0] || !pack.loaded) return "";
    const auto it = pack.keyByDisplay.find(displayOrKey);
    return it != pack.keyByDisplay.end() ? it->second.c_str() : "";
}

std::string WorldNamePreferDisplay(const WorldNamesPack& pack, const char* raw) {
    if (!raw || !raw[0]) return {};
    const char* disp = WorldNameLookupDisplay(pack, raw);
    if (disp && disp[0]) return disp;
    return raw;
}

bool WorldNameEquals(const WorldNamesPack& pack, const char* a, const char* b) {
    if (!a || !a[0] || !b || !b[0]) return false;
    if (_stricmp(a, b) == 0) return true;

    const char* da = WorldNameLookupDisplay(pack, a);
    const char* db = WorldNameLookupDisplay(pack, b);
    if (da && da[0] && _stricmp(da, b) == 0) return true;
    if (db && db[0] && _stricmp(db, a) == 0) return true;
    if (da && da[0] && db && db[0] && _stricmp(da, db) == 0) return true;

    const char* ka = WorldNameLookupKey(pack, a);
    const char* kb = WorldNameLookupKey(pack, b);
    if (ka && ka[0] && _stricmp(ka, b) == 0) return true;
    if (kb && kb[0] && _stricmp(kb, a) == 0) return true;
    if (ka && ka[0] && kb && kb[0] && _stricmp(ka, kb) == 0) return true;
    return false;
}

}  // namespace xcat
