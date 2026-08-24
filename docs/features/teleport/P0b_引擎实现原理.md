# teleport P0b · 引擎实现原理与位移真源（2026-08-02）

> **历史笔记**：ImpactNext / Attr=4 旁路已于产品路径拆除；现役仅 fill+Doing。下文仍有考古价值。
> **状态**：✅ 只读采证完成（dump + IDA 双向）；偏移核对全数一致；**位移真源已改写，见 §4**
> **产品**：新枫之谷：经典版（TW · `Maplestory_Classic.exe`）· **不是**枫星
> **数据源**：`DumpRestoredData/dump.cs.restored.C`（折中档）· 原始真源 `Dumps/runtime/out/dump.cs` · IDB `Dumps/runtime/GameAssembly.dll.i64`
> **RVA 基址**：`0x7FFB16B40000`（同 [`P0a`](P0a_瞬移CALL锚点.md)）· VA = base + RVA  
> **2026-08-22**：下文 `ApplyImpact 0x11A4E60` / `LeaveFoothold 0x11AF5C0` 是**旧 dump**。当前 runtime IDB 为 `ApplyImpact 0x11C85D0`、`LeaveFoothold 0x11D2F00`、`SetImpactNext 0x11C5150`（imagebase 以打开的 `GameAssembly.dll.i64` 为准）。语义仍对（清 Valid → 写 Ap.V、两轴同一条 Min/Max 路 + 整数截断）；RVA 以 [`../simple_combat/满火力进站与竖直权限.md`](../simple_combat/满火力进站与竖直权限.md) 为准，勿把旧表抄进新代码。
> **2026-08-24 本 IDB 复核 `SetImpactNext 0x11C5150`：** 写 `WingsNow` 的种子是 `byte_670D2938=0xAF xor 0xAF` → **恒为 0**（停翼，不是进翼）。正文「每次调用都写 Now」仍对，**值不是 1**。旧 dump `add al,5Dh` 不要用。Valid 置位 `0x53 xor 0x52=1`。ApplyImpact 清 Valid：`0x4E+0xB2` 溢出 → **0**。对照 [`../protocol/运动系统.md`](../protocol/运动系统.md) §12.5。
> **实现**：`x/features/ports/teleport_port.cpp`（现役已不走本文 ImpactNext 旁路）
> **安全**：全程只读 dump / IDB / 源码，未改 `.text`、未发包、未运行客户端

本文回答一个问题：**角色到底是被什么挪过去的**。

[`P0a`](P0a_瞬移CALL锚点.md) 给的是 IDA 侧的入口 RVA 与调用链，本文给的是**字段布局 + 函数体语义 + 位移成因**。

> ⚠️ **本文 2026-08-02 第二轮做过重大修订。** 首版把 `SetImpactNext` 当成「写一次冲击速度、引擎自己推过去」，
> 第二轮进函数体后发现**不成立**：它是个会改写入参的合并函数，而位移主体也不是它提供的。
> 详见 §2 与 §4。首版的偏移核对（§5）不受影响，仍然有效。

---

## 0. 一句话

瞬移是**两条独立的线**，混为一谈是所有弯路的根源：

| 线 | 做什么 | 靠什么 |
|---|---|---|
| **发包线** | 让服务器接受这次超距位移 | `MovePath._x/_y` 落点 + `MoveElem.Attr = 4` + `_forcedFlush` |
| **位移线** | 让本地角色真的到落点 | **偷换 `CurFootHold` 到目标踏板**，再用 `SetImpactNext` 触发引擎做一次 `RelPos → AbsPos` 重算 |

`SetImpactNext` 在位移线里扮演的是**扳机**，不是引擎。官方全程**没有** `Transform.set_position`。

> **扳机可以换人扣**：产品贴怪主路径把 Attr + 扳机 + 特效 + 收态整包交给官方 `TryDoingTeleport`，
> 但它仍然先手工挂好目标踏板，所以位移机制不变。见 §4.5 补充与 [`模块设计.md`](模块设计.md) §0.1。

---

## 1. dump 实锤的三层结构

### 1.1 状态层 · `UserLocal.Teleport`

挂在 `UserLocal+0x3C8`（C 档 L70175），TypeDefIndex 1566：

```csharp
protected struct UserLocal.Teleport
{
    public bool    IsValid;      // 0x0
    public bool    ByPortal;     // 0x1
    public Vector2 Pos;          // 0x4   ← 登记的落点
    public int     StartTick;    // 0xC
    public int     CoolTimeEnd;  // 0x10  ← 冷却在客户端就有
}
```

这解释了原生为何是**两阶段**：Register 只填这个结构（落点 + 起始 tick + 冷却），
真正的位移留给下一帧的 Doing 消费。**冷却是客户端自带语义**，不是服务器单方面风控。

