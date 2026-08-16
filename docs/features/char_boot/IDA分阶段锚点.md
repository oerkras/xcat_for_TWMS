# char_boot · IDA / DUMP 分阶段锚点（Classic TWMS）

> **产品**：新枫之谷：经典版 · **不是**枫星  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64`  
> **本轮采样**：2026-08-14 · `imagebase=0x7ffd72820000` · `auto_analysis_ready=true` · Hex-Rays 可用  
> **对照 dump**：`Dumps/runtime/out_0814/dump.cs`（字段布局）+ `out/dump.cs.restored`（明文类名）  
> **契约真源**：[`模块设计.md`](./模块设计.md)  
> **方法**：CFF 函数不靠伪代码；RVA + 种子实算 + 包形 + 离线表。

本轮把起号 SM 拆成阶段，只钉**本仓 dump / 当前 IDB**能核对的东西。脚本对话正文、INT20，客户端没有完整脚本 VM 可读源，标 **BIN**。船费 150 **必收**（产品口径，不是 IDA 解出来的）。登录建角见 **§12**（选角页，不是局内 SM）。

---

## 0. 当前 IDB 对照（先纠规格笔误）

| 项 | 规格旧值（错） | 本轮真值 |
|---|---|---|
| `UserLocal.TalkToNpc` RVA | `0x10a5eb0` | **`0x10a5c50`**（与 `shop_port.cpp` `kRvaUserLocalTalkToNpc` 一致） |
| 方法哈希 | `dc981e1d…090cc7` | **`ba3d7a86…4b618d`**（`kHashTalkToNpc`） |
| VA（本 IDB） | — | `0x7ffd72820000+0x10a5c50` = **`0x7ffd738c5c50`** |
| 函数名 | 未改名 | 已改名 `UserLocal_TalkToNpc`（书签 `char_boot:`） |

`0x10a5eb0` 与实函数差 `0x260`，不是「另一个重挂」，是规格抄错。落码跟 `shop_port`，不要另写一套 RVA。

`WM.CheckMovePortal` RVA `0xDE5930` → VA `0x7ffd73605930`，已改名 `WM_CheckMovePortal`。产品进门 **禁止** StickUp 后再直调它（travel BIN：~100ms 断线）。`jobin00` / `jobout00` 同样走 StickUp + `unity_kbd` ↑。

---

## 1. 阶段总览

```
Farm（岛） → GotoHarbor → BoardShip → GotoEllinia
  → EnterLib(jobin00) → JobTalk(汉斯) → WaitJob
  → GotoHangup（种子含 jobout00）→ Done
