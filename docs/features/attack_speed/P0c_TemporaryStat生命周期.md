# attack_speed P0c · TemporaryStat 生命周期

> **状态**：✅ 静态结案  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：runtime dump  
> **上级**：[`模块设计.md`](模块设计.md)

---

## 1. 总览

```text
服端下发 buff
  OpRecv_002A → DecodeForLocal → DecodeSpeed/Booster → 写 n/r/t
  → RebuildStats_FromForced（重算 +80，不清 +84）

客户端过期检测
  FixedUpdate → CheckByTime
  → Speed/Booster：不参与
  → 其它 CTS / +1BC：过期则置 BitSet
  → 非空则 OutPacket 通知（不编码具体 BitSet）

服端清场
  OpRecv_002B（主）/ OpRecv_00E8（旁路）
  → BitSet128_Decode
  → SecondaryStat_Reset → 按位 call Reset*（清 n/r）
  → RebuildStats_FromForced
```

玩家路径 **不是** `LocalUser_SetTemporaryStat`（那是 Mob / `OpRecv_0117`）。

---

## 2. 下发 · `OpRecv_002A`

| | |
|---|---|
| VA | `0x7FFB1798D5D0` |
| IDA 名 | `OpRecv_002A_TemporaryStat_Set` |

```text
new Dictionary + List
SecondaryStat_DecodeForLocal(TS @ LocalUser+0xF0, pkt, dict, list)
  → 返回已应用 BitSet
Decode2（附加）
若干 UI / 技能 / 坐骑旁路（IsHas 掩码不含 CTS 7/11）
LocalUser_RebuildStats_FromForced
若 IsCalcDamageStat → OutPacket(opcode 0x7B)
```

### 2.1 `SecondaryStat_DecodeForLocal`

| | |
|---|---|
| VA | `0x7FFB17896BC0` |
| RVA | `0xD56BC0` |

循环语义（与 Reset 对称）：

1. `esi = CTS索引数组[i]`
2. `BitSet_Create(esi)` ∧ 包内 flag → 非空则继续
3. `Dictionary.get`（静态 **`+0x30`** MethodInfo）→ `call rax` 进 Decode*

### 2.2 `DecodeSpeed` / `DecodeBooster`

| | Speed (CTS 7) | Booster (CTS 11) |
|---|---|---|
| VA | `0x7FFB178ABF50` | `0x7FFB178AC830` |
| n | `Decode2 → +84` | `Decode2 → +BC` |
| r | `Decode4 → +88` | `Decode4 → +C0` |
| t | `now + Decode4 → +8C` | `now + Decode4 → +C4` |

`now` = `Util_GetNow`（`0x7FFB178FE0C0`）。  
`t` 的 `now+dur` 经 MBA 混淆，语义为整数加法。

远程：`DecodeForRemote`（`0x7FFB17898090`）对 Speed 有 bit7 → `Decode1→+84` 等简化路径。

---

## 3. 过期检测 · `CheckByTime`

| | |
|---|---|
| VA | `0x7FFB178A0F90` |
| RVA | `0xD60F90` |
| 调用方 | `sub_7FFB17910F50` ← FixedUpdate 一带 |

### 3.1 单条 CTS 语义

```text
if n > 0:                          // 阈 0
  MBA(now, t) 与 -100 比较         // 过期判定（种子实读 −100）
  → 过期则 BitSet OR 对应 CTS 掩码
```

**不**在客户端调用 Reset。

### 3.2 实际扫描的字段（无 Speed / Booster）

从 `rbx`（SecondaryStat）读取的 `(n,t)` 对包括：

| n / t | CTS 静态 mask（例） |
|---|---|
| `+104/+10C` … `+128/+130` 等 | `0x110`… |
| `+1A4/+1AC`、`+1B0/+1B8` | … |
| **`+1BC/+1C4`**（绝对速度） | **`0x200`** |
| `+220/+228`、`+278/+280`、`+2AC/+2B4` | … |
| `+3D0/+3D8`、`+3DC/+3E4`、`+3E8/+3F0`、`+434/+43C` | … |

对 `+84/+8C`、`+BC/+C4`：**0 处访问**。

### 3.3 过期通知包

