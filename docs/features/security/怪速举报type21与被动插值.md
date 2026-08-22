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

### 7.2 位移阈值（去混淆 + 实读种子，2026-08-21 二次反汇编复核）

发送体算逐帧欧氏位移 `sqrt(dx²+dy²)`（`0x6178CFC2 subps xmm6,xmm8` → `mulps/shufps/addps` → `0x6178D013 sqrtsd`），与由 `[obj+0x44]`（记 `e`，见下「字段身份修正」）导出的门限比较（`0x6178D091–0x6178D0BB`）：

| 处 | 混淆式（实测反汇编） | 实读种子 | 解出 |
|---|---|---|---|
| 门限 | `mov edi,[rdi+44h]` → `|edi|` → `2·(e & ~0xA) − (e ^ 0xA) + 0x14` | — | **`e + 10`** |
| 分支常量 | `0x4EE714F9 + [0x670C8210]` | `0xB118EB1B`（实读=2971200283） | `0x1_00000014 → 0x14` = **20** |

**分支方向（实读跳表指针）**：`ucomisd disp, (e+10)` → `cmovz r15,&off_65DEDB28` 仅在 `disp ≤ e+10` 命中；`[off_65DEDB28]=0x6178D0EA`（`object_new`+建包+发送的**正常移动包**路径）。即 **`disp ≤ e+10` 走正常移动包；`disp > e+10` 走另一分支（异常/prevpos 路径）**。无论哪支是「hack 上报」，物理扳机一致：**每帧位移超过 `e+10` 就离开正常路径**。

> **字段身份修正（重要）**：`e = [obj+0x44]` 是**整型**（`cvtsi2sd`/`mov edi,[rdi+44h]`），`obj` 是发送函数的入参（`[rsp+arg_48]`），带坐标字段 `+0x3C/+0x40`（`cvtsi2ss` 读为 x/y），是**一条 MovePath / 移动记录**对象，**不是** CMS 里的 `MoveAbility` 枚举（`Stop=0…FlyRandom=4`，且其 backing field 在 `0x100/0x34/0x20`，无 `0x44`）。前一版把 `[mob+0x44]` 认成「MoveAbility 速度字段」——**字段名错**，但 `e+10` 阈值公式经二次反汇编**确认无误**。`e` 的实际量级（怪速）需运行时实测（见 §7.9 夹速标定）。

> **方法学告警**：IDA MCP `insn_query` 的 `op_any` **不匹配内存操作数里的位移**（`mov edi,[rdi+44h]` 用 `op_any=0x44` 扫出 0 命中，实则存在）。凡「某偏移零命中」的结论必须改用 `include_disasm` 反汇编原文核对，不能只信 `op_any`。§7.5 的旗读取结论已按此重核。

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

1. **Mob 链未被 hook，且发送体不读旗**（§7.1）：反汇编原文扫发送体核心区间 `0x6178C900–0x6178D200`（523 条，含两处建包+发送簇与阈值判定），**无任何 `[reg+1D4h/1D5h/1D8h]` 读取**（此结论用 `include_disasm` 原文复核，非 `op_any`——见 §7.2 方法学告警）。→ 发送**不由 arm 能控的 Float/AB 旗门控**，纯 `disp vs e+10` 自门控。`mob_fh_ban` 只 hook `VecCtrlMob`（`0x11Dxxxx`），本链全是 `Mob` 类（`0x617xxxx`）。**arm 是否真能压住这条链 = 未证实**（见下「A/B 二假设」）。
2. **超时/Disarm 时仍带残速**：8s 超时或换控 Disarm 后怪带大速度、`InspectUpdateActive` 恢复 → 1–2 帧大位移 → 置旗上报。
3. **服务器换控抖动**：被猛拉的怪服务器收控再放控，每次抖动给 1–2 帧未 hook 的大位移窗。

零星漏报按帧累加进服端怪 hacklog 计数（`MobHackLogDisconnectCount` LiveValue 408 一类）→ 跨吸攒到阈值、第二波跳闸，正合「第一次吸不断、第二次才断」。

### 7.6 证据边界（本节）