```

| 阶段 | 客户端要动的东西 | 本轮结论 |
|---|---|---|
| Arm / Farm | 只读 vitals | 字段铁。INT20 **不是**客户端文案里的铁 |
| GotoHarbor / GotoEllinia / GotoHangup | travel 同盘 BFS | 图论铁；码头除外 |
| BoardShip | TalkToNpc(22000) + UI | CALL 铁；**150 必收**（入态 `meso>=150`，扣费后内环不再查 meso） |
| EnterLib | `FirePortalByName("jobin00")` | 脚本门 pt=7，**种子没有进厅边** |
| JobTalk | TalkToNpc(1032001) + UI | CALL 同船；转职条件在服务端 |
| ExitLib | 可选 | 种子 **有** `jobout00` pt=1 → `101000000` |
| Hangup | `101010000` | `map_info.town=0`，室外 `top00` 一跳 |

---

## 2. Arm / Farm — 只读，不进 IDA 热路径

`CharacterStat`（`player_vitals.cpp` 哈希 = 0814 dump TypeDef **1838** 字段名；08-06 注释写 1833 是旧 dump 编号，**偏移未漂**）。

IDA 写入点：`CharacterStat_DecodeFromPacket` RVA `0x12DE520`（`mov [rsi+38h],al` / `mov [rsi+3Ah],ax` / `mov [rsi+40h],ax`）。编排只读 `player_vitals`，不必自己解包。

| 字段 | 偏移 | 0814 字段哈希（与 `player_vitals` 一致） |
|---|---|---|
| level | `+0x38` | `ba9fa222…2134b295` |
| job | `+0x3A` | `ed62c47d…ba932cc7c` |
| nINT | `+0x40` | `c73cabfb…fc14281113` |
| ap / sp | `+0x4C` / `+0x4E` | `c7751ced…` / `ca11124c…` |
| meso | `+0x58` | `c0b441b5…5358982c` |

法师 8 级：教程任务 **1013 / 1036 / 1009** 明文「法师 8 级、其余 10 级」。这是岛上 NPC 测验文案，不是汉斯函数里的 `cmp 8`。

INT 20：离线 `quest_say_text` / `Npc.json` 汉斯条目 **没有**「智力 20」。1029 只说转职除等级外要准备**属性**；1036 解释「把点加智力的初心者升级更难」。**客户端字符串钉不住 20**。保持 `requireInt20` 可关假设，BIN 跟汉斯对话。

`40000`：`map_info.town=0`，`returnMap=60000`。死亡回港再 GotoFarm 与表一致。

---

## 3. BoardShip — TalkToNpc + 桑克斯

### 3.1 NPC / 图（DUMP）

`map_life.tsv`（零填充模板号）：

| map | life | tpl | 坐标 |
|---|---|---|---|
| `60000` | n | `0022000` = **22000 桑克斯** | `(3360, -222)` |
| `60000` | n | `0020002` = **20002 比格斯** | `(585, 522)` |

比格斯更靠西侧出生点。`preferTemplateId=22000` 是硬条件，不是优化。

`Npc.json` 桑克斯闲话：航海/船长，**没有**菜单原文。`Map.json` `60000` ObjectDesc：

> 費用 : 150 楓幣  
> 透過桑克斯可使用航行維多利亞港的船隻。**無法再返回原地。**

**产品口径：150 必收。** 任务 1040「推荐函免费」不走：不做推荐函检测，港上 meso<150 也不 Talk。离岛 `readyToLeave` 含 `meso>=150`；人已在 `60000` 但不够 → Fail `ship_fare`。List 关键词不要放「免费」。

`60000` `map_info.town=1`。不能当 `farmMap`。

### 3.2 TalkToNpc 反汇编（CFF，本 IDB 实算）

`UserLocal_TalkToNpc` @ `0x7ffd738c5c50`，size `0x324`。伪代码只有跳表，**作废**。

约定：`rcx=UserLocal(this)` → `rsi`；`rdx=Npc` → `rdi`。

可核对的直指令：

| 地址 | 指令 | 语义 |
|---|---|---|
| `…5e7f` | `movzx ecx, word_7FFD790BB0E0` ; `add ecx, 20DCh` ; `call Create` | opcode 种子 |
| `…5e99` | `mov edx, [rdi+78h]` | `NpcObjectId` |
| `…5ebe` | `call Encode4` @ RVA `0x1CF8E00` | 与 `shop_port` 同 |
| `…5ec3` | `mov rdx, [rsi+64h]` | 玩家 `Vector2` @ `+0x64` |
| `…5ed3` | `call Encode(Vector2)` @ RVA `0x1CF99D0` | |
| `…5f4c` | `call` RVA `0x1CEC4F0` | 发出 |
| `…5f0d` | `il2cpp_runtime_class_init` | **必须 MainPump** |

opcode 种子（运行时 dump，磁盘原文件会读成 0）：

```
word_7FFD790BB0E0 = 0xDF64
0xDF64 + 0x20DC = 0x10040 → 低 16 位 0x40 = 64 = UserSelectNpc
```

与 P0b「槽 64」一致。应答仍走 UI（66），不手组。

另有两处常量混淆（本 IDB `get_int`）：

```
IMM 0x8DAEAD6A + seed 0x72515298 = 2 (mod 2^32)
  xor al,1 ; add eax,eax  →  (!pred)*2  cmp  2
