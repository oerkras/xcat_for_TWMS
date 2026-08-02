# attack_rpc P0b · 出站 Encode/Send 锚点（2026-08-03 新客户端）

> **状态**：✅ P0b 结案（发包管线已钉；攻击体布局 / TryDoing* 仍待）  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB74A20000`**  
> **上级**：[`模块设计.md`](模块设计.md) · 前序 [`P0a_本地出刀与OnMelee误区.md`](P0a_本地出刀与OnMelee误区.md)

---

## 0. 结论先行

全屏多怪结算层（B′ / A）的**可调用发包管线**已钉住：

```text
OutPacket.Create(ClientPacket 50..53)
  → Encode1/2/4/Str …（填攻击体）
  → NetworkManager.SendPacket(pkt)
       ├─ Prepare/EncodeForSend（序/CRC 类头）
       └─ Wire → Socket.BeginSend
```

这条路径**不经过** `TryDoing*` / `SetAttackAction` / 绳子谓词——与同行「跳本地动作门」模型一致。  
`UserRemote.OnMeleeAttack` 仍是收包侧（P0a），禁止当本地开火入口。

SecurityClient 攻包窗仍挂在出站旁路：

```text
Outbound_AttackPacketTap(this, ushort packetType)   RVA 0x48AB60
  ├─ CollectAttackPacket(packetType)
  └─ IsAttackPacket(packetType) → 白名单 50–53 / 191
```

> 静态上看 `NetworkManager.Send` **没有**直接 `call` 到 Tap（il2cpp MethodInfo 间接调用）；Tap 与 Collect 的**唯一 code xref 对**仍成立，语义与旧文 `…FCAB60` 一致。

---

## 1. 类哈希纠正

| 角色 | 新客户端 hash | 备注 |
|---|---|---|
| **UserLocal（真）** | `ac2e48ccb42621aeda0a94cfdaa2212c4be9ce58a3efa9661804a372cef3e1d` | `DoActiveSkill(int,uint)→bool` + `CollectAttackSkill` 唯一 xref |
| UserLocal（CRITICAL 表） | `a6c2b4310ec35fad…` | **误标**——勿再当 UserLocal |
| **OutPacket** | `aeb7167893ac51cbc0cf730326f2361e6e8b797eeb940786711185ef0fd658c` | Create / ctor(ushort) / Encode* / get_PacketId |
| **NetworkManager**（SessionTcpLayer） | `f0ee06b64ad95c59b95ca923b6db62ce451a5c512b3ef47e7c3814caca41909` | 旧 `d418de852f…` 已不在 script |
| SecurityClient | `d9ef28f16e8be00eacfa1d3189c0aaa048cfa998bc6efde64fb0c983ce4cb7f` | CRITICAL 已对 |
| 出站攻包旁路 nested | `fa1d8d5b… . bbb6346d…` | `void(this, ushort)` = Tap |

---

## 2. 锚点表（RVA · VA = imagebase + RVA）

### 2.1 SecurityClient

| 符号 | RVA | VA |
|---|---|---|
| `CollectAttackPacket` | `0x3C44C10` | `0x7FFB78664C10` |
| `IsAttackPacket` | `0x3C450E0` | `0x7FFB786650E0` |
| `CollectAttackSkill` | `0x3C45270` | `0x7FFB78665270` |
| `SendAttackPacketCountCheck` | `0x3C46C10` | `0x7FFB78666C10` |
| `Outbound_AttackPacketTap` | `0x48AB60` | `0x7FFB74EAAB60` |

白名单（CMS / 旧 BIN）：`UserMelee/Shoot/Magic/BodyAttack` = **50–53**，`SummonedSkill` = **191**。

### 2.2 UserLocal

| 符号 | RVA | VA | 证据 |
|---|---|---|---|
| `DoActiveSkill` | `0x1064E60` | `0x7FFB75A84E60` | 唯一 `CollectAttackSkill` code xref；sig `bool(this,int,uint)` |

`OutPacket.Create` 在 UserLocal 方法上有 **≥40** 处 code xref（含疑似 TryDoing/DoActiveSkill* 发包点）；具体哪几个是 50–53 攻包仍需按 opcode 动态/常量解混淆钉名。

### 2.3 OutPacket

| 符号 | RVA | VA |
|---|---|---|
| `Create` | `0x1CB7BB0` | `0x7FFB766D7BB0` |
| `get_PacketId` | `0x1CC2EA0` | `0x7FFB766E2EA0` |
| `get_TotalSize` | `0x1CC2EB0` | `0x7FFB766E2EB0` |
| `.ctor(ushort)` | `0x1CC2EC0` | `0x7FFB766E2EC0` |
| `EncodeForSend`（候选） | `0x1CC2F60` | `0x7FFB766E2F60` |
| `Encode1(sbyte)` | `0x1CC4040` | `0x7FFB766E4040` |
| `Encode1(byte)` | `0x1CC4110` | `0x7FFB766E4110` |
| `Encode1(bool)` | `0x1CC41F0` | `0x7FFB766E41F0` |
| `Encode2(short)` | `0x1CC4370` | `0x7FFB766E4370` |
| `Encode2(ushort)` | `0x1CC43F0` | `0x7FFB766E43F0` |
| `Encode4(int)` | `0x1CC4480` | `0x7FFB766E4480` |
| `Encode4(uint)` | `0x1CC4500` | `0x7FFB766E4500` |
| `EncodeStr` | `0x1CC48C0` | `0x7FFB766E48C0` |

字段（CMS / travel_port 惯例，待运行时复核）：`PacketId@+0x20`，buffer/cursor 在基类 `Packet`。

旧 travel/shop 常量 `kRvaOutPacketCreate=0x1CB93E0` 等 **已失效**（`RVA_REMAP` AMBIG）；以上为新表。

### 2.4 NetworkManager

| 符号 | RVA | VA |
|---|---|---|
| `SendPacket(OutPacket)→bool` | `0x1CB98B0` | `0x7FFB766D98B0` |
| Prepare / 组发送缓冲 | `0x1CC7A50` | `0x7FFB766E7A50` |
| Wire（`Socket.BeginSend`） | `0x1CC8050` | `0x7FFB766E8050` |

旧 `kRvaNmSend=0x1CB9510` / 类哈希 `d418de852f…` **已失效**。

`SendPacket` 空包早退：混淆常量解出 **0**（`0xD3115D6F + seed@7B2ABEF4 → 0`），与 `test rsi,rsi` 分支一致。

---

## 3. P1 接口草图（默认关 · 实验）

```text
attack_rpc_port（默认 off）
  输入：mob 列表（objId / 坐标 / 命中序）+ skillId/slv + 攻包类型(50/51/52/53)
  步骤：
    1. OutPacket.Create(type)
    2. 按 BIN 样本 Encode 攻击体（P0c 钉字段表后才能写死）
    3. NetworkManager.SendPacket(pkt)   // 禁止旁路 Session.Send HashSet（踢线先例）
  门控：面板间隔 / maxN / type20 计数观测（Collect 窗仍会涨）
  非目标：伪造任意伤害数值；默认替换 attack_input
