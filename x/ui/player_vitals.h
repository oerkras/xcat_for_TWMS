#pragma once
// Shared player vitals for Classic TWMS (titlebar + autopot + timed_keys + buffs).
// 真源唯一：WorldManager→CharacterData→CharacterStat；BasicStat 取 WM。
// 字段防漂移：dump 哈希 → field_get_offset（hint fallback）。

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

void* LocalCharacterData();   // WM→CharacterData*
void* LocalCharacterStat();   // WM→CD→CharacterStat*
void* LocalMyUser();          // WM→MyUser*（hash 防漂）
size_t OffWmMyUser();         // WM.MyUser 偏移
size_t OffWmCharacterData();  // WM→CharacterData*（hash 防漂）
size_t OffCdCharacterStat();  // CharacterData→CharacterStat*（hash 防漂）
// CharacterStat.money；失败 -1（字段走 hash 防漂）。
int64_t ReadMoney();
// CharacterData.ItemSlots[invType] → List<ItemSlotBase>*；invType: 1=equip 2=consume 4=etc…
void* GetItemSlotList(int invType);

// 槽位字段偏移（meta 解析后；dump fallback：ItemId@0x10 / nNumber@0x28）
size_t OffSlotItemId();
size_t OffSlotBundleNumber();

// CharacterData 技能字典偏移（hash 防漂；dump fallback 0x50/0x58/0x68/0x70）
size_t OffSkillRecord();
size_t OffSkillRecordEx();
size_t OffSkillCooltime();
size_t OffSkillCoolTimeOver();
void* LocalSecondaryStat();  // WM→SecondaryStat*

void ClearBind();
bool HasBind();

// Ready latch: 单次 trusted sample 即置位（level≥1, mhp>0, not 0/0, not dead）。
// 防登录误触靠调用方 IsPlayReady / land grace，不靠多拍 streak。
void NoteSample(const Vitals& v, DWORD now);
void ClearReadyLatch();
bool IsReadyLatched();
int HpPct(const Vitals& v);
int MpPct(const Vitals& v);
bool IsDead(const Vitals& v);

}  // namespace x::ui::player
