# mob_pool P0a · Mob 进场包族与 Mob.Init 包体（经典版 TWMS）

> **状态**：🔍 IDA 静态推进中（2026-08-06）· 种子均已 `get_int` 实读  
> **产品**：经典版 TWMS（`Maplestory_Classic.exe`）· **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB83A80000`**  
> **约束**：方案 ⑥ 只考虑 **MI.methodPointer / 数据面观察**，禁止 INLINE HOOK（E9 / `.text`）  
> **上级**：[`活怪n与刷怪槽M.md`](活怪n与刷怪槽M.md)  
> **目标**：为「刷怪事件 → 按需 Collect」提供可挂钩语义与包体真源  
> **命名真源**：`DumpRestoredData/dump.cs.restored` MobPool 段（L66999–67068）

---

## 0. 结论先行

1. **真刷怪入口**是 `OnPacketMobEnterField`（哈希名 `a65fe338…`，RVA `0xF75DD0`），不是先前误称的「OnLocalMob」。
2. **`a4ab8b…`（RVA `0xF76F90`）= `OnPacketMobChangeController`**：换控制器。`flag==0` → **`SetRemoteMob`**（`SetActive(false)`），**不是**离场删除。
3. **真离场**是 `OnPacketMobLeaveField`（RVA `0xF763C0`）。
4. `SetLocalMob` 的 `extra` = `_calcDamageStatIndex@0x1F0`；`flag` 只决定要不要 `ChaseTarget`（`1` 否 / `≥2` 候选）。
5. `Mob.Init` 被 **EnterField** 与 **ChangeController→SetLocalMob 创建路径** 共用；包体前缀已钉（§5）；后半是资源/HP/就绪，**无额外 wire**（§6）。
6. 方案 ⑥：**优先挂 `EnterField` 创建成功 + `LeaveField`（dict Remove）**；`ChangeController` 仅作补集。Leave 的 `Decode1`：`0`=立即离 / `≠0`=DelayedDead（§7）。
7. **`SetRemoteMob` 条件踢池**（§7.6）：`SetActive(false)` 后仅当 `_inViewSplit@0x100==0` 才 `dict.Remove`；EnterField 怪多为 true → **常不踢**。MI 设计见 [`P0b_MI观察与按需Collect.md`](P0b_MI观察与按需Collect.md)（**未落码**）。
8. **Enter→开火门控**见 [`P0c_Enter到开火时间线.md`](P0c_Enter到开火时间线.md)：瓶颈多在贴身/间隔/RTT；官方 `FindHitMobInRect` **要 `_inViewSplit`、不要 MobCtrl/Active**（不必等 SetLocalMob）。

### ⚠ 纠偏（相对本文首版）

| 旧称（已废） | 正确 restored 名 | 含义 |
|---|---|---|
| `OnLocalMobPacket` | `OnPacketMobChangeController` | 换控制器 |
| `RemoveLocalMob`（`c3857b`） | `SetRemoteMob` | 变远程 / `SetActive(false)` |
| （未命名）`a65fe338` | `OnPacketMobEnterField` | **进场刷怪** |
| 工厂 `df1af0` | `CreateMob` | `MobPool.CreateMob(templateId)` |

---

## 1. 锚点表

| 符号（restored / IDA） | RVA | VA |
|---|---:|---|
| `OnPacketMobEnterField`（`a65fe338…`） | `0xF75DD0` | `0x7FFB849F5DD0` |
| `OnPacketMobLeaveField`（`b617db88…`） | `0xF763C0` | `0x7FFB849F63C0` |
| `OnPacketMobChangeController`（`a4ab8b…`） | `0xF76F90` | `0x7FFB849F6F90` |
| `SetLocalMob` | `0xF74340` | `0x7FFB849F4340` |
| `SetRemoteMob`（`c3857b…`） | `0xF74880` | `0x7FFB849F4880` |
| `CreateMob`（`df1af0…`） | `0xF6CDF0` | `0x7FFB849ECDF0` |
| `GetMob`（`ea5dab…`） | `0xF6CD30` | `0x7FFB849ECD30` |
| `Mob.Init` | `0xEFBDF0` | `0x7FFB8497BDF0` |
| `Mob.SetTemporaryStat` | `0xF09F50` | `0x7FFB84989F50` |
| `Mob.SetActive` | — | EnterField **不**调；SetLocalMob 创建调 true；SetRemoteMob 调 false |
| `Mob.ChaseTarget` | — | SetLocalMob，`flag≥2` |
| `Mob.LoadEffectLayer` | — | Init，`effectId>0` |
| `Mob` 谓词 `c282…`（读 `MobData.IsDamagedByMob@0xA9`） | `0xF047F0` | EnterField 尾 |
| `VecCtrlMob_Init` | — | `0x7FFB84C419E0` |
| `InPacket.DecodeVector2`（`e37a…`） | — | `0x7FFB8574CCC0` |
| `InPacket.Decode2`（`b597…`） | — | `0x7FFB8574C200` |

`Mob.Init` 调用方（代码 xref）：**仅** `SetLocalMob` + `OnPacketMobEnterField`。

---

## 2. 包族总览

```text
EnterField          → 怪进本地视野（真刷怪）
LeaveField          → 怪离开本地视野（真消失）
ChangeController    → 本地控 / 远程控 切换
  flag==0 → SetRemoteMob
  flag≠0 → SetLocalMob（可带 Chase）
