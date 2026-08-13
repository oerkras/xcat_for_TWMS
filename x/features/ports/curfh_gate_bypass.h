#pragma once

// Classic TWMS — 实验：地面门旁路（≠ 站立伪装）。
//
// 站立伪装（ground_spoof）：出刀瞬间写 VecCtrl.CurFh(+0x28)，骗过读台判定。
// 本模块：改 GameAssembly .text，让 Magic / Shoot / Prepare 在 CurFh==null 时
// 仍走「有台」CONT 边（jnz→jmp；Prepare 的 setnz→mov dl,1），不种台。
//
// IDA（imagebase 0x7ff848c80000）锚点：
//   TryDoingMagicAttack  jnz @ RVA 0x1091E42
//   TryDoingShootAttack  jnz @ RVA 0x10599F0
//   DoActiveSkillPrepare setnz @ RVA 0x10B353B
// Melee 只看 LadderOrRope(+0x40)，本旁路不碰。
//
// 默认关；仅实验 TAB。
// ★ 故意不灭 grap MemoryCrc.RpmScan：只改 GA 三处判空，接受完整性格可能扫到脏页。

namespace x::features::ports::curfh_gate_bypass {

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

}  // namespace x::features::ports::curfh_gate_bypass
