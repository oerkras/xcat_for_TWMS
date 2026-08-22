#pragma once
// Classic TWMS · 吸怪平行检测族 BIN 探针 v2（检测头 + 父函数 + GetInt thunk）。
// 默认关（不脏 GA .text）。开：环境变量 MOB_INSPECT_PROBE=1。
// 开了之后：abs-jmp 五个函数头； Rel5 钩调度/分发/tick/wrap（序言不够 12 或
// 紧跟 RIP-rel lea）。签名不对：检测头拒钩，父函数跳过。
// 日志 tag=MobInspect → x.jsonl；与 KickSniff 205 按时间对齐。
// GetInt 只在 MainPump 调专用 thunk（不 FindClass）。钩子里只打点，不碰托管。

namespace x {
namespace features {
namespace ports {
namespace mob_inspect_probe {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

}  // namespace mob_inspect_probe
}  // namespace ports
}  // namespace features
}  // namespace x
