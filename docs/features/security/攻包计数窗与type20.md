# SecurityClient 攻包计数窗与 ClientHacks type20（Classic TWMS）

> **产品**：新枫之谷：经典版（TWMS / `Maplestory_Classic.exe`）。**不是**枫星。  
> **状态**：CMS 符号 + TW IDA 常量/xref 解码 + BIN 探针采证（2026-08-01）  
> **证据源**：`Dumps/cms_cw/dump.cs` · TW `GameAssembly.dll.i64` · `bin/XCat_data/logs/x.jsonl` · 工作挖空 `Dumps/runtime/_dig_securityclient_attack_window.md`  
> **实现**：只读端口 `x/features/ports/security_attack_port.*`（**禁止** GA `.text` hook）  
> **关联**：[`客户端Hack标志与服端推断.md`](客户端Hack标志与服端推断.md) · [`MscSecurity能力面.md`](MscSecurity能力面.md)

---

## 0. 一句话（人话）

服务器通过客户端自证盯的是：**约 60 秒内，攻击相关发包/技能是不是刷得像洪水**。  
踩线会发 `ClientHacks`，类型 **`AttackPacketCountCheck = 20`（type20）**。  
正常挂机 / 多发 gap≥40ms **几乎踩不到**（门槛约 **33 次/秒 × 满 60 秒**）。

这不是：技能 CD 校验、伤害校验、也不是「有没有真键盘」。  
`peakKey` = 字典里某个 **包型/技能 ID** 的计数峰值，**不是**物理按键次数。

---

## 1. SecurityClient 本地硬窗（仅两套）

`Msc.Security.SecurityClient`（TW TypeDef **15147**；**2026-08-03 重哈希** `d9ef28f1…ce4cb7f`，旧 `ba499947…` 作废；见 `Dumps/runtime/CRITICAL_REMAP_20260803.md`）：

| 字段 | off | 语义 |
|------|-----|------|
| `m_mAttackPacketCnt` | `+0x0` | `Dictionary<ushort,int>` 按 **packetType** 累加 |
| `m_mAttackSkillCnt` | `+0x8` | `Dictionary<int,int>` 按 **skillId** 累加 |
| `m_tAttackPacketCntDetectTime` | `+0x10` | 窗起点（游戏侧 `tCur`，**不是** `GetTickCount`） |
| `TERM_MS` | const | **60000** |
| `CHECK_COUNT` | const | **2000** |
| `MAX_PARAM_LENGTH` | const | 1024 |
| `m_bDetectMobSpeedHack` | `+0x14` | 怪加速检测标志（另线） |
| `m_tMobSpeedHackDetectTime` | `+0x18` | 怪加速检测时间（另线 → type **21**） |

同类「滑动计数窗」在本类里 **只有攻包窗**；怪速是 detectTime 另一条。  
**没有**第三套 `TERM_MS`/`CHECK_COUNT`。

### 1.1 API（TW RVA / 典型 VA = `0x7FFB16B40000` + RVA）

| 方法 | RVA（2026-08-03） | 作用 |
|------|-------------------|------|
| `CollectAttackPacket(ushort)` | `0x3C44C10` | 攻包类型 +1 |
| `IsAttackPacket(ushort)` | `0x3C450E0` | 白名单；false 则不 Collect |
| `CollectAttackSkill(int)` | `0x3C45270` | 技能 id +1 |
| `SendAttackPacketCountCheck(int tCur)` | `0x3C45650` | 超窗汇总；超阈值 → **type20** |
| `IsOverTime` / `SetDetectMobSpeedHack` / `SendMobSpeedHackDetectCheck` | `0x3C46720` / `0x3C46A90` / `0x3C46C10` | 工具 + type **21** |

### 1.2 常量解码（IDA · CFF 混淆体）

| 常量 | 解码路径（摘要） | 值 |
|------|------------------|-----|
| `TERM_MS` | `mov ecx, 0xC3D1A1EC` ⊕ `dword_…1B14` | **60000** |
| 举报类型 | `mov edx, 0x4CD31703` + `dword_…1AEC` | **20** |
| `CHECK_COUNT` | dump 元数据 + 函数体旧解码 | **2000** |

Hex-Rays 对 SecurityClient 本体几乎全是 CFF 跳转表；可读逻辑靠反汇编 + 常量解码 + xref。

---

## 2. `IsAttackPacket` 白名单（实锤）

出站旁路：`IsAttackPacket` → true 才 `CollectAttackPacket`。

MBA 还原后对 `ushort` 在 0–511 穷举，命中集合为：

