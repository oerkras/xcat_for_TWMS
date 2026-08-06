# teleport P0a · 瞬移 CALL 锚点与路径研究（2026-08-01）

> **状态**：✅ 产品主路径 = 手填 `Teleport` + `Doing`（短距/长距；无 Register）；引擎自同步；BIN 短距连跳未踢  
> **⚠️ 术语澄清**：「不启用法师线」指的是**只关 `TryRegisterTeleport`**（技能包那一段）。
> **`TryDoingTeleport` 一直在产品路径里用**。见 §0 表与 [`模块设计.md`](模块设计.md) §0.1。  
> **产品**：新枫之谷：经典版（TW · `Maplestory_Classic.exe`）· **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase `0x7FFB16B40000`  
> **实现**：`x/features/ports/teleport_port.*` · 调用方：`simple_combat` / `fly`（hop）/ 面板测试按钮  
> **对照**：CMS `Dumps/cms_cw/dump.cs`（签名可读）；枫星仓仅作工程对照，**不复用偏移**  
> **安全**：禁止 Flush E9 / 改 GA `.text`；遵守 [`../security/GRAP与枫星对齐.md`](../security/GRAP与枫星对齐.md)

---

## 0. 结论先看

| 路径 | 用途 | 产品决策 |
|------|------|----------|
| **手填 `Teleport@0x3C8` + 挂 `CurFh` + `TryDoingTeleport`**（`TeleportNativeSkillCall`） | 绝对落点；官方 helper 负责 Attr / 冲击 / 特效 / 收态；本仓另补 `Apl←Ap` | ★ **唯一现役产品路径**（`simple_combat` 贴怪 / `fly` F6 / `travel` 贴门 / 面板测试） |
| ~~`ImpactBlinkTo`~~ | Mp + Attr=4 + ForcedFlush + SetImpactNext | **已拆除**（2026-08-02）；勿再引入 |
| ~~`TeleportTo`（Attr=4 旁路）~~ | 旧 F6 hop | **已拆除**；F6 现走 `TeleportNativeSkillCall(..., snapStand=false)` |
| ~~法师 `TryRegisterTeleport`~~ | 技能包 | **已拆除**；产品不发瞬移技能包 |
| ~~`RestoreWalkable` / `SyncRelPosOnly` settle~~ | 旁路收尾 | **已拆除**；Settling 禁止再 SyncRel |

BIN（2026-08-01）：Attr 远距一跳可达（例 hop≈1435）；冷静间隔 + Restore 后可走；狂点易软断（`Disconnected` + `lean_local_or_soft`；sticky `pendingError=205`=哨兵≠踢因）。怪仍能打到角色 ⇒ **服务器认坐标**，非纯视觉。

---

## 1. 原生符号与 RVA（TW）

基址 `0x7FFB16B40000`。VA = base + RVA。

| 符号（IDA 已命名） | VA | RVA | 作用 |
|--------------------|-----|-----|------|
| `UserLocal_TryRegisterTeleport` | `0x7FFB17BC04A0` | **`0x10804A0`** | 登记/发起瞬移；可发技能包 |
| `UserLocal_TryDoingTeleport` | `0x7FFB17B5CF30` | **`0x101CF30`** | 落地执行（帧循环调用） |
| `UserLocal_IsTeleportSkillAvailable` | `0x7FFB17C05250` | `0x10C5250` | 落点合法性（CFA/MBA 混淆） |
| `UserLocal_ToggleTeleport` | `0x7FFB17BB8900` | `0x1078900` | 键位开关相关 |
| `UserLocal_TryRegTeleport_caller_*` | `0x7FFB17BBA4C0` | `0x107A4C0` | Register 另一调用壳 |
| `VecCtrl_SetMovePathAttribute` | `0x7FFB17CE1920` | **`0x11A1920`** | Attr 写入（旁路与原生共用） |
| `UserLocal_SendSkillUseRequest` | （Register callee） | — | 技能使用包 |

