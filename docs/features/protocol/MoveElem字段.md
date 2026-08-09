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
| wire 按 Attr 表 | **部分 PASS**（Attr 0 / 2，2026-08-07） | `send.log` 实包，见 §12 |
| Flush 本地 `Δpos≈v·el` | **已否定** | IDA 无 `3E8h`；矛盾包仍发 |
| 出站包对 `Δpos≈v·el` 自洽 | **PASS**（2026-08-07） | `send.log` 1122 个绝对段：X 残差 ≤1.4px；Y 残差恒等于 ½gt²（30/60/90/120ms 四点吻合），见 §12.3 |
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

> **⚠️ 结论更正 · 2026-08-07 实机定案（先读这段再往下看）**
>
> 本章 §11.6～§11.12 的调查方向整体偏了一层。真源是 `UnityEngine.InputSystem` 的 `Keyboard` 设备；
> KeyPad / latch / `SetInput` 全在它下游，被上游「有没有按键」那道门闩管着——门不开，`CalcWalk` 根本不跑。
>
> **门闩吃「状态变化」，不吃「当前状态」**（2026-08-07 IDB 逐项查证 + 实机数据双向确认）。
> 游戏代码段（`0x7ff849xxxxxx`）对所有轮询式输入 API 的调用次数：
>
> | API | RVA | 游戏代码调用方 |
> |---|---|---|
> | `Keyboard.get_current` | `0x4755120` | 无（仅 MethodInfo 数据引用） |
> | `InputAction.IsPressed` / `WasPressedThisFrame` | `0x469D330` / `0x469D490` | 无（仅 InputSystem 包自身） |
> | `InputAction.ReadValue<float/int>` | `0x210B3D0` / `0x210B1F0` | 无 |
> | `InputAction.get_triggered` / `get_phase` | `0x469C3C0` / `0x469C070` | 无 |
> | `Input.GetAxis` / `GetAxisRaw` / `GetKeyInt` | `0x4F13480` / `0x4F13600` / `0x4F13900` | 无 |
> | `Input.GetKey(KeyCode)` | `0x4F13BC0` | **仅 1 处**：`UserLocal.IsSit()` @ `0x1080BA0` |
>
> 那唯一一处旧版调用只管起立，与走位无关；三个 KeyCode 被常量混淆，实读种子表 `0x7FF84F4D3940` 手算：
> `0x1DBDC3E4+0xE2423D2D=0x1_00000111`→273 上、`0xF7FCD8EB+0x08032829=0x1_00000114`→276 左、
> `0x27C18122+0xD83E7FF1=0x1_00000113`→275 右。
>
> 一处都不轮询，配上运行时实测（纯事件注入 71.4% 能走 / 直写状态缓冲降到 10.2%），
> 结论是消费方式为**变化驱动**（change monitor / `InputAction` 回调），只认状态发生了改变。
> 这一条同时解释了「前台一顿一顿」和「直写反而更差」两个现象，见下方坑 4。
>
> 三条已实机证伪的路径，别再试：
>
> | 路径 | 实机结果 |
> |---|---|
> | 写 `KeyPad.SetFields`(A/B/C) | 无位移 |
> | vtable 钩「`KeyPad.PackState`」(Slot4) 改返回值 `bit0` | **该测试无效**，见下 |
> | 直写 `VecCtrl.InputX@+0x50` 或锁存 `+0x100+0x10` | `inX`/`ma` 确实写进去了，`vx` 恒 `0.00` |
>
> ⚠️ **第二条是伪实验，结论不成立**（2026-08-07 更正）。`kOffKeyPadSlot4`（`klass+0x178`）所指的类
> **不是** KeyPad 而是 `Rand32`，slot 4 = `Rand32.Random()`。四条独立证据一致：
> ① 运行期自校验日志 `origRva=0x16C9170 expect=0x16C9170 match=1`，地址是真的；
> ② 该 RVA 在 dump 里就是 `public virtual uint Random()`；
> ③ 反编译为 xorshift（三状态字 `a1[4..6]`、移位 13/19 · 4/25 · 8/11，XOR 全被展开成
> `(a|~b)+(b|~a)-2*~(a|b)-2*(a&b)` 的混淆形式）；
> ④ `Random()` 正是 `Rand32` 的首个自有虚方法，恰好落在 slot 4（前四槽属 `Object`）。
> 所以当初是在给一个伪随机数翻位，从未碰到真的 PackState。连带后果：BIN 里 `packRet` / `qbit` / `hits`
> 三列全是随机噪声，且 `hits>0` 几乎恒真，把 BIN 的空闲节流整体旁通。钩子与这三列已拆除；
> `attack_input_port` 里那条会**写** `bit0` 的 PackBit 驱动路（等于污染游戏 RNG 流）已默认拒装。
>
> 第三条的 BIN 早就给了答案，只是当时读反了：我们写的 `inX=1 ma=2` **一直挂着没被清零**，
> 说明 `WorkUpdate` 的输入分支压根没进来——既没人覆盖我们，也没人算速度。
> 所以 §11.6 表格里「被同帧 `SetInput(锁存)` 盖掉」这个判断方向对、机制错：真问题是**门闩没开**。
>
> **正解**：构造 `StateEvent` 灌进 Keyboard 设备，见 `x/features/ports/unity_kbd_port.cpp`。  
> **模块设计（Hold / 自管 Repush / 外部用法）**：[`../unity_kbd/模块设计.md`](../unity_kbd/模块设计.md)。  
> 整条链会当成「真的按住了方向键」来跑，且绕开 Raw Input，**失焦照样生效**。
>
> 实机验证（02:14 BIN）：`walkW Hold mode=Kbd kbd=1 vk=0x00 fg=0` 持续位移，
> `human_no_move` 0 次；尾段 1496 个采样里 1079 个 `vx≠0`，峰值 `|vx|=1270.97`。
>
> #### 真源链路
>
> ```
> InputSystem.QueueEvent(StateEvent 'STAT' + 'KEYS')
>   → Keyboard.IEventPreProcessor.PreProcessEvent   ← 每个事件的必经收口点（守位钩子在这里）
>   → Keyboard 设备状态位图（leftArrow=bit61 / rightArrow=bit62）
>   → 状态变化监视器 / InputAction 回调   ← 门闩在这里（吃变化，不吃当前值）
>   → latch (vc+0x100+0x10)
>   → VecCtrl.SetInput → OnResolveMoveAction → ma
>   → CalcWalk → vx → ApX
> ```
>
> | 项 | 值（运行期 dump · remount 2026-08-06） |
> |---|---|
> | `InputSystem.QueueEvent(InputEventPtr)` | RVA `0x46F9C30` |
> | `Keyboard.get_current()` | RVA `0x4755120` |
> | `InputSystem.get_settings()` | RVA `0x46FA2E0` |
> | `InputSettings.set_backgroundBehavior` | RVA `0x47786B0` |
> | `InputSystem.EnableDevice(InputDevice)` | RVA `0x46F8920` |
> | `InputDevice.m_DeviceId` | `+0xE4` |
> | `InputDevice.m_LastUpdateTimeInternal` | `+0x130` |
> | `StateEvent` 布局 | `baseEvent@0x00`（20B）· `stateFormat@0x14` · `stateData@0x18` |
> | `KeyboardState` | fmt `'KEYS'` · 16B 位图 · **`Key` 枚举值 == 位号** |
> | `Keyboard.IEventPreProcessor.PreProcessEvent` | RVA `0x4757020` |
>
> `PreProcessEvent` 反编译（VA `0x7FF84D3D7020`）顺带交叉验证了上面两行布局：
> 它判 `type=='STAT'`（`1398030676`）→ 判 `*(u32*)(ev+0x14)=='KEYS'` → 拿 `ev+0x18` 当位图
> **原地改写**（Unity 自己把 bit111 挪到 bit127），恒返回 1。偏移与 `StateEvent` 表一致，
> 且证明「在这里改事件位图」是 Unity 认可的用法。
>
> 四个必踩的坑：
>
> 1. **时间戳**：`InputManager` 会丢弃早于 `device.m_LastUpdateTimeInternal` 的乱序状态事件。
>    取「设备上次更新时间 + ε」并保证严格递增——既不判乱序，也不会落到未来被推迟到下一帧。
> 2. **失焦保活**：默认 `backgroundBehavior` 会在失焦时 Reset 并禁用键盘设备，注入整个被丢弃；
>    必须先置 `IgnoreFocus`，后台走位才成立。
> 3. **别空发**：状态事件是整块覆盖，手里没按键时入队等于把玩家真按住的键抹掉一帧。
> 4. **前台卡顿的真身是「取消」信号，不是「抢写」竞争**：这条是本页最贵的一课，
>    前后走错两次方向才定案。
>
>    窗口在**前台**时，Unity 原生输入后端每帧都会投一条真实键盘事件（内容是「方向键=未按」，
>    实测约 31/s）。既然门闩吃的是**状态变化**，每条这种事件都会触发一次 `canceled`，
>    游戏就停一步；我们下一个事件再触发 `performed`，又走一步——所以表现是**一顿一顿**，
>    不是变慢。**先后两次错判都源于把它当成「谁最后落笔谁赢」的抢写竞争**：
>    提高补写频率（占空比 33%→54%）是在跟一个「取消」信号抢，抢不到；直写状态缓冲更糟，见下。
>
>    **正解：在 `Keyboard.PreProcessEvent`（RVA `0x4757020`）里把自己持有的方向键位 OR 回事件位图。**
>    它是每个键盘事件落到设备前的必经收口点，改完外来事件就不再携带「未按」，
>    取消信号根本不会产生，卡顿从根上消失。只碰自己持有的那几位，玩家真按的其他键一律不动；
>    松手时掩码清空、钩子自动停手，清零事件正常放行去触发 `canceled`，角色照常停下。
>
>    实现要点：该方法是**显式接口实现**，IDB 里只有数据引用、没有代码引用，派发完全走运行期 vtable。
>    硬编码槽位换个构建就错，改为在 `Il2CppClass` 对象内**按已知原函数地址逐 qword 匹配**并改写
>    （见 `unity_kbd_port.cpp::InstallEventGuard`）——自带正确性校验，也不依赖结构体布局。
>    `XCAT_KBD_GUARD=0` 可关。只处理 `'STAT'`：原函数自身也只认 `'STAT'`；
>    若日后发现 `guard/s` 压不住卡顿，`DeltaStateEvent`（`stateOffset@0x1C` / `data@0x20`）是第一嫌疑。
>
>    **量化前必须先按 `keys=` 拆样本。** `keypad_walk_bin`（**默认关**；排障设
>    `XCAT_WALK_BIN=1`，产物 `logs/keypad_walk_bin.log` 体积大、日常勿开；上传器亦跳过该频道）
>    的 `keys=` 列走的是 `GetAsyncKeyState(VK_LEFT/VK_RIGHT)`，读的是 **OS 物理按键**，
>    和内部注入毫无关系。
>    调试时人手按一下方向键，那几秒的 `vx≠0` 会被算进「内部注入的成绩」，
>    直接把结论带偏一个数量级。只有 `keys=-` 的样本才是纯内部注入。
>
>    按 `keys=-` 统计的实测：
>
>    | 做法 | 纯内部注入样本 | `vx≠0` | `inX` 有效但 `vx=0` |
>    |---|---|---|---|
>    | 每帧 `QueueEvent` 补写 | 1170 | **71.4%** | 4.7% |
>    | 每帧直写前台状态缓冲 | 196 | **10.2%** | 39% |
>
>    **直写是死路，别再试第二遍。** 思路本身看着无懈可击：
>    `InputControl.get_currentStatePtr()`（RVA `0x46FD900`，内部转
>    `InputStateBuffers.GetFrontBufferForDevice`）拿到设备前台状态缓冲基址，加设备
>    `+0x14`（`m_StateBlock@0x10` + `m_ByteOffset@0x04`）得到 16B 位图首地址，
>    在 `WM.FixedUpdate` orig 之前改位——此刻事件队列早已在 `EarlyUpdate` 抽干，
>    我们是本帧最后落笔的人，稳赢排队竞争。**但实测反而更差。**
>    原因是游戏那道门闩吃的不是「位图当前值」而是**状态变化**（只有事件路径才会触发的
>    change monitor / `InputAction`）：直写把位提前设成 1，随后到达的事件因「状态没变」
>    而不再触发门闩，等于自断触发。代码保留在 `XCAT_KBD_DIRECT=1` 开关后面仅供复验，
>    **默认必须关**。
>
>    帧槽要用 `main_thread::SetInputFrameTick`，**别用 `SetPrePhysicsFrameTick`**：
>    后者宿主是 `WM.FixedUpdate` 钩子，钩子没挂上或 idle 回落时整个槽静默，
>    补写毫无征兆停摆，而日志里「注册成功」照样打印。`SetInputFrameTick` 首选同一相位、
>    宿主失活时自动回落 `SendWill` 渲染帧。
>
>    自证：`unity_kbd::Stats()` 给出 `pushes` / `clobbers` / `directs` / `guards` / `hookCalls`，
>    `walkW Hold` 行按 `grd=<钩子是否装上> tick=h<宿主>@<次/秒> dw= push= clob= guard= hc=` 打出速率。
>    读法：
>
>    | 字段 | 前台应有 | 失焦应有 | 不对时说明 |
>    |---|---|---|---|
>    | `grd` | `1` | `1` | `0` = vtable 槽没匹配上，卡顿修复未生效 |
>    | `hc/s` | ≈2×`push`（128~172） | ≈`push`（70~103） | 为 0 = 钩子压根没被调到（见下「两种 guard=0」） |
>    | `guard/s` | ≈`hc`−`push`（68~94） | ≈0 | `hc>0` 而此项为 0 = 被调到但没改写，多半事件结构认错 |
>    | `dw/s` | `0` | `0` | 非 0 说明误开了 `XCAT_KBD_DIRECT` |
>    | `push/s` | >0 | >0 | 为 0 = 帧槽没跑（只注册成功不算数） |
>
>    `guard` 计的是**真正改写过位的次数**，等价于「被拦下的 `canceled` 数」，
>    所以它前台≈外来事件率、失焦≈0 才是正常特征。
>
>    **别拿 `clob` 当外来事件率的标尺**：`clob` 只在每次推送时比一次设备时间戳，
>    每次推送最多计 1 次，天然低估。实测前台 `clob≈30/s` 而 `guard≈83/s`，
>    差的不是 bug 而是采样口径 —— 逐事件计数的 `guard` 才准，对照式取 `guard ≈ hc − push`。
>
>    **两种 `guard=0` 必须分开**（实测踩过：`grd=1` 且槽地址校验无误，但 `guard` 恒 0）：
>
>    - `hc=0` → 钩子没进来。两个已知成因：
>      ① `InputManager` 调 pre-processor 前先看设备标志位
>      `InputDevice.DeviceFlags.HasEventPreProcessor`（`0x4000`），这位为 0 则永不调用。
>      用 `get_hasEventPreProcessor`（RVA `0x4709030`）实读、`set_hasEventPreProcessor`
>      （RVA `0x4709040`）置位；`hasEventPreProcessor before=/after=` 日志给出实测值。
>      **本仓实测 `before=1`，即标志位本来就开着，不是此因。**
>      ② 改错了槽 —— 见下「vtable 槽必须按公式算」。**本仓实测就是此因。**
>    - `hc>0` → 进来了但没改写。见下「事件指针按值传」。