### 1.2 协议层 · `MovePathType` / `MoveElem` / `MovePath`

`MovePathType` 共 23 个值（TypeDefIndex 1587，C 档 L71565 处为 `Teleport = 4`）：

| 值 | 名 | 值 | 名 | 值 | 名 |
|---:|---|---:|---|---:|---|
| 0 | Normal | 8 | Assassination | 16 | StartWings |
| 1 | Jump | 9 | Rush | 17 | Wings |
| 2 | Impact | 10 | StatChange | 18 | VerticalJump |
| 3 | Immediate | 11 | SitDown | 19 | CustomImpact |
| **4** | **Teleport** | 12 | MobPowerKnockBack | 20 | CombatStep |
| 5 | HangOnBack | 13 | BackstepShot | 21 | AranAdjust |
| 6 | FlashJump | 14 | StartFalldown | 22 | MobToss |
| 7 | Assaulter | 15 | FallDown | | |

落到线上的单元是 `MoveElem`（TypeDefIndex 1588，C 档 L71587）：

```csharp
public class MoveElem : ICloneable
{
    public byte  Attr;         // 0x10  ← 这里写 4 = Teleport
    public short X, Y;         // 0x12 / 0x14
    public short Vx, Vy;       // 0x16 / 0x18
    public byte  MoveAction;   // 0x1A
    public short Fh;           // 0x1C
    public short FhFallStart;  // 0x1E
    public short Elapse;       // 0x20
    public byte  Stat;         // 0x22
}
```

`MovePath` 是聚包器（TypeDefIndex 1591，C 档 L71645）：

| 字段 | 偏移 | 说明 |
|---|---|---|
| `_x` / `_y` | `0x10` / `0x12` | short 落点坐标 |
| `_vx` / `_vy` | `0x14` / `0x18` | int |
| `_interval` / `_offset` / `_received` | `0x1C` / `0x20` / `0x24` | 攒包节奏 |
| `_fhLast` | `0x28` | short |
| `_gatherDuration` | `0x2C` | 与静态 `GatherTimeShort/Long` 配合 |
| `Elem` | `0x30` | `List<MoveElem>` |
| `_elemLast` | `0x38` | |
| `_keyPadState` | `0x40` | `List<byte>` |
| `_forcedFlush` | `0x48` | **bool — 强制立刻发包** |
| `_move` | `0x4C` | `MovePathRect` |
| `_shortUpdate` | `0x5C` | |

方法面（RVA）：

| 方法 | RVA | 用途 |
|---|---|---|
| `SetForcedFlush()` | `0x11986F0` | 置 `_forcedFlush` 的**官方原语**（已落地，见 §6） |
| `IsTimeForFlush(bool)` | `0x1198700` | 判断是否已到攒包窗口 |
| `Flush(OutPacket, bool, MovePath)` | `0x1198DC0` | 真正发包 |
| `Encode(OutPacket)` | `0x119AB50` | wire 编码 |
| `AddNewElem(...)` | `0x119DB20` | 追加 MoveElem |
| `MakeMovePath(...)` | `0x119C2F0` | 构造路径 |
| `DiscardByInterrupt(int, VecCtrl, bool)` | `0x119F600` | **中断丢弃路径**（见 §6③） |
| `CalcPassivePos(...)` | `0x119E460` | 他人插值（Hermite 样条） |
| `SetKeyPadState(...)` | `0x119F350` | 键位状态入包 |

「服务器认坐标」的机制到此清楚：`_forcedFlush = true` 让本来按 `GatherTime` 攒包的 MovePath 立即 `Encode` + `Flush`，
`MoveElem.Attr = 4` 告诉服务器「这是一次合法的 Teleport 位移」，服务器据此更新权威坐标。

### 1.3 物理层 · `VecCtrl`

TypeDefIndex 1596（C 档 L71776）：

