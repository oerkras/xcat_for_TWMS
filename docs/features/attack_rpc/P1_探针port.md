# attack_rpc P1 · 探针 port（Create+Encode+Send）

> **状态**：✅ 代码已落地 · 默认关 · 实机发包 NOT RUN  
> **产品**：经典版 TWMS · **不是**枫星  
> **上级**：[`模块设计.md`](模块设计.md) · [`P0c_攻包BODY布局.md`](P0c_攻包BODY布局.md)

---

## 0. 做什么

不经 `OnFuncKey` / `TryDoing*` / `SetAttackAction`，在主线程：

```text
OutPacket.Create(50)
  → Encode 头 + 命中环（P0c 序）
  → Network_SendOutPacket@0x1CB7CE0  （内部 → NM.SendPacket，走 HashSet）
```

目标：验证「跳本地门仍能出站 50」；为后续多怪 ramp / 接入战斗循环垫底。

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

## 3. 已知简化（探针可接受）

- `portal` / `+0x158` / `HitAction` / `ForeAction` 等用占位常量，非完整照抄官方栈变量。  
- `tOrKey` 用 `GetTickCount`（非游戏 tCur）。  
- 两处 `EncodeVector2` 暂用同一 mob 坐标。  
- FieldID 用 `world::GetMapId()`。  
- 服端可能拒包 / 不计伤；本阶段只看 **是否出站 + 是否踢线**。

---

## 4. 人工验证清单

1. 不勾面板、不设 `ATTACK_RPC`：日志 `feature init (OFF)`，无 melee 发包日志。  
2. 实验 TAB 勾「攻包伪造探针」（或 `ATTACK_RPC=1`），进图站怪旁：应有 `AttackRpc melee ok mobs=…`；`kick.log` 探针 armed `0x1CB98B0` 时 `send.log` 见 op=50。  
3. 对照 `security` 窗：`pktSum` 是否上涨；注意 type20（60s/2000）。  
4. 确认未被本地踢（卖店 Session.Send 类旁路会 ~105ms 踢——本路径应走 SendOutPacket）。

---

## 5. NOT RUN / 已知坑

- 实机进图发包 / 掉血 / 踢线  
- 占位字段与官方逐字节 diff  
- **卡顿根因（2026-08-03 BIN）**：开启后 `melee fail err=no_nm`；`ClassTypeObject` 曾在主线程 job 内嵌套 `managed_main::TypeGetObject` → 泵超时卡顿。已改为 shop 同款直调 + 失败退避（1→10s）+ FindAll≥3s 一次。  
