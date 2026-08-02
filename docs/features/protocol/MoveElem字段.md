# MoveElem 字段拆解

> **产品**：新楓之谷：經典版（TW / beanfun）  
> **日期**：2026-07-30（§8–§11 深挖补记 2026-08-01）  
> **证据源**：`Dumps/runtime/out/dump.cs` ↔ `Dumps/cms_cw/dump.cs`；IDA `GameAssembly.dll.i64`；运行时 `Dumps/runtime/movepath_elems.log`  
> **工作笔记**：[`Dumps/move_elem_notes.md`](../../../Dumps/move_elem_notes.md)

---

## 1. 身份对照

| | TW（runtime dump） | CMS Rosetta |
|---|---|---|
| 类 | `f35f57ffc7406ea3…` TypeDef **1588** | `MoveElem` TypeDef 1481 |
| Attr 枚举 | `f04118ab33ae6583…` TypeDef **1587** | `MovePathType` TypeDef 1480 |
| rawAction 枚举 | `c5424cead0b219fa…` TypeDef **1624** | `MoveActionType` TypeDef 1516 |
| 容器 | `MovePath.Elem` @ `+0x30`（`List<MoveElem>`） | 同 |
| 上一片 | `MovePath._elemLast` @ `+0x38` | 同 |

字段类型与偏移与 CMS **逐字节一致**（Il2Cpp 对象头后从 `0x10` 起）。

别名表：`Dumps/sdk_aliases.tsv`、`Dumps/cms_cw/restore_class_map.tsv`、`Dumps/cms_cw/restore_enum_map.tsv`。

---

## 2. 对象布局（实例）

```text
+0x00  Il2CppObject (klass / monitor)
+0x10  byte   Attr          // MovePathType
+0x12  short  X
+0x14  short  Y
+0x16  short  Vx
+0x18  short  Vy
+0x1A  byte   MoveAction    // (rawAction<<1) | faceLeft
+0x1C  short  Fh            // 当前 foothold id
+0x1E  short  FhFallStart   // 下落起点 foothold
+0x20  short  Elapse        // 本段耗时 (ms)
+0x22  byte   Stat          // SecondaryStat 变更标记等
```

对齐后实例尺寸约 **0x28**（含尾部 padding）。

`MoveElem_Reset`（IDA `0x7ffb17cd8540`）按同一偏移清零：`Attr@+0x10` ← `(gbyte^0xF)`（当前为 Normal=0）；`QWORD@+0x12` 清 xy+v；`MoveAction@+0x1A=0`；Fh 对清零；再覆盖 Elapse/Stat。

`MovePath_ParseMove_private` 对 `+0x10…+0x22` 有明确写回（含 `ma@+1A` / `el@+20` / `st@+22`）。

### TW 哈希字段 ↔ 名字

| Off | TW 字段 hash（截断） | 名 |
|---|---|---|
| +0x10 | `a061e3cf…` | Attr |
| +0x12 | `ce279213…` | X |
| +0x14 | `a267ad33…` | Y |
| +0x16 | `f4ffff22…` | Vx |
| +0x18 | `a66b76f3…` | Vy |
| +0x1A | `a44dd373…` | MoveAction |
| +0x1C | `ec25a8c2…` | Fh |
| +0x1E | `dab1d45c…` | FhFallStart |
| +0x20 | `e92423b3…` | Elapse |
| +0x22 | `cb6397b2…` | Stat |

---

## 3. Attr = MovePathType（0..22）

| 值 | CMS 名 | 说明（常用） |
|---|---|---|
| 0 | Normal | 普通走/滑 |
| 1 | Jump | 跳 |
| 2 | Impact | 撞击落地类 |
| 3 | Immediate | 立即同步 |
| 4 | Teleport | 瞬移 |
| 5 | HangOnBack | 背挂 |
| 6 | FlashJump | 闪跳 |
| 7 | Assaulter | 突袭 |
| 8 | Assassination | 暗杀位移 |
| 9 | Rush | 冲刺 |
| 10 | StatChange | 属性变点（常带 Stat） |
| 11 | SitDown | 坐下 |
| 12 | MobPowerKnockBack | 怪击退 |
| 13 | BackstepShot | 后跳射击 |
| 14 | StartFalldown | 开始下落 |
| 15 | FallDown | 下落中 |
| 16 | StartWings | 开始飞（翅膀） |
| 17 | Wings | 飞行中 |
| 18 | VerticalJump | 垂直跳 |
| 19 | CustomImpact | 自定义撞击 |
| 20 | CombatStep | 战斗步伐 |
| 21 | AranAdjust | 战神修正 |
| 22 | MobToss | 怪投掷 |

飞天相关优先看 **16 / 17（StartWings / Wings）**；现行贴板锁路径实测全是 `Normal(0)`。

Encode Absolute 分支在写完 `X/Y/Vx/Vy/Fh` 后，用混淆常量判断 **Attr==15（FallDown）** 才继续写 `FhFall`（`dword ^ 0xE54518AD == 0xF`）。说明 **attr 决定 wire 省略规则**。

---

## 4. 附：MovePath / MovePathRect

`MovePath`（`fd62a654…`）关键字段（与 CMS 同）：

| Off | 名 | 类型 |
|---|---|---|
| +0x10 / +0x12 | `_x` / `_y` | short |
| +0x14 / +0x18 | `_vx` / `_vy` | int |
| +0x1C | `_interval` | int |
| +0x20 | `_offset` | int |
| +0x24 | `_received` | int |
| +0x28 | `_fhLast` | short |
| +0x2C | `_gatherDuration` | int |
| +0x30 | `Elem` | `List<MoveElem>` |
| +0x38 | `_elemLast` | MoveElem* |
| +0x40 | `_keyPadState` | `List<byte>` |
| +0x48 | `_forcedFlush` | bool |
| +0x4C | `_move` | MovePathRect（16B） |
| +0x5C | `_shortUpdate` | bool |

`MovePathRect`（`c0d41b91…`）：`Left / Top / Right / Bottom` @ `+0 / +4 / +8 / +C`（int）。

挂载链：`FieldActorBase.VecCtrl @ +0x50` → `VecCtrl.MovePath @ +0x78`。

---

## 5. 线上编解码（包内顺序）

| API | RVA | IDA 名 |
|---|---|---|
| `Encode(OutPacket)` | `0x119AB50` | `MovePath_Encode` |
| `Decode(InPacket, isPassive)` | `0x119D290` | `MovePath_ParseMove_private` |
| `OnMovePacket` | `0x11A0490` | `MovePath_OnMovePacket` → Decode |
| `Flush` | `0x1198DC0` | `MovePath_Flush` → Encode（**无**本地 `Δpos≈v·Δt` 门禁，见 [移动协议.md](./移动协议.md) §4） |

