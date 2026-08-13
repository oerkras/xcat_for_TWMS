# pet_loot P0a · TW 锚点复核（2026-08-04 remount）

> **状态**：✅ P0a 完成（拾取入口 / 技能位 / 矩形写点已钉死；默认矩形数值运行时读）  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase `0x7FFB83A80000`（2026-08-04 客户端）  
> **真源**：`Dumps/runtime/out/dump.cs`（对照 `out/old/20260803_pre_20260804/`）+ IDA ByPet / find_bytes  
> **产品**：经典版 TWMS · **不是**枫星

---

## 0. 方法

1. CMS 可读 dump 对齐方法签名序与字段  
2. TW 哈希 dump：TypeDefIndex 保序 + 方法哈希钉 RVA  
3. IDA：`Pet.TryPickUpDrop` → `DropPool.TryPickUpDropByPet`；`find_bytes` 钉 `.rdata` 矩形包  
4. `CollisionCheck` TypeDefIndex **2446** 不变；类哈希重锚

---

## 1. 类型哈希（TW · 2026-08-04）

| 语义 | TW TypeDef 哈希前缀 | 备注 |
|------|---------------------|------|
| `DropPool` | `df1d3ac49f3798f5…` | TDI 1489；旧 `c4424063…` 作废 |
| `Drop` | `dd446a8d5623496c…` | TDI 1488；旧 `f9a2bf4d…` 作废 |
| `Pet` | `f5be2907a4e45eab…` | TDI 1516；旧 `ce17438d…` 作废 |
| `CollisionCheck` | `d3fd874923d972c6…` | TDI 2446；旧 `be8cdb1b…` 作废 |
| `UserLocal` | `d344a8e976a30de4…` | shape 重锚；旧 `ac2e48cc…` 作废 |
| `CashItemManager` | `da9619a468e58334…` | TDI 1247；召宠包 |

---

## 2. 字段偏移（TW · 防漂移）

运行时：`FindClass(classHash)` + `il2cpp_class_get_field_from_name(fieldHash)` + `il2cpp_field_get_offset`；  
下列数值为 **dump 验证 fallback**（`path=meta|meta-partial|fallback`，见 `DropPort` / `PetPort` 日志）。

| 语义 | fallback | 字段 hash 前缀（08-04） |
|------|----------|------------------------|
| `DropPool._mapDrop` | `+0x20` | `fa870d8b…` |
| `Drop.Pt1` | `+0x20` | `ccb092d6…` |
| `Drop.Id` | `+0x30` | `daf6f448…` |
| `Drop.OwnType` | `+0x3C`（ByPetParity 死钉） | CMS `da3077a0…` 在 TW 易误钉 Id；`mov [rdi+3Ch]` cmp User=0 |
| `Drop.IsMoney` | `+0x44` | `fd049595…` |
| `Drop.Info` | `+0x48` | `b3fb2cdb…` |
| `Drop.EndParabolicMotion` | `+0x7C` | `f24df618…` |
| `Drop.LastTryPickUp` | `+0x80` | `a9bab9bf…` |
| `Drop` Send 盖戳 | `+0x88` | `ce6afe24…` |
| `User.m_apPet` | `+0x2B0` | `a3e632d0…` |
| `User.CurPos` | `+0x240` | `b992bfa5…` |
| `Pet._rcPet` | `+0x100` | `b52942f1…` |
| `Pet.ExceptionList` | `+0x90` | `<e21440b3…>k__BackingField` |
| `VecCtrlOwner.VecCtrl` | `+0x50` | `<dc76f5c9…>k__BackingField` |
| `VecCtrlOwner.Pos` | `+0x64` | `c9d7ef43…` |
| `VecCtrl.Ap`（AbsPos） | `+0x98`（Y=+8） | `a860e652…`；**`worldY=-Ap.Y`** |
| `ItemSlotPet.usPetSkill` | `+0x3C` | `b5152b3f…` |
| `WM.MyUser` | `+0x28` | `<d4428e1b…>k__BackingField` |
| `CollisionCheck.rcPet` | static `+0x20` | `dd70a7dc…` |
| ByPet Contains 矩形包 | `.rdata` RVA **`0x557ED00`** | 三级定位；旧 `0x55736B0` 作废 |

---

## 3. 方法 RVA（TW · 2026-08-04）

| 语义 | RVA | 备注 |
|------|-----|------|
| `DropPool.TryPickUpDropByPet` | **`0xF59980`** | |
| `DropPool.TryPickUpDrop` | `0xF577A0` | 脚边 |
| `DropPool.SendDropPickUpRequest` | `0xF590A0` | |
| `Pet.TryPickUpDrop` | **`0xF9CBE0`** | |
| `Pet.SendDropPickUpRequest` | `0xF5C090` | |
| `Pet.GetUpgradePetSkill` | `0xF5CF10` | |
| `Pet.GetItemSlot` | **`0xF5BD50`** | → `usPetSkill@+0x3C` |
| `Pet.IsInExceptionList` | `0xF51C20` | |
| `CashItemManager.SendActivatePetRequest` | `0xC5A960` | 召宠 |
| `Object.FindObjectsOfTypeAll` | `0x4E4A610` | |

### 3.1 调用链

`Pet.TryPickUpDrop` → Singleton `DropPool` → **`TryPickUpDropByPet`**。本 feature 只直调 `Pet.TryPickUpDrop`。

### 3.2 Contains 真源

- `.rdata` 矩形包 **`@0x557ED00`**（运行时亦可扫 GA 映像 / ByPet rip-rel）  
- 不写 `_rcPet` / `CollisionCheck.rcPet`

### 3.3 位姿空间（BIN 0.1.17 + 08-04 仍适用）

`VecCtrl.Ap` = Unity Y-up；`Drop.Pt1` = 枫谷 Y-down → 门控/近距用 **`worldY = -Ap.Y`**。

---

## 4. 宠物技能位（常量）

真源：`Pet.GetItemSlot` → `ItemSlotPet.usPetSkill@+0x3C`。  
禁止 `Pet+0x428`。

| 位 | 值 | 含义 |
|----|-----|------|
| `PetSkillPickupItem` | 1 | 拾取道具（硬门） |
| `PetSkillLongRange` | 2 | 远程 |
| `PetSkillDropSweep` | 4 | 扫荡 |
| `PetSkillIgnore` | 8 | 忽略表 |
| `PetSkillPickUpAll` | 16 | 含金币等 |
