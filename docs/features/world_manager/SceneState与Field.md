# SceneState 门控 + 地图场景（Field）拆解（Classic TWMS）

> 承接 [`字段全表.md`](字段全表.md)。  
> 真源：`DumpRestoredData/dump.cs.restored.B` · 对照 CMS `SceneField`（**偏移有漂**）。  
> **命名**：游戏原名 `Field` = **地图场景**，不是结构体「字段」。本仓玩法门控用 **`IsPlayReady`**；纯场景判断用 `IsInMapScene` / `GetMapScene`。

---

## 0. 结论

| 问题 | 答案 |
|------|------|
| 进图场景读点 | `WM._sceneState @ +0x34 == 3`（游戏名 `Field`） |
| 纯场景 API | `ports::world::IsInMapScene()`（CCU 反选等） |
| **玩法 Tick / 写路径** | **`ports::world::IsPlayReady()`**（地图场景 ∧ WM 存活；新模块默认只调这个） |
| 已接入 IsPlayReady | titlebar / mob_scan / buffs / timed_keys / simple_combat / teleport / autopot / pet_feed |
| 地图场景对象 | `WM+0x58` → `GetMapScene()`（类名仍叫 `Field` / CMS `SceneField`） |
| 怪/掉落 | 独立 Singleton；Root GO 只在地图场景对象上 |

```
ready = IsPlayReady() && MyUser=="MyUser" && CharacterStat 可读
```

---

## 1. SceneState

| 值 | 游戏原名 | 含义 |
|----|----------|------|
| 0 | `None` | 未初始化 |
| 1 | `InterStage` | 切图过渡 |
| 2 | `Login` | 登录选角 |
| 3 | **`Field`** / 别名 **`MapScene`** | **在地图里** |
| 4 | `CashShop` | 商城 |
| 5 | `GlobalMarket` | 自由市场类 |

| TW off | 用途 |
|--------|------|
| `+0x34` | `_sceneState` 主读 |
| `+0x58` | 地图场景对象* → `GetMapScene()` |

`IsPlayReady` / `IsInMapScene` **不替代** MyUser 防串号。

---

## 2. 继承链

`PacketController` → `SceneMap` → **`Field`（地图场景）**

---

## 3–5. SceneMap / Field 字段

见历史表：TW 相对 CMS 约 `+8`/`+0x10`；`UserRoot`/`MobRoot`/`DropRoot`/`PortalRoot` 等是视觉父节点，不是怪池。

---

## 6. 落地状态

1. ✅ `ports::world`：**仓内 WM 唯一解析真源**（`EnsureBound` / `GetWorldManager` / `PeekWorldManager` / `Rebind` / `IsAlive`）+ 场景门控 `IsInMapScene` / `IsPlayReady` / `GetMapScene` / `GetMapSceneKey` / `GetMapId` / `GetCharacterId`
2. ✅ 已收敛到 world_port（禁止各自 FindAll WM）：titlebar / mob_pool / skill / pet / consumable / travel / player_combat / invuln
3. ✅ titlebar：`not_in_map_scene`；mob_scan：`mapKey=`
4. ✅ buffs / timed_keys（门控）
5. ✅ 玩法 Tick / 写路径统一 `IsPlayReady()`：titlebar / mob_scan / buffs / timed_keys / simple_combat / teleport / autopot / pet_feed；**仅** CCU 等纯场景反选保留 `IsInMapScene`
6. ✅ `x/runtime/il2cpp_bind`：GA/IL2CPP 导出公共核；ports + titlebar/invuln/fly/vitals/ccu/auto_enter/kick_sniff + **main_thread_pump** 均已切入；本地 FindClass 仅作一行转发（业务 RVA 仍本地）
7. BIN：看 `WorldPort WM bind … alive=1`；petfeed `not_play_ready`

---

## 7. 非目标

- 不 hook `OnFieldEntered`  
- 不把地图场景 Root 当怪池  
- 不用 SceneState 单独替代 MyUser
- **其它模块不得再 FindAll(WorldManager)**；需要实例只调 `ports::world::GetWorldManager()`
