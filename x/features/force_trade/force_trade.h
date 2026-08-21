#pragma once

// Classic TWMS：强制交易（实验）。
// 主路径：改写 UIUserInfo 人物卡「交易」按钮的等级比较 threshold global
// （imm+global→0 ⇒ level>=0 恒真）。关功能 restore。默认关。
// No GameAssembly .text patch / 不占 HWBP。Server Trade 权威不变。
// 产品 = 经典版；不覆盖右键菜单 / 丢物客户端门。

#include <Windows.h>

namespace x::features::force_trade {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

void Tick(DWORD now);

}  // namespace x::features::force_trade
