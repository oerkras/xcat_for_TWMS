# 客户端 Hack 标志 → 服端逻辑推断（Classic TWMS）

> **状态**：dump 符号 + TW IDA xref/常量解码 + BIN 探针（攻包窗详文 2026-08-01；**pendingError/AutoBlock 边界 2026-08-06 修订**）  
> **证据源**：`Dumps/cms_cw/dump.cs`（语义名）；TW `runtime/out` / `GameAssembly.dll.i64`；`x.jsonl` tag=`SecAttack`  
> **日期**：2026-07-30（§3.3 攻包窗于 2026-08-01 扩链至专文；§4 AutoBlock↔pendingError 于 2026-08-06 证伪）  
> **目的**：澄清 `m_bFloatHackCheckNeed` / `m_bABHackCheckNeed` / `ClientHacksType` **能**反推什么、**不能**当成玩家飞天校验器

---

## 0. 一句话

这三样推的是 **怪物仿真完整性 + 客户端宏/速度举报** 的服端处置链；  
**推不出** 玩家 C→S `UserMove` 的物理校验公式。  
`HackingAutoBlock.Move/Position` 只是 CMS **结果码名**：TW `Session._pendingErrorCode@0x40` **从不写出 22/24**（只见哨兵 204/205）——见 [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md) §3。  
F6 飞天踢线请对齐 MovePath 基线 + kick_sniff 的 **STATE / RING**，不要去清角色上不存在的 Float/AB 标志，也不要等 `pendingError=22/24`。

---

## 1. 证据边界（硬）

| 有 | 没有 |
|---|---|
| 类型归属、字段邻域、枚举名、C→S opcode 名、LiveValue 开关名 | 判定公式 / 阈值常量实值 / 服端源码 |
| Il2Cpp 方法签名（`SendMobPrevPosHack` 等） | dump 方法体（全是 `{ }`） |

置信标注：`实锤`（符号+归属） / `强推断`（字段簇+命名） / `弱推断`（服端处置细节）。

---

## 2. `m_bFloatHackCheckNeed` / `m_bABHackCheckNeed`（实锤：在怪上）

### 2.1 归属

| 项 | 值 |
|---|---|
| 类 | `Msc.Game.Object.Control.VecCtrlMob`（CMS TypeDef **1504**） |
| 偏移 | `m_bABHackCheckNeed` @ **0x1D4**；`m_bFloatHackCheckNeed` @ **0x1D5** |
| 相邻 | `m_nHackedCode`（`HackCode`）@ **0x1D8** |
| **不在** | `VecCtrlUser`（玩家只有自由落体 tick / 梯子 / `MakeContinuousMovePath` 等） |

> 误读风险：名称含 Float/Hack，易被当成「玩家浮空检测」。**字段在 Mob 控制器上。**

### 2.2 同簇字段（强推断语义）

```text
碰撞前后：
  m_ptBeforeCollision / m_apBeforeCollision / m_ptAfterCollision
  m_bSimpleCollisionHacked

飞目标抽检：
  m_bNeedForCheckFlyTarget_A/B … MoveRandMan* … CheckTarget*XY
  VecCtrlMob.MoveCtx.FlyContext

自检门闩：
  InspectUpdateActive() 重写
  Guard* 坐标 / m_bGuardArea* / m_bPassedInspectUpdate

结果：
  m_bABHackCheckNeed / m_bFloatHackCheckNeed / m_nHackedCode
```

| 标志 | 强推断含义 |
|---|---|
| `m_bABHackCheckNeed` | **A/B = After/Before**：碰撞（或 guard）前后坐标不一致，需上报 |
| `m_bFloatHackCheckNeed` | 怪「浮空 / 飞目标」与本地仿真不一致，需上报 |
| `m_nHackedCode` | 本地匿名分型（`HackCode _1…_37`，无业务名） |

### 2.3 配套发包 / 开关（实锤符号）

