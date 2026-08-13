# docs/features — 模块索引

> **当前目标**：新楓之谷：經典版（TW / beanfun · Gamania Galaxy）。**不是**枫星。
> 本文是 `docs/features/` 下设计文档的总索引。
> 逆向笔记见 [`Dumps/ANALYSIS_NOTES.md`](../../Dumps/ANALYSIS_NOTES.md)。
> 工程对照仓（仅复用设计/代码模式）：`xcat_for_fengxing/docs/features/`。

---

## 1. 运维与架构（ops/）

| 文档 | 主题 |
|------|------|
| [`ops/GAMA_PASS与注入闭环.md`](ops/GAMA_PASS与注入闭环.md) | **GAMA PASS 无人值守** + Classic 注入 + 守护/挂机 `KillLaunchChain` + Cookie 复用/强制重同步 + 顶栏登录提示 |
| [`ops/启动系统实现.md`](ops/启动系统实现.md) | Galaxy 换票、NGM deep-link、启动骨架（对照枫星注入器启动文档） |
| [`ops/架构总览.md`](ops/架构总览.md) | TWMS 分层 DAG；`xcat_app` 内嵌 WebView / GamaPass 换票 |
| [`ops/日志系统.md`](ops/日志系统.md) | 统一 `xcat_log`：launcher / inject / payload JSONL + GUI callback |
| [`ops/il2cpp托管调用线程规约.md`](ops/il2cpp托管调用线程规约.md) | **托管调用必须在 MainPump 上**：换图黑屏根因（Class::Init 被打断）、故障链反汇编、`il2cpp_fault_probe` / `hang_autopsy` 排障手册 |
| [`unity_kbd/模块设计.md`](unity_kbd/模块设计.md) | **InputSystem 键盘真源** ✅：QueueEvent + 自管 Repush + `HoldUntil`；走路 / Travel ↑ / 定时键同路 |

---

## 2. 协议与移动（protocol/）

| 文档 | 主题 |
|------|------|
| [`protocol/移动协议.md`](protocol/移动协议.md) | MovePath API、S→C opcode（`0x00D9` 等）、C→S Flush |
| [`protocol/MoveElem字段.md`](protocol/MoveElem字段.md) | MoveElem 布局；`xy/v/ma/fh/attr`；`MoveActionType` rawAction 表；Encode wire 待采 |

工作草稿仍在 `Dumps/opcode_move_notes.md`、`Dumps/move_elem_notes.md`（以本目录正式文档为准）。

---

## 3. 有模块设计文档的 Feature