| 级别 | 内容 |
|---|---|
| 实锤 | 位移门 `e+10`（`mov edi,[rdi+44h]`→`|e|`→`2·(e&~0xA)−(e^0xA)+0x14`）与分支常量 `0x14`（种子 `0xB118EB1B` 实读）+ 跳表指针 `[off_65DEDB28]=0x6178D0EA` 实读；节拍常量取自 `mob_gather*.cpp` 源码 |
| 实锤 | 发送体核心区间 `0x6178C900–0x6178D200`（523 条反汇编原文）**无** `[reg+1D4h/1D5h/1D8h]` → 发送非旗门控、纯位移自门控 |
| 强推断 | 发送体 = 移动/prevpos 上报（`sqrt(dx²+dy²)` vs `e+10` + `object_new` 建包发送）；Mob 链未在 `mob_fh_ban` hook 集内 |
| 待验 | `e=[obj+0x44]` 的运行时量级（怪速）与 `obj` 相对 `gBanMob` 的取法（→ §7.9 夹速用运行时日志标定）；**arm 到底靠什么压住这条 Mob 链**（A/B）；服端 408 阈值（LiveValue，客户端 dump 无值）|

### 7.7 A / B 二假设（决定修法）

「第一次吸不断、第二次才断」有两解，修法不同：

| | 假设 | 含义 | 修法 |
|---|---|---|---|
| **A** | arm 的 WUA reroute 连带停掉了驱动本链的 Mob 更新 | 压制有效，漏的是 §7.5 边缘帧 | 轻量：收紧 Disarm 时机 + 残速兜底 |
| **B** | Mob 发送链不受 arm 影响，逐吸都在发，服端计数跨吸攒到阈值才跳 | 压制无效 | 根治：**按位移门反推**——把每帧位移夹到 ≤ `MoveAbility+10`（发不出举报，与 hook/arm 无关），或直接 hook `SendMobPrevPosHack`/其驱动 |

**判 A/B 的最省力实测**：临时把 `mob_fh_ban` 的命令上限 / intent 夹到「每帧位移 < MoveAbility」（如 `kMaxCmd` 降到 ~1500 px/s），实机看第二波是否还断。停 → 位移门就是扳机、夹速即根治；仍断 → 扳机另在他处。

### 7.8 驱动链身份实证（2026-08-21，倾向 B）

沿 `xrefs_to` 逐级上溯发送链，锤定其根驱动：

```text
Mob.SendMobPrevPosHack 段  sub_7FFD61789D50 (RVA 0xF59D50)
  ← sub_7FFD6176E060 (RVA 0xF3E060) / sub_7FFD61797150 (RVA 0xF67150)
      ← 根 A：sub_7FFD6175C050 (RVA 0xF2C050，0x82f8 大函数，仅 data xref = MethodInfo/vtable 派发)
      ← 根 B：sub_7FFD617C86C0 (RVA 0xF986C0，仅 data xref = 同上)
```

| 判据 | 实证 |
|---|---|
| RVA 频段 | 全链 `0xF2xxxx–0xF6xxxx`（Mob 类段，同 `SendMobPrevPosHack 0xF5CEC0`）；**远离** VecCtrlMob 段 `0x11Cxxxx–0x11Exxxx`（InspectUpdateActive/WUA/CalcPassivePos 皆在此）|
| 根驱动派发 | `sub_7FFD6175C050`/`sub_7FFD617C86C0` **仅 data xref**（MethodInfo/vtable）= 顶层 il2cpp 方法入口，非内部 helper |
| 根驱动 callees | 调发送链 + 若干 VecCtrlMob 辅助（`0x11D9660`/`0x11DE070`/`0x11DE500`），但**不含** `mob_fh_ban` 实拦的 `0x11DBAB0`(Inspect)/`0x11DF260`(WUA)/`0x11DE6B0`(CtrlStop)… |
| `mob_fh_ban` hook 集 | 只 `VecCtrlMob`：Inspect / WUA→base / CtrlStop·Move·Jump·Fly / CD·CDF |

→ **arm 的重路不落在这条 Mob prevpos 链上。** `mob_fh_ban` 覆盖的是 **Float/AB/碰撞**自检（VecCtrlMob CD/CDF/Inspect），与**原始位移 prevpos** 是**不同举报族**。故倾向 **B**：prevpos 位移举报**不受 arm 压制**，「第一次不断」更可能是服端跨吸计数未攒满，而非被压住。

**未穷尽的反面可能**：根驱动可能自带门（如「仅对**非**客户端控怪跑 prevpos 自检」）——若真如此，我方接管的怪反而天然不触发。这条没证死，但**不影响修法结论**：夹速（每帧位移 ≤ MoveAbility+余量）对 A/B/控态门三种情形都安全，且无需在大 CFF Mob 方法上加 hook。

### 7.9 修法落地：逐帧位移夹速（2026-08-21 已实现）