| 字段 | 偏移 | 类型 / 说明 |
|---|---|---|
| `<Owner>` | `0x10` | `FieldActorBase` |
| **`impactNext`** | **`0x18`** | `VecCtrl.ImpactNext` 类：`Valid@0x10` / `Vx@0x14` / `Vy@0x18`（float） |
| `_curAttrShoe` | `0x20` | |
| `<CurFootHold>` | `0x28` | `StaticFoothold` — **§4 的主角** |
| `<LastFootHold>` | `0x30` | |
| `<FallStart>` | `0x38` | |
| `<LadderOrRope>` | `0x40` | |
| `<AttrField>` | `0x48` | |
| `InputX` / `InputY` | `0x50` / `0x54` | |
| `JumpNext` | `0x58` | |
| `TryJumpedInFly` | `0x59` | |
| `FalldownNext` | `0x60` | `FallDownData{ Valid@0x0, FhFallStart@0x8 }` |
| `WingsNext/Now/Prev` | `0x70`/`0x71`/`0x72` | `WorkUpdateActive` 每帧滚动 `Prev = Now`；`SetImpactNext` 会写 `Now` |
| `WingsParam` | `0x74` | |
| `<MovePath>` | `0x78` | 上面那个聚包器 |
| `<Active>` | `0x80` | |
| `MoveAction` | `0x84` | |
| `Rp` | `0x88` | `RelPos{ Pos@0x0, V@0x8 }` — 有台时沿踏板的**标量**位置 |
| `Ap` | `0x98` | `AbsPos{ X@0x0, Y@0x8, Vx@0x10, Vy@0x18 }` — 站位/碰撞/发包权威 |
| `Apl` | `0xB8` | 上一帧 AbsPos —— ⚠️ **同时是视觉插值的起点**，由 `BeginUpdateActive` 每逻辑帧滚动。写坏它 = 皮滑行（[`P0c`](P0c_视觉层同步链.md) §4） |
| `MapBound` | `0xD8` | |
| `BeginUpdateActivePassed` | `0xE8` | UpdateActive 链门闩 |
| `<Page>` / `<ZMass>` | `0xEC` / `0xF0` | |

> **核对定式**：il2cpp **类**字段自 `0x10` 起（对象头 16 字节），**结构体**字段自 `0x0` 起。
> 所以 `Ap@0x98` + `AbsPos.Y@0x8` = `0xA0`，而 `impactNext@0x18` 是**指针**，要再解引用一次才到 `Valid@0x10`。

---

## 2. `SetImpactNext` 的真实语义 —— 它不是 setter

`VecCtrl.SetImpactNext(double vx, double vy)` @ RVA `0x11A1A10`（VA `0x7FFB17CE1A10`）被 CFA 控制流平坦化，
Hex-Rays 只能看到跳转表 prologue。逐块还原后的真实语义：

```c
void VecCtrl::SetImpactNext(double vx, double vy)
{
    this->WingsNow = <MBA 混淆常量>;               // VecCtrl+0x71，每次调用都写
    ImpactNext* im = this->impactNext;             // VecCtrl+0x18

    if (!im->Valid) { im->Vx = 0; im->Vy = 0; }    // 只有「无待消费冲击」时才清零
    im->Valid = true;

    im->Vx = (vx < 0) ? fmax(vx, (double)im->Vx + vx)
                      : fmin(vx, (double)im->Vx + vx);
    im->Vy = (vy < 0) ? fmax(vy, (double)im->Vy + vy)
                      : fmin(vy, (double)im->Vy + vy);
}
```

### 2.1 指令级证据（VA）

| 观察 | 指令 |
|---|---|
| 写 `WingsNow` | `0x7FFB17CE1FAA: movzx eax, byte_7FFB1D389354 / add al,5Dh / mov [rsi+71h], al` |
| 读 `impactNext` | `0x7FFB17CE1FBC: mov rax, [rsi+18h]` |
| 读 `Valid` | `0x7FFB17CE1FE1: movzx ecx, byte ptr [rax+10h]` |
| `!Valid` 时清零 Vx+Vy（一条 qword 盖掉两个 float） | `0x7FFB17CE2002: mov qword ptr [rax+14h], 0` |
| 置 `Valid` | `0x7FFB17CE202F: mov [rax+10h], cl` |
| 按 vx 符号分流 | `0x7FFB17CE203C: xorpd xmm0,xmm0 / ucomisd xmm0,xmm7 / ja` |
| 取旧 Vx 并升 double | `0x7FFB17CE2078: movss xmm0,[rbx+14h] / cvtss2sd xmm8,xmm0` |
| 累加 | `0x7FFB17CE20E7: addsd xmm8, xmm7` |
| vx<0 走 `fmax` | `0x7FFB17CE20F2: call sub_7FFB1A97E930` |
| vx≥0 走 `fmin` | `0x7FFB17CE21CD: call sub_7FFB1A97EA70` |
| 回写 Vx | `0x7FFB17CE21E4: cvtsd2ss xmm0,xmm0 / movss [rbx], xmm0` |
| Vy 完全对称（`rbx+18h`） | `0x7FFB17CE21F8` 起 |

两个 helper 已反编译确认：`sub_7FFB1A97E930` = `fmax`、`sub_7FFB1A97EA70` = `fmin`（均带 NaN 短路）。

### 2.2 合并真值表

这套 min/max 组合的效果是**朝新方向饱和**：

