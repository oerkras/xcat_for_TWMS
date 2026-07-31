# 客户端 Hack 标志 → 服端逻辑推断（Classic TWMS）

> **状态**：dump 符号级推断（方法体为空；无 IDA 正文）  
> **证据源**：`Dumps/cms_cw/dump.cs`（语义名）；TW `runtime/out` 哈希类需 Rosetta 对齐  
> **日期**：2026-07-30  
> **目的**：澄清 `m_bFloatHackCheckNeed` / `m_bABHackCheckNeed` / `ClientHacksType` **能**反推什么、**不能**当成玩家飞天校验器

---

## 0. 一句话

这三样推的是 **怪物仿真完整性 + 客户端宏/速度举报** 的服端处置链；  
**推不出** 玩家 C→S `UserMove` 的 `HackingAutoBlock.Move/Position` 物理校验公式。  
F6 飞天踢线请对齐 MovePath 基线 + [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md)，不要去清角色上不存在的 Float/AB 标志。

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
| `SecurityClient.SetDetectMobSpeedHack` / `SendMobSpeedHackDetectCheck` | 怪加速检测与发送 |
| LiveValue `MobHackLogDisconnectCount` (408) | 怪 hack log → 断线次数阈值 |
| LiveValue `FindMobInRectHackLogDisconnectCount` (411) | 矩形找怪类 hack |
| LiveValue `MobNotMoveHackCheckValue` (525) | 怪「该动不动」类检查 |

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

### 3.3 `SecurityClient` 旁证（实锤）

`Msc.Security.SecurityClient`：

- `CollectAttackPacket` / `CollectAttackSkill`
- 窗长 `TERM_MS = 60000`，`CHECK_COUNT = 2000`
- `SendAttackPacketCountCheck` → 对应 type **20**
- `SendMobSpeedHackDetectCheck` → type **21**

### 3.4 反推服端逻辑（举报链）

```text
[客户端] 宏 / 超攻速 / 怪加速 本地检出
    → 发 ClientHacks(type) 或 ClientHacks_AllServer
[服务端] 按 type 分类
    → HackLog / 阈值断线 / 可能映射到某档 HackingAutoBlock
    → （细节：弱推断；dump 无处置表）
```

这是 **「处理客户端自证」**，不是在包内重放 `Δpos ≈ v·dt`。

---

## 4. 与 `HackingAutoBlock` / 飞天的分界（勿混）

| 机制 | 对象 | 方向 | 和 F6 飞 |
|---|---|---|---|
| Float / AB / Mob `HackCode` | **怪** | 客户端自检 → 上报 | 无关 |
| `ClientHacksType` | 宏 / 攻速 / 怪速 | C→S 举报 | 基本无关（除非同开宏） |
| `HackingAutoBlock.Move=22` / `Position=24` | 玩法终裁**结果码名** | 多为服端裁定后体现在踢/通知 | 飞天相关，**无公式** |
| C→S `UserMove`（CMS=47） | **角色**路径 | 客户端 Flush → 服端验 | **真·位移入口** |

玩家飞天诊断路径：

1. [`../fly/模块设计.md`](../fly/模块设计.md) — 积分器 / MoveElem 自洽  
2. [`../protocol/移动协议.md`](../protocol/移动协议.md) — Flush / opcode  
3. [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md) — 断线边沿 / `pendingError`  
4. **不要**改地图重力、不要在 `VecCtrlUser` 上找 Float/AB

---

## 5. 能推到哪 / 推不到哪

| 级别 | 内容 |
|---|---|
| 实锤 | 标志在 `VecCtrlMob`；`ClientHacks` 发包与枚举；SecurityClient 攻包/怪速 API |
| 强推断 | AB=碰撞前后；Float=怪浮空/飞目标；服端收举报后阈值断线 |
| 弱推断 | 具体阈值、是否映射到 Move=22、TW opcode 数值是否与 CMS 273/30 相同 |
| **推不出** | 玩家 UserMove 物理校验器源码与阈值；靠清 Float 过飞天踢 |

---

## 6. 相关入口

| 路径 | 说明 |
|---|---|
| `Dumps/cms_cw/dump.cs` | `VecCtrlMob` ~66021；`ClientHacksType` ~1084573；`SecurityClient` ~1130883 |
| `DumpRestoredData/dump.cs.restored` | 同字段 Rosetta 名（方法仍空） |
| [`MscSecurity能力面.md`](MscSecurity能力面.md) | RawInput / 反宏产品面 |
| [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) | 内核 AC 边界；与本文「逻辑举报」分层 |
