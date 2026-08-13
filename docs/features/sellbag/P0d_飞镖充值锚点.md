# sellbag / auto_supply P0d — 飞镖 Charge 锚点（Classic TWMS）

> **产品**：新枫之谷：经典版 · **不是**枫星  
> **对照**：CMS `SendRechargeRequest` / `RequestRecharge=2` 仅作命名；实现与 RVA 以本仓 TW dump / IDA 为准。  
> **真源**：`DumpRestoredData/dump.cs.restored.C` + `Dumps/runtime/GameAssembly.dll.i64`  
> **实现**：`x/features/ports/shop_port.cpp` → `RechargeShurikensInOpenShop`

## 已钉（铁证）

| 项 | 值 | 证据 |
|---|---|---|
| UI | `UIShopDialog` | 类哈希同 P0a |
| 充值入口（产品） | `SendRechargeRequestPacket()` RVA **`0x54E1B0`** | dump 哈希 `c86e335c…743e`；IDA 内含 Create+Encode+Send |
| 确认包装（不用） | `SendRechargeRequest()` RVA `0x549100` | 会弹 YesNo；自动化绕过，直调 Packet |
| C→S | `UserShopRequest` **= 67** | 与买/卖同源 |
| 操作码 | Recharge `Encode1(2)` | CMS `Shop.RequestRecharge=2`；IDA Packet 路径 Encode1 后 Encode2(pos) |
| 选中 | `_sellSelectedIndex` @ **`+0x1AC`** | 与卖出共用卖栏选中 |
| 卖栏 | `List<Item>` @ **`+0x198`** | 消耗 TAB + `CmpSellItem` 投影 |
| 忙标记 | `_hasShopRequestSent` @ **`+0x1B4`** | 发包后写 1 |
| Item DTO | `UintPrice` / `MaxSlotCount`（卖栏常 0）/ `Stock` / **`ItemSlot@0x48`→`nNumber`** | dump TDI 435；Charge 数量真源 = Bundle；`CmpSellItem` 比 `Stock@0x3C` |
| 满格真源 | **`ItemBundle.nMaxPerSlot@0x4C`**（hash `bfe3de62…8820e`）；次选 `Info.slotMax@0x58`（hash `bca765ba…11322`） | 运行时 `dump.cs` TDI 2002/2031；IDM `_itemBundleItem@0x38` / `_dataTable@0x18`→`info@0x18`；按 itemId 各不相同 |
| 字段防漂 | `EnsureShopFieldOffsets`：hash/明文 → `field_get_offset`；常量仅 fallback | 日志 `path=meta\|meta-partial\|fallback` |

## 充值包形（UI 路径；禁止手组旁路）

```
OutPacket.Create(67)
Encode1(2)                 // recharge
Encode2(Position)          // 选中卖栏 Item.Position
NetworkManager.Send        // UI 内发；禁止 Session.Send 旁路 HashSet
```

数量由服端按槽位补满（客户端 Packet 无 count 参数）。

## 产品路径

1. `ShopReady`  
2. 切角色区消耗 TAB → `CmpSellItem` 刷新 `_sellItemList@0x198`  
3. 扫 `2070000…2079999`：当前量 = `ItemSlot.nNumber`；满格优先 `Item.MaxSlotCount`，否则 **`ItemBundle.nMaxPerSlot@0x4C`**（`ItemDataManager._itemBundleItem`），再否则 `Info.slotMax@0x58`；仅查表失败才退回 500。需 `cur < max` 且 **`UintPrice > 0`**  
4. 估成本 = **`UintPrice` 整格一口价**（不是 ×deficit；实机补满一堆 150）  
5. 买得起则优先更便宜（同价取赤字更大），写 `_sellSelectedIndex` / lastSell  
6. 若 busy → `SHOP_BUSY`（`auto_supply` 下拍重试）  
7. 主线程调 `SendRechargeRequestPacket()`；每次最多 1 格，行程循环至 `NONE`/`NO_MESO`

## 注意

- 勿调 `SendRechargeRequest`（确认框）。  
- 现金飞镖（如 `5021xxxx`）本 P0 不扫。  
- 手组 67 + Session.Send 会本地踢线。  
- 字段偏移走 shop_port 防漂移（remount 时更新字段 hash；启动见 `Shop field offsets path=`）。  
- 运行时 BIN：开店 + 勾选「自动补飞镖」+ 消耗栏有未满飞镖。