| 旧 `Vx`（未消费） | 新 `vx` | 结果 | 含义 |
|---:|---:|---:|---|
| 0 | +100 | `min(100, 100)` = **100** | 正常写入 |
| +300 | +100 | `min(100, 400)` = **100** | **旧的 300 被吞掉**，不是叠加 |
| +100 | +300 | `min(300, 400)` = **300** | 取新值 |
| **−80**（击退） | +100 | `min(100, 20)` = **20** | **被代数抵消** |
| +80 | −100 | `max(−100, −20)` = **−20** | 反向同理 |

引擎意图很清楚：**同向冲击不叠加放大（防火箭），反向冲击代数抵消（击退能拉回突进）**。

### 2.3 对本仓的影响

- **首版文档写的「不预写 AbsPos，否则 impact 再叠加 dx 变双倍位移」，理由是错的** ——
  impact **之间**不会翻倍，min/max 卡死了。不预写 Ap 这个做法本身仍然对（见 §5），但别用「双倍」当理由。
- 真正的风险是反过来的：**位移被吞掉或被抵消**。挨打后立刻瞬移，`SetDamaged` 写入的反向分量会削掉本次冲击。
- hop 串若快于一帧，第 N 跳未被消费就被第 N+1 跳整个替换 → **丢跳而不报错**。
  用 `GetImpactValidity` 门控挡的正是这个（已落地，见 §6①）。

---

## 3. 消费链（全部由反汇编实证）

```text
VecCtrlUser.WorkUpdateActive                  RVA 0x11CFA90 / VA 0x7FFB17D0FA90  (Slot 11)
  └─ VecCtrl.WorkUpdateActive                 RVA 0x11A4400 / VA 0x7FFB17CE4400
       ├─ 0x4838  WingsPrev(+0x72) = WingsNow(+0x71)
       ├─ 0x483F  rax = this->impactNext (+0x18)
       ├─ 0x485E  cl  = impactNext.Valid (+0x10)
       ├─ 0x487C  if (Valid) call ApplyImpact            RVA 0x11A4E60   ★
       │            ├─ 0x52FE  impactNext.Valid = 0      ← Valid 在此翻位
       │            ├─ 0x5320  xmm9 = impact.Vx (+0x14)
       │            ├─ 0x5332  xmm8 = impact.Vy (+0x18)
       │            ├─ 0x533A  if (CurFootHold != null) call LeaveFoothold  RVA 0x11AF5C0  ★★
       │            └─ 写入 [this+0xA8] / [this+0xB0] = Ap.Vx / Ap.Vy
       ├─ 0x4890  JumpNext (+0x58) 分支
       └─ 尾段积分
            dt = tElapse / 1000.0                        ← 常量实读 0x408F400000000000 = 1000.0
            有台：RelPos.Pos += RelPos.V * dt  →  virtual CollisionDetect       [vtable+0x208]
            无台：Ap.{X,Y}   += Ap.{Vx,Vy} * dt →  virtual CollisionDetectFloat  [vtable+0x218]
```

### 3.1 本轮新钉死的 RVA

| 函数 | RVA | dump 中的名字 | 作用 |
|---|---|---|---|
| `ApplyImpact` | `0x11A4E60` | `VecCtrl.ec29af0f…()`（protected 无参） | 消费 impactNext：清 Valid → 读 Vx/Vy → 写 Ap.V |
| `LeaveFoothold` | `0x11AF5C0` | `VecCtrl.e334e53d…()`（private 无参） | 离开踏板：`RelPos` + `CurFootHold` → `Ap`，转空中态 |
| `AbsPos.SetFromRelPos` | `0x11B5E00` | 名字已恢复 | `LeaveFoothold` 的 callee，真正做重算的那一步 |
| `JustJump()` | `0x11AD920` | 名字已恢复 | `LeaveFoothold` 的另一个调用方 —— 跳跃与冲击共用同一套离台逻辑 |

### 3.2 两个直接可用的结论

**① `Valid` 的翻位点在 `ApplyImpact` 的入口**，早于任何位置计算。
所以 settle 的 `sawValid` 必须**尽早采样**——`ImpactBlinkJobFn` 在 arm 之后立刻 `PeekImpactValid` 那一手是必要的，不是保险。

**② impact 写的是速度场，不是位置。**
`insn_query` 扫遍整个 `0x11A4E60`，所有浮点写入只落在 `[rsi+0xA8]`/`[rsi+0xB0]`（`Ap.Vx`/`Ap.Vy`），
**没有一条写 `Ap.X`(0x98) 或 `Ap.Y`(0xA0)**。配合 `dt = tElapse/1000.0`，参数单位是 **px/秒**。
这与 `VecCtrl.NockBack(int, int = 100, int = 300)` 的默认值也吻合：速度 100、持续 300ms ≈ 30px 的击退手感。

---

## 4. 位移真源：踏板偷换，不是冲击速度

### 4.1 那个对不上的 20 倍

