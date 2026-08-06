#pragma once
// 本图活动边界：仅本图 foothold AABB。
// 离线 VR 已停用（不可靠；真软重载见 teleport P0d fill_slim）。
// 产品=经典版 TWMS。用于 fill 落点闸；不读运行时 MapData.Info（可选后续挂）。

#include <cstdint>

namespace x::features::ports::map_bounds {

// fill/teleport 无 fh 贴地：相对 FH 外包向内缩，避免贴边飞出。
// 已 Snap 种台（plantFh≠0）的路径用 margin=0：FH AABB 外沿台面的门/落点合法，不可再内缩误杀。
constexpr int kLandMarginPx = 24;

struct Rect {
    bool ok = false;
    const char* src = "";  // "fh" | ""
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// mapId：传当前图；≤0 时尝试 world::GetMapId()。
bool QueryPlayBounds(int mapId, Rect* out);

// marginPx：向内缩（默认 kLandMarginPx）。无边界数据时返回 true（不误杀；台上 Snap 仍是主闸）。
bool PointInPlayBounds(float x, float y, int mapId, int marginPx = kLandMarginPx);

// 换图 / FH 缓存重建后清 AABB 缓存（可选；Query 按 mapId 也会错位失效）。
void InvalidateFhAabbCache();

}  // namespace x::features::ports::map_bounds
