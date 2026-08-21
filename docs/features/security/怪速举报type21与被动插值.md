# 怪速举报 type21（MobSpeedHack）· 被动插值界限检查（Classic TWMS）

> **产品**：新枫之谷：经典版（TWMS / `Maplestory_Classic.exe`）。**不是**枫星。
> **状态**：TW IDA 常量/xref 解码（CFF 去混淆 + **实读种子验算**）+ CMS 符号对照
> **证据源**：`Dumps/runtime/GameAssembly.dll.i64`（运行时 dump，imagebase `0x7ffd60830000`）· `Dumps/cms_cw/dump.cs`（语义名）· `x/features/ports/security_attack_port.cpp`（锚点 RVA）
> **日期**：2026-08-21
> **目的**：把「吸怪→软重连」溯因里对 type21 的怀疑**证伪**（§0–§6：type21 是被动怪插值时间越界检查、节流 5 分钟、无 px/s 天花板）；并锁定真正逐帧上报的 `Mob.SendMobPrevPosHack` 链——解其位移阈值/确认无节流、量化预武装窗口（§7）。
> **关联**：[`客户端Hack标志与服端推断.md`](客户端Hack标志与服端推断.md) · [`攻包计数窗与type20.md`](攻包计数窗与type20.md) · [`../mob_gather/模块设计.md`](../mob_gather/模块设计.md) · `x/features/ports/mob_fh_ban.cpp`

---

## 0. 一句话

`ClientHacksType.MobSpeedHack = 21` 的唯一触发源是 **`MovePath.CalcPassivePos`** 里对**被动怪**（客户端只外推、不接管的怪）插值时间戳的界限判定：
**`!(0 <= _offset <= MoveElem.Elapse)` 才置位**。两道判定阈值实测**都等于 0**，不存在「多少像素每秒」的可调数字；且上报被 `m_tMobSpeedHackDetectTime` **节流到 300000 ms（5 分钟）一次**。

因此 type21 **不是**吸怪断连的元凶：吸怪走的是主动接管路径（`MakeMovePath/Flush`），根本不进 `CalcPassivePos`；就算进了，5 分钟一次也凑不出「第二波几秒断连」的量级。

---

## 1. 函数与调用链（RVA 相对 imagebase `0x7ffd60830000`）

```text
a480 安全 tick  sub_7FFD60CC4D90  (RVA 0x494D90)
  ├─ SendAttackPacketCountCheck  RVA 0x3C8FAB0  → 可能 type20（见攻包窗专文，本仓锚点）
  └─ SendMobSpeedHackDetectCheck RVA 0x3C90F00  → 若 flag 置位则发 type21

MovePath.CalcPassivePos  sub_7FFD619F1A20 (RVA 0x11C1A20)
  └─ SetDetectMobSpeedHack  sub_7FFD644C0DF0 (RVA 0x3C90DF0)   ← 唯一 code xref
        （置 m_bDetectMobSpeedHack = true）
```

| 方法 | RVA | 作用 |
|------|-----|------|
| `SendAttackPacketCountCheck(tCur)` | `0x3C8FAB0` | 攻包窗（type20）；本仓已知锚点，见 `security_attack_port.cpp` |
| `SendMobSpeedHackDetectCheck(tCur)` | `0x3C90F00` | 读 flag，节流后发 type21，回写 detectTime |
| `SetDetectMobSpeedHack()` | `0x3C90DF0` | 只把 `m_bDetectMobSpeedHack` 置 true（无速度比较） |
| `MovePath.CalcPassivePos(...)` | `0x11C1A20` | Hermite 样条重建被动怪位置；**速度判定在此** |

`SetDetectMobSpeedHack` 的 xref 只有 1 处 code ref（其余 2 处 data），即 `CalcPassivePos` 内 `0x11C275F` 处的 `call`。→ **type21 只有这一个触发源。**

### 1.1 SecurityClient 字段（与攻包窗专文一致）

| 字段 | static off | 语义 |
|------|-----------|------|
| `m_bDetectMobSpeedHack` | `+0x14` | 怪速举报标志（byte）|
| `m_tMobSpeedHackDetectTime` | `+0x18` | 上次上报时刻（游戏 `tCur`）|

`SendMobSpeedHackDetectCheck` 经 `[rcx+0xB8]` 取 static_fields，读 `byte[+0x14]`、算 `tCur-[+0x18]`、发包、回写 `[+0x18]=tCur`——字段偏移与 [`攻包计数窗与type20.md`](攻包计数窗与type20.md) §1 完全吻合，据此锁定函数身份。