### 1.1 视觉层同步链锚点（2026-08-02 补录）

位移之后「皮怎么被搬过去」这条链的锚点，机制与还原过程见 [`P0c_视觉层同步链.md`](P0c_视觉层同步链.md)。

| 符号 | VA | RVA | 作用 |
|---|---|---|---|
| `VecCtrl.GetPos()` | `0x7FFB17CB6CC0` | **`0x1176CC0`** | **视觉坐标真源**：`round(lerp(Ap, Apl, alpha))` |
| `VecCtrl.BeginUpdateActive()` Slot 10 | `0x7FFB17CE41E0` | **`0x11A41E0`** | 每逻辑帧 `Apl ← Ap`（32 字节整拷） |
| `VecCtrl.SetFirstFoothold(fh, pos)` | `0x7FFB17CF4D80` | **`0x11B4D80`** | 官方硬钉：`CurFootHold=fh` + 直写 TRS（不碰 Ap/Apl） |
| `FieldActorBase.<Slot 16>` 视觉同步 | `0x7FFB17CD7330` | **`0x1197330`** | **唯一写 actor TRS 处**；三道门控 + 整数脏检查 |
| `LocalUser.<Slot 16>` | `0x7FFB17AEEE00` | `0xFAEE00` | 先调基类，再做镜头 / HUD |
| `UserLocal.<Slot 16>` | `0x7FFB17B50430` | `0x1010430` | 覆写链最外层 |
| `FieldActorBase.get_Pos()` Slot 11 | `0x7FFB17CD64B0` | `0x11964B0` | 裸 getter；**不参与 TRS** |
| `FieldActorBase.get_PosPrev()` | `0x7FFB17CD64C0` | `0x11964C0` | 同上 |
| `Transform.set_position(Vector3)` | `0x7FFB1B9A4230` | `0x4E64230` | UnityEngine；全库 112 处调用方 |
| `Time.get_unscaledTime()` | `0x7FFB1B99F2E0` | `0x4E5F2E0` | 插值时间基准 |
| `Object.op_Equality(Object,Object)` | `0x7FFB1B991A10` | `0x4E51A10` | Slot 16 门控③（Unity 假空） |
| `static int Round(double)` | `0x7FFB17F3C500` | `0x13FC500` | `GetPos` 取整 |
| 插值分母常量 `0.03f` | `0x7FFB1BF52CC8` | — | 实读 `0x3CF5C28F`；= 一个逻辑帧 30ms |

字段：`FieldActorBase.VecCtrl@0x50` / `Pos@0x64` / `PosPrev@0x6C` / `IsUseAlternative@0x74`；
`VecCtrl.Ap@0x98` / `Apl@0xB8`；`UserBase._avatarRoot@0x80`；`LocalUser.CurPos@0x240` / `PrevPos@0x248`。

### CMS 签名（对照）

```csharp
bool IsTeleportSkillAvailable(SkillEntry skill, int skillLevel, ref Vector2 positionAfterTeleport);
bool TryRegisterTeleport(SkillEntry skill, int skillLevel,
                         string portalName = "", string targetPortalName = "",
                         bool isForced = false);
void TryDoingTeleport();
```

### 经典法师技能 ID（WZ / 本仓目录）

| SkillId | 线 |
|---------|-----|
| `2001002` | 法师一转相关 |
| `2101002` | 火毒 · 瞬间移动 |
| `2201002` | 冰雷 · 瞬间移动 |
| `2301001` | 主教 · 瞬间移动 |

---

## 2. 原生调用链

