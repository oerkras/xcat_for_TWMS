# teleport P0c · 视觉层同步链：皮到底跟谁走（2026-08-02）

> **历史笔记**：ImpactNext 旁路已拆除；视觉链结论仍适用于 native Doing。
> **状态**：✅ 只读采证完成（dump + IDA 双向）；主链全程反汇编实证
> **产品**：新枫之谷：经典版（TW · `Maplestory_Classic.exe`）· **不是**枫星
> **数据源**：`DumpRestoredData/dump.cs.restored.C` · IDB `Dumps/runtime/GameAssembly.dll.i64`
> **RVA 基址**：`0x7FFB16B40000` · VA = base + RVA
> **安全**：全程只读 dump / IDB / 源码，未改 `.text`、未发包、未运行客户端

本文回答一个问题：**「改了坐标但皮囊出错」到底错在哪一环**。

[`P0b`](P0b_引擎实现原理.md) 讲的是位移怎么发生，本文讲的是**位移发生之后，精灵是怎么被引擎搬过去的**。

---

## 0. 三个能立刻改写做法的结论

| # | 结论 | 影响 |
|---|---|---|
| **①** | 皮的唯一真源是 **`VecCtrl.Ap` 与 `Apl` 的实时插值**，不是任何"皮字段" | 写 `FieldActorBase.Pos@0x64` 对精灵**完全无效** |
| **②** | 每个渲染帧引擎都会用 `Ap`/`Apl` **重算并覆写** `Transform.position` | 手工钉 TRS 只能活到下一帧，**必被拉回** |
| **③** | `Apl` 是插值的**起点**，不是"回溯点" | 写了新 `Ap` 却留着旧 `Apl`，皮必然**从旧点滑过去**而不是瞬移 |

一句话：**想让皮到某点，就把 `Ap` 和 `Apl` 一起写成那个点；其余一切写法都是在和引擎抢方向盘。**

---

## 1. 完整链路（RVA 全钉死）

```text
每个渲染帧
  └─ UserLocal.<Slot 16>                        RVA 0x1010430 / VA 0x7FFB17B50430
       └─ LocalUser.<Slot 16>                   RVA 0xFAEE00  / VA 0x7FFB17AEEE00
            ├─ FieldActorBase.<Slot 16>         RVA 0x1197330 / VA 0x7FFB17CD7330   ★ 唯一写 TRS 的地方
            │    ├─ 门控①  IsUseAlternative(+0x74) 必须为 false，否则直接 return
            │    ├─ 门控②  VecCtrl(+0x50) 必须非空
            │    ├─ 门控③  Object.op_Equality(this, null) 必须为 false（对象未销毁）
            │    ├─ p = VecCtrl.GetPos()        RVA 0x1176CC0 / VA 0x7FFB17CB6CC0   ★ 插值真源
            │    └─ if ((int)tr.position.x != (int)p.x || (int)tr.position.y != (int)p.y)
            │           tr.position = new Vector3(p.x, p.y, 0)                       ★ 整数脏检查
            └─ 之后才是镜头 / HUD / 名牌等（LocalUser 自己的活）

每个逻辑帧（30ms）
  └─ VecCtrl.BeginUpdateActive()                RVA 0x11A41E0 / VA 0x7FFB17CE41E0
       └─ Apl ← Ap（32 字节整体拷贝）                                                 ★ Apl 的唯一滚动点
  └─ VecCtrl.WorkUpdateActive(tElapse)          RVA 0x11A4400
       └─ 积分更新 Ap / Rp（见 P0b §3）
```

---

## 2. `VecCtrl.GetPos()` —— 皮坐标是算出来的，不是存出来的

`GetPos()` 在 dump 里名字已恢复（C 档 L71935-71936，`public Vector2 GetPos()`），全游戏 **24 个调用方**。
函数体被 CFA 平坦化，逐块还原后：

