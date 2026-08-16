#pragma once
// map_attack_port — Classic TWMS · 全图攻击
//
// P2：FindHit 入参 Rect 换成本图 foothold AABB（xywh float）。不抬 maxCount。
// 普攻官方 mc=1 → 一刀仍一只，但这只可从全图盒里挑。产品=经典版，不是枫星。

#include <cstdint>

namespace x::features::ports::map_attack {

void Init();
void Shutdown();

void SetEnabled(bool on);
bool IsEnabled();

}  // namespace x::features::ports::map_attack
