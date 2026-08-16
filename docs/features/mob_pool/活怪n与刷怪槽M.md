# 活怪 n 与刷怪槽 M（Classic TWMS）

> **状态**：✅ 已落地 `mob_pool_port` + `mob_scan` 日志 + titlebar `怪 n/M`  
> **源码**：`x/features/ports/mob_pool_port.{h,cpp}` · `x/features/mob_scan/mob_scan.cpp`  
> **BIN 样例**：`bin/XCat_data/logs/mobscan.log`（如 `n=11/M=25 map=1000005 lifeMob=25 raw=11`）

---

## 0. 一句话

| 符号 | 是什么 | 不是什么 |
|------|--------|----------|
| **n** | 此刻 **MobPool** 里通过过滤的**活怪数** | 不是刷怪点总数 |
| **M** | 地图 **LifeList** 里 `Type==Mob` 的**刷怪点容量** | 不是「场上应永远站满」的数 |

**`n ≤ M` 且经常明显小于 M，是官方刷怪引擎的正常现象，不是统计漏算。**

---

## 1. 本仓实现（SSOT）

### 1.1 快照字段（`ports::mob::Snapshot`）

| 字段 | 日志 | 含义 |
|------|------|------|
| `count` → **n** | `n=` | 入榜活怪数（≤128，超则 `trunc=1`） |
| `spawnSlots` → **M** | `M=` | 刷怪槽；优先 `lifeMob`，失败用本图峰值 |
| `rawDict` | `raw=` | MobPool 字典里有效 Mob 指针数（活怪过滤**前**） |
| `nInView0` | `iv0=`（`fill_rej` / 摘要） | 入榜但 `inView=0` 的活怪数（不挡 n） |
| `lifeMob` | `lifeMob=` | `MapData.LifeList` 中 `Type==Mob(1)` 条数 |
| `lifeAll` | `lifeAll=` | LifeList 总条数（含 NPC 等） |
| `mapId` | `map=` | `MapData.Id` |
| `truncated` | `trunc=` | 是否顶到 `kMaxLiteMobs=128` |

`mobscan.log` 行格式：

```text
mobscan n=…/M=… map=… lifeMob=… lifeAll=… raw=… trunc=… mapKey=… [样本怪…]
```

标题栏：`怪 n/M` ← `GetCachedAliveCount()` / `GetCachedSpawnSlots()`（同源缓存）。

### 1.2 n（活怪）怎么采

1. 解析 **MobPool** Singleton → 读池内 `Dictionary`（`MobPool+0x10`）  
2. 遍历 entries：`hash≥0` 且 value 像堆指针 → 计入 `raw`  
3. `FillLite` 过滤后才进 `n`：

| 条件 | 要求 |
|------|------|
| Unity 对象存活 | `m_CachedPtr` 等存活检查 |
| 类型 | klass 为 Mob |
| `id` | ≠ 0 |
| `ready` | `IsReady != 0` |
| `deadType` | `== 0` |
| `hpPct` | `> 0` |
| `suspended@0x1B8` | `== 0` |
| `tpl` | `!= 9999999`（地图特殊体） |
| `inViewSplit@0x100` | **不挡入榜**（只写入 `MobLite.inView`；FindHit 出刀仍要它。BIN：当硬门 → 下层活怪假空图落地） |

> **不要**用 `VecCtrl.Active@0x80` 挡活怪：`SetRemoteMob` 置 false 后怪仍可命中；BIN（`1000002`）曾因此 `raw>0 n=0`。  
> **不要**把 `inViewSplit` 当成「场上有没有怪」：它是命中资格，不是存在性。  
> **出刀侧**：`simple_combat` 锁怪可带 `iv=0`（避免假空图），但 `Firing` 在 `!inView` 时**只贴飞不砍**（`fire hold iv=0`）；**站稳后**约 2.5s 仍不可命中则 `iv0_timeout` + `kBanUnreachable`(2.5s) 换怪；飞近纠位期间不计超时。避免 FindHit 拒刀空挥吃 whiff softban。  
> `n>M` 若仍出现且样本怪坐标/tpl 正常，更像池内真实多怪（非失活留尸），与 LifeList 容量不必强行相等。

失败时回退 `FindAll(Mob)`（同样走 `FillLite`）。

### 1.3 M（刷怪槽）怎么采

| 优先级 | 做法 |
|--------|------|
| **1 权威** | `WorldManager`（`ports::world`）→ `_currentMapData@+0x88` → `LifeList@+0x38`，计 `MapLifeData.Type@+0x20 == Mob(1)` → `lifeMob` / `spawnSlots` |
| **2 回退** | 按 `mapId` 记录本图见过的最大 `n`（`UpdateSpawnPeak`），LifeList 读失败时用峰值 |