```c
Vector2 VecCtrl::GetPos()
{
    float  alpha = (sStaticTime - Time.unscaledTime) / 0.03f;   // 0.03s = 一个逻辑帧
    double x     = Ap.X + (Apl.X - Ap.X) * alpha;               // lerp(Ap, Apl, alpha)
    double y     = Ap.Y + (Apl.Y - Ap.Y) * alpha;
    return new Vector2((float)RoundToInt(x), (float)RoundToInt(y));
}
```

### 2.1 指令级证据（VA）

| 观察 | 指令 |
|---|---|
| 读静态时间基准 | `0x7FFB17CB6F29: mov rax,[rcx+0B8h] / movss xmm7,[rax]` |
| 取当前时间 | `0x7FFB17CB6F36: call sub_7FFB1B99F2E0` = `Time.get_unscaledTime()`（RVA `0x4E5F2E0`） |
| **读 `Ap.X`** | `0x7FFB17CB6F3B: movsd xmm8, [rsi+98h]` |
| **读 `Apl.X`** | `0x7FFB17CB6F44: movsd xmm6, [rsi+0B8h]` |
| 求差 | `0x7FFB17CB6F5E: subss xmm7, xmm0` |
| **除以 0.03** | `0x7FFB17CB6F78: divss xmm7, cs:dword_7FFB1BF52CC8`（实读 `0x3CF5C28F` = **0.03f**） |
| lerp X | `0x7FFB17CB6F91: subsd xmm6,xmm8 / mulsd xmm6,xmm7 / addsd xmm6,xmm8` |
| 取整 | `0x7FFB17CB6FA5: call sub_7FFB17F3C500` = `static int Round(double)`（RVA `0x13FC500`，按符号分流） |
| **读 `Ap.Y` / `Apl.Y`** | `0x7FFB17CB6FAA: movsd xmm0,[rsi+0A0h]` / `0x7FFB17CB6FB2: movsd xmm1,[rsi+0C0h]` |
| 打包返回 | `0x7FFB17CB6FE7` 起，MBA 混淆后等价于 `RAX = (bits(Y) << 32) | bits(X)` |

偏移与 [`P0b`](P0b_引擎实现原理.md) §1.3 的 `VecCtrl` 布局逐一对上：`Ap@0x98`(X)/`0xA0`(Y)、`Apl@0xB8`(X)/`0xC0`(Y)。

### 2.2 alpha 的行为

| alpha | 渲染位置 | 何时 |
|---|---|---|
| ≈ 1 | **`Apl`**（上一逻辑帧） | 刚跑完一次逻辑更新 |
| ≈ 0 | **`Ap`**（当前） | 下一次逻辑更新前夕 |

即：**画面永远滞后一个逻辑帧，并在 30ms 内从 `Apl` 平滑滑到 `Ap`**。这是标准的固定步长渲染插值。

> ⚠️ **没有 clamp。** 反汇编里 alpha 未做 `[0,1]` 钳制，掉帧或逻辑卡顿时会外插到 `Ap` 之外或 `Apl` 之前。
> 极端卡顿下皮会冲过头再弹回来 —— 这不是我们的 bug，是引擎设计裸露的边界。

---

## 3. 写 TRS 的那一步：三道门控 + 整数脏检查

`FieldActorBase.<Slot 16>`（RVA `0x1197330`，1416 字节，50 个基本块，CFA 平坦化）还原后：

```c
void FieldActorBase::SyncTransform()
{
    if (this->IsUseAlternative) return;              // +0x74，门控①
    VecCtrl vc = this->VecCtrl;                       // +0x50
    if (vc == null) return;                           // 门控②
    if (Object.op_Equality(this, null)) return;       // 门控③（Unity 假空）

    Vector2 p = vc.GetPos();
    Transform tr = this.transform;
    if ((int)tr.position.x != (int)p.x || (int)tr.position.y != (int)p.y)
        tr.position = new Vector3(p.x, p.y, 0);       // z 恒为 0
}
```

