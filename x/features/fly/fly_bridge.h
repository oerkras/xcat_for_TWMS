#pragma once
#include <Windows.h>

namespace twms_fly_impl {
void OpenLogs();
void Log(const char* fmt, ...);
bool BindApis();
bool ResolveActor(bool forceRescan);
void PollF6();
void PollF7Rebind();
void PollF8PreferCamera();
void TickFly();
void WatchBinding();
void ArmFly(bool on);
bool IsFlyOn();
bool IsSessionDead();
float GetFlySpeed();
void SetFlySpeed(float v);
const char* ResolveHow();
void* LocalUserPtr();
void* TransformPtr();
DWORD& TickCountRef();
}
