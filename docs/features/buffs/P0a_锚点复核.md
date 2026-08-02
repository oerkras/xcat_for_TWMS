# buffs P0a · TW 锚点复核（2026-08-01）

> **状态**：✅ P0a 完成  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64` · imagebase `0x7FFB16B40000`  
> **真源**：`Dumps/runtime/out/dump.cs` + `Dumps/cms_cw/dump.cs` + `Dumps/cms_cw/user_field_map.tsv`  
> **产品**：经典版 TWMS · **不是**枫星

---

## 1. 类型哈希（TW）

| 语义 | TW TypeDef 哈希前缀 |
|------|---------------------|
| `User` / LocalUser 基类 | `c3f0cabae2a31347…` |
| `User.AffectedSkillEntry` | `…bb0350ac24a0f6cc…` |
| `SkillEntry` | `f4d54db0d6528d95…` |
| `SkillInfo`（Singleton） | `b4dbdfd3ec103102…` |
| `CharacterData` | `e83153031d52fc5e…` |
| `WorldManager` | `a480358a12395b67…` |
| `SecondaryStat` | `a5b6cdccb24498b0…`（CMS 名；TW 布局同序） |

---

## 2. 字段偏移（TW 实锤）

| 语义 | CMS | TW | 证据 |
|------|-----|-----|------|
| **`User._listAffectedSkillEntry`** | `+0x318` | **`+0x330`** | `user_field_map`（+0x18）+ runtime dump L69244 |
| `AffectedSkillEntry.SkillId` | `+0x10` | **`+0x10`** | dump |
| `AffectedSkillEntry.StartTime` | `+0x14` | **`+0x14`** | dump |
| `SkillEntry.SkillId` / `Name` | `+0x10` / `+0x18` | **同** | dump |
| `CharacterData.SkillRecord` | `+0x50` | **`+0x50`** | TW dump L80029 |
| `CharacterData.SkillCooltime` | `+0x68` | **`+0x68`** | TW dump L80032 |
| `CharacterData.ItemSlots` | `+0x40` | **`+0x40`** | 已用于 autopot |
| `WorldManager.CharacterData` | — | **`+0xE0`** | consumable/pet |
| `WorldManager.SecondaryStat` | `+0xB8` | **`+0xF0`** | invuln 实锤（**勿用 CMS +0xB8**） |

---

## 3. 方法 RVA / VA（TW · IDA 已重命名）

基址 `0x7FFB16B40000`；RVA = VA − base。

| 语义 | RVA | VA | IDA 名 |
|------|-----|-----|--------|
| **`UserLocal.DoActiveSkill(int,uint)`** | **`0x1066540`** | `0x7FFB17BA6540` | 公开总入口（对标枫星 UseOnClientImmediate） |
| **`UserLocal.GetSkillLevel(int)`** | **`0x1064F30`** | `0x7FFB17BA4F30` | `UserLocal_GetSkillLevel` |
| **`DoActiveSkillPrepare`** | **`0x10A8980`** | `0x7FFB17BE8980` | `UserLocal_DoActiveSkillPrepare` |
| **`DoActiveSkillStatChange`** | **`0x10A35E0`** | `0x7FFB17BE35E0` | `UserLocal_DoActiveSkillStatChange` |
| **`SendSkillUseRequest`** | **`0x10BD4D0`** | `0x7FFB17BFD4D0` | `UserLocal_SendSkillUseRequest`（真发包；默认不直调） |
| **`SkillInfo.GetSkill(int)`** | **`0x156DF70`** | `0x7FFB180ADF70` | `SkillInfo_GetSkill` |
| `SkillInfo.IsSkill(int)` | `0x156E210` | `0x7FFB180AE210` | — |
| `SkillInfo.GetSkillLevel(ref CD, id, ref SE)` | `0x156F9A0` | `0x7FFB180AF9A0` | — |

对齐依据：CMS `UserLocal` 方法序（Prepare / StatChange / SendSkillUse）与 TW dump L70740–70795 同序哈希。

入口多为 CFF 壳 —— **调用入口可用**；不改 `.text`。

---

## 4. 实现侧结论

```text
Presence:
  LocalUser(MyUser) → _listAffectedSkillEntry@0x330
  遍历 List → AffectedSkillEntry.SkillId@+0x10

Learn / Resolve:
  level = UserLocal.GetSkillLevel(skillId)
  entry = SkillInfo.GetSkill(skillId)   // Singleton 解析同 mob_pool

Cast (main thread):
  优先 DoActiveSkill(lu, skillId, scanCode=0)   // 对标枫星 UseOnClientImmediate
  失败回退 DoActiveSkillPrepare(lu, skillEntry, level, 0)
  （二者最终都会走到 SendSkillUseRequest；不直调发包）

Remain (optional):
  WM+0xF0 SecondaryStat：`GetRemainTime(skillId,tCur)` / 扫 rXxx_→tXxx_
  **tXxx_ = 绝对到期 ms**（同 invuln `GetTickCount()+时长`），剩余=`(t-tCur)/1000`
  禁止把裸扫小数当剩余秒（曾致 UI 167s/146s vs 图标 1/22）
```

---

## 5. 尚未钉死（实机补）

| 项 | 建议 |
|----|------|
| `tCur` 是否必须 `WorldManager.GetUpdateTime` | 现用 `GetTickCount`（与 invuln 写入同钟）；偏差再绑 getter |
| SkillInfo Singleton Lazy 布局 | 对齐 mob_pool `TryLazyValue` |
| 非 StatChange 类 BUFF（召唤等） | Prepare 通吃；验身仍靠 AffectedSkill |