落在 `mob_fh_ban::ApplyVtolOnVc`（`x/features/ports/mob_fh_ban.cpp`）：算完本拍目标速度
`(dvx,dvy)` 后，预测位移 `pred = |v|·dt` 超上限就等比缩速，被拽怪每帧位移永不越 `e+10` 门、
永远走正常移动包路径。与 hook/arm 无关，对 §7.7 的 A/B/控态门三种情形都安全。

| 项 | 值 |
|---|---|
| 面板入口 | 吸怪 快攻 TAB「防断」卡：`位移夹速` 勾选 + `位移上限`（px/帧） |
| payload | v141 `mobGatherDispClampOn` / `mobGatherDispCapPx`（INI 同名键，core 段） |
| 厂默 | 开、48 px/帧（≈1600 px/s @30ms 拍）；防爆钳 8–400 |
| DLL 侧 | `mob_fh_ban::SetDispClamp(on, capPx)`，payload apply 每次下发 |
| 标定日志 | `MobFhBan impact … disp=<真实逐帧位移> cap=<上限> clamp=<缩速比> dt=<ms>` |

**标定法**：`e` 的运行时量级（怪速）没静态值，只能实测——开吸怪看 impact 日志：
还断连 → 调小上限；两波都不断 → 逐步调大换吸得更快，直到再现断连为止取安全值。
`clamp<1.00` 说明该拍真被夹了；`disp` 长期远小于 `cap` 说明上限没约束到、可放大。

**首帧不再瞎（2026-08-21 补）**：`SeedArmBaseline` 在 `Arm()` 时以怪真实 AbsPos 播种逐帧
基准，`realizedDisp` 从被拽后第 1 拍即有效（不再恒 `disp=-1.0`）。同时**真实位移越过 cap 的
危险帧插队打**（`big=1`，20ms 地板防刷屏），不被 200ms 全局采样抽掉。故标定时直接看
`disp=`（服端量的那个逐帧位移）与 `big=1` 出现频率即可，不必再靠 `clamp` 反推。

> **实机取证注意（2026-08-21）**：一份 cap=19 的日志全程 `clamp=1.00`、0 个 `clamp<1.00`，
> 是**开关没开**（`位移夹速` 未勾）的基线，不是夹速失效——`cap=` 只反映存值，不代表在夹。
> 判「夹速到底有没有生效」看有没有 `clamp<1.00` 帧，别只看 `cap=`。

### 7.10 速度夹被实机证伪 → 改「牵引点 leash」（2026-08-21 第二版）

开夹速（cap=42）实测：`big=1` 危险帧的 `realizedDisp` 死死钉在 **117–123px**，且 **`clamp=0.29`
与 `clamp=0.62` 都给 ~120px**——把速度命令夹到 29% vs 62%，怪的实际逐帧位移**纹丝不动**。且大位移帧
后 ~95ms 就静默断连（`21:26:17.574 disp=123` → `21:26:17.669 Disconnected`）。

**结论：夹「速度命令」是错杠杆。** 被拽怪的实际位移由**目标点距离**驱动、与我们下的速度大小无关
（近目标 `disp=0.4`、远目标封顶 ~120px）。§7.9 那版夹的是 `ComputeSetVelocity` 的输出速度，游戏根本
不按它搬怪。

**改法：牵引点 leash（第二版）。** `ApplyVtolOnVc` 里在算速度前，先把目标点从真 aim 沿方向**夹到离怪
≤ cap px**（`adist>cap` 时 `lt = mob + dir·cap`），再喂给 `ComputeSetVelocityImpl`。怪当「近目标」处理、
每拍小步挪、逐帧位移 ≤ cap。速度侧夹保留为兜底（leash 后通常不触发）。日志加 `leash=<本拍目标离怪距离
=期望逐帧位移>`；标定看 `disp` 是否跟着 `leash`/`cap` 走、`big=1` 是否消失、断连是否停。开关/上限仍复用
`位移夹速`/`位移上限`（payload v141，无需升版本）。

### 7.11 prevpos 内联在 CFF、不可 MinHook；开关落盘竞态已修（2026-08-21 第三版）

**IDA 复核（更正一次误判）**：本轮一度按 `cur_base(0x7ffd60830000)+0xF5CEC0` 探到落在 `sub_7FFD61789D50`
中间，误判「构建位移、RVA 对不上」——**错**。`SendMobPrevPosHack` 的发送体就在当前 build 的 RVA `0xF5CEC0`，
但它**内联在 14KB 大 CFF 函数 `sub_7FFD61789D50`（RVA `0xF59D50`）里，没有独立函数入口**。这正是它进不了
`mob_fh_ban` hook 表的根本原因：**MinHook 需要函数入口，而它没有**（§7.8 只上溯到「Mob 链未被 hook」，本节补上
「就算想 hook 也 hook 不了——它压根不是独立函数」）。

