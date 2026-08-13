#pragma once
// simple_combat — Classic TWMS 自动打怪（状态机）
//
// Idle → Acquire → [MoveTo→Impact|human] → Aim → Firing → Recover → …
// 位移：Impact 贴怪（默认，同 F6 Impact）> 拟人走路。fill+Doing 已废。
// Impact 与出刀互斥；F5 / 面板启停。

#include <Windows.h>
#include <cstdint>

namespace x::features::simple_combat {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();
// F5 勾选且未被 HardPause：真正会出刀。AutoSupply 等硬暂停后 IsEnabled 仍为 true，
// 赶路互斥应看本函数，勿只看 IsEnabled（否则开趟 RequestGoto 会被拒）。
bool IsFarmingActive();

void SetAttackIntervalMs(uint32_t ms);
// 状态机 worker 心跳 ms（面板「TICK值」）；下限 5。
void SetTickIntervalMs(uint32_t ms);
void SetSmartInterval(bool on);
void SetClusterPriority(bool on);
bool IsClusterPriority();
// 打中换怪：本角色确认命中同一 oid N 次后改打攻击盒外最近活怪；空刀/他人伤害不计；活怪 < 3 停刀。
void SetHitRotateEnabled(bool on);
bool IsHitRotateEnabled();
void SetHitRotateN(uint32_t n);
uint32_t HitRotateN();
void SetTeleportEnabled(bool on);  // 强制关：fill+Doing 战斗回落已禁用
// Impact 贴怪（默认开）：近战直升机——旋翼环持续托举悬停在怪旁，空中出刀。
// 优先于拟人。需无敌；交战期间自动挂 fh-ban（无怪超宽限则卸掉落地）。
void SetImpactApproachEnabled(bool on);
bool IsImpactApproachEnabled();
// 空中贴怪防抖（钉点 + 旋翼到位软悬停）。关=安全回退到每拍跟理想点 + 旧 90ms 律。
void SetAntiJitterEnabled(bool on);
bool IsAntiJitterEnabled();
// 防贴脸退避（LiveStep）：站距 X/Y 内有任何怪（含锁定目标）就把旋翼站位点沿 X 推开。
// 需同时满足「自定义站距开 + 空中贴怪开 + 有锁」，任一不满足即静默不生效，行为与关闭时一致。
// 关闭是唯一总闸：关掉后所有退避代码不执行，站位点回到 mx ± standOff。
void SetAntiHugEnabled(bool on);
bool IsAntiHugEnabled();
// 飞行速度倍率（百分比，100 = 基准 1.0X）。只缩放旋翼各档的意图上限；
// 作动器上限与防坠闸不跟随（理由见 heli_rotor.h 的 SetSpeedScale）。
void SetFlySpeedPct(unsigned pct);
unsigned FlySpeedPct();
// 拟人位移：同层走路贴近；仅当 Impact 贴怪关时生效。
void SetHumanWalkEnabled(bool on);
bool IsHumanWalkEnabled();
void SetLiveStepEnabled(bool on);
bool IsLiveStepEnabled();
void SetTeleportParams(uint32_t minDx, uint32_t standOff, uint32_t cooldownMs, uint32_t maxHop);
// F5 空中贴怪的**自定义站距**（远程职业用）。与上面 SetTeleportParams 的 standOff 无关：
// 那个还乘进 InHitBand 等地面判定，这个只喂直升机悬停点与它自己的闸门。
// custom=false → 用内置近战最优值（kHeliStandOffPx / kHeliLiftPx，实测最优，别乱调）。
// custom=true  → x/y 原样生效，且出刀闸与到位判据随之放大；命中率由用户自行负责。
// y 带符号：+Y 向上 ⇒ 正数 = 站在怪上方。
void SetStandOffParams(bool custom, uint32_t x, int32_t y);
// 加速秒杀早切：maxHp=0 关闭此道；其余见 common/xcat_payload_control.h 默认值。
void SetOneshotParams(uint32_t maxHp, uint32_t minBumps, uint32_t minFires, uint32_t minLagMs,
                      uint32_t foxFillGapMs);
// 硬暂停持有者（可并存）：任一置位 → GoIdle；勿再用单 bool 互踩。
enum class HardPauseHolder : uint32_t {
    ChannelHop = 1u << 0,
    Encounter = 1u << 1,
    AutoLie = 1u << 2,
    AutoSupply = 1u << 3,
    // 空中换图（回城卷 / 手动回城 / 贴门过图）：进图后同测谎落台，站稳即自清。
    MapArrive = 1u << 4,
};
void SetHardPause(HardPauseHolder holder, bool on);
// 硬闸安全落台进行中（测谎 / 自动补给 / 遇人 / 进图）：旋翼飞近可站台再卸禁挂台。
// 补给用卷/赶路前应等本函数为 false，避免图底 freefall→重载。
bool IsSafeLandActive();
// 主动请求同款安全落台（开店前 / 赶路到站后仍悬空）。Travel 活跃时 no-op（由 Travel settle 托空）。
// 已有落台则重钉本图落点；否则 Begin + MapArrive（无其它硬闸持有者时靠 MapArrive 保闸）。
void RequestSafeLand(const char* why);
// 兼容旧调用：映射到 HardPauseHolder::AutoLie（仅 auto_lie 仍走此入口时）。
// 新代码请用 SetHardPause。
void SetExternalPause(bool on);
// 可重叠的短暂停战（buffs / timed_keys）：成对 Acquire/Release，勿与硬暂停混用语义。
void AcquireExternalPause();
void ReleaseExternalPause();
// 外部动作（BUFF DoActive / 定时键）应 defer 的途中窗：
// 仅 Settling | 换图 arm。不含 MoveTo / post-native quiet——贴怪 hop 期间若也挡，
// ExternalPause 永远进不去，定时键/BUFF 会 defer 到死（BIN 0.1.40）。
// 注：现行 SettleMs*=0 时 TP 后进 Aim 不进 Settling，故运行时几乎只剩 map arm。
bool IsTeleportTransit();
// 吸物时分复用：未挂机恒 true；挂机中仅 Firing 为 false（Aim/Recover 也吸）。
// MoveTo（含拟人）/Settling/Acquire/Idle 放行。pet_loot 据此开门；勿与 IsTeleportTransit 混用。
bool IsLootPulseActive();
// 脉冲代数：pet_loot 边沿检测用（进 Aim/Recover/Settling / 离开 Firing 时递增）。
uint32_t LootPulseGeneration();
// 多发：普攻 OnFuncKey 真正成功时再武装空刀观察（勿在 TryCast 排程时记账）。
void NotifyMultiNormalAttackFired();
void ResetForMapChange();

// 以下原生瞬移入口已禁用（封禁风险）；调用只记日志。
void RequestNativeTeleportToRandomMob();
void RequestTeleportToRandomMob();
void RequestNativeTeleportCall();

// 踢号压测已禁用（同 fill+Doing）。
void RequestTeleportKickStress();
void RequestTeleportKickStressFine();
void RequestTeleportKickStressFine10();
void RequestTeleportKickStressLocal();
void StopTeleportKickStress();
bool IsTeleportKickStressActive();

void SetHighValueLootUrgent(bool on);
// 高价值紧急拾取中：IsLootPulseActive 恒 true，并短暂停刀（ExternalPause）。
bool IsHighValueLootUrgent();

void Tick(DWORD nowMs);

}  // namespace x::features::simple_combat
