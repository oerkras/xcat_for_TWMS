#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace xcat {

struct WorldNamesPack {
    bool loaded = false;
    // _Center2 → 菇菇寶貝；菇菇寶貝 → _Center2（重名保留首次）
    std::unordered_map<std::string, std::string> displayByKey;
    std::unordered_map<std::string, std::string> keyByDisplay;
};

// 面板预填：dataservice/world_names.tsv 里可解析的 _CenterN（N≥1）。
struct WorldNameCenterEntry {
    int32_t worldId = 0;
    char displayName[64]{};
};

bool LoadWorldNamesPack(const char* payloadBinDir, WorldNamesPack& out);
const WorldNamesPack& GetSharedWorldNames(const char* payloadBinDir);

// key（_CenterN）→ 繁中显示名；未命中返回 ""
const char* WorldNameLookupDisplay(const WorldNamesPack& pack, const char* keyOrName);

// 繁中显示名 → key；未命中返回 ""
const char* WorldNameLookupKey(const WorldNamesPack& pack, const char* displayOrKey);

// 缓存/面板用：优先返回繁中；已是繁中或未知则原样
std::string WorldNamePreferDisplay(const WorldNamesPack& pack, const char* raw);

// 自动进匹配：_Center2 ↔ 菇菇寶貝 ↔ 大小写不敏感相等
bool WorldNameEquals(const WorldNamesPack& pack, const char* a, const char* b);

// 枚举 TSV 中的 _CenterN（跳过裸 _Center）；按 worldId 升序写入，返回条数。
uint32_t WorldNamesListCenters(const WorldNamesPack& pack, WorldNameCenterEntry* out, uint32_t maxOut);

}  // namespace xcat
