# teleport P0d — fill_slim 软重载结案（Classic TWMS）

> **状态**：✅ 结案（2026-08-05）· 产品路径已瘦身为 `fill_slim`  
> **产品**：新枫之谷：经典版（TW · `Maplestory_Classic.exe`）· **不是**枫星  
> **源码**：`x/features/ports/teleport_port.cpp` → `ApplyFillDoing`  
> **暴露面**：`simple_combat` 贴怪瞬移（悬崖多层图最易复现）  
> **关联**：[`模块设计.md`](模块设计.md) · [`P0c_视觉层同步链.md`](P0c_视觉层同步链.md) · [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md)

---

## 0. 一句话

挂机贴怪「飞出图外 / Field 软重载」的主因**不是**跨层落点本身，而是 **Doing 成功后我们再抢钉台 / 硬写 Ap / HealVisual**，与官方 `TryDoingTeleport` 收态打架；~80–200ms 后 Ap 漂到 `(0,0)`（或图外），触发断线边沿（`lean_local_or_soft` + MyUser drift；日志里 sticky `pendingError=205` 是 Session **哨兵**，不是踢因码——见 [`../kick_sniff/断线错误码.md`](../kick_sniff/断线错误码.md) §3）。

**修复**：Doing 后只做 `Apl←Ap`（且 Ap 近原点则跳过），日志标记 `fill_slim`。

---

## 1. 现象（用户可见）

| 项 | 描述 |
|---|---|
| 体感 | 角色突然离开可行走区 /「飞出地图」→ 本图软重载或短暂失联感 |
| 高发图 | 悬崖 / 多层台，例：`map=101030400`（FH AABB 约 `L=-113 T=-2580 R=1913 B=-694`；`(0,0)` 在底边外） |
| 低发 | 平坦同层短跳为主的图（同一套 fill，但跨层竖直跳更少） |
| 日志链 | Settling `near_zero` abort → 随后 MyUser drift / KickSniff `lean_local_or_soft`（战斗中段，非进图瞬间） |

说明：进图瞬间的 sticky `pendingError=205`（哨兵）/ `Connecting↔Connected` 是频道/进场常态，**不等于**战斗软重载。

---

## 2. 误判与纠偏

| 早期假设 | 为何不够 / 被否 |
|---|---|
| 「跨层跳 → Ap 变 `(0,0)`」 | 跨层是**触发器**（挂机打死怪立刻竖直 fill），不是充分条件；同图官方瞬移 / 裸 Doing 大距离应力测未等价复现 |
| 「落点坐标脏 / 飞到天上再掉」 | 部分样本有 `land_miss`（land 与 pos 差极大），但多数 Doing 当下 `ap=land` 正确，**延迟**才漂原点 |
| 「单纯加 Settling 闸 / 禁跨层」 | `0.1.48` 禁跨层可减复现，但牺牲多层图能力；闸门挡不住 Doing **之后**的收态竞争 |
| 「原点邻域拒填就够」 | `0.1.53` 拒 `|x|<8 && |y|<8` 作 land，挡不住 **Doing 后** Ap→0 |

**正确框架**：问题在 **我们的 fill+Doing 管道后置步骤 + 战斗节奏**，不是官方瞬移技能语义本身。

---

## 3. 根因（机制）

### 3.1 旧 `ApplyFillDoing` 相对「裸 Doing」多做了什么

典型产品路径（贴怪）：

1. 手填 `UserLocal.Teleport` Pos / IsValid  
2. 清 CoolTime（曾与 `FillTeleportPending` **双清**）  
3. 写 Mp XY  
4. **Doing 前**种 CurFh/LastFh + RelPos（贴地需要，保留）  
5. `TryDoingTeleport` + ForcedFlush  
6. **Doing 后（有害）**：再种 CurFh/RelPos、硬写 Ap、`HealVisualToAp`、再 `Apl←Ap`

第 6 步与官方 Doing 同帧/近帧收态（踏板、AbsPos←RelPos、插值滚动）**抢写**。悬崖图上出刀后立刻跨层 fill，竞争窗口被放大 → Ap 被拉成 `(0,0)` 或图外点 → 客户端判非法位置 → Field soft reload。

### 3.2 与视觉链文档的关系

[`P0c`](P0c_视觉层同步链.md) 主张「皮对齐靠 `Apl←Ap`，勿钉 Transform」。结案进一步收紧：

- **Doing 已经负责**把 Ap / 台 / RelPos 收到目标；  
- 我们再硬写 Ap / 重钉台 / HealVisual = **第二次收态**，在多层图上是软重载主因；  
- 保留的唯一后置：读 Ap，非原点邻域则 `Apl←Ap`（插值两端对齐，不抢 CurFh/Ap）。

