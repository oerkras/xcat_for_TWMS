# pet_feed P0b · 只读探针

> **状态**：✅ 已并入 P0c worker（读态 + 召唤）  
> **锚点**：随 **2026-08-03** remount（见 [`P0a_锚点复核.md`](P0a_锚点复核.md)）  
> **日志**：`bin/XCat_data/logs/petfeed.log`

## 读态（字段只读）

- `m_apPet@0x2B0` → `act` / `full`（`Pet+0xBC`）
- Consume `212xxxx` → `food*`
- Cash `500xxxx` → `summonPos`

不依赖旧包 `GetRepleteness` / `GetActivatedPetCount` RVA。
