# mob_pool P0c · Enter → 开火时间线与门控（只读）

> **状态**：🔍 静态对照产品代码 · **本轮不改产品代码**  
> **产品**：经典版 TWMS · **不是**枫星  
> **目的**：钉清「新怪进池 → `simple_combat` 出站攻击」每一段延迟与硬/软门，回答「瓶颈在 Collect 还是在出刀」  
> **上游**：[`P0a`](P0a_OnLocalMob与Init包体.md) · [`P0b`](P0b_MI观察与按需Collect.md)  
> **出站真源**：`attack_input_port`（OnFuncKey）；对照 [`attack_rpc/P0b`](../attack_rpc/P0b_出站Encode与Send锚点.md) · [`attack_speed/P0a`](../attack_speed/P0a_出刀忙锁与Prepare链.md)

---

## 0. 结论先行

1. **合法最早开火点（我们侧）**：`EnterField` 创建尾 `IsReady=true` 且坐标可被 `FillLite` 接受 → 进入 `mob_scan` 缓存 → `simple_combat` `Acquire` 选中 →（可选 `MoveTo`/`Settling`）→ `Aim`/`Firing` → `TryFirePrimary` → 主线程 `OnFuncKey` → 游戏 `TryDoing*` → **`MobPool_FindHitMobInRect` 命中** → `OutPacket` 攻包。  
2. **相对「别人抢怪」**：P0b / 截包只压缩 **T0→T2（感知）**；实战多数时间花在 **T3→T6（贴身 / 带内 / 间隔 / 主线程泵）** 与 **RTT**。  
3. **相对「自己旧轮询」**：`mob_scan` 默认周期与 `kMobCacheFreshMs=16` 已在一帧量级；P0b 的收益是去掉空转热扫 + 边沿立刻醒，**不是**再抠半帧抢怪。  
4. **官方命中过滤（本轮已钉）**：`FindHitMobInRect` **要求** `_inViewSplit@0x100 != 0`，**拒绝** `_suspended@0x1B8 != 0`；**不读** `MobCtrlState@0xE8`、`IsReady@0xEC`、`_deadType@0x1B4`，也**不调** `Mob_e544`（VecCtrl.Active）。→ **不必等 ChangeController / SetLocalMob**；EnterField 已写 `inViewSplit=true`。`_suspended` 寿命见 [`P0d`](P0d_suspended与initDelay.md)（Init 常 true → Update 多在约 1 帧内清）。  
5. 我方 `FillLite` 仍用 `IsReady`/`deadType`/hp（与官方命中门不完全同构）；`CtrlPreferRank` 只是选怪软优先。

---

## 1. 端到端时间线

```text
T0  服务器广播 EnterField
T1  本机收包 → 解密 → opcode 分发（MI 虚派发；EnterField 无静态 E8）
T2  OnPacketMobEnterField
      · 创建：CreateMob + Mob.Init → IsReady=true · _inViewSplit=true
      · 更新：只 STS + inViewSplit（不重 Init）
      · ★ 不调 SetActive
T3  mob_scan Collect / TryRefreshCacheLite → Snapshot（FillLite 门）
T4  simple_combat Tick · Acquire · PickNearestTarget（Ctrl 软优先）
T5  [可选] MoveTo fill+Doing · Settling(≥32ms PosSane) · Aim
T6  Firing · FireGateOk · TryFirePrimary · OnFuncKey(Down[/Up])
T7  游戏 TryDoing* / SetAttackAction · Encode · Network.Send
T8  服务器结算（与他人比的是 T8 到达序）
```

| 段 | 谁决定 | 量级（本机，粗估） | 对「抢别人」 |
|---|---|---|---|
| T0→T1 | 网络 RTT/抖动 | ms～数十 ms | **主战场**（改不了别人） |
| T1→T2 | 客户端包处理 + Init | 通常亚 ms～数 ms | 截包略早于 Init 尾；**常打不出去** |
| T2→T3 | `mob_scan` 周期 / `RequestImmediateScan` | 默认 **`mobScanIntervalMs=20`**；边沿唤醒 ≈ worker 醒 + Collect；选怪另要求缓存 ≤**16ms** | 只赢自己旧缓存 |
| T3→T4 | combat tick | 默认 **`kSimpleCombatTickDefaultMs=16`** | 一帧量级 |
| T4→T5 | 距离 / 瞬移 CD / 预算 / softBan | **0～数百 ms+** | 常是本地主耗时 |
| T5→T6 | 命中带 + 攻击间隔 + hold/pulse | 间隔面板值；加速开则短 | 节奏门，非感知 |
| T6→T7 | 主线程泵 / 游戏忙锁 | 泵拥堵可 defer | 本地 |
| T7→T8 | 上行 RTT | ms～数十 ms | **主战场** |

