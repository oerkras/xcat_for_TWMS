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
void SetQuizWorldPaused(bool paused); // quiz 意图；实际硬闸 = quiz|following|ui（见 Refresh）

}  // namespace x::features::auto_lie::anti_macro_follower