Encode / Decode **按 Attr 分支**（壳 + 间接跳），静态 F5 难读干净。

- **已实锤**：对象字段序与类型；Flush 只打包不校验轨迹自洽；FallDown 才写 FhFall  
- **未钉**：每种 Attr 的完整 wire 省略表 → 建议 iDbg hook `MovePath_Encode` / `Flush`，走路 · 跳 · 飞各采 hex 再表驱动还原  

常见 Absolute 类片段（行业惯例 + Encode 反汇编 Absolute 块读序，**完整包序仍待 hex 复核**）：

```text
byte  Attr
short X, Y
short Vx, Vy      // 部分 Attr 才有
short Fh
byte  MoveAction
short Elapse
// FallDown 等再跟 FhFall；StatChange 再跟 Stat
```

---

## 6. IDA 跳转

- `G` → `MoveElem_ctor` / `MoveElem_Reset` / `MoveElem_Clone`
- `G` → `MovePath_Encode` / `MovePath_ParseMove_private` / `MovePath_Flush` / `MovePath_OnMovePacket`
- `G` → `MoveAction_GetRawAction_sar1` / `MoveAction_MakeActDir` / `MoveAction_GetFaceLeft_bit0`
- `G` → `UserLocal_OnResolveMoveAction` / `User_OnResolveMoveAction` / `FieldActorBase_OnResolveMoveAction`
- `G` → `UserBase_IsDead_byMoveAction_S9`
- Recv 入口见 [移动协议.md](./移动协议.md)

---

## 7. 验证状态

| 项 | 状态 | 证据 |
|---|---|---|
| TW↔CMS 字段偏移 | 已完成 | dump.cs TypeDef 1588 ↔ 1481 |
| Reset 清零序 | 已完成 | IDA `MoveElem_Reset` |
| MovePathType 0..22 | 已完成 | TW enum 1587 ↔ CMS `MovePathType` |
| MoveAction 打包公式 | 已完成 | IDA `MakeActDir` / `GetRawAction_sar1` |
| rawAction = `MoveActionType` | 已完成 | TW enum 1624 ↔ CMS 1516；IsDead xor→9 |
| wire 按 Attr 表 | **NOT RUN** | Encode 壳；需 hook 采包 |
| Flush 本地 `Δpos≈v·el` | **已否定** | IDA 无 `3E8h`；矛盾包仍发 |
| 积分器离线自洽 | **PASS**（2026-08-01） | `fly_integrator_check.cpp` → 9/9 hold |
| 元素探针偏移 | **PASS**（2026-08-01） | `movepath_elems.log` 340 元素跟随走跳 |

---

## 8. 探针缩写：`xy / v / ma / fh / attr`（2026-08-01）

日志一行形如：

```text
E0[Normal(0) xy=(205,-155) v=(0,0) ma=5 fh=22 fhFall=0 el=30 st=0]
```

| 缩写 | 字段 | 深层含义 |
|---|---|---|
| **attr** | `Attr` / `MovePathType` | 本段位移**语义标签**（走/跳/瞬移/翅膀…）；决定 Encode 分支与可选字段 |
| **xy** | `X,Y` | 本段结束时的权威坐标；服端用它更新「人在哪」 |
| **v** | `Vx,Vy` | 速度向量；与 `Δpos≈v·el/1000` 的契约面在**服端**（客户端 Flush 不拦） |
| **ma** | `MoveAction` | `(rawAction<<1) \| faceLeft`；姿态×朝向打包字节 |
| **fh** | `Fh` | 立足板 **id**（short）；`0` = 空中。运行时 `CurFootHold*` 是指针，Elem 里是编号 |

挂载与节奏：

```text
FieldActorBase → VecCtrl(+0x50) → MovePath(+0x78) → List<MoveElem>(+0x30)
~30ms 物理步累加元素 → ~500ms Flush → Encode → UserMove (pktId=47)
```

读包心智模型：

> 在 `el` ms 内，角色以速度 `v` 到达 `xy`，姿态 `ma`，踩着板 `fh`（或空中），位移类型 `attr`。

### 8.1 合法基线形状（`movepath_elems.log` · fly=0）

| 场景 | attr | xy | v | ma | fh |
|---|---|---|---|---|---|
| 站立心跳 | Normal | 不变 | `(0,0)` | 4/5（Stand） | ≠0 |
| 走路 | Normal | 递增 | `(±125,0)` 量级 | 2/3（Walk） | ≠0 |
| 起跳首元 | Jump | 起跳点 | `vy≈+555` | 6/7（Jump） | 0 |
| 空中续段 | Normal | 随积分 | vy 单调衰减 | 6/7 | 0 |
| Alert 待机 | Normal | 不动 | `(0,0)` | 8/9（Alert） | ≠0 |
| StatChange | StatChange | 常 `(0,0)` | `(0,0)` | 0 | 0 |

### 8.2 贴板飞实测（问题根因）

飞行 ON：全程 `attr=Normal ma=5 v=(0,0) xy` 冻在臂上点、`fh≠0`。  
本地 Ap 已移动，元素未跟 → **服端认为人站着没动**（本地幻影）。  
基线走跳里 `fh=0` 出现数十次属正常空中，**不是踢因**。

---

## 9. MoveAction 打包 / 解包（IDA 实锤）

```text
ma        = (rawAction << 1) | faceLeft     // faceLeft: 1=左, 0=右
rawAction = ma >> 1                        // MoveActionType
faceLeft  = ma & 1
```

| IDA 名 | VA | 作用 |
|---|---|---|
| `MoveAction_GetRawAction_sar1` | `0x7ffb181d2ff0` | `return ma >> 1` |
| `MoveAction_MakeActDir` | `0x7ffb181d3020` | `return (raw<<1) \| (bLeft?1:0)` |
| `MoveAction_GetFaceLeft_bit0` | `0x7ffb181d3000` | 取朝向 bit0（混淆比较） |
| `UserBase_IsDead_byMoveAction_S9` | `0x7ffb17d83930` | `GetRawAction(ma) == 9`（Dead；xor 常量解出 `0x9`） |