---

## 2. 定点解码（CFF 去混淆 · 逐处实读种子验算）

> 依 `.cursor/rules/ga-const-obfuscation.mdc`：运行时 dump 的种子已初始化，用 `get_int(u32le/u8)` 实读，不得按「反正是 0」免验。

| 常量 | 位置 | 混淆式 | 种子实读值 | 解出 |
|------|------|--------|-----------|------|
| **节流窗** | `SendMobSpeedHackDetectCheck` | `0x593B817C ⊕ [0x6712B668]` | `0x593F129C` | `0x000493E0` = **300000 ms** |
| **举报类型** | 同上 | `0x9C0B4441 ⊕ [0x6712B654]` | `0x9C0B4454` | `0x15` = **21** |
| flag 复位 = false | 同上 | `([0x6712B650] + 0x39) & 0xFF` | `0xC7` | `0x100 → 0x00` |
| flag 置位 = true | `SetDetectMobSpeedHack` | `[0x6712B644] ⊕ 0x3A` | `0x3B` | `0x01` |
| **判定阈 A** | `CalcPassivePos @+0x26DE` | `0xE664E0A9 ⊕ [0x670D2854]` | `0xE664E0A9` | **0** |
| **判定阈 B** | `CalcPassivePos @+0x2699` | `0xA64EF68C ⊕ [0x670D286C]` | `0xA64EF68C` | **0** |

分支指针实读确认（排除 CFF cmov 读反方向）：

| 指针 | 值 | 去向 | 语义 |
|------|-----|------|------|
| `[0x676028A8]` | `0x7FFD619F2717` | → `SetDetectMobSpeedHack` | 判为 hack |
| `[0x676028B0]` | `0x7FFD619F26C5` | → 第二道门 | 继续判 |
| `[0x676028C0]` | `0x7FFD619F276A` | → 位置计算 | 判为正常 |
| `[0x676028C8]` | → `loc_2717` | → `SetDetect` | 判为 hack |

> 种子地址为本 dump 绝对 VA（imagebase `0x7ffd60830000`）；换 dump 需重读种子，**不得**照抄解出值。字节宽 flag 写入（`+0x39` / `⊕0x3A`）是 `ga-const` 规则里的「布尔字段字节变体」，已按写 bool 处理，不当算术。

---

## 3. 判定语义（去混淆后）

`CalcPassivePos` 做样条插值时 `t = _offset / Elem.Elapse`（可见系数常量 `4040AAAAAAAAAAAB` = 33.333…）。取两个整型字段：

- `eax = MovePath._offset`（本帧插值到的已过时长）
- `ecx = MoveElem.Elapse`（该段路径总时长，`MoveElem +0x20`）

两道门（阈值都=0）合并后等价于：

```text
门1 (阈A=0): 若 _offset < 0            → SetDetectMobSpeedHack()
门2 (阈B=0): 否则若 !(Elapse>0 && _offset<=Elapse) → SetDetectMobSpeedHack()

净效果：  !(0 <= _offset <= Elem.Elapse)  →  置 MobSpeedHack 标志
```

即：**被动怪插值时间戳越出 `[0, 本段时长]` 才判怪速 hack**（时间倒流，或外推超过本段应有距离）。这是一条**时间/插值一致性检查**，不是速度幅值阈值——两个混淆常量都实测为 0 正对应「下界 0」「与 0 比较」。

门2 的 `al` 由 4 个 set 布尔经奇偶 MBA 合成（`(P&R)+(Q|R)+(S&P)+1` 后 `test al,1`），真值表化简后：`al&1==1`（判正常）**当且仅当** `Elapse>0 && 0<=_offset<=Elapse`；其余一律 `al&1==0` → 走 SetDetect。

---

## 4. 对「吸怪 → 软重连」的结论

| 判据 | 结论 |
|------|------|
| 触发源 | 仅 `CalcPassivePos`（**被动怪**外推）。吸怪主动接管走 `MakeMovePath/Flush`，**不进此路** |
| 频率 | `detectTime` 节流窗 **300000 ms**，全局 5 分钟最多 1 次 |
| 阈值 | 无 px/s 天花板；纯插值时间界限（常量=0）|

→ **type21 基本排除为吸怪断连主因。** 「先解定点」的实际产出是排除法：把嫌疑收敛回逐帧、**无时间节流**的 `Mob.SendMobPrevPosHack` 那条链（见 §7）。

