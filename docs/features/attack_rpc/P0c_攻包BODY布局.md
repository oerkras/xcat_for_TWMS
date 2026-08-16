# attack_rpc P0c · 攻包 BODY 布局（现网 IDA Encode 序）

> **状态**：✅ 现网静态结案 · **实机 hex 修正已写入 §9** · 掉血验收仍待勾选  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB74A20000`**  
> **真源**：`UserLocal_TryDoingMeleeAttack` / `TryDoingNormalAttack` 内 `OutPacket.Encode*` 调用序（种子已 `get_int`/`ida_bytes` 解混淆）  
> **上级**：[`模块设计.md`](模块设计.md) · [`P0b_出站Encode与Send锚点.md`](P0b_出站Encode与Send锚点.md)

---

## 0. 结论先行

1. **字段序以现网 Encode 为准**；**wire 字节以现网 `send.log` op=50 为准**（二者有张力时听 hex，见 §9）。  
2. `Create(ClientPacket)` 常量已解混淆：  
   | 函数 | RVA | opcode |
   |---|---:|---:|
   | `UserLocal_TryDoingNormalAttack` | `0x10C59D0` | **50** |
   | `UserLocal_TryDoingMeleeAttack` | `0x10B0EE0` | **50** |
   | `UserLocal_TryDoingShootAttack` | `0x1048F80` | **51** |
   | `UserLocal_TryDoingMagicAttack` | `0x1082020` | **52** |
3. 发包：官方门 `Network_SendOutPacket@0x1CB7CE0` → 内部 `HashSet` 后 `Session.SendPacket@0x1CB98B0`。P1 探针**走 SendOutPacket**（2026-08-03：直调 Session.Send → 第 3 次后 ~109ms 踢）。  
4. `flags` 低 4 位 | **`mobCount<<4`**：命中 `0x11` = count=1 + 低位 1；落空 `0x01` = count=0。  
5. 命中环按 `List<AttackInfo>` 迭代；**wire 上 damages 前是 `u16`（非 Encode1 AttackCount）**——见 §9。

---

## 1. 锚点表（2026-08-03）

| 符号 | RVA | VA |
|---|---:|---|
| `UserLocal_TryDoingNormalAttack` | `0x10C59D0` | `0x7FFB75AE59D0` |
| `UserLocal_TryDoingMeleeAttack` | `0x10B0EE0` | `0x7FFB75AD0EE0` |
| `UserLocal_TryDoingShootAttack` | `0x1048F80` | `0x7FFB75A68F80` |
| `UserLocal_TryDoingMagicAttack` | `0x1082020` | `0x7FFB75AA2020` |
| `OutPacket_Create` | `0x1CB7BB0` | （P0b） |
| `OutPacket_EncodeVector2` | `0x1CC5090` | 线上各 2B＝i16；**Y 会 IEEE 翻号**（本 dump RVA；现网见 [`P2_物落脚下.md`](P2_物落脚下.md)） |
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
| 9 | `Encode4_int` | 时间戳/密钥（`tOrKey`）← **实机 = GetUpdateTime 钟** |

头长至此后：**19 字节**。

`TryDoingNormalAttack` 落空路径同构，但：  
- skill 两枚 `Encode4` 解出 **0**；  
- 无命中环；结尾两次 `Encode2_short` 写玩家 XY。  
BODY≈23 → `DataPos off≈29`。

---

## 3. 命中环（每只怪 · `AttackInfo`）— IDA 视角

| # | API | 源 | CMS 对照 |
|---:|---|---|---|
| 1 | `Encode4_int` | `Mob*+0x134` | Mob oid |
| 2 | `Encode1_byte` | `AttackInfo+0x20` | `HitAction` |
| 3 | `Encode1_byte` | `+0x24` 与 bool 打包 | `ForeAction` + 谓词位 |
| 4 | `Encode1_byte` | `AttackInfo+0x28` | `FrameIdx` |
| 5 | `Encode1_byte` | Mob 朝向/状态打包 | （wire 独有） |
| 6 | `EncodeVector2` | Mob 位置（Unity） | wire＝i16(x), i16(**-y**)；见 [`P2`](P2_物落脚下.md) |
| 7 | `EncodeVector2` | `Mob+0x6C` | 第二点，同样翻 Y |
| 8 | `Encode1_byte` | `AttackInfo+0x30` | `AttackCount`（**IDA**；wire 见 §9） |
| 9 | `Encode4_int` ×N | `Damages[i]` | `Damages[]` |
| 10 | `Encode4_uint` | `UserLocal_GetFieldID` | 场 ID（真包常 **0**） |

环后：玩家 XY，再 `Network_SendOutPacket`。多怪环已见静态循环。

---

## 4. Magic / Shoot 差异（摘要）

| | Magic `0x1082020` | Shoot `0x1048F80` |
|---|---|---|
| Create | **52** | **51** |
| 相对 Melee | 头里多一枚 `Encode4` | 头里多 `Encode4` + 两枚 `Encode2` |
| Send | 同 `Network_SendOutPacket` | 同 |

---

## 5. 常量解混淆样例（Normal 落空头）

种子均在 `0x7FFB7B264xxx`（运行时填充；**必须实读**）：

| 写法 | 本轮实算 |
|---|---|
| `movzx ecx, word_…264154; add ecx, 0FFFF91A7h` | → **50** |
| `byte_…26413C ^ 0xA6` | → **1** |
| skill 相关两枚 | → **0** |
| `byte_…264130 + 0x42` | → **0** |

---

## 6. 对 P1 伪造的含义

| 做法 | 评估 |
|---|---|
| 自建 `Create(50)` + §9 wire Encode + `Session.SendPacket` | P1 现用；与 shop 同 Send |
| 多怪 | 重复命中环，`flags` 高 nibble = count |

---

## 7. 产物

| 路径 | 说明 |
|---|---|
| 本文 | Encode 序 + 实机修正 |
| `Dumps/runtime/send.log*` | op=50 真包 hex |

---

## 8. NOT RUN

- 单怪掉血 / 无即踢（需实验 TAB 勾选实机）  
- Shoot/Magic 头字段逐项命名  
- `ForeAction` 打包位精确掩码（wire 多数 `0x80`/`0x81`）

---

## 9. 实机修正（send.log op=50 · 2026-08-03）

> **冲突仲裁**：IDA §3 写 `Encode1(AttackCount)` + `Encode4×N`；**现网 kick_sniff BODY 为 `u16` + `dmg i32` + `field u32`**。P1 伪造跟 hex。

### 9.1 单怪命中（off=55）样例

```text
01 11 | skill0×2 | bool0 | action u16 | 01 05 | tOrKey u32
| oid | 06 | 80/81 | frame | 01 | xy1 | xy2 | A5 01/47 01 | dmg | field0 | player xy
```

| 字段 | 真包观察 |
|---|---|
| portal | 恒 `01`（不是 `03`） |
| flags | 命中 `11`；落空 `01` |
| action | `05`..`10`；左向可带 `0x8000` |
| +0x158 / weap | `01` / `05` |
| tOrKey | 量级 **GetUpdateTime**（如 `0x6CBA`），非 `GetTickCount` |
| ForeAction | 多数 `80`/`81`，**不是 0** |
| Frame | 多数 `00` |
| 两 XY 后 | **`u16`**：`A5 01` / `47 01` |
| fieldId | 恒 `0`（勿塞 mapId） |
| 落空 | off≈29，无命中环 |

### 9.2 对旧注释的作废

| 旧说法 | 处置 |
|---|---|
| portal=`0x03` | ❌ 真包 `0x01` |
| ForeAction 多数 0 | ❌ 真包 `0x80`/`0x81` |
| `Encode1 AttackCount` 上线 | ⚠️ IDA 有；**wire 用 u16**，P1 跟 wire |
| FieldID=`GetMapId()` | ❌ 真包 0 |
| tOrKey=`GetTickCount` | ❌ 改 `GetUpdateTime` |
| `Encode2S` 直写 AbsPos Y ⇔ `EncodeVector2` | ❌ 官方内部 `-Y`；漏翻则掉落在头顶。见 [`P2`](P2_物落脚下.md) |
