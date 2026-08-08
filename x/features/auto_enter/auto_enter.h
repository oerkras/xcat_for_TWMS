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
// **仅该轮**优先粘 sticky；频道 UI 已武装到 sticky 则跳过 SelectChannel 直 GoWorld。
// worker 可调；下一拍 Tick 会进 WaitWorldList。
void RequestRestart(const char* why);

// 记下「当前应粘回」的 1-based 频道（与 UI ch.N / auto_enter Pick 同口径）。
// channel_hop 成功后调用；软重连 PickSticky 只在 softFast 时消费。
void NoteStickyChannel(int channelId1Based, const char* why);

// 当前是否停在 Failed（软重进可据此早退，不必空等到 play-ready 超时）。
bool IsFailed();

// 选角链路已收尾（Done）。软重进用来发现「Done 了但迟迟不 play-ready」的卡死，
// 避免空等到 reenter_timeout（dcaf08：Done@01:16:21 → 仍 playReady=0 直到 01:18:11 fail）。
bool IsDone();

}  // namespace auto_enter
}  // namespace features
}  // namespace x