如果参数是 px/s，`SetImpactNext(185, 0)` 应让角色以 185px/s 滑行——50ms 只走 9px。但 BIN 实测：

```text
06:36:06.084  ImpactNext dx=185.0 dy=0.0 dist=185.0 fh=9
06:36:06.084  ImpactSettle arm to=(305,-275) fh=9
06:36:06.134  ImpactSettle plant ap=(308,-275) want=(305,-275)
```

50ms 内 `Ap` 实际走完约 185px，等效 3700px/s，是传入值的 **20 倍**。
且这条是 `plant` 而非 `force_ap`——`ImpactSettleJobFn` 只在 `job->forceAp` 时才 `WritePhysicsPos` 硬写 Ap，
所以这个 `ap=(308,-275)` 是引擎自己到位的真实值。

### 4.2 成因

`ImpactBlinkJobFn` / `TeleportJobFn` 都做了同一件事：`ClearFoothold(vc)` 之后**立刻把 `CurFootHold` 挂成目标踏板**，
然后才调 `SetImpactNext`：

```text
ClearFoothold(vc)                                  // CurFh / LastFh / FallStart 清空
WriteF64(vc, Apl.X/Y, 旧 Ap)                       // ⚠️ 注释是错的，见下方警告
WriteI16(mp, _x/_y, 落点)                          // 发包线：给服端认位
WritePtr(vc, CurFh/LastFh, plantFh)                // ★ 偷换到目标踏板
gSetAttr(vc, 4)                                    // 发包线：Attr = Teleport
CallForcedFlush(vc, mp)                            // 发包线：立即发出
CallSetImpactNext(vc, dx, dy)                      // ★ 扳机：置 Valid
ArmImpactBlinkSettle(...)                          // 等 Valid 翻位后收尾
```

> ⚠️ **`Apl` 那一行的注释「留一帧回溯点」是错的**（源码注释同错）。
> `Apl` 不是回溯点，是**视觉插值的起点**：`GetPos() = round(lerp(Ap, Apl, alpha))`。
> 把它写成旧坐标，等于明确告诉渲染器「上一帧你在老地方」——皮**必然**从老地方滑过来。
> 这正是「皮魂分家」的直接成因之一。见 [`P0c`](P0c_视觉层同步链.md) §2 / §7.1。

于是下一帧 `ApplyImpact` 跑起来时看到 `CurFootHold != null`，就调 `LeaveFoothold` 去「离开踏板」——
**而它要离开的是我们刚换上去的目标踏板**。`AbsPos.SetFromRelPos(RelPos, 目标踏板)` 一执行，
`Ap` 就被重算到目标踏板上，一帧到位，**与 impact 的大小无关**。

> **真实因果**：`SetImpactNext(dx, dy)` 的作用不是提供位移，而是把 `Valid` 置起来，
> 触发引擎跑一次 `LeaveFoothold` 重算。真正的瞬移由「偷换 `CurFootHold` + 引擎按新踏板重算 Ap」完成。
> `dx/dy` 只需要非零。

### 4.3 这条解释把此前所有对不上的观测串起来了

| 观测 | 解释 |
|---|---|
| 50ms 内走完 185px，等效 20 倍 | 不是积分出来的，是一帧重算出来的 |
| 短跳精确到 1–3px | 源、目标在同一或相邻踏板，`RelPos` 标量差异小 |
| 长跳残留 30–40px（`kBlinkSettleNearPx = 48f`、BIN `dist = 36.9`） | `RelPos` 沿踏板的标量是从**源踏板**带过来的，投到目标踏板上必然错位，错位量即残留 |
| `Ap.Vx/Vy` 确实被写了速度 | 写了，但它只负责重算之后那一小段惯性滑行，不是主位移 |
| `force_ap` 后皮魂分家 | 残留超过 `kBlinkSettleNearPx` → 超时 → 硬写 Ap。**精确机制**：`WritePhysicsPos` 只写 `Ap`、不写 `Apl`，于是 `GetPos()` 仍从旧 `Apl` 插值滑行（[`P0c`](P0c_视觉层同步链.md) §7.1） |

### 4.4 三个能证伪它的实验（均未执行）

| # | 做法 | 若假设成立 | 若假设不成立 |
|---|---|---|---|
| **一** | 把 `CallSetImpactNext(vc, dx, dy)` 改成 `CallSetImpactNext(vc, dx > 0 ? 1.0 : -1.0, 0.0)` | **仍然精确落到目标点** | 只挪 1px 或原地不动 |
| **二** | 保留 `ClearFoothold`，但**不**回挂 `plantFh`（`plantFhId = 0`） | 不瞬移，改为以 dx px/s 缓慢滑行 | 仍然瞬移 |
| **三** | 换 `CurFootHold` 的同时，按目标点在新踏板上的投影预写 `RelPos.Pos` | 30–40px 残留归零，`force_ap` 不再触发 | 残留不变 |