| 文档 | 主题 |
|------|------|
| [`invuln/模块设计.md`](invuln/模块设计.md) | 无敌 ✅ v2.6.3：`+0x298` hit 门 + 帧钉/`8ms` 去闪；重绑 `400ms` + 绑宽限 `1.5s`；面板 `[core] invuln`；**无** inline / **无**热键（F10=换频） |
| [`kick_sniff/断线错误码.md`](kick_sniff/断线错误码.md) | `SessionState` / RING；`pendingError=205`=哨兵≠踢因；AutoBlock 22/24 **不进** `+0x40`；**TW≠CMS 偏移** |
| [`titlebar/模块设计.md`](titlebar/模块设计.md) | Win32 标题栏 vitals + 金/经/物值每分 + 职业繁中名；锚点 `WM→CharacterData→CharacterStat`（共享 `player_vitals`）+ 背包卖价（DumpRestoredData） |
| [`world_manager/字段全表.md`](world_manager/字段全表.md) | `WorldManager` TW 字段全表 + CharacterData/Field/MapData 一跳；对 CMS 漂移标注 |
| [`world_manager/SceneState与Field.md`](world_manager/SceneState与Field.md) | 进图门控 `SceneState` + `Field`/`SceneMap` 字段拆解；与现行 MyUser 门控对照 |
| [`autopot/模块设计.md`](autopot/模块设计.md) | 自动喝药：阈值策略 + `SendStatChangeItemUseRequest`（药水）；回家卷见 auto_supply `PortalScroll`；面板 `[core] hpPotion/mpPotion`（✅ 已挂入；实机待验） |
| [`simple_combat/模块设计.md`](simple_combat/模块设计.md) | 自动打怪：Impact 贴怪 = 近战直升机（旋翼环闭环悬停，默认）/ 拟人走路 / fill+Doing 瞬移互斥；F5 |
| [`attack_speed/模块设计.md`](attack_speed/模块设计.md) | **攻击加速** ✅ BIN 结案：清忙锁 + `SS+1BC=140`；频率=面板间隔；**不做** layer mid-cut |
| [`attack_rpc/模块设计.md`](attack_rpc/模块设计.md) | 结算层出刀实验 🔍；目标全屏多怪；默认关 |
| [`attack_rpc/P0a_本地出刀与OnMelee误区.md`](attack_rpc/P0a_本地出刀与OnMelee误区.md) | 否决直调 `OnMeleeAttack`（UserRemote）；`SetAttackAction` `0xFD39C0` |
| [`attack_rpc/P0b_出站Encode与Send锚点.md`](attack_rpc/P0b_出站Encode与Send锚点.md) | OutPacket/NM.Send 新 RVA；复用 security/attack_speed/sellbag 研究 |
| [`attack_rpc/P0c_攻包BODY布局.md`](attack_rpc/P0c_攻包BODY布局.md) | 现网 TryDoing* Encode 序；命中环；`Network_SendOutPacket@0x1CB7CE0` |
| [`attack_rpc/P1_探针port.md`](attack_rpc/P1_探针port.md) | 攻包伪造探针；默认关；`ATTACK_RPC=1` |
| [`attack_speed/P0a_出刀忙锁与Prepare链.md`](attack_speed/P0a_出刀忙锁与Prepare链.md) | `SetAttackAction` → busy → Prepare → Slot14 解锁；排除 hitstun/AntiRepeat |
| [`attack_speed/P0b_双速系统与字段表.md`](attack_speed/P0b_双速系统与字段表.md) | ActionSpeed vs 武器档/Booster；Forced→`+80`；CTS 7/11 表 |
| [`attack_speed/P0c_TemporaryStat生命周期.md`](attack_speed/P0c_TemporaryStat生命周期.md) | Decode/Reset/CheckByTime；Speed 客户端不自清 |
| [`teleport/模块设计.md`](teleport/模块设计.md) | 瞬移 port：仅 fill+Doing（`fill_slim`）；点飞不钉台 |
| [`teleport/P0d_fill_slim软重载结案.md`](teleport/P0d_fill_slim软重载结案.md) | **挂机飞出图 / Field 软重载** ✅：Doing 后抢收态 → Ap→0；瘦身后置只 `Apl←Ap`（BIN `101030400`） |
| [`fly/模块设计.md`](fly/模块设计.md) | F6 Impact 飞（Attr=2）+ fh-ban；Coast/fill 点飞已拆；给后续 Agent 的真源说明 |
| [`travel/模块设计.md`](travel/模块设计.md) | **超级赶路** ✅：同盘 seed BFS + Snap 钉台贴门（`snap=1`，禁悬空）；世界地图 Spot 预检 Notice；无码头/跨盘自动 |
| [`worldmap_marker_travel/模块设计.md`](worldmap_marker_travel/模块设计.md) | **世界地图 Spot 双击赶路** ✅：`UpdateView`/`OnPointerDown` 换桩 → YesNo/Notice → `RequestGoto`；字段偏移与 BIN |
| [`teleport/P0a_瞬移CALL锚点.md`](teleport/P0a_瞬移CALL锚点.md) | TW IDA 钉死 Register/Doing/Attr RVA、原生调用链、旁路 vs 原生、BIN 纪要；§1.1 视觉层同步链锚点（13 个 RVA + 字段偏移） |
| [`teleport/P0b_引擎实现原理.md`](teleport/P0b_引擎实现原理.md) | **位移真源**：`SetImpactNext` 的 fmax/fmin 饱和合并语义、完整消费链（`ApplyImpact 0x11A4E60` / `LeaveFoothold 0x11AF5C0`）、「踏板偷换才是位移主因」及三个证伪实验；`MovePathType` 全表 / `MoveElem`·`MovePath`·`VecCtrl` 布局；14 组偏移核对零偏差；文档时间线纠偏 |
| [`teleport/P0c_视觉层同步链.md`](teleport/P0c_视觉层同步链.md) | **皮跟谁走**：`VecCtrl.GetPos()` = `round(lerp(Ap, Apl, alpha))`、`Apl` 由 `BeginUpdateActive` 每帧滚动、Slot 16 写 TRS 的三道门控与整数脏检查；实证 `Pos@0x64` 不参与视觉；「改坐标皮出错」逐症状机制对照 |
| [`mob_pool/活怪n与刷怪槽M.md`](mob_pool/活怪n与刷怪槽M.md) | **n**=MobPool 活怪 / **M**=LifeList 刷怪槽；实现 +「为何 n≪M」官方引擎说明；BIN 自洽判据 |
| [`mob_scan/模块设计.md`](mob_scan/模块设计.md) | ✅ 扫怪 worker：面板周期、事件唤醒、按需 Lite、MobCtrl、相对旧实现优势；消费端协作 |
| [`mob_pool/P0a_OnLocalMob与Init包体.md`](mob_pool/P0a_OnLocalMob与Init包体.md) | 🔍 Mob 包族：`EnterField`/`LeaveField`/`ChangeController`；`SetLocalMob`/`SetRemoteMob`；`Mob.Init` 包体；方案 ⑥ 观察点（已纠名） |
| [`mob_pool/P0b_MI观察与按需Collect.md`](mob_pool/P0b_MI观察与按需Collect.md) | 📐 设计：MI 观察 Enter/Leave → `RequestImmediateScan`；SetRemote **条件踢池**；**未落码** |
| [`mob_pool/P0c_Enter到开火时间线.md`](mob_pool/P0c_Enter到开火时间线.md) | 🔍 Enter→开火门控；**FindHit 要 inViewSplit、不要 MobCtrl/Active**；感知 vs 贴身/RTT |
| [`mob_pool/P0d_suspended与initDelay.md`](mob_pool/P0d_suspended与initDelay.md) | 🔍 `_suspended`/`_initDelay`：Init 写 true、Update×GetUpdateTime 解除；窗口多约 1 帧 |
| [`mob_pool/P0e_SetDeadType与deadType.md`](mob_pool/P0e_SetDeadType与deadType.md) | 🔍 `SetDeadType`/`_deadType@0x1B4`：唯一写 API；Leave 不写；调用方静态 BLOCKED |
| [`mob_pool/P0f_SetDeadType观察方案.md`](mob_pool/P0f_SetDeadType观察方案.md) | 📐 MI/HWBP 采 `edx`+返回址；与 Leave 时序；**未落码** |
| [`mob_pool/REMOUNT_20260806.md`](mob_pool/REMOUNT_20260806.md) | 🔧 晚间 GA remount：RVA+0x1E70；字段 off 稳；`mob_pool_port` 哈希已钉 |
| [`pet_feed/模块设计.md`](pet_feed/模块设计.md) | **只自动召唤**（喂食交官方）；`[core] petSummon` → `SendActivatePetRequest`（🚧 P0c✅ 待实机） |
| [`pet_feed/P0a_锚点复核.md`](pet_feed/P0a_锚点复核.md) | **2026-08-03 remount**：`m_apPet@0x2B0`、Activate `0xC56910`、新类哈希 |
| [`timed_keys/模块设计.md`](timed_keys/模块设计.md) | 定时按键：7 槽周期脉冲；发键真源 **`unity_kbd` / `InjectKeyHold`**（✅ 已挂入；实机待验） |
| [`unity_kbd/模块设计.md`](unity_kbd/模块设计.md) | **InputSystem Keyboard** ✅：Hold + 自管帧 Repush；Travel / 走路 / 脉冲消费说明 |
| [`buffs/模块设计.md`](buffs/模块设计.md) | BUFF 管理器：技能-only 续航；对照枫星 `buffs`，经典版走 `AffectedSkillEntry` + `DoActiveSkillPrepare`（✅ 已挂入；实机待验） |
| [`buffs/P0a_锚点复核.md`](buffs/P0a_锚点复核.md) | TW IDB 钉死：在身列表 `+0x330`、Prepare/GetSkill/GetSkillLevel RVA |
| [`multi_skill/模块设计.md`](multi_skill/模块设计.md) | 技能多发：清单 gap 串发；技能 `DoActiveSkill`（可选 SendUse）+ 普攻 OnFuncKey；对照枫星仅借排程语义（✅ 可行性对照；实机待验） |
| [`auto_enter/模块设计.md`](auto_enter/模块设计.md) | 自动进游戏：分区→**未满频道随机 (PickOpen)**→选角；单次 Go、禁 Trigger；**softFast**/sticky（✅；旧 PickLeast 已退役） |
| [`auto_enter/选角与SelectedIndex锚点.md`](auto_enter/选角与SelectedIndex锚点.md) | TW IDA 钉死：`UILoginCharacter+0x168` SelectedIndex；可跳过 Select 的依据 |
| [`auto_enter/RVA重锚_20260803.md`](auto_enter/RVA重锚_20260803.md) | 2026-08-03 客户端更新：登录 UI 类哈希 + 方法 RVA 全表重锚 |
| [`soft_login/模块设计.md`](soft_login/模块设计.md) | 软重连试连：ConnectLogin→softFast 重进→playReady；Done≠playReady 闸 + 150s 墙钟 / 10 轮 soft cycle（✅ 默认关） |
| [`ccu/模块设计.md`](ccu/模块设计.md) | 分区 CCU：登录频道页或 auto_enter 喂数一次 → SHM → 底栏（✅） |
| [`channel_hop/模块设计.md`](channel_hop/模块设计.md) | 随机换频：挂机卡/F10 → `manualRejoinSeq` → **直调** `SendTransfer@0xBB5200`（无菜单；✅ 挂入；08-03 锚点已同步） |
| [`encounter/模块设计.md`](encounter/模块设计.md) | 遇人策略：UserPool → 停手/换频；可勾选 GM/隐身升级 + 强制 Alarm（✅ 契约 v60） |
| [`pet_feed/P0b_只读探针.md`](pet_feed/P0b_只读探针.md) | `ReadState` + `petfeed.log`；字段只读，未发包 |
| [`pet_feed/P0c_自动召唤.md`](pet_feed/P0c_自动召唤.md) | `TryActivatePet` + `[core] petSummon`；喂食交官方 |
| [`pet_loot/模块设计.md`](pet_loot/模块设计.md) | **拾物**：脚下 / 宠扩盒 / 人物直吸；黑名单默认箭矢·彈丸；归属预筛 |
| [`pet_loot/P0a_锚点复核.md`](pet_loot/P0a_锚点复核.md) | TW IDA 钉死：DropPool/Pet 拾取 RVA、技能位、`_rcPet@0x100` |
| [`pet_loot/P0b_Drop归属预筛.md`](pet_loot/P0b_Drop归属预筛.md) | **Drop.OwnerId**：`WM+0x114` 真源；禁 CS/`+0x98`；远程 Peek 黑名单 |
| [`auto_lie/模块设计.md`](auto_lie/模块设计.md) | 自动测谎：TextCaptcha+LLM / NonFinite / 測謊機 `2190000` / 契约·status（✅ 挂入；离线基建 BIN 已过） |
| [`auto_lie/基建与离线验收.md`](auto_lie/基建与离线验收.md) | 离线基建验收：就绪灯、本地/LLM 夹具泵（真 PNG）、报警·烟测、BIN 清单 |
| [`auto_lie/P0a_锚点复核.md`](auto_lie/P0a_锚点复核.md) | **测谎数据源**：UIAntiMacro* Prefab、jpegData/path、WM+0x1D0、`IsOpenAntiMacro@0x936780` |
| [`auto_supply/模块设计.md`](auto_supply/模块设计.md) | **自动回城卖/补给**：就近寻店卖装 + 去店用卷 `SendPortalScrollUseRequest`（2030000/2030059）+ 可选补货；Charge/回程用卷待验 |
| [`auto_supply/P2_货架寻店.md`](auto_supply/P2_货架寻店.md) | 按货架/物品码全局寻店：**不做**（无 SetShopDlg Commodity 全表；产品语义为就近能卖） |
| [`auction_town_bypass/模块设计.md`](auction_town_bypass/模块设计.md) | **野外开拍卖** ✅ 零 `.text`；默认开；服端断线+守护会干净重拉 |
| [`drop_alert_bypass/模块设计.md`](drop_alert_bypass/模块设计.md) | **战斗中可丢物** ✅ 数据面清 `LocalUser+0x114`；抑制客户端警戒；默认关 |