> **§4 修正（2026-08-21 二次解码）**：本节此前写「真正漏检的是 arm 之前的预武装窗口那几帧」——**已证伪**。实测预武装窗口 ≈ 1–2 帧、且那几帧怪跑原生 AI（未被拽），位移 < `MoveAbility+10`，`InspectUpdateActive` **不置旗** → 预武装漏出的 prevpos 举报 ≈ **0 条**。真正的漏检口是 **`Mob` 类发送链根本没被 `mob_fh_ban` hook**（`mob_fh_ban` 只 hook `VecCtrlMob`），压制全靠「`InspectUpdateActive` 不置 Float/AB 旗 → Mob 链读旗=false 跳过」这条**间接**路径——其对猛拉帧是否真兜住，是当前最大存疑点。详见 §7。

---

## 5. 证据边界

| 级别 | 内容 |
|------|------|
| 实锤 | 6 个定点实读种子解码 + 分支指针实读；`SetDetectMobSpeedHack` 唯一 code xref；字段偏移与攻包窗专文吻合 |
| 强推断 | `sub_7FFD619F1A20 = MovePath.CalcPassivePos`（Hermite 样条 + 双 MoveElem 插值 + 7 个 ref 出参 + 返回 bool）；`_offset/Elapse` 字段映射 |
| 弱推断 | 服端收到 type21 后的累加/处置细节（→ `MobHackLogDisconnectCount` 408 等，见 CMS dump）|
| 推不出 | 主动接管路径（`MakeMovePath/Flush`）是否另有独立怪速校验（本文未覆盖，属下一步）|

---

## 6. 复现要点

1. 锚点：`SendAttackPacketCountCheck` RVA `0x3C8FAB0`（`security_attack_port.cpp` 已固化）→ `xrefs_to` 得安全 tick `sub_7FFD60CC4D90` → `callees` 得同族 `SendMobSpeedHackDetectCheck`（`0x3C90F00`）。
2. `SendMobSpeedHackDetectCheck` 前一个函数即 `SetDetectMobSpeedHack`（`0x3C90DF0`，仅写 `byte[+0x14]`）→ `xrefs_to` 得 `CalcPassivePos`（`0x11C1A20`）。
3. `disasm` 定位 `call SetDetect`（`0x11C275F`）上溯两道门 → 逐处 `get_int` 实读种子验算（见 §2）。

---

## 7. `Mob.SendMobPrevPosHack` 链 · 位移阈值 / 无节流 / 预武装量化（2026-08-21）

> **目的**：接 §4 的排除法，锁定真正逐帧上报的 prevpos 链，解它的阈值/节流，并量化「预武装窗口漏检」。
> **同 build 佐证**：`InspectUpdateActive` RVA `0x11DBAB0` 与出货 `mob_fh_ban.cpp`（`kRvaInspect`）精确吻合；`SendMobPrevPosHack` RVA `0xF5CEC0` 与 `Dumps/runtime/out/dump.cs.restored` 吻合（该 restored dump imagebase `0x7ffd60880000`，RVA 与 loaded IDB 一致）。

### 7.1 调用链

```text
VecCtrlMob.InspectUpdateActive  RVA 0x11DBAB0   ← mob_fh_ban 短路（HookInspect：已武装直接 return）
   （自检哨兵；callees 里不含 SendMobPrevPosHack → 发送与自检解耦）

Mob.SendMobPrevPosHack  RVA 0xF5CEC0  （内联在 sub_7FFD61789D50 = RVA 0xF59D50，Mob 类大 CFF 函数）
   ← sub_7FFD6176E060 / sub_7FFD61797150  ← sub_7FFD6175C050 / sub_7FFD61771F10 / sub_7FFD617C86C0
   （全在 0x617xxxxx = Mob 类区间；Mob.WorkUpdateActive 0x11DF260 的 callees 里【不含】此链）
```

**关键结构**：`SendMobPrevPosHack` 是 **`Mob` 类**方法（`0xF5xxxx`），驱动它的更新链也全在 Mob 类；`mob_fh_ban` 只 hook **`VecCtrlMob`**（`0x11Dxxxx`）。→ **发送链本身没被 hook**，压制只能靠 §4 修正里说的间接旗路径。

### 7.2 位移阈值（去混淆 + 实读种子）

