#pragma once
// rest_mp_accel — 实验：加速回蓝 tick（Classic TWMS）
//
// 按间隔写满 WM+0x17C / +0x180（最初有效的快回蓝路径）。无坐椅门控。
// 间隔用户自调；过密会踢。BIN：logs/rest_mp_accel.log。禁止 GA .text / E9。

namespace x {
namespace features {
namespace rest_mp_accel {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();

void SetIntervalMs(unsigned ms);
unsigned IntervalMs();

}  // namespace rest_mp_accel
}  // namespace features
}  // namespace x
