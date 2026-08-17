#pragma once

// Classic TWMS 卷轴掉落语音：离线 XiaoxiaoNeural 零件拼接（掉落+部位+属性+卷轴+成功率）。

namespace xcat::sound {

// prefsBinDir = .../XCat_data ；读 dataservice/scroll_voice/{map.tsv,fragments,*.wav}
void LoadScrollVoice(const char* prefsBinDir);

// 叮咚 + 组合播报。无表/缺零件时只叮咚。itemId<=0 只叮咚。
// 新来的打断正在播的口播，队列只留这一条。
bool PlayScrollDropAnnounce(int itemId);

// 烘焙口播。itemId=2070005 播「雷之镖拾取成功」，其余「拾取成功」。缺文件则 false。
// 排队接在当前口播之后，不打断「掉落+部位+…」。
bool PlayPickupSuccessAnnounce(int itemId = 0);

}  // namespace xcat::sound