---

## 2. T2 之后：池内「可被我们看见」的门（FillLite）

源：`x/features/ports/mob_pool_port.cpp` · `FillLite`

| 门 | 条件 | 硬？ |
|---|---|---|
| Unity 活着 / Klass | `UnityObjectAlive` + Mob klass | 硬 |
| oid | `id != 0` | 硬 |
| **IsReady** | `+IsReady != 0`（Init 尾写入） | 硬 |
| deadType | `== 0` | 硬 |
| hpPct | `> 0` | 硬 |
| 坐标 | 有限、非原点脏、不超界；可回退 VecCtrl Ap | 硬（脏则跳过） |
| 特殊模板 | 排除占位 tpl | 硬 |
| **VecCtrl.Active** | **不读** | — |
| **MobCtrlState** | 只写入 `MobLite.ctrl`，不挡入榜 | — |

含义：

- EnterField 创建尾 `IsReady=true` 后，**只要坐标已种好**，即可进 Snapshot。  
- SetRemote 失活但未踢池时，若仍 ready/hp>0，**仍可能占活怪榜**（P0a §7.6 风险；实机 **NOT RUN**）。  
- 「我方控」只影响 `PickNearestTarget` 的 `CtrlPreferRank`（软优先），**不是** FillLite 硬门。

---

## 3. T3：缓存新鲜度（感知层）

| 机制 | 值 / 行为 |
|---|---|
| `mob_scan` combat 周期 | 面板 `mobScanIntervalMs`（默认见 `xcat_payload_control`） |
| `RequestImmediateScan` | SetEvent 醒 worker；换怪 /（设计中）Enter/Leave 用 |
| `EnsureFreshMobSnap` | 缓存年龄 > **`kMobCacheFreshMs=16`** → 叫醒 + `TryRefreshCacheLite`（字典只读，战斗线程禁完整 Collect） |
| 锁存续 | `RefreshLock` → **`TryFillLive(ptr)`**，不等缓存（死了立刻切） |

**推论**：选怪路径已按 ~1 帧要求新鲜度；再把扫描打到 1ms，对 T3→T4 几乎无增益。P0b 价值 = **边沿唤醒 + 降默认扫描功耗**。

---

## 4. T4–T6：`simple_combat` 状态机门控

状态：`Idle → Acquire → [MoveTo→Settling] → Aim → Firing → Recover → …`  
Worker 默认 tick：**16ms**；同 tick 最多 5 pass（Aim/Firing/Recover 连跑）。

### 4.1 顶层停刀（进状态机前）

| 门 | 效果 |
|---|---|
| `!IsPlayReady` | GoIdle |
| 换图 / 坏坐标 `kMapArmGraceMs=1500`（坏坐标可更短档） | **禁止 tp/fire** |
| `pet_feed::ShouldHoldCombatForSummon` | 停刀等召唤 |
| `HardPause` / ExternalPause / FireSuppressed | 停刀 |
| `QueryCombatCtx` 失败 | 可能再武装 |

### 4.2 Acquire / 选怪

| 门 | 效果 |
|---|---|
| Snapshot 不够新 / `!ok` | return（只催 scan） |
| `PickNearestTarget` | softBan / 落点 / 同层优先 / 群怪密度 / **CtrlPreferRank** |
| 贴怪关 + 跨层 | softBan 4s，清锁 |
| 已在命中带 | → Aim；否则贴怪开 → MoveTo，关 → Aim（站桩可打） |

### 4.3 MoveTo / Settling（贴怪开时主耗时）

| 门 | 效果 |
|---|---|
| 瞬移 CD / 位移预算 / 跨层 fill_gate | 干等或拒跳 |
| `EstimateLand*` / LandSafe | no_land → softBan |
| Settling | `kPostDoingMinSettleMs=32`（跨层 64）；等 PosSane；产品 settle 时间窗多为 0 |
| 贴怪瞬移 | **不再等** `MotionBusy`（与出刀节奏解耦） |