| 观察 | 指令（VA） |
|---|---|
| 门控① 读 `IsUseAlternative` | `0x7FFB17CD762A: movzx eax, byte ptr [rsi+74h] / xor eax,1 / add eax,eax`，比对失败即跳 `loc_7FFB17CD7886`（return） |
| 门控② 读 `VecCtrl` | `0x7FFB17CD765E: mov rax,[rsi+50h] / test rax,rax` |
| 门控③ | `0x7FFB17CD76CD: call sub_7FFB1B991A10` = `Object.op_Equality`（RVA `0x4E51A10`） |
| 取 VecCtrl 传参 | `0x7FFB17CD7700: mov rcx,[rsi+50h]` → `0x7FFB17CD7724: call GetPos` |
| X 脏检查 | `0x7FFB17CD7777: movd xmm0,[rsp+…] / punpckldq / cvtps2pd / cvttpd2dq / pcmpeqd` |
| Y 脏检查 | `0x7FFB17CD77E5` 起，结构对称 |
| 组 Vector3 并写入 | `0x7FFB17CD7856/7862/786E`（x, y, z=0）→ `0x7FFB17CD7881: call sub_7FFB1B9A4230` = `Transform.set_position`（RVA `0x4E64230`） |

### 3.1 整数脏检查的实际后果

比较的是 **`(int)` 截断后的值**，不是浮点值：

- **亚像素级修改不会让皮动一下** —— 想靠 0.5px 微调对齐是白费力气。
- 反过来，只要整数不同，引擎**每帧都会写**，我们钉的任何 TRS 值都活不过一帧。

---

## 4. `Apl` 的唯一滚动点：`BeginUpdateActive()`

`VecCtrl.BeginUpdateActive()`（Slot 10，RVA `0x11A41E0`）里：

```text
0x7FFB17CE4301: lea rcx, [rsi+98h]              ; src = &Ap
0x7FFB17CE4308: lea rax, [rsi+0B8h]             ; dst = &Apl
0x7FFB17CE4315: movups xmm0, [rcx]              ; Ap.X, Ap.Y
0x7FFB17CE4318: movups xmm1, [rcx+10h]          ; Ap.Vx, Ap.Vy
0x7FFB17CE431C: movups [rax+10h], xmm1
0x7FFB17CE4320: movups [rax], xmm0
```

**整块 32 字节 `AbsPos` 拷贝，`Apl ← Ap`，每逻辑帧一次。**

顺带否掉一个可能的误会：`VecCtrl.WorkUpdateActive`（`0x11A4400`）与 `VecCtrlUser.WorkUpdateActive`（`0x11CFA90`）
**都不写 `Apl`**（前者全函数只有两处 `movsd` 写入，目标都是 `Rp.Pos@0x88`；后者只读 `0x98`/`0xB8` 做比较）。

---

## 5. 覆写链：本地玩家走的是三层

| 层 | 类 | dump 行 | VA | 行为 |
|---|---|---|---|---|
| 最外 | `UserLocal`（TW 哈希名 `bbb1465…: LocalUser`，TypeDefIndex 1577） | L70330 | `0x7FFB17B50430` | 调下一层 |
| 中间 | `LocalUser : UserBase`（1560） | L69562 | `0x7FFB17AEEE00` | **先**调基类，再做镜头 / HUD / 名牌 |
| 基类 | `FieldActorBase`（1586） | L71513 | `0x7FFB17CD7330` | 真正写 TRS |

`Npc`（1511）另有一份覆写（`0x7FFB17AD5430`）；`Mob`（1507）无覆写，直接继承基类。

> **`UserLocal` 的身份认定**：TW dump 未恢复其外层类名，但它的嵌套类型全部叫 `UserLocal.Teleport` /
> `UserLocal.Rush` / `UserLocal.DualKeyChecker`（C 档 L69989-70070），且继承自 `LocalUser` —— 据此认定。

---

