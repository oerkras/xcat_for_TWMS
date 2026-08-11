#pragma once

#include <Windows.h>

namespace x::features::autopot::config {

inline constexpr DWORD kCheckIntervalMs = 100;
inline constexpr DWORD kHpCooldownMs = 250;
inline constexpr DWORD kMpCooldownMs = 150;
inline constexpr DWORD kPotEffectDelayMs = 300;
inline constexpr DWORD kEmptyPotBackoffMs = 8000;
// UseRequest 找到药但 qty 未降（游戏 CD / 拒用）：短冷却即可，过长会把残蓝拖死。
inline constexpr DWORD kEmptyUseCooldownMs = 900;
// empty 连败进软退避的最小间隔（可大于 empty-CD，避免 2s 内三连误熔断）。
inline constexpr DWORD kEmptyStreakGapMs = 2500;
// 未绑 / Type≠Item / 包内无药：短退避，避免每 100ms 打主线程 FKM+扫栏。
inline constexpr DWORD kBindMissBackoffMs = 3000;
inline constexpr int kFailStreakLimit = 3;
inline constexpr DWORD kHealStuckBackoffMs = 15000;
inline constexpr int kHpThresholdPct = 50;
inline constexpr int kMpThresholdPct = 30;
inline constexpr int kHpEmergencyPct = 25;
inline constexpr int kMpEmergencyPct = 15;
inline constexpr DWORD kDualOneDesyncMs = 2000;
inline constexpr DWORD kLandGraceMs = 500;
inline constexpr int kVitalsReadyStreak = 3;

}  // namespace x::features::autopot::config