```

| 事件 | 对 `mob_scan` / Collect |
|---|---|
| EnterField **创建**（`CreateMob`+`Init`） | ✅ 应触发 |
| LeaveField | ✅ 应失效缓存 |
| ChangeController → SetRemoteMob | ⚠️ **条件踢池**（§7.6：仅 `_inViewSplit==0`）；EnterField 怪常保留；**不要**当成 Leave |
| ChangeController → SetLocalMob 创建 | ✅ 少见补集（控权时本地尚无该 oid） |
| 已有 oid 只 `SetTemporaryStat` | ❌ 通常否 |

---

## 3. `OnPacketMobEnterField`（真刷怪）

### 3.1 包头（在 `Init` 之前消费）

```text
u32 mobId                 // object id → ebp；lookup / Init 参数
u8  calcDamageStatIndex   // → SetTemporaryStat 的 extra
u32 templateId            // → CreateMob(templateId)
```

**没有** ChangeController 那种 leading `flag`。

### 3.2 流程

```text
lookup(dict@+0x10, mobId)
if 已有有效对象:
    Mob._inViewSplit@0x100 = true   // byte seed^0xCF → 1
    SetTemporaryStat(mob, pkt, calcIdx, 0)
    // 不 Init
else:
    mob = CreateMob(templateId)
    Insert(dict, mobId, mob)
    Mob._inViewSplit@0x100 = true   // byte seed^0x1C → 1
    SetTemporaryStat(mob, pkt, calcIdx, 0)
    Mob.Init(mob, mobId, pkt)
if MobData.IsDamagedByMob@0xA9:     // 经 c282 谓词
    pool._mobDamagedByMob@0x38 = mob
    pool._timeLastHitMobDamagedByMob@0x40 = <time helper>
```

要点：

- **不调用 `SetActive`**；就绪靠 `Init` 尾写 `IsReady=true`（创建路径）。
- 更新路径只刷 TS + `_inViewSplit`，不重跑 `Init`。
- 无直接代码 xref（仅 RUNTIME_FUNCTION）→ 典型 **MI 虚派发**，适合 methodPointer 观察。

---

## 4. `OnPacketMobChangeController` + `SetLocalMob` / `SetRemoteMob`

### 4.1 派发

```text
u8  flag  = Decode1          // 与 0 比较（seed 已验）
u32 mobId = Decode4
if flag == 0:
    SetRemoteMob(this, mobId)     // 原误称 RemoveLocalMob
else:
    u8 calcDamageStatIndex = Decode1
    SetLocalMob(this, flag, mobId, calcDamageStatIndex, pkt)