## 6. 纠偏：`FieldActorBase.Pos@0x64` 不是视觉真源

现有文档（[`同步模型.md`](同步模型.md) §1）把 `FieldActorBase.Pos@0x64` 标为「角色视觉坐标」。**不成立。**

- 基类 Slot 16 全函数只读 `+0x74`、`+0x50`，以及经 `GetPos()` 间接读的 `Ap`/`Apl`。
  **没有任何一条指令读 `+0x64`。**
- `Pos@0x64` 与 `PosPrev@0x6C` 只是普通字段，对外经两个 16 字节的裸 getter 暴露：

```text
0x7FFB17CD64B0: mov rax, [rcx+64h] / retn      ; Slot 11，public virtual Vector2
0x7FFB17CD64C0: mov rax, [rcx+6Ch] / retn      ; public Vector2
```

它大概率服务于名牌挂点 / 命中测试等逻辑侧消费方（**本轮未定位其写入方，标 NOT RUN**），
但**不参与** `Transform` 计算。往它写坐标不会让精灵移动一个像素。

---

## 7. 「改坐标皮出错」的机制对照表

| 做法 | 引擎实际反应 | 症状 |
|---|---|---|
| 只写 `Pos@0x64`（+ `CurPos@0x240`） | TRS 链根本不读它 | **皮完全不动** |
| 钉 `Transform`，但 `Ap`/`Apl` 仍是旧点 | 下一渲染帧 `renderPos` 仍算出旧点，int 不同 → **覆写回去** | **闪一下就被拉回** |
| 钉 `Transform`，且 `Ap` 已到位 | 插值收敛后 int 相同 → 脏检查不再触发 | **看起来"一次钉就够"**（见 §7.2） |
| **持续**钉 `Transform` | 每帧与引擎对写 | **冻皮 / 抢动画 / 掐刀**（既有 BIN 已打穿） |
| 写 `Ap`，`Apl` 留旧值 | `GetPos()` 在两点间插值，直到下个逻辑帧 `Apl ← Ap` | **皮用 ≤30ms 从旧点滑过去**，长跳看着像拖影 |
| 写 `Ap` + `Apl` 同值 | 插值两端相同 → 恒等于目标点 | **皮一帧到位** ← 正解 |
| 写 `Ap` 但 `RelPos` 没跟着改 | 引擎下一帧 `AbsPos ← RelPos` 重算，把 `Ap` 拉回旧点 | **魂被拉回、皮留原地**（既有 BIN 已打穿） |
| 写了 `Ap` 但 `IsUseAlternative`=true | Slot 16 首行就 return，TRS 一次都不写 | **魂到皮留原地** |

### 7.1 本仓当前写法（fill+Doing · 2026-08-02）

`x/features/ports/teleport_port.cpp` **现役仅** `ApplyFillDoing`：

1. 手填 `UserLocal.Teleport` + Mp XY +（可选）CurFh → `TryDoingTeleport` + ForcedFlush  
2. 成功后 **`Apl ← Ap`**（P0c：皮插值起点对齐落点；**禁止**再写旧 Apl / SyncRel settle）  
3. ~~`WritePhysicsPos` / ImpactBlink 写旧 Apl~~ **已拆除**

> ImpactNext 旁路的「只写 Ap 不写 Apl」「主动把 Apl 钉成旧点」问题随旁路代码一并消失。  
> 产品路径见 [`模块设计.md`](模块设计.md)。

### 7.2 与既有 BIN 记录的对账

[`同步模型.md`](同步模型.md) §5 有两条实测：**「只写 Ap、不钉 Pos → 魂到、皮留原点」**、
**「只钉 Pos、不钉 Transform → 皮仍不到落点」**，结论是「落点必须一次钉 TRS」。
本文机制**不与之冲突**，而是给出了它成立的原因：

