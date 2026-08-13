#pragma once
// 经典版 / TWMS 采证工具（调试专用）。
//
// 对 MovePath.Flush(OutPacket, bool bFly, MovePath oPath) 下 inline abs-jmp 钩，
// 把每次 C→S UserMove 上报的 MoveElem 列表（Attr/X/Y/Vx/Vy/MoveAction/Fh/…）dump
// 到 logs\movepath_flush.log。用途：land_miss 被踢时对齐 kick.log 时间线，实锤看清
// 「我们上报的落点/fh 到底畸形在哪」——把 inset 从经验值升级成引擎口径。
//
// 红线：本仓禁止常驻 inline hook。此钩默认关；UI 勾选「采证上报包」只有在游戏进程
// 已带 XCAT_ALLOW_TEXT_PATCH=1 时才真下钩，否则 SetEnabled 拒绝并记一条 WARN。
// 关开关或探针卸载即还原 .text。
// install/remove 一律在 Unity pump 线程上做（Flush 的执行线程），避免改字节时另一线程正执行该处。
namespace x::features::movepath_flush_probe {

// 幂等。true=下钩采证，false=还原。由 payload_control ApplyControl 依 UI 开关调用。
void SetEnabled(bool on);

// 当前是否已挂钩（.text 已被 abs-jmp 覆写）。
bool IsActive();

// 探针卸载时还原 .text。DETACH 路径调用一次。
void Shutdown();

}  // namespace x::features::movepath_flush_probe