实验一成本最低、能一锤定音；实验三是实验一成立后的**正解方向**——
不该继续在 dx 精度上打磨，该去解那道投影题。

### 4.5 仍未解答

**纯官方路径（技能触发 → `IsTeleportSkillAvailable` 算落点 → Register → Doing）并不偷换踏板**，
它的位移量级如何达成，本轮未验证。可能是走了另一条通路，也可能官方瞬移本来就带一小段滑行。
在解开之前，**不要**把本仓旁路的行为反推成「官方就是这么做的」。

> 📌 **2026-08-02 补充（重要）**：本仓的**产品贴怪主路径** `TeleportNativeSkillCall`
> 虽然扳机交给了官方 `TryDoingTeleport`，但它在调 Doing **之前**已经
> `WritePtr(vc, CurFh/LastFh, plantFh)` 把踏板挂到了目标（`ApplyFillDoing`）。
> 所以**§4 的踏板偷换机制对产品主路径同样成立**，只是扳机换了人扣。
> 上面这条「未解答」仅针对**没有人为挂台**的纯官方技能瞬移。
> 三条路径的分工见 [`模块设计.md`](模块设计.md) §0.1。

---

## 5. 本仓实现的映射与偏移核对

### 5.1 刻意不做的两件事

1. **不预写 AbsPos** —— 理由已在 §2.3 修正：不是怕「双倍位移」，而是预写会让 `LeaveFoothold` 的重算基准失真。
2. **不写 Pos / CurPos / Transform** —— 失败即失败，不回退硬钉牵绳（见 §7 时间线）。

### 5.2 偏移核对表（源码常量 ↔ dump 字段）

| 源码常量 | 值 | dump 字段 | 核对 |
|---|---|---|:--:|
| `kOffVcCurFh` | `0x28` | `VecCtrl.<CurFootHold>` | ✅ |
| `kOffVcLastFh` | `0x30` | `VecCtrl.<LastFootHold>` | ✅ |
| `kOffVcFallStart` | `0x38` | `VecCtrl.<FallStart>` | ✅ |
| `kOffVcJumpNext` | `0x58` | `VecCtrl.JumpNext` | ✅ |
| `kOffVcFallDownValid` / `FallDownFh` | `0x60` / `0x68` | `FalldownNext@0x60` + `FallDownData{Valid@0x0, FhFallStart@0x8}` | ✅ |
| `kOffVcMovePath` | `0x78` | `VecCtrl.<MovePath>` | ✅ |
| `kOffVcActive` | `0x80` | `VecCtrl.<Active>` | ✅ |
| `kOffVcMoveAction` | `0x84` | `VecCtrl.MoveAction` | ✅ |
| `kOffVcRp` | `0x88` | `VecCtrl.Rp`（`RelPos`） | ✅ |
| `kOffVcApX/Y/Vx/Vy` | `0x98`/`0xA0`/`0xA8`/`0xB0` | `Ap@0x98` + `AbsPos{X,Y,Vx,Vy}` | ✅ |
| `kOffVcAplX/Y` | `0xB8` / `0xC0` | `Apl@0xB8` + `AbsPos{X,Y}` | ✅ |
| `kOffVcBeginUpdateActivePassed` | `0xE8` | `VecCtrl.BeginUpdateActivePassed` | ✅ |
| `kOffMpX` / `kOffMpY` | `0x10` / `0x12` | `MovePath._x` / `._y` | ✅ |
| `kOffMpForcedFlush` | `0x48` | `MovePath._forcedFlush` | ✅ |
| `kAttrTeleport` | `4` | `MovePathType.Teleport` | ✅ |

**14 组偏移 + Attr 枚举，零偏差。**

> 源码行号变动频繁，本文一律以**函数名**定位（`TeleportJobFn` / `ImpactBlinkJobFn` / `ImpactSettleJobFn` /
> `TickImpactBlinkSettle` / `CallSetImpactNext` / `CallForcedFlush` / `PeekImpactValid`），不写行号。

---

## 6. 落地状态与待办

### ① 用 `GetImpactValidity()` 替掉 settle 的距离猜测 —— ✅ 已落地

`kRvaGetImpactValidity = 0x11A2400` 已绑定，`PeekImpactValid()` 优先调用它、失败则裸读
`VecCtrl+0x18 → +0x10`。`ImpactBlink` 走独立的 `TickImpactBlinkSettle`，以 `sawValid && !impactValid`
作为「已消费」判据，`kBlinkSettleNearPx(48) / kBlinkSettleMaxMs(400)` 退化为兜底。