```

验收：

1. 平地 BIN：自建包 opcode ∈ {50..53}，服端扣血 / 无即踢。  
2. 绳态：同一 API 仍能发包（证明跳过本地门）。  
3. 多怪：单包多 `AttackInfo` 或连发，whiff/type20 可观测。

---

## 4. 仓内可复用研究（勿重挖）

| 文档 | 拿什么 | 对本模块 |
|---|---|---|
| [`../security/攻包计数窗与type20.md`](../security/攻包计数窗与type20.md) | 白名单 50–53/191；Collect 仅出站旁路；type20=60s/2000；LiveValue 557/558 | P1 频率门控 / 观测 |
| [`../attack_speed/P0a_出刀忙锁与Prepare链.md`](../attack_speed/P0a_出刀忙锁与Prepare链.md) | `OnFuncKey → TryDoing* → SetAttackAction → OutPacket`；`+0x118` busy | 正路对照；B′ 必须跳过这段 |
| [`../buffs/P0a_锚点复核.md`](../buffs/P0a_锚点复核.md) | 旧 `DoActiveSkill`/`Prepare`/`SendSkillUseRequest` RVA（**旧 imagebase**） | 技能包≠攻包；模式可抄、RVA 要重锚 |
| [`../sellbag/P0a_商店卖出锚点.md`](../sellbag/P0a_商店卖出锚点.md) | `Create → Encode* → NM.Send` 实锤；**禁 Session.Send**（105ms 踢） | P1 发包范式 |
| [`../travel/模块设计.md`](../travel/模块设计.md) / travel_port | 同套 OutPacket/NM 旧 RVA | 迁表见本文 §2.3–2.4 |
| [`../simple_combat/模块设计.md`](../simple_combat/模块设计.md) | 产品默认 **不做伪造攻包**；出刀走 `OnFuncKey` | attack_rpc 必须默认关、另 port |
| [`../multi_skill/模块设计.md`](../multi_skill/模块设计.md) | 多发 → `DoActiveSkill` → `CollectAttackSkill` | 与攻包窗叠加风险 |

> **缺口（docs 里没有）**：`AttackInfo` TW 字段表、Melee/Magic **wire 包序/hex**。P0c 只能靠 BIN 新采，不能从现有 md 抄完。

旧 RVA 换算提示：`attack_speed`/`buffs` 写的是 imagebase `0x7FFB16B40000`；本轮是 `0x7FFB74A20000`。例如旧 `DoActiveSkill@0x1066540` → 新 **`0x1064E60`**；旧 `SetAttackAction@0xFD50A0` → 新 **`0xFD39C0`**（P0a）。

---

## 5. 仍未决（P0c / P1 前）

1. **攻击体布局**：`AttackInfo` 字段 + 各职业 Melee/Magic 包序（BIN；勿抄 CMS 当 TW）。  
2. **UserLocal 内 50–53 的官方 Encode**：从 41 处 `Create` xref + `attack_speed` 链上的 `TryDoingNormalAttack` 旧 RVA `~0x10C70B0` 重匹配。  
3. **`Outbound_AttackPacketTap` 的 MethodInfo 调用点**（静态无 E8）。  
4. 更新 `CRITICAL_HASH_MAP`：UserLocal → `ac2e48cc…`；补 OutPacket / NetworkManager。  
5. travel/shop port 旧 OutPacket/NM RVA 迁移（范围外，另开任务）。

---

## 6. 产物

| 路径 | 说明 |
|---|---|
| 本文 | P0b 结论文档 |
| `Dumps/runtime/P0B_ATTACK_RPC_SEND_20260803.tsv` | 锚点机读表 |
| IDA 命名 | `OutPacket_*` / `NetworkManager_Send*` / `UserLocal_DoActiveSkill` / `Outbound_AttackPacketTap` / `SecurityClient_CollectAttackSkill` |
