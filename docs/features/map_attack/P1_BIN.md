# map_attack P1 BIN · 2026-08-14 21:35

> **产品**：经典版 TWMS · 本机 `bin/XCat_data/logs`  
> **结论**：P1 钩子与官方 `maxCount` **对得上**。角色是 **新手 job=0 / wt=30 单手剑普攻**（不是盗贼飞镖），A 空绑 `t=5 v=52`，官方 NA `maxCount=1`。**不能**作为 P2 扩盒验收。

## 开关

`21:35:24.108` `HitPin arm=1 target=00007FFD737A77F0 rva=0xF877F0`  
同秒 `MapAtk enabled=1 hit_pin_armed=1`  
FindHit VA 与 P0a IDB（imagebase `0x7ffd72820000` + `0xF877F0`）一致。

## 统计（`x.jsonl` + `x.jsonl.1`，tag=MapAtk，`mc=` 行 147）

| 项 | 值 |
|---|---|
| `mc` | **147/147 = 1** |
| `si` | **147/147 = 0**（未见技能续扫 `startIndex=1`） |
| `n` | 1：120；0：27；**>1：0** |
| Rect `w×h` | 仅五档：`80×40` / `64×15` / `70×56` / `71×14` / `78×67`。w∈[64,80]，h∈[14,67] |

对照：`combat.log` 全程 `path=OnFuncKey(A) t=5 v=52`。`MeleeVeto`：`job=0 wt=30`（新手单手剑）。`CharBoot` 同段 `lv=8 job=0`。图先 `40001` 后 `50001`，`lifeMob` 二十多，盒仍只收 1 只。近战 NA 与射击 NA 一样，P0a 种子都是 `maxCount=1`。

## 判定

- 钩打到了真 `FindHitMobInRect`，名单长度 = 官方 `maxCount`，P1 门禁过。  
- 这是 **近战普攻 NA**（P0a 近战两处种子同样解出 1），不是技能 `MobCount`，也不是盗贼飞镖。  
- 攻击盒是武器近距盒，**不是**地图 AABB。P1 按设计不改参。  
- 当时设计是 `mc=1` 拒改 Rect。用户已改口：近战 NA **可以先扩盒**测远距单目标（仍不抬 maxCount）。见 [`P2_扩盒.md`](./P2_扩盒.md)。
