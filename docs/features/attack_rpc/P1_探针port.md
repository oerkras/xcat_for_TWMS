# attack_rpc P1 · 探针 port（Create+Encode+Send）

> **状态**：✅ 代码已落地 · 默认关 · 出站路径已 BIN（`normal ok`）· 掉血待勾选验收  
> **产品**：经典版 TWMS · **不是**枫星  
> **上级**：[`模块设计.md`](模块设计.md) · [`P0c_攻包BODY布局.md`](P0c_攻包BODY布局.md)

---

## 0. 做什么

不经 `OnFuncKey` / `TryDoing*` / `SetAttackAction`，在主线程：

```text
SetAttackAction(lu, action, 0, null, 0)   ← 2026-08-03 补（对齐 TryDoing）
CollectAttackPacket(50)                  ← 2026-08-03 补（Tap 旁路补窗）
OutPacket.Create(50)
  → Encode 头 + 命中环（P0c 实机修正表）
  → Network_SendOutPacket@0x1CB7CE0（this=facade）
       → HashSet.Contains(opcode) → Session.SendPacket@0x1CB98B0
```

2026-08-03 BIN：直调 `Session.SendPacket` 旁路 HashSet → 第 3 次 forge 后 ~109ms 踢（已修）。  
SendOut 后可连打 5 刀；再开第 6 刀 ~0.9s 延后踢 → 缺本地动作态假设，现补 `SetAttackAction`+`Collect`。  
单次勾选满 **2** 次 ok 会 `auto_stop`；间隔地板 800ms。

---

## 1. 开关

| 方式 | 效果 |
|---|---|
| （默认） | **关**；worker 空转 |
| **实验 TAB「攻包伪造探针」** | 写入 `user.ini [core] attackRpc*`，payload `ApplyControl` 调 `SetEnabled` |
| 环境变量 `ATTACK_RPC=1` | Init 时打开（仍可用） |
| `ATTACK_RPC_MOBS` / `_MS` / `_DMG` | Init 时可选覆盖（仍可用） |
| `ports::attack_rpc::SetEnabled(true)` | 运行时打开 |

面板字段：`attackRpc`、`attackRpcMobs`(1..15)、`attackRpcIntervalMs`(50..5000)、`attackRpcDamage`；core ini v38。

打开后 worker 约每 20s 顺带 `security_attack::ProbeWindow`（type20 观测）。

---

## 2. 文件

| 路径 | 角色 |
|---|---|
| `x/features/ports/attack_rpc_port.{h,cpp}` | 组包 / 发送 |
| `x/features/attack_rpc/attack_rpc.{h,cpp}` | feature worker |
| `common/xcat_payload_control.{h,cpp}` | core 字段 + Read/Write |
| `x/ipc/payload_control.cpp` | ApplyControl → SetEnabled/参数 |
| `xcat_app/workspace_tabs.cpp` | 实验 TAB 勾选 + 参数 |
| `x/probe/CMakeLists.txt` / `xcat_probe.cpp` | 编入 + Init |

---

## 3. Encode / Send 现状（2026-08-03 BIN 对齐后）

| 项 | 取值 |
|---|---|
| opcode | `Create(50)` |
| portal | `0x01` |
| flags | `(1) \| (mobCount<<4)` → 单怪 `0x11` |
| skill×2 | `0` |
| bool | `false` |
| action | `5`（左向 `\| 0x8000`） |
| +0x158 | 读 `UserLocal+0x158`，失败回退 `1` |
| 武器字节 | `5` |
| **tOrKey** | **`WorldManager.GetUpdateTime`**（禁止 `GetTickCount` 当钟） |
| HitAction / ForeAction / Frame | `06` / `0x80` / `0` |
| 两 XY | `Encode2` i16（等价线上 EncodeVector2） |
| dmg 前 | `u16 0x01A5`（真包；非 IDA 的 Encode1 AttackCount） |
| fieldId | `0` |
| Session | facade 单例 / FindAll；`+0x10` Session；klass 漂移不硬拒 |
| Send | `Session.SendPacket@0x1CB98B0`（MethodInfo 优先） |
| hex 日志 | `buf+0x20+6` BODY（与 kick_sniff 同） |

多怪：`attackRpcMobs=N` → flags 高 nibble + 命中环循环；优先 `ctrl>0`。

---

## 4. 人工验证清单

1. 不勾面板：日志 `feature init (OFF)`，无 forge/normal ok。  
2. 实验 TAB 勾「攻包伪造探针」，**可暂时关自动打怪**，进图站怪旁（间隔≥200ms、mobs=1）：  
   - `AttackRpc forge BODY off=55 … tOrKey=`（量级应像真包 ~1e4–1e5，不是 Windows 开机毫秒）  
   - `AttackRpc normal ok mobs=1 body~55`  
   - **无**周期性 `no_nm` / `bind_fail`  
3. 观感或 `combat`/mob hp：**单怪掉血**；`kick.log` 无立刻断线。  
4. 单怪通过后：`attackRpcMobs` 提到 2+，日志 `mobs=N`；顺带看 security 窗 type20。  
5. 关开关后不再发包。

---

## 5. 已知坑（已修 / 仍待）

| 坑 | 状态 |
|---|---|
| 主线程嵌套 `TypeGetObject` 卡顿 | ✅ 已改直调 + 失败退避 |
| `no_nm`（Session klass 过严 / FindAll 不匹配） | ✅ 对齐 shop：facade 缓存 + `+0x10` 兜底 |
| forge hex 误 dump 数组对象头 | ✅ 改为 `+0x20+DataPos` |
| `tOrKey=GetTickCount` 量级错 | ✅ 改 `GetUpdateTime` |
| ForeAction=0 | ✅ 改 `0x80` |
| 单怪掉血 | 🔍 待用户勾选验收（代码已对齐） |
