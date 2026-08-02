# pet_feed P0c · 自动召唤

> **状态**：✅ 代码已挂入 · 锚点随 **2026-08-03** remount  
> **范围**：只召唤；喂食交官方 `PetRepletenessAutoEat=50`  
> **IDB**：imagebase `0x7FFB74A20000`

## 交付

| 项 | 说明 |
|----|------|
| `TryActivatePet` | 主线程泵 → `SendActivatePetRequest` RVA **`0xC56910`** |
| Tick | `act==0` +（可选有粮）+ `summonPos` → 召；pending 5s |
| 面板 | 「自动召唤宠物」→ `petSummon`；「有粮才召」→ `petSummonRequireFood` |
| 日志 | `bin/XCat_data/logs/petfeed.log` |
| 类哈希 | `UserLocal=ac2e48…` · `CashItemManager=edfa536…` |

## 人工验收

1. 注入当前 `xcat.dll`（须含 08-03 remount）
2. 勾选「自动召唤宠物」
3. 进图收宠 → `summon try pos=…` → 宠再出
4. 有宠时不发召唤包；饱食由游戏自喂

## 风险

- Cash `nActiveState` 粘滞：场上空宠时走 `active-stuck` 回退，由服务器裁决
- 真过期宠不再 `ignore-dead`（避免刷死宠激活）
- Activate timeout 用 serial 作废晚到泵，防止误报成功
- CashItemManager Singleton 解析失败 → timeout 日志
- 禁宠图未钉
- `NowNetTicks` 必须用 FILETIME 纪元 `504911232…`，勿用 Unix `621355968…`