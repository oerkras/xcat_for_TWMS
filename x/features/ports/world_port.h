#pragma once
// world_port — Classic TWMS WorldManager 唯一解析真源 + 场景门控（只读）
//
// 其它模块禁止再 FindAll(WorldManager)；一律走 EnsureBound / GetWorldManager。
// IL2CPP 绑定走 x::runtime::il2cpp（禁止本文件再 GetProcAddress 一套）。
//
// 注意命名：游戏里 Field = 地图场景（SceneField），不是结构体「字段」。
// 对外 API 用 MapScene，避免中文「字段」歧义；枚举值 Field 保留游戏原名。
//
// 真源：docs/features/world_manager/SceneState与Field.md
//   WM._sceneState @+0x34 · WM._field @+0x58 · Field.FieldKey @+0x98
// 禁止 INLINE HOOK；不调用游戏写接口。

#include <cstdint>

namespace x::features::ports::world {

enum class SceneState : int {
    None = 0,
    InterStage = 1,
    Login = 2,
    Field = 3,
    MapScene = Field,
    CashShop = 4,
    GlobalMarket = 5,
    Unknown = -1,
};

bool EnsureBound();
void* GetWorldManager();
void* PeekWorldManager();
bool Rebind(bool force = true);
bool IsAlive();
void Invalidate();

SceneState GetSceneState();
// 纯场景判断（含 CCU「非地图才采样」等反选）。玩法 Tick / 写操作请用 IsPlayReady。
bool IsInMapScene();
// 玩法就绪 SSOT：地图场景 ∧ WM 存活。新模块默认只调这个，勿再各自拼门控。
bool IsPlayReady();
void* GetMapScene();
int GetMapSceneKey();
// MapData 指针是否有效。出生图 Field id=0（菇菇村訓練所入口）也算有图；
// 禁止用 GetMapId()==0 当「没进图」——那是合法图号，没图请看 HasMapData。
bool HasMapData();
// MapData.Id（WM._currentMapData@+0x88 → Id@+0x10）。
// 无 MapData → 0（兼容旧调用）；有 MapData 时 0 是出生图，不是哨兵。
int GetMapId();
// CharacterStat.CharacterID@+0x10（战斗/伤害等）；掉落归属请用 GetDropOwnerCharacterId。
uint32_t GetCharacterId();
// Drop.OwnerId 本地真源：BIN 钉 WM+0x114（+0x98 实为 CS 同值，不可用）。
uint32_t GetDropOwnerCharacterId();
// drop_pool 用地上 OwnerId 扫 WM 命中后钉死偏移
void NoteDropOwnerWmFieldOff(size_t off);
void ClearDropOwnerWmFieldOff();
size_t PeekDropOwnerWmFieldOff();
void* GetCharacterData();   // 转发 player::LocalCharacterData（防漂）
void* GetCharacterStat();   // 转发 player::LocalCharacterStat
int64_t ReadMoney();        // 转发 player::ReadMoney；失败 -1
void* GetItemSlotList(int invType);  // 转发 player::GetItemSlotList

// WorldManager.CharacterRegDate（.NET DateTime ticks，低 62 bit）。
// 现行 dump 槽 +0x238；未进角 / 未收到 S2C 395 时返回 false。
// 纯内存读，可 off-pump。发布侧进角后只调一次。
bool ReadCharacterRegDateTicks(int64_t* outTicks);

}  // namespace x::features::ports::world
