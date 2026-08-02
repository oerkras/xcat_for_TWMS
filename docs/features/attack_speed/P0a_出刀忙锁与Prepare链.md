# attack_speed P0a · 出刀忙锁与 Prepare 链

> **状态**：✅ 静态结案  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · `0x7FFB16B40000`  
> **上级**：[`模块设计.md`](模块设计.md)

---

## 1. 调用链（普攻）

```text
OnFuncKey(BasicActionAttack)
  → jumptable → TryDoingNormalAttack (RVA ~0x10C70B0)
       ├─ AntiRepeat_TryRepeat          // 排除为攻速
       ├─ LocalUser_SetAttackAction     // ★ 共用忙锁入口
       ├─ GetActionInfo / Afterimage
       └─ OutPacket 攻包
```

技能侧：`TryDoingMagicAttack` / `SendSkillUseRequest` 等同调 `SetAttackAction` 与 `AntiRepeat`。

### 1.1 `LocalUser_SetAttackAction`

| | |
|---|---|
| VA | `0x7FFB17B150A0` |
| RVA | `0xFD50A0` |

关键副作用（语义）：

1. `[UserBase+0x114] = 5000`
2. busy 谓词：`UserBase_fad1d863…() >= 0`（常量解出 **−1**，`setnle`）
3. 虚表 **Slot32** `PrepareActionLayer(actionIdx, 100, false)`  
   - LocalUser 实现会用 TemporaryStat 派生 speed **覆盖**第 2 参（名义 100）
4. 成功：`[UserBase+0x118] = actionIdx`

失败（仍 busy）则本次出刀被挡。

---

## 2. 忙锁字段

| 偏移 | 角色 |
|---|---|
| `UserBase+0x114` | 与动作相关的时长/窗（SetAttackAction 写 5000） |
| `UserBase+0x118` | **当前动作索引**；**`-1` = 空闲可再出刀** |

### 2.1 解锁路径（Slot14 Update）

`UserBase` Slot14 每帧 Update（研究中记为 `UserBase_f6c6…` @ `0x7FFB17D7F520` 一带）：

```text
每 tick:  [actionLayer+0x14] -= 30
帧前进…
完成时:
  [UserBase+0x118] = -1
  清 +0x128 相关
  PrepareActionLayer(6, 100, false)   // 回到待机类动作
```

因此：**缩短 layer 剩余延迟 ⇒ 更早把 `+0x118` 置 −1 ⇒ 更早允许下一刀**。

### 2.2 产品侧（2026-08-03 结案）

现行 `attack_accel` **已周期写 `+0x118=-1`**，再出刀不再依赖等 layer 扣完。  
另写 `SS+0x1BC=140` 在 Prepare 时缩短 delay 表（动画/抬手）。  
**不**做运行时强改 `layer+0x14`：在已有清忙锁下对 DPS 近似纯视觉，且易 whiff——见 [`模块设计.md`](模块设计.md) §5。

---

## 3. `PrepareActionLayer` 与 ActionSpeed

LocalUser 覆盖路径（语义）：

```text
speed = SecondaryStat_GetActionSpeed(TS)
clamped = Min(Max(speed, 70), 140)
delay' = baseDelay * 100 / clamped
```

- `Math.Max` @ `0x7FFB1A97E970` · `Math.Min` @ `0x7FFB1A97EAB0`
- 基延迟来自动作帧表（WZ / ActionData）；客户端用 speed 缩放

`GetActionSpeed` 公式见 [`P0b`](P0b_双速系统与字段表.md)。

---

## 4. 虚表槽（UserLocal / UserBase）

| Slot | 语义 |
|---|---|
| 32 | `PrepareActionLayer` |
| 30 | `ChangeFrame` |
| 18 | `SetFlip` |
| 14 | 每帧 Update（解锁忙锁） |

---

## 5. 已排除项（证据摘要）

| 候选 | 结论 |
|---|---|
| `m_tHitPeriodRemain(+0x298)` | invuln 常驻写 5000 仍可攻击；`SetDamaged` 早退用 |
| `AntiRepeat` | `_repeatCount` / 同坐标；CountLimit 解出 100 |
| 门函数 `sub_7FFB17BA41A0`（RVA `0x10641A0`） | Sit / TS / Dead / hitstun |
| `UserLocal+0x488` | `SetDamaged` 置位、Slot14 条件清零；受击封锁 |
| `RegisterOneTimeAnimation(..., 0, 1000)` | 特效；固定 1000；不解锁 `+0x118` |

---

## 6. 与本仓实现的关系

| 层 | 模块 | 现行 |
|---|---|---|
| 引擎忙锁 / Prepare | `attack_accel` | 清 `+0x118` + 写 `+1BC=140` |
| 自研出刀频率 | `attack_input_port` + 面板间隔 | `simpleCombatAttackIntervalMs`（默认 333，下限 5）；hold/pendingUp |
| 打怪状态机 | `simple_combat` | 贴怪/Recover 等；不替代上述两层 |

产品总述见 [`模块设计.md`](模块设计.md)。

---

## 7. NOT RUN / 已由产品 BIN 覆盖

- ~~进图后 `+1BC` 是否粘住~~ → ✅ `attack_accel.log` `speed=140`（2026-08-03）
- 技能 vs 普攻 busy 重叠的帧级对照（仍可选）