发送体算逐帧欧氏位移 `sqrt(dx²+dy²)`（`subps/mulps/shufps/addps/sqrtsd`），与由 `[mob+0x44]`（MoveAbility 类速度字段，记 `e`）导出的门限比较：

| 处 | 混淆式 | 实读种子 | 解出 |
|---|---|---|---|
| 门限 MBA | `2·(e & ~0xA) − (e ^ 0xA) + 0x14` | — | **`e + 10`** |
| 分支常量 | `0x4EE714F9 + [0x670C8210]` | `0xB118EB1B` | `0x1_00000014 → 0x14` = **20** |

净语义：**`displacement > MoveAbility + 10` → 走建包+发送**（`object_new` → 填 `dx/prevX/curX/dir` → `sub_7FFD616D40F0` 发包）。是**相对 MoveAbility 的位移门 + 10px 余量**，非固定像素常量。

### 7.3 节流：无

反汇编覆盖发送前后 ~340 条指令，**无** `tCur − _tLast ≥ TERM` 时间门；CMS 亦**无** `MOB_PREV_POS_HACK_TERM` 常量（对比 type21 的 `300000`）。`_tLastMobPrevPosHack`（Mob static `+0x0`）字段存在，但发送决策纯靠位移门。

| 举报 | 时间节流 | 判据 |
|---|---|---|
| type21 MobSpeedHack | **300000 ms**（§2） | 被动怪插值时间越界 |
| **prevpos SendMobPrevPosHack** | **无** | `displacement > MoveAbility+10`，**逐帧** |

**量级**：`mob_fh_ban` 命令上限 `kMaxCmd=4800 px/s`，30ms/帧 = **144 px/帧**；典型 `MoveAbility+10 ≈ 50–110` → 猛拉帧几乎必越界、逐帧上报。巡航档 `kBaseCruise=620 px/s ≈ 18.6 px/帧` 低于门限 → 站桩基本不触发。**prevpos 洪水来自猛拉相，不是站桩相。**

### 7.4 预武装窗口量化（`mob_gather` 节拍）

节拍：recruit（arm+控+拽）每 `RecruitIntervalMs=40ms`；worker sleep `aimMs=17ms`；控包 `ApplyCtrlWave` 节流 `1000ms`；arm 保活 `8000ms`；客户端帧 ≈30ms。

`HoldJobFn` 同一 pump job 内顺序 = `Arm()` → `ClearFh` → `ComputeSetVelocity`（拽）→ **任何怪的第一次拽必在已 arm 之后**。

| 情形 | 预武装漏检 |
|---|---|
| 已受控怪（ctrl>0） | arm 与拽同 job、拽前必 arm → **0 帧** |
| 新收怪（ctrl≤0） | 授控→下个 recruit arm，暴露窗 ≤ 40ms ≈ **1–2 帧**；但窗内跑原生 AI（未拽），位移 < MoveAbility+10 → 不置旗 → **≈ 0 条举报** |

→ **预武装窗口不是洪水源。**

### 7.5 真正的漏检向量（大位移 + InspectUpdateActive 未短路）

1. **Mob 链未被 hook**（§7.1）：间接旗压制对猛拉帧是否真兜住 = **最大存疑**（发送体自己重算位移，未见明显读 VecCtrlMob 旗）。
2. **超时/Disarm 时仍带残速**：8s 超时或换控 Disarm 后怪带大速度、`InspectUpdateActive` 恢复 → 1–2 帧大位移 → 置旗上报。
3. **服务器换控抖动**：被猛拉的怪服务器收控再放控，每次抖动给 1–2 帧未 hook 的大位移窗。

零星漏报按帧累加进服端怪 hacklog 计数（`MobHackLogDisconnectCount` LiveValue 408 一类）→ 跨吸攒到阈值、第二波跳闸，正合「第一次吸不断、第二次才断」。

### 7.6 证据边界（本节）

| 级别 | 内容 |
|---|---|
| 实锤 | `SendMobPrevPosHack` RVA 吻合 restored dump；位移门 `e+10` 与分支常量 `20` 实读种子解出；节拍常量取自 `mob_gather*.cpp` 源码 |
| 强推断 | 发送体 = prevpos 上报（`sqrt(dx²+dy²)` vs MoveAbility+10 + 建包发送）；Mob 链未在 `mob_fh_ban` hook 集内 |
| 待验 | **Mob 发送链是否读 VecCtrlMob 的 Float/AB 旗**（决定 arm 是否真压制猛拉帧）；发送体是否另有上游帧级门 |