1. **第二条完全吻合** —— `Pos@0x64` 本就不参与 TRS（§6），钉它当然没用。
2. **第一条**：若当时 `RelPos` 未同步，引擎下一帧会用 `AbsPos ← RelPos` 把 `Ap` 拉回旧点，
   皮自然留在原点 —— 这与 §5 另一条「`AbsPos←RelPos` 回写 → 魂被拉回、皮留原地」是同一个机制。
3. **「一次钉 TRS 就够」为什么成立**：落点时 `Ap` 已经到位（踏板偷换 + Plant + RelPos），
   但 `Apl` 还是旧点，于是 `GetPos()` 会插值滑行 ≤30ms —— 这段滑行就是被观测到的「皮不到落点」。
   手工钉一次 TRS 把皮直接拍到位；等下一逻辑帧 `Apl ← Ap` 后插值收敛，int 相同，脏检查不再覆写，
   所以钉一次就"粘住"了。

**换句话说：一次钉 TRS 是在用外力掩盖 `Apl` 造成的插值滑行。**
直接写 `Apl` 能达到同样效果，且不与引擎抢 `Transform` 的写入权 —— 这是本文建议改动的全部理由。

---

## 8. 建议的正确写法与证伪实验（均未执行）

### 8.1 收态写法

```text
落点收态（一次）：
  1. Ap.X/Y  = 目标点       （Ap.Vx/Vy = 0）
  2. Apl.X/Y = 目标点       ← 新增，缺这一步就是 ≤30ms 的插值滑行
  3. Plant CurFh + RelPos.SetFromAbsPos     ← 缺这步，Ap 会被下一帧重算拉回
  4. Attr → Normal，ForcedFlush = false
  5. 不写 Pos@0x64（无效）；Transform 可不写 —— 第 2 步已经让引擎自己算出目标点
```

与 [`同步模型.md`](同步模型.md) §6 现行写法的差异只在第 2 / 5 条：现行做法是**钉一次 TRS** 来盖掉滑行，
本文主张**改成写 `Apl`**，等效但不抢引擎的 `Transform` 写入权。
两者不是对错之争 —— 现行做法能work，理由见 §7.2；只是外力掩盖不如去掉病根稳妥。
在 §8.2 实验一/二跑通之前，**不建议直接摘掉现有的一次 TRS 钉**。

### 8.2 实验

| # | 做法 | 若本文模型成立 | 若不成立 |
|---|---|---|---|
| **一** | `WritePhysicsPos` 里补写 `Apl.X/Y = 目标点`，其余不动 | `force_ap` 后皮**一帧到位**，不再拖影 | 无变化 → `Apl` 另有写入方 |
| **二** | 收态里**去掉**「一次钉 Transform」，只保留 `Ap`+`Apl` | 皮照样准确到位 | 皮不到位 → 存在本文未发现的第二条视觉链 |
| **三** | 落点后读一次 `IsUseAlternative(+0x74)` 与 `Active(+0x80)` 并打进 BIN | 出错样本里能看到门控被置起 | 门控正常 → 问题在 `Ap`/`Apl` 数值本身 |

实验一成本最低且直接可证伪，**建议先做**。实验三是纯观测、零风险，可以和实验一同批加日志。

---

## 9. 官方硬钉原语：`VecCtrl.SetFirstFoothold`

`VecCtrl.SetFirstFoothold(StaticFoothold fh, Vector2 pos)`，RVA `0x11B4D80` / VA `0x7FFB17CF4D80`，仅 260 字节：

```text
0x7FFB17CF4DF0: mov [rdi+28h], rdx         ; this->CurFootHold = fh
0x7FFB17CF4DF4: call sub_7FFB16ECE470      ; GC write barrier
0x7FFB17CF4DF9: mov rcx, [rdi+10h]         ; this->Owner (FieldActorBase)
0x7FFB17CF4E14: call sub_7FFB1B9896A0      ; Owner.transform
0x7FFB17CF4E53: …组 Vector3(pos.x, pos.y, 0)
0x7FFB17CF4E6C: call sub_7FFB1B9A4230      ; Transform.set_position  ← 无脏检查、无插值
```