当前 IDB 内联分支已逐条核对（`insn_query` 原文）：

```text
0xF5D091  mov eax,edi / neg / cmovns edi,eax   ; |e| = |[obj+0x44]|
0xF5D0A1  and 0x7FFFFFF5 / add eax,eax / xor edi,0xA / sub / add 0x14  ; 混淆算出门限 = |e|+10
0xF5D0B5  cvtsi2sd xmm1,eax                     ; 门限→double
0xF5D0BB  ucomisd xmm0,xmm1                     ; disp(sqrt 逐帧位移) vs 门限
0xF5D0C4  cmova ecx,eax(=0)                     ; disp>门限 → ecx=0；否则 ecx=0x14
0xF5D0DB  cmovz r15,&off_65DEDB28               ; ecx==0x14(=正常) 时取正常包跳表
0xF5D0E7  jmp [r15]
0xF5D0EA  正常移动包：object_new(sub_60BBF360)+建包+send
```

**要「直接堵」只剩字节补丁**：把 `0xF5D0C4 cmova ecx,eax` 改成无条件保 `ecx=0x14`（NOP 掉 `cmova`），
即令 `disp` 无论多大都走 `0xF5D0EA` 正常包路径。但这条 CFF 函数是**所有怪共用**，无条件改动影响面大、
且 §7.8/§7.9 的结论仍是优先「牵引点整形」而非在大 CFF 上动刀——**字节补丁列为 leash 若被证伪后的兜底选项**。

**面板开关落盘竞态（已修）**：`位移夹速` 连续两轮实机都没生效（`state\user.ini` 恒 `mobGatherDispClampOn=0`、
日志无 `leash=`/`impact`）。根因是 `MobGatherSaveUi` 写盘后记的是自己的 `c.writeTickMs`，**没回读**
`WritePayloadControl` 单调 +1 后的真实 tick（仓里注释记的「开 1 半秒被写回 0」同款）。已照抄 4848 行 `verify`
修法（写完 `ReadPayloadControl` 取真实 tick）。另在 `SetDispClamp` 加**值变化时**打 `MobFhBan dispclamp apply
on=? cap=?`，运行时真值直接可见、不必再靠 UI 猜。

> **测试前必核**：勾「位移夹速」后，`state\user.ini` 必须变成 `mobGatherDispClampOn=1`，且 `x.jsonl` 应出现
> `MobFhBan dispclamp apply on=1 cap=<上限>`。两者缺一即说明这轮 leash 没真正启用（前两轮就栽在这）。

### 7.12 字节补丁（§7.11 兜底）已实机证伪：服端**独立量位移**，客户端抑制举报无效（2026-08-22）

§7.11 把「无条件走正常包」的字节补丁列为 leash 兜底。本轮把它落地成默认关、可回滚的开关，实机跑了一轮——**证伪**。

**实现**（产品 = 经典版 / TWMS）：
- 补丁点 `RVA 0xF5D0DB`：`cmovz r15, rax`（`4C 0F 44 F8`）→ `mov r15, rax; nop`（`4C 8B F8 90`）。
  前一条 `lea rax, &正常包路径` 每次都执行，故无条件 `mov` 后 `r15` 恒 = 正常包 → **每帧每怪永走 `0xF5D0EA` 正常移动包、永不进 prevpos/异常分支**。
  （注：§7.11 曾提议改 `0xF5D0C4 cmova`；实际选了更靠后的最终决策点 `0xF5D0DB cmovz`，等价且 in-place 等长。）
- 模块 `x/features/ports/mob_prevpos_patch.{h,cpp}`：照抄 `curfh_gate_bypass` 模板（`GaBase+RVA`、`expect` 字节守卫、幂等、`SetEnabled(false)` 回滚），卸载路径 `xcat_probe.cpp` 加还原。
- 开关 `mobGatherAntiReport`（payload core INI `v146→147`，默认关），吸怪 TAB「寻簇」卡勾选。

**安装确认（实锤）**：`x.jsonl.2`
- `Bootstrap payload log ver=0.1.176 build=0x000000B0`（04:32:56，跑的是含补丁的新 build）。
- `MobPrevPosPatch patched ga+0xF5D0DB (cmovz r15->mov r15; forces normal move packet)`（04:33:19，expect 字节匹配、补丁生效）。

