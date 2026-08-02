# attack_speed P0b · 双速系统与字段表

> **状态**：✅ 静态结案  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：runtime dump · 常量均 `get_int` 实读种子  
> **上级**：[`模块设计.md`](模块设计.md)

---

## 1. 两套速度（SETTLED）

```text
                    ┌─ A: GetActionSpeed → PrepareActionLayer → 忙锁解锁
出刀观感 ──────────┤
                    └─ B: WeaponTier + Booster → GetDamageDelay → 伤害结算节奏
                         （不读入 Prepare；不解锁 +0x118）
```

---

## 2. A 系统 · ActionSpeed

### 2.1 `SecondaryStat_GetActionSpeed`

| | |
|---|---|
| VA | `0x7FFB178952B0` |
| RVA | `0xD552B0` |
| 原名 | `sub_178952B0` |

```text
speed = (+80  if +84 >= 0 else 0) + Max(+84, TwoState[1].nOption)  // Dash_Speed
if (+1BC != 0) speed = +1BC     // 绝对覆盖
→ 返回后 Prepare 再 clamp [70, 140]
```

阈值阈值：

| 比较 | 解出 |
|---|---|
| `+84 < 0` 丢弃 `+80` 贡献 | 阈 **0** |
| `+1BC != 0` 覆盖 | 阈 **0** |

### 2.2 玩家 SecondaryStat 字段（攻速相关）

宿主：`LocalUser` / `WorldManager+0xF0`（**勿用 CMS +0xB8**）。

| 偏移 | 语义 | 写入 |
|---|---|---|
| `+80` | 基速（默认 **100**） | `SetFrom`：默认 / 装备累加 / Forced 覆盖 + clamp |
| `+84` | **CTS_Speed.n** | `DecodeSpeed`（CTS **7**） |
| `+88` | Speed.r | DecodeSpeed |
| `+8C` | Speed.t = now + dur | DecodeSpeed |
| `+94/+98/+9C` | Jump n/r/t | DecodeJump（CTS 8）· **非** Booster |
| `+BC/+C0/+C4` | **CTS_Booster** n/r/t | `DecodeBooster`（CTS **11**） |
| `+1B8/+1BC/+1C0` | 绝对速度块（n 在 `+1BC`） | `OpRecv_0044` 等；`+1C4` 为 t（CheckByTime） |
| `+450` | TwoState TempStat 数组 | ctor：`0 EnergyCharged`，`1 Dash_Speed`，`3 RideVehicle`，`4 PartyBooster`，`5 GuidedBullet` |

**警告**：`dump.cs` 的 `MobStat.nSpeed@+88` 是怪物布局，**不要**当成玩家 SecondaryStat。

### 2.3 ForcedStat → `+80`

`ForcedStat`（`LocalUser+0xF8`）：

| 偏移 | 字段 |
|---|---|
| `+38` | `nSpeed` |
| `+3C` | `nSpeedMax` |
| `+40` | `nSTR`（**不是** speed；早期误读已纠正） |

`SecondaryStat_SetFrom`（VA `0x7FFB1789A0C0`）中 `+80` 流水：

```text
默认 100
  → 装备/选项累加（含 OR-merge 辅助）
  → if Forced.nSpeed(+38) > 0:  +80 = nSpeed     // 覆盖，非相加
  → +80 = Min(Max(+80, 100), nSpeedMax!=0 ? nSpeedMax : 140)
```

种子：`0x13A9D2BA ^ seed = 100`；各 Forced 字段门控阈 **0**；封顶默认 **140**。

触发全量 `SetFrom`：`OpRecv_002C` Forced 变更 → `LocalUser_RebuildStats_FromForced`（`0x7FFB1792C6C0`）→ `BasicStat_SetFrom` + `SecondaryStat_SetFrom`。  
`002A` / `002B` 收尾也常调 Rebuild。

### 2.4 CTS 表槽（Decode / Reset）

表基：Decode `0x7FFB1D3F4E10` · Reset `0x7FFB1D3F50A0`（槽距 8）。

| CTS | Decode | Reset（只清 n/r） |
|---|---|---|
| 7 Speed | `DecodeSpeed` → `+84/+88/+8C` | `ResetSpeed_n_r` @ `0x7FFB178B5880` |
| 8 Jump | → `+94/+98/+9C` | `…5910` |
| 11 Booster | `DecodeBooster` → `+BC/+C0/+C4` | `ResetBooster_n_r` @ `0x7FFB178B5A80` |

Runtime 派发：Decode MethodInfo 字典在 TypeInfo 静态 **`+0x30`**；Reset 在 **`+0x40`**。  
索引数组在静态 **`+0x38`**（`[arr+r15*4+20h]`）。

---

## 3. B 系统 · 武器档与伤害延迟

### 3.1 `UserBase+0x15C` WeaponAttackSpeed

| | |
|---|---|
| 默认 | **6**（无武器时种子解出） |
| 有武器 | `NotifyAvatarModified`：`[+0x15C] = [item+0x40]` |
| Prepare | **零读取** |

### 3.2 `CalcWeaponAttackSpeedTier`

| | |
|---|---|
| VA | `0x7FFB180CA030` |

```text
tier = clamp(WAS(+15C) + SecondaryStat(+0xBC) + PartyBooster.nOption, 2, 10)
// skillId == 4001334: 特殊 WAS 变换后进公式
delay = (tier + 10) * frameDelay >> 4   // LocalUser_GetDamageDelay
```

Booster `n` 通常为负，拉低 tier → 缩短伤害延迟；**不改变**忙锁解锁。

---

## 4. 与 A 系统的交叉

| 写入 | 影响 GetActionSpeed？ | 影响 GetDamageDelay？ |
|---|---|---|
| `+80` / `+84` / Dash / `+1BC` | ✅ | 否（直接） |
| `+BC` Booster | 否 | ✅ |
| `+0x15C` WAS | 否 | ✅ |
| Forced `nSpeed` | 经 `+80` ✅ | 否 |

---

## 5. NOT RUN

- 实机读 `+80/+84/+1BC/+BC/+15C` 在吃 Speed/Booster buff 前后的值
- PartyBooster TwoState 运行时 nOption 采样