### 接口派发：vtable 槽必须按公式算，不能扫描

`InputManager.OnUpdate`（RVA `0x4773560`）里调 `PreProcessEvent` 的派发序列，是本节所有
偏移的唯一来源：

```asm
call sub_…389030                 ; get_hasEventPreProcessor(device)
test al, al
jz   …                           ; 标志位为 0 → 整段 pre-process 跳过
…
mov   rax, [r12]                 ; klass 取自「设备实例」，不是 FindClass 的结果
movzx ecx, word ptr [rax+12Eh]   ; interface_offsets_count (uint16)
mov   r8,  [rax+0B0h]            ; Il2CppRuntimeInterfaceOffsetPair*（每项 16B）
cmp   [r8-8], rdx                ; 逐项比 interfaceType == IEventPreProcessor
movsxd rcx, dword ptr [r8]       ; 命中项 +8 处的 int32 offset
shl   rcx, 4                     ; × sizeof(VirtualInvokeData) = 16
add   rax, rcx
add   rax, 138h                  ; klass->vtable 基址
mov   r8,  [rax+8]               ; VirtualInvokeData.method
mov   rdx, r15                   ; eventPtr —— 单寄存器
call  qword ptr [rax]            ; VirtualInvokeData.methodPtr ← 要改的就是这一格
test  al, al
jz    …                          ; 返回 false ⇒ 该事件被整条丢弃
```

