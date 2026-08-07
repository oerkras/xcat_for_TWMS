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
void SetTeleportEnabled(bool on);  // 强制关：fill+Doing 战斗回落已禁用
// Impact 贴怪（默认开）：近战直升机——旋翼环持续托举悬停在怪旁，空中出刀。
// 优先于拟人。需无敌；交战期间自动挂 fh-ban（无怪超宽限则卸掉落地）。
void SetImpactApproachEnabled(bool on);
bool IsImpactApproachEnabled();
// 飞行速度倍率（百分比，100 = 基准 1.0X）。只缩放旋翼各档的意图上限；
// 作动器上限与防坠闸不跟随（理由见 heli_rotor.h 的 SetSpeedScale）。
void SetFlySpeedPct(unsigned pct);
unsigned FlySpeedPct();
// 拟人位移：同层走路贴近；仅当 Impact 贴怪关时生效。
void SetHumanWalkEnabled(bool on);
bool IsHumanWalkEnabled();
void SetLiveStepEnabled(bool on);
bool IsLiveStepEnabled();
void SetTeleportParams(uint32_t minDx, uint32_t standOff, uint32_t cooldownMs, uint32_t maxHop,
                       uint32_t crossLayerFillGateMs, uint32_t fillBudgetPx);
// 加速秒杀早切：maxHp=0 关闭此道；其余见 common/xcat_payload_control.h 默认值。
void SetOneshotParams(uint32_t maxHp, uint32_t minBumps, uint32_t minFires, uint32_t minLagMs,
                      uint32_t foxFillGapMs);
// 硬暂停持有者（可并存）：任一置位 → GoIdle；勿再用单 bool 互踩。
enum class HardPauseHolder : uint32_t {
    ChannelHop = 1u << 0,
    Encounter = 1u << 1,
    AutoLie = 1u << 2,
    AutoSupply = 1u << 3,
};
void SetHardPause(HardPauseHolder holder, bool on);
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
// 吸物时分复用：未挂机恒 true；挂机中「不出刀就吸」——仅 Aim/Firing/Recover 为 false。
// 拟人 MoveTo / Settling / Acquire / Idle 均放行（charVac 直调官方 Send，不走捡物键）。
// pet_loot 据此开门；勿与 IsTeleportTransit 混用。
bool IsLootPulseActive();
// 脉冲代数：pet_loot 边沿检测用（离开出刀链 / Settling 武装时递增）。
uint32_t LootPulseGeneration();
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

void Tick(DWORD nowMs);

}  // namespace x::features::simple_combat
