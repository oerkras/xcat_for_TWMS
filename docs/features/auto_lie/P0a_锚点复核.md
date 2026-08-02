# Classic TWMS 测谎（AntiMacro）数据源 · P0a 锚点

> **产品**：新楓之谷：經典版（TWMS）· **不是**枫星  
> **状态**：✅ 静态锚点已钉（dump + IDA）；**离线基建 / 文件泵已实现**（见 [`模块设计.md`](模块设计.md)）；实机开题采样仍 NOT RUN  
> **IDB**：`Dumps/runtime/GameAssembly.dll.i64`（imagebase `0x7FFB16B40000`）  
> **对照**：CMS 明文 `Dumps/cms_cw/dump.cs` ↔ TW 混淆 `Dumps/runtime/out/dump.cs`  
> **对照仓（设计模式）**：枫星 `auto_lie` / `resource_cache` —— **经典版无 UGC resource_cache**，题图走封包 `jpegData`  
> **玩家道具**：Consume **`2190000` 測謊機** → 对战斗态他号使用后，对方弹本表 UI（与服端弹窗同源）；发起发包 opcode **NOT RUN**

---

## 0. 与枫星 Cache 灯语义差异（硬约束）

| | 枫星 | 经典版 TWMS |
|---|---|---|
| 第 5 灯 `quizCache` | `resource_cache` 测谎题图根已发现 | **应对齐「测谎数据面就绪」**，不是 Worlds 路径 |
| 题图来源 | UGC `.win.mod` / resource_cache | **`AntiMacroTextCaptchaInfo.jpegData`（封包 byte[]）** |
| 轨迹题 | `UIAntiMacro`（UGC 原生） | **`UIAntiMacroNonFinite`**（同系状态机：5s 准备 / 330 点） |

建议：`quizCacheRootOk=1` 当 **UIAntiMacroUtil + TextCaptcha + NonFinite 三类 `FindClass` 成功**（klass resolve，latch）。勿照搬枫星磁盘 cache 路径；勿用 `IsOpenAntiMacro`（那是事件灯）。

---

## 1. UI 对话框（Prefab 名明文 · 类名哈希）

| Prefab | TW TypeDef | 角色 | Open / IsInstantiated RVA |
|---|---|---|---|
| `UIAntiMacroNonFinite` | `f72ddff9…` (108) | **鼠标轨迹测谎**（shader + path） | Open `0x925B30` · Get `0x9265C0` · IsInst `0x926960` |
| `UIAntiMacroTextCaptcha` | `c16991b3…` (113) | **文字/JPEG 验证码** | Open `0x931CD0` · Get `0x9332E0` · IsInst `0x933680` |
| `UIAntiMacroCharacterName` | `e0929b05…` (104) | 角色名确认 | Open `0x9215C0` |
| `UIAntiMacroNotice` | `ce32b71f…` (110) | 通知条 | Open `0x92F950` |
| `UIAntiMacroGuideNonFinite` | `f9c241d3…` (105) | 轨迹题引导 | Open `0x922D70` · IsInst `0x923130` |

> IDA 侧方法体多为控制流平坦化（dispatcher + `jmp rax`）；**布局与 RVA 以 Il2CppDumper 为准**，伪代码勿盲信。

---

## 2. 封包 Info（题面真源）

### 2.1 `AntiMacroNonFiniteInfo`（TW `ca59e12a…`）

| Off | CMS 名 | 类型 | 用途 |
|---|---|---|---|
| `+0x10` | `level` | `int` | 难度档 |
| `+0x14` | `rand` | `Vector2` | 随机种子/偏移 |
| `+0x20` | `path` | `List<Vector4>` | **加密轨迹** → `DecodePath` / `MakePathCanvas` |

构造：`.ctor(InPacket)` · TW RVA `0x9253F0`

### 2.2 `AntiMacroTextCaptchaInfo`（TW `c3f7ac1f…`）

| Off | CMS 名 | 类型 | 用途 |
|---|---|---|---|
| `+0x10` | `timeInterval` | `int` | 倒计时相关 |
| `+0x18` | `remainChance` | `long` | 剩余次数 |
| `+0x20` | `jpegData` | `byte[]` | **题图 JPEG**（= 经典版「cache」等价物） |

构造：`.ctor(InPacket)` · TW RVA `0x931C30`

**TextCaptcha 常量**（TW enum `ed975dbf…` ≡ CMS `AntiMacroTextCaptchaConstant`）：FontBox 196×44、MovingImage 100×40、`MaxTimeSeconds=599`、Alpha 150/240。

---

## 3. `UIAntiMacroNonFinite` 运行时字段（CMS→TW 偏移漂移）

基类 `UIDialog` 后字段整体 **+0x20**（TW SerializeField 从 `0xA0` 起，CMS 从 `0x80`）。