### 4.4 Aim → Firing

| 门 | 效果 |
|---|---|
| `InHitBand` / `InMeleeHoldBand` | 才进 Firing |
| 跨层 | 贴怪开重贴；关则 ban |
| sticky / standstill / whiff 纠偏 | 可推迟开火 |

### 4.5 Firing → `TryFirePrimary`

| 门 | 层 | 效果 |
|---|---|---|
| `IsFireSuppressed` | combat + attack | 停 |
| `IsPreparingSkill` | combat | 停 |
| `FireGateOk` | combat | 不在带 → 回 Aim / 重贴 |
| 主线程泵拥堵 | combat + attack | Recover `pace_wait_pump` / soft skip |
| `SoftBlocked`（间隔） | attack | soft；不计 fail |
| pendingUp / hold | attack（非 pulse） | 松键前挡下一刀 |
| `ApplyFaceNow` | attack | SetInput 朝向 |
| `OnFuncKey` Down[/pulse Up] | attack | **产品唯一出刀真源** |

源：`attack_input_port.cpp` · `TryFirePrimary`；**不**走 `attack_rpc_port` 伪造包（那是探针）。

---

## 5. T7：游戏内出刀与命中过滤（本轮 IDA 钉死）

产品路径：

```text
OnFuncKey → TryDoing*（AntiRepeat / SetAttackAction）
  → MobPool_FindHitMobInRect（组 AttackInfo / 命中环）
  → OutPacket Create(50..53) → Encode → Network.Send
```

当前 IDB（`imagebase 0x7FFB83A80000`）关键锚点：

| 符号 | VA | RVA |
|---|---|---:|
| `MobPool_FindHitMobInRect` | `0x7FFB849E8670` | `0xF68670` |
| 调用它的出刀大函数（含 `AntiRepeat_TryRepeat`） | `0x7FFB84B46EC0` | `0x10C6EC0` |
| Melee 候选（doc 旧中点 `0x10B0EE0` 落其内） | `0x7FFB84B2ED40` | `0x10AED40` |
| Normal 辅助/落空候选（旧中点 `0x10C59D0` 落其内） | `0x7FFB84B45350` | `0x10C5350` |

> 旧文档把「函数中部 RVA」写成入口；现网以 **函数 start** 为准。命名仍可按 opcode/Encode 序与 P0c_攻包BODY 对齐，不阻塞本结论。

### 5.1 `FindHitMobInRect` 对单只 Mob 的硬门

证据：对 `r13=Mob*` 的非栈位移读 + 分支（`suspended` 的 `cmovnz→rsi` 与 `inView` 的 `cmovnz→rdi` 对照，**rsi=拒 / rdi=续**）：

| 字段 | 偏移 | 判定 | 结果 |
|---|---:|---|---|
| `_inViewSplit` | `0x100` | `==0` → 拒；`!=0` → 续 | **EnterField 写 true → 过** |
| `_suspended` | `0x1B8` | `!=0` → 拒；`==0` → 续 | Init 常 true；Update 多在首帧清（[`P0d`](P0d_suspended与initDelay.md)） |
| `_mobId` | `0x134` | 读出填命中 | — |
| `MobCtrlState` | `0xE8` | **本函数不读** | — |
| `IsReady` | `0xEC` | **本函数不读** | — |
| `_deadType` | `0x1B4` | **本函数不读** | — |
| VecCtrl.Active（`Mob_e544`） | — | **无 xref 到本函数 / TryDoing 命中路径**；仅 Leave/SetRemote/Chase 等 | — |

附加 callee（几何/属性，非控权）：

| callee | 角色（静态） |
|---|---|
| `Mob_GetBodyRect` | 身体矩形 |
| `Mob_ad53…` | 命中族共用谓词（未见读 `+E8/+EC/+1B4`） |
| `Mob_ea3c…` | 读 `_stat@0x150`，解混淆比较常量 **0**（`0x9B6C1BD3^seed@7DE8`） |
| `Mob_c282…` | `data@0x138` → `MobData+0xA9`（`IsDamagedByMob`） |

### 5.2 对「抢先开火」的含义