由此定下三条：

| 事实 | 后果 |
|---|---|
| 槽地址 = `deviceKlass + 0x138 + interfaceOffset*16` | 必须按此式算；`IEventPreProcessor` 只有一个方法，接口内槽位恒 0 |
| klass 取自实例 `*(void**)dev` | 不能用 `FindClass` 的类对象顶替 |
| `eventPtr` 走单寄存器 | `InputEventPtr` **按值传**，拿到的直接是 `InputEvent*`，不必再解一层 |

> **踩坑（务必别重犯）**：初版图省事，在类对象里开 8KB 窗口「逐 qword 找等于
> `PreProcessEvent` 地址的格子，命中即改」，并自认为「扫到就等于校验通过」。实测
> `slots=1`、`orig` 减映像基址正好等于 RVA `0x4757020`，看着毫无破绽 —— 但 `hc` 恒 0，
> 钩子一次都没进。**同一个函数地址在类对象里会出现在不止一处，扫到的那格未必是派发读的那格。**
> 地址值对 ≠ 位置对。凡是运行期按表派发的东西，一律按派发器自己的公式算地址，
> 并且**写完回读**确认落地（`PatchVtableMethodPtr` 返回 true 也不代表写进去了）。
>
> 改成按公式算之后，`guard resolve` 一行把两处病灶同时照出来（某次实测值）：
>
> ```
> devKlass=…51892640  findKlass=…51892910  slot=…518928F8  off=696(0x2B8)  match=1
> ```
>
> ① `FindClass("UnityEngine.InputSystem","Keyboard")` 返回的类对象与设备实例的
> `*(void**)dev` **不是同一个**（差 `0x2D0`）—— 所以必须用实例的 klass；
> ② 真正的接口槽在 `devKlass+0x2B8`，**比旧扫描起点 `findKlass` 还靠前 `0x18` 字节**，
> 旧代码从物理上就扫不到它，扫到的是同一函数在 vtable 里的类级槽位（接口派发不读）。
> 两处叠加，才造出「一切校验都过、钩子却一次不进」的假象。

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
| 只写 `InputX@+0x50`（旧现行） | **否（实机已证）** | 门闩未开 → 输入分支不进，`vx` 恒 0 |
| 写锁存 `*(vc+0x100)+0x10` | **否（实机已证）** | 同上，仍在门闩下游 |
| MainPump 在 `SetInput` 之后写 `+0x50` | 无意义 | 同帧 `CalcWalk` 不跑，插进去也没人算 |
| `input_port` 注入左右键（`UserLocal.OnKey`） | 否 | 走的不是这条；真源是 Keyboard 设备的状态**变化** |
| hook `SetInput` 注入方向 | 可行但碰 `.text` | GRAP 默认禁止 |
| **灌 Keyboard 设备状态** | **是（2026-08-07 实机）** | 唯一真源，见本章开头结论更正 |

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

