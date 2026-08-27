#pragma once
// titlebar — Win32 标题栏显示角色 vitals + 金/经/物值/MP/蓝瓶每分钟（Classic TWMS）
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

// 标题栏环形窗当前金/经每分钟；valid=false 时探活勿覆盖。charName 与 vitals 对齐才可上报。
struct CachedRates {
    bool valid = false;
    double expPerMin = 0.0;
    double mesoPerMin = 0.0;
    char charName[64]{};
};
CachedRates GetCachedRates();

}  // namespace titlebar
}  // namespace features
}  // namespace x
