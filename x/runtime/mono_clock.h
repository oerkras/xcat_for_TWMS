#pragma once
// mono_clock — 与 GetTickCount() 同一时间轴、但分辨率 1ms 的单调钟。
//
// 为什么需要：GetTickCount() 只随系统时钟中断步进，实测步长 15.625ms，且
// timeBeginPeriod(1) 改不动它（simple_combat worker 一直在调，实测量化依旧）。
// 于是任何 `now - last >= gate` 形式的节奏门控都被量化到 15.625ms 的整数倍：
// 面板填 50ms，能满足 >=50 的最小刻度是第 4 步 62.5ms —— 0.1.39 的 429 次出刀间隔里
// 70.2% 落在 62.5±3ms、88.8% 落在 15.625 整数倍 ±3ms（均匀分布零假设仅 38.4%）。
// 这也解释了为何降 tick、nBooster_、skipPrepare 三次实验全部零收益：卡点不在它们身上。
//
// 为什么对齐而不是另起时间轴：出刀链路存在跨模块混用（simple_combat 把 now 传给
// input_port::TickReleases(DWORD)，而 input_port 内部仍用 GetTickCount）。NowMs() 首次
// 调用时自旋到 GetTickCount 的步进边界再锚定 QPC，使两者刻度对齐、残差 <1ms，
// 新旧时间戳可安全混比，模块得以逐个迁移而不必一次性全改。

#include <Windows.h>

namespace x::runtime {

// 自开机起的毫秒数，与 GetTickCount() 同轴，分辨率 1ms。QPC 不可用时退化为 GetTickCount()。
DWORD NowMs();

// 预热锚定（首次锚定需自旋至多一个系统 tick ≈16ms）。在 worker 线程 Init 期调用，
// 避免这段自旋落到 Unity 主线程泵上造成掉帧。可不调，NowMs() 会自行懒锚定。
void WarmUpClock();

}  // namespace x::runtime
