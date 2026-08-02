#pragma once
// titlebar — Win32 标题栏显示角色 vitals + 金/经/物值每分钟收益（Classic TWMS）
//
// 数据真源：DumpRestoredData B / runtime dump
//   WorldManager → CharacterData(+0xE0).CharacterStat(+0x10) / BasicStat(+0xE8)
//   LocalUser 仅接受 GameObject 名 MyUser（或 WM.MyUser@+0x28）；禁止 FindAll first-hit
//   UIStatusBar 仅作 CharacterStat* 对齐校验
//   职业名：Classic 静态繁中表；物值：ItemSlots 增量 × ItemDataManager 卖价
// 标题写入：SendMessageTimeoutW(SMTO_ABORTIFHUNG)，禁止裸 SetWindowTextW。

#include <Windows.h>

namespace x {
namespace features {
namespace titlebar {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetEnabled(bool on);
bool IsEnabled();

}  // namespace titlebar
}  // namespace features
}  // namespace x
