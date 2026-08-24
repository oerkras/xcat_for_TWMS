#pragma once

#include <cstdint>

namespace x::ipc {

// Poll user.ini [core] written by the launcher panel; apply invuln / combat / etc.
void PayloadControl_Poll();
// PLAY workers 全部 Init 之后强制再灌一次：此前 Poll 只灌无敌 + 遇人（战斗/飞/攻速等会跟 Init 抢泵，
// 且 Init 会把 desired 清掉）。ForceApply 才允许完整 Apply。遇人不得等这一步。
void PayloadControl_ForceApply();
void PayloadControl_PublishSimpleCombat(bool on);
void PayloadControl_PublishFly(bool on);
void PayloadControl_PublishHomePos(int32_t x, int32_t y, int32_t mapId, bool hasMap);

}  // namespace x::ipc