```text
按键 / UserLocal_DoActiveSkill
  └─ call @ 0x7FFB17BAFF60
       → TryRegisterTeleport
            ├─ foothold probe / 落点（含 IsTeleport… 语义）
            ├─ VecCtrl_SetMovePathAttribute
            ├─ UserLocal_SendSkillUseRequest
            └─ 登记待执行状态

每帧 Update（sub_7FFB17B50620）
  └─ call @ 0x7FFB17B55D04
       → TryDoingTeleport
            ├─ 可再调 TryRegisterTeleport
            └─ sub_7FFB17BD5A10（落地 helper · RVA `0x1095A10`）
                 ├─ VecCtrl_SetMovePathAttribute          // 再写 Attr
                 ├─ VecCtrl.IsOnFoothold                   // `0x11B5AF0`
                 ├─ **VecCtrl.SetImpactNext(vx, vy)**       // `0x11A1A10` ★位移真入口
                 ├─ EffectGeneral(uol, …)                  // `0xEC8DB0` 瞬移特效
                 └─ （CFA 壳内还有朝向/Key 辅助；无直接 Transform.set_position）
```

**Register 的 code xref（摘要）**

| 调用方 | CALL 地址 |
|--------|-----------|
| `UserLocal_DoActiveSkill` | `0x7FFB17BAFF60`（施放主路径） |
| `UserLocal_TryDoingTeleport` | `0x7FFB17B5E25B` |
| `UserLocal_TryRegTeleport_caller_*` | `0x7FFB17BBB7AB` |
| `UserLocal_MoveToPortal` | `0x7FFB17BD8EE5` |
| `OpRecv_00F0` | `0x7FFB17C5D81C` |

要点：原生有**技能语义**与**两阶段收态**；Doing 内部完成走路态恢复，**不需要**我们的 `RestoreWalkable`。

### 2.1 官方如何同步皮（2026-08-02 IDA 实锤）

落地 helper **不**调 `Transform.set_position`。位移走 **`VecCtrl.SetImpactNext(double vx, double vy)`**：

| 证据 | 内容 |
|------|------|
| CMS | `VecCtrl.ImpactNext { Valid, Vx, Vy }`；`SetImpactNext(vx,vy)` |
| TW RVA | `0x11A1A10`（VA `0x7FFB17CE1A10`） |
| 同函数其它调用方 | `UserLocal_SetDamaged`（受击击退）、`DoCombatStep` —— 与瞬移共用「下一帧冲击位移」管道 |
| 特效 | `EffectGeneral` 播 UOL，与位移解耦 |

含义：引擎在随后 `WorkUpdateActive` 消费 `impactNext`，自己推 Ap / RelPos / MoveAction，
再由 `FieldActorBase` Slot 16 按 `Ap`/`Apl` 插值写 TRS。  
我们旁路只写 AbsPos + Attr=4 + Flush，**跳过了 ImpactNext**，所以才被迫钉 Transform / 走路牵绳——那是在补官方本会做的事。

> 📌 2026-08-02 精化：上句里的「推 Pos」需去掉——`Pos@0x64` 不在 TRS 链上。
> 且「被迫钉 Transform」的真实原因是 `Apl` 未同步导致的 ≤30ms 插值滑行，
> 写 `Apl` 即可等效替代，见 [`P0c`](P0c_视觉层同步链.md) §7.2。

落点语义（`IsTeleportSkillAvailable`）：朝向 ± WZ `range`（约 130–150）+ Y 窗 ±80（MBA 已证）。详见：

- `Dumps/runtime/_cfa_IsTeleportSkillAvailable_pseudo.md`
- `Dumps/runtime/_cfa_IsTeleportSkillAvailable_blocks.txt` / `_jmptab.txt`
- `Dumps/runtime/_mba_decode_teleport.py`

---

## 3. 产品主路径（Attr=4 旁路）

```text
主线程 TeleportJobFn（2026-08-02 按源码核对后重写）
  ClearFoothold(vc)                          // 清 CurFh / LastFh / FallStart
  WriteF64(vc, Apl.X/Y, 旧 Ap)               // ⚠️ 注释写"回溯点"，实为插值起点，见下
  WriteI16(mp, _x/_y, 落点)                  // 发包线
  WritePtr(vc, CurFh/LastFh, plantFh)        // ★ 偷换到目标踏板 = 位移真源
  TryAttrFallback(vc, mp)                    // Attr=4 Teleport + ForcedFlush
  CallSetImpactNext(vc, dx, dy)              // ★ 扳机：置 Valid
  → 之后由 settle（ArmImpactSettle / TickImpactBlinkSettle）延迟 Plant+RelPos
```

