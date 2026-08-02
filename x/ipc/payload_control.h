#pragma once

namespace x::ipc {

// Poll user.ini [core] written by the launcher panel; apply invuln / combat / etc.
void PayloadControl_Poll();
void PayloadControl_PublishSimpleCombat(bool on);
void PayloadControl_PublishFly(bool on);

}  // namespace x::ipc
