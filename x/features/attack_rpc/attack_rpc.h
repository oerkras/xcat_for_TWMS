#pragma once
// attack_rpc — 结算层攻包探针 feature（默认关）
// 启用：环境变量 ATTACK_RPC=1，或运行时 ports::attack_rpc::SetEnabled(true)

namespace x::features::attack_rpc {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

}  // namespace x::features::attack_rpc
