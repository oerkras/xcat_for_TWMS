#pragma once
// Classic TWMS — 遇人策略（对照枫星 UX；执行走 UserPool + channel_hop，非 Reload）.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::features::encounter {

enum class State : unsigned {
    Idle = 0,
    Watching,    // 已 pause，未勾换频或未满确认窗
    Confirming,  // 勾了换频，累计持续有人
    Hopping,     // 已请求 channel_hop，宽限中
};

void Init();
void Shutdown();
void StartWorker();
// Signal-only（DllMain DETACH / loader lock）.
void StopWorker();

void SetEnabled(bool on);
void SetStrategies(bool stopCombat, bool reconnect, bool gmEscalate = true,
                   bool stopGather = false);

State GetState();
const char* GetStateName();
int LastOtherCount();
int LastAdminLikeCount();     // JobCategory 8/9
int LastHideSuspectCount();   // avatar 未激活嫌疑（藏人功能开时恒 0）
// 吸怪刚开 / 换图：清掉旧人数并立刻再采，未采到前按「有人」处理。
void InvalidateOccupancy();
void RequestSampleNow();
// 遇人仍钉着 simple_combat pause（channel_hop Fail/OK 结束时勿抢清）
bool HoldsCombatPause();

void Tick(DWORD now);

}  // namespace x::features::encounter