| 符号 | 角色 |
|---|---|
| `SendMobPrevPosHack()` | 怪前位异常上报 |
| `SecurityClient.SetDetectMobSpeedHack` / `SendMobSpeedHackDetectCheck` | 怪加速检测与发送；**已解码**见 [`怪速举报type21与被动插值.md`](怪速举报type21与被动插值.md)：仅 `CalcPassivePos` 触发、阈值全=0、节流 300s |
| LiveValue `MobHackLogDisconnectCount` (408) | 怪 hack log → 断线次数阈值（表 id；累加器未钉） |
| LiveValue `FindMobInRectHackLogDisconnectCount` (411) | 矩形找怪类 hack |
| LiveValue `MobNotMoveHackCheckValue` (525) | 怪「该动不动」类检查 |
| LiveValue `MobPullingHack` / `Threshold` / `Kick` (926–928) | **独立拉怪 Kick 通道**（不经 408 枢纽） |
| LiveValue `MobDensityByCentroid*` (932–934) · `MobDensityBySector*` (938–941) | 质心/扇区挤堆 |
| LiveValue `MobDeadPositionInspect*` (935–937) | 死亡点离出生点过远 |

> 408 不是 928/934/936 的别名。吸怪 205 的平行族与 thunk 表见 [`吸怪平行检测族-拉怪密度死点.md`](吸怪平行检测族-拉怪密度死点.md)（2026-08-22）。

### 2.4 反推服端逻辑（怪物链）

```text
[客户端] VecCtrlMob 每帧仿真怪移动/碰撞
    → InspectUpdateActive / Guard / FlyTarget 自检
    → 失败：置 Float 或 AB + HackCode
    → C→S：PrevPosHack / MobSpeedHack / HackLog 类包
[服务端] 收举报或对照自身怪权威
    → 累计（LiveValue 阈值）
    → 记 HackLog / 断线
```

**与 F6 飞天：无关**（不读写玩家 `VecCtrlUser` 这两 bool）。

---

## 3. `ClientHacksType`（实锤：客户端→服端举报枚举）

### 3.1 位置与发包

| 项 | 值 |
|---|---|
| 枚举 | `Framework.Network.ClientHacksType`（CMS TypeDef **14705**） |
| C→S | `ClientPacket.ClientHacks = 273` |
| C→S（全服？） | `ClientPacket.ClientHacks_AllServer = 30` |

### 3.2 分型（从命名 · 实锤枚举值）

| 值 | 名 | 推断域 |
|---|---|---|
| 0 | None | — |
| 1–3 | FewKeyCount / UpKeyRepeat / MainInputKeyChanged | 按键统计异常 |
| 4–9 | RawInputKeyMacroDetected* | RawInput 宏（含 compress / UpKey 变体） |
| 10 | SoftKeyMacroDetected | 软键盘宏 |
| 11–16 | WorldInputKeyMacroDetected* | 世界输入宏 |
| 17 | AntimacroKeyBoardCheck | 反宏键盘 |
| 18 | InputKeyHandleCheck | 输入句柄 |
| 19 | NGSCaptureClient | NGS 捕获相关 |
| 20 | AttackPacketCountCheck | 攻包频率（见下） |
| 21 | MobSpeedHack | 怪加速（回到 §2） |

### 3.3 `SecurityClient` 攻包窗 / type20（摘要 · 详文另册）

> **完整挖空 + BIN + `IsAttackPacket` 白名单**：见 [`攻包计数窗与type20.md`](攻包计数窗与type20.md)（2026-08-01）。

`Msc.Security.SecurityClient`：

- `CollectAttackPacket` / `CollectAttackSkill` / `IsAttackPacket`
- 窗长 `TERM_MS = 60000`，`CHECK_COUNT = 2000`
- `SendAttackPacketCountCheck` → **type 20**；`SendMobSpeedHackDetectCheck` → **type 21**
- 攻包白名单（CMS）：`UserMelee/Shoot/Magic/BodyAttack` = **50–53**，`SummonedSkill` = **191**
- 每帧：`a480_Update` → 安全 tick → 同时跑 type20/21 检查

**TW IDA（2026-08-03 重锚 · imagebase `0x7FFB74A20000`）**：

| 符号 | VA / RVA |
|---|---|
| `SecurityClient_CollectAttackPacket` | `0x7FFB78664C10` / `0x3C44C10` |
| `SecurityClient_IsAttackPacket` | `0x7FFB786650E0` / `0x3C450E0` |
| `SecurityClient_CollectAttackSkill` | `0x7FFB78665270` / `0x3C45270` |
| `SecurityClient` 类哈希 | `d9ef28f1…ce4cb7f`（旧 `ba499947…` 作废） |
| `LiveValueManager.GetInt_Def` | 见 LiveValue 挖空（随 GA 重基） |
| `LiveValue.GetInt`（static） | 同上 |

