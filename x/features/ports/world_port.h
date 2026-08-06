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
// MapData.Id（WM._currentMapData@+0x88 → Id@+0x10）；0=未知 / 非图内。
int GetMapId();
// WM._characterId @+0x98；0=未进角/未知。用于 DamageInfo.CharacterId 比对。
uint32_t GetCharacterId();
void* GetCharacterData();   // 转发 player::LocalCharacterData（防漂）
void* GetCharacterStat();   // 转发 player::LocalCharacterStat
int64_t ReadMoney();        // 转发 player::ReadMoney；失败 -1
void* GetItemSlotList(int invType);  // 转发 player::GetItemSlotList

}  // namespace x::features::ports::world