```

### 4.2 `SetRemoteMob`

- 查找 mob → 清理辅助 → **`Mob_SetActive(mob, false)`**（`edx=0`）→ 后续 dict/列表 helper。  
- 语义：本地控权交出 / 变远程，**≠ LeaveField**。

### 4.3 `SetLocalMob` ABI

```text
SetLocalMob(MobPool* this, int flag, int mobId, int calcDamageStatIndex, InPacket* pkt)
// edx→esi=flag · r8d→ebp=mobId · r9d→ebx=calcIdx · [arg]→r15=pkt
```

| 项 | 结论 |
|---|---|
| `extra` | `_calcDamageStatIndex@0x1F0`（经 STS 写入） |
| `flag==1` | 不 Chase |
| `flag≥2` | Chase 候选（另有 UserLocal 门；种子解出阈=1 / 比较=0） |
| 已有 oid | 只 `SetTemporaryStat` |
| 无 oid | `Decode4 templateId` → `CreateMob` → Insert → STS → **`Init` → `SetActive(true)`** → 可选 Chase |

Chase 解混淆：

```text
0x7F65D618 ^ dword@98C4(0x7F65D619) = 1
0x360BEFB6 ^ dword@98D4(0x360BEFB6) = 0
```

---

## 5. `Mob.Init` 包体前缀（wire）

> 前置：调用方已消费 `templateId`；`_mobId@0x134` 来自参数。  
> EnterField / SetLocalMob 创建路径共用此后前缀。

| # | API | 宽 | 落入 | 置信 |
|---:|---|---:|---|---|
| 1 | `DecodeVector2` | 4 | `Pos@0x64` + `PosPrev@0x6C` | ✅ |
| 2 | `Decode1` | 1 | `MoveAction@0x58` | ✅ |
| 3 | `Decode2` | 2 | foothold id₀ | ✅ |
| 4 | `Decode2` | 2 | foothold id₁ → `VecCtrlMob_Init` | ✅ |
| 5 | `Decode1`（有符号） | 1 | `_summonType@0x114` + appear switch | ✅ |
| 5b | 条件 `Decode4` | 4 | linked mob oid → `GetMob` | ✅ `st<0 && st!=-3` |
| 6 | `Decode1` | 1 | `_teamForMCarnival@0x158` | ✅ |
| 7 | `Decode4` | 4 | effect id → `LoadEffectLayer`（`>0`） | ✅ |

`summonType` switch 索引：`-4→0 … -1→3`；`≥0` default。  
`VecCtrlMob_Init` → `MoveAbility@0x100`、`m_nHomeMass@0x104`（`MobData.MoveAbility@0x34` 常作其一来源）。  
Pos 不是视觉真源（见 teleport P0c）。

---

## 6. `Mob.Init` 后半（非 wire · 本轮）

包体 Decode 结束后的装载/UI（对 Collect 非阻塞，但影响 `IsReady`/血条）：

| 步骤 | 行为 | 字段/备注 |
|---|---|---|
| Wz / 资源 | `MsResourceManager` + `WzJsonNode_*` 读模板节点 | 外观/特效配置 |
| 延迟/悬浮 | 写 `_initDelay@0x1B0`、`_suspended@0x1B8` | 字节种子写出 **true**；寿命见 [`P0d`](P0d_suspended与initDelay.md) |
| Tremble | `AnimationPlayer_EffectTremble` | 进场震动 |
| summon 回写 | `[rsi+114h]=r14`（再次确认 `_summonType`） | |
| 动作层 | `SpriteAnimation_SetMoveAlpha` 等 | |
| 大块 helper | `Mob_d2dce1…` / `Mob_f6f081…(0)` | 体积极大；偏动画/状态表，未逐块拆 |
| 控权/蓝 | `_mobCtrlState@0x104 = -1`；`_mp@0x148 = 0` | 常量已解 |
| 清 hit 窗 | `qword [rsi+160h]=0`（`_hitExpire` 一带） | |
| HP 条 | `MakeHpIndicator(100, 0xFFFF0000)` | 满血百分比 + 颜色打包 |
| effect | `effectId>0` → `LoadEffectLayer` → `_effectLayer@0x1F8` | |
| 就绪 | `IsReady@0xEC = true` | `byte@7B94 ^ 0x5A = 1` |

**本段无新的 `InPacket_Decode*`。**

---

## 7. `OnPacketMobLeaveField` · Decode1 语义（本轮钉死）

### 7.1 包头

```text
u32 mobId     = Decode4   → edi；dict lookup / 最终 Remove
u8  leaveMode = Decode1   → ebp；**只参与一次与 0 的比较**
```

解混淆：

```text
mov eax, 2447DF83h
xor eax, dword_7FFB8A2C9914   ; seed == IMM → 解出 0
cmp ebp, eax                  ; cmp leaveMode, 0
```

分支表（prologue `lea`）：

| `leaveMode` | 跳转 | 路径 |
|---:|---|---|
| **`== 0`** | `loc_7FFB849F6BE9` | **立即离场** |
| **`!= 0`** | `loc_7FFB849F6AAA` | **延迟死亡队列** |

`ebp` 在函数内**仅**出现于 `Decode1` 与上述 `cmp`——**不会**写入 `Mob._deadType@0x1B4`，也**不会**再按 1/2/3…细分（`SetDeadType` 存在但本函数未调）。

### 7.2 `leaveMode == 0`：立即离场（`6BE9`）

1. 若 `Mob._inViewSplit@0x100 != 0`：写回 **`false`**（`byte@9920 + 0xBE → 0`）  
2. 调 `Mob_e5447d2b…`（读 VecCtrl/`Active@0x80` 一类就绪谓词；返回值再混淆分支）  
3. 汇合到公共收尾（§7.4）

语义倾向：**无死亡演出、直接从场上清掉**（扫图/切图/非死亡离开）。

### 7.3 `leaveMode != 0`：延迟死亡（`6AAA`）

1. 把该 `Mob*` **追加**进 `MobPool._listMobDelayedDead@0x18`  
2. `jmp` 汇合公共收尾（跳过立即路径里的 inViewSplit 清理段）

语义倾向：**死亡动画 / 延后销毁**；具体非零数值（1 vs 2 vs …）在本函数内**同路径**，差异若存在应在 DelayedDead 后续处理或他处 `SetDeadType`（**本函数未证**）。

### 7.4 两路公共收尾（均会走到）

| 步骤 | 符号 / 行为 |
|---|---|
| 取消追击 | `CancelChaseTarget`（`bd2102…` @ `0xF73E30`） |
| 清「被怪伤」槽 | 若 `IsDamagedByMob`：`_mobDamagedByMob@0x38 = null` |
| 清 dazzled 槽 | 谓词真则 `_mobDazzledByMe@0x30 = null` |
| **踢主字典** | `sub_7FFB86D9DF60(dict@+0x10, mobId)` ← 离场对 Collect 的硬信号 |
| 其它 | 模板 id 相关 helper 等 |

因此：**无论 Decode1 是否为 0，主池都会 Remove**；区别只是要不要先入 `_listMobDelayedDead`。

### 7.5 对方案 ⑥

| 观察 | 建议 |
|---|---|
| Collect 失效 | 挂 LeaveField 公共尾（dict Remove）即可，**不必**解析 Decode1 |
| 若要区分「死了」vs「直接消失」 | 读 Decode1：`!=0` ≈ 延迟死；`==0` ≈ 立即清 |
| `_deadType` 字段 | **不要**假定本包写入；另寻 `SetDeadType` 调用方 |

---

## 7.6 `SetRemoteMob` 是否踢主字典（本轮钉死）

`ChangeController` 在 `flag==0` 时调用。流程（CFF 展平后净效果）：

```text
lookup(dict, mobId) → out mob
if !valid(mob): return                    // 不踢
if !e544(mob):  return                    // 读 VecCtrl.Active@0x80；假则早退，不踢
Mob.SetActive(mob, false)                 // edx=0
if (Mob._inViewSplit@0x100 << 2) == 0:    // IMM^seed@98E0 = 0x257DA281^0x257DA281 = 0
    dict.Remove(mobId)                    // ★ 同 LeaveField 的 sub_7FFB86D9DF60
