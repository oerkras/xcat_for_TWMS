# pet_feed P0a · TW 锚点复核

> **状态**：✅ 核心召唤/读态锚点有效（**2026-08-03 remount**）  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB74A20000`**  
> **GA**：`Dumps/runtime/GameAssembly.dll`（2026-08-03）  
> **真源**：`Dumps/runtime/out/dump.cs` + `Dumps/cms_cw/`（Rosetta）  
> **产品**：经典版 TWMS · **不是**枫星  
> **代码真源**：`x/features/ports/pet_port.cpp`（`remapped 2026-08-03`）

---

## 0. 版本线

| 日期 | imagebase | 说明 |
|------|-----------|------|
| 2026-08-01 | `0x7FFB16B40000` | 初锚；RVA/类哈希见下方「旧包」列 |
| **2026-08-03** | **`0x7FFB74A20000`** | 客户端更新；**字段偏移大多未漂**；方法 RVA / 类型哈希重锚 |

---

## 1. 类型哈希（TW · 2026-08-03）

| 语义 | TW TypeDef 哈希前缀 | dump.cs |
|------|---------------------|---------|
| `User`（含 `m_apPet`） | `a03443a914a5c245…` | L69138 |
| `UserLocal` | `ac2e48ccb42621ae…` | L70154 |
| `Pet` | `ce17438d8a1e0a4b…` | L67504 |
| `ItemSlotPet` | `e5b18dece81d4a95…` | L80775 |
| `CashItemManager` | `edfa536aa4e0251d…` | L55177 |

`pet_port` FindAll 用 **`UserLocal`=`ac2e48…`**（收 GO 名 `MyUser`）。

---

## 2. 字段偏移（TW · 08-03 仍成立）

| 语义 | TW | 证据 |
|------|-----|------|
| **`m_apPet`** | **`User+0x2B0`** | dump L69218；`pet_port` / 旧 IDA `mov …,[reg+2B0h]` |
| `Pet._repleteness` | `Pet+0xBC` | dump L67531 |
| **`ItemSlotPet.nRepleteness`** | **`byte @0x38`** | dump L80781 |
| `ItemSlotPet.dateDead` | `+0x40` | L80784 |
| `ItemSlotPet.nRemainLife` | `+0x48` | L80785 |
| `ItemSlotPet.nActiveState` | `+0x4E` | L80787 |
| 原生自动喂常量 | `1000` / `60000` / **`50`** | UserLocal L70163–70165 |

读态实现：**字段只读**（数 `m_apPet` 非空 + `Pet+0xBC` / 槽位 `+0x38`），不依赖混淆的 `GetRepleteness` RVA。

---

## 3. 方法 RVA（2026-08-03 · 代码在用）

基址 `0x7FFB74A20000`；RVA = VA − base。

### 3.1 召唤（主路径）

| 语义 | RVA | VA | 备注 |
|------|-----|-----|------|
| **`CashItemManager.SendActivatePetRequest(nPos)`** | **`0xC56910`** | **`0x7FFB75676910`** | dump L55226；`pet_port` 已用 |
| 相邻：`SendCashSlotItemUseRequest(int,int,long)` | `0xC57F90` | `0x7FFB75677F90` | 方法序对齐 |

旧包（08-01）对应：Activate `0xC58020` → 新 `0xC56910`（Δ `−0x1710`）。

### 3.2 Unity / 主线程泵（与全仓共用）

| 语义 | RVA（08-03） |
|------|----------------|
| `FindObjectsOfTypeAll` | `0x4E3FA20` |
| `Component.get_gameObject` | `0x4E47E00` |
| `Object.get_name` | `0x4E54D60` |
| `Canvas.SendWillRenderCanvases` | `0x5239AB0` |

### 3.3 喂食（**首版不调用** · 旧包 RVA 仅备查）

下列为 **2026-08-01** 旧 imagebase 下的钉值；**08-03 未重锚**（召唤-only 不依赖）。若开 P2 自建喂，须按 CMS 方法序在新 dump 重钉。

| 语义 | 旧 RVA（08-01） |
|------|-----------------|
| `UserLocal.TryConsumePetFood` | `0x101B700` |
| `UserLocal.FindProperPetToEatFood` | `0x109F4D0` |
| `UISlotItem.SendPetFoodItemUseRequest` | `0x5E9790` |
| `Pet.GetRepleteness` 等读态方法 | `0xF8F5B0` 等 | 代码已改字段只读 |

---

## 4. 实现侧（与代码一致）

```text
ReadState:
  UserLocal MyUser → m_apPet@0x2B0
  activated = 数组非空个数
  minFull   = Pet+0xBC（槽位+0x38 兜底）
  food      = Consume 扫 212xxxx
  summonPos = Cash 扫 500xxxx（活宠、未激活）

Summon:
  CashItemManager.SendActivatePetRequest @ RVA 0xC56910
  （主线程泵 SendWill @ 0x5239AB0）
```

---

## 5. 验收（08-03 remount）

| 检查 | 结果 |
|------|------|
| `m_apPet` | ✅ 仍 `0x2B0` |
| Activate RVA | ✅ `0xC56910`（dump+IDA+代码） |
| 类哈希 | ✅ `ac2e48` / `edfa536` / `ce17438` / `e5b18de` |
| 原生阈值 50 | ✅ UserLocal 常量簇仍在 |
| 喂食方法 RVA | ⏳ 未重锚（非首版路径） |