`UserLocal.OnResolveMoveAction`（TW `bbb14657…` : `c3f0caba…`）  
- RVA `0x1082E40` / VA `0x7ffb17bc2e40`（IDA 已标 `UserLocal_OnResolveMoveAction`）  
- 调用 `GetRawAction_sar1` + `MakeActDir`，按 `inputX/inputY` 与当前 foothold/空中态解析下一 `ma`  
- 函数体有控制流平坦化，**名字表不靠 F5 字符串，靠枚举 dump**

父类链：`FieldActorBase` 虚槽 Slot:20 → `User` → `UserLocal` override。

---

## 10. rawAction = `MoveActionType`（0..17 名字表）

> TW enum `c5424cead0b219fa…` TypeDef **1624** ≡ CMS `MoveActionType` TypeDef **1516**（`restore_enum_map.tsv`）。  
> 数值与 CMS **逐项一致**（含 Walk=Move=1 双名）。

| raw | CMS 名 | 典型 ma（右/左） | 探针实证（340 元素） |
|---|---|---|---|
| 1 | **Walk** / Move | 2 / 3 | 走：`vx` 符号与朝向一致，`fh>0` |
| 2 | **Stand** | 4 / 5 | 站立主体（ma=5×247）；偶发 ma=4 带残余 vx |
| 3 | **Jump** | 6 / 7 | 空中/跳：`fh=0` 全覆盖；起跳 `attr=Jump` |
| 4 | **Alert** | 8 / 9 | 地面静止另一姿态（ma=9×11） |
| 5 | **Prone** | 10 / 11 | 本 log 未见 |
| 6 | **Fly1** | 12 / 13 | 本 log 未见（翅膀另看 Attr 16/17） |
| 7 | **Ladder** | 14 / 15 | 本 log 未见 |
| 8 | **Rope** | 16 / 17 | 本 log 未见 |
| 9 | **Dead** | 18 / 19 | `IsDead` 比较 raw==9（IDA xor 实锤） |
| 10 | **Sit** | 20 / 21 | 本 log 未见 |
| 11 | **Stand0** | 22 / 23 | 本 log 未见 |
| 12 | **Hungry** | 24 / 25 | 本 log 未见 |
| 13 | **Rest0** | 26 / 27 | 本 log 未见 |
| 14 | **Rest1** | 28 / 29 | 本 log 未见 |
| 15 | **Hang** | 30 / 31 | 本 log 未见 |
| 16 | **Chase** | 32 / 33 | 本 log 未见 |
| 17 | **No** | 34 / 35 | 本 log 未见 |

注意：枚举从 **1** 起；`raw=0` 仅见于 `StatChange` 等特殊元，不是 `MoveActionType` 常规成员。

CMS 另有更大的 `ActionType`（Walk1/Stand1/Swing… 动画层，TypeDef 1517）——那是**剪辑/图层动作**，不是 Elem 里的 `ma>>1`。勿与 `MoveActionType` 混用。

### 10.1 对飞天工线

| 期望元素 | 含义 |
|---|---|
| `ma=2/3` + `v=(±125,0)` + `xy` 递增 | 客户端自走（InputX 实验验收） |
| `ma=5` + `v=(0,0)` + `xy` 冻结 | 站立心跳（贴板锁现状） |
| `ma=6/7` + `fh=0` | 合法空中（清 fh 后状态机会吐这个） |

---

## 11. InputX 调用链与 OnResolveMoveAction 决策树（2026-08-01）

### 11.1 `VecCtrl.InputX/Y` 身份

| 证据 | 内容 |
|---|---|
| CMS dump | `VecCtrl.InputX@0x50` / `InputY@0x54`；`MoveAction@0x84` |
| TW restore | `restore_field_map.tsv` → **exact** |
| IDA `VecCtrl_GetInput` @ `0x11B52B0` | `*outX = *(vc+0x50); *outY = *(vc+0x54)` |
| IDA `VecCtrl_SetInput` @ `0x11B52C0` | 写 `+0x50/+0x54` 后 **立刻** 虚调 Owner `OnResolveMoveAction`（vtable `+0x278`），结果写回 `MoveAction@+84` |

### 11.2 Resolve → OnResolve

IDA `VecCtrl_ResolveMoveAction` @ RVA `0x11A1680`（VA `0x7ffb17ce1680`）：

```text
edx = [vc+0x50]   // InputX
r8d = [vc+0x54]   // InputY
r9d = [vc+0x84]   // cur MoveAction
call Owner.vtable OnResolveMoveAction(this, inputX, inputY, curMA, vc)
[vc+0x84] = eax   // 新 MoveAction
```

调用方（代码 xref，不含 data）：

| 调用方 | 角色 |
|---|---|
| `sub_7FFB17A7E2E0` | 连续两次 Resolve；再把 `vc+84` 拷到 User `+0x58` |
| `sub_7FFB17AD5470` | 单次 Resolve + 与 User`+0x58` 比对/回写 |
| `VecCtrl_SetInput` 内联 | 写 Input 后走同一套 Owner 虚调（不经 `ResolveMoveAction` 符号） |

⇒ **飞天 `FLY_DRIVE_INPUT` 写 `InputX` 的控制面与客户端自洽路径一致**（静态 PASS）。
端到端位移仍要 BIN：`v≠0` + `xy` 递增。

### 11.3 决策树（部分 · 混淆常量已解）

寄存器约定（`User_OnResolveMoveAction` 序言）：

| 寄存器 | 参数 |
|---|---|
| `edi` | inputX |
| `ebp` | inputY |
| `esi` | curMoveAction |
| `rbx` | this (User) |

**UserLocal**（先跑，再可能调用父类）：

| MakeActDir(raw) | 解出值 | 名 |
|---|---|---|
| UL1 | 10 | Sit |
| UL2 | 9 | Dead |
| UL3 | 2 | Stand |
| 其余 | fallthrough → `User_OnResolveMoveAction` | |

**User**（父类 · Walk 站点已钉死）：

| MakeActDir(raw) | 解出值 | 名 | 旁证 |
|---|---|---|---|
| `@…15f97` | **1** | **Walk** | `cmp edi, 0`（`99A0C3CB xor key → 0`）→ `setl dl` 作 faceLeft；`MakeActDir(1, faceLeft)` |
| site | 7 | Ladder | 常量解出 |
| site | 8 | Rope | 常量解出 |
| site | 5 | Prone | 常量解出 |
| 其它 | 保留 face / `cmp ebp, edi` | | |

完整 CFG 仍受控制流平坦化干扰，**未逐块还原**；但 **Walk=1 由 inputX 相对 0 的路径发出、朝向=inputX<0** 已实锤。

### 11.4 `SetInput(0,0)` 清零与飞天线程竞态（2026-08-01 续）