---

## 4. 修复过程（版本时间线）

| 版本 / 阶段 | 动作 | 结果 |
|---|---|---|
| `0.1.45`–`0.1.47` | Settling PosSane / minSettle / 跨层时间闸；落点 VR/FH 边界 | 仍见跨层后 Ap→`(0,0)` |
| `0.1.48`–`0.1.49` | 禁跨层 fill；Settling `land_miss` abort | 掉图↓，但多层图 acquire miss / 能力损失 |
| `0.1.50`–`0.1.54` | `kDirectTeleportNoLayerHop` 直跳；原点邻域拒 land | 仍可在杀怪后跨层 fill 软重载（例：长包 4× `near_zero` + 多枚 MyUser drift） |
| **`fill_slim`（源码 · 用户自测标 `0.1.56`）** | 去掉 Doing 后重钉台 / 硬写 Ap / HealVisual；禁 fill 前二次 `ClearTeleportClientCooldown`；后置仅 `Apl←Ap`；`FillCam` 注记 `fill_slim` | **同毒图未再掉图**（见 §5） |

中间闸门（边界、拒原点 land、Settling abort）仍保留作**护栏**，不再当作根治手段。

### 4.1 现役 `ApplyFillDoing`（`fill_slim` / `+replant`）

```text
拒原点邻域 land
→ FillTeleportPending（CoolTime 只在此清一次）
→ Mp XY
→ Doing 前：CurFh/LastFh + RelPos（贴地）或清空台（点飞）
→ TryDoingTeleport + ForcedFlush
→ 若 CurFh 仍空：补种 CurFh/LastFh + RelPos（fill_slim+replant；BIN e27c33）
→ 仅 Apl←Ap（Ap 近原点则跳过）
→ 只读 cam probe：note=fill_slim[+replant] …
```

**禁止再引入**（产品路径）：

- Doing 后**无条件**抢种 / 硬写 Ap（CurFh 已非空时不再种）  
- Doing 后硬写 Ap  
- Doing 后 `HealVisualToAp`（瞬移收态语境）  
- fill 前再调一次 `ClearTeleportClientCooldown`（双清）

---

## 5. BIN 验收（结案样本）

| 项 | 值 |
|---|---|
| Upload | `2026-08-05_16-14-59_B9B29AE541C3AA4_008d6f7a_093bff` |
| 宣称版本 | `0.1.56 build 56`（payload 目录名可能仍带旧包名，以日志 `ver=0.1.56` 为准） |
| 地图 | `MapBounds map=101030400 src=fh …`（历史毒图） |
| `fill_slim` | 轮转日志中持续出现（百次级） |
| `near_zero` / `land_miss` / `Settling abort` / `MyUser drift` | **0**（本包战斗段） |
| `tp_ok` / `settle_ok` | 大量持续（挂机在跳） |
| `lean_local_or_soft` | 仅 **16:09:16–20 进图/连线**；战斗段未再现 |
| FillCam 典型 | `ap ≈ apl ≈ pos ≈ land`；未见 Ap→`(0,0)` |

用户体感：「这次没掉图了」——与上表一致。

### 5.1 仍可见、但非掉图

`FillCam` 可能报 `stuck=1`、`cur` 停在旧相机点：镜头探针滞后，**Ap 已贴落点**。勿与软重载混谈。

### 5.2 旁支 · 离线 VR 误杀选怪（已停用）

| 项 | 值 |
|---|---|
| 样本 | `f2b59b` / `0.1.57` · `map=107000402` |
| 现象 | F5 开着但「不打怪」：`acquire miss … noLand=36` |
| 根因 | `QueryPlayBounds` 盲信离线 VR `B=250`（地板怪 Y≈300+ 全出界） |
| 处置 | 产品闸 **停用 VR**，仅本图 FH AABB；`LandSafeForFill` 在已 Snap（`fh≠0`）时 `margin=0`（与 teleport 一致） |

真软重载靠 fill_slim；外包闸只挡离谱坐标，**不得**再引入离线 VR。

### 5.3 旁支 · 起伏地错台 / 叠台循环（已修）

