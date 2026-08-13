# pet_loot P0b · Drop 归属客户端预筛（工程真源 B）

> **状态**：✅ BIN 钉死（2026-08-12）  
> **产品**：经典版 TWMS（`Maplestory_Classic.exe`）· **不是**枫星  
> **代码**：`ports/drop_pool_port.*`（`DropClientPickable` / `LocalDropSelfOwnerId`）· `ports/world_port.*`（`GetDropOwnerCharacterId`）· `ports/user_pool_port.*`（Peek 远程）  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64`（本轮 imagebase `0x7FF848C80000`）  
> **模块总览**：[`模块设计.md`](模块设计.md) §3.1 · 3c

---

## 0. 目标

在客户端预筛「这件地上物该不该吸」，避免多人图吸走别人的物；**不得**用错本地 ID 把自己的物整图否决（曾 `why=ok_own` / `ownSkip=60+` / `absorbed=0`）。

适用范围：宠吸 `RunVacuumOnMain`、人物直吸 `RunCharVacOnMain`（共用 `DropClientPickable`）。脚边原生 `TryPickUpDrop` **不读**本预筛。

---

## 1. 两套 CharacterId（BIN 铁律）

| 槽 | 典型 BIN 值 | 用途 |
|----|-------------|------|
| `Drop.OwnerId@+0x34` | **118536** | 掉落归属（服端进包写入） |
| `CharacterStat+0x10` / 常与之相等的槽 | **194899** | 战斗 / DamageInfo 等（`GetCharacterId()`） |
| `User+0x1B0` | ≈194899 | User uint getter；**≠** Drop.OwnerId |
| `WM+0x98`（`get_CharacterId`） | ≈194899 | 与 CS 同值；**≠** Drop.OwnerId |
| **`WM+0x114`** | **118536** | **本地 Drop 归属真源**（与地上 OwnerId 对齐） |

> **勿混**：[`drop_alert_bypass`](../drop_alert_bypass/模块设计.md) 的警戒字段是 **`LocalUser+0x114`**（另一对象同偏移数字）。本条是 **`WorldManager+0x114`**。

---

## 2. IDA / 包路径（工程真源）

### 2.1 地上物怎么来的 OwnerId

`DropPool` 进包处理（CFF）内：

```text
call  ReadInt          // sub_…950760 一类
mov   [Drop+34h], eax  // OwnerId ← 服端下发，非本地拷贝
```

### 2.2 官方 ByPet 比较（易误导）

`DropPool.TryPickUpDropByPet` / `IsThereDropNear` 反汇编可见：

```text
movsxd rax, [Drop+34h]     ; OwnerId
mov    edx, [localObj+98h]
cmp    rax, rdx
```

`WorldManager.get_CharacterId`（RVA **`0xDC35B0`**，本 IDB）= `mov eax,[rcx+98h]; ret`。

**BIN 证伪「+0x98 = Drop 同槽」**：同拍日志

```text
via WM+0x114 scan (wm+0x98=194899 cs+0x10=194899)
```

即反汇编的 `+0x98` 在本机构建上读到的是 **CS 系 ID**，不能当预筛 self。

### 2.3 本地真源怎么钉的

1. 枚举到 `OwnerId=118536` 的 drop 时，对 **本地 WM** 做 `u32` 扫描（`0x10..0x180`）  
2. 命中 → 钉死偏移（首锤 **`WM+0x114`**）  
3. `GetDropOwnerCharacterId()` 之后优先读钉死偏移；默认也试 `+0x114`，再试 `+0x98`（且必须 ≠ CS）

dump 名（`dump.cs.restored`）把 `+0x114` 标成混淆 backing field、把 `+0x98` 标成 `_isExclRequestSent` / 或旧表写成 CharacterId——**以 BIN + getter/扫描为准**。

---

## 3. 运行时策略（`DropClientPickable`）

```text
OwnType∈{No, ExplosiveNoOwn}     → 放行
OwnType==Party                   → 跳过
OwnerId==0                       → 放行（无主）
OwnerId ∈ 远程 User 已学槽集合   → 跳过（Peek 枚举，≤400ms 刷新；禁止 InvokeAndWait）
OwnerId == 本地 DropSelfId       → 放行
独图（remotes==0）且未认亲       → 放行（fail-open；服端仍拒非己）
独图已认亲但 ID 对不上           → 清错缓存 + 本拍放行，靠真吸重学
多人未命中远程集合且未认亲       → 放行（避免堵吸）
```

### 3.1 本地 self ID 解析顺序

`LocalDropSelfOwnerId(ownerHint)`：

1. 缓存若 `== CS+0x10` **且** 地上 `OwnerId` 合理且 `≠ CS` → **purge**（真 CS 毒）；合一角（OwnerId==CS）保留  
2. 有 `ownerHint` → 扫 **WM** / `WM+0x90` CharacterId 盒 → 命中则 `NoteDropOwnerWmFieldOff` + 缓存（含 OwnerId==CS）  
3. `GetDropOwnerCharacterId()`：钉死 off → **`WM+0x114`（允许==CS）** → `WM+0x98`（≠ CS）  
4. 地上 OwnerId==CS → 直接认作 self（本角两套 ID 合一）  
5. 兜底：扫 User / CharacterData；**真吸成功** `NoteDropSelfOwnerFromPickup`  
6. **禁止**把 `WM+0x98` / `User+0x1B0` 在「等于 CS」时当成 Drop self；`WM+0x114` 即使==CS 仍可用

### 3.2 远程黑名单

- 只读 `PeekRemoteUserCount` / `PeekEnumRemoteUsers`（纯内存）  
- **不**用远程 `User+0x1B0`（常为 CS 系）  
- 外物 OwnerId 扫到某远程 User 上的偏移后，记 `gRemoteDropOwnerOff`，刷新时只读该槽

### 3.3 线程

- 热路径 **禁止** `SampleRemoteUserCount` / 每 drop `InvokeAndWait`（曾泵超时 / 体感卡顿）  
- il2cpp 托管调用仍只许主泵；本预筛读写已解析偏移，worker 可读

---

## 4. API / 文件

| 符号 | 文件 | 作用 |
|------|------|------|
| `DropClientPickable` | `drop_pool_port.cpp` | 统一可捡预筛 |
| `LocalDropSelfOwnerId` | 同上 | self 缓存 + WM 扫描钉槽 |
| `NoteDropSelfOwnerFromPickup` | 同上 | 真吸认亲 |
| `GetDropOwnerCharacterId` | `world_port.cpp` | 读 WM Drop 归属槽 |
| `NoteDropOwnerWmFieldOff` | `world_port.*` | 钉死 WM 偏移 |
| `PeekRemoteUserCount` / `PeekEnumRemoteUsers` | `user_pool_port.*` | 远程集合（无泵） |

---

## 5. 日志认路

| 日志 | 含义 |
|------|------|
| `Drop.OwnerId self=… via WM+0x114 scan (wm+0x98=… cs+0x10=…)` | **正路径**：扫钉真源；`+0x98` 应等于 CS、不等于 self |
| `Drop.OwnerId self=… via WM field/ByPetParity … pinOff=0x…` | 已从钉死/默认槽直接读出 |
| `Drop.OwnerId self=… (learned from pickup; cs+0x10=…)` | 真吸兜底（旧路径；有 WM+0x114 后应少见） |
| `Drop.OwnerId self cache purge … (==cs+0x10; ground=…)` | 真 CS 毒：缓存==CS 且地上≠CS；合一角不会打这条 |
| `Drop.OwnerId self=… (==cs+0x10; ground OwnerId equals CS…)` | 本角 Drop/CS 合一，信任 OwnerId |
| `Drop.OwnerId probe hint=… +98=…` | 扫未命中时的一次性探针 |
| `petloot`：`ownerId=` / `myCid=` / `ownSkip=` / `remotes=` / `ownType=` / `why=ok_own` | `remotes>0` 多人；`ownType`∈0..4 或 `-1`（未解析）；`ok_own`+高 `ownSkip`=self 认错 |
| OwnType 真源 | ByPet `mov ecx,[drop+3Ch]` + seed 解出 cmp **User=0**；禁止用误钉哈希读 Id |
| 健康独图 | `myCid==ownerId`；允许 `myCid==cs+0x10`（部分角两套 ID 合一） |
| 毒日志（已收窄） | 仅当缓存==CS **且** 地上 OwnerId≠CS 才 purge；合一角不得清 |
| 健康多人 | `remotes≥1`；别人归属物 → `ownSkip↑`；己物仍吸 |

### 5.1 健康样例（2026-08-12）

```text
Drop.OwnerId self=118536 via WM+0x114 scan (wm+0x98=194899 cs+0x10=194899)
mode=petmap ... remotes=0|N ownType=0 ownerId=118536 myCid=118536 ownSkip=0 ... why=ok_sent|ok|wait_land
```

`remotes=-1` = Peek 失败；多人期望 `remotes≥1` 且别人归属时 `ownSkip↑`；`ownType` 应为 0..4。
### 5.2 故障样例（已修）

```text
Drop.OwnerId self=194899 via GetDropOwnerCharacterId (cs+0x10=194899)
mode=petmap ... ownerId=118536 myCid=194899 ownSkip=80+ why=ok_own absorbed=0
```

根因：把 `User+0x1B0` / `WM+0x98` / CS 缓存成 self，独图硬否决全部己物。

---

## 6. 配置旁路（黑名单默认）

与归属无关但同日落地：`[pet_loot]` 默认 skip 关键词 **`箭矢`**、**`彈丸`**（`skipFilterEnabled=1`；缺 `skipCount` 用默认，显式 `0` 表示用户清空）。见 [`模块设计.md`](模块设计.md) §5。

---

## 7. 验收清单

```text
1) 进图有己物：首拍或首枚 OwnerId 后出现 via WM+0x114 scan（或 pin 直读）
2) myCid == ownerId（如 118536）；ownSkip 在独图应为 0（黑名单 skipN 另计）
3) 无长期 why=ok_own + absorbed=0
4) wm+0x98 日志值允许 == cs+0x10，且 ≠ self
5) 多人：别人 OwnerId 应 ownSkip 增加；勿用 CS 系 ID 误伤自己
6) 关宠吸/换图后清缓存不残留毒 ID
```

---

## 8. 与 WM 字段表的关系

[`world_manager/字段全表.md`](../world_manager/字段全表.md) 旧推断曾把 `+0x98` 写成 CharacterId、`+0x114` 写成 PartyID。  
**掉落归属以本 P0b 为准**：`WM+0x114` = Drop.OwnerId 同槽；`WM+0x98` = CS 系（本机构建）。字段全表已加交叉引用。
