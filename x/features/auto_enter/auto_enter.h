#pragma once
#include <Windows.h>
#include <cstdint>

namespace x {
namespace features {
namespace auto_enter {

// Login auto-enter: world (panel) → random non-full channel → char slot → confirm.
// UI-semantic calls only; shared main_thread_pump (no GA .text).
void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetDesired(bool on, int32_t worldId, const char* worldName, uint32_t charSlot);
bool IsDesired();

// Soft-relogin / 踢线回登录后：若自动进仍开着，从 Done/Failed 拉回 Idle 重跑选区选角。
// why=="soft_login" 时开快轨：Connected/点区/选频/离频/选角 settle 压到最短、
// **仅该轮**优先粘 sticky（不可用则候选池随机，不就近/不偏人少）；
// 频道 UI 已武装到 sticky 则跳过 SelectChannel 直 GoWorld。
// worker 可调；下一拍 Tick 会进 WaitWorldList。
void RequestRestart(const char* why);

// 记下「当前应粘回」的 1-based 频道（与 UI ch.N / auto_enter Pick 同口径）。
// channel_hop 成功后调用；软重连 PickSticky 只在 softFast 且 sticky 仍空闲时消费。
void NoteStickyChannel(int channelId1Based, const char* why);
// 当前 sticky（UI ch.N）；未设置返回 0。
int StickyChannel1Based();

// 当前是否停在 Failed（软重进可据此早退，不必空等到 play-ready 超时）。
bool IsFailed();

// 选角链路已收尾（Done）。软重进用来发现「Done 了但迟迟不 play-ready」的卡死，
// 避免空等到 reenter_timeout（dcaf08：Done@01:16:21 → 仍 playReady=0 直到 01:18:11 fail）。
bool IsDone();

// soft 泵采样：分区列表是否真有条目（或频道页已挂选中世界可续进）。
// 仅指针非空不算 ready（BIN 21:44：world/ch 壳在、WorldItems=0 → 卡大厅）。
struct SoftHallCtx {
    int ok = 0;              // 泵上采完
    int worldUi = 0;
    int channelUi = 0;
    int worldItems = 0;      // UILoginWorld.WorldItems.Count
    int selectedWorld = 0;   // 频道页 selectedWorld 非空
    int ready = 0;           // worldItems>0 || (channelUi && selectedWorld)
};
void SoftHallSampleOnPump(void* user);  // user = SoftHallCtx*

// softFast 卡在 WaitWorldList 且 WorldItems 一直空：超过 minAgeMs 则 true（软重连早 soft cycle）。
bool IsWorldItemsStarve(DWORD minAgeMs);

}  // namespace auto_enter
}  // namespace features
}  // namespace x
