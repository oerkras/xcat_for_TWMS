# mob_pool P0e · `SetDeadType` / `_deadType@0x1B4`（只读）

> **状态**：🔍 静态部分结案（写入 API 已钉；**调用方静态未出**）· **本轮不改产品代码**  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FF848C80000`**（2026-08-06 晚 remount；旧 `0x7FFB83A80000`）  
> **上游**：[`P0a`](P0a_OnLocalMob与Init包体.md)（Leave 不写 deadType）· [`P0c`](P0c_Enter到开火时间线.md) · [`P0d`](P0d_suspended与initDelay.md) · [`REMOUNT_20260806`](REMOUNT_20260806.md)  
> **字段真源**：`dump.cs.restored` Mob：`_deadType // 0x1B4` · `SetDeadType(int)` · `GetDeadType()` · `_revive // 0x1D0`

---

## 0. 结论先行

1. **`Mob.SetDeadType(this, int)`** = 薄 setter：`mov [rcx+1B4h], edx; ret`。  
   - RVA **`0xF398A0`** · VA **`0x7FF849BB98A0`**（`Mob_SetDeadType`；旧 `0xF37A30`，见 [`REMOUNT_20260806`](REMOUNT_20260806.md)）  
   - 兄弟 getter RVA **`0xF398C0`**（`Mob_GetDeadType`）  
2. **在全部 `Mob_*` / `MobPool_*` 命名函数内，`_deadType` 的唯一写点就是 `SetDeadType`**（对 `+0x1B4h]` 的 `mov` 穷举）。  
3. **`SetDeadType` / `GetDeadType` / `GetReviveList` 均无 code xref**（对比同表邻居 `SetActive` 有 `MobPool_SetLocalMob` 等直调）。  
   - 函数指针仅见于 Mob `methodPointers` 槽 **`0x7FFB8A341E68`**（邻槽：`IsRisingByToss` / `GetReviveList` / `GetDeadType`）。  
   - 全镜像 `find_bytes` 对该 VA 仅命中这一处；无 RIP-`lea` 到该槽。  
   - → **调用链静态 BLOCKED**：疑似仅经 MI / `methodPointers[index]` 间接派发，或本 build 业务路径极少走到。  
4. **`OnPacketMobLeaveField` 不写 `_deadType`**（P0a 已钉）；DelayedDead 只入 `_listMobDelayedDead@0x18` + 主池 Remove。  
5. **读侧（已钉常量）**：

| 位点 | 解混淆 | 分支净效果 |
|---|---|---|
| `Mob_ca347…` Update | `0xDD543FC2 + seed@7EE4 → **0**` | `deadType==0` → **跳过**本块 `MakeHpIndicator`；`!=0` → 调 `MakeHpIndicator` |
| `MobPool_d87ed2…`（Field Update 链） | `0xA60697DE + seed@9720 → **1**` | `deadType > 1` → **跳过** `Mob_e6c1…`；`≤1` → `Mob_e6c1(mob, 0)` |
| `Mob_f6f081…` | `0xB15BEF36 + seed@7D20 → **0**` | `deadType==0` / `!=0` 分路（`!=0` 路先读 `_suspended@0x1B8`） |

6. **与 `FillLite`**：产品硬门 `deadType==0`。官方 `FindHitMobInRect` **不读** deadType（P0c）。Leave 踢池后本就进不了 dict 扫描 → **Leave 路径不依赖 deadType**。deadType 只在「仍留在主池且字段非 0」时挡我方选怪。  
7. **枚举具体值（1 vs 2 vs …）**：静态未找到写入样例 → **未钉**；观察方案见 [`P0f`](P0f_SetDeadType观察方案.md)（MI 换针 / HWBP · **未落码**）。

---

## 1. API 与邻域（dump）

```text
// dump.cs.restored · Mob
private float _initDelay;     // 0x1B0
private int   _deadType;      // 0x1B4
private bool  _suspended;     // 0x1B8
...
private readonly List<int> _revive; // 0x1D0

public void SetDeadType(int idx);   // RVA 0xF37A30
public List<int> GetReviveList();     // RVA 0xF37A40 · 同样无 code xref
public int GetDeadType();             // RVA 0xF37A50
```

`methodPointers` 片段（`0x7FFB8A341E40+`）：

| 槽 | VA | 符号 |
|---|---|---|
| +0x20 | `…7A20` | `IsRisingByToss` |
| +0x28 | `…7A30` | **`SetDeadType`** |
| +0x30 | `…7A40` | `GetReviveList` |
| +0x38 | `…7A50` | `GetDeadType` |

---

## 2. 写点穷举

