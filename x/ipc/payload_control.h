#pragma once

namespace x::ipc {

// Poll user.ini [core] written by the launcher panel; apply fly / invuln.
void PayloadControl_Poll();
// Persist current desired flags (F10 / future hotkeys) so the panel can sync.
void PayloadControl_PublishInvuln(bool on);
void PayloadControl_PublishFly(bool on);

}  // namespace x::ipc