§3.2① 补充了一条必要条件：`Valid` 在 `ApplyImpact` **入口**就被清，所以采样必须早于 `kSettleMinMs`。

### ② `MovePath.SetForcedFlush` 真源 `0x11986F0` —— ✅ 已落地

`kRvaMovePathSetForcedFlush = 0x11986F0` 已绑定为 `gMpSetFlush`，`CallForcedFlush()` 按
「MovePath 原语 → VecCtrl wrapper(`0x11A19E0`) → 直写 `_forcedFlush@0x48`」三级降级。
绑定成功时日志打 `bound path=ImpactNext+Blink … MpFlush@0x11986F0 GetValid@0x11A2400`。

`IsTimeForFlush(bool)@0x1198700` 仍未使用，可用于压 `205` 的发包节奏。

### ③ `DiscardByInterrupt(int, VecCtrl, bool)@0x119F600` —— ⬜ 待逆

名字直译「因中断丢弃移动路径」。P0a §5 记录的
「狂点 → 冷却拒跳 → `Disconnected` · sticky `pendingError=205`（哨兵≠踢因）」仍未定位到判定点，这个函数是最像的候选。
**仅命名推断，未验证。**

### ④ §4.4 的三个实验 —— ⬜ 全部未执行

按「实验一 → 实验三」的顺序做。在实验一出结果之前，
**不要调低 `gImpactBlinkCdMs` 或压缩 `fly` 的 hop 间隔**——
我们尚不知道那 20 倍的适用条件，缩短间隔等于拿运气赌。

---

## 7. 文档时间线纠偏（重要 · 避免后人踩回旧坑）

本目录几份文档是**演进关系**，不是并列关系，读的时候必须看日期：

| 文档 | 日期 | 当时结论 | 现状 |
|---|---|---|---|
| [`坐标真源交叉验证.md`](坐标真源交叉验证.md) | 2026-08-01 | 「必须物理 + Transform 双钉」，对照 MXD / 枫星 | **已被取代**。当时尚未发现 `SetImpactNext` |
| [`P0a_瞬移CALL锚点.md`](P0a_瞬移CALL锚点.md) §2.1 | 2026-08-02 | IDA 实锤落地 helper 走 `SetImpactNext`，不调 `Transform.set_position` | 有效 |
| [`同步模型.md`](同步模型.md) §9 | 2026-08-02 | ImpactNext 探针；禁 Transform 回退 | 有效 |
| 本文 **首版** | 2026-08-02 上午 | `SetImpactNext` 写冲击速度，引擎消费它推 Ap；§4 三条「尚未利用」 | **已被本文第二轮取代**：①② 当天即落地；语义与位移成因均已改写 |
| 本文 **第二轮** | 2026-08-02 | 合并语义（§2）+ 完整消费链（§3）+ 踏板偷换（§4） | 有效 |
| [`P0c_视觉层同步链.md`](P0c_视觉层同步链.md) | 2026-08-02 下午 | 视觉真源 = `Ap`/`Apl` 经 `GetPos()` 插值；`Pos@0x64` 与 TRS 无关；`Apl` 是插值起点而非回溯点 | 有效 · **本文 §4.2 的 `Apl` 注释据此纠偏** |

两条要记住的教训：

1. **「必须钉 Transform」是没找到正确管道之前的补偿方案**，不是引擎设计。
   精确说：**落点钉一次**有效（它盖掉了 `Apl` 造成的插值滑行），**持续每帧钉**才是冻皮、掐刀的根因；
   而正解是补 `Apl`，根本不必跟引擎抢 `Transform` 的写入权。见 [`P0c`](P0c_视觉层同步链.md) §7.2。
2. **别把「函数名像 setter」当成「它就是 setter」。** `SetImpactNext` 会改写入参，
   `LeaveFoothold` 听着像清理其实在做坐标重算。CFA 混淆下，只有进函数体才算数。

---

## 8. 复现方法（可核对）

### 8.1 dump 侧

