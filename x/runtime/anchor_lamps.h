#pragma once
// Payload-side anchor MISS 灯：feature 上报 + shape/泵采样 → SHM。

#include "../../common/xcat_anchor_lamps.h"

namespace x::runtime::anchor_lamps {

using xcat::AnchorLampCode;

// id 建议短名：WM / UL / NM / FAC / SA / Pump / FlyCam / TravelSend / …
void Set(const char* id, AnchorLampCode code, const char* detail = nullptr);

// 采样 shape + MainPump，合并已上报 feature 灯，写入 SHM。
void Publish(const char* binDir);

}  // namespace x::runtime::anchor_lamps