| 事实 | 证据 |
|---|---|
| 多处 `SetInput` 调用点把参数解成 **0** | IDA 对 `sub_7FFB17CFDCE0` / `…CFFAF0` / `…D00B50` 等站点 xor/add 后为 `SetInput(?,0)` 或双 0 |
| 飞天 `TickFly` 跑在 **独立 worker**（`fly.cpp` `FlyThread` ~60Hz），不是 `User.Update` / MainPump | `xcat_probe.cpp` → `fly::StartWorker()` |
| 非 drive 路径每帧 `ApplyContactState` → `WriteVcInput(0,0)` | `fly_impl.cpp`；drive 臂 **early-return 不走** 此清零 |
| 飞天只写字段，**不调** `SetInput`（因此也不内联 Resolve） | `WriteVcInput` 仅 `*(vc+50/54)` |

含义：即便控制面选对，**主线程若每帧 `SetInput(0,0)`，可在两帧之间盖掉 worker 写入**。探针用 `in_before`（写前读）抓帧间踩踏；`in_rb`（写后立刻读）只能抓同线程竞态。

`VecCtrl.Move(x,y)` @ `0x11A1720` 是 **瞬移/置位 Ap**（写 `+0x98/+0xA0` 等），**不是** 走路输入通道——勿与 `SetInput` 混淆。

### 11.5 IDA 符号（本轮已标）

| 名 | RVA |
|---|---|
| `VecCtrl_ResolveMoveAction` | `0x11A1680` |
| `VecCtrl_Move`（置位，非 Input） | `0x11A1720` |
| `VecCtrl_GetInput` | `0x11B52B0` |
| `VecCtrl_SetInput`（写 Input + 内联 Resolve） | `0x11B52C0` |
| `VecCtrl_SetMoveAction` | `0x11A1670` |
| `UserLocal_OnResolveMoveAction` | `0x1082E40` |
| `User_OnResolveMoveAction` | `0xFD5990` |
| `MovePathFlush_big_caller_likely_EndUpdateActive` | 大函数调用 `MovePath_Flush`（旧笔记 `0x11AD910` 在本 IDB 为 **nullsub**，已废） |
| `MoveAction_MakeActDir` | （邻近 `GetRawAction_sar1`） |
| `MoveAction_GetRawAction_sar1` | — |

### 11.6 物理积分：`CalcWalk` ← `InputX`（2026-08-01 IDA 续）

CMS 名对照（`restore_method_map` + TW dump 槽位）：

| 名 | TW RVA | 证据 |
|---|---|---|
| `BeginUpdateActive` | `0x11A41E0` | Slot:10 hash `b64bf8b8…` |
| `WorkUpdateActive`（基类） | `0x11A4400` | Slot:11；内调 `CalcWalk` / `CalcFloat` |
| `CalcWalk` | `0x11A6090` | `(int tElapse)`；多次读 `[vc+50h]`；调 `MaxWalkSpeed` |
| `MaxWalkSpeed` | `0x11AFD20` | hash `b49a16d0…` |
| `AccSpeed` / `DecSpeed` | `0x11B00C0` / `0x11B0280` | hash 对照 |
| `CalcFloat` | `0x11A9530` | 空中积分 |
| `EndUpdateActive`（基类） | `0x11AD910` | Slot:12；本 IDB **单字节 `ret`**（空实现） |
| `IsTimeForFlush` | `0x1198700` | `VecCtrl*EndUpdate` 调用链上的门 |
| `VecCtrlUser.BeginUpdateActive` | `0x11CF920` | 调基类 Begin |
| `VecCtrlUser.WorkUpdateActive` | `0x11CFA90` | **本地玩家**走路主循环 |

`CalcWalk` 实锤读法：

```text
cvtsi2sd xmm6, dword ptr [rsi+50h]   ; InputX -> double
mulsd    xmm6, xmm8                  ; × 步行力/系数
… MaxWalkSpeed …
cmp [rsi+50h], 0                     ; 有无横向输入（密钥解出 0）
```

调用链（本地玩家）：

```text
VecCtrlUser_WorkUpdateActive @ 0x11CFA90
  ① SetInput( helper[+0x10], 0 )     // 把手柄/键位锁存拷进 InputX
  ② VecCtrl_WorkUpdateActive（基类）
       └─ CalcWalk(tElapse)          // 读 InputX 积分 Ap
  ③ SetInput(…, 0)                   // 再写一次（Y 常量解出为 0）
```

#### 键位锁存（比裸写 `InputX` 更上游）

`VecCtrlUser`（hash `d65293d7…`）字段：

| Off | 含义 |
|---|---|
| `+0x100` | 嵌套对象*（hash `fbb5af63…`） |
| 对象 `+0x10` | int — **锁存 InputX**（`SetInput` 的 edx 来源） |
| 对象 `+0x14` | int — **锁存 InputY** |

每帧 `WorkUpdate` **先** `SetInput(锁存, 0)` **再** `CalcWalk`。  
因此 worker 线程只写 `vc+0x50` 时，下一帧 Update 开头会被锁存（无键=0）盖掉，**CalcWalk 永远看到 0**——这是 `FLY_DRIVE_INPUT` 最硬的静态失败模式。

| 控制面 | 能否驱动 CalcWalk | 备注 |
|---|---|---|
| 只写 `InputX@+0x50`（现行） | **否（静态）** | 被同帧 `SetInput(锁存)` 覆盖 |
| 写锁存 `*(vc+0x100)+0x10` | **否（静态）** | 同帧更早的 KeyPad 采样会重写锁存 |
| MainPump 在 `SetInput` 之后写 `+0x50` | 理论可行 | 要插在 ①② 之间，难无 hook |
| `input_port` 注入左右键 | **首选** | 见 §11.8 |
| hook `SetInput` 注入方向 | 可行但碰 `.text` | GRAP 默认禁止 |

`VecCtrl.Move` 仍是置位 Ap，与走路无关。  
`VecCtrlDragon_WorkUpdateActive`（旧误标 User）**不**走 `CalcWalk`；本地角色是 `VecCtrlUser`。

### 11.7 IDA 符号补表（本轮）

