#pragma once
// fly — Classic TWMS 鼠标飞：Impact（SetImpactNext/NockBack）→ MoveElem Attr=2
// flyMode：0=NockBack  1=SetImpactNext
// 武装期：fly_fh_ban 禁挂台；ApplyImpact 必须放行（旁路已拆除）。

namespace x::features::fly {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

bool IsArmed();
void SetArmed(bool on);

// 外部暂停（补给/测谎等）：不改 armed 开关，仅抑制 hop；解除后恢复。
void SetExternalPause(bool on);
bool IsExternallyPaused();

// 0=Impact NockBack  1=Impact SetImpactNext
void SetMode(unsigned mode);
unsigned GetMode();

void SetHopCdMs(unsigned ms);
unsigned GetHopCdMs();

}  // namespace x::features::fly
