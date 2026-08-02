#pragma once

#include <Windows.h>

namespace x::features::autopot {

struct Stats {
    int hpPct = 0;
    int mpPct = 0;
    int hpPotionQty = -1;
    int mpPotionQty = -1;
    bool valid = false;
};

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetHpEnabled(bool on);
void SetMpEnabled(bool on);
bool IsHpEnabled();
bool IsMpEnabled();
bool IsEnabled();
void SetHpThresholdPct(int pct);
void SetMpThresholdPct(int pct);
int GetHpThresholdPct();
int GetMpThresholdPct();
Stats GetStats();

}  // namespace x::features::autopot