---

## 4. 安全与完整性（security/）

| 文档 | 主题 |
|------|------|
| [`security/ClientFileCRC.md`](security/ClientFileCRC.md) | 登录阶段客户端文件 CRC：**安装树完整性校验**，非 AppData 外挂扫描；附 154 条实抓清单 |
| [`security/MscSecurity能力面.md`](security/MscSecurity能力面.md) | RawInput 反宏、窗口子类化、DriveType SSD IOCTL、MultiClient 单实例；与 BlackCat 边界 |
| [`security/GRAP与枫星对齐.md`](security/GRAP与枫星对齐.md) | 同 MD5 套件；LoadLibrary 弱；**默认禁止 INLINE HOOK**；§4.1 NGS 单文件有条件例外 |
| [`security/MemoryCrc派发与节奏.md`](security/MemoryCrc派发与节奏.md) | MemoryCrc RpmScan / vptr+8 / Init·Iter；**证伪**旧 DRBG→Virt30 链；周期派发未决 |
| [`security/NGS补丁与CRC.md`](security/NGS补丁与CRC.md) | 同行单文件 `NGS.EXE.CRC`；ProgramData `NGService` patch 门禁；GA 写探针顺序 |
| [`security/GA文本探针.md`](security/GA文本探针.md) | 2026-08-01：进程内 GA `.text` 填充探针 **PASS**；边界与开关；≠ 业务 E9 |
| [`security/KRW驱动.md`](security/KRW驱动.md) | 本仓自研 `tools/krw`（IOCTL R/W）；对照 RW_Driver 不入库；**非** AC bypass |
| [`security/客户端Hack标志与服端推断.md`](security/客户端Hack标志与服端推断.md) | Float/AB（**VecCtrlMob**）+ `ClientHacksType` → 服端举报链；**≠** 玩家飞天校验 |
| [`security/攻包计数窗与type20.md`](security/攻包计数窗与type20.md) | SecurityClient **60s/2000** 攻包窗；`IsAttackPacket` 白名单；type20/21；BIN `SecAttack` |
| [`security/检测面盘点与187秒墙.md`](security/检测面盘点与187秒墙.md) | **我方检测面 SSOT**：3 分钟断线 = 自家 HWBP（93 段零越线）；`Collecting from unknown thread` 根因；三层检测面清单；`.text` 授权不再自签 |

