#pragma once
// Classic TWMS 自动测谎：TextCaptcha(LLM) + NonFinite(物理光标)。

#include <Windows.h>

#include <cstdint>

namespace x::features::auto_lie {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
void SetDryRun(bool on);  // 干跑：答题流水线照跑，但不 OnOk
bool IsDryRun();

// 调试：NonFinite 题目区域叠层（青框+蓝/红轨迹）；独立于 autoLie 总开关
void SetMouseRegionOverlay(bool on);
bool IsMouseRegionOverlay();

// 基建：不依赖服端测谎 UI
void StartAlarmTest();   // Alarm 音效约 12s（每 3s 一响）
void StartMouseSmoke();  // ~3s SetCursorPos 烟测；不 ClipCursor；硬闸战斗
void StartMouseSim(uint32_t seq);  // 内置对照仓 UV 轨迹 165+330@33Hz 模拟答题

void Tick(DWORD now);

// 知识题 / 轨迹题面板开着、正在跟光标、轨迹模拟中，或谓词快照已过期。
// 快照过期时 IsNonFiniteOpen 会谎称没开；加点 / 加技能点必须让路。
bool IsBusy();

}  // namespace x::features::auto_lie
