#pragma once
// Classic TWMS input port — 键脉冲门面。
// 真源：unity_kbd（InputSystem KeyboardState 事件），与拟人走路 / 超级赶路 StickUp 同路。
// 旧 UserLocal.OnKey 泵已停用（主线程嵌套等待会 KEY_FAIL / 卡游戏）。
// 禁止 SendInput。Inject* 须在 worker 调用（持时 Sleep 不占 Unity 主线程）。

#include <Windows.h>
#include <cstdint>

namespace x::features::ports::input {

void Init();
void Shutdown();

// Win32 VK → UnityEngine.InputSystem.Key；未知返回 0 (None)。
int32_t VkToUnityKey(WORD vk);

bool EnsureBound();
bool Ready();

// Worker 同步脉冲：unity_kbd Down → holdMs → Up（对齐 travel CallUpKey）。
// 若已在 Unity 泵线程则内联最短脉冲（不嵌套等泵）；持时 Sleep 仅 worker。
bool InjectKeyHold(WORD vk, DWORD holdMs);

// 直接按 Unity Key 枚举脉冲。
bool InjectUnityKeyHold(int32_t unityKey, DWORD holdMs);

// 同步脉冲无异步松键队列；保留为空操作以兼容旧调用点。
void TickReleases(DWORD nowMs);

void ForceReleaseVk(WORD vk);
void ForceReleaseUnityKey(int32_t unityKey);

}  // namespace x::features::ports::input