| 项 | 值 |
|---|---|
| 样本① | `c0e40b` / `0.1.57` · `map=101030102` |
| 现象① | 瞬移后掉/抽台 → `reapproach_cross` 同 land 死循环（例 `to=(699,551)` vs `from.y≈494`） |
| 根因① | `EstimateLand` 偏移后 `SnapOnFh(原 fh)`：碎斜台邻段 Y 与引擎真站立面不一致 |
| 处置① | 偏移后改 `SnapStandAt`；`|ΔY|>kSameLayerY` 拒侧；两侧失败 → loose 贴怪台；`outFh` 用落点 FH |
| 样本② | `7b792b` / `0.1.59` · 同图起伏叠台 |
| 现象② | land Y≈526–528，人落定 ≈468–482；`Settling done d=32/44`（`landEps=48`）→ `reapproach_cross` / Aim `cross_layer_direct` 再贴 526 |
| 根因② | 同 X 多层 FH：`SnapStandAt` 跟怪 Y 贴到**下层**；角色收态在**上层**；`|dy|~50` 被当跨层 |
| 处置② | `SnapStandAt` 覆盖 X 且 `|fy-iy|≤72` 时优先**最上表面**；`NearMeleeFloor`（`dx<100` 且 `dy≤100`）禁 `reapproach_cross` / `cross_layer_direct`、放行 melee；跨层 `landEps` 48→16 |
| 样本③ | `b71cfd` / `0.1.60` · `map=101030102` |
| 现象③ | `Settling abort land_miss`：`land=(1336,542) pos=(1336,410)` 反复；体感「瞬移上短台再掉落」 |
| 根因③ | `fh112 (1324,543→1384,542) z=44` 的 `prev=fh96 z=10`：**异 zMass 伪 Walk 链**把 60px 短台算进长链内部 → 落点通过安全钳；Doing 短暂贴 542 后穿落到同 z 的 `fh111 Y=410`。且 `BeginMapArmGrace`→`ClearSoftBan` 后立刻重选同怪 |
| 处置③ | Walk 边仅同 zMass；同 z 链内缩后无区间的短台禁止 `SnapStandAt`/`IsXSafeOnFh`；`land_miss` 后 sticky SoftBan 8s（`kBanUnreachable`） |
| 样本④ | `5901f4` / `0.1.61` · 同图 |
| 现象④ | fh112 已不再踩；改踩 `fh148/149`（Y≈490 z=38 脊）`land→pos` 495→347；softBan 怪后**换怪**仍摔同一台 |
| 根因④ | 只 ban 怪不够；同 z 脊台链 `147–150` 上多只怪轮流 EstimateLand |
| 处置④ | `land_miss` 后 `BanLandFhSameZChain`（120s，换图清）；`LandSafeForFill`/`EstimateLand` 拒禁飞 FH（止血，非根治） |

### 5.4 根因采证 · `settle_diag`（`e27c33` / `0.1.62`）

| 项 | 值 |
|---|---|
| 样本 | `e27c33` · `map=101030102` · 2× `land_miss`（fh120 / fh148） |
| enter | **69/69 `curFh=0`**（wantFh 恒非 0）；`ap=apl=pos=land`；`rp` 仍为种植值、`rpV=0` |
| 成功 done | settle 期内引擎 **重挂** `curFh≈wantFh`，`d=0` |
| 失败 land_miss | 全程 `curFh=0`，`rp` 冻结，`ap.y` 在 ~400ms 内从 land 回漂（同 X） |
| 根因 | **`TryDoingTeleport` 清掉 CurFh**；贴住依赖 CollisionDetect 重挂；脊/岛台永不重挂 → 悬空漂移 |
| 处置 | Doing 后若 CurFh 仍空：**仅**补种 CurFh/LastFh+RelPos（`fill_slim+replant`）；**不**硬写 Ap / HealVisual |

读 BIN：`settle_diag enter` 应出现 `curFh=wantFh`（或随后 tick 立刻非 0）；Teleport 行带 `replant=1`。

### 5.5 残余 · Walk 滑链 + `rpV=nan`（`a9b624` / `0.1.63`）

| 项 | 值 |
|---|---|
| 样本 | `a9b624` · 同图 · replant 已生效（enter 74/74 `curFh=wantFh`） |
| 现象 A | 一次 `land_miss`：plant fh130 后沿 Walk 链滑走（130→131→…→121），`rpV=nan`，`d=637` |
| 现象 B | settle_ok 时 `wantFh≠curFh` + `rpV=nan` → Aim/Fire → `near_zero` / 断线（sticky `pendingError=205`=哨兵） |
| 根因 | Doing 后 InputX 锁存 → CalcWalk 积 RelPos；邻台交接偶发 `RelPos.V=nan`；放行出刀易软重载 |
| 处置 | fill 后 `SetInput(0,0)` + 零 Ap.V；Settling 拒 nan/`\|rpV\|` 过大；同点滑台主线程 `StabilizeFoothold` 一次，仍毒则 `land_miss` |

读 BIN：不应再出现 `done` 且 `rpV=nan`；偶发 `Settling stabilize` 后应 `rpV` 有限且 `curFh≈wantFh`。

