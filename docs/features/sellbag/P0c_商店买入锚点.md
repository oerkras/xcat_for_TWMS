# sellbag P0c — NPC 商店买入锚点（Classic TWMS）

> **产品**：新枫之谷：经典版 · **不是**枫星  
> **对照**：CMS `dump.cs` 仅作枚举命名；实现与 RVA 以本仓 TW dump / IDA 为准。  
> **真源**：`DumpRestoredData/dump.cs.restored.C` + `Dumps/runtime/GameAssembly.dll.i64`

## 已钉（铁证）

| 项 | 值 | 证据 |
|---|---|---|
| UI | `UIShopDialog` | 类哈希同 P0a |
| 买入入口 | `SendBuyRequestPacket(int nCount)` RVA **`0x54B4E0`** | 与卖出 `0x54C080` 同形：内含 `OutPacket.Create` + `Encode1` + `NM.Send` |
| 卖出入口 | `SendSellRequestPacket` RVA `0x54C080` | P0a |
| C→S 枚举 | `UserShopRequest` **= 67**（0x43） | 买/卖共用；买 Create：`word+0x7757`；卖 Create：`word^0x102E` → 皆 67 |
| 操作码 | Buy `Encode1(0)` / Sell `Encode1(1)` | 买：`byte^0xA1→0`；卖：`byte^0x61→1` |
| 买选中下标 | `_buySelectedIndex` @ **`+0x1A8`** | IDA：发包前 `mov edx,[rsi+1A8h]` → `Encode2` |
| 卖选中下标 | `_sellSelectedIndex` @ `+0x1AC` | P0a / shop_port |
| 忙标记 | `_hasShopRequestSent` @ **`+0x1B4`** | 买/卖发包后写 1；未清则 `SHOP_BUSY` |
| 买列表 | `List<Item>` @ **`+0x178`** / **`+0x180`** | 按买侧 UITab 二选一：`[dlg+rdx*8+178h]`；产品侧两表都扫 |
| Item DTO | `ItemId`/`Price`/… 明文 meta；fallback `@+0x10…` | dump TDI 435 |
| 字段防漂 | `EnsureShopFieldOffsets`（hash/明文→offset） | 日志 `Shop field offsets path=` |

## 买入包形（UI 路径编码；产品禁止手组旁路）

```
OutPacket.Create(67)
Encode1(0)                 // buy
Encode2(buySelectedIndex)  // _buySelectedIndex@+0x1A8，店内买栏下标
Encode4(itemId)            // 选中 Item.ItemId
Encode2(nCount)
NetworkManager.Send        // UI 内发；禁止 Session.Send 旁路 HashSet
```

## 产品路径

1. `ShopReady`（`UIShopDialog` 实例存在）  
2. 在 `+0x178` / `+0x180` 按 `ItemId` 匹配行，得到 index  
3. 写 `_buySelectedIndex@+0x1A8`（可同步写 `+0x1B0` 作 lastBuy 痕迹）  
4. 若 `_hasShopRequestSent@+0x1B4 != 0` → `SHOP_BUSY`（调用方重试）  
5. 主线程调 `SendBuyRequestPacket(nCount)` @ `0x54B4E0`  
6. 对账：消耗栏该 `itemId` 数量上升 / meso 下降

## 注意

- 与卖出相同：手组 67 + Session.Send 会本地踢线；只走 UI 方法。  
- 买栏与卖栏（`+0x198`）分离；补药只查买栏。  
- 部分 NPC 需菜单点「商店」后才有 `UIShopDialog`（见 P0b）。  
- 飞镖 Charge 见 **P0d**。