| 名 | RVA |
|---|---|
| `VecCtrl_BeginUpdateActive` | `0x11A41E0` |
| `VecCtrl_WorkUpdateActive` | `0x11A4400` |
| `VecCtrl_CalcWalk` | `0x11A6090` |
| `VecCtrl_CalcFloat` | `0x11A9530` |
| `VecCtrl_MaxWalkSpeed` | `0x11AFD20` |
| `VecCtrl_AccSpeed` / `DecSpeed` | `0x11B00C0` / `0x11B0280` |
| `VecCtrl_EndUpdateActive`（基类空） | `0x11AD910` |
| `MovePath_IsTimeForFlush` | `0x1198700` |
| `VecCtrlUser_BeginUpdateActive` | `0x11CF920` |
| `VecCtrlUser_WorkUpdateActive` | `0x11CFA90` |
| `VecCtrlDragon_WorkUpdateActive` | `0x11B71E0` |
| `VecCtrlDragon_EndUpdateActive` | `0x11B8D10`（调 Flush） |
| `KeyPad_Query` | `0x16BF3D0`（VA `0x7ffb181ff3d0`；~830B；~124 调用点） |
| `VecCtrlUser_ApplyInputMessage` | `0x11CE7E0` |
| `FeedVecCtrlUserInput` | `0xF71B60` |
| `VecCtrlUser_InputLatch_Clear` | `0x11D0330`（写锁存 `+10/+14 = 0`；**无代码 xref**） |
| `VecCtrl_Orch_UpdateThenFlush` | 大函数；`IsTimeForFlush` → `UpdateActive` → `Flush` |
| `VecCtrl_Orch_UpdateThenFlush_alt` | 同模式另一包装 |

### 11.8 整条通路（键 → Elem → Flush）（2026-08-01 续拆）

```text
[玩家键 / input_port KeyDownTouch]
        │
        ▼
 KeyPad_Query(mod, mask)          // ecx=取模；User: (5,0)+(0,0)×3
   底层 = KeyPad_GetState / vmethod[+0x178]
        │
        ▼
 *(VecCtrlUser+0x100) 锁存对象
   +0x10 = InputX 锁存 (±1 来自 Query&1；或 Clear→0)
   +0x14 = InputY 锁存（Query 聚合后再 MBA 写入）
        │
        ▼
 VecCtrl_SetInput(latchX, 0)     // 写 InputX@+0x50 + 内联 OnResolve → ma
        │
        ▼
 VecCtrl_WorkUpdateActive（基类）
   → CalcWalk / CalcFloat / MakeMovePath → 往 MovePath 塞 Elem
        │
        ▼（同帧后半）
 可选：锁存 X 取负（Ap 变化且 latchX≠0 时）再 SetInput 一次
        │
        ▼
 外层编排（非基类空 EndUpdate）：
   VecCtrl_Orch_UpdateThenFlush(*)
     IsTimeForFlush → UpdateActive(Begin/Work/End) → MovePath_Flush → Encode 发包
   Dragon/Mob 另有子类 EndUpdate 内直接 Flush
```

#### 与「网络喂输入」分流

| 通道 | 入口 | 写什么 | 是否进 CalcWalk |
|---|---|---|---|
| **本地键盘** | `KeyPad_Query` → 锁存 → `SetInput` | `+0x100` 锁存 → `InputX@+0x50` | **是** |
| **收包喂入** | `OpRecv_012C` / `UserLocal_Update_or_InputTick` → `FeedVecCtrlUserInput` → `ApplyInputMessage` | `bool@+0xF8` + MapBound 浮点等 | **否**（另一套；勿当走路锁存） |
| **MovePath.SetKeyPadState** | CMS：`MovePath._keyPadState@+0x40` | 进 **Encode 包体** 的键位列表 | **否**（上报装饰，不驱动物理） |

> CMS `dump.cs` 里 `VecCtrlUser` 字段是 `_user@0xF8 / _maxFreeFallTickCount@0x100`，**与 TW 本 IDB 不符**。TW 实锤：`[vc+0x100]` 是锁存对象指针（`+0x10/+0x14` = XY）。偏移以 IDA 为准，勿抄 CMS 字段名硬套。

#### F6 控制面终表（静态）

| 方案 | 静态结论 | 原因 |
|---|---|---|
| A. worker 只写 `InputX@+0x50` | **不足** | 同帧 `SetInput(锁存)` 在 CalcWalk 前覆盖 |
| B. worker 写锁存+InputX（现行 `WriteVcInput`） | **仍不足** | WorkUpdate **更早** KeyPad 重采样锁存，无键仍变 0 |
| C. MainPump 夹在 SetInput↔CalcWalk 写 `+0x50` | 理论可行 | 无 `.text` hook 极难对准窗口 |
| D. `input_port` 注入 Left/Right（UnityKey 61/62） | **待 BIN 验桥** | 与 KeyPad 无共享全局 |
| G. `KeyPad_SetFields` 写单例 A/B/C | **降级 · 未证驱动物理** | 见 §11.10；写面多为收包；`+178` 虚体≠PackState 仍可能 |
| E. 调 `SetInput(±1,0)`（`attack_input_port` 已用同 RVA） | 只够朝向/瞬时 | 下一帧仍被 KeyPad 锁存清掉 |
| F. hook SetInput / WorkUpdate | 可行但禁 | GRAP 禁止改 `.text` |

`KeyPad_Query` / PackState 见 §11.9–§11.10。

### 11.9 `KeyPad_Query` 语义与 InputManager 桥（2026-08-01 续）

#### 函数族（同模块，`g_KeyPad_Il2CppClass` @ `qword_7FFB1D4974F8`）

| 名 | RVA / VA | 角色 |
|---|---|---|
| `KeyPad_CtorOrInit` | `0x16BEBD0` | 初始化 |
| `KeyPad_SetFields` | `0x16BEE40` | **明文**写 A/B/C → `+10/+14/+18` **且镜像** `+1C/+20/+24` |
| `KeyPad_SetFields_Obf` / `_Obf2` | `0x16BED40` / `0x16BEDA0` | 混淆写（`OpRecv_009D`）；等效强制 tag：`A\|=0x100000`，`B\|=0x1000`，`C\|=0x10` |
| `KeyPad_GetFields` | `0x16BEE20` | 读出 A/B/C |
| `KeyPad_PackState` | `0x16BEE60` | **破坏性**：先拷到 shadow，再把 primary 改成片段并返回 pack int |
| `KeyPad_PackStateFromShadow` | `0x16BF010` | **非破坏**；从 shadow 打包；与 PackState **同值**（抽样一致） |
| `KeyPad_ShadowToPrimary` | `0x16BF000` | shadow → primary |
| `KeyPad_GetState` | `0x16BF120` | 单例 → `call [klass+0x178]`；`-1`→`0` |
| `KeyPad_Query` | `0x16BF3D0` | `mod==0` 尾调同一虚体；否则 `state % mod`（mask==0 时） |
| `KeyPad_QueryMod` | `0x16BF710` | 取模变体 |
| `KeyPad_EnsureSingleton` | `0x16BF8F0` | `static_fields[0]=instance` |
| `KeyPadHolder_PackFromShadow` | `0x11B9AC0` | `[holder+10]→PackStateFromShadow`；挂在 Orch/`UpdateThenFlush` 编码侧 |

