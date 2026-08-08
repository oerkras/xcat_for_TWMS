#pragma once

#include <Windows.h>

namespace x::features::auto_lie::mouse_region_overlay {

// 与原生 mousePosList 上限一致（NonFinite 最多 330 点）。
constexpr int kMaxTrailPoints = 330;

// 外部透明置顶框：面板四角 + 答案轨迹（蓝）+ 实时轨迹（红）+ 光标。
struct Snapshot {
    bool valid = false;
    POINT corners[4]{};
    POINT center{};
    bool plannedValid = false;
    POINT planned{};
    POINT cursor{};
    int answerCount = 0;
    POINT answerTrail[kMaxTrailPoints]{};
    int liveCount = 0;
    POINT liveTrail[kMaxTrailPoints]{};
    char label[96]{};
};

void SetEnabled(bool on);
bool IsEnabled();
void SetSnapshot(const Snapshot& snap);
void Tick(DWORD now);
void Shutdown();

}  // namespace x::features::auto_lie::mouse_region_overlay