| 手段 | 结果 |
|---|---|
| `find_bytes`：`89 91/81 B4 01 00 00` 等 | 命中多处他类（颜色 ARGB / Inventory…）；**Mob 仅 `SetDeadType`** |
| `Mob`/`MobPool` 名函数扫 `+1B4h]` | write = 1（setter）；read = Update / f6f / getter / `MobPool_d87` |
| `Mob_Init` / `OnDie` / `OnDestructByMiss` / LeaveField | **无** `1B4` 写、**无**对 setter 的 E8 |
| `SetActive` 对照 | 有明确 `call Mob_SetActive` xref → setter 被直调在本仓是常态；`SetDeadType` 缺失 xref 属异常簇（含 `GetReviveList`） |

**默认值**：字段无显式 Init 写 → 新对象一般为 **0**（分配清零假设；未跑分配器实证 → 标推断）。

---

## 3. 读侧与关联

### 3.1 Update（`Mob_ca347…`）

- 种子：`dword_7FFB8A2C7EE4` 实读 + `IMM 0xDD543FC2` → **0**。  
- `deadType != 0` 才走 `Mob_MakeHpIndicator`；`==0` 走旁路。  
- 与「活怪画血条」直觉相反的表象：更像 **非 0 态仍要刷延迟 HP/死亡表现**；**不**据此改写 `FillLite` 语义。

### 3.2 Field Update → `MobPool_d87ed2…`

- 父函数 `sub_7FFB84853460` 同帧还调 `NpcPool_*` / `DropPool_*` / `ChatManager_OnUpdate` → **场景/Field 周期更新**，不是 Leave 包。  
- `deadType > 1` 跳过 `Mob_e6c1…`；`≤1`（含 0、1）调用之（`edx=0`）。  
- `Mob_e6c1…` 内有 `_suspended=true` 写点（见 P0d §2.3）——须视为 **函数内条件分支**，不能说「每帧凡 deadType≤1 必挂起」。

### 3.3 `Mob_f6f081…`

- 调用方：`Mob_Init` / `SetTemporaryStat` / `Mob_c8e47…` / `Mob_afd9…`。  
- `deadType == 0` 与 `!=0` 分路；非 0 路读 `_suspended`。

### 3.4 DelayedDead

- `MobPool_GetDelayedDeadMob`：只碰池对象 `+0x18` 列表，**不读/写** `0x1B4`。  
- 调用方含 `DropPool_*`（掉落相关消费延死列表）。  
- **≠** `SetDeadType` 调用链。

### 3.5 命中环

- `FindHitMobInRect` / 多数 FindHit*：**不读** `_deadType`（P0c）。  
- `FindHitUndeadMobInRect`：存在（名称暗示 undead 特判）；本轮未深拆其与 deadType 关系。

---

## 4. 与 Leave / FillLite / 开火

```text
EnterField → 池内 Mob（deadType 默认 0）
    │
    ├─ SetDeadType(n) ???  ← 静态调用方 BLOCKED
    │       └─ FillLite: n!=0 则拒
    │       └─ FindHit: 仍可能收（不读 deadType）
    │
    └─ LeaveField
            ├─ leaveMode==0 立即离
            └─ leaveMode!=0 → DelayedDead 列表
            └─ 两路均 dict Remove · 不写 deadType
```

**实操含义**：

| 场景 | deadType 门是否关键 |
|---|---|
| 怪已 Leave 出主池 | 否（dict 已无） |
| 怪仍在主池且 `SetDeadType` 曾写入非 0 | 是（挡 `FillLite`；不挡官方 FindHit） |
| 静态能否证明「死亡必 SetDeadType」 | **否**（本轮） |

---

## 5. 种子验算（运行时 dump）

| 标签 | IMM | seed VA | seed 实读 | `(IMM+seed) mod 2^32` |
|---|---|---|---:|---:|
| Update cmp | `0xDD543FC2` | `0x7FFB8A2C7EE4` | `0x22ABC03E` | **0** |
| Pool cmp | `0xA60697DE` | `0x7FFB8A2C9720` | `0x59F7F0A3` | **1** |
| f6f cmp | `0xB15BEF36` | `0x7FFB8A2C7D20` | `0x4EA3F4CA` | **0** |

---

## 6. 仍待

| # | 项 | 建议 |
|---:|---|---|
| 1 | `SetDeadType` 真实调用方与 `edx` 枚举 | ✅ 方案见 [`P0f`](P0f_SetDeadType观察方案.md)；**实机 NOT RUN** |
| 2 | `deadType==1` vs `>1` 的内容差异 | 结合 `Mob_e6c1` 条件块 + P0f 直方图 |
| 3 | `GetReviveList` / `_revive` 与 deadType 时序 | 同簇无 xref；P0f 场景 E 可顺带 |
| 4 | 实机：死亡瞬间主池内 `deadType` 是否非 0 | P0f §6 与 Leave 时序判据 |

---

## 7. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-06 | 初稿：setter 钉名；写点穷举；读侧三处种子；Leave/DelayedDead 切割；调用方静态 BLOCKED |
| 2026-08-06 | 链到 [`P0f`](P0f_SetDeadType观察方案.md) 运行时观察设计 |
