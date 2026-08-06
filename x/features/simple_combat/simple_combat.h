#pragma once
// simple_combat — Classic TWMS 站桩自动打怪（状态机重设计）
//
// Idle → Acquire → [MoveTo→Settling] → Aim → Firing → Recover → …
// 瞬移与出刀互斥；F5 / 面板启停。

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
void SetTeleportEnabled(bool on);
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
// 吸物时分复用：未挂机恒 true；挂机中仅换怪/贴怪等待与落地脉冲窗为 true。
// Aim/Firing/Recover 为 false。pet_loot 据此开门；勿与 IsTeleportTransit 混用。
bool IsLootPulseActive();
// 脉冲代数：pet_loot 边沿检测用（Settling 武装 / 从关到开续期时递增）。
uint32_t LootPulseGeneration();
void ResetForMapChange();

// F11 / 测试贴怪：fill+Doing → 随机活怪身边（长距绝对落点）。
void RequestNativeTeleportToRandomMob();
void RequestTeleportToRandomMob();

// 面板「原生CALL」：短距 ~140px fill+Doing。
void RequestNativeTeleportCall();

// 踢号压测：随机贴怪 fill+Doing，间隔由慢→快直到断线；见 combat.log DONE。
void RequestTeleportKickStress();
// 细扫档：50→0ms，步进 5ms，每级 12 跳。
void RequestTeleportKickStressFine();
// 钉地板：30→10ms，步进 5ms，每级 12 跳（不到 5/0，验证 10ms 是否可过）。
void RequestTeleportKickStressFine10();
// 原地短跳：同台 ±120px 来回，CD 扫档对齐 fine0-50（排除远距因素）。
void RequestTeleportKickStressLocal();
void StopTeleportKickStress();
bool IsTeleportKickStressActive();

void Tick(DWORD nowMs);

}  // namespace x::features::simple_combat
