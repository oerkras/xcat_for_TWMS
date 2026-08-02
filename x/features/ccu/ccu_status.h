#pragma once

#include <Windows.h>
#include <cstdint>

namespace x {
namespace features {
namespace ccu {

struct CcuStatus {
    long long worldChannelOnline = -1;  // -1 = 尚未采到
    int worldChannelCount = 0;
    uint32_t worldChannelAgeSec = 0;
    int fillKnown = 0;       // 已知人数的频道数（login ChannelItem 表）
    int fillPrefer = 0;      // 其中未满且非成人
    uint32_t fillAgeSec = 0; // 填表年龄；0=尚无表
};

// auto_enter PickLeast 喂入：channelId 为 UI/登录 1-based（= hop 0-based+1）
struct ChannelFillRow {
    uint8_t channelId = 0;
    int16_t users = -1;
    int16_t cap = -1;
    uint8_t adult = 0;
};

// channel_hop 选频提示（越小越优先）
enum class ChannelPickHint : int {
    Prefer = 0,  // 已知未满、非成人、未被本会话拒收
    Neutral = 1, // 无表 / 未知
    Avoid = 2,   // 已知满员 / 成人 / 本轮拒收
};

}  // namespace ccu
}  // namespace features
}  // namespace x
