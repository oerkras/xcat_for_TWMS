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

// 「这一列脚下有没有地板」——掉落区判据，AABB 无法替代。
//
// AABB 是全图地板取 min/max 的外接矩形，**表达不了矩形内部的空洞**：BIN a69130 里角色沉到
// (-707,-1064)，距 B(-1052) 只剩 12px，`PointInPlayBounds` 判「界内」，0.4s 后就掉出图触发换图
// （102020200→102020300，3s 后被路线拉回）。矩形永远看不见 x=-707 那一列底下是空的。
//
// 这里按 x 逐列实算：取所有横跨该列的地板，在该列上线性插值出高度（斜坡不能拿端点近似），
// 挑出脚下最近的一块。找不到 ⇒ 失去升力就会一路掉出地图 = 掉落区。
// 竖直段（x1==x2）是墙不是地板，不计入。
//
// 坐标：与 foothold / mob / combat.log / FlightState 同一空间，**+Y 向上**（实测，见
// map_bounds_port.cpp 里 FloorBelowLocked 的证据段）。`Rect` 的 top/bottom 是**数值**
// 含义（top=min y、bottom=max y），故 +Y 向上时 **top 才是图底、bottom 才是图顶**——
// 这两个字段名与直觉相反，用之前先回头看这一行。
// 「脚下」= fy ≤ y，最近的一块是其中最大的 fy。
// 无地板数据（含换图瞬间 FH 未 Collect）返回 true，宁可不拦也不误杀。
bool HasFloorBelow(float x, float y, int mapId, float* floorYOut = nullptr);

// 从 x 出发，向左右找最近的「脚下有地板」的列，返回该列 x。用于掉落区自救的水平方向。
// 只给横向目标，不涉及垂直冲量符号——垂直符号历来是事故高发区，不在本判据职责内。
bool NearestFloorColumn(float x, float y, int mapId, float maxScanPx, float* safeXOut);

// 换图 / FH 缓存重建后清 AABB 缓存（可选；Query 按 mapId 也会错位失效）。
void InvalidateFhAabbCache();

}  // namespace x::features::ports::map_bounds
