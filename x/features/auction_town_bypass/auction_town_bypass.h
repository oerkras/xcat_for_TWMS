#pragma once

// Classic TWMS：野外点拍卖按钮的客户端静默绕过（仅客户端）。
// 零 .text：开启时维持 Field/WM MapDataInfo
//   - IsTown(+0x50)=1
//   - Option(+0x5C) 仅清/还原 0x10（IsUnableToMigrate）
// 不 patch GA .text / 不 INLINE HOOK / 不占 HWBP。
// Worker 独占 gBak 写回；SetEnabled 只改开关位。
// 调度：换图/未稳住 50ms 快写；已稳住 1s 慢校验（已正确则零写）。
// 不能「仅点击才写」：状态栏直调 migrate，无回调；禁 .text/HWBP。
// 默认开：野外迁移仍可能被服端断线；挂机「守护模式」会当踢线干净重拉——守护时建议关。
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

// 原生 MapDataInfo.IsTown（拍卖绕过强制写 1 时返回备份原值）。
// -1=未知/未进图；0=非主城；1=主城。跨线程只读原子快照（worker Tick 更新）。
int QueryNativeIsTown();

}  // namespace x::features::auction_town_bypass