| 语义 | CMS | TW |
|---|---|---|
| RawImage 渲染 | `+0x80` | `+0xA0` |
| Material | `+0x88` | `+0xA8` |
| `lieDetectorBaseTexture` | `+0x90` | `+0xB0` |
| 描述 Text | `+0x98` | `+0xB8` |
| ShaderModule | `+0xA8` | `+0xD0` |
| AntiMacroInfo | `+0xB0` | `+0xD8` |
| TickCounter | `+0xB8` | `+0xE0` |
| `_rawPosList` | `+0xC0` | `+0xE8` |
| `_mousePosList` | `+0xC8` | `+0xF0` |
| `_isResultRecv` | `+0xD0` | `+0xF8` |
| `_pathTexture` | `+0xD8` | `+0x100` |
| `_isSuccess` | `+0xE0` | `+0x108` |

关键方法（TW RVA）：`CreateLieDetector 0x925D30` · `Update 0x9282F0` · `DecodePath` 系 · `SendAnswer` 系（见同 TypeDef 方法表）。

---

## 4. `UIAntiMacroTextCaptcha` 运行时字段（同样 +0x20）

| 语义 | CMS | TW |
|---|---|---|
| textureRenderer | `+0x80` | `+0xA0` |
| movingRenderer[] | `+0x88` | `+0xA8` |
| inputField | `+0x90` | `+0xB0` |
| btOk | `+0x98` | `+0xB8` |
| remainChance Text | `+0xD8` | `+0xF8` |
| `_timeOver` / `_remainSeconds` | `+0xE0`/`+0xE4` | `+0x100`/`+0x104` |
| `_baseTexture` / `_movingTexture` | `+0xE8`/`+0xF0` | `+0x108`/`+0x110` |

---

## 5. `UIAntiMacroUtil`（探测 / 难度 / 画布）

- **TW 类哈希**：`fcab468da59ba173dd671940c0be7136e7f14f6109ee602381b891064720afb`
- **NonFiniteConstant**（与 CMS 数值一致）：`TICK=1000` · `FRAME_PER_TICK=33` · `END_READY_FRAME=149` · `START_SOLVING_FRAME=150` · **`POS_COUNT=330`** · EasyVanish=100 · HardVanish=20

| CMS API | TW RVA | 用途 |
|---|---|---|
| `IsOpenAntiMacro` | **`0x936780`** | **总开关探测（首选）** |
| `IsOpenAntiMacro_Keyboard` | `0x9367B0` | 键盘宏场景 |
| `TryGetWinCursorPos` | `0x936C30` | 轨迹物理路径 |
| `TryGetCursorPos` | `0x937040` | 面板局部坐标 |
| `GetDifficulty` | `0x937330` | 难度表 |
| `SetDifficulty(shader, lv)` | `0x92CB30` | 写 LieDetectorShader |
| `GetShape` | `0x937580` | 形状枚举 |
| `MakePathCanvas(path)` | `0x92D3E0` | path → 纹理 |

---

## 6. WorldManager 计数（风控感知）

| 语义 | CMS WM | TW WM（`a480358a…`） |
|---|---|---|
| `_remainAntiMacroQuestion` | `+0x198` | **`+0x1D0`**（`int b2771ff6…`） |
| `_remainInitialQuiz` | `+0x19C` | **`+0x1D4`**（`int cb339368…`） |

已写入 `docs/features/world_manager/字段全表.md`；读口可挂 `ports::world` 旁路只读，**勿 FindAll 另开一套**。

---

## 7. 推荐消费顺序（实现 auto_lie 时）

```
1) Resolve UIAntiMacroUtil TypeObject → quizCacheRootOk 可点亮（基础设施）
2) Tick: call IsOpenAntiMacro (0x936780) 或 TextCaptcha/NonFinite.IsInstantiated
3) 分支:
   - TextCaptcha: 读 Info.jpegData → OCR/视觉 → 填 inputField → SendAnswer
   - NonFinite: 读 Info.path + TickCounter 帧 → 轨迹策略（对照枫星 UIAntiMacro 文档，偏移用本表 TW 列）
4) SoftRead: WM+0x1D0/0x1D4 剩余题；面板 fail/total 待另钉 MonitoringList（本轮未扫）
```

---

## 8. 本轮未做 / 风险

| 项 | 状态 |
|---|---|
| 封包 opcode（开测谎 / 交答案） | **NOT RUN** — 需抓包或跟 Open 调用方 |
| `MonitoringList` 失败/总数 SoftRead | **NOT RUN** |
| IDA 伪代码（平坦化） | 仅作旁证；以 dump 布局为准 |
| 实机点亮 Cache 灯 | ✅ 已接：`payload_status` → `FindClass(Util+NonFinite+TextCaptcha)` latch → `quizCacheRootOk` |

---

## 9. 证据指针

- Prefab 串：`Dumps/runtime/out/dump.cs` L4187+（`UIAntiMacro*`）
- WM 字段：同文件 L61336–61338（`+0x1D0`/`+0x1D4`）
- Util：同文件 L4704–4739 · RVA `0x936780`
- CMS 明文对照：`Dumps/cms_cw/dump.cs` L3963–4401
- IDA：`GameAssembly.dll.i64` · VA = `0x7FFB16B40000 + RVA`