> **已作废（2026-08-07）**：下面「`bit0` → `latchX` 极性」这条推断实机不成立。
> vtable 钩 Slot4 强改返回值 `bit0`，`hook=1` 但 `travel=0` / `vx=0`。
> 本节保留仅作代数拆解参考，**不要**据此写驱动代码；真源见本章开头的结论更正。

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

> **已结案（2026-08-07）**：下列三步不必再做。答案是第 2 条的方向——
> 「真正写锁存前的键采样」来自 InputSystem `Keyboard` 设备的状态**变化**
> （change monitor / `InputAction` 回调，非轮询；本章开头已列出零轮询的实证表），
> 落地见本章开头结论更正与 `x/features/ports/unity_kbd_port.cpp`。

1. ~~运行时看 `GetState` @ `call [rax]` 实际落入哪；对比按左/右/空闲返回值。~~  
2. ~~若虚体非 Pack：回到 **D.`input_port`** 或找真正写锁存前的键采样函数。~~  
3. ~~若虚体是 Pack：用 GetFields 在实机按键时采 A/B/C，再反推左右。~~

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
| `kick.log` | `lean_local_or_soft` / sticky `pendingError=205`（哨兵），无 op=432 |

= 与先前 Attr=4 短跳踢线同构；**方案 G 仍未采到有效样本**。下次必须确认：短跳关、驱动开、log 出现 `F6 drive-input ON … scheme G`。

