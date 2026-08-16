# map_attack P0a · 攻击盒与 MobCount

> **状态**：结案（只读 IDA / DUMP，**未写功能代码**）  
> **产品**：新枫之谷：经典版（TW · `Maplestory_Classic.exe`）· **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64`  
> **imagebase（本轮 MCP `server_health`）**：`0x7ffd72820000`  
> **CMS DUMP**：`Dumps/cms_cw/dump.cs`（字段偏移用；方法 RVA 以 runtime IDB 为准，CMS `FindHit` RVA `0x8DD7C0` 已过时）  
> **日期**：2026-08-14

关联：[模块设计.md](./模块设计.md)

---

## 0. 结案一句话

官方出刀名单 = **`UnityEngine.Rect`（float `x,y,w,h`，世界 AbsPos）∩ `maxCount`**。

| 路径 | Rect 从哪来 | `maxCount` |
|---|---|---|
| 技能（A 槽 t=1） | `SkillLevelData.rcAffectedArea@+0xAC` 拷进栈盒，再 `Rect.Offset` 加上角色位置 | **`SkillLevelData.MobCount@+0x98`（真读字段，不是种子 1）** |
| 普攻 / NA（空绑或合成 5/52） | 武器 Range / 角色朝向拼出的整数角点，再 `cvtdq2ps` 写成 `x,y,w,h` | **种子实算 = 1**（射击、近战各抽到） |
| 包 flags | 高 nibble = 名单 count | **硬顶 15**（与本轮无关，P0c 已否决「一包塞全图」） |

地图 AABB 锚点：复用 `map_bounds_port::QueryPlayBounds`（本图 foothold 外包）。布局是 `left/top/right/bottom` 整数；塞进 FindHit 前必须改成 Unity Rect：`x=left, y=top, w=right-left, h=bottom-top`。`top=min y`、`bottom=max y`，AbsPos **+Y 向上**，字段名与直觉相反。

**普攻不能作为「一刀多怪」验收。** 射击 / 近战 NA 的 `maxCount=1` 是 `get_int` 实算。P2 近战扩盒测的是远距**那一只**；一刀多怪仍要 A 槽绑 `MobCount ≥ 2`。

---

## 1. 符号与 RVA（本轮 IDB）

`RVA = VA - 0x7ffd72820000`。remount 后 VA 会漂，**以 RVA + hit_pin 的 12 字节序言校验为准**。

| 符号 | VA | RVA | size |
|---|---|---|---|
| `MobPool.FindHitMobInRect` | `0x7FFD737A77F0` | **`0xF877F0`**（与 `hit_pin_port` 一致） | `0x1148` |
| `TryDoingShootAttack`（疑） | `0x7FFD73890390` | `0x1070390` | `0xecd3` |
| `TryDoingMeleeAttack`（疑） | `0x7FFD7387AE40` | `0x105AE40` | `0x15349` |
| `Skill.GetLevelData`（疑） | `0x7FFD73D9F8B0` | `0x157F8B0` | CFF |
| `Rect.Offset` 包装 | `0x7FFD73C36460` | `0x1416460` | `0x92`（CFF 跳到真身） |
| `UnityEngine.Rect.Offset` 真身 | `0x7FFD74524D40` | `0x1D04D40` | 只改 `x,y`，不改 `w,h` |

DUMP 签名（`Msc.Game.Object.MobPool`）：

```
int FindHitMobInRect(Rect rect, ref List<Mob> mobs, int maxCount,
                     Mob except, int wishMobId=0, int poison=0,
                     int wishTemplateId=0, bool includeDazzledMob=false,
                     int startIndex=0)
