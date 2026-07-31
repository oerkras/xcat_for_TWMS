# Msc.Security 其它能力面（RawInput / 窗口 / IOCTL）

> 与 [`ClientFileCRC.md`](ClientFileCRC.md) 互补：CRC 管**文件完整性**；本页管**输入反宏、窗口子类化、磁盘类型探测、单实例**等。  
> 真源：CMS CW 可读 dump（`Dumps/cms_cw/dump.cs`）+ TW 同构哈希类；运行时另见 `\Device\BlackCat1` / `Global\MscClientMtx`。  
> 日期：2026-07-30

---

## 0. 一句话总览

| 模块 | CMS 名 | 干什么 | 算不算「反作弊」 |
|------|--------|--------|------------------|
| **KeyMacroAnalyzer + RawInputHandler** | 同左 | 吃真·Raw 键鼠，做**按键宏/连点/异常速率/设备句柄**检测并上报 | **是（反宏）** |
| **WndHandler** | 同左 | 子类化游戏主窗，拦截 `WM_INPUT` 等喂给 RawInput | **基础设施**（服务反宏） |
| **DriveTypeChecker** | 同左 | `IOCTL_STORAGE_QUERY_PROPERTY` 查当前盘是否 SSD | **性能策略**（CRC 并行/单线程），不是扫外挂 |
| **MultiClient** | 同左 | `Global\MscClientMtx` 单实例 + 找窗前置 | **防多开**（弱） |
| **GRAP / BlackCat** | 另线 | `\Device\BlackCat1` 内核 AC | **是（主 AC）**，≠ 上面 Win32 IOCTL |

---

## 1. RawInput → 反按键宏（核心）

### 1.1 链路

```
WndHandler.RegisterWndProc  (SetWindowLongPtr 挂自定义 WndProc)
  → WM_INPUT (255)
  → RawInputHandler.ProcessRawInputMessage
  → GetRawInputData / GetRawInputBuffer
  → KeyMacroAnalyzer.Put(handle, message, key)
  → KeyMacroCheckObject 模式匹配 / 速率检查
  → SendInputMacroHackLog …（疑似上报）
```

- 注册设备：`HID_USAGE_GENERIC_MOUSE` + `KEYBOARD`（`m_DeviceCount = 2`）
- 主控：`KeyMacroAnalyzer : SingletonBehaviour`（TW 哈希锚 `a408fa41…`）
- 开关回调：`IsOpenAntiMacro_Keyboard` / `IsKeyMacroPass` / `IsKeyMacroCheckField`

### 1.2 在查什么（方法名自解释）

| 能力 | 方法/字段 | 含义 |
|------|-----------|------|
| 按设备记账 | `keyboards` / `mouses`：`Dictionary<IntPtr, KeyMacroCheckObject>` | 每个 Raw 设备句柄单独建检测对象 |
| 模式匹配 | `CheckFewKeyPattern` / `CheckPattern(s)` | 短/中/长按键序列（`KeyMacroType` CPT_SHORT…LONGLONG） |
| 业务场景句柄 | `SetHuntingHandle` / `SetCollectHandle` / `SetRuneHandle` / `SetAntiMacroInputHandle` | 打怪/采集/符文/反宏窗口各自绑定输入设备 |
| 速率异常 | `CheckAntiMacroKeyBoardRate` / `CheckRuneKeyBoardRate` | 同一场景下键盘事件速率是否异常 |
| 句柄一致性 | `SendInputKeyHandleCheck` / `IsSameInputKeyHandle` | 是否始终同一物理设备在打（防远程注入/切换） |
| 录制缓冲 | `MAX_RECORD_COUNT=5000` · `MAX_RECORD_MINUTES=5` | 短时 keylog 样条，供模式分析 |
| 输入类型 | `KeyInputTypeAnalyze`：Etc / Skill / Collect / Rune | 按玩法分类 |

**结论**：这是典型的 **用户态反键盘宏 / 连点器 / 异常输入源**，不是扫进程列表、也不是扫 AppData。

置信度：方法与类型名为 CMS 明文，**高**。

---

## 2. 窗口钩子（WndHandler）— 喂数据的管道

不是「枚举所有桌面窗口找外挂标题」为主业，而是：

1. `EnumWindows` + `GetWindowThreadProcessId` → 找到本进程主窗  
2. `SetWindowLongPtr(GWL_WNDPROC)` → 保存 `unityWndProcHandler`，换成自定义 `WndProc`  
3. 自定义过程里处理：
   - `WM_INPUT` → RawInput  
   - 一系列鼠标消息（MOVE/LBUTTON…/WHEEL）  
   - `WM_DEVICECHANGE` → 键鼠热插拔  
   - `WM_SIZING` / `WM_GETMINMAXINFO` / `WM_NCACTIVATE` 等窗口态  
4. 未吃掉的消息 `CallWindowProc` 回 Unity 原过程  
5. `DumpRawInputRegistrations` / `GetRegisteredRawInputDevices` → 自检 Raw 注册是否被拆掉  

`everyNFrames` / `stallMs`：周期健康检查（子类化是否被冲掉、是否卡住）。

