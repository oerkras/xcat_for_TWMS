#pragma once
// fly — Classic TWMS 鼠标飞
// 策略：flyMode 0=点击飞(A) / 1=跟随飞(B)；F6/面板武装。
// A/B：fill+Doing hop，flyHopCdMs 控间隔，不钉台。

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

// 0=点击飞(A) 1=跟随飞(B)
void SetMode(unsigned mode);
unsigned GetMode();

void SetHopCdMs(unsigned ms);
unsigned GetHopCdMs();

}  // namespace x::features::fly
