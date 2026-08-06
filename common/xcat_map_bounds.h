#pragma once
// 离线地图 VR*（WZ info → dataservice/map_info.tsv）解析库。
// **产品落点闸已停用 VR**（见 map_bounds_port：仅 FH AABB；BIN 107000402 过紧 VR 误杀地板）。
// 本模块保留给 dump / 表热载 / 对照；勿再接到 PointInPlayBounds。

#include <cstdint>
#include <unordered_map>

namespace xcat {

struct MapVrRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool valid = false;  // left<right ∧ top<bottom（MS：Y 向下增大）
};

struct MapBoundsPack {
    bool loaded = false;
    std::unordered_map<int, MapVrRect> byId;
};

// 读 dataservice/map_info.tsv 尾列 VRTop/VRLeft/VRBottom/VRRight（旧表无列则 byId 空）。
bool TryLoadMapBoundsPack(const char* payloadBinDir, MapBoundsPack& out);

// 进程内共享：失败限频重试；文件 mtime 变化约 3s 内热载（OTA 换表可生效）。
const MapBoundsPack& GetSharedMapBounds(const char* payloadBinDir);

// mapId 命中且矩形合法 → true；缺图/缺 VR → false。
bool TryGetOfflineMapVr(const MapBoundsPack& pack, int mapId, MapVrRect* out);

// 点是否在 VR 内（marginPx≥0 向内缩；越界/无矩形 → false）。
bool PointInVr(const MapVrRect& vr, float x, float y, int marginPx = 0);

}  // namespace xcat