**结论**：**为反宏截获输入**；附带窗口生命周期维护。`FindWindow`/`ShowWindow`/`SetForegroundWindow` 主要在 **MultiClient** 里用于多开冲突时把已有窗拉前台，不是标题黑名单扫描。

---

## 3. 「部分 IOCTL」— 先分清两条线

### 3.1 DriveTypeChecker（`0x2D1400`）≠ BlackCat

CMS 明文：

```csharp
private const uint IOCTL_STORAGE_QUERY_PROPERTY = 2954240; // 0x2D1400
public static bool IsCurrentDriveSsd();
public static bool IsDriveSsd(string driveLetter);
```

结构体对齐 Windows：

- `STORAGE_PROPERTY_QUERY`（PropertyId / QueryType / AdditionalParameters）
- `DEVICE_SEEK_PENALTY_DESCRIPTOR`（`IncursSeekPenalty`）

即标准 **`IOCTL_STORAGE_QUERY_PROPERTY`**（`FILE_DEVICE_MASS_STORAGE=0x2D`，Function=`0x500`）：查卷是否有 seek penalty → **是否 SSD**。

调用语境：挂在 **ClientFileCRC** 算 CRC 路径上（SSD → 并行 / HDD → 单线程一类策略）。  
**不是**自研反作弊 IOCTL。

> 旧笔记 `msc_security_ioctl_notes.md` 曾把 `0x2D1400` 说成 BlackCat 自研码——**已作废**；以本文与 CMS 名为准。

### 3.2 BlackCat 设备（真 AC 通道，另线）

运行时句柄扫描实锤：进程打开 **`\Device\BlackCat1`**，以及磁盘上的 `BlackCat64.sys`。  
GRAP 侧更常走 **ntdll/syscall**，KernelBase IAT hook 往往打空。

**硬禁令（与 [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) §4.1 一致）**：

- **禁止 INLINE HOOK**：不得对 `ntdll` / `GameAssembly` / 任意模块 `.text` 做 E9/FF25/Detours 类改码。  
- 对 ntdll inline 已实测会杀进程；对 GA `.text` 同样落入 MemoryCrc / 完整性画像，**不因「只 hook 游戏」而豁免**。  
- 功能侧用数据面（字段、`_forcedFlush`、自然 Flush）替代入口 hook。

这与 DriveTypeChecker 的存储 IOCTL **不是同一条 API 语义**。

---

## 4. MultiClient（单实例）

- Mutex 名：`Global\MscClientMtx`（运行时亦见 `\BaseNamedObjects\MscClientMtx`）
- `Check()`：已存在 → `FindWindow` + `BringWindowToFront`（`ShowWindow`/`SetForegroundWindow`/`SetWindowPos`/`IsIconic`）
- 作用：**限制同机多开客户端**（或把已有实例拉起），强度有限，可被其它手段绕过，但是明确产品逻辑。

---

## 5. 其它相关（同程序集，便于地图）

| 类型 | 角色 |
|------|------|
| `HackLogHelper` | 把宏检测 detail / key pattern 编成日志串 |
| `SecType<T>` | 内存中加密小对象（防简单 CE 改关键标量） |
| `ClientFileCRC` | 见专文：安装树 CRC |
| Registry SN 辅助 | `GetUserRegistrySN` / `EncodeUserRegistrySNBuffer` 等（机器特征上报相关，细节待深挖） |

---

## 6. 能力边界（回答「还有没有反作弊」）

```
┌─────────────────────────────────────────────┐
│ GRAP / NGS-X / BlackCat（内核+用户态）        │  ← 主反作弊
├─────────────────────────────────────────────┤
│ Msc.Security KeyMacro + RawInput + WndProc   │  ← 反键盘宏/异常输入
│ Msc.Security MultiClient mutex               │  ← 弱·防多开
│ Msc.Security ClientFileCRC                   │  ← 文件完整性（非扫挂）
│ Msc.Security DriveTypeChecker IOCTL          │  ← SSD 探测（性能）
└─────────────────────────────────────────────┘
```

**没有**从 ClientFileCRC 清单推出「完全没反作弊」；  
**也没有**把 DriveTypeChecker 的 `0x2D1400` 当成「驱动杀软指令」。

GRAP 自身强弱定调（与枫星同包实测对齐）见 [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md)：对正规 `LoadLibraryW` **弱**，对匿名映像/外部乱写 **敏感**，对 **INLINE HOOK（改 `.text`）一律禁止**；经典版额外盯 Msc 反宏与 CRC。

---

## 7. 证据指针

| 材料 | 路径 |
|------|------|
| CMS 明文类 | `Dumps/cms_cw/dump.cs`：`KeyMacroAnalyzer` / `RawInputHandler` / `WndHandler` / `DriveTypeChecker` / `MultiClient` |
| TW 哈希锚 | `a408fa41…`（KeyMacro 主控）、`c685f5ba…`（DriveTypeChecker 结构）、`a45ed966…`（ClientFileCRC） |
| BlackCat 句柄 | `Dumps/msc_security_ioctl_notes.md` § v6.1（设备名结论仍有效；IOCTL 码归因以本文 §3 为准） |
| CRC 专文 | [`ClientFileCRC.md`](ClientFileCRC.md) |
