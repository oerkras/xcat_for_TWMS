#pragma once

#include <Windows.h>

#include <cstdint>

namespace x::features::auto_lie::anti_macro_follower {

void Init();
void SetEnabled(bool enabled);
void Tick(DWORD now);
void Stop();
void Shutdown();

bool IsFollowing();
bool IsUiVisible();
void SetQuizWorldPaused(bool paused);

}  // namespace x::features::auto_lie::anti_macro_follower