> 旧笔记曾写 `0x16BAE60` 等，少加了 `0x400`；以本表 VA−`0x7ffb16b40000` 为准。

#### 对象布局

| Off | 名 | 镜像 |
|---|---|---|
| `+0x10` | A（primary） | `+0x1C` shadow |
| `+0x14` | B | `+0x20` |
| `+0x18` | C | `+0x24` |

```text
KeyPad_Query(mod, mask):
  state = vcall(singleton.klass+0x178)   // GetState 同源；≠已证 PackState
  if mod == 0:  return state            // 尾调
  rem = state % mod
  if mask == 0: return rem
  else:         return MBA(rem, mask)
```

#### User.WorkUpdate 四个调用点（解码实锤）

| 站点 | 参数 | 调用后用法 |
|---|---|---|
| `…ff44` | `(5, 0)` | `state%5`；与 `0` 比较后进 Y 相关路径 |
| `…ffc9` | `(0, 0)` | 全量；再 `%2000` + MBA → **锁存 Y@+14** |
| `…00ac` | `(0, 0)` | 再一次；继续贡献 Y |
| `…0154` | `(0, 0)` | **`state&1` vs 常量 0**：even→锁存 X **+1**；odd→**−1**（每帧必写） |

#### 与 `input_port` / `KeyDownTouch` 的关系（静态）

| 事实 | 证据 |
|---|---|
| `InputManager_KeyDownTouch` @ RVA `0x1661F60` | 与 `input_port.cpp` 一致 |
| KeyDownTouch **不**引用 `g_KeyPad_Il2CppClass` | data-global 交集为空 |
| `SetFields` 代码 xref | 复制器 / `OpRecv_009D` / 构造旁路为主——**未见本地键盘直写** |

#### F6 控制面终表（更新）

| 方案 | 静态结论 | 原因 |
|---|---|---|
| A–C / E–F | 同前 | 见上表 |
| D. `input_port` 键注入 | **仍首选待 BIN** | 与真实按键同源的概率更高 |
| G. `KeyPad_SetFields` | **仅候选** | 见 §11.10；勿当已证物理驱动 |

取单例：`klass → [klass+0xB8] → [static_fields]`。

#### IDA 命名本轮

| 符号 | 说明 |
|---|---|
| `g_KeyPad_Il2CppClass` / `KeyPad_*` | 字段与 Query API |
| `KeyPadHolder_PackFromShadow` | Orch 编码侧包装 |
| `KeyPad_ApplyFromRecv009D` | 收包改 KeyPad（`SetFields_Obf`×8） |
| `InputManager_KeyDownTouch` 等 | 与 `input_port` 对齐 |
| `VecCtrl_Orch_UpdateThenFlush` / `_alt` | Flush 编排 |

### 11.10 `PackState` 代数拆解与左右极性（2026-08-01）

#### 闭环形式（Python 对拍 · Pack ≡ FromShadow）

对 `A∈[0,63]`：`At = (A>>1)<<13`（**丢掉 A 的 bit0**）。  
对抽样范围：`Bt = (B>>3)<<7`；`Ct ≈ (C>>4)<<21`（大 C 有进位例外）。  
最终 `ret` 为各片段的 MBA-XOR 组合；**PackState 与 PackStateFromShadow 对拍同值**。

| 输入 | pack | bit0 |
|---|---|---|
| `(0,0,0)` | `0` | 0 |
| `(2,0,0)` / `(3,0,0)` | `8192` | 0 |
| `(64,0,0)` | `262145` | **1** |
| `(0,8,0)` | `128` | 0 |
| `(0,0,16)` | `2097152` | 0 |
| Obf 空闲 tag `(0x100000,0x1000,0x10)` | `0x214002` | 0 |

`A∈[0,63]` 全组合 **无奇数 pack**；要 `bit0=1` 需要更大的 A（或 C≥256 等）。

#### 与锁存 X 的张力（重要）

User 路径：`even → latchX=+1`，`odd → latchX=−1`，且**无“未按键跳过写”**。  
若虚体真是 PackState，则空闲 `(0,0,0)` / Obf-tag 空闲皆 even → **每帧强制 +1**，与正常站立矛盾。

因此静态结论：

1. **`klass+0x178` 尚未钉死 = PackState**（仅 MethodInfo 数据挂接；无代码直接 call）。  
2. PackState / FromShadow 更像 **MovePath/Orch 上报打包**（`KeyPadHolder_PackFromShadow` ← Orch 消费函数）。  
3. 本地走路 Query 的虚体更可能是 **读实时键位的另一方法**（待 BIN：断 `GetState` 的 `call rax` 看目标）。

#### `SetFields` 对 F6 的含义

| 动作 | 影响 |
|---|---|
| `SetFields` 明文 | 同时写 primary+shadow |
| `SetFields_Obf` | 写入时 OR 固定 tag 位 |
| 若虚体≠PackState | **改 A/B/C 不驱动锁存**；最多污染 Encode 键位字段 |
| 若虚体=PackState（需 BIN 推翻站立矛盾） | 需找到使 `pack&1` 翻转的 A/B/C，不能靠 A=±1 |

#### 下一步（BIN / 静态）

1. 运行时看 `GetState` @ `call [rax]` 实际落入哪；对比按左/右/空闲返回值。  
2. 若虚体非 Pack：回到 **D.`input_port`** 或找真正写锁存前的键采样函数。  
3. 若虚体是 Pack：用 GetFields 在实机按键时采 A/B/C，再反推左右。

### 11.11 `Slot:4` 钉死 + KeyPadLive 子类（2026-08-01 续）

#### `klass+0x178` = 虚表 Slot 4（静态钉死）

| 证据 | 内容 |
|---|---|
| `dump.cs` TypeDef **2517** | `PackState` @ RVA `0x16BEE60` 标 **`Slot: 4`**、`virtual uint` |
| Il2Cpp 惯例 | vtable 基址 `klass+0x138` → slot4 @ `+0x178`（与 GetState/Query 一致） |
| 结论 | 对 **基类实例**，Query/GetState 的虚调 **就是** `KeyPad_PackState` |