它是**除 Slot 16 之外，VecCtrl / FieldActor 区域里唯一碰 Transform 的函数**。

**注意它不写 `Ap` / `Apl` / `Rp`**，只挂踏板 + 硬钉 TRS。所以它只适合「首次放置 / 换图落地」这种
`Ap` 由别处设定好的场景；直接拿来当瞬移用，下一帧就会被 Slot 16 按 `Ap`/`Apl` 拉回去。

---

## 10. 本轮新钉死的 RVA 汇总

| 符号 | RVA | VA | 来源 |
|---|---|---|---|
| `VecCtrl.GetPos()` | `0x1176CC0` | `0x7FFB17CB6CC0` | dump 名字已恢复 |
| `VecCtrl.BeginUpdateActive()` Slot 10 | `0x11A41E0` | `0x7FFB17CE41E0` | dump 名字已恢复 |
| `VecCtrl.SetFirstFoothold(fh, pos)` | `0x11B4D80` | `0x7FFB17CF4D80` | dump 名字已恢复 |
| `FieldActorBase.<Slot 16>` 视觉同步 | `0x1197330` | `0x7FFB17CD7330` | 哈希名，按行为认定 |
| `LocalUser.<Slot 16>` | `0xFAEE00` | `0x7FFB17AEEE00` | 同上 |
| `UserLocal.<Slot 16>` | `0x1010430` | `0x7FFB17B50430` | 同上 |
| `FieldActorBase.get_Pos()` Slot 11 | `0x11964B0` | `0x7FFB17CD64B0` | 裸 getter，反汇编确认 |
| `FieldActorBase.get_PosPrev()` | `0x11964C0` | `0x7FFB17CD64C0` | 同上 |
| `Transform.set_position(Vector3)` | `0x4E64230` | `0x7FFB1B9A4230` | dump（UnityEngine） |
| `Time.get_unscaledTime()` | `0x4E5F2E0` | `0x7FFB1B99F2E0` | dump（UnityEngine） |
| `Object.op_Equality(Object,Object)` | `0x4E51A10` | `0x7FFB1B991A10` | dump（UnityEngine） |
| `static int Round(double)` | `0x13FC500` | `0x7FFB17F3C500` | dump（哈希名，签名可辨） |
| 插值分母常量 `0.03f` | — | `0x7FFB1BF52CC8` | `get_int` 实读 `0x3CF5C28F` |

字段侧补充：`UserBase._avatarRoot@0x80`、`UserBase.get_AvatarRoot()@0x123CBE0`、
`UserBase.get_RendererTransform()@0x123CC50`、`LocalUser.CurPos@0x240`、`LocalUser.PrevPos@0x248`。

---

## 11. 复现方法

### 11.1 dump 侧（⚠️ 有坑）

```powershell
$f = ".\DumpRestoredData\dump.cs.restored.C"
rg --no-ignore -n "RVA: 0x1176CC0" $f          # → 下一行即签名 public Vector2 GetPos()
rg --no-ignore -n "TypeDefIndex: 15[5-8][0-9]" $f   # → 类边界与继承关系一次看全
```

> **两个实测踩到的坑**：
> ① `DumpRestoredData/` 被 gitignore，**不带 `--no-ignore` 且按目录搜时会静默零结果**；按文件路径搜则正常。
> ② 该文件是 CRLF，正则里的 `$` 锚点（如 `"Slot: 16$"`）**永远不匹配**。去掉 `$` 或用 `\b`。
>
> 找虚函数覆写最稳的办法不是搜 `Slot: N`，而是**搜那个哈希方法名本身** —— 覆写会沿用同名。

### 11.2 IDA 侧

CFA 平坦化函数一律 `disasm` 逐块看，`decompile` 只能拿到跳转表。本轮实际有效的手法：

