#pragma once

// Classic TWMS / 经典版 — 怪位移「prevpos 举报」内联分支旁路（.text 补丁）。
//
// ★ 2026-08-22 安全禁用：BIN 已证伪（服端自量位移，翻分支救不了断连）。
//   SetEnabled 忽略 on、只卸载；ApplyPayloadControl 永不装。模块文件保留，便于以后回滚。
//   不要再给 GA .text 打脏页。
//
// 产品 = 经典版（Maplestory_Classic.exe）。此处不涉及枫星。
//
// 背景（见 docs/features/security/怪速举报type21与被动插值.md §7）：
//   Mob 类每帧算逐帧欧氏位移 disp = sqrt(dx²+dy²)，与由 [obj+0x44]（记 e）导出的
//   门限 |e|+10 比较：disp ≤ |e|+10 走「正常移动包」路径，disp > |e|+10 走
//   prevpos/异常上报路径。猛拉相逐帧越界 → 逐帧上报，服端跨吸攒计数 → 掉线。
//   该发送体内联在 14KB CFF 大函数 sub_7FFD61789D50（RVA 0xF59D30）里、无独立入口
//   → MinHook 挂不上；且它纯位移自门控、不读任何旗（§7.1），mob_fh_ban 只 hook
//   VecCtrlMob 也压不住这条 Mob 链。故只能内联翻分支。
//
// IDA（imagebase 0x7ffd60830000 · 08-20 dump）锚点：
//   分支决策 @ RVA 0xF5D0DB：cmovz r15, rax（4C 0F 44 F8）
//     此前 lea rax, off_65DEDB28（&正常移动包路径）每次都执行 → rax 恒 = &正常包。
//     把 cmovz 改成无条件 mov r15, rax（4C 8B F8 90，ModRM F8 不变、只 0F44→8B + NOP）
//     → r15 恒 = &正常包 → 每帧每怪永走正常移动包、永不进 prevpos/异常分支。
//
// ⚠ 全局副作用：这是 Mob 更新热路径，补丁对「每帧位移 > |e|+10」的所有怪生效（不止被拽怪）；
//    正常游玩里越界罕见，对吸怪机器人无害。
// ⚠ 未证死的存疑：两条分支都发带新位置的包，服务器是否**独立**量位移掐线尚未证实；
//    若服端独立量，本旁路救不了掉线——只能靠实机开关对比验证。
//
// 默认关；实验/吸怪 TAB 勾选。expect 字节守卫：build 不匹配则拒绝并记 warn，不乱写。
// ★ 故意不灭 grap MemoryCrc.RpmScan：接受完整性校验可能扫到脏页。
// ★ 卸载必须 SetEnabled(false) 还原，否则脏页留在 GA .text。

namespace x::features::ports::mob_prevpos_patch {

void SetEnabled(bool on);
bool IsEnabled();
bool IsInstalled();

}  // namespace x::features::ports::mob_prevpos_patch
