# mob_pool P0b · MI 观察刷怪 → 按需 Collect（设计 · 未实现）

> **状态**：📐 设计稿 · **本轮不改产品代码**  
> **产品**：经典版 TWMS · **不是**枫星  
> **约束**：**禁止** INLINE HOOK（E9 / `.text`）；只允许 **MethodInfo.methodPointer 换针** + 已有 `mob_scan::RequestImmediateScan`  
> **语义真源**：[`P0a_OnLocalMob与Init包体.md`](P0a_OnLocalMob与Init包体.md)（含 §7.6 SetRemote 条件踢池）  
> **现成 API**：`x/features/mob_scan/mob_scan.h` → `RequestImmediateScan()`（`simple_combat` 已在用）

---

## 0. 一句话

用 **Il2Cpp MethodInfo 换针**观察 `MobPool` 进/离场（及可选控权创建），在 hook 里 **只唤醒** 已有 `mob_scan` worker 立刻 `Collect`——把「1ms 空转热扫描」换成「事件驱动补扫」。

---

## 1. 目标与非目标

| 要 | 不要 |
|---|---|
| 新怪进池后尽快进 `MobLite` 缓存 | 改 GA `.text` / E9 |
| 怪离场后尽快从缓存消失 | 解析全包、伪造发包 |
| 复用 `RequestImmediateScan` | 在 hook 里重做 Collect / FindAll |
| 默认可关、可打日志 | 默认改变打怪语义以外的行为 |
| | 把 `SetRemoteMob` 当成离场 |

---

## 2. 为何够用（相对 1ms 轮询）

| 路径 | 延迟来源 | 事件驱动后 |
|---|---|---|
| 周期 `mob_scan` | 最长一个 combat interval（现可调到 1ms≈忙等） | 进场/离场边沿立刻醒 |
| `simple_combat` 已 `RequestImmediateScan` | 只覆盖「自己换目标/杀怪」 | 补上「别人抢刷新 / 新刷」 |

不替代周期扫描（兜底仍保留，间隔可回到默认 50ms 级）。

---

## 3. 挂点矩阵（P0a 结论）

| 优先级 | restored 方法 | RVA | 观察时机 | 调 `RequestImmediateScan`？ |
|---:|---|---:|---|---|
| **P0** | `OnPacketMobEnterField` | `0xF75DD0` | **创建路径** `Mob.Init` 返回后（或函数尾） | ✅ |
| **P0** | `OnPacketMobLeaveField` | `0xF763C0` | **dict Remove**（`86D9DF60`）之后或函数尾 | ✅ |
| P1 | `SetLocalMob` | `0xF74340` | 仅 **创建**（`CreateMob`+`Init`+`SetActive(true)`）尾 | ✅ 补集 |
| — | `SetRemoteMob` | `0xF74880` | 见 §3.1 | ❌ 默认不挂；见下 |

### 3.1 `SetRemoteMob`（已钉）

```text
SetActive(false)
若 _inViewSplit@0x100 == 0 → dict.Remove
若 _inViewSplit != 0     → 不踢池（EnterField 进场怪多为 true）
```

- **不要**当 Leave 触发 Collect。  
- 若日后发现「不踢但仍占活怪榜」（`FillLite` 不看 `VecCtrl.Active`），再单开：要么挂 DF60 边沿，要么在 FillLite 加 Active/`IsReady` 交叉（**另开任务**）。

### 3.2 为何优先包入口而不是 `CreateMob`/`Init`

| 挂 `EnterField`/`LeaveField` | 挂 `Init`/`CreateMob` |
|---|---|
| 一包一个语义事件 | `Init` 还有 ChangeController 创建调用 |
| Leave 必踢池 | `CreateMob` 无离场对称点 |
| MI 虚派发（无静态 E8）正合适 | `Init` 有静态 call，但也更吵 |

---

## 4. 实现骨架（设计级 · 未落码）

### 4.1 模块建议落点

```text
x/features/mob_pool_observe/     # 或并入 mob_scan 旁路
  mob_pool_observe.{h,cpp}       # Install / Uninstall MI
挂入：xcat_probe 在 mob_scan::Init 之后；开关默认关
```

面板 / ini（建议，未实现）：

```ini
[core]
mobPoolObserve=0    ; 0=关 1=开
```

### 4.2 MethodInfo 解析

复用 `x/runtime/il2cpp_method`：

| 项 | 值 |
|---|---|
| Klass | `MobPool` hash `f4afa0ce…13ec02`（`mob_pool_port` SSOT） |
| 解析 | `FindMethodByRva(klass, RVA)` + shape 校验（`void` + `InPacket*` arity） |
| 换针 | 与 `attack_accel` / `kick_sniff` 同构：`VirtualProtect` 写 `MethodInfo.methodPointer` |
| 原指针 | 保存；hook 末尾 `call` 原函数（或先 call 再观察——见 §4.3） |

