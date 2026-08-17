#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace attack_accel {

// 攻击加速（SetDesired）：清忙锁 LocalUser+0x11C=-1（字段哈希防漂移，hint 0x11C）。
// 出刀频率看面板「间隔」/ simpleCombatAttackIntervalMs（默认 123，下限 1）。
// 非「技能无 CD」。禁止 GA .text hook。
// 用户入口：PayloadControl.attackAccelClearBusy（首页挂机「攻击无CD」）；首页 attackAccel 暂关。
//
// 攻速槽（SetBoosterDesired，独立开关，默认关）：写 SecondaryStat.nBooster_@0xBC = -8
//   并按游戏钟续 tBooster_@0xC4。与清忙锁**刻意不共用开关**。
//   用户入口：kAttackAccelBoosterUserEnabled=false（置灰 + Apply 强制关）；代码保留。
//
// 实验·砍动作层倒计时（默认关，独立开关，不改动上述加速语义）：
//   周期把 User+0x120/0x128 指向的 layer+0x14 置 0 → 动画帧尽快推进（偏视觉，易 whiff）。
//
// 实验·跳过 PrepareActionLayer（默认关，独立开关；数据面改 LocalUser 虚表槽，禁 E9）：
//   防漂移：方法哈希 → FindMethodCached(RVA→kind) → 扫 VirtualInvokeData；hint Slot32 仅末级。
//   仅落地武装后才跳过攻击类 Prepare；action==6 Idle 永远透传。
//   跳过时立刻 +0x11C=-1 并改调 Idle Prepare（无攻击层则 Slot14 永不解锁，会卡刀）。
//   SetAttackAction 仍可能事后写 actionIdx → worker 在 skipArmed 时周期清忙锁兜底。
//
// 实验·A 系 ActionSpeed（SetActionSpeedDesired，默认关）：写 SecondaryStat.nSpeed_@0x84=+40
//   → GetActionSpeed ≈ nSpeed(+80) + Max(40, Dash) → Prepare clamp 到 140（只抬手/动画）。
//   若鞋/Forced 已把 +80 顶到近 140，加完仍被夹死 → 写成功但体感无；要攻速体感用 Party/nBooster_。
//   不写 nSlow_@1BC；CheckByTime 不扫 Speed，服端 002A/002B 仍可覆盖。
//
// 实验·PartyBooster（SetPartyBoosterDesired，默认关）：写 TempStats[4].Value（滑条，默认 -8）
//   → GetAttackSpeedDegree 的 partyBooster 加数（与 nBooster_ 同属 B 系，可叠加）。
//
// 实验·破 degree 下限（SetBreakDegreeFloorDesired，默认关；与 Party 独立）：
//   写 CalcWeaponAttackSpeedTier 独占种子 dword（GA .data，禁 E9）：lo 由滑条设定（默认 -10）。
//   不开本项时引擎仍夹 [2,10]，PB=-8 顶格仍是 deg=2（×0.75）。
//   开启后 PB=-8 可落到 deg<0（如 wpn4+(-8)→-4，×0.375）；lo=-10 时最快 deg=-10 → 延迟×0。
// remount 2026-08-06：Prepare 0xFFD8E0/0x1254B80；字段位移未变，哈希已换；IDB imagebase 0x7ff848c80000。
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
void SetActionSpeedDesired(bool on);
bool IsActionSpeedDesired();
void SetPartyBoosterDesired(bool on);
bool IsPartyBoosterDesired();
// PartyBooster 写入值（越负越快；夹到 xcat::kAttackAccelPartyBoosterValue*）。
void SetPartyBoosterValue(int v);
int PartyBoosterValue();
void SetBreakDegreeFloorDesired(bool on);
bool IsBreakDegreeFloorDesired();
// 破限目标 lo（越负越快；夹到 xcat::kAttackAccelBreakDegreeFloorLo*）。
void SetBreakDegreeFloorLo(int lo);
int BreakDegreeFloorLo();

