#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace xcat {

struct ItemCatalogRow {
    std::string code;
    std::string category;  // equip / consume / etc
    std::string kind;      // 原表第 5 列（如 POTION）
    std::string subkind;   // 原表第 6 列
    std::string nameZh;
};

struct ItemCatalogPack {
    bool               loaded = false;
    std::vector<ItemCatalogRow> rows;
    std::unordered_map<std::string, std::string> nameByCode;
    // 中文名精确全匹配 → code（重名保留首次；查询侧不接受子串）
    std::unordered_map<std::string, std::string> codeByExactName;
    std::unordered_map<std::string, int>          shopPriceByCode;
    std::unordered_map<std::string, int>          sellPriceByCode;
};

// payloadBinDir = bin/XCat_data/（末尾可有或无 \）
std::string ResolveDataServiceDir(const char* payloadBinDir);

bool LoadItemCatalogPack(const char* payloadBinDir, ItemCatalogPack& out);

// payload 内共享的只读物品目录：首次调用按 payloadBinDir 载入一次（线程安全），之后各 feature
// 复用同一份，避免 titlebar/sellbag/autoshop/overlay 各载一份重复 ~数 MB（4GB VM 多开下可观）。
// 载入失败返回 loaded=false 的空包（各查询函数对空包安全返回 无名/0）。
const ItemCatalogPack& GetSharedItemCatalog(const char* payloadBinDir);

const char* ItemCatalogLookupName(const ItemCatalogPack& pack, const char* code);

// 物品中文名精确全匹配 → code；未命中返回 ""。不做子串/模糊。
const char* ItemCatalogLookupCodeByExactName(const ItemCatalogPack& pack, const char* nameZh);

// 中文名包含 keyword（UTF-8 字节子串）→ 收集 code。
// maxOut=0 表示不限；返回命中总数（可能 > maxOut，此时 out 已截断）。
// 先精确全匹配则只写 1 条并返回 1（避免「紅色藥水」扫出整表藥水）。
size_t ItemCatalogCollectCodesByNameContains(const ItemCatalogPack& pack, const char* keyword,
                                             std::vector<std::string>& out, size_t maxOut = 0);

int ItemCatalogLookupShopPrice(const ItemCatalogPack& pack, const char* code);

int ItemCatalogLookupSellPrice(const ItemCatalogPack& pack, const char* code);

// 把用户输入的关键词列表拆成条目。ASCII 分隔：, ; | 空白；
// 同时把全角 ；，、｜ 与 ideographic space 先换成 ASCII，再切。
// 「箭矢；弩箭矢」必须变成两条，不能整段拿去子串匹配。
void SplitKeywordList(const char* text, std::vector<std::string>& out);

}  // namespace xcat
