#pragma once

// Classic TWMS — 实验：地面门旁路（≠ 站立伪装）。
//
// 站立伪装（ground_spoof）：出刀瞬间写 VecCtrl.CurFh(+0x28)，骗过读台判定。
// 本模块：改 GameAssembly .text，让 Magic / Shoot / Prepare 在 CurFh==null 时
// 仍走「有台」CONT 边，不种台。
//
// IDA（imagebase 0x7FFF43ED0000 · 09-17 dump）锚点：
//   TryDoingMagicAttack @0x113E730：User+0x50 @0x11446FA -> VecCtrl+0x28 @0x114471B
//     cmovnz @0x1144730（CurFh==null → FAIL 0x114482C）；FAIL 边沿 test al; jnz+7
//     @ RVA 0x1144843（75 07 -> EB 07 跳过 FAIL 槽）
//   TryDoingShootAttack @0x1105190：[reg+0xB8]+0x28 后 test rdx; jnz+7
//     @ RVA 0x111039A（75 07 -> EB 07）
//   含 xor al,1 的 Prepare @0x1161E50：call 0x111FCD0（读 +0x28）后
//     xor al,1 @ RVA 0x1162E68（34 01 -> 31 C0；解出 cmp 常量 2 → 强制 CONT）
// 旧 Prepare jnz+7 @0x10D2B3A / setnz @0x10B34A7 已弃。
// Melee 只看 LadderOrRope(+0x40)，本旁路不碰。
//
// 默认关；仅实验 TAB。
// ★ 故意不灭 grap MemoryCrc.RpmScan：只改 GA 三处判空，接受完整性格可能扫到脏页。

namespace x::features::ports::curfh_gate_bypass {

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

}  // namespace x::features::ports::curfh_gate_bypass