### 4.3 Hook 时序（推荐）

**EnterField**

```text
hook(this, pkt, mi):
  orig(this, pkt, mi)           // 先让官方跑完 Init / Insert
  if 本包曾创建（见下）或「尾扫兜底」:
      mob_scan::RequestImmediateScan()
  可选 log: mobId / templateId
```

创建判定（任选其一，实现时定）：

1. **前后 dict 计数 / raw 差**（简单，可能误报）  
2. **只在 orig 后读 pkt 已不适用**（已被消费）  
3. **实用折中**：EnterField **每次** orig 后都 `RequestImmediateScan`（更新路径多醒一次，成本=一次 SetEvent，Collect 仍有自己的节流/锁）

推荐先做 **(3)**，BIN 看日志噪声再收紧。

**LeaveField**

```text
hook(this, pkt, mi):
  orig(...)
  mob_scan::RequestImmediateScan()   // 两路均踢池，无需读 leaveMode
```

### 4.4 线程与安全

| 项 | 要求 |
|---|---|
| 调用线程 | 网络/IL2CPP 管理线程（与其它 MI 观察一致） |
| hook 内禁止 | 重 Collect、FindObjects、长时间锁、抛异常 |
| 允许 | `RequestImmediateScan`（原子+SetEvent）、轻量 log |
| 重入 | `RequestImmediateScan` 已是 auto-reset 事件；多次边沿合并为一次醒 |
| 卸载 | Shutdown / detach 先 Restore MI，再停 worker |

### 4.5 与周期扫描的关系

```text
combat interval：建议恢复默认 ~50ms（或用户可调）作兜底
事件：Enter/Leave → 立即醒
simple_combat 换目标：保持现有 RequestImmediateScan
```

---

## 5. 日志与验收

### 5.1 日志（建议）

`mobscan.log` 或独立 `mobpool_obs.log`：

```text
obs enter via=MI
obs leave via=MI
obs scan wake reason=enter|leave|combat|force
```

已有 `mobscan n=…/M=…` 行：看事件后 **下一行** 的 `n`/`raw` 是否边沿变化。

### 5.2 BIN 清单（实现后）

| # | 场景 | 期望 |
|---:|---|---|
| 1 | 开观察、关 1ms；图内刷怪 | enter 日志后短延迟内 `n`↑ |
| 2 | 杀怪 / 怪自然死 | leave 后 `n`↓ |
| 3 | 换控制器（他人控） | **无** leave 误触发；`n` 不无故掉光 |
| 4 | 关观察 | 行为回退为纯周期扫描 |
| 5 | 进图/换频/卸载 | 无残留 MI、无崩溃 |

---

## 6. 风险与缓解

| 风险 | 缓解 |
|---|---|
| Enter 更新路径多余 wake | 先接受；再加「仅 raw↑」门 |
| SetRemote 留尸在池 | 不挂 SetRemote；必要时另改 FillLite |
| RVA 漂移 | hash 名 + RVA fallback（与 port 同代 remount） |
| 双 hook 顺序 / 重入 | 只 SetEvent；Collect 仍单 worker |
| 与其它 MobPool MI 冲突 | 安装前读回 methodPointer，卸载还原 |

---

## 7. 分阶段落地（未授权不执行）

| 阶段 | 内容 | 代码？ |
|---:|---|---|
| **A（本文）** | 语义 + 设计 | ❌ 已完成文档 |
| B | IDA 改名（可选） | IDA only |
| C | `mob_pool_observe` MI×2 + ini 开关 + 日志 | 需你点头 |
| D | BIN 验收 §5.2 | 实机 |
| E | 视情况收紧 Enter 门控 / FillLite Active | 另开 |

---

## 8. 锚点速查

| 符号 | RVA | VA（imagebase `0x7FFB83A80000`） |
|---|---:|---|
| `OnPacketMobEnterField` | `0xF75DD0` | `0x7FFB849F5DD0` |
| `OnPacketMobLeaveField` | `0xF763C0` | `0x7FFB849F63C0` |
| `SetLocalMob`（P1） | `0xF74340` | `0x7FFB849F4340` |
| `SetRemoteMob`（默认不挂） | `0xF74880` | `0x7FFB849F4880` |
| dict Remove | — | `0x7FFB86D9DF60` |
| `RequestImmediateScan` | — | `mob_scan.cpp` |

---

## 9. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-06 | 初稿：P0 Enter/Leave MI → RequestImmediateScan；SetRemote 条件踢池结论并入；明确不改代码 |
