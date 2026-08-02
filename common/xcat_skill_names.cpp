#include "xcat_skill_names.h"

#include "xcat_multiskill_select.h"

#include <cstdio>
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

bool IsNumericCode(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
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

// "0001000" / "1000" → "1000"；全零保留 "0"
std::string NormalizeSkillCode(const char* code) {
    if (!code || !code[0]) return {};
    const char* p = code;
    while (*p == '0' && p[1]) ++p;
    return p;
}

bool LoadNamesTsv(const std::string& path, SkillNamesPack& pack) {
    std::string raw;
    if (!ReadFileUtf8(path, raw)) return false;

    std::vector<std::string> lines;
    std::vector<std::string> cols;
    SplitLines(raw, lines);
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        SplitTab(line, cols);
        if (cols.size() < 2 || !IsNumericCode(cols[0]) || cols[1].empty()) continue;
        const std::string key = NormalizeSkillCode(cols[0].c_str());
        if (key.empty()) continue;
        if (pack.nameByCode.find(key) == pack.nameByCode.end()) pack.nameByCode[key] = cols[1];
    }
    return !pack.nameByCode.empty();
}

bool LoadCatalogTypesTsv(const std::string& path, SkillNamesPack& pack) {
    std::string raw;
    if (!ReadFileUtf8(path, raw)) return false;

    std::vector<std::string> lines;
    std::vector<std::string> cols;
    SplitLines(raw, lines);
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        SplitTab(line, cols);
        // code name job type passive …
        if (cols.size() < 4 || !IsNumericCode(cols[0])) continue;
        const std::string key = NormalizeSkillCode(cols[0].c_str());
        if (key.empty()) continue;
        const int typ = atoi(cols[3].c_str());
        if (typ < 0 || typ > 2) continue;
        if (pack.typeByCode.find(key) == pack.typeByCode.end()) pack.typeByCode[key] = typ;
        // 名表缺行时用 catalog 名补一刀
        if (!cols[1].empty() && pack.nameByCode.find(key) == pack.nameByCode.end()) {
            pack.nameByCode[key] = cols[1];
        }
    }
    return !pack.typeByCode.empty();
}

}  // namespace

bool LoadSkillNamesPack(const char* payloadBinDir, SkillNamesPack& out) {
    out = {};
    const std::string ds = ResolveDataServiceDir(payloadBinDir);
    const std::string namesPath = JoinPath(ds, "skill_names.tsv");
    const std::string catalogPath = JoinPath(ds, "skill_catalog_full.tsv");

    if (FileExists(namesPath)) LoadNamesTsv(namesPath, out);
    if (FileExists(catalogPath)) out.typesLoaded = LoadCatalogTypesTsv(catalogPath, out);

    out.loaded = !out.nameByCode.empty() || out.typesLoaded;
    return out.loaded;
}

const SkillNamesPack& GetSharedSkillNames(const char* payloadBinDir) {
    static SkillNamesPack s_pack;
    static std::once_flag s_once;
    std::call_once(s_once, [&] { LoadSkillNamesPack(payloadBinDir, s_pack); });
    return s_pack;
}

const char* SkillNameLookup(const SkillNamesPack& pack, const char* code) {
    if (!code || !code[0] || pack.nameByCode.empty()) return "";
    const std::string key = NormalizeSkillCode(code);
    if (key.empty()) return "";
    const auto it = pack.nameByCode.find(key);
    return it != pack.nameByCode.end() ? it->second.c_str() : "";
}

const char* SkillNameLookupById(const SkillNamesPack& pack, int skillId) {
    if (skillId <= 0) return "";
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%d", skillId);
    return SkillNameLookup(pack, buf);
}

int SkillCatalogTypeLookup(const SkillNamesPack& pack, const char* code) {
    if (!code || !code[0] || !pack.typesLoaded) return -1;
    const std::string key = NormalizeSkillCode(code);
    if (key.empty()) return -1;
    const auto it = pack.typeByCode.find(key);
    return it != pack.typeByCode.end() ? it->second : -1;
}

bool SkillLooksLikeBuffCandidate(const SkillNamesPack& pack, const char* code,
                                 bool keepIfActiveOrEnabled) {
    if (keepIfActiveOrEnabled) return true;
    if (!code || !code[0]) return false;
    const int typ = SkillCatalogTypeLookup(pack, code);
    if (typ < 0) return true;  // 表外：宁可显示，避免漏续航技
    if (typ == kSkillCatalogTypeSupport) return true;
    return false;  // 攻击 / 被动默认藏
}

bool SkillLooksLikeAttackCandidate(const SkillNamesPack& pack, const char* code,
                                   bool keepIfSelected) {
    if (keepIfSelected) return true;
    if (!code || !code[0]) return false;
    // 普攻是多发清单固定占位（非 numeric skillId）。
    if (IsNormalAttackCode(code)) return true;
    const int typ = SkillCatalogTypeLookup(pack, code);
    if (typ < 0) return true;  // 表外：宁可显示，避免漏攻击技
    return typ == kSkillCatalogTypeAttack;
}

}  // namespace xcat
