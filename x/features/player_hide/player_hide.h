#pragma once
// Classic TWMS — 隐藏同图其他玩家（AvatarRoot + Slot14 过滤远程伤字 + ShowSkill*）.
// 对照：枫星 maplecat/player_hide + damage_hide/effect_hide；本仓用 MethodInfo 换钩，不搬 .text ret.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace x::features::player_hide {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
int LastHiddenCount();

void Tick(DWORD now);

}  // namespace x::features::player_hide
