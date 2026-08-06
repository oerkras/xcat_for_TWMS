#pragma once
// 只读采证：KeyMacroAnalyzer Put / 句柄检查 vs 我方 Win32 SendInput。
// 开：环境变量 XCAT_KEYMACRO_BIN 缺省或 =1；关：=0。
// 操作：①手按 ←/→ ≥1s ②F5 拟人追怪 HoldWalk ③对照 logs/key_macro_bin.log
//   看 put 是否进账、handle 是否=0/异于 hunt、是否 hit handleChk / hackLog。

#include <Windows.h>

namespace x::features::ports::key_macro_bin {

void Init();
void Shutdown();
bool Enabled();

// attack_input_port 在 SendInput 前后打点（与 Put 时间线对照）。
void NoteOsSendInput(WORD vk, bool down, UINT sent);

}  // namespace x::features::ports::key_macro_bin
