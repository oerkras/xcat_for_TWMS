#pragma once
// 卡死验尸：主泵哑掉超过阈值时，在进程内回溯全部线程的调用栈并落盘。
//
// 为什么必须在进程内做：经典版客户端的进程对象做过加固，外部调试器即使带
// SeDebugPrivilege 也拿不到读权限——cdb 非侵入附加会报 `ReadVirtual() failed
// ... (error == 5)`、`Peb.Ldr is invalid or inaccessible`，`lm` 模块表全空，
// 于是每个线程只剩一个栈顶地址，栈根本走不下去。XCat 自己在进程里，读本进程
// 内存不受这个限制。
//
// 2026-08-09 实测卡死现场（pid 57380）：主线程停在 ntdll!NtWaitForAlertByThreadId
// 且 8 秒内用户态 CPU 时间零增长（22.375s → 22.375s）——在死等一把锁，既不是
// 空转也不是等 IO。要定位锁被谁攥着，只能同时看主线程的完整调用栈，以及那一
// 刻其余线程各自停在哪里。
//
// 输出：bin/XCat_data/logs/hang/hang_<yyyymmdd_hhmmss>.txt，每帧一行
// 「模块名+0xRVA」。GameAssembly 的 RVA 可直接拿去运行时 IDB 里定位函数。

#include <Windows.h>

namespace x::runtime::hang_autopsy {

// 启动看门狗（幂等）。主泵装好并真正 tick 过之前不武装：那之前不 tick 是正常的。
void Start();

// 停止看门狗（幂等）。DETACH 调用，只置标志不 join——绝不在加载器锁上等线程。
void Stop();

// 立刻抓一次，不看主泵状态；reason 写进文件头。返回是否成功落盘。
bool CaptureNow(const char* reason);

}  // namespace x::runtime::hang_autopsy