```

Win64 实锤（`hit_pin_port.cpp` 注释 + 本轮 call site）：

```
rcx=this  rdx=Rect*  r8=List<Mob>**  r9=maxCount
[rsp+20]=except  +28=wish  +30=poison  +38=wishTemplate
+40=includeDazzled  +48=startIndex
```

`Rect` = **`UnityEngine.Rect`**：`float m_XMin, m_YMin, m_Width, m_Height`（DUMP `TypeDefIndex 8108`，16 字节）。FindHit 序言 `movups xmm0, xmmword ptr [rdi]` 一次吃满。**不是** Win32 `RECT` 的 l/t/r/b 整数。

`SkillLevelData`（DUMP `Msc.Data`，本轮字段仍对齐）：

| 字段 | 偏移 | 宽 |
|---|---|---|
| `AttackCount` | `+0x88` | int |
| `MobCount` | `+0x98` | int |
| `rcAffectedArea` | `+0xAC` | Rect 16B |
| `Range` | `+0xBC` | int |

`0xAC + 16 = 0xBC`，与 `movups [rax+0ACh]` 后紧跟 `Range` 一致。

---

## 2. 射击 `TryDoingShootAttack` · FindHit 三处

函数 `sub_7FFD73890390`。栈盒主槽：`[rbp+1E80h]` = 攻击 `Rect`（先 `xorps` 清零）。

### 2.1 技能路径：拷 `rcAffectedArea` + Offset 角色坐标

```
7FFD7389808A  add  rax, 0ACh          ; GetLevelData 返回值
7FFD73898090  movups xmm0, [rax]
7FFD73898093  movaps [rbp+1E80h], xmm0
; 虚调取角色位置 → cvttsd2si edx/r8d
7FFD7389815C  setz r9b                ; 朝向（包装第 4 参）
7FFD73898169  lea  rcx, [rbp+1E80h]
7FFD73898170  call sub_7FFD73C36460   ; → Rect.Offset
```

`GetLevelData` 调用：`xor r8d,r8d; call sub_7FFD73D9F8B0`，成功则 `rax` 为 `SkillLevelData*`。

`Rect.Offset` 真身（`loc_7FFD74524E8A` 起）：

```
addss  xmm2, xmm0          ; x += dx
addss  xmm0, xmm1          ; y += dy
movss  [rsi], xmm2
movss  [rsi+4], xmm0
; width/height 不动
```

语义：WZ 里的 `rcAffectedArea` 是**相对盒**，加上角色 AbsPos 变成世界盒。朝向位只影响 Offset 包装的跳转表，**全图方案替换整盒后朝向无意义**。

### 2.2 普攻 / NA 路径：整数角点 → float x,y,w,h

```
7FFD73897F43  movd    xmm0, edi
7FFD73897F47  movd    xmm1, r12d
7FFD73897F4C  punpckldq xmm1, xmm0
7FFD73897F50  cvtdq2ps xmm0, xmm1
7FFD73897F53  movlps  [rbp+1E80h], xmm0     ; x,y = (float)r12d, (float)edi
; … esi/eax 另一角 …
7FFD73897F73  subps   xmm1, xmm0            ; w,h = 对角 - (x,y)
7FFD73897F76  movlps  [rbp+1E88h], xmm1
```

同一条链上见 `mov esi, [rsi+0BCh]`（Range）。布局钉死为 **xywh float**，不是 ltrb 直接写进 FindHit。

### 2.3 三处 call 的 `maxCount`（种子均 `get_int` / 字段直读）

| # | call VA | r9 来源 | 实算 | 备注 |
|---|---|---|---|---|
| 1 | `0x7FFD738983E5` | `mov r9, [rbp+1F20h]`，而 `1F20` ← `r8d=[rax+98h]` | **MobCount** | 技能主扫描；`startIndex=0` |
| 2 | `0x7FFD73898551` | `r9d = 0xC3A1CECC xor dword_7FFD790B9FF4` | **1** | NA / 普攻。种子 `0xC3A1CECD`，xor = 1 |
| 3 | `0x7FFD73899233` | MBA：`((n^1) + ((2n)\|(-4)) + 2)`，`n=[rbp+1F20]` | **MobCount−1** | `arg_40=startIndex=1`；续扫，不是第二套上限 |

call 2 种子表（同一批，except/wish/poison 全 0，禁止再写「反正是 0」）：

| 槽 | 算式 | 种子 `get_int` | 值 |
|---|---|---|---|
| r9 maxCount | `0xC3A1CECC xor [790B9FF4]` | `0xC3A1CECD` | **1** |
| except | `0x7FEB4DC3 xor [790B9FF8]` | `0x7FEB4DC3` | 0 |
| wish | `0x524E9DEF xor [790B9FFC]` | `0x524E9DEF` | 0 |
| poison | `0x861BEC3C + [790BA000]` | `0x79E413C4` | 0（mod 2^32） |

call 1 / call 3 的 except/wish/poison 同样解出 0（种子见附录）。

**P2 含义**：钩 `FindHit` 入口会打到这三处。扩盒时若 `maxCount==1` 必须拒绝改参（避免 NA 假成功）。续扫 `startIndex=1` 的 `maxCount=MobCount-1` 是引擎自己的，P1 日志里把 `startIndex` 打出来，防止把两次扫描加成「打了 2×MobCount」。

---

## 3. 近战 `TryDoingMeleeAttack` · 对照（抽 4 处）

函数 `sub_7FFD7387AE40`，FindHit xref 四条。

| call VA | r9 | 实算 |
|---|---|---|
| `0x7FFD73886675` | `0x51BF504C xor [790B9934]` | 种子 `0x51BF504D` → **1**（NA） |
| `0x7FFD73887A08` | `0x0DBB9358 xor [790B9924]` | 种子 `0x0DBB9359` → **1**（NA） |
| `0x7FFD738871DD` | `r9d=edi`，`edi=[rax+98h]` | **MobCount** |
| `0x7FFD7388809B` | `r9d=esi`，`esi=[rax+98h]` | **MobCount** |

近战同样：`add rax, 0ACh` 取 `rcAffectedArea`（`0x7FFD73886D4F`）；`[rax+0BCh]` 取 Range。与射击同一套数据，不是另一套坐标系。

---

## 4. 地图 AABB 怎么塞进 FindHit

`x/features/ports/map_bounds_port.h`：`QueryPlayBounds` → `Rect{left, top, right, bottom}`，来源本图 foothold min/max。

| 字段 | 数值含义 | AbsPos（+Y 向上） |
|---|---|---|
| `left` / `right` | min x / max x | 左右 |
| `top` | **min y** | **图底**（更低） |
| `bottom` | **max y** | **图顶**（更高） |

写入 FindHit 的 `UnityEngine.Rect`（与官方 NA `cvtdq2ps` 同一布局）：

```
x = (float)left
y = (float)top          // min y，不是「屏幕上方」
w = (float)(right - left)
h = (float)(bottom - top)
```

禁止再做 `worldY = -Ap.Y`。官方已经把 AbsPos 塞进 Unity Rect；FindHit 与 mob 坐标同一空间。

空洞：AABB **看不见**内部掉落空洞（`HasFloorBelow` 注释，BIN a69130）。全图扫描要的就是外包，P2 若远处怪在 AABB 内但服端拒伤，那是服端核，不是 AABB 算错。

---

## 5. 对 P1 / P2 的硬约束（仍不写代码）

1. **不抢** `FindHit` E9。P2 只在 `hit_pin` 加默认空的 `BeforeFindHit`。  
2. 回调里若 `maxCount <= 1`：**不改 Rect、不抬 r9**，打 `na_mobcount=1`。  
3. 抬 r9 上限 `min(15, SkillLevelData.MobCount)`。MobCount 读字段 `+0x98`（泵上，FindHit 已在主泵）。  
4. 地图盒转换必须 xywh float，禁止把 `map_bounds::Rect` 当 16 字节 memcpy 进 `rdx`（那是 ltrb int，布局错）。  
5. 服端不认盒外伤害 → 结案不可做，禁止回退造包。

P1 只读日志建议字段（tag=`MapAtk`）：`maxCount`、`startIndex`、Rect `x,y,w,h`、返回 n、前 8 个 oid。零行为变化。

---

## 6. 本轮未做 / 不需要做

| 项 | 原因 |
|---|---|
| 魔法 `TryDoingMagic` 全扫 | 盗贼飞镖走射击；近战已对照同一 MobCount 模式。P1 日志若出现第三套 RVA 再补 |
| `FindHitMobInManyRects` | 多盒技能，超出「扩一个 Rect」；不挡 P1 |
| 反推 Offset 朝向跳转表 | 全图替换整盒，不走官方 Offset |
| 写 cpp / CMake / payload | 等用户点头 P1 |

---

## 附录 · 其余种子（`get_int` u32le）

射击 call 1 except/wish/poison：

- `0xD5A26E2E xor [790B9FE8=0xD5A26E2E]` → 0  
- `0x3BC0535A xor [790B9FEC=0x3BC0535A]` → 0  
- `0x763E3E44 + [790B9FF0=0x89C1C1BC]` → 0  

射击 call 3 except/wish/poison：

- `0xC8E8A6EC + [790BA004=0x37175914]` → 0  
- `0xF4CF9C34 + [790BA008=0x0B3063CC]` → 0  
- `0x2BAE700F + [790BA00C=0xD4518FF1]` → 0  

近战 NA call `86675` except/wish/poison 全 0（`[790B9938/993C/9940]`）。  
近战 NA call `87a08` 全 0（`[790B9928/992C/9930]`）。
