#pragma once
// 只读采证：真人按 ←/→ 时同步采 KeyPad/锁存/InputX/Ap + MovePath Attr（不驱动位移）。
// 开：环境变量 XCAT_WALK_BIN 缺省或 =1；关：=0。
// 操作：关 F5/拟人 → 进图站平地 → 空闲/按住→/按住← 各 ≥1s；挨打看 aLast/non0
// → 交 logs/keypad_walk_bin.log

namespace x::features::ports::keypad_walk_bin {

void Init();
void Shutdown();
bool Enabled();

}  // namespace x::features::ports::keypad_walk_bin
