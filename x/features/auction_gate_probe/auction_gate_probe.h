#pragma once

// Classic TWMS：一次点官方状态栏拍卖按钮（实验）。
// 主泵 FindAll(UIStatusBar) → OnClickButton(17) → 官方链
// CheckRedAccountRestriction → SendMigrateToGlobalMarketRequest → 0x002E。
// 不写 CharacterStat.level / 建角 DateTime（不破坏战斗）；零 .text / 不 INLINE HOOK。
// 15/24h 仍在官方 migrate 体内；服端 0x002E 权威不变。

#include <Windows.h>

namespace x::features::auction_gate_probe {

void Init();
void Shutdown();

// IPC 脉冲：开短线程 → InvokeAndWait 主泵跑一次。忙则拒绝。
void RequestRun();

}  // namespace x::features::auction_gate_probe
