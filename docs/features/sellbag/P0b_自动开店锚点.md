# sellbag P0b — 自动开店 / NPC 对话链锚点（Classic TWMS）

> **产品**：新枫之谷：经典版 · **不是**枫星  
> **对照**：CMS `dump.cs` 仅作枚举命名；实现与 RVA 以本仓 TW dump / IDA 为准。  
> **真源**：`DumpRestoredData`（C 档看链）+ `Dumps/runtime/GameAssembly.dll.i64` + CMS 枚举对照。

## 链路总览

```
靠近杂货 NPC
  → UserLocal.TalkToNpc(Npc)          // 或 OnFuncKey(BasicAction=5, NpcTalk=54)
  → C→S UserSelectNpc (=64)
  → （可选）ScriptManager 对话 / AskMenu
  → C→S UserScriptMessageAnswer (=66)     // 选「商店」等菜单项时
  → S→C OpRecv_015D (0x15D=349)
  → UIShopDialog.InPacket 开店
  → ShopReady=true → 既有 UserShopRequest(=67) 卖出
```

## 已钉（铁证）

| 项 | 值 | 证据 |
|---|---|---|
| C→S 选 NPC | TW `ClientPacket` 槽 **64** ≡ CMS `UserSelectNpc` | TW dump L1067363；CMS L1083835；与 65/66/67 连续对齐 |
| C→S 脚本应答 | 槽 **66** ≡ CMS `UserScriptMessageAnswer` | 同上 |
| C→S 商店操作 | 槽 **67** ≡ CMS `UserShopRequest` | P0a 已钉 |
| S→C 开店 UI | `OpRecv_015D` → `UIShopDialog` InPacket @ RVA `0x5380E0` | IDA xref：`OpRecv_015D` → `sub_7FFB170780E0` |
| 对话键 | `FkmType.BasicActionNpcTalk = 54`（`FuncType BasicAction=5`） | DumpRestored C；与普攻 52 同路 `OnFuncKey` |
| 找 NPC | `NpcPool.FindNpc` / `FindNpcByTemplateID` / `GetNpc` | RVA `0xF89E50` / `0xF8C290` / `0xF88D20` |
| 开对话 CALL | `UserLocal.TalkToNpc(Npc)` RVA **`0x1080080`** | dump 方法序 + CMS 同名；IDA 已改名 `UserLocal_TalkToNpc` |
| NpcObjectId | `Npc+0x78` | DumpRestored 字段；TalkToNpc 内 `mov edx,[rdi+78h]` |
| 玩家坐标 | `FieldActorBase+0x64`（`Vector2`） | 与 `mob_pool_port` / `player_combat` 一致 |

### UserSelectNpc 包形（从 TalkToNpc 反汇编还原）

```
opcode = *(u16*)word_7FFB1D384B04 XOR 0x48D9   // 静态期读到 0x4899^0x48D9 = 0x40 = 64
OutPacket.Create(64)
Encode4(Npc.NpcObjectId)                   // RVA Encode(int)=0x1CC5CB0
Encode(Vector2 playerPos)                     // RVA Encode(Vector2)=0x1CC68C0；取自 self+0x64
NetworkManager.Send / SessionTcpLayer
```

## DumpRestoredData 角色对照

| 符号 | 档 | 用途 |
|---|---|---|
| `ScriptManager.Type.AskMenu / AskSelectMenu / Say / AskYesNo` | C | 对话 UI 类型枚举；菜单应答走 66 号包 |
| `JoyStickKey.NpcTalk=4` | C | 摇杆侧「对话」 |
| `Npc.TalkToNpc` 候选 | C 序位 | `ce2c9566…` @ `0xF84A60`（ShowQuestList 大函数旁）；**优先调 UserLocal 侧** |
| `GetSellSlot` / `FriendItem.InShop` | — | 仍为误伤，勿用 |

## 实现策略（本轮）

1. **首选**：`NpcPool` 按 `grocery` 种子 template ID 找 NPC → `UserLocal.TalkToNpc`（RVA `0x1080080`）；找不到再近距扫。  
2. **备用**：`OnFuncKey(5,54)`（`TryNpcTalkFuncKey`）——站在 NPC 旁也能触发选中。  
3. **菜单**：`UIUtilDialogEx`（类哈希 `d517fc58…`）  
   - `Type=List`：扫 `List<string>@0xE0` 文案含「商店/交易/買賣…」→ `SelectMenu(0x786F20)` + `OnClickBtOk(0x78F680)`（走官方脚本应答，含 66，不手组包）。  
   - `Type=Say/YesNo`：仅这两种点确定推进（勿对未知 type 盲点）。  
   - `TickOpeningShop` 以 `kMenuConfirmMs` 节流，避免每 tick 连点 Say。  
4. **寻店**：同 hops 偏好 `potion` 标签杂货店（少菜单、更常直接开店）。  
5. **验收**：到店图后自动 Talk → 菜单自动点 → `ShopReady` → 卖出。
6. **回程前关店**：`buttonExit.Press` → `SetRet` → `Close`；仍挡操作则
   `GameObject.SetActive(false)`。`ShopReady` 要求 `activeSelf`（FindAll 残留不算开）。
   关不死 FailTrip。BIN：假关后角色锁操作 / `ok-close-lag` ×8。

## 注意

- TalkToNpc / OpRecv 体均有 CF 平坦化；以 **RVA + 包形 + xref** 为锚，不依赖反编译可读性。  
- `Create` 的 opcode 在二进制里 XOR 混淆，运行时以 **枚举槽 64** / 静态字还原为准，不要硬编码混淆常数进产品逻辑。  
- 远程店 `UserRemoteShopOpenRequest=65` 与本链无关。
