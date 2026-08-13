#pragma once

#include <cstdint>

namespace x {
namespace features {
namespace kick_sniff {

// Data-plane only (no GameAssembly .text patch). Polls SessionTcpLayer → Session
// for SessionState + _pendingErrorCode, and dumps a snapshot on disconnect / on demand.
// Diagnostic probes default OFF（守护/换频只依赖轮询 disconnectSeq）:
//   KICK_CALL_EDGE=1 / kick_call_edge.on
//                      → MethodInfo swap Close/Disconnect/OnDisconnect/set_SessionState (via=MI)
//   KICK_HWBP=1 / kick_hwbp.on
//                      → DR：a480 本地拆线 + CloseSession + SessionState write
//   KICK_HWBP=2 / kick_teardown_hwbp.on
//                      → DR：Nm.Disconnect + cs_caller_1CD5570 + CloseSession（状态错乱踢线归因）
//   KICK_SEND=1 / send_probe.on → DR on SendPacket → send.log
//   GALAXY_TOKEN_PROBE / galaxy_token_probe.on → 断线/连上时读 PlayerPrefs Galaxy_*（见 galaxy_token_probe）
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
