#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace attack_accel {

// 攻击加速（SetDesired）：清忙锁 LocalUser+0x118=-1（字段哈希防漂移，hint 0x118）。
// 出刀频率看面板「间隔」/ simpleCombatAttackIntervalMs（默认 50，下限 1）。
// 非「技能无 CD」。禁止 GA .text hook。
//
// 攻速槽（SetBoosterDesired，独立开关，默认关）：写 SecondaryStat.nBooster_@0xBC = -8
//   并按游戏钟续 tBooster_@0xC4。与清忙锁**刻意不共用开关** —— attackAccel 会顺带下发
//   animBusyOverride=0 / immediateUp，挂在一起就量不出 booster 的净效果。
//   0.1.36(无 booster) 与 0.1.37(有 booster) 实测出刀中位同为 63/64ms，说明清忙锁开着时
//   引擎那条延迟根本不是瓶颈；booster 单开（不碰忙锁）才是它真正的用武场景。
//
// 实验·砍动作层倒计时（默认关，独立开关，不改动上述加速语义）：
//   周期把 User+0x120/0x128 指向的 layer+0x14 置 0 → 动画帧尽快推进（偏视觉，易 whiff）。
//
// 实验·跳过 PrepareActionLayer（默认关，独立开关；数据面改 LocalUser 虚表槽，禁 E9）：
//   防漂移：方法哈希 → FindMethodCached(RVA→kind) → 扫 VirtualInvokeData；hint Slot32 仅末级。
//   仅落地武装后才跳过攻击类 Prepare；action==6 Idle 永远透传。
//   跳过时立刻 +0x118=-1 并改调 Idle Prepare（无攻击层则 Slot14 永不解锁，会卡刀）。
//   SetAttackAction 仍可能事后写 actionIdx → worker 在 skipArmed 时周期清忙锁兜底。
// remount 2026-08-04：Prepare 0xFDE830/0x1251E30；AbsSpeed 种子阈 0 已验。
void Init();
void Shutdown();
void StartWorker();
void StopWorker();
void SetDesired(bool on);
bool IsDesired();
void SetBoosterDesired(bool on);
bool IsBoosterDesired();
void SetCutLayerDesired(bool on);
bool IsCutLayerDesired();
void SetSkipPrepareDesired(bool on);
bool IsSkipPrepareDesired();

}  // namespace attack_accel
}  // namespace features
}  // namespace x
