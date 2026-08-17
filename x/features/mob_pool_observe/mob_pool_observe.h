#pragma once
// mob_pool_observe — 怪物刷新感知增强（独立 FEATURE）
//
// MethodInfo 换针观察 MobPool EnterField / LeaveField；hook 内只唤醒
// mob_scan::RequestImmediateScan。禁止 INLINE HOOK / 不改周期扫描语义。
// 默认关；实验 TAB「怪物刷新感知增强」→ core.mobPoolObserve。

#include <Windows.h>

namespace x::features::mob_pool_observe {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

}  // namespace x::features::mob_pool_observe
