#pragma once

#include <cstdint>

namespace x {
namespace features {
namespace kick_sniff {

// Data-plane only (no GameAssembly .text patch). Polls SessionTcpLayer → Session
// for SessionState + _pendingErrorCode, and dumps a snapshot on disconnect / on demand.
// Diagnostic probes default OFF（守护/换频只依赖轮询 disconnectSeq）:
//   KICK_CALL_EDGE=1 → MethodInfo swap Close/Disconnect/OnDisconnect/set_SessionState (via=MI)
//   KICK_HWBP=1      → DR execute/write on call-edge + SessionState (via=HWBP)
//   KICK_SEND=1      → DR on SendPacket → send.log
// No GameAssembly .text patch.
//
// Offset truth table (TW ≠ CMS): docs/features/kick_sniff/断线错误码.md
//   TW Session: pendingError@0x40, List@0x58, SessionState@0x60
//   Do NOT copy CMS Session List@0x50 / State@0x58.
//
// Also keeps a ring of newly observed S→C InPackets (recvList + NM queue) to help
// distinguish server kick notices vs local self-disconnect when the edge fires.
void Init();
void Shutdown();
void StartWorker();
void StopWorker();

// Append a snapshot to kick.log (crash / teardown / on-demand).
void DumpNow(const char* why);

// Last observed values (0 / -1 if unknown).
int LastPendingErrorCode();
int LastSessionState();
bool SawDisconnect();
// Monotonic: bumps on each Disconnecting/Disconnected edge (for launcher clean relaunch).
uint32_t DisconnectSeq();

}  // namespace kick_sniff
}  // namespace features
}  // namespace x