站立矛盾仍在：基类空闲 pack=0 → even → 锁存 X=+1。因此要么单例实际不是「纯 0 字段的基类」，要么还有未拆门闩。

#### 子类 `KeyPadLive`（TypeDef **1253**）覆盖 Slot 4

| 项 | 值 |
|---|---|
| 继承 | `KeyPadLive : KeyPad`（混淆名见 dump） |
| 额外字段 | `uint @ +0x28` |
| Slot4 覆盖 | RVA `0xC92660` / VA `0x7ffb177d2660` |
| 实现 | **`return *(uint*)(this+0x28);`**（4 字节，无 Pack MBA） |
| 命名（IDA） | `KeyPadLive_PackState_Slot4` / `KeyPadLive_Ctor` |

若 Query 单例的 `klass` 是 **Live**，则左右由 **`[+0x28]` 的 bit0** 直接决定（even→+1，odd→−1），与 A/B/C Pack 无关。

#### 谁写 Live`+0x28`

| 符号 | RVA / VA | 角色 |
|---|---|---|
| `KeyPadLive_UpdateFromInput` | `0xC66070` / `0x7ffb17606070` | 大函数；写 `+0x28` / `+0x24`；内调 `KeyPad_Query` |
| `KeyPadLive_UpdatePump` | `0xC65B90` | 其调用方（metadata 挂接） |

`+0x28` 写点抽样（MBA 对拍）：

| 站点 | 语义 |
|---|---|
| `…078e9` | **`[+0x28] := 1`**（解码常量） |
| `…07797` / `…07de9` | **`[+0x28] += 1`**（两套 MBA，对拍为 +1） |

→ Live 状态更像 **计数/事件累加**，不是简单的「左=1/右=2」枚举；bit0 会随 +1 翻转。

#### 单例类型仍未静态钉死（F6 分叉）

| 假设 | 后果 |
|---|---|
| **A. 单例 = 基类**（`EnsureSingleton`→`object_new(g_KeyPad)`） | Slot4=`PackState`；改 `+0x28` 无效；需 `SetFields` 做出 odd/even pack（如 A≥64→odd） |
| **B. 单例 = Live**（或静态字段后被换成 Live） | Slot4=`return +0x28`；**写单例 `+0x28` 的 bit0** 即控左右 |

`EnsureSingleton` 静态上看是基类 `object_new`；Live 大量出现在 TypeDef **1337** 键位库 / `UpdateFromInput`。**必须 BIN**：断 `GetState` 的 `call [klass+0x178]`，看目标是 `0x16BEE60` 还是 `0xC92660`，并读单例 `klass`。

#### F6 控制面（本轮更新）

| 方案 | 静态地位 |
|---|---|
| D. `input_port` KeyDownTouch | 仍优先待 BIN（与 Update 链无直接 call 证据） |
| G. `SetFields` A/B/C | 仅当单例为**基类**时候选；需 odd/even pack |
| **H. 写 KeyPadLive 单例 `+0x28`** | 仅当单例为 **Live** 时首选；BIT0↔±1 |
| 验单例类型 | **下一刀 BIN 硬门禁** |

### 11.12 BIN LOG · KeyPad 单例 / Slot4（填空表）

> **目的**：一刀分叉基类 Pack vs Live`+0x28`，决定 F6 写 G 还是 H。  
> **模块**：`GameAssembly.dll`（RVA 下列；VA = `ImageBase + RVA`，ASLR 下用运行时基址）。  
> **前置**：进图、站平地、可走左右；x64dbg / IDA debugger 任选。

#### 0. 基址

| 项 | 填写 |
|---|---|
| 日期/时间 | |
| `GameAssembly` ImageBase | `0x________________` |
| 构建/客户端备注 | |

#### 1. 断点（只下这些）

| # | RVA | 符号 | 命中时看什么 |
|---|---|---|---|
| BP1 | `0x16BF3AC` | `KeyPad_GetState` 内 `call rax`（Slot4） | **`rax` = 调用目标** |
| BP2 | `0x11D0154` | `VecCtrlUser_WorkUpdate` 内 `KeyPad_Query`（X 路，注释 bit0→latch） | 调用后 **`eax`** |
| BP3 | `0x11D0159` | 紧接 `and eax,1` | **`eax&1`** → 随后 latch |
| BP4 | `0x11D0199` | `mov [r14+10h], ecx` 写锁存 X | **`ecx`**（应为 ±1） |

辅助（可选）：

| RVA | 用途 |
|---|---|
| `0x16BEE60` | 命中 = 基类 `PackState` |
| `0xC92660` | 命中 = `KeyPadLive` Slot4（`mov eax,[rcx+28h]; ret`） |
| `0x16BEE40` | `SetFields` 是否被本地路径打到 |

#### 2. BP1 判据（硬门禁）

在 BP1 停住时记录：

| 字段 | 怎么读 | 空闲 | 按右 | 按左 |
|---|---|---|---|---|
| `rax`（Slot4 目标） | 绝对地址 | | | |
| `rax - ImageBase` | RVA | | | |
| `rcx`（this/单例） | | | | |
| `[rcx]`（klass*） | | | | |
| `[rcx+0x10]` A | dword | | | |
| `[rcx+0x14]` B | | | | |
| `[rcx+0x18]` C | | | | |
| `[rcx+0x1C..24]` shadow | | | | |
| `[rcx+0x28]` Live 字段 | 若 access 违规 → **基类** | | | |

**RVA 对照**：

| `rax` RVA | 结论 | F6 下一刀 |
|---|---|---|
| `0x16BEE60` | **基类 PackState** | 方案 G：`SetFields` 造 odd/even pack |
| `0xC92660` | **KeyPadLive** | 方案 H：写单例 `+0x28` bit0 |
| 其它 | 记 RVA + 反汇编首 4 条 | 重开静态 |

#### 3. 极性链（BP2→BP4，每种键态采 1～2 帧）

| 键态 | Query 后 `eax` | `eax&1` | 锁存写入 `ecx` | `InputX@vc+50`（可选） |
|---|---|---|---|---|
| 空闲 | | | | |
| 按住 → | | | | |
| 按住 ← | | | | |

静态预期（若成立）：`even → latchX=+1`，`odd → latchX=−1`。

#### 4. 若 BP1 = PackState（基类）— 补采 A/B/C

空闲 / 右 / 左各一次 `GetFields` 或直接读 `[singleton+10/14/18]`：

| 键态 | A | B | C | 备注 |
|---|---|---|---|---|
| 空闲 | | | | |
| → | | | | |
| ← | | | | |

