#pragma once
// Classic TWMS 自动测谎：TextCaptcha(LLM) + NonFinite(物理光标)。

#include <Windows.h>

namespace x::features::auto_lie {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
void SetDryRun(bool on);  // 干跑：答题流水线照跑，但不 OnOk
bool IsDryRun();

// 基建：不依赖服端测谎 UI
void StartAlarmTest();   // Alarm 音效约 12s（每 3s 一响）
void StartMouseSmoke();  // ClipCursor+SetCursorPos 小方框约 3s

void Tick(DWORD now);

bool IsBusy();

}  // namespace x::features::auto_lie
