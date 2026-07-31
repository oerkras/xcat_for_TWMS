# MoveElem 字段拆解

> **产品**：新楓之谷：經典版（TW / beanfun）  
> **日期**：2026-07-30  
> **证据源**：`Dumps/runtime/out/dump.cs` ↔ `Dumps/cms_cw/dump.cs`；IDA `MoveElem_Reset`  
> **工作笔记**：[`Dumps/move_elem_notes.md`](../../../Dumps/move_elem_notes.md)

---

## 1. 身份对照

| | TW（runtime dump） | CMS Rosetta |
|---|---|---|
| 类 | `f35f57ffc7406ea3…` TypeDef **1588** | `MoveElem` TypeDef 1481 |
| Attr 枚举 | `f04118ab33ae6583…` TypeDef **1587** | `MovePathType` TypeDef 1480 |
| 容器 | `MovePath.Elem` @ `+0x30`（`List<MoveElem>`） | 同 |
| 上一片 | `MovePath._elemLast` @ `+0x38` | 同 |

字段类型与偏移与 CMS **逐字节一致**（Il2Cpp 对象头后从 `0x10` 起）。

别名表：`Dumps/sdk_aliases.tsv`、`Dumps/cms_cw/restore_class_map.tsv`。

---

## 2. 对象布局（实例）

```text
+0x00  Il2CppObject (klass / monitor)
+0x10  byte   Attr          // MovePathType
+0x12  short  X
+0x14  short  Y
+0x16  short  Vx
+0x18  short  Vy
+0x1A  byte   MoveAction    // 朝向/动作编码
+0x1C  short  Fh            // 当前 foothold id
+0x1E  short  FhFallStart   // 下落起点 foothold
+0x20  short  Elapse        // 本段耗时 (ms)
+0x22  byte   Stat          // SecondaryStat 变更标记等
```

对齐后实例尺寸约 **0x28**（含尾部 padding）。

`MoveElem_Reset` 按同一偏移清零（`+0x10` Attr、`+0x12` 起 8 字节 XY/VxVy、`+0x1A` MoveAction、`+0x1C` Fh 对、`+0x1F` 起覆盖 Elapse/Stat），与 dump 一致。

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

飞天相关优先看 **16 / 17（StartWings / Wings）**。

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
| `Flush` | `0x1198DC0` | `MovePath_Flush` → Encode |

Encode / Decode **按 Attr 分支**（壳 + 间接跳），静态 F5 难读干净。

- **已实锤**：对象字段序与类型  
- **未钉**：每种 Attr 的 wire 省略规则 → 建议 iDbg hook `MovePath_Encode` / `Flush`，走路 · 跳 · 飞各采 hex 再表驱动还原  

常见 Absolute 类片段（行业惯例，**待本客户端 hex 复核**）：

```text
byte  Attr
short X, Y
short Vx, Vy      // 部分 Attr 才有
short Fh
byte  MoveAction
short Elapse
// StatChange 等再跟 Stat / 额外 foothold
```

---

## 6. IDA 跳转

- `G` → `MoveElem_ctor` / `MoveElem_Reset` / `MoveElem_Clone`
- `G` → `MovePath_Encode` / `MovePath_ParseMove_private` / `MovePath_Flush` / `MovePath_OnMovePacket`
- Recv 入口见 [移动协议.md](./移动协议.md)

---

## 7. 验证状态

| 项 | 状态 | 证据 |
|---|---|---|
| TW↔CMS 字段偏移 | 已完成 | dump.cs TypeDef 1588 ↔ 1481 |
| Reset 清零序 | 已完成 | IDA `MoveElem_Reset` |
| MovePathType 0..22 | 已完成 | TW enum 1587 ↔ CMS `MovePathType` |
| wire 按 Attr 表 | **NOT RUN** | Encode 壳；需 hook 采包 |
