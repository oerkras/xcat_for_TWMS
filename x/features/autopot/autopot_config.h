#pragma once

#include <Windows.h>

namespace x::features::autopot::config {

inline constexpr DWORD kCheckIntervalMs = 100;
inline constexpr DWORD kHpCooldownMs = 250;
inline constexpr DWORD kMpCooldownMs = 250;
inline constexpr DWORD kPotEffectDelayMs = 450;
inline constexpr DWORD kEmptyPotBackoffMs = 8000;
// 未绑 / soft 拒 / 包内无药：短退避，避免每 100ms 打主线程 FKM+扫栏。
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