| 假设 | 结论 |
|---|---|
| 必须本地 `MobCtrl>0` 才能进命中环 | ❌ **否** |
| 必须 `SetActive(true)` / `e544` | ❌ **否**（出刀命中路径不调） |
| 必须等 `ChangeController→SetLocalMob` | ❌ **否**（控权≠可打） |
| EnterField 后 `_inViewSplit=true` 且未 suspended | ✅ **即可被 FindHit 选中**（还要矩形相交等） |
| 我方 `FillLite` 的 `IsReady` 门 | 仍有效（产品选怪）；与官方命中门 **不完全同构** |

`attack_accel` 清忙锁、`SoftBlocked` 间隔仍约束 **出刀节奏**，与「怪可不可以被打到」是两层问题。

---

## 6. 瓶颈排序（证据化）

| 优化 | 压缩段 | 预期对「先打到」 | 优先级 |
|---|---|---|---|
| 降 RTT / 少空刀 / 更快进带 | T4–T8 | **高** | 产品调参 / 贴怪节奏 |
| 攻击间隔 / pulse / 清忙锁 | T6–T7 | 高（已有加速） | 已部分落地 |
| P0b MI Enter/Leave → ImmediateScan | T2→T3 | **低～中**（自用新鲜度） | 设计已写，未落码 |
| 截包早于 EnterField | T1→T2 | 低（常不可打） | 不优先 |
| 1ms 热扫 | T2→T3 | 极低 | 不建议 |
| 等 SetLocalMob 再开火 | — | **无必要**（本轮证伪） | 勿做 |

---

## 7. 仍待静态 / 实机

| # | 问题 | 状态 |
|---:|---|---|
| 1 | MobCtrl / Active 是否挡命中环 | ✅ **已钉：不挡**（§5.1） |
| 2 | `_suspended` / `_initDelay` 寿命 | ✅ 见 [`P0d`](P0d_suspended与initDelay.md)：Init 常 true；Update 在 `GetUpdateTime()>delay` 时清；小 delay ⇒ **约 1 帧**；`delay==0` 例外走包侧 |
| 3 | `Mob_ad53` / `_stat+0x18C` 精确语义 | 低优先级细拆 |
| 4 | DelayedDead / `_deadType` 写入点 | ✅ 见 [`P0e`](P0e_SetDeadType与deadType.md)；观察见 [`P0f`](P0f_SetDeadType观察方案.md)（未落码） |
| 5 | SetRemote 留池是否污染 `FillLite` 选怪 | 实机（官方仍可能命中 remote） |
| 6 | 服务器是否另验控权（客户端可打 ≠ 服结算） | **服务端黑盒 · NOT RUN** |

---

## 8. 锚点速查（产品侧）

| 符号 / 常量 | 位置 |
|---|---|
| `FillLite` | `mob_pool_port.cpp` |
| `CtrlPreferRank` | `mob_pool_port.h` |
| `kMobCacheFreshMs=16` | `simple_combat.cpp` |
| `kSimpleCombatTickDefaultMs=16` | `xcat_payload_control.h` |
| `EnsureFreshMobSnap` / `NoteNeedFreshMobs` | `simple_combat.cpp` |
| `TryFirePrimary` / `SoftBlocked` | `attack_input_port.cpp` |
| EnterField RVA | `0xF77C40`（2026-08-06 remount；旧 `0xF75DD0`） |
| `FindHitMobInRect` RVA | `0xF6A4E0`（旧 `0xF68670`） |

---

## 9. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-06 | 初稿：Enter→开火时间线 + FillLite/combat/attack 门控表；明确感知非抢怪主因；列出 TryDoing 控权缺口 |
| 2026-08-06 | **钉死**：`FindHitMobInRect` 要 `inViewSplit`、拒 `suspended`；**不**要 MobCtrl/Active/IsReady；证伪「等 SetLocalMob」 |
| 2026-08-06 | 链到 [`P0d`](P0d_suspended与initDelay.md)：suspended 解除条件与约 1 帧窗口 |
| 2026-08-06 | 链到 [`P0e`](P0e_SetDeadType与deadType.md)：`SetDeadType` 唯一写；Leave 不写；调用方静态 BLOCKED |
| 2026-08-06 | 链到 [`P0f`](P0f_SetDeadType观察方案.md)：SetDeadType 运行时观察设计（未落码） |
| 2026-08-06 | 晚间 remount：见 [`REMOUNT_20260806`](REMOUNT_20260806.md)；Enter/FindHit RVA +0x1E70 |
