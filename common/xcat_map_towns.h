#pragma once
// 离线 map_info.tsv 的 town=1 真源（挂机图禁记 / 守护主城暂停 / 回城卷落点候选）。
// 禁止用 mapId%1000000==0 冒充主城——野外入口（如 107000000 沼澤地Ⅰ）也会整除。

#include <cstdint>

namespace xcat {

// payloadBinDir = XCat_data 根（其下 dataservice/map_info.tsv）。
// 未加载到表或 mapId 不在表中 → false（不当城镇，避免误禁记挂机图）。
bool IsMapInfoTown(const char* payloadBinDir, int mapId);

}  // namespace xcat
