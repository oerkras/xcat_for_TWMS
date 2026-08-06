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
| `SkillEntry` | `f690543f60fcc48d…`（TDI 2149） |
| `SkillInfo`（Singleton） | `ae17de4134212468…`（TDI 2164；**勿** `eff831ff…`=ItemDataManager） |
| `CharacterData` | `e83153031d52fc5e…` |
| `WorldManager` | `a480358a12395b67…` |
| `SecondaryStat` | `a5b6cdccb24498b0…`（CMS 名；TW 布局同序） |

---

## 2. 字段偏移（TW 实锤）

| 语义 | CMS | TW | 证据 |
|------|-----|-----|------|
| **`User._listAffectedSkillEntry`** | `+0x318` | **`+0x330`** | 明文字段；**runtime** `skill_port` `FieldOffWalk` |
| `AffectedSkillEntry.nSkillID` / `tStart` | `+0x10` / `+0x14` | **同** | 明文；nested klass meta |
| `SkillEntry` id / Name | `+0x10` / `+0x18` | **同** | hash `dd351efc…` / `fbd8e637…` → skill_port |
| `SkillInfo` 主字典 | — | **`+0x10`** | hash `d39be012…` → skill_port |
| `FieldActorBase.Pos`（存活探针） | `+0x64` | **`+0x64`** | hash `c9d7ef43…` / 明文 `Pos` |
| `LocalUser.CurPos` | `+0x240` | **`+0x240`** | hash `b992bfa5…` / 明文 `CurPos` |
| `CharacterData.SkillRecord` | `+0x50` | **`+0x50`** | TW dump L80041；**runtime SSOT** `player_vitals` hash `ba336108…` |
| `CharacterData.SkillRecordEx` | — | **`+0x58`** | hash `e3d90051…` → vitals |
| `CharacterData.SkillCooltime` | `+0x68` | **`+0x68`** | hash `f8c076a1…` → vitals |
| `CharacterData.SkillCoolTimeOver` | — | **`+0x70`** | hash `d6b56905…` → vitals |
| `CharacterData.ItemSlots` | `+0x40` | **`+0x40`** | 已用于 autopot / shop |
| `WorldManager.CharacterData` | — | **`+0xE0`** | consumable/pet / vitals |
| `WorldManager.SecondaryStat` | `+0xB8` | **`+0xF0`** | hash `aae31a63…` → vitals（**勿用 CMS +0xB8**） |
| `UserLocal.PreparingSkill` | — | **`+0x398`** | hash `a5df568c…` → skill_port（SkillID@+0） |

**容器布局 SSOT**：`x::runtime::il2cpp_container`  
- `Dictionary` / `List` / `Queue` / `Stack` / `HashSet` 字段 → `field_get_offset`  
- fb：Dict ent`0x18` free`0x28`；List items`0x10` size`0x18`；Queue arr`0x10` head`0x18` size`0x20`；Stack arr`0x10` size`0x18`；HashSet slots`0x18` cnt`0x20`  
- **Il2CppArray 原生头**（ABI 常量，非 field meta）：`max_length@0x18` / `data@0x20` → `OffArrayMaxLength` / `OffArrayData`；`il2cpp_bind::ArrayLen/At` 已走 SSOT  
- **Dict Entry 步长**：`DictEntryStrideIntPtr(0x18)` / `IntIntTight(0x10)` / `IntIntAlign(0x18)` + `DictEntryAt`；value@`0x10`/`12`/`16`  
已接线：`skill_port` / `drop_pool` / `security_attack` / `foothold` / `mob_pool` / `shop` / `pet` / `consumable` / `user_pool` / `travel` / `auto_enter` / `anti_macro` / `channel_hop` / `titlebar_game` / `kick_sniff` / `worldmap` / `player_vitals` / `attack_rpc`。  
日志：`Il2CppContainer field off path=… hits=…/22`（Array/Entry 不计入 hits）。

**游戏类容器槽 SSOT**：`x::runtime::il2cpp_network`（NM Facade + Session）  
- Facade：Session / SessionState / PacketQueue / OpcodeHashSet  
- Session：PendingError / RecvList / State / Closed / SeqSend  
日志：`Il2CppNetwork … hits=…/9`。已接线 `kick_sniff` / `shop` / `attack_rpc` / `travel`。  
另：`MobPool` dict、`NpcPool` list 在各自 port 内 hash→field。

**地图槽 SSOT**：`x::runtime::il2cpp_mapdata`  
- WM.`currentMapData`、MapData.`id/LifeList/Portals/FootholdMap/LadderRopes`、PortalManager.`_portalList`  
日志：`Il2CppMapData … hits=…/7`。已接线 `foothold` / `travel` / `mob_pool` / `world_port`。

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
| **`SkillInfo.GetSkill(int)`** | **`0x1575210`** | dump.C L95168 | 返回 SkillEntry；**勿** `0x1432D20`（ItemDataManager） |
| `SkillInfo.IsSkill(int)` | `0x156E210` | `0x7FFB180AE210` | — |
| `SkillInfo.GetSkillLevel(ref CD, id, ref SE)` | `0x156F9A0` | `0x7FFB180AF9A0` | — |

对齐依据：CMS `UserLocal` 方法序（Prepare / StatChange / SendSkillUse）与 TW dump L70740–70795 同序哈希。

入口多为 CFF 壳 —— **调用入口可用**；不改 `.text`。

---

## 4. 实现侧结论

```text
Presence:
  LocalUser(MyUser) → _listAffectedSkillEntry（meta，fb@0x330）
  遍历 List → AffectedSkillEntry.nSkillID / tStart（meta）

Learn / Resolve:
  level = UserLocal.GetSkillLevel(skillId)
  entry = SkillInfo.GetSkill(skillId)   // Singleton 解析同 mob_pool

Cast (main thread):
  优先 DoActiveSkill(lu, skillId, scanCode=0)   // 对标枫星 UseOnClientImmediate
  失败回退 DoActiveSkillPrepare(lu, skillEntry, level, 0)
  Prepare 态：UserLocal.PreparingSkill（hash a5df568c…，fb@0x398）.SkillID≠0
  （二者最终都会走到 SendSkillUseRequest；不直调发包）

Remain (optional):
  WM+0xF0 SecondaryStat：`GetRemainTime(skillId,tCur)` / 扫 rXxx_→tXxx_
  **tCur = WorldManager.GetUpdateTime**（`_updateTime*1000`）；**禁止 GetTickCount**
  **tXxx_ = 绝对到期 ms**（游戏钟），剩余=`(t-tCur)/1000`；API 返回剩余 ms
  CD：优先 `CharacterData.SkillCoolTimeOver@+0x70`（绝对到期），回退 `SkillCooltime@+0x68` ushort
  禁止把裸扫小数当剩余秒（曾致 UI 167s/146s vs 图标 1/22）
```

---

## 5. 尚未钉死（实机补）

| 项 | 建议 |
|----|------|
| `tCur` 是否必须 `WorldManager.GetUpdateTime` | **是**（`skill_port` 已绑 RVA `0xDBC9D0` / 静态 `_updateTime*1000`）；`GetTickCount` 会让 remain 冻结 |
| SkillInfo Singleton Lazy 布局 | 对齐 mob_pool `TryLazyValue` |
| 非 StatChange 类 BUFF（召唤等） | Prepare 通吃；验身仍靠 AffectedSkill |
