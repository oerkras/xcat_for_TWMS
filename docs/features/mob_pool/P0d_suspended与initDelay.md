# mob_pool P0d · `_suspended` / `_initDelay` 生命周期（只读）

> **状态**：🔍 静态结案（时长量级有条件）· **本轮不改产品代码**  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase **`0x7FFB83A80000`**  
> **上游**：[`P0c_Enter到开火时间线.md`](P0c_Enter到开火时间线.md)（FindHit 拒 `_suspended`）· [`P0a`](P0a_OnLocalMob与Init包体.md)  
> **字段真源**：`dump.cs.restored` Mob：`_initDelay@0x1B0` · `_suspended@0x1B8`

---

## 0. 结论先行

1. **`_suspended==true` ⇒ `FindHitMobInRect` 拒收**（P0c §5.1）。这是进场后「看得见但打不进命中环」的客户端硬门。  
2. **`Mob_Init` 多条刷怪特效分支把 `_suspended` 写成 `true`**（4 处字节种子均解出 **1**）。  
3. **主解除路径**：`Mob_ca347…`（仅 data/MI xref → **Mob 每帧 Update**）在 `_initDelay != 0` 且  
   `GetUpdateTime() - _initDelay > 0` 时：清 `_initDelay=0` →（可选）精灵淡出虚调 → **`_suspended=false`**（`byte@7FC8 ^ 0x2A = 0`）。  
4. **时长量级（条件结论）**：Init 若干路径把 `_initDelay` 写成 **Wz 延迟经 MBA 后的小浮点**（见 §3，如 `wz+800` 或强制 `1.0f`），而 `GetUpdateTime` 为 **世界钟 ms（`_updateTime*scale`）**、量级远大于该值 → **首帧 Update 即满足 `now > delay`，suspended 窗口 ≈ 1 次 Mob Update（通常 ≤1 帧）**。  
5. **例外**：Init 若留下 `_initDelay==0` 且 `_suspended==true`，Update 的延时解除**不会进**；需靠包处理侧 `Mob_d010…` / `Mob_d2af…` 等写 `false`（§4）。  
6. **服务端是否另有无敌窗**：黑盒 · **NOT RUN**。

---

## 1. 谁读（命中相关）

| 函数 | 行为 |
|---|---|
| `MobPool_FindHitMobInRect` | `movzx` `+1B8`；非 0 → 拒 |
| `FindBodyAttackMob` / `FindHitMobInManyRects` / `FindHitUndead…` / `FindHit…TrapezoidPlural` | 同构读 |
| `Mob_IsTargetInAttackRange` | 读 |
| `Mob_ca347…`（Update） | 读；门控后续 AI/动作 |

---

## 2. 谁写 `_suspended`

### 2.1 `Mob_Init`（RVA `0xEFBDF0` · VA `0x7FFB8497BDF0`）

| 位点 | 种子算式 | 解出 |
|---|---|---:|
| `…7DD55` | `byte@7BF8(46) ^ 0x2F` | **1** |
| `…7E299` | `byte@7BFC(101) + 0x9C` → mod 256 | **1** |
| `…7E532` | `byte@7BF0(141) + 0x74` → mod 256 | **1** |
| `…7E56D` | `byte@7BF4(44) ^ 0x2D` | **1** |

均在刷怪特效 / summon / alpha 相关 CFF 块附近；**未见 Init 内直接写 false**。

### 2.2 Update 解除（主路径）

| 位点 | 算式 | 解出 |
|---|---|---:|
| `Mob_ca347…` `…8EF9F` | `byte@7FC8(42) ^ 0x2A` | **0** |

前置条件见 §3。

### 2.3 其它写点

