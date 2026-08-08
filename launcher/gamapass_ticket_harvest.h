#pragma once

// GamaPass 共用：从已拉起的经典版 cmdline 收 Galaxy 四元组票。

#include "http_beanfun_login.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace msc::launcher {

// 本轮会话起点（略减 2s 时钟偏差），用于过滤残留 NGM/Classic。
FILETIME GamaPassSessionNotBeforeNow();

// 若本轮后新建的 NGM 已在跑，置 sawNgmHint=true 并打日志。返回是否新置位。
bool GamaPassNoteNgmLaunchHint(const FILETIME& sessionNotBefore, bool& sawNgmHint,
                               HttpLoginLogFn log, const wchar_t* logTag);

// 扫描 Maplestory_Classic.exe：cmdline 可解析四元组则返回 ticketFilled。
// matched 仅表示找到进程；票未齐时 ok=false 且 ticketFilled=false（继续等）。
HttpLoginResult GamaPassTryHarvestClassicTicket(const FILETIME& sessionNotBefore,
                                                bool& sawNgmHint, HttpLoginLogFn log,
                                                const wchar_t* logTag);

}  // namespace msc::launcher
