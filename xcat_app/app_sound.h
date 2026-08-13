#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace xcat::app::sound {

void Init();
void Shutdown();

void UiClick();
void UiConfirm();
void UiToggle();
void UiError();
void UiShutdownAsync(HWND hwnd, UINT doneMsg);

void LaunchOk();
void LaunchFail();
void IpcReady();
void GameContextReady();
void Notify();
void Alarm();
void RestrictionAlarm();
void ScrollDrop();

}  // namespace xcat::app::sound
