#pragma once

#include "inject_result.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <string>

namespace xcat {

/// LoadLibrary 注入：VirtualAllocEx + WriteProcessMemory +
/// CreateRemoteThread(kernel32!LoadLibraryW)。
InjectResult ClassicLoadLibraryInject(DWORD pid, const std::wstring& absoluteDll);

/// 远程 FreeLibrary(moduleBase)；等待旧模块卸载用。
bool ClassicFreeLibraryRemote(DWORD pid, uintptr_t moduleBase);

/// 按完整路径优先查找已加载模块；返回 0 表示未加载。
uintptr_t ClassicFindLoadedModuleBase(DWORD pid, const std::wstring& absoluteDll);

/// 等待指定模块从目标进程卸载；expectedBase 非 0 时只等待该实例消失。
bool ClassicWaitForModuleUnload(DWORD pid, const std::wstring& absoluteDll,
                                uintptr_t expectedBase, int timeoutMs);

}  // namespace xcat