`CheckByTime` 结果 `IsHas` 为真 → `OutPacket_Create` → `SessionTcp` 发送。

- 包内 **不编码** 过期 BitSet（仅「有过期」类通知）
- opcode：`movzx ecx, word_seed; add ecx, imm` → 存 `uint16` 到 `OutPacket+0x20`  
  - 研究实算：**0x71（113）**（与 `di` 写入一致；以 runtime 种子为准）
- 另有基于 `User+0x1B0` 的时间门后再发

服端据此可回 `002B` 指明清哪些位。

---

## 4. 清场 · `OpRecv_002B` / `00E8`

| 包 | VA | IDA 名 |
|---|---|---|
| `002B` | `0x7FFB17990320` | `OpRecv_002B_TemporaryStat_Reset` |
| `00E8` | `0x7FFB17CD53D0` | `OpRecv_00E8_TemporaryStat_Reset_alt` |

```text
BitSet128_Decode(pkt)
SecondaryStat_Reset(bitset)
  → 循环 CTS 索引；bit 命中 → Reset* (n/r=0)
LocalUser_RebuildStats_FromForced
若 IsCalcDamageStat → OutPacket(0x7B)
```

### 4.1 `SecondaryStat_Reset`

| | |
|---|---|
| VA | `0x7FFB17896350` |
| RVA | `0xD56350` |

- MethodInfo 字典：静态 **`+0x40`**
- `ResetSpeed`：清 `+84/+88`（**不清** `+8C`）
- `ResetBooster`：清 `+BC/+C0`（**不清** `+C4`）

### 4.2 `0x7B` 回包

`002A` / `002B` 在 `IsCalcDamageStat(剩余/应用 BitSet)` 为真时发同一类包：

- `002A`：`movzx ecx, word; xor ecx, 0xB915` → **123（0x7B）**
- 与伤害统计相关的客户端→服端同步，非 Speed 专用

---

## 5. 对攻速杠杆的含义

| 操作 | 持久性 |
|---|---|
| 写 `+84` | 直到服端 `002B` 带 Speed 位，或新 `002A` 覆盖 |
| 写 `+80` | 下次 `RebuildStats`/`SetFrom` 可被 Forced/装备重算覆盖 |
| 写 `+1BC` | 进 `CheckByTime`；可能触发过期通知 |
| 写 `+BC` | 同 Speed：客户端不自清；等 `002B` |
| 只 hook `ResetSpeed` | 服端不下位则本地残留；下位则仍被清 |

`002A` **不**改 `+0x118` / 当前 `layer+0x14`——已出手的一刀节奏不变，**下一刀** Prepare 才吃新 speed。

---

## 6. 符号重命名清单（IDA）

| 名 | VA |
|---|---|
| `SecondaryStat_GetActionSpeed` | `0x7FFB178952B0` |
| `SecondaryStat_DecodeForLocal` | `0x7FFB17896BC0` |
| `SecondaryStat_DecodeForRemote` | `0x7FFB17898090` |
| `SecondaryStat_DecodeSpeed_n_r_t` | `0x7FFB178ABF50` |
| `SecondaryStat_DecodeBooster_n_r_t` | `0x7FFB178AC830` |
| `SecondaryStat_ResetSpeed_n_r` | `0x7FFB178B5880` |
| `SecondaryStat_ResetBooster_n_r` | `0x7FFB178B5A80` |
| `OpRecv_002A_TemporaryStat_Set` | `0x7FFB1798D5D0` |
| `OpRecv_002B_TemporaryStat_Reset` | `0x7FFB17990320` |
| `OpRecv_00E8_TemporaryStat_Reset_alt` | `0x7FFB17CD53D0` |
| `LocalUser_RebuildStats_FromForced` | `0x7FFB1792C6C0` |
| `LocalUser_SetTemporaryStat` | `0x7FFB17A44990`（Mob） |
| `Util_GetNow` | `0x7FFB178FE0C0` |

---

## 7. NOT RUN

- 抓包对齐：过期通知 opcode ↔ 服端 `002B` 时序
- Speed buff 到期时客户端是否只靠服端清、本地 `+84` 残留时长
- `Indie` / 其它绝对速度 opcode 与 `+1BC` 的完整编码
