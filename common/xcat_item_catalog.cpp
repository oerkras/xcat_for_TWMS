#include "xcat_item_catalog.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
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

void IndexPack(ItemCatalogPack& pack) {
    pack.nameByCode.clear();
    pack.codeByExactName.clear();
    for (const auto& row : pack.rows) {
        if (row.code.empty() || row.nameZh.empty()) continue;
        pack.nameByCode[row.code] = row.nameZh;
        // 重名：保留首次，避免后写覆盖导致 CODE 漂移
        if (pack.codeByExactName.find(row.nameZh) == pack.codeByExactName.end()) {
            pack.codeByExactName[row.nameZh] = row.code;
        }
    }
}

// 常见异体字：用户输入「紅寳」图鉴为「紅寶」→ 否则 miss=1、黑名单永不生效。
std::string NormalizeZhLookupKey(const char* keyword) {
    std::string s = keyword ? keyword : "";
    auto replaceAll = [](std::string& str, const char* from, const char* to) {
        if (!from || !from[0] || !to) return;
        const size_t fl = std::strlen(from);
        const size_t tl = std::strlen(to);
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, fl, to);
            pos += tl;
        }
    };
    replaceAll(s, "\xE5\xAF\xB3", "\xE5\xAF\xB6");  // 寳 → 寶
    return s;
}

bool LoadCatalogTsv(const std::string& path, ItemCatalogPack& pack) {
    std::string raw;
    if (!ReadFileUtf8(path, raw)) return false;

    std::vector<std::string> lines;
    std::vector<std::string> cols;
    SplitLines(raw, lines);
    pack.rows.clear();
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        SplitTab(line, cols);
        if (cols.size() < 2 || !IsNumericCode(cols[0])) continue;
        ItemCatalogRow row{};
        row.code = cols[0];
        row.category = cols.size() > 1 ? cols[1] : "";
        row.kind = cols.size() > 2 ? cols[2] : "";
        row.subkind = cols.size() > 3 ? cols[3] : "";
        row.nameZh = cols.size() > 4 ? cols[4] : "";
        pack.rows.push_back(std::move(row));
    }
    return !pack.rows.empty();
}

bool LoadPriceTsv(const std::string& path, std::unordered_map<std::string, int>& out) {
    std::string raw;
    if (!ReadFileUtf8(path, raw)) return false;

    std::vector<std::string> lines;
    std::vector<std::string> cols;
    SplitLines(raw, lines);
    out.clear();
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        SplitTab(line, cols);
        if (cols.size() < 2 || !IsNumericCode(cols[0])) continue;
        const int price = atoi(cols[1].c_str());
        if (price > 0) out[cols[0]] = price;
    }
    return !out.empty();
}

}  // namespace

std::string ResolveDataServiceDir(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return "dataservice\\";
    return JoinPath(payloadBinDir, "dataservice");
}

bool LoadItemCatalogPack(const char* payloadBinDir, ItemCatalogPack& out) {
    out = {};
    const std::string ds = ResolveDataServiceDir(payloadBinDir);
    const std::string catalogPath = JoinPath(ds, "item_catalog.tsv");
    const std::string shopPath = JoinPath(ds, "shop_prices.tsv");
    const std::string valuePath = JoinPath(ds, "item_value.tsv");

    if (FileExists(catalogPath) && LoadCatalogTsv(catalogPath, out)) {
        IndexPack(out);
    }
    LoadPriceTsv(shopPath, out.shopPriceByCode);
    LoadPriceTsv(valuePath, out.sellPriceByCode);
    // 卖店价与商店买入价分表；禁止用 shop 填 sell（物值会虚高）。
    out.loaded = !out.rows.empty() || !out.sellPriceByCode.empty() || !out.shopPriceByCode.empty();
    return out.loaded;
}

const ItemCatalogPack& GetSharedItemCatalog(const char* payloadBinDir) {
    static ItemCatalogPack s_pack;
    static std::once_flag s_once;
    std::call_once(s_once, [&] {
        LoadItemCatalogPack(payloadBinDir, s_pack);  // 失败时保持空包，查询函数对空包安全
    });
    return s_pack;
}

const char* ItemCatalogLookupName(const ItemCatalogPack& pack, const char* code) {
    if (!code || !code[0]) return "";
    const auto it = pack.nameByCode.find(code);
    return it != pack.nameByCode.end() ? it->second.c_str() : "";
}

const char* ItemCatalogLookupCodeByExactName(const ItemCatalogPack& pack, const char* nameZh) {
    if (!nameZh || !nameZh[0]) return "";
    auto it = pack.codeByExactName.find(nameZh);
    if (it != pack.codeByExactName.end()) return it->second.c_str();
    const std::string norm = NormalizeZhLookupKey(nameZh);
    if (norm != nameZh) {
        it = pack.codeByExactName.find(norm);
        if (it != pack.codeByExactName.end()) return it->second.c_str();
    }
    return "";
}

size_t ItemCatalogCollectCodesByNameContains(const ItemCatalogPack& pack, const char* keyword,
                                             std::vector<std::string>& out, size_t maxOut) {
    out.clear();
    if (!keyword || !keyword[0] || !pack.loaded) return 0;

    const std::string norm = NormalizeZhLookupKey(keyword);
    const char* keys[2] = {keyword, nullptr};
    int keyN = 1;
    if (norm != keyword) {
        keys[1] = norm.c_str();
        keyN = 2;
    }

    // 黑名单要子串全量（「寶」须含「紅寶殼」）；set 去重避免宽词 O(n²)
    std::unordered_set<std::string> seen;
    size_t total = 0;
    auto pushCode = [&](const std::string& code) {
        if (code.empty()) return;
        ++total;
        if (maxOut != 0 && out.size() >= maxOut) return;
        if (!seen.insert(code).second) return;
        out.push_back(code);
    };

    for (int ki = 0; ki < keyN; ++ki) {
        auto it = pack.codeByExactName.find(keys[ki]);
        if (it != pack.codeByExactName.end() && !it->second.empty()) pushCode(it->second);
    }

    for (const auto& row : pack.rows) {
        bool hit = false;
        for (int ki = 0; ki < keyN; ++ki) {
            if (row.nameZh.find(keys[ki]) != std::string::npos) {
                hit = true;
                break;
            }
        }
        if (!hit) continue;
        pushCode(row.code);
    }
    return total;
}

int ItemCatalogLookupShopPrice(const ItemCatalogPack& pack, const char* code) {
    if (!code || !code[0]) return 0;
    const auto it = pack.shopPriceByCode.find(code);
    return it != pack.shopPriceByCode.end() ? it->second : 0;
}

int ItemCatalogLookupSellPrice(const ItemCatalogPack& pack, const char* code) {
    if (!code || !code[0]) return 0;
    const auto it = pack.sellPriceByCode.find(code);
    return it != pack.sellPriceByCode.end() ? it->second : 0;
}

}  // namespace xcat