> ⚠️ **本块曾长期写成 `WriteDestination(AbsPos / CurFh / Transform …)` + `RestoreWalkableFields`，与代码不符**：
> 现版 `teleport_port.cpp` 里**既没有 `WriteDestination`，也没有任何 `Transform.set_position` 调用**，
> 且**不预写 AbsPos**、Restore 已延后到 settle。位移机制见 [`P0b`](P0b_引擎实现原理.md) §4，
> `Apl` 那行的正确语义见 [`P0c`](P0c_视觉层同步链.md) §7.1。

| API | 说明 |
|-----|------|
| `TeleportTo(x,y, allowLongHop, plantFhId)` | `allowLongHop=false`：硬帽 ≤`kCombatTeleportMaxHopPx`(160)；`true`：测试不限距 |
| `RestoreWalkable()` | Snap 种台 + Attr=Normal + `SetInput(0,0)`；**F5 关 / 测试按钮后必调** |
| `SetMinCooldownMs` | 打怪默认 200ms；API 下限 ≥50ms |

**为何旁路后要 RestoreWalkable**：只借了 Move 的 Teleport 属性发包，没走 Doing 收态 → KeyPad 有键、`inX=0` 不能走（BIN：怪仍能打 = 服认位）。

相关协议：[`../protocol/MoveElem字段.md`](../protocol/MoveElem字段.md) Attr=4 Teleport；[`../protocol/移动协议.md`](../protocol/移动协议.md)。

---

## 4. RVA 誊写事故（已修）

曾把正确 RVA **`0x10804A0` / `0x101CF30`** 写成 **`0x1804A0` / `0x1ACF30`**（`0x10xxxxxx` 少写一位 `0`）。  
VA 一直对，错在 VA→RVA 手算；因法师线长期关着，错误休眠未踩雷。以本文 §1 表为准。

---

## 5. BIN 纪要（2026-08-01）

| 观察 | 含义 |
|------|------|
| `ok path=attr` + hop 至 ~1435 | Attr 远距一跳可行 |
| `RestoreWalkable ok` · `ma raw=2` Stand | 冷静间隔后可恢复走路 |
| 狂点 → 冷却拒跳 → `Disconnected` · sticky `pendingError=205`（哨兵） | 频率/超距风控；kick 环常 `lean_local_or_soft`；205≠踢因 |
| 怪能打到角色 | 服务器认坐标 |
| `bound path=attr-only mage=0` | 产品关闭法师线后的绑定日志 |
| 测试按钮全程 `path=attr` | `allowLongHop=true` 强制 Attr（刻意） |

日志：`XCat_data/logs/combat.log` · `x.jsonl` tag=`Teleport`/`SimpleCombat` · `Dumps/runtime/kick.log`。

---

## 6. 代码与面板入口

| 路径 | 说明 |
|------|------|
| `x/features/ports/teleport_port.cpp` | Attr 主路径；法师线 `#if` 语义由 `kEnableMageTeleportSkillPath` 控制 |
| `x/features/simple_combat/` | `MoveTo` 贴怪；`RequestTeleportToRandomMob` 测试 |
| `xcat_app/workspace_tabs.cpp` | 「测试贴怪瞬移」→ `[core] teleportTestSeq` |
| `common/xcat_payload_control.*` | `teleportTestSeq` / 贴怪参数 |
| `x/ipc/payload_control.cpp` | 边沿消费 seq（防注入重放） |

---

## 7. 非目标

- 不把法师 Register/Doing 当作默认挂机瞬移（射程/朝向与「贴绝对坐标」语义不合）  
- 不恢复已废弃的「一律放行超距」战斗路径（160 帽保留；仅测试 `allowLongHop`）  
- 不 INLINE HOOK `Flush` / 改 `.text`