详文：[`攻包计数窗与type20.md`](攻包计数窗与type20.md)。

- `TERM_MS`/`CHECK_COUNT` 元数据仍为 **60000 / 2000**；函数体 CFF，靠反汇编解码。
- BIN：`security_attack_port` tag=`SecAttack` 已采到 `peakKey≥1`；布局须 `Dictionary.freeCount@+0x28`（勿读 `+0x2C`=`version`）。
- `GetInt_Def` xref **未见** 键 **430 / 557 / 558**；进图探针常全 **-999999**（与 type20 硬窗脱钩）。

| LiveValueInt | 键 | 语义 |
|---|---|---|
| `UserSkillUseRequestCountCheck` | **430** | 技能发包计数检查（阈值=服端表） |
| `PacketAttack1SecLimit` | **557** | 攻包 1s 上限（阈值=服端表） |
| `PacketAttack500msLimit` | **558** | 攻包 500ms 上限（阈值=服端表） |

### 3.4 反推服端逻辑（举报链）

```text
[客户端] 宏 / 超攻速 / 怪加速 本地检出
    → 发 ClientHacks(type) 或 ClientHacks_AllServer
[服务端] 按 type 分类
    → HackLog / 阈值断线 / 可能映射到某档 HackingAutoBlock
    → （细节：弱推断；dump 无处置表）
```

这是 **「处理客户端自证」**，不是在包内重放 `Δpos ≈ v·dt`。  
（角色侧：客户端 `MovePath_Flush` 同样**不**做 `Δpos≈v·dt`；该式是对服端 UserMove 校验的推断，见 [`../protocol/移动协议.md`](../protocol/移动协议.md) §4。）

---

## 4. 与 `HackingAutoBlock` / 飞天的分界（勿混）

| 机制 | 对象 | 方向 | 和 F6 飞 |
|---|---|---|---|
| Float / AB / Mob `HackCode` | **怪** | 客户端自检 → 上报 | 无关 |
| `ClientHacksType` | 宏 / 攻速 / 怪速 | C→S 举报 | 基本无关（除非同开宏） |
| `HackingAutoBlock.Move=22` / `Position=24` | CMS **结果码名** | 服端裁定用语；**TW 不写入** `Session+0x40` | 飞天相关语义，**无客户端公式**；踢线看 TCP/STATE |
| C→S `UserMove`（CMS=47） | **角色**路径 | 客户端 Flush → 服端验 | **真·位移入口** |

玩家飞天诊断路径：

1. [`../fly/模块设计.md`](../fly/模块设计.md) — 积分器 / MoveElem 自洽  
2. [`../protocol/移动协议.md`](../protocol/移动协议.md) — Flush / opcode  
3. [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md) — `SessionState` / RING / **哨兵 205**（勿当踢因）  
4. **不要**改地图重力、不要在 `VecCtrlUser` 上找 Float/AB、**不要**等 `pendingError=22/24`

---

## 5. 能推到哪 / 推不到哪

| 级别 | 内容 |
|---|---|
| 实锤 | 标志在 `VecCtrlMob`；`ClientHacks` 发包与枚举；SecurityClient 攻包/怪速 API；**TW `Session+0x40` 只写 204/205，非 AutoBlock** |
| 强推断 | AB=碰撞前后；Float=怪浮空/飞目标；服端收举报后阈值断线；位移踢 = 服端掐 TCP + 客户端本地拆线 |
| 弱推断 | 具体阈值；AutoBlock 名是否出现在 notice payload；TW opcode 数值是否与 CMS 273/30 相同 |
| **推不出** | 玩家 UserMove 物理校验器源码与阈值；靠清 Float 过飞天踢；靠 `pendingError=Move/Position` 认踢 |

---

## 6. 相关入口

| 路径 | 说明 |
|---|---|
| `Dumps/cms_cw/dump.cs` | `VecCtrlMob` ~66021；`ClientHacksType` ~1084573；`SecurityClient` ~1130883 |
| `DumpRestoredData/dump.cs.restored` | 同字段 Rosetta 名（方法仍空） |
| [`MscSecurity能力面.md`](MscSecurity能力面.md) | RawInput / 反宏产品面 |
| [`攻包计数窗与type20.md`](攻包计数窗与type20.md) | SecurityClient 攻包窗 / type20 专文（IDA+BIN） |
| [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) | 内核 AC 边界；与本文「逻辑举报」分层 |
