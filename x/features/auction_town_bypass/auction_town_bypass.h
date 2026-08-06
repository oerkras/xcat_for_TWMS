#pragma once

// Classic TWMS：野外点拍卖按钮的客户端静默绕过（仅客户端）。
// 零 .text：开启时维持 Field/WM MapDataInfo
//   - IsTown(+0x50)=1
//   - Option(+0x5C) 仅清/还原 0x10（IsUnableToMigrate）
// 不 patch GA .text / 不 INLINE HOOK / 不占 HWBP。
// Worker 独占 gBak 写回；SetEnabled 只改开关位。
// 默认关：野外迁移常被服端断线，挂机「守护模式」会当踢线干净重拉。
// 真源：WorldManager.SendMigrateToGlobalMarketRequest RVA 0xDDD620（2026-08-04）。

#include <Windows.h>

namespace x::features::auction_town_bypass {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();

void Tick(DWORD now);

}  // namespace x::features::auction_town_bypass
