# pet_loot P0a · TW 锚点复核（2026-08-03 remount）

> **状态**：✅ P0a 完成（拾取入口 / 技能位 / 矩形写点已钉死；默认矩形数值运行时读）  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase `0x7FFB74A20000`（2026-08-03 客户端）  
> **真源**：`Dumps/runtime/out/dump.cs` + CMS Rosetta / IDA ByPet xref  
> **产品**：经典版 TWMS · **不是**枫星

---

## 0. 方法

1. CMS 可读 dump 对齐方法签名序与字段  
2. TW 哈希 dump 钉 RVA（`RVA_SEMANTIC_20260803.tsv` order_zip）  
3. IDA：`Pet.TryPickUpDrop` → `DropPool.TryPickUpDropByPet`；字节模式钉 `.rdata` 矩形包  
4. `CollisionCheck` 类哈希重锚（TypeDefIndex 2446 不变，`rcPet` 仍 static `+0x20`）

---

## 1. 类型哈希（TW · 2026-08-03）

| 语义 | TW TypeDef 哈希前缀 |
|------|---------------------|
| `DropPool` | `c4424063dbd2cca8…` |
| `Drop` | `f9a2bf4d48c02a87…`（旧 `efac2857…` 作废） |
| `Pet` | `ce17438d8a1e0a4b…` |
| `CollisionCheck` | `be8cdb1b8f248e5a…`（旧 `e431509c…` 作废） |
| `UserLocal` | `ac2e48ccb42621ae…` |

---

## 2. 字段偏移（TW · 与 CMS 同布局；ByPet 反汇编复核）

| 语义 | 偏移 | 说明 |
|------|------|------|
| `DropPool._mapDrop` | `+0x20` | `Dictionary<int,Drop>` |
| `DropPool._mapDropPoint` | `+0x28` | |
| `Drop.Pt1` | `+0x20` | `Point` int x/y |
| `Drop.Id` | `+0x30` | drop 实体 id |
| `Drop.OwnType` | `+0x3C` | 0–4；实机若异常大数需写成 `No=2` 才能过 ByPet |
| `Drop.IsMoney` | **`+0x44`**（TW；CMS 为 0x40） | bool |
| `Drop.Info` | **`+0x48`**（TW；CMS 为 0x44） | 非金币时多为 itemId |
| `Drop.EndParabolicMotion` | `+0x7C` | **ByPet 硬门：必须 `== 3` 才继续**；写 `0` 会重置抛物线 |
| `Drop.LastTryPickUp` | `+0x80` | 节拍相关；宠吸拍前仅在非 0 时清零 |
| `Drop` Send 盖戳 | `+0x88` | ByPet `Send` 后写入；拍前非 0 时清零 |
| `Pet._rcPet` | `+0x100` | Unity `Rect`；**次要**（ByPet Contains 不读） |
| `Pet.ExceptionList` | `+0x90` | `List<int>` backing |
| `CollisionCheck.rcPet` | static `+0x20` | 模板矩形；**次要** |
| ByPet Contains 矩形包 | `.rdata` RVA **`0x55736B0`** | `int32 offX/Y` + `float w/h`；原生 (25,10)+(50,60)；旧 `0x5574990` |

---

## 3. 方法 RVA（TW · 2026-08-03 · IDA 已重命名）

| 语义 | RVA | VA | IDA 名 |
|------|-----|-----|--------|
| `DropPool.TryPickUpDropByPet` | **`0xF528E0`** | `0x7FFB759728E0` | `DropPool_TryPickUpDropByPet` |
| `DropPool.TryPickUpDrop` | `0xF508D0` | `0x7FFB759708D0` | `DropPool_TryPickUpDrop`（脚边） |
| `DropPool.SendDropPickUpRequest` | `0xF51F70` | `0x7FFB75971F70` | |
| `Pet.TryPickUpDrop` | **`0xF96240`** | `0x7FFB759B6240` | `Pet_TryPickUpDrop` |
| `Pet.SendDropPickUpRequest` | `0xF54F90` | `0x7FFB75974F90` | `Pet_SendDropPickUpRequest` |
| `Pet.GetUpgradePetSkill` | `0xF55F40` | `0x7FFB75975F40` | `Pet_GetUpgradePetSkill` |
| `Pet.GetItemSlot` | **`0xF54B60`** | `0x7FFB75974B60` | ByPet 内 xref；→ `usPetSkill@+0x3C` |
| `Pet.IsInExceptionList` | `0xF4AC20` | `0x7FFB7596AC20` | `Pet_IsInExceptionList` |

### 3.1 `Pet.TryPickUpDrop` 调用链（实锤）

反汇编：取宠位置 → `DropPool` Singleton → **`DropPool.TryPickUpDropByPet(thisPet, pos, …)`** @ `0xF528E0`。  
本 feature **只直调 `Pet.TryPickUpDrop`**。

### 3.2 Contains 真源

- ByPet `.rdata` 矩形包 **`@0x55736B0`**（`psubd xmm9, [pack]` / `movsd xmm0, [pack+0x10]`）  
- `_rcPet` / `CollisionCheck.rcPet`：**不写入**

### 3.3 `ByPet` 门控（IDA · 2026-08-03 复核）

| 检查 | 证据 | 通过条件 |
|------|------|----------|
| `IsMoney@+0x44` | `movzx eax,[rdi+44h]` | 金币走 `PickUpAll` 技能位分支 |
| `OwnType@+0x3C` | `mov ecx,[rdi+3Ch]` | 异常大数会跳过 → 写成 `No=2` |
| **`EndPara@+0x7C`** | `mov eax,[rdi+7Ch]` + 混淆常量 `cmp` / `cmovz` | **必须等于 Ready(=3)** |
| `Real@+0x2D` | `test [rdi+2Dh]` | 须为真 |
| Send 盖戳 `@+0x88` | `mov [rdi+88h], eax` after Send | 拍前清零 |

**正确宠吸方法**：

```text
清门控(EndPara→3, +0x88→0, LastTry→0, OwnType异常→2)
→ 拍内 VirtualProtect 改写 ByPet .rdata 矩形包（原生 off=25,10 size=50x60）
   RVA 0x55736B0：int32 offX/offY + float w/h
→ Pet.TryPickUpDrop()   // → ByPet → Send
→ 恢复 .rdata
```

禁止：手组 `SendDropPickUpRequest`、写 `EndPara=0`、改 GA `.text`。

### 3.4 真吸判定

见 [`模块设计.md`](模块设计.md) §3.2（同拍 Δ / 跨拍 fell）。

## 4. 宠物技能位（常量）

真源：`Pet.GetItemSlot` → `ItemSlotPet.usPetSkill@+0x3C`（CMS 同布局）。  
`GetUpgradePetSkill` = 对该字段的封装；ByPet@`0xF528E0` 内有 `GetItemSlot` xref。  
**禁止**把 `Pet+0x428` 当技能位（实机乱跳，已证伪）。

| 位 | 值 | 含义 |
|----|-----|------|
| `PetSkillPickupItem` | 1 | 拾取道具（硬门） |
| `PetSkillLongRange` | 2 | 远程 |
| `PetSkillDropSweep` | 4 | 扫荡 |
| `PetSkillIgnore` | 8 | 忽略表相关 |
| `PetSkillPickUpAll` | 16 | 含金币等 |
