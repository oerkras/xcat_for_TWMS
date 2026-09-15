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

// 基础四维 + 未分配 AP。独立读路径：不要塞进 Read()（autopot/标题栏热路径）。
struct BaseApStats {
    uint32_t characterId = 0;
    int16_t str = 0;
    int16_t dex = 0;
    int16_t intel = 0;
    int16_t luk = 0;
    int16_t ap = 0;
    int16_t job = 0;
    bool ok = false;
};

// 未分配 SP + 等级/职业。独立读路径：不要塞进 Read()。
struct BaseSpStats {
    uint32_t characterId = 0;
    int level = 0;
    int job = 0;
    int16_t sp = 0;
    bool ok = false;
};

void Init();
void Shutdown();

// WM→CharacterData→CharacterStat（+ BasicStat）。无 FindAll，可在 worker 调。
bool Read(Vitals& out);

// 仅 auto_stat 等低频模块调用。worker 纯内存读；不含装备加成。
bool ReadBaseApStats(BaseApStats& out);
bool ReadBaseSpStats(BaseSpStats& out);

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

// CharacterData.ItemSlots[nTI] 直下标。GetItemSlotPos 参数 = dump.cs TDI 2039：
//   -1 None · 0 Equip · 1 Consume · 2 Install · 3 Etc · 4 另栏 · 5 Cash · 6 Count
// GetItemSlotList 种子解出合法 nTI 为 [0,6)。6 不是背包栏。
// BIN：Consume=1（GetItemSlotPos 药水）；Cash=5（GetItem_Ex `cmp ebx,5` → Equipped2@+0x30）。
// GetItem 特殊路径：0=Equipped 穿戴，5=Cash。商店 UITab 的 1/2/4 不是这套，见 shop_port。
namespace item_type {
constexpr int Equip = 0;
constexpr int Consume = 1;
constexpr int Install = 2;
constexpr int Etc = 3;
constexpr int Cash = 5;
}  // namespace item_type
static_assert(item_type::Consume == 1);
static_assert(item_type::Cash == 5);

void* GetItemSlotList(int invType);

// 槽位字段偏移（meta 解析后；dump fallback：ItemId@0x10 / nNumber@0x30；nPOS@0x28）
size_t OffSlotItemId();
size_t OffSlotBundleNumber();

// CharacterData 技能字典偏移（hash 防漂；dump fallback 0x50/0x58/0x60/0x68/0x70）
size_t OffSkillRecord();
size_t OffSkillRecordEx();
size_t OffSkillMasterLevel();  // Dict<int,int>；满级封顶
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
