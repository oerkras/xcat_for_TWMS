#pragma once

// Classic TWMS — 实验：地面门旁路（≠ 站立伪装）。
//
// 站立伪装（ground_spoof）：出刀瞬间写 VecCtrl.CurFh(+0x28)，骗过读台判定。
// 本模块：改 GameAssembly .text，让 Magic / Shoot / Prepare 在 CurFh==null 时
// 仍走「有台」CONT 边，不种台。
//
// IDA（imagebase 0x7ffd60830000 · 08-20 dump）锚点：
//   TryDoingMagicAttack @0x10AB6D0：User+0x50 -> VecCtrl+0x28 后
//     cmovnz rax,rcx @ RVA 0x10B18CD（48 0F 45 C1 -> 48 8B C1 90）
//   TryDoingShootAttack @0x1072800：同上
//     cmovnz rax,r12 @ RVA 0x107977E（49 0F 45 C4 -> 49 8B C4 90）
//   DoActiveSkillPrepare @0x10D0320：同上
//     jnz +7 @ RVA 0x10D2B5A（75 07 -> EB 07）
// 旧 Prepare setnz @0x10B34A7 是 cmp r10d,eax 的 0F 95 C2，不是 CurFh 门，已弃。
// Melee 只看 LadderOrRope(+0x40)，本旁路不碰。
//
// 默认关；仅实验 TAB。
// ★ 故意不灭 grap MemoryCrc.RpmScan：只改 GA 三处判空，接受完整性格可能扫到脏页。

namespace x::features::ports::curfh_gate_bypass {

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

}  // namespace x::features::ports::curfh_gate_bypass