IMM 0x529CCD14 + seed 0xAD5CCD14 = 0
```

第一处是虚调用 `[vtable+0x1C8]` 的布尔门（方向按「解出 2 + 预缩放」读，**不要当 0**）。随后还读 `UserLocal+0x418` 字节。Talk 无反应时 BIN 先看这两处，不要先改 opcode。

### 3.3 对话 UI

与开店同一套。本 IDB 已用 getter 直指令钉死（见 §11）：

| 字段 | 偏移 | IDA |
|---|---|---|
| Type | `+0xA0` | `SetUtilDlgEx` `mov [rsi+0A0h], ebp` @ RVA `0x7A3FDA` |
| Result | `+0xA4` | `UIUtilDialogEx_get_Ret` `mov eax,[rcx+0A4h]` |
| List 文案 `List<string>` | `+0xE0` | `UIUtilDialogEx_get_MenuTexts` `mov rax,[rcx+0E0h]` |
| Say/YesNo 正文 `string` | `+0x120` | `UIUtilDialogEx_get_SayText` `mov rax,[rcx+120h]` |
| SelectIndex | `+0x114` | dump 字段；无 4 字节 getter（`SetKeyFocus(int)` CFF） |

Type 枚举（restored / TypeDef 608）：`Text=0` `YesNo=1` `InputNo=2` `InputStr=3` `List=4` `Avatar=5` `Pet=6` …。开店口径 0=Say 即这里的 `Text`。

点选 RVA（0814 dump = `shop_port` 现码，**未漂**）：`SetKeyFocus 0x7978C0` / `OnClickBtOk 0x7A02E0` / `OnClickBtYes 0x7A0390` / `OnClickBtNo 0x7A03F0`。

**禁止**抄 `shop_port` List 无关键词就点第 0 项（杂货店才那样兜底）。船/转职菜单第 0 项可能是废话或去错岛。

List 预置词补：`維多利亞` `维多` `乘船` `搭船` `前往` `150` `楓幣`。无命中 = Rejected。不要加「免费」。

成功谓词：**只认** `GetMapId()==104000000`。客户端无航船等待图号。`104000000` `map_info.town=1`。

---

## 4. GotoEllinia — 图论，无新 CALL

港 `104000000` → 魔法森林室外 `101000000`：同盘 BFS，墙 600s。travel **不做**码头；船已经在上一阶段做完。

`101000000`：`map_info.town=1`，`% 1000000==0`，不能当 `hangupMap`。

同图还有脚本门 `in04` / `enterERShip` `(-702,-3123)`。EnterLib **只**开火 `jobin00`，禁止扫「任意 pt=7」。

---

## 5. EnterLib — `jobin00`（最高风险）

`travel_script_portal.tsv`：

```
101000000  jobin00  pt=7  enterMagiclibrar  (334, -3987)
```

种子 **没有** `101000000 → 101000003`。`RequestGoto(101000003)` 会 Unreachable。这是禁止直跳图书馆的铁证。

`pt=7` 目标靠脚本，不靠 tm。进门产品路径 = `FirePortalByName` StickUp + ↑，与普通野图门相同，**不要** DirectEnter / CheckMovePortal。

AbsPos：更大 Y = 更高。`-3987` 是树顶。单次贴门上限 14s，必须重试。

失败码三分：`NO_PORTAL` / `STICK_FAIL` / `FIRED_NO_WARP`。脚本门是否进 PortalManager、↑ 是否触发 `enterMagiclibrar`：**BIN 刀 3**。

---

## 6. JobTalk — 汉斯

`map_life`：`101000003` n `1032001` `(-7, -77)`。室外 `101000000` **没有**汉斯。

`Npc.json`：

```
1032001 漢斯  func=法師轉職官
talk: 若想要變成法師的人，請來找我！
```

任务名 `1050 法師轉職`、`2080 法師之路`。`quest_say_text` **没有** 1050 正文（脚本不在这张表）。转职判定（8 级、INT、job==0）在 **服务端**。客户端能做的只有 Talk + 点选 + 等 `CharacterStat.job` 变成 `200`。

List 预置：`魔法師` `法师` `轉職` `转职` `成為` `成为`。汉斯后续任务（2132 修炼）菜单也可能出现——**无命中不要点第 0 项**，避免把转职点成修炼。

`101000003` `map_info.town=1`，`returnMap=101000000`。馆内死亡会回室外，再点开始走跳态 EnterLib，不会自动跟汉斯续对话。

CALL 与船相同，必须泵上（函数内 `runtime_class_init`）。

---

## 7. ExitLib / GotoHangup

种子（普通门，不是脚本）：

```
101000003  jobout00  pt=1  tm=101000000
101000000  top00     pt=1  tm=101010000
101010000  east00    pt=1  tm=101000000
```

从图书馆 `RequestGoto(101010000)` 图论上 **通**（出馆 + 北郊一跳）。这与进厅不对称：进厅没有 tm 边，出门有。

`jobout00` **不在** `travel_script_portal.tsv`。ExitLib 不必默认 Fire。契约改为：

1. `GotoHangup` = `RequestGoto(hangupMap)`（允许当前图是 `101000003`）
2. 仅当 `Unreachable` 且仍在图书馆 → 再 `FirePortalByName("jobout00")` 后重试 Goto

`101010000` 魔法森林北郊：`map_info.town=0`，`returnMap=101000000`。默认挂机图 **不是城镇**。死亡回魔法森林室外，GotoHangup 再走 `top00`。

---

## 8. 对话点选（两阶段共用）

| Type | 偏移 | 行为 |
|---|---|---|
| 0 Text/Say | 正文 `+0x120` | 可读；v1 仍可盲点 Ok |
| 1 YesNo | 正文同一 `+0x120` | 可读；v1 仍可盲点 Ok |
| 4 List | 文案 `+0xE0` | 关键词命中才 `SetKeyFocus`+Ok |
| 其它 | — | Fail `BadType` |

Say 正文本轮已钉 `+0x120`（Il2CppString）。Unity `Text` 组件在 `+0x1B0`，那是显示层，匹配关键词用 `+0x120` 即可。v1 策略仍允许盲点 Ok，但**不要再说「正文未钉」**。

---

## 9. 本轮未钉（BIN / 别假装）

| 项 | 为什么 IDA 不够 |
|---|---|
| 汉斯 INT 20 | 无客户端字符串、无本函数 `cmp 20` |
| `jobin00` StickUp 能否进脚本门 | 运行时 PortalManager + ↑ |
| Talk 远距 | `TryTalkNearestNpc` 全图 8000；桑克斯 x=3360  theoretically 够，BIN |
| `UserLocal+0x418` 语义名 | 类型已钉 `bool`；`test cl,cl / jnz`。中文名仍要运行时 Talk 失败样本 |
| vtable+0x1C8 谓词是哪个虚函数 | 槽位已钉；反转后 `*2` 再 cmp 解出常量 2。具体方法名 BIN |
| 汉斯对话脚本正文 | 不在 GA；点选用 List `+0xE0` / Say `+0x120` |

---

## 10. IDA 书签 / 已改名

`imagebase=0x7ffd72820000`。CFF 函数伪代码作废，只认直指令。

| 前缀 | VA | 名 |
|---|---|---|
| `char_boot:` | `0x7ffd738c5c50` | `UserLocal_TalkToNpc` |
| `char_boot:` | `0x7ffd73605930` | `WM_CheckMovePortal` |
| `char_boot:` | `0x7ffd72fb14d0` | `UIUtilDialogEx_get_MenuTexts` List `+0xE0` |
| `char_boot:` | `0x7ffd72fb14e0` | `UIUtilDialogEx_get_SayText` `+0x120` |
| `char_boot:` | `0x7ffd73afe8ac` | `CharacterStat.job` 写入 `+0x3A` |
| `char_boot:` | `0x7ffd73d301b0` | `MapPortalData_get_Script` `+0x40` |
| `char_boot:` | `0x7ffd737b9660` | `Npc_get_ObjectId` `+0x78` |

---

## 11. 起号所需偏移总表（本 IDB 已钉）

**落码契约**：char_boot **不要**再抄一套 fallback。哈希/偏移跟现码走：

- vitals → `player_vitals.cpp`
- Talk / NpcPool / Dialog → `shop_port.cpp`
- 门 / AbsPos / mapId → `travel_port.cpp` + `il2cpp_mapdata.cpp`

本表是 0814 dump + 当前 IDB getter/decode 的核对，证明那些 fallback **没漂**。

证据口径：`dump.cs` 字段布局 + IDA `mov eax/rax,[rcx+off]` / `mov [rsi+off],…`。常量混淆只出现在 TalkToNpc 布尔门，字段偏移本身不混。

### 11.1 Arm / Farm / WaitJob / WaitSpend — CharacterStat

链：`WM+0xE0` CharacterData → `CD+0x10` CharacterStat。

| 对象 | 字段 | off | IDA 证据 | 现码 |
|---|---|---|---|---|
| WorldManager | CharacterData* | `+0xE0` | 0814 dump `a3ce8101… @ 0xE0` | `kFbWmCharacterData` |
| WorldManager | MapData* | `+0x88` | 0814 dump `d469e5a5… @ 0x88` | `il2cpp_mapdata::OffWmMapData` |
| WorldManager | fieldKey | `+0x80` | 0814 dump `b6671f7d… @ 0x80` | `kHashWmFieldKey` |
| MapData | Id | `+0x10` | 0814 dump `Id @ 0x10` | `OffMapId` |
| CharacterData | CharacterStat* | `+0x10` | 0814 `cd2cb764… @ 0x10` | `kFbCdCharacterStat` |
| CharacterStat | level `byte` | `+0x38` | decode `mov [rsi+38h], al` RVA `0x12DE89F` | `kFbCsLevel` |
| CharacterStat | job `short` | `+0x3A` | decode `mov [rsi+3Ah], ax` RVA `0x12DE8AC` | `kHashCsJob` |
| CharacterStat | nINT `short` | `+0x40` | decode `mov [rsi+40h], ax` RVA `0x12DE8D6` | `kHashCsInt` |
| CharacterStat | ap / sp | `+0x4C` / `+0x4E` | decode 同函数 | `kFbCsAp` / `kFbCsSp` |
| CharacterStat | money `long` | `+0x58` | decode B `mov [rsi+58h], rax` RVA `0x12EA5F7` | `kFbCsMoney` |

WaitJob：只读 `job`，等 `0→200`。写入 RVA `0x12DE520` 已钉，**不必**追 S→C opcode。

### 11.2 Talk / 找 NPC

| 对象 | 字段 | off | IDA 证据 | 现码 |
|---|---|---|---|---|
| NpcPool | `_npcList` | `+0x10` | `NpcPool_get_List` `mov rax,[rcx+10h]` RVA `0xFACA90` | `kFbNpcPoolList` |
| Npc | NpcObjectId | `+0x78` | `Npc_get_ObjectId` `mov eax,[rcx+78h]`；TalkToNpc `[rdi+78h]` | `kFbNpcObjectId` |
| Npc | NpcData* | `+0x80` | `Npc_get_TemplateId` `mov rax,[rcx+80h]` | `kFbNpcData` |
| NpcData | Id（模板号） | `+0x10` | 同上 `mov eax,[rax+10h]`；0814 `b72f891d… @ 0x10` | `kFbNpcDataId` |
| FieldActorBase | pos `Vector2` | `+0x64` | `FieldActorBase_get_Pos` `mov rax,[rcx+64h]`；Talk `[rsi+64h]` | `kFbActorPos` / `kHashActorPos=dfe2e5f1…` |
| FieldActorBase | VecCtrl* | `+0x50` | 0814 backing `a53533ce… @ 0x50` | `kFbUserVecCtrl` |
| VecCtrl | AbsPos | `+0x98` | `VecCtrl_get_AbsPos` `movups xmm0,[rdx+98h]` RVA `0x11C2440` | `kHashVcAp` / `kFbVcAp` |
| UserLocal | bool 门 | `+0x418` | TalkToNpc `movzx ecx, byte ptr [rsi+418h]`；`test cl,cl; jnz` | 无现码；Talk 失败再 BIN |
| UserLocal | vtable 谓词 | `+0x1C8` | `mov rax,[rcx+1C8h]`；`call` 后 `xor al,1; add eax,eax` cmp 解出 **2** | 无现码 |

CALL：`UserLocal_TalkToNpc` RVA **`0x10A5C50`**（规格旧值 `0x10A5EB0` 作废）。opcode 64 见 §3.2。必须 MainPump。

### 11.3 对话 UI

| 对象 | 字段 | off | IDA 证据 | 现码 |
|---|---|---|---|---|
| UIUtilDialogEx | Type | `+0xA0` | `SetUtilDlgEx` `mov [rsi+0A0h], ebp` RVA `0x7A3FDA` | `kFbUiDlgType` / `kHashUiDlgType=f20229ee…` |
| UIUtilDialogEx | Result | `+0xA4` | `get_Ret` `mov eax,[rcx+0A4h]` RVA `0x7914B0` | 开店未单独暴露 |
| UIUtilDialogEx | List\<string\> | `+0xE0` | `get_MenuTexts` `mov rax,[rcx+0E0h]` RVA `0x7914D0` | `kFbUiDlgMenuTexts` |
| UIUtilDialogEx | Say 正文 string | `+0x120` | `get_SayText` `mov rax,[rcx+120h]` RVA `0x7914E0` | **开店未用；char_boot 可选用** |
| UIUtilDialogEx | SelectIndex | `+0x114` | 0814 dump `ec9d301d… @ 0x114`；无 4B getter | `SetKeyFocus(int)` 写入，CFF |
| UIUtilDialogEx | Unity Text 显示 | `+0x1B0` | dump SerializeField；不是 Il2CppString | 不要拿来匹配关键词 |

Type 枚举：0 Text / 1 YesNo / 4 List。

| CALL | RVA | VA | 现码 |
|---|---|---|---|
| SetKeyFocus(int) | `0x7978C0` | `0x7ffd72fb78c0` | `kRvaUiDlgSelectMenu` |
| OnClickBtOk | `0x7A02E0` | `0x7ffd72fc02e0` | `kRvaUiDlgOnClickBtOk` |
| OnClickBtYes | `0x7A0390` | `0x7ffd72fc0390` | 未用 |
| OnClickBtNo | `0x7A03F0` | `0x7ffd72fc03f0` | 未用 |
| SetUtilDlgEx | `0x7A3C20` | `0x7ffd72fc3c20` | 不必直调 |

### 11.4 进厅 / 出门 — MapPortalData

`pt=7` = `MapPortalType.Script`（枚举 TypeDef 2085，restored 明文）。脚本名在 `Script +0x40`，不是新 CALL。产品路径仍是 StickUp+↑，**禁止** `CheckMovePortal`。

| 字段 | off | IDA getter RVA | 指令 |
|---|---|---|---|
| ID / pt | `+0x10` / `+0x14` | `0x15100B0` / `0x15100D0` | `mov eax,[rcx+10h/14h]` |
| Enable | `+0x18` | `0x15100F0` | `movzx eax, byte ptr [rcx+18h]` |
| PName | `+0x20` | `0x1510110` | `mov rax,[rcx+20h]` |
| X / Y | `+0x28` / `+0x2C` | `0x1510130` / `0x1510150` | `mov eax,[rcx+28h/2Ch]` |
| ToMapId | `+0x30` | `0x1510170` | `mov eax,[rcx+30h]` |
| Script | `+0x40` | `0x15101B0` | `mov rax,[rcx+40h]` |
| PortalRect | `+0x54` | `0x1510230` | `movups [ret],[this+54h]` |
| HRange / VRange | `+0x64` / `+0x68` | setters `0x1510290` / `0x15102B0` | 与 `travel_port` fb 一致 |
| Portal 包装 | `Portal.data +0x10` | dump / `kFbPortalData` | `travel_port` |

`jobin00`：Type=7、Script=`enterMagiclibrar`、ToMapId 种子无边。`jobout00`：Type=1、ToMapId=`101000000`。

### 11.5 仍不是偏移问题（别在 IDA 里耗）

| 项 | 状态 |
|---|---|
| INT 20 是否服务端要 | 客户端无 `cmp 20`；BIN |
| 船费 150 | **必收**（产品口径）。GA 里没有 `cmp 150`，不要再当 BIN |
| 汉斯菜单原文 | 脚本不在 GA；运行时读 `+0xE0`/`+0x120` |
| `+0x418` 的产品名 | bool 门已钉；失败样本才能命名 |
| 虚谓词 `vtable+0x1C8` 方法名 | 槽已钉；CFF |
| 建角名长 prefab 上限 | `UIInputField+0x194` 已钉字段；具体数字在预制体，运行时读 |
| 查重 S2C opcode | 未钉；产品路径轮询 `avatarSelect` 面板，不手解回包 |

---

## 12. 登录建角（`UILoginNew` / `auto_enter` 前置）

**产品口径**：一键起号可选项「自动创建角色」。默认关。槽复用 `core.charSlot`（默认 1）。空号才建；已有角色跳过。名字 8 位 `a-z`，重名再随。

**不要**把下面这些 CALL 写进局内 `char_boot` worker。选角页才有 `UILoginCharacter` / `UILoginNew`。现码 `auto_enter.cpp` 已有选角 RVA，**没有**建角。

IDB：`imagebase=0x7ffd72820000`（2026-08-14）。0814 dump TypeDef **903** = `UILoginCharacter`（`b41ae874…`），**905** = `UILoginNew`（`ef634e33…`），**1174** = `SceneLogin`（`d563b656…`）。restored dump 把 SceneLogin 误名叫 `FtpWebRequest`，枚举误名叫 `Exception`——**别信 restored 的枚举名**，只信 0..5 的值和 `auto_enter` 已钉的 phase **2** = 选角可确认。

### 12.1 调用链（托管，MainPump）

```
选角页 count==0
  → UILoginCharacter.OnClickButtonNew          RVA 0xA97B90
       AvatarList.Count < SlotCount (+0x170 / +0x1A8)
       SceneLogin.SetLoginPhase(5)             RVA 0xC1E9B0
  → UILoginNew 取名面板（nameSelect +0x40）
       写入名字后
       OnClickButtonYesName                    RVA 0xA9FFF0
         rdx = inputFieldName(+0x50)->m_Text(+0x180)
         jmp SendCheckDuplicateIDPacket        RVA 0xAA0020   C2S opcode 9
  → 查重成功：SetCheckedName                    RVA 0xA9F3A0
       随后 InitNewCharEquip                   RVA 0xA9F3E0
       切到 avatarSelect +0x48
  → OnClickButtonYesAvatar                     RVA 0xAA06F0  （5 字节 thunk）
       jmp CreateChar                          RVA 0xAA0700  C2S opcode 10
  → 等 GetAvatarCount()>=1
  → 现有 SelectCharacter(0) + OnClickButtonSelect
