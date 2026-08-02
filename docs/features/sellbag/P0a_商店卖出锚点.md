# sellbag P0a — NPC 商店卖出锚点（Classic TWMS）

> **产品**：新枫之谷：经典版 · **不是**枫星  
> **对照**：枫星 `shop_port` / `sellbag` 仅作行为契约；实现走 Il2Cpp。

## 已钉

| 项 | 值 | 证据 |
|---|---|---|
| UI | `UIShopDialog` | `dump.cs` TypeDefIndex 434；类哈希 `a6a2ea0331ac52556389c1120821204e693ebb9cf87e01cf7e7662a76a91a66` |
| C→S 枚举 | `ClientPacket`（`e59ffc…`）**= 67** | dump L1067366；CMS 名 `UserShopRequest` |
| OutPacket.Create | RVA `0x1CB93E0` | 同 travel_port |
| Encode1 / Encode2(short) | `0x1CC5940` / `0x1CC5BA0` | dump OutPacket 方法表 |
| NetworkManager.Send | RVA `0x1CB9510` | 同 travel_port |
| 背包 | `CharacterData.ItemSlots@0x40`；TI Equip=1 / Etc=4 | titlebar / consumable |

## DumpRestoredData 交叉复核（2026-08-02 · C 档）

真源：`DumpRestoredData/dump.cs.restored.C` + `restore_tokens_C.tsv`（README：铁证优先 A/B，看调用链用 C；IDA 已灌 B→C）。

| 还原项 | 结论 | 用途 |
|---|---|---|
| `UIShopDialog.Const` | `ShopBuyLeft/ShopSellLeft/ShopTop/ShopSpace/ShopSlot` 可读 | UI 布局常量；不直接发包 |
| `UIShopDialog` 方法 | `Awake` / `OnOpen` / `SetRet` / `OnClickButton` / 指针事件有名；其余仍哈希 | 确认「已开店」生命周期；卖出仍不直调 UI |
| 嵌套 `Item`（TDI 435） | `ItemId`/`Position`/`ItemName`/`Price`/`Quantity`/`ItemSlot` | 商店侧列表 DTO；可作扫槽增强（非必须） |
| `ShopItem`（TDI 436） | `Number`/`Set`/`Price`/`ItemSlot` | 店内商品行 |
| `ScriptManager.Type.AskSelectMenu` 等 | Say / YesNo / AskSelectMenu 有名 | **以后**自动开店对话链入口，本轮未接 |
| `FkmType.BasicActionNpcTalk=54` | 有名 | NPC 对话输入动作候选 |
| `GetSellSlot` / `InShop` | **误伤**：前者属拍卖/GlobalMarket 佣金语境；后者是 `FriendItem.InShop` | **不要**当杂货店 API |
| GlobalMarket `SellItem=51` | 仍非 NPC opcode | 与初判一致 |

**开店入口**（2026-08-02 续查）：见 [`P0b_自动开店锚点.md`](./P0b_自动开店锚点.md) —
`UserLocal.TalkToNpc` → `UserSelectNpc=64` →（可选脚本 66）→ `OpRecv_015D` 开 `UIShopDialog`。
仍无单独的 `OpenNpcShop` RPC；「等菜单/等人点商店」在部分 NPC 上仍可能需要。

## 卖出包形（产品路径 · IDA 对齐 UIShopDialog.SendSellRequestPacket @`0x54C080`）

需 `UIShopDialog` 实例存在（用户已开 NPC 店）后：

```
OutPacket.Create(67)          // word^0x102E → 67
Encode1(1)                    // sell（byte^0x61 → 1）
Encode2(nPOS)                 // Item.Position
Encode4(itemId)               // Item.ItemId
Encode2(qty)                  // nCount
NetworkManager.Send           // 禁止 Session.Send 旁路 HashSet（会本地踢）
```

> 旧误形 `Encode1(ti)+Encode2(pos)+Encode2(qty)` 已废。2026-08-02 BIN：错包 + Session.Send → 105ms 断线。

## 注意

- dump 内 GlobalMarket `SellItem=51` **不是**杂货店 opcode。
- `UIShopDialog` 方法体 CF 平坦化严重；卖出走 `SendSellRequestPacket` UI 入口。
- 未开店发包可能被拒；`ShopReady` 必须先过。
- **角色区 TAB**：卖装备/其他前必须切到对应 `UITab`（`ItemType` 1→tab0 … 4→tab3），否则
  `CmpSellItem` 投影的 `_sellItemList@0x198` 为空。
  实现：`UITab.OnClickTab` @ RVA `0xAC16C0`（优先 `dlg+0xC8`，按 `Items` 数量挑角色侧）。
  - 本拍刚切 TAB / `listN==0` → 返回 `LIST_STALE`，`sellbag` 限次重试（勿当失败跳过）。
  - 列表非空但未命中 item → 仍为 `LIST_MISS`（真缺项）。
  - `SHOP_BUSY` / `LIST_STALE` 均不消耗队列下标。
