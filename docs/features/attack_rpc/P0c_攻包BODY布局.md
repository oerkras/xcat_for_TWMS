# attack_rpc P0c · 攻包 BODY 布局（现网 IDA Encode 序）

> **状态**：✅ 现网静态结案（Melee 50 头+命中环；Normal 落空对照）· 多怪环已见循环 · BIN 实机复核 NOT RUN  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB74A20000`**  
> **真源**：`UserLocal_TryDoingMeleeAttack` / `TryDoingNormalAttack` 内 `OutPacket.Encode*` 调用序（种子已 `get_int`/`ida_bytes` 解混淆）  
> **上级**：[`模块设计.md`](模块设计.md) · [`P0b_出站Encode与Send锚点.md`](P0b_出站Encode与Send锚点.md)

---

## 0. 结论先行

1. **不要用旧 `send.log` 当字段真源**（旧探针/旧客户端）。字段序以现网 Encode 为准。  
2. `Create(ClientPacket)` 常量已解混淆：  
   | 函数 | RVA | opcode |
   |---|---:|---:|
   | `UserLocal_TryDoingNormalAttack` | `0x10C59D0` | **50** |
   | `UserLocal_TryDoingMeleeAttack` | `0x10B0EE0` | **50** |
   | `UserLocal_TryDoingShootAttack` | `0x1048F80` | **51** |
   | `UserLocal_TryDoingMagicAttack` | `0x1082020` | **52** |
3. 发包：`Network_SendOutPacket` **`0x1CB7CE0`** → 内部 `HashSet.Contains` 后调 `NetworkManager_SendPacket@0x1CB98B0`。  
4. `flags` 低 4 位 | **`mobCount<<4`**：命中 `0x11` = count=1 + 低位 1；落空 `0x01` = count=0。  
5. 命中环按 `List<AttackInfo>` 迭代；单怪 damages 循环 `AttackCount` 次 `Encode4`。

---

## 1. 锚点表（2026-08-03）

| 符号 | RVA | VA |
|---|---:|---|
| `UserLocal_TryDoingNormalAttack` | `0x10C59D0` | `0x7FFB75AE59D0` |
| `UserLocal_TryDoingMeleeAttack` | `0x10B0EE0` | `0x7FFB75AD0EE0` |
| `UserLocal_TryDoingShootAttack` | `0x1048F80` | `0x7FFB75A68F80` |
| `UserLocal_TryDoingMagicAttack` | `0x1082020` | `0x7FFB75AA2020` |
| `OutPacket_Create` | `0x1CB7BB0` | （P0b） |
| `OutPacket_EncodeVector2` | `0x1CC5090` | `BitConverter.GetBytes`×2 + `BlockCopy` |
| `Network_SendOutPacket` | `0x1CB7CE0` | → `NM.SendPacket` |
| `User_SetAttackAction` | `0xFD39C0` | 两路径均先调 |

位移校验：旧 `TryDoingNormalAttack@0x10C70B0` − `0x16E0` = **`0x10C59D0`**（与 `SetAttackAction` 同代差）。

---

## 2. Melee 公共头（`TryDoingMeleeAttack` · Create→命中环前）

Create 位点 `0x10B60FB`；随后 Encode（`rdi/rbx` = OutPacket*）：

| # | API | 语义（IDA） |
|---:|---|---|
| 1 | `Encode1_byte` | 全局单例 `+0x98`（portal/序号类） |
| 2 | `Encode1_byte` | **`flags`**：`(nibble) \| (mobCount<<4)` |
| 3 | `Encode4_int` | **skillId** |
| 4 | `Encode4_int` | 技能附属（无则 0；有 SkillEntry 时走 helper） |
| 5 | `Encode1_bool` | 常量 **false** |
| 6 | `Encode2_ushort` | **action \| (left<<15)** 打包 |
| 7 | `Encode1_byte` | `UserLocal+0x158` |
| 8 | `Encode1_byte` | 动作/武器相关字节 |
| 9 | `Encode4_int` | 时间戳/密钥（`tOrKey`） |

头长至此后：**19 字节**（与旧 BIN 头长相合，但中间字段语义以本表为准，勿再抄旧「pad5」解读）。

`TryDoingNormalAttack` 落空路径同构，但：  
- #2/#5 等用种子常量写出（`0xA7^0xA6→1`，`0xBE+0x42→0`，skill 相关两枚 `Encode4` 解出 **0**）；  
- 无命中环；结尾用两次 `Encode2_short` 写 **玩家 XY**（float→int），再 `SendOutPacket`。  
BODY≈23 → `DataPos off≈29`。

---

## 3. 命中环（每只怪 · `AttackInfo`）

条件：`mobIndex < mobCount`。元素 = `List.get_Item` → `AttackInfo*`（`r12`）。

| # | API | 源 | CMS 对照 |
|---:|---|---|---|
| 1 | `Encode4_int` | `Mob*+0x134` | Mob oid（非 `AttackInfo.MobID` 槽；从 `AttackInfo.Mob` 间接） |
| 2 | `Encode1_byte` | `AttackInfo+0x20` | `HitAction` |
| 3 | `Encode1_byte` | `+0x24` 与 bool 打包 | `ForeAction` + 谓词位 |
| 4 | `Encode1_byte` | `AttackInfo+0x28` | `FrameIdx` |
| 5 | `Encode1_byte` | Mob 朝向/状态打包 | （wire 独有） |
| 6 | `EncodeVector2` | Mob 位置 vtable | ≈ `PositionHit` 一侧 |
| 7 | `EncodeVector2` | `Mob+0x6C` | 第二点（hit/旧坐标） |
| 8 | `Encode1_byte` | `AttackInfo+0x30` | `AttackCount` |
| 9 | `Encode4_int` ×N | `Damages[i]` | `Damages[]` |
| 10 | `Encode4_uint` | `UserLocal_GetFieldID` | 场 ID |

环后还有：玩家 `EncodeVector2`、若干 `Encode1` / 可选 `Encode2_short`，再 `Network_SendOutPacket`。

**单怪 × `AttackCount=1` 粗算**：4+1+1+1+1+8+8+1+4+4 = **33 B/怪**（≠ 旧 BIN「+26」假设——以本表为准）。

多怪：同一环 `inc` 后跳回 `List.get_Item`（`0x10B63B0` 一带），**静态已见循环**，无需再猜「+26×N」。

---

## 4. Magic / Shoot 差异（摘要）

| | Magic `0x1082020` | Shoot `0x1048F80` |
|---|---|---|
| Create | **52** | **51** |
| 相对 Melee | 头里多一枚 `Encode4`（技能附属/keydown 类） | 头里多 `Encode4` + 两枚 `Encode2`（子弹起点 XY） |
| Send | 同 `Network_SendOutPacket` | 同 |

细字段命名留 P0c+ / 实机 BIN 对齐。

---

## 5. 常量解混淆样例（Normal 落空头）

种子均在 `0x7FFB7B264xxx`（运行时填充；**必须实读**，勿抄本文瞬时值当永久常量）：

| 写法 | 本轮实算 |
|---|---|
| `movzx ecx, word_…264154; add ecx, 0FFFF91A7h` | → **50** |
| `byte_…26413C ^ 0xA6` | → **1** |
| `0x92E53601 + dword_…264138` | → **0**（skillId） |
| `0xBE6938C8 ^ dword_…264134` | → **0** |
| `byte_…264130 + 0x42` | → **0** |

---

## 6. 对 P1 伪造的含义

| 做法 | 评估 |
|---|---|
| 调官方 `TryDoingMeleeAttack` 下游「已组好 List 后的 Encode 段」 | 最稳，但仍可能撞 `SetAttackAction` |
| 自建 `OutPacket.Create(50)` + 按 §2–3 Encode + `Network_SendOutPacket` | 对齐同行「跳门」；**禁止**旁路到 Session.Send |
| 多怪 | 重复 §3 环，改 `flags` 高 nibble = count |

---

## 7. 产物

| 路径 | 说明 |
|---|---|
| 本文 | Encode 序真源 |
| `Dumps/runtime/P0C_ATTACK_BODY_20260803.tsv` | 机器表 |
| IDA 命名 | 上表函数 / `OutPacket_EncodeVector2` / `Network_SendOutPacket` |

---

## 8. NOT RUN

- 现网 `kick_sniff@NM.SendPacket` 实机 BIN 与本文逐字节对齐  
- Shoot/Magic 头字段逐项命名  
- `EncodeVector2` 两处坐标语义（当前 vs hit）最终定名  
- `ForeAction` 打包位精确掩码  