| CMS `ClientPacket` | 值 | 含义 |
|--------------------|-----|------|
| `UserMeleeAttack` | **50** | 近战 |
| `UserShootAttack` | **51** | 射击 |
| `UserMagicAttack` | **52** | 魔法 |
| `UserBodyAttack` | **53** | 身体撞击 |
| `SummonedSkill` | **191** | 召唤兽技能 |

等价：

```text
pkt ∈ {50,51,52,53}  ||  pkt == 191
```

（混淆写法：`transform(50..53) → 0..3` 且 `< 4`，再 OR `==191`。）

**不在白名单**：移动 / 聊天 / 吃药 / `UserSkillUseRequest(103)` 等。  
技能次数走 **`CollectAttackSkill`**（见下），不经本函数。

> TW opcode 数值是否与 CMS 完全同号：以进图发包对照为准；机制与白名单语义按 CMS 名对齐。

---

## 3. 调用链（IDA xref · 2026-08-01）

```text
出站旁路 sub_…FCAB60
  ├─ IsAttackPacket(packetType)
  └─ CollectAttackPacket   ← 仅白名单

UserLocal_DoActiveSkill
  └─ CollectAttackSkill(skillId)

a480_Update（每帧）
  └─ sub_…FC9520（安全 tick）
        ├─ SendAttackPacketCountCheck(tCur)     → 可能 ClientHacks type20
        └─ SendMobSpeedHackDetectCheck(tCur)   → type21
```

| 被调 | 调用落点 |
|------|----------|
| `IsAttackPacket` / `CollectAttackPacket` | 发包旁路（`CollectAttackPacket` 仅此一处 code xref） |
| `CollectAttackSkill` | **仅** `UserLocal_DoActiveSkill` |
| `SendAttackPacketCountCheck` + `SendMobSpeedHackDetectCheck` | 同一 Update 安全 tick |

→ 无真实键盘也会涨计数：只要游戏走出站攻击包或 `DoActiveSkill`。  
自动化 `OnFuncKey(A-slot)` / 多发 Prepare 同样会进窗。

---

## 4. 窗语义与踩线密度

```text
Collect*：dict[key]++
SendAttackPacketCountCheck(now)：
  若相对 detectTime 超过 TERM_MS(60000)
    → 汇总 / 重置窗
  若（单键峰值等）> CHECK_COUNT(2000)
    → 发 ClientHacks(AttackPacketCountCheck=20)
```

探针 `pct` / 风险口径按 **单键 peak（peakKey）** 相对 2000（与 type20「按键/按包型」判定一致）；`pktSum`/`skillSum` 仍保留总量观测。

| 说法 | 约数 |
|------|------|
| 门槛 | 60s 内某键 ≈ **2000** |
| 密度 | ≈ **33.3 次/秒**，约 **30ms 一下** 且持续满分钟 |
| 普攻 ~200ms | ≈ 5/s → 满分钟百余，差一个数量级 |
| multi_skill gap 120ms | ≈ 8/s → 满分钟约四五百，仍远不到 |

**结论**：type20 是极端洪水/异常发包举报，**不是**调 gap 的精细天花板。

---

## 5. 同类机制对照（勿混）

| 机制 | 在哪 | 像不像攻包窗 |
|------|------|----------------|
| type20 攻包/技能计数 | `SecurityClient` 硬编码 60s/2000 | **本窗** |
| type21 怪加速 | 同クラス `+0x14/+0x18` + Send | 平行举报，对象是怪速 |
| ClientHacks 1–3 | 按键统计 | 事件型，非 60s/2000 包计数 |
| ClientHacks 4–16 | RawInput / Soft / World 宏 | 输入模式检测 |
| ClientHacks 17–19 | 反宏键盘 / 句柄 / NGS | 另一路 |
| LiveValue **430 / 557 / 558** | 服端下发阈值表 | 「限流」语义相近，但是**远程表** |
| `VecCtrlMob` Float/AB | 怪仿真 | 与玩家攻包无关 |
| `HackingAutoBlock.*` | 服端裁定结果码 | **结果**，不是客户端滑动窗 |

### 5.1 LiveValue 430/557/558（BIN）

| LiveValueInt | 键 | 语义 |
|--------------|-----|------|
| `UserSkillUseRequestCountCheck` | 430 | 技能发包计数检查 |
| `PacketAttack1SecLimit` | 557 | 攻包 1s 上限 |
| `PacketAttack500msLimit` | 558 | 攻包 500ms 上限 |

- 曾用热路径探针进图挂机多次：`GetInt` 均 **-999999**，`defaultMask=0xf` → **本环境未下发有效阈值**。  
- 静态 xref 解码 `GetInt_Def` 调用点 **未见** 430/557/558（多见 21/446/10039 等）。  
- **与 type20 硬窗脱钩**：盯洪水看 SecurityClient；不要等 LiveValue。  
- **运行时探针已拆除**（不再挂 `multi_skill` / 不再编 `livevalue_port`）；结论见挖空笔记。

