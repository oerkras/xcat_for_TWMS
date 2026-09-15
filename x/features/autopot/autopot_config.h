#pragma once

#include <Windows.h>

namespace x::features::autopot::config {

inline constexpr DWORD kCheckIntervalMs = 100;
inline constexpr DWORD kHpCooldownMs = 250;
inline constexpr DWORD kMpCooldownMs = 150;
inline constexpr DWORD kPotEffectDelayMs = 300;
inline constexpr DWORD kEmptyPotBackoffMs = 8000;  // VerifyPot ineffective 用
// UseRequest qty 未降：单次短 CD（危急不可破）。
inline constexpr DWORD kEmptyUseCooldownMs = 450;
// empty 连败满 streak：只加长 empty-CD，不走 8s 软退避（非危急时 8s 空窗太久）。
inline constexpr DWORD kEmptyStreakCooldownMs = 1500;
inline constexpr DWORD kEmptyStreakGapMs = 1500;
// 未绑 / Type≠Item / 包内无药：短退避，避免每 100ms 打主线程 FKM+扫栏。
inline constexpr DWORD kBindMissBackoffMs = 3000;
inline constexpr int kFailStreakLimit = 3;
inline constexpr DWORD kHealStuckBackoffMs = 15000;
inline constexpr int kHpThresholdPct = 50;
inline constexpr int kMpThresholdPct = 30;
// 跌破门槛后连喝到「门槛 + 带宽」再停（弱药一瓶只回几个百分点，别把人吊在门槛边）。
// 带宽自适应：至少 kHpRefillBandPct；实测一瓶回多少就按「3 瓶的量」拉宽，封顶 Max；目标线不超 Ceil。
inline constexpr int kHpRefillBandPct = 15;
inline constexpr int kHpRefillBandMaxPct = 40;
inline constexpr int kHpRefillCeilPct = 90;
inline constexpr float kHpRefillPotsWorth = 3.f;
inline constexpr int kHpEmergencyPct = 25;
inline constexpr int kMpEmergencyPct = 15;
inline constexpr DWORD kDualOneDesyncMs = 2000;
inline constexpr DWORD kLandGraceMs = 500;
inline constexpr int kVitalsReadyStreak = 3;

}  // namespace x::features::autopot::config
