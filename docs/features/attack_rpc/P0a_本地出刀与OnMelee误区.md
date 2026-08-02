# attack_rpc P0a · 本地出刀链与 OnMelee 误区（2026-08-03 新客户端）

> **状态**：⚠️ P0 部分结案（架构纠正 + 1 个唯一命中）；TryDoing* / 绳子谓词 / 真发包点仍待钉  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB74A20000`**（已换代）  
> **类哈希**：见 `Dumps/runtime/CRITICAL_HASH_MAP_20260803.tsv`  
> **上级**：[`模块设计.md`](模块设计.md)

---

## 0. 结论先行

原先方案里的 **B = 直调 `OnMeleeAttack`**，在 CMS 归属上是 **错的**：

| CMS 符号 | 所在类 | 角色 |
|---|---|---|
| `OnMeleeAttack(...)` | **`UserRemote`** | **收包/表现**（别人打你看到的近战结算表现） |
| `TryDoingMeleeAttack` / `TryDoingNormalAttack` / … | **`UserLocal`** | **本地发起**出刀 |
| `SetAttackAction(int,int,SkillEntry,int)→bool` | **`User`** | 动作/忙锁入口（本地） |
| `IsOnRope` / `IsOnLadder` | **`VecCtrl`** | 绳子/梯子态 |
| `IsOnLadderOrRope` | **`User`** | 组合谓词 |
| `OnPacketUserMeleeAttack` | **`UserPool`** | 入站包分发 |

因此：

- 同行「挂绳也能打 / 像全屏」**更不像**调 `UserRemote.OnMeleeAttack`。  
- 更贴近：**本地组命中列表后直发 50–53 攻包（原 A）**，或调 **TryDoing* 下游的 Encode/Send 私有函数**（修正后的 B′）。  
- 我们现网 `OnFuncKey → TryDoing* → SetAttackAction → … → OutPacket` 仍是正路；B′ 要插在 **SetAttackAction 之后、出站之前**，才能跳绳子门。

---

## 1. 证据

### 1.1 CMS 类归属（`Dumps/cms_cw/dump.cs`）

脚本扫「方法定义前最近的 `class`」：

- `OnMeleeAttack` → **UserRemote**（约 L64922）  
- `TryDoingMeleeAttack` / `TryDoingNormalAttack` → **UserLocal**（约 L64417 / L64426）  
- `SetAttackAction(int,int,SkillEntry,int)` → **User**（约 L63217；另有 Dragon/Summoned 重载勿混）  
- `IsOnRope` → **VecCtrl**（约 L65690）  
- `OnPacketUserMeleeAttack` → **UserPool**

### 1.2 新客户端 script.json 软签名匹配

工具：`Dumps/runtime/_p0_attack_path_corrected.py`  
表：`Dumps/runtime/P0_ATTACK_RPC_20260803.tsv`

| 目标 | 结果 |
|---|---|
| `User.SetAttackAction` soft=`bool(int,int,obj,int)` | ✅ **unique** · RVA **`0xFD39C0`** · leaf `b136248a83c56e10…` |
| `UserLocal.TryDoing*` | ❌ miss（新客户端签名/参数编码对不上 CMS，或仍在哈希海） |
| `VecCtrl.IsOnRope/Ladder` | ⚠️ `bool()` 歧义 23 个，未钉 |
| `User.IsOnLadderOrRope` | ⚠️ `bool()` 歧义 19 个，未钉 |
| `UserRemote.OnMeleeAttack` | ❌ 新 `CRITICAL_HASH_MAP` **尚未锚 UserRemote** |

`SetAttackAction` 的 script 签名（实锤形）：

```text
bool User__(int, int, SkillEntry_o*, int, MethodInfo*)
TypeSignature=iiiiiii
Address(RVA)=0xFD39C0
```

### 1.3 IDA

- imagebase `0x7FFB74A20000` → VA `0x7FFB759939C0` 落在函数 **`0x7FFB75993750`**（size `0x28C`）内。  
- Hex-Rays 为 **CFF 跳表**（`jmp qword ptr [rax]`），静态伪代码不可读。  
- 已尝试命名函数入口为 `User_SetAttackAction`（以 IDA 当前名为准）。  
- **运行时 dump + 解 CFF / 动态跟** 才能看清是否写 `+0x118`、是否在绳态直接 return。

---

## 2. 修正后的「B′」定义

```text
原 B（作废）:  调 UserRemote.OnMeleeAttack(List<AttackInfo>)
                → 这是收包侧，不能当本地挂绳开火

修正 B′:       找到「已有 AttackInfo 列表 → Encode → OutPacket(50/52)」的本地私有函数
                或 TryDoing* 末段 Send，跳过 IsOnRope / SetAttackAction 失败路径

对照 A:        直接组 OutPacket 50–53（协议自研）
```

**挂绳无动作** ⇒ 必须证明调用点 **不经过** 成功的 `SetAttackAction` / 攻击 ActionLayer。  
若仍调完整 `TryDoingMeleeAttack`，通常仍会被绳子门挡住（除非同行 nop 了谓词）。

---

## 3. 未决（部分已由 P0b 接走）

1. **动态钉 `User_SetAttackAction@0xFD3750/0xFD39C0`**：进图平地出一刀，看是否进该 CFF、绳态是否早退。  
2. ~~攻包旁路反查 Encode/Send~~ → **见 [`P0b_出站Encode与Send锚点.md`](P0b_出站Encode与Send锚点.md)**。  
3. **锚 `UserRemote` 类哈希**，确认 `OnMeleeAttack` 仅被 `OnPacket*` 调用（坐实收包）。  
4. **重匹配 TryDoing***：从 UserLocal（真 hash `ac2e48cc…`）+ `OutPacket.Create` xref 反查。  
5. **AttackInfo 字段表**（P0c）：BIN 对照后再谈填列表。

---

## 4. 对产品方案的影响

| 原计划 | 调整 |
|---|---|
| P1 直调 `OnMeleeAttack` | **停止**；改为 P0b 找 Encode/Send 或验证 nop 绳门 |
| 用 `List<AttackInfo>` 喂 Remote | 仅作对照实验（预期：本地角色不发包 / 行为怪异） |
| 与 `attack_input_port` 关系 | 现网 Key 路径保留；B′ 另 port，默认关 |

---

## 5. 产物清单

| 路径 | 说明 |
|---|---|
| 本文 | P0a 结论文档 |
| `Dumps/runtime/P0_ATTACK_RPC_20260803.tsv` | 匹配表 |
| `Dumps/runtime/_p0_attack_path_corrected.py` | 可复跑匹配脚本 |
| `Dumps/runtime/CRITICAL_HASH_MAP_20260803.tsv` | 类哈希 |
| IDA `User_SetAttackAction` @ `0x7FFB75993750` | 待动态验证 |