---

## 12. wire 布局实证：Attr 0 / 2（2026-08-07 · send.log 实包）

§5 长期挂着「wire 按 Attr 表 = NOT RUN」。旋翼贴怪期的 `send.log`（BIN 491473，145 个
`op=47`、580 个元素）把其中两种钉死了。

**每个元素的首字节就是 `Attr`，并据它决定后续布局**（与行业惯例一致）：

| Attr | 布局 | 长度 | 字段序 |
|---|---|---|---|
| **0 Normal** | 绝对 | 14B | `attr｜x(2) y(2) vx(2) vy(2) fh(2) ma(1) el(2)` |
| **2 Impact** | 相对 | 8B | `attr｜vx(2) vy(2) ma(1) el(2)` |

**证明不是「解析看着通顺」，而是值域对撞**：Attr=2 元素携带的 `|vx|` 共 74 个不同取值，
其中 **66 个**能在同一时段 `combat.log` 的旋翼命令 `desx=` 里找到同值。那是我们自己下的
冲量目标，值域辨识度极高；若首字节不是 Attr、或 Attr=2 不走 8B 相对布局，解析必然错位、
读出来只会是噪声。脚本：`Dumps/runtime/_attr2_check.py`。

### 12.1 Impact 滑翔在 wire 上长什么样