### 5.6 复核 · `39722a` / `0.1.64`

| 项 | 值 |
|---|---|
| 样本 | `39722a` · `map=101030102` · ~40s |
| 通过 | enter/done **27/27** `curFh=wantFh`；**0** `land_miss`；**0** done 带 nan；1× stabilize 捕到 skate+nan |
| 残留 | stabilize 时 Ap 已漂 `d=59`（1075→1134），仍按 land **重种 RelPos** → 撕裂；`settle_ok`→出刀→`near_zero` / lean soft |
| 处置 | Ap 离 land>`kPostStabilizeLandEpsPx`(24)：**只**清锁存/零 V（`replant=0`）；stabilize 后仍 `d>24` → `land_miss` 禁出刀 |

### 5.7 复核 · `2f112a` / `0.1.65`

| 项 | 值 |
|---|---|
| 样本 | `2f112a` · 同图 · ~20s · enter 17 / done 15 |
| 通过 | done **0** nan；fh130 `land_miss`（`rpV` 炸裂、d=22>跨层 eps）未放行出刀 |
| 残留 | 跨层 fh35：enter 完好 → 77ms 起 `rpV=nan` 沿链狂奔 d=69→541；因 `d>landEps` **从不 stabilize** → ~400ms `near_zero` soft |
| 处置 | 毒化/`fhDrift` **不要求 landed** 即 latch 自愈；自愈后 `d>120` → `land_miss why=runaway` |

---

### 5.8 复核 · `4ab7b0` / `0.1.66`

| 项 | 值 |
|---|---|
| 样本 | `4ab7b0` · `noflush` 已生效（`flush=0`×20） |
| 通过 | runaway `land_miss` 1×；stabilize 能捕到 drift |
| 残留 | stabilize 在 **d=0** 仍 `replant=1` 抢 fh115↔116 交接 → 随后 d=76 `near_zero` soft；滑未停 |
| 处置 | Settling **永不 replant**；毒化时 **拆 CurFh** 断 Walk + **同拍** `land_miss why=skate_toxic`；Doing 前不写 RelPos（`+norelp`） |

### 5.9 复核 · `bbda00` / `0.1.67` ✅

| 项 | 值 |
|---|---|
| 样本 | `bbda00` · ~1.5min · `+noflush+norelp` |
| 通过 | **near_zero=0**；done 27/27 无 nan；1× `skate_toxic`（detach=1 同拍 miss）后仍可继续跳 |
| 噪声 | 多次 `lean_local` / SessionTcpLayer lost 对齐 **换图进镇**（`102000000`），非战斗滑链 soft |
| 结论 | 滑链→`near_zero` soft 路径已堵住；毒台以 miss+ban 换安全 |

---

## 6. 若复发时的差分顺序

在**不**恢复后置抢收态、**不**恢复离线 VR 闸的前提下，按侵入性从小到大：

1. ~~关 ForcedFlush（只 Doing）~~ **已做**（`+noflush`）  
2. ~~跳过 Doing 前 RelPos（仅种 CurFh）~~ **已做**（`+norelp`）  
3. `FillTeleportPending` 内不再清 CoolTime  
4. 战斗侧：出刀后延迟再 fill / 优先同 Y 再跨层（节奏，不改 Doing 契约）

Settling 护栏：

- `rpBad`/`fhDrift` → 拆台 + `land_miss why=skate_toxic`（同拍，禁出刀）  
- 禁止 settle 期 `replant=1`（4ab7b0 根因）  

每步用同一毒图 + 本文件 §5 指标对照。

---

## 7. 文档与代码指针

| 位置 | 说明 |
|---|---|
| `teleport_port.cpp` `ApplyFillDoing` | fill_slim+replant；清 InputX / 零 Ap.V；禁硬写 Ap |
| `teleport_port` `StabilizeFootholdMainThread` | Settling 遇 nan/滑链自愈 |
| `simple_combat` Settling | 拒 `rpV` 毒化；stabilize 一次后仍毒 → land_miss |
| `FillCam` / `player_combat::LogFillDoingCamProbe` | `note=fill_slim …` |
| `map_bounds_port` | 仅 FH AABB；离线 VR 不进产品闸 |
| `simple_combat` `LandSafeForFill` | Snap 后 `margin=0`；Settling `near_zero`/`land_miss` 仍为护栏 |
| `common/xcat_map_bounds.*` | dump/表热载残留；**勿**再接回 PointInPlayBounds |
| [`模块设计.md`](模块设计.md) §2 | fill_slim + FH 边界闸 |
| [`P0c` §7.1](P0c_视觉层同步链.md) | 后置只 `Apl←Ap` |
