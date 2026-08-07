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
// why=="soft_login" 时开快轨：Connected settle 缩短、**仅该轮**优先粘回 sticky 频道。
// worker 可调；下一拍 Tick 会进 WaitWorldList。
void RequestRestart(const char* why);

// 记下「当前应粘回」的 1-based 频道（与 UI ch.N / auto_enter Pick 同口径）。
// channel_hop 成功后调用；软重连 PickSticky 只在 softFast 时消费。
void NoteStickyChannel(int channelId1Based, const char* why);

// 当前是否停在 Failed（软重进可据此早退，不必空等到 play-ready 超时）。
bool IsFailed();

}  // namespace auto_enter
}  // namespace features
}  // namespace x
