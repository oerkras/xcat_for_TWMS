#pragma once
// Shared player vitals for Classic TWMS (titlebar + autopot + timed_keys + buffs).
// 真源唯一：WorldManager→CharacterData→CharacterStat；BasicStat 取 WM+0xE8。

#include <Windows.h>
#include <cstdint>

namespace x::ui::player {

struct Vitals {
    int level = 0;
    int job = 0;
    int hp = 0;
    int mhp = 0;
    int mp = 0;
    int mmp = 0;
    int exp = 0;
    int maxExp = 0;
    int64_t meso = 0;
    char name[64]{};
    bool ok = false;
};

void Init();
void Shutdown();

// WM→CharacterData→CharacterStat（+ BasicStat）。无 FindAll，可在 worker 调。
bool Read(Vitals& out);

// 与 Read 相同（保留旧名；forceRebind 忽略）。
bool ResolveAndRead(Vitals& out, DWORD now, bool forceRebind);

// 与 Read 相同（无栏缓存后等价）。
bool ReadCached(Vitals& out);

void* LocalCharacterStat();

void ClearBind();
bool HasBind();

// Ready latch: consecutive trusted samples (level≥1, mhp>0, not 0/0, not dead).
void NoteSample(const Vitals& v, DWORD now);
void ClearReadyLatch();
bool IsReadyLatched();
int HpPct(const Vitals& v);
int MpPct(const Vitals& v);
bool IsDead(const Vitals& v);

}  // namespace x::ui::player