原始数据：`Dumps/client_file_crc_paths.{json,tsv}` · 采证 DLL：`Dumps/runtime/out_bin/ClientFileCrcTrace.dll`

---

## 5. 相关代码入口

| 路径 | 说明 |
|------|------|
| `xcat_app/` | 产品 ImGui 壳 → `bin/xcat.exe` |
| `launcher/msc_launch.{h,cpp}` | NGM 启动骨架 |
| `launcher/msc_webview_login.*` | WebView 一键换票会话（链进 `xcat.exe`） |
| `Dumps/` | dump.cs / opcode / Msc.Security 笔记 |
| `x/features/invuln/` | 无敌 ✅ v2.6.3（hit `+0x298` + 帧钉/`8ms`；重绑 400ms + grace 1.5s）；见 [`invuln/模块设计.md`](invuln/模块设计.md) |
| `x/features/attack_accel/` | 攻击加速 ✅ BIN：清忙锁 + `SS+1BC=140`；见 [`attack_speed/模块设计.md`](attack_speed/模块设计.md) |
| `x/runtime/main_thread_pump.*` | Unity 主线程泵；`SetFrameTick`（invuln 去闪） |
| `x/ipc/payload_control.*` | 面板 ↔ payload：`user.ini [core]` |
| `x/features/kick_sniff/` | 断线 / pendingError 轮询；见 [`kick_sniff/断线错误码.md`](kick_sniff/断线错误码.md) |
| `x/features/titlebar/` | 标题栏：`titlebar.cpp` 编排 + `titlebar_game` 读 + `titlebar_win` 写；见 [`titlebar/模块设计.md`](titlebar/模块设计.md) |
| `x/features/autopot/` | 自动喝药；见 [`autopot/模块设计.md`](autopot/模块设计.md) |
| `x/ui/player_vitals.*` | 共享 HP/MP 读数（WM→CS 单真源）+ 可信闩锁 |
| `x/features/ports/mob_pool_port.*` | 活怪 n + 刷怪槽 M；见 [`mob_pool/活怪n与刷怪槽M.md`](mob_pool/活怪n与刷怪槽M.md) |
| `x/features/ports/player_combat_port.*` | LocalUser 战斗坐标 |
| `x/features/ports/attack_input_port.*` | 普攻键脉冲（InjectKeyHold） |
| `x/features/mob_scan/` | 扫怪 worker → `[core] mobScanIntervalMs` · 事件唤醒 / 按需刷新；见 [`mob_scan/模块设计.md`](mob_scan/模块设计.md) · n/M 见 [`mob_pool/活怪n与刷怪槽M.md`](mob_pool/活怪n与刷怪槽M.md) |
| `x/features/simple_combat/` | 状态机打怪 → `[core] simpleCombat` · F5 · Impact贴怪默认（含 `heli_rotor` 旋翼环）· **防抖** [`simple_combat/防抖.md`](simple_combat/防抖.md) · `logs/combat.log` |
| `x/features/ports/teleport_port.*` | QueryFlightState / ImpactSetVelocity / ImpactImpulseToward + fill+Doing；见 [`teleport/模块设计.md`](teleport/模块设计.md) · [`P0d`](teleport/P0d_fill_slim软重载结案.md) |
| `x/features/fly/` | F6 Impact 飞 + fh-ban；见 [`fly/模块设计.md`](fly/模块设计.md) |
| `x/features/travel/` + `ports/travel_port.*` + `worldmap_marker_travel/` | 同盘赶路 + Spot 入口；见 [`travel/模块设计.md`](travel/模块设计.md) · [`worldmap_marker_travel/模块设计.md`](worldmap_marker_travel/模块设计.md) |
| `x/features/pet_feed/` + `ports/pet_port.*` | P0b 宠物只读探针 → `logs/petfeed.log` |
| `x/features/pet_loot/` + `ports/drop_pool_port.*` | 宠物吸物 → `user.ini [pet_loot]` · `logs/petloot.log` |
| `x/features/timed_keys/` + `ports/input_port.*` | 定时按键 → `user.ini [timed_keys]`；见 [`timed_keys/模块设计.md`](timed_keys/模块设计.md) |
| `x/features/buffs/` + `ports/skill_port.*` | BUFF 续航 → `user.ini [buffs]` + runtime SHM；见 [`buffs/模块设计.md`](buffs/模块设计.md) |
| `x/features/multi_skill/` + `ports/multi_skill_port.*` + `skill_port.*` | 技能多发 → `[core] multiSkill*` + `multiskill_select.tsv`；见 [`multi_skill/模块设计.md`](multi_skill/模块设计.md) |
| `x/features/auto_enter/` | 自动进游戏；见 [`auto_enter/模块设计.md`](auto_enter/模块设计.md)、[`选角与SelectedIndex锚点.md`](auto_enter/选角与SelectedIndex锚点.md) |
| `x/features/soft_login_probe/` | 软重连试连；见 [`soft_login/模块设计.md`](soft_login/模块设计.md) |
| `x/features/ccu/` | 分区 CCU（登录页/auto_enter 喂数）→ SHM → 底栏；见 [`ccu/模块设计.md`](ccu/模块设计.md) |
| `x/features/channel_hop/` | 随机换频 → `[core] manualRejoinSeq`；见 [`channel_hop/模块设计.md`](channel_hop/模块设计.md) |
| `x/features/encounter/` | 遇人策略 → `[core] autoRelogin*`；见 [`encounter/模块设计.md`](encounter/模块设计.md) |
| `x/features/auto_supply/` + `ports/shop_port.*` + `ports/consumable_port.*` | 自动回城卖/补给 → `[auto_supply]`；去店用卷 `PortalScroll`；见 [`auto_supply/模块设计.md`](auto_supply/模块设计.md) |
| `x/features/auction_town_bypass/` | 野外开拍卖 → `[core] auctionTownBypass`；见 [`auction_town_bypass/模块设计.md`](auction_town_bypass/模块设计.md) |
| `x/features/drop_alert_bypass/` | 战斗中可丢物 → `[core] dropAlertBypass`；见 [`drop_alert_bypass/模块设计.md`](drop_alert_bypass/模块设计.md) |
| `DumpRestoredData/` | dump.cs 符号恢复分档（titlebar 偏移锚点） |
