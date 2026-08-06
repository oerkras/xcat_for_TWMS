#pragma once
// Classic TWMS — 引擎帧率锁（Application.targetFrameRate + 关 vSync）。
// 不改显示器硬件刷新率；只锁 Unity 主循环上限，便于高低配对齐打怪节奏。
// 对照：枫星 maplecat/visual_opt（彼为降帧省电；本模块为自定义目标帧率）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::features::frame_lock {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
void SetTargetFps(uint32_t fps);
bool IsEnabled();
uint32_t TargetFps();
int LastAppliedFps();  // 最近一次读回；失败 -1

void Tick(DWORD now);

}  // namespace x::features::frame_lock
