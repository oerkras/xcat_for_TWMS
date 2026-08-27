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
// 软重连 RESULT success：同图不走换图宽限，UserPool 字典残影会 other=1 names=[?]。
// 开一段残影窗并 silent 解开误 pause，避免落地立刻 RequestHop。
void NoteSoftReenterLand(const char* why);
// 吸怪刚开 / 换图：清掉旧人数并立刻再采，未采到前按「有人」处理。
void InvalidateOccupancy();
void RequestSampleNow();
// 遇人仍钉着 simple_combat pause（channel_hop Fail/OK 结束时勿抢清）
bool HoldsCombatPause();
// 战斗热路径：已绑定 UserPool 纯内存 peek。有人则 ApplyHold（停吸 / 先停手 / 换频）。
// 返回 true = 出刀应 GoIdle（先停手 / 换频 / 已在 Hopping）。仅「遇人停吸」仍卸吸怪，返回 false。
// 不 Resolve、不投泵。池未绑 / 无人 / 无刷怪图 → false（调用方 fail-open）。
bool TryHoldFromBoundPeek();

// 赶路开趟前同步冻结遇人：FSM→Idle、silent 解开硬闸/停吸、丢掉排队中的 hop。
// 世界地图关图可能阻塞 1.5s，必须在 Close / RequestGoto 之前调用。
void SuspendNow(const char* why = "travel");

void Tick(DWORD now);

}  // namespace x::features::encounter