| 函数 | 解出 | 调用方（静态） |
|---|---:|---|
| `Mob_d010…` `…B3A6D` | `byte@8A00(21)+0xEB` → **0** | `MobPool_d87ed2…`（包/池处理） |
| `Mob_d2af…` `…C8BDB` | `byte@8F4C(30)^0x1E` → **0** | `MobPool_b6d265…` |
| `Mob_a7c6…` `…B38E8` | `byte@89EC(182)^0xB7` → **1** | 仅 data xref（MI） |
| `Mob_e6c1…` `…BE5C4` | `byte@8C50(206)^0xCF` → **1** | `MobPool_d87ed2…` |

---

## 3. `_initDelay` 与解除条件

### 3.1 Init 写入（仅 `Mob_Init` + Update 清零）

| 位点 | 写入 |
|---|---|
| `…7DD41` / `…7E27F` | `dword = 0` |
| `…7E234` | `float = MBA(wz相关int)`；样本 MBA：**`x → x+800`** |
| `…7E482` | `float = MBA(GetInt/时间组合)`（更复杂；疑似 **now+delay** 族，未逐项代数化完） |
| `…7E4A8` | 若上一步浮点为 0 → **强制 `1.0f`**（`0x3F800000`） |
| Update `…8EF42` | 到期后 `dword = 0` |

Wz 来源：Init 特效块内 `WzJsonNode_GetInt` / JSON 读（刷怪 effect 延迟类字段）。

### 3.2 Update 到期逻辑（净效果）

```text
t = GetUpdateTime()           // sub_7FFB84842010：静态 float × scale → int（世界钟 ms）
d = (float)_initDelay

if d == 0:
    跳过解除块          // ★ 不会在此清 suspended
else:
    if (float)t - d > 0: // 即 t > d
        _initDelay = 0
        [可选] 精灵虚调淡出
        _suspended = false
```

时间函数：`sub_7FFB84842010`（与攻包 `tOrKey` / buffs 文档中的 **WorldManager.GetUpdateTime** 同族；Update 里 `var_8C =` 其返回值）。

旁路：同函数内另有对 `_initDelay` 做 `+= -500.0f`（常量 `@FEDCF4`）的块——更像倒计时/分段，**置信度低于主比较**；主结论不依赖它。

### 3.3 窗口量级

| Init 留下的 `d` | 与 `GetUpdateTime()` 比较 | 预期 suspended 持续 |
|---|---|---|
| `wz+800` / `1.0` 等**小值** | 世界钟已远大于 `d` | **首帧 Update 即清**（≤1 次 Update） |
| **绝对截止** `now+wz`（若走组合 MBA 路径） | `t > now+wz` | **约 wz ms** |
| `d == 0` 且 suspended 仍 true | 本路径不解除 | 等 §2.3 包侧写 false；否则一直拒命中 |

实机要 BIN：进场后 `combat`/`mobscan` 侧「首刀空」是否仅 1 帧——标 **时长实机 NOT RUN**。

---

## 4. 与开火时间线的关系（回到 P0c）

```text
EnterField → Init（常 suspended=true，inViewSplit=true，IsReady=true）
  → 同帧或下一帧 Mob Update：多半清 suspended
  → FindHit 才收进命中环
  → 我方 FillLite 早在 IsReady 后即可选怪（不看 suspended）
```

含义：

- 我方可能 **先锁怪 / 先 OnFuncKey**，但官方命中环仍可能空一帧（whiff / mobCount=0）。  
- **不必等 SetLocalMob**；要等的是 **`_suspended` 落下**（通常极短）。  
- 优化感知（P0b）帮不上这段；贴身/间隔仍是大头。

---

## 5. 锚点速查

| 符号 | VA | RVA |
|---|---|---:|
| `Mob_Init` | `0x7FFB8497BDF0` | `0xEFBDF0` |
| `Mob_ca347…`（Update） | `0x7FFB8498A520` | `0xF0A520` |
| `GetUpdateTime` 族 | `0x7FFB84842010` | `0xDC2010` |
| `FindHitMobInRect` | `0x7FFB849E8670` | `0xF68670` |

---

## 6. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-06 | 初稿：读写点全表、Init 种子全解出 1、Update 解除条件、与 GetUpdateTime 比较；窗口量级条件结论 |