```

`OnClickButtonYesAvatar` 本身几乎是空的（`jmp CreateChar`）。真正发包的是 `UILoginNew_CreateChar`。已在 IDA 改名，书签前缀 `char_boot:`。

### 12.2 类 / 字段 / 哈希（0814 dump，与 `auto_enter` 同口径）

| 对象 | 字段 | off | 哈希 / 证据 |
|---|---|---|---|
| SceneLogin | CharUi | `+0xD0` | `df26ddce…` · `kFbSlCharUi` 已在 `auto_enter` |
| SceneLogin | **NewUi `UILoginNew`** | `+0xD8` | backing `f6fc8999a47805e6b3a1d952bd30798995f28c91785924297733c47d87bd995` |
| SceneLogin | LoginPhase 枚举 | `+0x98` | `auto_enter`：确认角色要求 **2**；New 按钮把 phase 设成 **5**（种子实算，见下） |
| UILoginCharacter | SelectedIndex | `+0x168` | 已有 |
| UILoginCharacter | AvatarList | `+0x170` | 已有；`Count` 在 List `+0x18` |
| UILoginCharacter | SlotCount | `+0x1A8` | 已有；解锁槽数 |
| UILoginNew | nameSelect GO | `+0x40` | restored 明文 |
| UILoginNew | avatarSelect GO | `+0x48` | 查重成功后应亮；产品用这个轮询，不钩 S2C |
| UILoginNew | inputFieldName | `+0x50` | `UIInputField`（TypeDef 271，继承 Unity `InputField`） |
| UILoginNew | `_newCharacterData` | `+0x68` | 默认外观数据 |
| UILoginNew | `_avatarEquipList` | `+0x78` | `Dictionary<BodyPart,int[]>`；CreateChar 按此编码，循环宽度解出 **6** |
| UIInputField | m_Text | `+0x180` | YesName / CreateChar / Append 都读这个；Append 还有 `lea r14,[rsi+180h]` |
| UIInputField | m_CharacterLimit | `+0x194` | Append：`cmp [rsi+194h], r14d` / `setle` |

方法（明文名或 0814 哈希）：

| 方法 | RVA | VA | 哈希 / 备注 |
|---|---|---|---|
| `OnClickButtonNew` | `0xA97B90` | `0x7ffd732b7b90` | 明文；size `0x691` |
| `GetAvatarCount` | `0xA9EEE0` | | `aa68db0a…` 已在 `auto_enter` |
| `IsSlotEnable(int)` | `0xA9AB80` | | `cab134ab…` 已在 `auto_enter` |
| `SelectCharacter(int,bool)` | `0xA930B0` | | 已有 |
| `OnClickButtonSelect` | `0xA94500` | | 已有 |
| `SetCheckedName(string)` | `0xA9F3A0` | `0x7ffd732bf3a0` | `d96919a972d45020b4eaf7ee9a4fdf24e157cdd902ac78a888158ca4aa20805` |
| `InitNewCharEquip` | `0xA9F3E0` | `0x7ffd732bf3e0` | `d6cb50c727b11f038a4b116a3ec23c0c8c70577513558d550adbc43e6a64ce5` |
| `OnClickButtonYesName` | `0xA9FFF0` | `0x7ffd732bfff0` | 明文；size `0x23`，只转调查重 |
| `SendCheckDuplicateIDPacket(string)` | `0xAA0020` | `0x7ffd732c0020` | `e3d8527ad40691d0ae9751b93e5665608a5bab67bd8c759b80b0ff379cb09e0` |
| `OnClickButtonYesAvatar` | `0xAA06F0` | `0x7ffd732c06f0` | 明文；**5 字节** `jmp CreateChar` |
| `CreateChar`（private） | `0xAA0700` | `0x7ffd732c0700` | `c113a5408f0a03121c00c8ff3d798b495f88f16687f0c90fc18f70ff42d9a5c` |
| `SceneLogin.SetLoginPhase(enum)` | `0xC1E9B0` | `0x7ffd7343e9b0` | `cff043141ceb038f05b8e8eb76bf56d49beccae9761941c28bde9fd241b361c` |

`UILoginNew` 类哈希：`ef634e3353bb2397e543912b92aabe4f8136f806fc5a793b42f76729f61d50b`。

### 12.3 opcode 种子实算（禁止手搓，只作核对）

OutPacket 构造：`sub_7FFD7450C3F0`（与 TalkToNpc 同一 ctor，登录 opcode 是小数）。

**查重 C2S = 9**（`SendCheckDuplicateIDPacket` `0xAA0372`）：

```
movzx ecx, cs:word_7FFD790A10D0   ; 实读 28310 = 0x6E96
add   ecx, 0FFFF9173h             ; ecx += -0x6E8D
; (0x6E96 + 0xFFFF9173) mod 2^32 = 9
call  OutPacket
call  EncodeStr                   ; sub_7FFD74519210，名字
```

**建角 C2S = 10**（`CreateChar` `0xAA0E36`）：

```
movzx ecx, cs:word_7FFD790A10F0   ; 实读 23164 = 0x5A7C
add   ecx, 0FFFFA58Eh             ; ecx += -0x5A72
; = 10
call  OutPacket
EncodeStr  inputField+0x180
Encode4    [obj+10Ch]
; 随后按 +0x78 字典循环，宽度 xor 解出 6
```

**OnClickButtonNew → phase 5**：

```
mov edx, 7670717Bh
add edx, cs:dword_7FFD790A0ED0    ; 实读 2307886730 = 0x8990A28A
; 0x7670717B + 0x8990A28A = 5
call SceneLogin_SetLoginPhase
```

同函数里 `AvatarList.Count` vs `SlotCount`：`count < SlotCount` 才能 New。`Count` 与解出常量 **0** 做 `setle`：空列表走创建。槽满走 toast（解出字符串 id 9）。

`CreateChar` 里 `AvatarList.Count` 与解出 **15** 比较：`count < 15` 才发建角包（账号角色上限，不是「1 槽」）。

查重函数里还有 `xor al,1; add eax,eax` 与解出 **2** 的布尔比较（空名 / 非法名则不发包），模式与 TalkToNpc 谓词相同。

### 12.4 「1 槽」语义（已钉，别再发明）

`auto_enter`：`gCharSlot` 默认 1，`index = slot - 1`，`SelectCharacter(index)`。面板 `DragInt` 1..15。这就是用户说的「默认 1 槽」。

`OnClickButtonNew` **不吃槽号**。它只问「还有没有空位」，新角色追加到 AvatarList 末尾。空号时末尾 = 槽 1。所以：

- 空号 + autoCreate → 建出来的一定是 index 0
- 已有角色再点 New = 建第 2 个。规格禁止这条路径

`IsSlotEnable` 读 `AvatarList`（`+0x170`），CFF 未把「空列表时 index 0 是否 true」逐条解完；落码沿用 `auto_enter` 已有泵上调用。空号再加一道 `SlotCount>=1`。

### 12.5 名字字符集（未钉死客户端规则）

`UIInputField.Append`（RVA `0x1B09C60`）CFF。`ValidateChar`（RVA `0x1B0A240`）会调 TypeDef 1401 的两个静态方法（RVA `0xE930C0` 吃 string、`0xE932E0` 吃 char），像是按字节算名长，不是 `char.IsLetter`。

产品不跟这套 CFF 死磕：只生成 8 位小写 ASCII，直调 `SendCheckDuplicateIDPacket(self, il2cpp_string)`，不依赖 InputField 过滤。服务端拒了就换名。

`SetCheckedName` 的 xref：`SceneLogin_SetLoginPhase` 内 `0xC1F985` 调用，随后 `InitNewCharEquip`；另有 `sub_7FFD7344B300`。查重 **S2C opcode 未钉**。成功判据：`avatarSelect` 从隐到显，或 `nameSelect` 关掉。失败：超时仍停在取名 → 换名。

### 12.6 落码契约

| 做 | 不做 |
|---|---|
| 泵上调 `OnClickButtonNew` / `SendCheckDuplicateIDPacket` / `OnClickButtonYesAvatar` | 手组 opcode 9/10 |
| 泵上 `il2cpp_string_new` | worker 上 `string_new` / 类初始化 |
| `count==0` 才 New | 删角、覆盖 1 槽、给已有号再建一个 |
| 槽号用 `core.charSlot` | `[char_boot]` 再做一个 slot 键 |
| 默认外观 | 点 Left/Right 捏脸；创建时选法师 |