// 只读引擎动作忙位（LocalUser + ActionBusy，即上面清忙锁写的那个字段）。
// **只读、不写、不 hook、不调引擎函数**——与已停用的 SetDesired(清忙锁写 -1) 是两条路。
//
// 语义（428 份历史 attack_accel.log 实测：-1 占 99.2%，其余为 0/5/6/7/16/17 等小整数）：
//   busy <  0 → 空闲，攻击键会被引擎接受
//   busy >= 0 → 正在放动作（值是动作序号 ActionType），此刻按键会被引擎吞掉
//
// localUser 由调用方传入（simple_combat 的 tick 里已有 CombatCtx），本函数不重复解析，
// 以免在高频出刀路径上再走一遍 QueryCombatCtx。
// 返回 false = 偏移未就绪 / lu 为空 / 读取越界。调用方**必须按「不拦」处理**：
// 读不到时退回旧行为，绝不能让一次读失败演变成永久禁止出刀。
bool QueryActionBusy(void* localUser, int& outBusy);

// 出刀成功后立刻读：busy>=0 时即为当前 ActionType（对照 dump ActionType：
// SwingO1=5…StabTf=21 近战挥砍；Shoot1=22…ShooTf=27 / Shoot6=48 远程射击）。
// 用于多发区分「贴脸挥弓 vs 远程射击」。
bool QueryActionIndex(void* localUser, int& outActionIdx);
bool IsRangedShootAction(int actionIdx);
bool IsMeleeWeaponAction(int actionIdx);

// 多发等短窗清忙锁：只写 ActionBusy=-1，不启攻击加速 worker/开关。
// 调用方须另用间隔/技能 CD 限频，避免服端看出刀过密。
bool ClearActionBusy(void* localUser);

// 当前动作层倒计时（User+0x120/0x128 → layer+0x14），取两层较大值。
// 单位：引擎 delay 计数（Slot14 每 tick 约 -=30）。读失败返回 false。
bool QueryActionLayerDelayRemain(void* localUser, int& outRemain);

// Prepare 写好的整段动作 delay 表加总（ActionLayer+0x20 的 int[]，已含 ActionSpeed 缩放）。
// 单位：引擎 delay 计数。取两层中 sum 较大者。读失败 / 空表返回 false。
bool QueryActionLayerDelaySum(void* localUser, int& outSum);

// 整段动作闸门 ms（多发限频）：对 delay 加总取**上界** ms=sum（只能多不能少；
// 旧 sum×2/3 在 Update 变慢时可能偏短）。须在 Prepare/出刀成功后立刻读层。
bool QueryActionLayerAnimMs(void* localUser, DWORD& outMs);

// 只读当前 ActionSpeed（对齐 GetActionSpeed：nSpeed(+80)+Max(nSpeed_,Dash)；
// nSlow_@1BC!=0 时绝对覆盖；再 clamp [70,140]）。失败返回 false。
bool QueryActionSpeed(void* localUser, int& outSpeed);

// 离线表（dataservice/action_delay_base.tsv + skill_action.tsv）：
// 存 **未缩放** baseDelay 正帧加总；按当前 ActionSpeed 算出 ms。
// 无映射 / 未加载返回 false → 调用方回退实时读层。
bool LookupOfflineSkillAnimMs(void* localUser, int skillId, DWORD& outMs);
bool LookupOfflineActionAnimMs(void* localUser, const char* actionName, DWORD& outMs);
// 只取离线 baseSum（未乘 ActionSpeed）；供缓存后按现速重算。
bool LookupOfflineSkillBaseSum(int skillId, int& outBaseSum);

// delay 单位 ↔ ms / ActionSpeed 缩放（供多发缓存 baseSum）。
DWORD DelayUnitsToAnimMs(int delaySum);
int ScaleDelayByActionSpeed(int baseSum, int actionSpeed);
int UnscaleDelayByActionSpeed(int scaledSum, int actionSpeed);
// busy 墙钟 ≈ 已缩放动画 ms → 反解未乘 ActionSpeed 的 baseSum；失败返回 0。
int AnimMsToBaseSum(DWORD animMs, int actionSpeed);

// 只读有效攻速档 degree：
// clamp(WAS(+15C) + nBooster_ + TempStats[4].Value(PartyBooster), lo, 10)。
// 默认可读 lo=2；BreakDegreeFloor 生效时 lo=滑条值。对齐 (degree+10)/16。诊断用。
bool QueryAttackSpeedDegree(void* localUser, int& outDegree);

// 相对缩放估算：delay ≈ baseMsAtDegree6 × (degree+10)/16（degree=6 → ×1.0）。
// 多发用它把「学到的 degree=6 基准」映射成当前档绝对间隔。
DWORD EstimateDamageDelayScaleMs(void* localUser, DWORD baseMsAtDegree6);

}  // namespace attack_accel
}  // namespace features
}  // namespace x
