#pragma once

// Classic TWMS — 站立伪装：出刀那一瞬把 VecCtrl.CurFh 种回一块合法台，骗过
// 技能派发里的「必须站在台上」判定，出完刀立刻摘掉。
//
// 为什么只种 CurFh（+0x28），不碰 LastFh（+0x30）/ LadderOrRope（+0x40）：
//   IDA 静态分析（GameAssembly runtime IDB，imagebase 0x7FFB16B40000）结论——
//   · DoActiveSkillMeleeAttack / ShootAttack / MagicAttack 本体没有地面门；
//   · TryDoingMagicAttack、DoActiveSkillPrepare 里各有一处 **内联** 读 +0x28；
//   · TryDoingMeleeAttack 只读 +0x40（绳梯），种台反而会踩坏它；
//   · TryDoingMagicAttack 的「在台上」分支会调 VecCtrl.IsUserFlying()（虚表 slot 18），
//     基类实现恒 false 且无子类覆写，所以种了 CurFh 不会被它反手判飞。
//   所以只需要 +0x28 这一处，多写一个字段都是纯风险。
//
// 为什么不和滑翔冲突：
//   OnFuncKey → OnAttack → TryDoing*Attack → DoActiveSkill* 全程同步跑在主线程泵的
//   同一个 job 里，种/摘之间不跨帧，物理（VecCtrl_WorkUpdateActive）永远看不到这块台，
//   不会把角色吸附回台面、不会打断冲量滑翔。禁台钩子（fly_fh_ban）照旧每帧清 fh。
//
// 两条派发路径都要种，缺一条就等于对那条路没开：
//   · 普攻 / 主键：attack_input_port::FireJobOnMain（OnFuncKey 正路）
//   · 技能多发：  skill_port::CastJobFn（DoActiveSkill / Prepare 回退，**不经** OnFuncKey）
//   2026-08-08 实测：只接了前者时，多发的蝸牛術（id=1000）624 次里 602 次
//   reason=prepare_false（96.5%），而同一局主键出的同一个技能 225 刀全被引擎接下。
//
// 种哪块台：起飞时禁台钩子清掉的那一块（NoticeFhObject 记 ID）。只记 ID 不记指针，
// 换图后旧 ID 解析不到就干脆不种（安全降级），绝不留悬垂指针。

#include <cstdint>

namespace x::features::ports::ground_spoof {

void SetEnabled(bool on);
bool IsEnabled();

// 禁台钩子清 CurFh **之前**调用，记下这块台的 ID。物理帧热路径：两次读 + 一次原子写。
void NoticeFhObject(void* fh);

// 派发前后成对调用，必须已在主线程泵上。两个 Plant 逻辑完全相同，只是把判决记进
// **各自的**槽位——攻击的 sp 与技能的 sp 不能互相覆盖，否则两边的取证都读成别人的。
//   PlantForFire      攻击路径用，判决进 FireDebug。
//   PlantForCast      技能路径用，判决进 CastDebug。
//   UnplantAfterFire  两者共用，幂等，未种也必须调（含 __except 路径）。
// 二者不会嵌套：都只在主线程泵上、顺序执行，共用同一组「本次种了什么」现场。
bool PlantForFire(void* localUser);
bool PlantForCast(void* localUser);
void UnplantAfterFire();

// 逐刀取证（combat.log）。任意出参可为空。
//   verdict  1=种了  0=功能关  -1=本来就在台上  -2=禁台未武装
//            -3=没有可用台 ID / 解析不到  -4=VecCtrl 读不到
//   fhId     本刀种下去的台 ID；0=没种
//
// 「这一刀引擎接没接 / 攻击动作 id 是多少」不在这里——那是
// attack_input_port::FireOutcomeDebug 的活，本模块只回答「台种上了没有」。
// （早先这里还回传过出刀前后的 VecCtrl.moveAction，那是**移动姿态**，由物理状态机
//   驱动，种台压根不会动它；拿它当攻击包 action 读会得出「伪装无效」的错误结论。）
void FireDebug(int* verdict, uint32_t* fhId);

// 同上，但取技能施放那一路的槽位（skill_port::CastJobFn）。
// 判读要点：只有 verdict==1 才代表「台确实种上了」。若 verdict==1 而 SkillPort 仍报
// prepare_false，那这次失败就**不是**地面门造成的（技能自身条件不满足），别再往
// 伪装身上赖；若 verdict 是 -2/-3，才是种台这一环没成。
void CastDebug(int* verdict, uint32_t* fhId);

}  // namespace x::features::ports::ground_spoof
