#pragma once

namespace x {
namespace features {
namespace kick_sniff {

// Data-plane only (no GameAssembly .text patch). Polls SessionTcpLayer → Session
// for SessionState + _pendingErrorCode, and dumps a snapshot on disconnect / on demand.
// Call-edge: MethodInfo swap (via=MI) + HWBP DR execute on Close/CloseSocket/OnDisconnect/
// CloseSession (via=HWBP). No GameAssembly .text patch.
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

// Call from fly AbortSession / any crash path — appends a snapshot to kick.log.
void DumpNow(const char* why);

// Last observed values (0 / -1 if unknown).
int LastPendingErrorCode();
int LastSessionState();
bool SawDisconnect();

}  // namespace kick_sniff
}  // namespace features
}  // namespace x
