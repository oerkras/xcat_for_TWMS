#pragma once
// Classic TWMS input port — UserLocal.OnKey (main-thread pump).
// KeyDownTouch CFF 实机 SEH；现行真源 OnKey@0x10198C0，MethodInfo=null。
// 禁止 SendInput。

#include <Windows.h>
#include <cstdint>

namespace x::features::ports::input {

void Init();
void Shutdown();

// Win32 VK → UnityEngine.InputSystem.Key；未知返回 0 (None)。
int32_t VkToUnityKey(WORD vk);

bool EnsureBound();
bool Ready();

// 主线程脉冲：Down → holdMs → Up。失败含：未绑定 / 输入框聚焦 / 无 TargetUser。
bool InjectKeyHold(WORD vk, DWORD holdMs);

// 直接按 Unity Key 枚举脉冲（用于 GetKeyByFunc 解析出的攻击键）。
bool InjectUnityKeyHold(int32_t unityKey, DWORD holdMs);

void TickReleases(DWORD nowMs);

void ForceReleaseVk(WORD vk);
void ForceReleaseUnityKey(int32_t unityKey);

}  // namespace x::features::ports::input