对照静态：`A=0` pack even；`A≥64` 常 odd。看实机是否用 Obf tag（`A\|0x100000` 等）。

#### 5. 若 BP1 = Live — 补采 `+0x28`

| 键态 | `[singleton+0x28]` | bit0 | 锁存 X |
|---|---|---|---|
| 空闲 | | | |
| → | | | |
| ← | | | |

静态旁证：`UpdateFromInput` 会 `:=1` 或 `+=1`（计数）；确认 bit0 是否稳定对应左右，而非每帧翻转。

#### 6. 桥接冒烟（可选，第二轮）

| 动作 | 断点/日志 | 期望 |
|---|---|---|
| `input_port` 注入 VK_RIGHT | BP1/BP4 + `pad_rb` | 与真按右同 Slot4 / 同 latch |
| 仅写 latch±1 | BP4 前再读 | 仍被 Query 盖掉（复现 06:16） |

#### 7. 结论区（勾一项）

- [ ] **BASE**：F6 → `KeyPad_SetFields(singleton, A,B,C)`，按 §4 表填编码  
- [ ] **LIVE**：F6 → 写 `*(uint*)(singleton+0x28)` 控 bit0  
- [ ] **OTHER**：Slot4=`______________`，暂停写码，回静态  

**本轮记录人 / 备注**：

```
（粘贴 BP1 rax RVA、三态 eax&1、最终勾选）
```

#### 8. 数据面自动 BIN（xcat.dll · 2026-08-01）

进图后 payload 每 500ms 写 `Dumps/runtime/keypad_bin.log`（`FLY_KEYPAD_BIN=0` 关闭）：

| 字段 | 含义 |
|---|---|
| `kind=BASE` | Slot4 RVA=`0x16BEE60` → 方案 G `SetFields` |
| `kind=LIVE` | Slot4 RVA=`0xC92660` → 方案 H 写 `+0x28` |
| `kind=OTHER` | 记下 `slot4_rva=` 回静态 |
| `keys=-/L/R` | 真键盘左右（请站平地按住采三态） |
| `A/B/C/+28` | 单例字段快照；`padX/inX/ma` 对照锁存 |

操作：注入 → 进图走几步 → 空闲/按→/按← 各停 1s → 交 `keypad_bin.log`。

#### 9. BIN 实锤 · 2026-08-01 06:42（`Dumps/runtime/keypad_bin.log`）

| 项 | 结果 |
|---|---|
| **kind** | **`BASE`（全程）** |
| **slot4_rva** | **`0x16BEE60`** = `KeyPad_PackState` |
| Live Slot4 | **未命中**（方案 H 写 `+0x28` **作废**） |
| 单例 | 稳定 `sing=…1480` / 同 `iklass` |
| `+28` | 恒 `1115123072`（基类无此字段，读的是邻接垃圾；bit0 恒 0） |
| A/B/C | **剧烈跳变**（含 `A=8192→33554560` 双 pack 形），= Query 每帧 `PackState` **破坏性改写 primary**，不能当「键位枚举」读 |
| 起步 A | `1048576=0x100000`（Obf tag 痕迹） |
| `keys=R/L` 段 | OS 见到左右箭头；**`padX=0 inX=0` 全程** |
| `ma` | 多数 `0`；仅 `fly=1` 短暂见 Stand/Jump |
| 用户反馈 | **进图确有真实移动**（与 A/B/C 被 Pack 持续改写一致） |

**结论勾选：BASE。** 方案 H（`+0x28`）作废。

**`padX=0` 不否定走路**：探针在 fly worker 每 500ms 异步采；锁存/`InputX` 只在 `WorkUpdate` 帧内短暂非零（Query→SetInput→CalcWalk，帧末可被清），异步几乎永远撞到 0。  
真走动证据应看 **ApX 位移 / ApVx**（探针已补 `dAp`/`vx`），或 MainPump 同步采锁存。

F6 控左右仍优先：G.`SetFields`（每帧 Query 前）或 D.`input_port`；不要写 `+0x28`。

#### 10. BIN 复采 · 2026-08-01 06:57（含 `dAp`/`vx`，同路径）

| 项 | 结果 |
|---|---|
| 探针版本 | 有 `note: padX/inX mid-frame…` + `ApX/dAp/vx` ✅ |
| kind | **BASE ×82**（无 LIVE） |
| padX/inX | 仍全程 0（异步预期内） |
| `keys=R` ×4 | `ApX=0 dAp=0` — 按箭头时 **未见走路位移** |
| `fly=1` 段 | `dAp` 达 +52 / +181 / −34 / +119；`ma=Stand/Jump` — **飞天在动 Ap** |
| `vx` | 全程 0（含飞天位移段）— 飞天写 Ap、未走 CalcWalk 速度场，或采样撞空窗 |
| `+28` | 本局恒 `1`（bit0=1）；仍非 Live 字段语义 |

**解读**：BASE 再钉死。本局「真位移」只出现在 F6 开飞，不在手按左右；手按左右未采到 `dAp≠0`，不能用来反推 L/R 的 A/B/C 编码。

#### 10. 方案 G 代码落地 · 2026-08-01

`fly_impl` 驱动默认改为 **SetFields 数据面**（写 primary+shadow A/B/C）：

| dir | A | 预期 |
|---|---|---|
| +1 右 | `0` | even → latchX=+1 |
| −1 左 | `64` | odd → latchX=−1 |
| 0 停 | 不写 | 避免空闲 even 强制右走 |

时机：worker tick + MainPump `SetFrameTick`。默认 `flyDriveInput=1` / `flyTeleportHop=0`。反了 → `FLY_KP_FLIP=1`。看 `fly.log`：`driveG ... pad_rb=` / `v=`。

#### 10.1 BIN 误跑短跳 · 2026-08-01 06:58

用户以为测 G，实际面板/IPC 把 **短跳 hop 打开**：

| 证据 | 值 |
|---|---|
| `fly.log` | `FLY_TELEPORT_HOP set=1` → `F6 teleport-hop ON`（**无** `scheme G`） |
| `movepath_elems` | 全程 `drive=0`；`Teleport(4)` + Jump `v=(0,-60)` 空中 fh=0 |
| 时长 | F6@06:58:32 → soft disc@06:58:34.5（~2s） |
| `kick.log` | `lean_local_or_soft` / `pendingError=205`，无 op=432 |

= 与先前 Attr=4 短跳踢线同构；**方案 G 仍未采到有效样本**。下次必须确认：短跳关、驱动开、log 出现 `F6 drive-input ON … scheme G`。

---