else:
    return                                // ★ 不踢池，仅失活
```

| `_inViewSplit` | SetActive(false) 后 | 主字典 |
|---:|---|---|
| **0 / false** | 是 | **踢出**（`86D9DF60`） |
| **≠0 / true** | 是 | **保留** |

与进场的关系：

- `EnterField` 创建/更新都会把 `_inViewSplit` 写成 **true** → 典型进场怪走 SetRemoteMob 时 **多数不踢池**。
- `SetLocalMob` 创建路径（静态未见写 `+0x100`）→ 默认 0 → SetRemoteMob 时 **会踢池**。
- `LeaveField` **无论** leaveMode，公共尾都会踢池（与 inViewSplit 无关）。

对 `mob_scan`：`FillLite` 门控是 `IsReady` / `deadType` / hp，**不读** `VecCtrl.Active`。因此「SetRemote 且不踢池」时，若 `IsReady` 仍真，**可能仍算活怪**（风险，实机待验）。

**方案 ⑥**：离场 Collect 仍以 **LeaveField** 为准；SetRemote **不要**当成 Leave。可选：仅在观测到走到 `DF60` 时补一次 scan（或统一 Leave+Enter 足够）。

---

## 8. 整包对照

### EnterField 创建

```text
mobId:u32
calcDamageStatIndex:u8
templateId:u32
── Init ──
xy / moveAction / fh0 / fh1 / summonType [/linked] / team / effectId
── Init 后半非 wire ──
IsReady=true
```

### ChangeController → SetLocalMob 创建

```text
flag:u8 (≥1)
mobId:u32
calcDamageStatIndex:u8
templateId:u32          // SetLocalMob 内
── Init（同上）──
SetActive(true)
[Chase if flag≥2]
```

### LeaveField

```text
mobId:u32
leaveMode:u8            // 0=立即离场；≠0=入 DelayedDead（数值本身不再细分）
→ CancelChaseTarget + dict Remove（两路皆然）
```

---

## 9. 方案 ⑥ 挂钩建议（修订）

| 优先级 | 挂点 | 触发 Collect |
|---:|---|---|
| **P0** | `OnPacketMobEnterField` 创建尾（`Init` 返回后 / `IsReady` 已真） | ✅ |
| **P0** | `OnPacketMobLeaveField` dict Remove（`86D9DF60`）或函数尾 | ✅ |
| P1 | `SetLocalMob` 创建尾（`SetActive(true)` 后） | ✅ 补集 |
| — | `SetRemoteMob` | ❌ 默认不当离场（§7.6：常不踢池） |
| — | 仅 STS / Chase / effect | ❌ |

可读字段：`mobId@0x134`、`templateId@0xB0`、`Pos@0x64`（或随后 VecCtrl Ap）。Leave 可选读 `leaveMode` 仅作日志分类。

---

## 10. 种子账本

| 位点 | 运算 | 种子 | 解出 |
|---|---|---|---:|
| ChangeController flag==0 | xor | `@992C` | `0` |
| SetLocalMob Chase 阈/比较 | xor | `@98C4` / `@98D4` | `1` / `0` |
| Init summon −3 / 0 | add/xor | `@7B64` / `@7C00` | `-3` / `0` |
| effectId>0 | add | `@7B90` | `0` |
| oneTimeAction | add | `@7B6C` | `4` |
| IsReady | xor byte | `@7B94^0x5A` | `1` |
| `_mobCtrlState` | add | `@7B8C` | `-1` |
| `_mp` | add | `@7B84` | `0` |
| MakeHpIndicator 参 | xor/add | `@7BA0`/`@7BA4` | `100` / `0xFFFF0000` |
| EnterField `_inViewSplit` | xor byte | `@9910^0xCF` / `@990C^0x1C` | `1` / `1` |
| **LeaveField leaveMode==0** | xor | `@9914` | **`0`** |
| LeaveField `_inViewSplit=false` | add byte | `@9920+0xBE` | `0` |
| **SetRemote `_inViewSplit<<2`** | xor | `@98E0` · IMM `0x257DA281` | **`0`**（`shl 2` 后比 0 → 仅 false 踢池） |

---

## 11. 未决

| 项 | 状态 |
|---|---|
| IDA 批量改名（Enter/Leave/ChangeController/SetRemote/CreateMob/GetMob） | 未做（等你点头） |
| LeaveField `leaveMode` 非零枚举（1/2/…）是否他处消费 | 本函数不分；DelayedDead≠`SetDeadType`；写入见 [`P0e`](P0e_SetDeadType与deadType.md)（调用方静态 BLOCKED） |
| SetRemoteMob 是否踢 dict | ✅ **条件踢**：仅 `_inViewSplit==0` 时 `86D9DF60`；EnterField 怪多为 true → 常不踢 |
| `Mob_d2dce1` / `f6f081` 内部分块 | 低优先级 |
| fh0/fh1 ↔ MoveAbility/HomeMass 精确对应 | 可再钉 |
| 实机抓包 | **NOT RUN** |
| MI 补丁产品化 | 设计见 [`P0b`](P0b_MI观察与按需Collect.md)；**代码未开工** |

---

## 12. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-06 | 首版（当时误把 ChangeController 当成 OnLocalMob，c3857b 误称 Remove） |
| 2026-08-06 | **纠名**：EnterField / LeaveField / ChangeController / SetRemoteMob / CreateMob；补 EnterField 流程；Init 后半；修订方案 ⑥ 挂钩 |
| 2026-08-06 | **LeaveField Decode1**：解出与 `0` 比较；`0`=立即离场 / `≠0`=DelayedDead；两路均 dict Remove；不写 `_deadType` |
| 2026-08-06 | **SetRemoteMob 踢池**：`SetActive(false)` 后仅当 `_inViewSplit==0` 才 `Remove`；EnterField 写入 true → 常保留 |