```powershell
$f = ".\DumpRestoredData\dump.cs.restored.C"
rg --no-ignore -n "RVA: 0x11A1A10 " $f          # → 命中行的下一行即签名
rg --no-ignore -n "^public class VecCtrl |^public class MovePath |^public class MoveElem " $f
$lines = Get-Content $f; $lines[71774..71830] -join "`n"
```

> **档位选择**：本文用 C 档（折中）。**字段偏移与方法签名不受 Rosetta 误映射影响**（恢复的只是名字），
> 但**方法名**在 C 档有 ordinal 同签名按序映射的误伤风险。写补丁锚点时按 `DumpRestoredData/README.md` 改用 **A / B** 档复核。

### 8.2 IDA 侧（IDB `Dumps/runtime/GameAssembly.dll.i64`，imagebase `0x7FFB16B40000`）

CFA 函数用 `decompile` 只能看到跳转表 prologue，必须走 `disasm` 逐块还原。有效手法：

- `analyze_function <VA>` — 拿 callers / callees / size / 首块伪代码，**判断函数职责最快的一招**
  （`LeaveFoothold` 的身份就是靠「被 `JustJump` 和 `ApplyImpact` 共同调用 + callee 是 `AbsPos.SetFromRelPos`」定的）
- `insn_query {mnem, func, include_disasm}` — 扫某函数内全部 `movsd`/`movupd`/`movss` 写入目标，
  **一次性回答「它到底写了哪个字段」**，比读 500 行反汇编快得多
- `disasm {addr, offset, max_instructions}` — 跳过 prologue 的表初始化段（通常前 60–100 条），直接看 CFA 块体
- `get_int {addr, ty:"u64"}` — 读浮点常量位模式（`0x408F400000000000` = 1000.0）

### 8.3 BIN 侧

```powershell
rg --no-ignore -n "ImpactNext|ImpactSettle" .\bin\XCat_data\logs\ .\Dumps\runtime\
```

`bound path=…` 行可确认当前构建实际绑上了哪几个 RVA，是判断「文档有没有过时」最快的方法。

---

## 9. 证据等级与置信度

| 结论 | 证据 | 置信度 |
|---|---|---|
| `VecCtrl` / `MovePath` / `MoveElem` / `AbsPos` / `RelPos` / `ImpactNext` 字段布局 | dump 原文直读 | **高** |
| `SetImpactNext` 的合并语义（条件清零 + 置 Valid + fmax/fmin 饱和） | 本轮逐块反汇编 + 两个 helper 反编译确认 | **高** |
| 消费链与新 RVA（`ApplyImpact 0x11A4E60`、`LeaveFoothold 0x11AF5C0`） | 反汇编 + `analyze_function` 的 callers/callees | **高** |
| impact 只写 `Ap.Vx/Vy`，不写 `Ap.X/Y` | `insn_query` 全函数扫描，写入目标仅 `0xA8`/`0xB0` | **高** |
| `dt = tElapse / 1000.0` | `get_int` 实读 `0x408F400000000000` | **高** |
| `TryRegisterTeleport` / `TryDoingTeleport` / `SetMovePathAttribute` | TW dump 里仍是哈希名；靠「RVA 与 P0a 一致 + 签名与 CMS 逐参对上」双证 | **高**（名字源自对照仓映射） |
| **踏板偷换是位移主因（§4）** | 机制链完整、与全部 BIN 观测自洽；但 `0x11AF5C0` **函数体未读** | **中高 · 假设** |
| 参数单位 = px/秒 | 由 §3 推导；与 §4 并不冲突（速度只管重算后的惯性段） | 中高 |
| 落地 helper 内部顺序（Attr → IsOnFoothold → SetImpactNext → EffectGeneral） | 引自 P0a §2.1，本文未重跑 | 中高 |
| `SetImpactNext` 写入 `WingsNow` 的**具体值** | 指令确定，常量被 MBA 混淆未解出 | 中 / 值未知 |
| `DiscardByInterrupt` 与 `205` 的关联 | 仅命名推断 | **低 · 未验证** |

**NOT RUN**：全程静态只读，未编译、未运行客户端、未做 BIN 复验。
§4.4 三个实验、§6③ 的逆向均未执行。`0x11AF5C0`、`CollisionDetectFloat`、`IsTeleportSkillAvailable` 的函数体均未进入。

---

## 10. 关联文档

- [`模块设计.md`](模块设计.md) — 职责边界与对外契约
- [`P0a_瞬移CALL锚点.md`](P0a_瞬移CALL锚点.md) — IDA 侧入口 RVA / 调用链 / BIN 纪要
- [`P0c_视觉层同步链.md`](P0c_视觉层同步链.md) — 位移发生**之后**：`Ap`/`Apl` 如何经 `GetPos()` 插值写进 `Transform`
- [`同步模型.md`](同步模型.md) — 坐标分层、收态禁区、探针实现
- [`坐标真源交叉验证.md`](坐标真源交叉验证.md) — 历史结论（已被 §7 纠偏）
- [`../protocol/MoveElem字段.md`](../protocol/MoveElem字段.md) — MoveElem wire 与 `MoveActionType`
- [`../protocol/移动协议.md`](../protocol/移动协议.md) — MovePath API 与 opcode
- [`../security/GRAP与枫星对齐.md`](../security/GRAP与枫星对齐.md) — 禁止 INLINE HOOK 的边界
- [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md) — 205 / 断线采证
- `DumpRestoredData/README.md` — 符号恢复分档 A / B / C / D 的选用建议
