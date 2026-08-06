#pragma once
// Run GC-allocating IL2CPP helpers on Unity main thread only.
// Worker-thread FindAll / TypeGetObject → "Fatal error in GC / Collecting from unknown thread".
//
// Login freeze (default ON at process start): FindAll/TypeGetObject no-op until
// cleared after in-map bind (titlebar vitals / invuln / drop_pool).
// Do NOT clear from auto_enter Done — that races lobby FindAll before play-ready.
// world_port 场景解析必须 bypassFreeze，否则 Titlebar/端口永远解不了冻（死锁）。
// Auto-enter probe runs FindAll directly inside its own main-thread job (bypass freeze).
//
// Map-transit block：!IsPlayReady（InterStage/CashShop/…）时禁托管 FindAll，防换图黑屏被扫对象拖长。
// 由 world_port::IsPlayReady 维护；bypassFreeze 仍放行（仅 WM 冷绑等）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace x::runtime::managed_main {

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnTypeGetObject = void* (*)(void* type);

// Default true at process start. Clear after MyUser / vitals bind; set again on logout.
void SetLoginFreeze(bool on);
bool IsLoginFrozen();

// !PlayReady 时置位；FindAll/TypeGetObject 默认拒绝（bypassFreeze 除外）。
void SetMapTransitBlock(bool on);
bool IsMapTransitBlocked();

// Returns managed array or nullptr. Honors login freeze / map-transit unless bypassFreeze.
void* FindAll(FnFindAll fn, void* typeObj, DWORD timeoutMs = 2000, bool bypassFreeze = false);

// Returns System.Type object or nullptr.
void* TypeGetObject(FnTypeGetObject fn, void* type, DWORD timeoutMs = 2000,
                    bool bypassFreeze = false);

using JobFn = void (*)(void* user);
bool Call(JobFn fn, void* user, DWORD timeoutMs = 2000);

}  // namespace x::runtime::managed_main