| 元素 | 占比 | 特征 | 含义 |
|---|---|---|---|
| `Attr=2 Impact` | 238/580 = **41%** | `el=0`、`ma=6/7`、`vx/vy` = 冲量目标 | 每发一次冲量吐一个，零时长的速度突变 |
| `Attr=0 Normal` | 342/580 = **59%** | `fh=0`、`ma=6/7`、`el` 中位 30ms | 两次冲量之间的惯性滑翔 |

`ma=6/7` + `fh=0` 正是 §10.1 列的**合法空中形状**，`fh` 全 0 也与 §8.2 的「基线走跳里
`fh=0` 数十次属正常」一致。也就是说：**客户端如实上报了每一次 Impact，包形没有任何畸形。**

> ⚠️ 别拿 §3 那句「现行贴板锁路径实测全是 `Normal(0)`」套到 Impact 滑翔上——那说的是
> **贴板锁**（§8.2 的本地幻影，元素冻结不动）。两条路径的 wire 形状完全不同。
> 同理，02:24 那批 `movepath_flush.log` 里 Attr=2 只占 1.5%~4.5%，那是**瞬移期**（`tel=1`）
> 的采样，不能用来判断旋翼期。看 Attr 分布前先确认样本取自哪条路径。

### 12.2 对断线排查的意义

包形合法 ⇒ 掉线不可能是「畸形位移包」这一类。真要在这条线上找原因，剩下的是**行为特征**
而非**包结构**：真人挨击退是偶发，我们是 2~3 次/秒持续不断。这是频度问题，不是格式问题。

同一份数据里另有一项待查：144 次包间跳跃里有 **10 次**超出 `|vx|·Δt+60` 的粗略容差
（最大 `Δx=145` / 容差 100）。该容差是自拟的、不是服端口径，**不能**据此下结论，
仅登记为下一步方向。→ 已在 §12.3 用正确尺子结案。

### 12.3 `Δpos≈v·el` 自洽性实测：X 完美，Y 的偏差恰好是 ½gt²

§12.2 那个「包间跳跃」容差是错的尺子：`kDumpBodyBytes=64` 只留下每包前 3~4 个元素，
拿**中途落点**去比**下一包起点**必然失真。正确的量法是**同包内相邻的绝对元素**——
截断不影响它们之间的关系，而它们恰好就是 §8 所说服端契约面的最小单位。

绝对元素的 `(x,y)` 是该段**结束**坐标，`(vx,vy)` 是段速度，`el` 是段时长；
夹在中间的 Impact 相对元素 `el=0`，位移贡献为 0。故应有
`x_i = x_{i−1} + vx_i·el_i/1000`。1122 段实测（`Dumps/runtime/_selfconsist.py`）：

| 轴 | 中位 | p90 | p99 | 最大 |
|---|---|---|---|---|
| X 残差 | 0.3px | 0.8px | 1.0px | 1.4px |
| Y 残差 | 3.1px | 8.1px | 14.6px | 22.0px |

X 只有整数量化噪声。Y 偏差**恒为负**（实际落点低于推算），因为元素报的是**段末速度**
而非段内平均速度，缺的正是重力二次项：

| `el` | 实测 Y 残差中位 | ½·2000·t² | 样本 |
|---|---|---|---|
| 30ms | −1.1 | −0.9 | 501 |
| 60ms | −3.8 | −3.6 | 449 |
| 90ms | −8.3 | −8.1 | 143 |
| 120ms | −14.5 | −14.4 | 27 |

跨 4 倍时长吻合，`g=2000 px/s²` 与飞控侧实测一致。**结论：出站包完全自洽**；
Y 的那点残差是所有空中段（含真人跳跃、正常坠落）共有的编码惯例，服端若查此式必须容忍它，
因而它不构成踢人依据。这条曾是「位移踢」最合理的机制假设，至此证伪。

---