挖空笔记：`Dumps/runtime/_dig_livevalue_probe_结论.md`。

---

## 6. 只读探针（数据面）

| 项 | 说明 |
|----|------|
| 代码 | `x/features/ports/security_attack_port.{h,cpp}` |
| 绑定 | Il2Cpp `FindClass(d9ef28f1…)` → `static_fields`（旧 `ba499947…` 作废） |
| 采样 | `multi_skill` worker **每 15s** + 测试前一拍；tag=`SecAttack` |
| BIN | `bin/XCat_data/logs/sec_attack.log`：`INIT` / `ZERO`（每次置零）/ `IDLE` / `SKIP` / `PROBE` |
| 不做 | 不 Hook、不调用 `SendAttackPacketCountCheck` |

### 6.1 Dictionary 布局（踩过的坑）

IL2CPP `Dictionary`：`entries@0x18` / `count@0x20` / **`freeCount@0x28`** / **`version@0x2C`**。

| 错误 | 后果 |
|------|------|
| 把 `freeCount` 读成 `+0x2C`（实为 `_version`） | 出刀后 `cnt=1` 仍 `ok=0`/`sum=0`（`count-version<0` 提前退出） |
| 用 `GetTickCount - detectTime` 当窗龄 | detectTime 是游戏 `tCur`，窗龄会假爆炸 |

已修：`freeCount@+0x28`，以 entries 扫描为主（对齐 `drop_pool` / `skill_port`）；日志带 `ver=`；`windowAgeMs` 固定 -1。  
运行时 SSOT：`x::runtime::il2cpp_container`（`security_attack_port` / `drop_pool` / `foothold` / `skill_port` 共用）。

### 6.2 BIN 采证（2026-08-01）

| 时段 | 结果 |
|------|------|
| 修前（~06:08） | 有 `dead_or_gone` 击杀，但 SecAttack `sum=0`/`ok=0`（布局 bug） |
| 修后空窗（~06:21） | `ok=1`，`free`/`ver` 分离；短打后仍可能采到已 Clear 的空表 |
| 修后有效（~06:28–06:29） | **`pktSum=4` `skillSum=4` `peakKey=4` · `4/2000 (0%)`**；同段 combat `dead_or_gone` |

→ 探针可读真计数；当前强度离 type20 极远。

日志字段要点：`ok` / `pktSum` / `skillSum` / `peakKey` / `pct` / `detectTime` / `pkt{hdr,sumOk,cnt,free,ver,elen}`。

---

## 7. 对 multi_skill / 挂机的结论

| 问题 | 答案 |
|------|------|
| gap 40–120 会不会 type20？ | 静态阈值下 **极难** |
| A 键普攻会不会进窗？ | **会**（白名单 50–53） |
| 多发会不会进技能表？ | **会**（`DoActiveSkill` → `CollectAttackSkill`） |
| 没真键盘为什么有 peakKey？ | 数的是包型/技能 ID，不是物理键 |
| LiveValue 557/558 能不能当限流？ | 本环境常空；**不能**替代 type20 窗 |

---

## 8. 证据边界

| 级别 | 内容 |
|------|------|
| 实锤 | 字段/常量元数据；IDA xref 调用链；`IsAttackPacket` 穷举白名单；TERM=60000、type=20 解码；BIN `peakKey` 上涨 |
| 强推断 | Send 超窗后 Clear+重置；type20 按单键峰值与 CHECK_COUNT 比较 |
| 弱推断 | TW `ClientPacket` 数值是否与 CMS 50–53/191 逐字相同；服端收到 type20 后的具体处置 |
| 推不出 | 服端是否还有独立攻包滑动窗源码；靠清字典绕过服端权威校验 |

---

## 9. 相关入口

| 路径 | 说明 |
|------|------|
| `x/features/ports/security_attack_port.*` | type20 计数窗只读探针 + 数据面拦截；BIN `logs/sec_attack.log` |
| `x/features/multi_skill/multi_skill.cpp` | SecAttack 15s + 出刀前采样 |
| `Dumps/runtime/_dig_securityclient_attack_window.md` | 工作挖空 |
| `Dumps/runtime/_dig_livevalue_probe_结论.md` | LiveValue 430/557/558 空表结论（运行时探针已拆除） |
| [`客户端Hack标志与服端推断.md`](客户端Hack标志与服端推断.md) | ClientHacks 全表 + 怪 Float/AB |
| CMS `ClientPacket` / `ClientHacksType` | `Dumps/cms_cw/dump.cs` |
