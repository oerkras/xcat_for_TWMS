// Classic TWMS — 抓「在 il2cpp 引擎代码内部发生」的首次异常。
// 默认不挂 VEH（链头可枚举，检测面）。排障再开：环境变量 XCAT_FAULT_PROBE=1，
// 或在 DLL 目录 / state 下放 fault_probe.on。state\no_fault_probe 仍强制关。
#pragma once

namespace x::runtime::il2cpp_fault_probe {

void Start();
void Stop();

}  // namespace x::runtime::il2cpp_fault_probe
