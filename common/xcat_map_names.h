#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace xcat {

struct MapNamesPack {
    bool loaded = false;
    // MapData.Id →「街道名·地图名」（缺一侧则只留有的）
    std::unordered_map<int, std::string> labelById;
    // 9 位 map key → 繁中 mapName（travel 路由）
    std::unordered_map<std::string, std::string> nameByKey;
    // 繁中 mapName → 9 位 key（重名保留首次）
    std::unordered_map<std::string, std::string> keyByName;
    // 繁中简介/说明 → 9 位 key（世界地图 Spot 偶发只给 desc）
    std::unordered_map<std::string, std::string> keyByDesc;
};

std::string MapNamesPadKey(const std::string& raw);

// 读 dataservice/map_names.tsv；失败返回 false（调用方可稍后重试）。
bool TryLoadMapNamesPack(const char* payloadBinDir, MapNamesPack& out);

// 进程内共享缓存：失败不永久 latch，限频重试（约 3s）。
const MapNamesPack& GetSharedMapNames(const char* payloadBinDir);

// Id>0 命中 label；未命中返回「圖{id}」；id<=0 返回空。
std::string MapNamesLabelById(const MapNamesPack& pack, int mapId);

// 解析 goto 查询：图号 / 短名精确 / 简介精确 / 短名子串（仅 query 较短时）。
// 返回 9 位 key；失败空。
std::string MapNamesResolveQuery(const MapNamesPack& pack, const std::string& raw);

// 只认图号 / 短名精确 / 简介精确，**不做子串**。世界地图空 Spot 兜底用，防点到「冰」误撞。
std::string MapNamesResolveExact(const MapNamesPack& pack, const std::string& raw);

// labelById 是否有这条图号（排除「圖{id}」占位）。
bool MapNamesHasId(const MapNamesPack& pack, int mapId);

// UTF-8 安全截断写入（保证 dst 以 '\\0' 结尾且不切断多字节字符）。
void CopyUtf8Truncate(char* dst, size_t dstCap, const char* src);

}  // namespace xcat