- `insn_query {func, mnem:"call", include_disasm}` — 先看调用序列，函数职责一眼可辨
- `insn_query {func, mnem:"movsd"/"movups", include_disasm}` — **定位字段读写目标最快**，
  `Apl ← Ap` 那次拷贝就是这么一次查出来的
- `insn_query {start, end, include_disasm}` — 跳过 prologue 的表初始化段（通常前 60~100 条）
- `analyze_function {addr}` — callers / callees 用来认身份（`GetPos` 的 24 个调用方一眼看出它是通用 getter）
- `get_int {addr, ty:"u32"}` — 读浮点常量位模式（`0x3CF5C28F` = 0.03f）
- `xrefs_to "0x7FFB1B9A4230"` — 全库 112 处 `set_position` 调用方，用来穷举「谁能动 TRS」

> `insn_query` 的 `op0` **不能**用来匹配位移量（试过 `op0:184` 找 `[rsi+0B8h]`，扫了 59881 条零命中）。
> 要按偏移找字段访问，只能按 mnemonic 拉全量再自己筛。

---

## 12. 证据等级与置信度

| 结论 | 证据 | 置信度 |
|---|---|---|
| `GetPos()` = `round(lerp(Ap, Apl, alpha))` | 逐块反汇编 + 常量实读 + 偏移与 VecCtrl 布局对齐 | **高** |
| 插值分母 = 0.03f | `get_int` 实读 `0x3CF5C28F` | **高** |
| Slot 16 写 `Transform.position(x,y,0)` 且带整数脏检查 | 逐块反汇编，`set_position` 调用点唯一 | **高** |
| 三道门控（`IsUseAlternative` / `VecCtrl` / Unity 假空） | 反汇编 + `op_Equality` 名字来自 dump | **高** |
| `BeginUpdateActive` 执行 `Apl ← Ap` | `lea`+`movups` 四条指令，源目标地址明确 | **高** |
| `Pos@0x64` 不参与 TRS | Slot 16 全函数扫描，无 `+0x64` 访问 | **高** |
| 覆写链 `UserLocal → LocalUser → FieldActorBase` | dump 同名覆写 + IDA callers 双证 | **高** |
| `UserLocal` 就是哈希类 `bbb1465…` | 嵌套类型名 + 继承关系推定 | 中高 |
| alpha 的静态基准 = 下一次逻辑更新时刻 | 由 `0.03` 与 lerp 方向**推断**，静态字段运行时值未读 | **中 · 推断** |
| §7 表中各症状与写法的对应 | 机制推论 + 与既有 BIN 记录自洽 | 中高 · **未复验** |
| `Pos@0x64` 的写入方与真实用途 | — | **NOT RUN** |
| Slot 16 的驱动方（Unity 消息名 / 管理器） | 基类 3 个 caller 均为派生覆写；真正驱动方未定位 | **NOT RUN** |

**NOT RUN**：全程静态只读，未编译、未运行客户端、未做 BIN 复验。§8 三个实验均未执行。
`CollisionDetectFloat`、`AbsPos.SetFromRelPos`、`LeaveFoothold` 的函数体本轮未进入（见 P0b §9）。

---

## 13. 关联文档

- [`P0b_引擎实现原理.md`](P0b_引擎实现原理.md) — 位移怎么发生：impact 合并语义、消费链、踏板偷换
- [`P0a_瞬移CALL锚点.md`](P0a_瞬移CALL锚点.md) — IDA 侧入口 RVA / 调用链 / BIN 纪要
- [`同步模型.md`](同步模型.md) — 坐标分层与收态禁区（§1「Pos@0x64 是视觉坐标」已被本文 §6 纠偏）
- [`模块设计.md`](模块设计.md) — 职责边界与对外契约
- [`坐标真源交叉验证.md`](坐标真源交叉验证.md) — 历史结论（已被 P0b §7 纠偏）
- [`../protocol/MoveElem字段.md`](../protocol/MoveElem字段.md) — 走路链 / Input 锁存（§11.6）