**结果（证伪）**：断连**一次没少**。`x.jsonl.1`/`x.jsonl` 里 `KickSniff STATE Connected(3)->Disconnected(1) pendingError=205` → `verdict=lean_local_or_soft`，04:33:54–04:35:42 约 100 秒内 **≈13 次**，每 6–12 秒一断（reenter→打几秒→又断），与未打补丁时**同频**。

**结论**：**服务器是自己量怪位移掐线的**——`disp ≤ e+10` 的「正常移动包」路径**照样把怪瞬移后的新坐标发给服务器**，服端据此独立判定，客户端那次 prevpos 自我举报**不参与**。故：
- **位移举报抑制全谱（A 充 `e` / B 夹 `disp` 侧的 report 抑制 / C 内联翻分支）对断连均无效**；能压住的只是「客户端主动 snitch」，压不住「服端自量」。
- 与实机现象自洽：其他玩家能看到怪被吸过去 = 服端收到并应用了位移 = 服端有数据独立判。

**附带实锤（本轮为策略 B）**：`state\user.ini` `mobGatherStrategy=1`（FH-SNAP 绑台）、`r=950`、`scale=10.00`。日志 `MobFhBan fh-snap ... d=823/800/721` —— 绑台是**一帧把怪 Ap/Apl 直接写到落点**的瞬移（~800px/次），**根本不过 leash**（leash 只在策略 A 的速度拉取里生效）。故策略 B 每只怪一次 ~800px 瞬移，服端必判、必踢，与半径/anti-report 都无关。「≤1000 半径 0 断连」那次是**策略 A + 速度拉取**，不是瞬移。

**修法收敛（终态）**：根治只能「**不产生大位移**」，让服端量不到——
1. 策略 A（速度拉取）+ leash 把逐帧位移夹到 ≤ `e+10`（~50–60px/帧）+ 小半径；
2. 或人过去就地吸（远怪自动巡点，飞行限速 1.0X）。
   策略 B 的 fh-snap 瞬移与 anti-report/leash 无关，天生触发位移检测，不用于抗断。

**收尾 IDA 复核（2026-08-22，VA 算术更正）**：一度把 RVA `0xF5D0DB` 错算成 VA `0x7ffd6172D0DB`（少进一位）导致反汇编对不上、疑似换版；`find_bytes 4C 0F 44 F8` 命中 `0x7ffd6178D0DB`，即 **RVA 0xF5D0DB = VA `0x7ffd6178D0DB`（base 0x7ffd60830000）**，`cmovz r15,rax` 与活进程 build 0.1.176 一致，**IDB 未换版**。据此把发送链上半段（disp 计算）逐条核实：

```text
0xF5CFB9  movss xmm6,[rsp+arg_64]        ; 载入新位置分量
0xF5CFBF  unpcklps xmm6,xmm0             ; xmm6 = {new.x,new.y}
0xF5CFC2  subps  xmm6,xmm8               ; xmm6 = {dx,dy} = 新位置 − 上帧位置
0xF5CFF9  mulps  xmm6,xmm6 / addps       ; dx²+dy²
0xF5D013  sqrtsd xmm1,xmm0               ; disp = √(dx²+dy²) 逐帧位移
0xF5D036  mov edi,[rdi+44h]              ; e = [obj+0x44]；门限=|e|+10
0xF5D0DB  cmovz r15,&正常包跳表           ; disp≤门限→正常包；否则→prevpos 举报
0xF5D0EA  正常包：object_new(671E1D28类)+ctor(617B7B60)+填[r13+10/14/28/30]+send(60850350)
```

两条铁结论：
1. **`disp = |新位置 − 上帧位置|`**，而「新位置」正是正常移动包 send 出去的东西——**移动怪 = 把新位置告诉服端**，二者不可分。故不存在「既把怪吸过来又瞒住服端位移」的客户端整形点。
2. **充大 `[obj+0x44]`（旧 B' 提案）= 令 `disp≤|e|+10` 恒真 = 与本轮 anti-report patch（强走正常包）完全等效**。patch 已实机证伪，故 B' 同死，无需再实现。

**安全禁用（2026-08-22，不拆模块）**：`mob_prevpos_patch` 文件保留；`SetEnabled` 以 `kKillSwitched=true` 硬关（忽略 on、只卸载）；`ApplyPayloadControl` 永不装；INI 读/写/normalize 与面板勾选一律强制 0（勾子灰掉）。卸载侧若发现 GA 仍是补丁字节（`4C 8B F8 90`），即使本进程没装过也会写回 `cmovz`（`4C 0F 44 F8`），清残留脏页。吸怪 / leash / 巡点 / 策略 A·B 原逻辑不动。回滚只需把 `kKillSwitched` 改回 `false` 并恢复 apply/INI。
