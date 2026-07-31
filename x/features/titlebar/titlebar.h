#pragma once
// titlebar — Win32 标题栏显示角色 vitals + 金/经每分钟收益（Classic TWMS）
//
// 数据真源：DumpRestoredData B / runtime dump
//   UIStatusBar(af621d…) +0x218 BasicStat* · +0x220 CharacterStat*
//   CharacterStat(fae1aa…) level/job/hp/mp/exp/money …
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