`MapLifeData` 相关（CMS≡TW 布局，打怪点几何）：

| 字段 | off | 用途 |
|------|-----|------|
| X,Y | +0x24 / +0x28 | 槽位 AbsPos（Y 已是 −WZ.y；更大 Y = 更高） |
| Type | +0x20 | 0 Unknown / **1 Mob** / 2 Npc |
| Rx0,Rx1 | +0x38 / +0x3C | 巡逻横区间（与 X 同号） |
| ID | +0x1C | 模板 id |
| MobTime / Fh | +0x2C… | 重生间隔 / 台 |

类哈希锚点见 `mob_pool_port.cpp` 头注释（MobPool / Mob / MapData）。

### 1.4 自洽判据（BIN）

| 观察 | 含义 |
|------|------|
| `raw ≈ n` | 池内条目几乎都是就绪活怪；过滤没有乱砍半池 |
| `n ≤ M`（稳态） | 常见；若长期 `n>M` 且样本怪正常，先当真实多怪/事件怪，勿用 Active 硬砍 |
| `lifeMob == M` 且稳定 | LifeList 读通，不是峰值瞎回退 |
| `lifeAll ≥ lifeMob` | 差值多为 NPC 等非 Mob life |
| `trunc=0` | 未顶 128 上限 |
| `n` 在 10～11 间抖 | 死/刷正常；不必等于 M |

离线 `Dumps/offline_tables/json/Map.json` **只有街/图名**，不能硬核验某图 Life 条数；以运行时 LifeList 为准。

---

## 2. 发现：为什么经典图经常 `n` 远小于 `M`

### 2.1 概念分离

- **M** = 地图数据里配置了多少个**刷怪点**（WZ / `MapData.LifeList`）  
- **n** = 服务器当前允许、且已生成在客户端 **MobPool** 里的活怪  

点位数 ≠「同时必须站满」。

### 2.2 官方刷怪引擎（MSC / 经典系共识）

场上并发受多道闸约束（社区/私服对照 + MSC 公开说明，**非本仓逆向服务端**）：

1. **全局重生 tick**（常见约 **7.56s** 一波）  
   怪死后不会立刻补满；要等下一次 tick 才可能刷点。
2. **每点冷却**（常见约 5s，或点上的 **MobTime**）  
   冷却未满，即便 tick 到了也不刷该点。
3. **地图容量随人数缩放**（MSC 常见模型）  
   - 单人约 **~75% × 刷怪点数**  
   - 人越多逐步抬向 100%，满员才接近 `n≈M`  
   - WZ 的 `mobRate` 在 MSC 上**不一定**参与容量（以实机为准）

因此：**单人刷图时标题长期 `怪 n/M` 不满格是预期行为**；组队更接近满槽。

### 2.3 对本仓 BIN 的解读（例）

`草叢狩獵場Ⅰ`（`map=1000005`）：

```text
mobscan n=11/M=25 lifeMob=25 lifeAll=25 raw=11 trunc=0
```

- `M=25`：本图 25 个 Mob 刷怪点（LifeList 全是 Mob）  
- `n=11`（约 44%）：低于「单人 ~75%」理论值也常见——叠加**刚进图补怪中**、**自动打怪在清场**、部分点冷却/未到 tick  
- `raw==n`：漏统计的嫌疑低；缺的是**服务器没刷满**，不是客户端字典少扫

### 2.4 与肉眼数怪

`n` 与「屏上可见怪」可能差 1～2（刚死未出池、未 `ready`、屏外）。  
以 `raw`/`FillLite` 条件为准做验收，不要拿「必须 n==M」当 bug。

---

## 3. 相关文档

| 文档 | 关系 |
|------|------|
| [`mob_scan/模块设计.md`](../mob_scan/模块设计.md) | worker 周期、事件唤醒、按需刷新、MobCtrl、相对旧实现优势、面板 `mobScanIntervalMs` |
| [`simple_combat/模块设计.md`](../simple_combat/模块设计.md) | 消费缓存选怪；刷怪槽对照枫星；详表以本文为准 |
| [`titlebar/模块设计.md`](../titlebar/模块设计.md) | 标题 `怪 n/M` 展示 |
| [`world_manager/字段全表.md`](../world_manager/字段全表.md) | `MapData.LifeList` 锚点 |
| 枫星对照 | `xcat_for_fengxing` spawnSlots / Life 子节点（模式复用，偏移不照搬） |
