#pragma once
// Classic TWMS：世界地图 Spot 双击 → 确认框 → 超级赶路（对照枫星 worldmap_marker_travel）。
// 经典版瞬移石走 UIMapTransferDialog，与 UIWorldMap Spot 互不干扰，无需 rockMode。

namespace x::features::worldmap_marker_travel {

void Init();
void Shutdown();

}  // namespace x::features::worldmap_marker_travel
