#pragma once

#include <cstdint>

namespace x::ipc {

// Poll user.ini [core] written by the launcher panel; apply invuln / combat / etc.
void PayloadControl_Poll();
// PLAY workers 全部 Init 之后强制再灌一次：settle 期 Apply 会被后续 Init 清掉 desired。
void PayloadControl_ForceApply();
void PayloadControl_PublishSimpleCombat(bool on);
void PayloadControl_PublishFly(bool on);
void PayloadControl_PublishHomePos(int32_t x, int32_t y, int32_t mapId, bool hasMap);

}  // namespace x::ipc
