# GAMA PASS 无人值守与注入闭环（经典版）

> **产品**：新楓之谷：經典版（TW · beanfun / Gamania Galaxy · `Maplestory_Classic.exe`）· **不是**枫星  
> **状态**：✅ 产品路径已落地；守护重拉+SSO 存活已有实机（如 `adc7b2`）；顶栏自动登录提示见 §5.4  
> **对照**：枫星仓 `xcat_for_fengxing` 仅作注入手法与门闩设计对照，**不得**硬套 `/sid:` / `msw.exe` 语义  
> **关联**：[`启动系统实现.md`](启动系统实现.md)（NGM / Galaxy 票）、[`GRAP与枫星对齐.md`](../security/GRAP与枫星对齐.md)、[`auto_enter/模块设计.md`](../auto_enter/模块设计.md)

---

## 1. 要解决什么

无人值守把角色送进图并保持可挂机：

1. **换票**：优先 GAMA PASS 浏览器点选（无需账密、不调 refresh）  
2. **开游戏**：官网/NGM 拉起或 **cmdline 票匹配后接管**已有经典版  
3. **注入**：正规 `CreateRemoteThread` → `kernel32!LoadLibraryW`  
4. **就绪**：进程内 Bootstrap（native settle → MainPump → LOGIN → play-ready → PLAY）  
5. **守护**：进程死 / 踢图 / 未进图卡死 / 心跳陈旧 → 干净重拉再走一键  

硬红线（与实现一致）：

- **不**调用 `/v1/refresh/token`  
- **不**清浏览器 Cookie / LocalStorage  
- **不**改写 OAuth `prompt` 刷 SSO  
- **不**默认 Manual Map / `NtCreateThreadEx` 落匿名映像 / 改 `GameAssembly` `.text`

---

## 2. 启动模式（用户可见）

| Combo | `LaunchMode` | 取票 | 冷启/切模式自动？ |
|---|---|---|---|
| 手动启动并注入 | `AttachWatch` | 不换票 | 约 7s 后自动 **监视** |
| GAMA PASS自动登录 | `GamaPassAuto` | 日常浏览器 UIA 点选 | 约 7s 后自动 **换票+开游戏+注入** |
| gamania (HK) | `OneClickLogin` | HTTP/WebView 账密 | **否**（手点「一键启动」；7s 防误触） |

落盘：`XCat_data/state/launch_mode.txt`（及安装根同步）。  
策略：`auth_strategy.txt`（GamaPass 模式强制 `GamaPassAuto`）。  
昵称：`gamapass_nick_slot.txt`（客户面：第几个游戏昵称；1=第一个；跳过「建立暱稱」）。  
账号：`gamapass_account_slot.txt`（客户面：登录第几个账号；1=第一个；Gama Pass 列表自上而下）。

### 2.1 自动启动与打断

| 行为 | 实现 |
|---|---|
| 武装自动 | `LaunchUiState.pendingAutoLaunch` + `LaunchPanel_ArmStrategyPrep(7000)` |
| 到期开跑 | `LaunchPanel_TryAutoLaunchWhenReady` → `LaunchPanel_StartOneClick` |
| 倒计时内再点 | 按钮文案变为「取消自动换票 / 取消自动监视」；`LaunchPanel_CancelPendingAutoLaunch` 清 pending + 准备窗 |
| 顶栏小按钮 | 待办中显示「取消」，同语义 |
| 已在换票中 | `IsBusy` 时按钮禁用；成功启动后清 pending，避免第二发 |

---

## 3. GAMA PASS 换票流水线

入口：`msc::weblogin::StartOneClick`（策略 `GamaPassAuto`）→ `HttpGamaPassUiaLoginToOtt`。

```
StartOneClick
→ 探测系统默认 Chromium 浏览器（Https UserChoice；非 Chromium 再安装回退）
→ CreateProcess 日常目录：--force-renderer-accessibility --new-window Galaxy URL
  （无 --remote-debugging-port、无 GamaPassCdpProfile）
→ Windows UI Automation 附着浏览器窗
→ 点 Gama Pass → 第 N 张账号卡 → 第 N 昵称 →「繼續」
→ 见 NGM / 等经典版 cmdline 四元组 → 接管
→ InjectIntoClassic
→ WM_CLOSE 本轮登录窗（不杀日常主进程）
```

要点：

| 项 | 约定 |
|---|---|
| 点击 | **GP / 繼續**：UIA Invoke 优先；**账号卡**：几何 `SendInput`（见 [`GAMA_PASS账号卡UIA点选.md`](GAMA_PASS账号卡UIA点选.md)） |
| 账号 | UI「登录账号」→ 第几个（1=第一个）；落盘 `gamapass_account_slot.txt`；UIA 按带 `@` 等 Name 自上而下取第 N 项 |
| 昵称 | UI「游戏昵称」→ 第几个；落盘 `gamapass_nick_slot.txt`；Radio/ListItem 第 N 项后点繼續 |
| HTTP 死路径 | `http_gamapass_login.*` 非主路径；禁止 `prompt=login→none` 改写 |
| WebView2 | **已拆除** |
| 浏览器选择 | **系统默认** Chromium |
| 守护干净重拉 | 同时结束 **Classic + NGM/NGM64**，再走一键 |
| 挂机关时段 | 同样结束 Classic + NGM |
| 挂机开时段冷启 | 无 Classic 时若仍有 NGM，先 `kill launch-chain` |
| SSO / Cookie | 见 §3.1；**正道=日常浏览器 UIA**；与日常同一 Cookie 罐 |
| 顶栏进度 | 见 §5.4 |
| CDP 旧路径 | `gamapass_cdp_login.*` / `chromium_cdp.*` **保留可编译**，一键入口不再调用 |

### 3.1 日常浏览器与 UIA（正道）

官方 Chrome 136+ 对默认 User Data **静默忽略**调试口，故产品主路径不再依赖 CDP/副本。

**正道**：日常 Chromium + `--force-renderer-accessibility` + Windows UI Automation 自动点选。会话与日常浏览同源，避免「副本登录顶掉日常 SSO」。

| 项 | 约定 |
|---|---|
| 会话目录 | 浏览器默认 User Data（不建 `GamaPassCdpProfile`） |
| 拉起 | `CreateProcess` + Galaxy URL；无 remote-debugging-port |
| 点选 | `launcher/win_uia.cpp` + `gamapass_uia_login.cpp` |
| 账号卡 | **几何 `SendInput` 为主**（Invoke 假成功）；门禁/cooldown/日志见 [`GAMA_PASS账号卡UIA点选.md`](GAMA_PASS账号卡UIA点选.md) |
| 收票 | `gamapass_ticket_harvest.cpp`：经典版 cmdline 四元组 |
| 落到账密页 | `ManualLogin`：提示在本窗登录；出现选账号后继续 UIA |
| 关窗 | 收票后对附着 HWND `WM_CLOSE`；**不清 Cookie / 不写回 / 不 refresh** |
| 日志锚点 | `[gamapass-uia]` / `click-gamapass` / `click-account-card` / `nick-` / `接管票` |

### 3.2 关浏览器时机（UIA）

| 时机 | 行为 |
|---|---|
| 收齐经典版 cmdline 票 | `WM_CLOSE` 本轮登录窗 |
| 超时 / 失败 | 同样尝试 `WM_CLOSE`；不 Terminate 日常主进程 |
| （历史）CDP `CloseRemoteBrowser` | 仅旧 CDP 路径；一键主路径不再走 |

票与 NGM 细节见 [`启动系统实现.md`](启动系统实现.md) §3–§5。

---

## 4. 注入实现（产品真源）

### 4.1 底层（与对照仓同源）

`injector/classic_loadlibrary.cpp`（与枫星仓同文件哈希一致）：

```text
VirtualAllocEx + WriteProcessMemory(DLL 宽路径)
  （失败 → NtMapViewOfSection 映射路径）
→ CreateRemoteThread(kernel32!LoadLibraryW)
→ 枚举 LDR 确认 xcat.dll 基址
```

### 4.2 经典版编排（进程外）

`injector/inject_after_launch.cpp` → `twms_inject::InjectIntoClassic`：

| 步骤 | 默认 | 说明 |
|---|---|---|
| 等模块 | `GameAssembly.dll` ≤120s | Psapi 优先；瞬时 `PARTIAL_COPY` 重试 |
| settle | **3s** | GA 出现 ≠ 托管就绪；细活交给 payload |
| 重复注入 | 已加载则跳过 | `payload already loaded` |
| 载荷路径 | `…/XCat_data/xcat.dll` | 旁路 exe |

调用方：

- 一键 / GamaPass：`launcher/msc_webview_login.cpp`  
- 附着监视 / 立即注入：`xcat_app/attach_inject.cpp`

### 4.3 进程内冷启（载荷）

注入成功后 `xcat.dll` Bootstrap（日志可见）：

```text
GA exports → native settle ≈15s + UnityWndClass
→ MainPump 真 tick → LOGIN workers（kick_sniff / auto_enter / …）
→ play-ready（map+WM）→ PLAY workers（combat / invuln / …）
```

经典版**磁盘上也有** `UnityPlayer.dll`（CRC 清单点名）。  
进程外**不**再单独 `Wait(UnityPlayer)`：通常等 `GameAssembly` 时 Player 已在；长稳定窗在进程内 15s settle 完成。

### 4.4 与枫星对照（编排差，手法同）

| 维度 | 经典版（本仓） | 枫星（对照） |
|---|---|---|
| 进程 | `Maplestory_Classic.exe` | `msw.exe` |
| 底层注入 | Classic `LoadLibraryW` | 同款 |
| 进程外等待 | GA + **3s** | GA → `UnityPlayer` → **~20s** 稳定窗 |
| 封装 | `InjectIntoClassic` | `InjectPayload` + session/build 门禁 |
| 活动 session 再注 | 已加载则跳过 | 常拒绝，需重启游戏 |
| 版本门禁 | 编排较轻 | 磁盘 DLL buildId 必须匹配 App |

**结论**：手法同级；编排各适其主。对本仓不宜原样搬 20s 外置等待；可择优考虑日后补 buildId 一致性门禁（非本闭环阻塞项）。

---

## 5. 无人值守与守护闭环

### 5.1 谁会自动一键

| 路径 | 是否自动 | 机制 |
|---|---|---|
| 冷启且已是 GamaPass | ✅ | `main.cpp` → `pendingAutoLaunch` + 7s |
| 切 Combo → GamaPass | ✅ | `workspace_tabs` 置 pending（可取消） |
| 挂机时段到点 | ✅ | `hangup_schedule::BeginCleanRelaunch` → `StartOneClick(..., honorStrategyPrep=false)` |
| 守护干净重拉 | ✅ | 同上；`NoteLaunchStarted` → `awaitingPlayable` |
| HK 账密一键 | ❌ | 需粘贴账密后手点 |

### 5.2 守护状态位（摘要）

`xcat_app/hangup_schedule.cpp`：

| 位 / 概念 | 作用 |
|---|---|
| `awaitingPlayable` | `NoteLaunchStarted` 置位；进图 / playable 后清 |
| `sessionArmed` | 本轮会话已武装守护 |
| ProcessDead 粘性 | `awaiting \|\| armed` 且有 `trackedPid` 时仍可走死进程重拉 |
| hangup vs watchdog | `armed \|\| awaiting` 时挂机让位给守护重拉 |
| 进度宽限 | `progressGrace`（2×N 后 N）、`prePlayableStuck`、`payloadHeartbeatStale` |

闸门日志字段示例：`gate=… watchdog=1 hangup=0 … trackedPid=… armed=… await=…`。

### 5.3 干净重拉 / 挂机：`KillLaunchChain`

`xcat_app/hangup_schedule.cpp`：

| API / 日志 | 行为 |
|---|---|
| `KillLaunchChain(why, tag)` | 结束 `Maplestory_Classic.exe` + `NGM64.exe` + `NGM.exe`；日志 `kill launch-chain (%s): Classic x? NGM64 x? NGM x?` |
| 干净重拉 WaitingGone | 等 **launch-chain**（Classic **或** NGM）都消失再 settle；超时 abort 带 `Classic=/NGM=` |
| 挂机关时段 | `KillLaunchChain("hangup-off", "Hangup")` |
| 挂机开时段冷启 | 无 Classic 且仍有 NGM → `KillLaunchChain("hangup-cold")`；清不掉则本 Tick 不 `StartOneClick` |

实锤对照：事故包只杀 Classic → 重拉卡 TokenWait → `/login`；修复后包见 `kill launch-chain` + 多轮 `Recovering→Healthy`（如 `…_491473` / `…_adc7b2`）。

### 5.4 顶栏状态条（首页默认可见）

用户默认停在**首页 TAB**，启动细节在启动页；故自动登录进度必须上顶栏第 4 行。

实现：`xcat_app/status_bar.cpp` + `launch_panel.cpp`（日志 → `ui.status`）。

| 行 | 内容 |
|---|---|
| 2/4 阶段 | `IsBusy` → `GAMA PASS登录中` / `换票中`；干净重拉中 → `干净重拉中` |
| 4/4 提示 | 倒计时 / 换票中 / 挂机 Starting / 守护 Recovering / 失败类文案 |

显示门禁（任一成立且 `ui.status` 非空）：

- `weblogin::IsBusy` 或注入忙碌  
- `pendingAutoLaunch`（含 7s 倒计时，每帧由 `TryAutoLaunchWhenReady` 刷新）  
- `IsCleanRelaunchInFlight`  
- 挂机 `UiMode::Starting` / 守护 `Recovering`  
- `StatusLooksActionable`（前缀：`GAMA PASS` / `挂机` / `失败` …，少靠零散单字）

样式：

| 主题 | 进行中 / 提示色 |
|---|---|
| 白天（Light） | 深蓝 ≈ `#0054A6` |
| 黑夜（Dark） | `brandText` 亮蓝 ≈ `#76BAFF` |
| 失败类 | 同系略沉的蓝（非暖黄/橙） |
| 过长 | 按可用宽度收成 `…`，悬停看全文 |
| 优先级 | 登录提示 **优先于** 更新进度条 |

文案示例：`GAMA PASS：约 N 秒后自动换票（顶部可取消，或切到启动页）` · `GAMA PASS 自动登录中…（浏览器点选换票，请稍候）`。

---

## 6. 实机证据（摘录）

### 6.1 冷启闭环（历史）

上传 `…_5de474` · **0.1.56 build 56** · light（2026-08-05）：

| 时间 | 事件 |
|---|---|
| 16:08:26 | 冷启 GamaPass · `pending auto` defer 5s |
| 16:08:31 | 自动 CDP 换票 |
| 16:08:35–44 | GP → 选号 → 昵称 → token |
| 16:08:47 | 接管 PID=2448（cmdline 票匹配，跳过 NGM 重开） |
| 16:08:48–51 | 见 GA → LoadLibraryW OK |
| 16:08:51–09:06 | payload 15s native settle → MainPump |
| 16:09:08–20 | auto_enter Done → `play-ready ok` |
| 16:09:20+ | Watchdog `Idle→Healthy` · 战斗已开火 |

### 6.2 守护重拉 + SSO 存活（修后）

上传 `…_adc7b2` · 标称 0.1.103（日志指纹已含修）：多轮 `err=205` → `kill launch-chain` → `select-account` 仍在 → `登录页已 blank` → 注入 → `Recovering→Healthy`；**无** `/login` / BadInput。

---

## 7. 关键文件

| 路径 | 职责 |
|---|---|
| `launcher/gamapass_uia_login.*` | **主路径** UIA 点选；日常浏览器 + 收票 |
| `launcher/win_uia.*` | UI Automation 封装；账号卡几何激活 |
| [`GAMA_PASS账号卡UIA点选.md`](GAMA_PASS账号卡UIA点选.md) | 账号卡 Invoke 不可用结论 + 门禁/cooldown/BIN 验收 |
| `launcher/gamapass_ticket_harvest.*` | 经典版 cmdline 收票共用 |
| `launcher/gamapass_cdp_login.*` | （保留）旧 CDP 状态机；一键入口不再调用；槽位 Get/Set 仍在此 |
| `launcher/chromium_cdp.*` | 调试口 / Runtime.evaluate；`PrepareCdpSafeUserData`；`RequestCdpSessionResync`；`CloseRemoteBrowser` 轮询落盘 |
| `launcher/msc_webview_login.*` | 一键会话编排 → 注入 |
| `launcher/msc_launch.*` | NGM deep-link / 接管验票 |
| `injector/classic_loadlibrary.*` | CRT LoadLibraryW |
| `injector/inject_after_launch.*` | 等 GA + settle + 注入 |
| `xcat_app/attach_inject.*` | 启动模式 + 附着监视 |
| `xcat_app/launch_panel.*` | 自动启动 / 取消 / StartOneClick；日志驱动 `ui.status` |
| `xcat_app/hangup_schedule.*` | 挂机时段 + 守护干净重拉 + `KillLaunchChain` |
| `xcat_app/workspace_tabs.cpp` | 启动页 Combo / 账号昵称 |
| `xcat_app/status_bar.cpp` | 顶栏 4 行；自动登录提示色与省略号 |
| `x/`（payload） | Bootstrap / MainPump / auto_enter / 功能 |

日志：`bin/launcher.log` · `XCat_data/logs/{inject,x,kick,auto_enter}.jsonl|log` · 上传见 `user_log_uploads/`。

---

## 8. 故障速查

| 现象 | 优先看 |
|---|---|
| 停在 select-account / 狂点账号列表 | 见 [`GAMA_PASS账号卡UIA点选.md`](GAMA_PASS账号卡UIA点选.md)：须 `click-account-card|…|pt|hit=` 且 cooldown≥2.5s；勿指望 Invoke |
| 官网「登录阶段超时」 | GP 点击后是否长时间停在 Galaxy；见 NGM 后是否 blank；干净重拉日志是否含 `kill launch-chain` 的 NGM 计数；残留旧 NGM 时 TokenWait 会空等 |
| 偶发首发 `accounts/error`、重开就好 | OAuth 半残态；现已自动 `oauth-error-clean-restart` 1 次。若日志无该标记仍失败：日常窗勾记住 / 关多余 Galaxy·OAuth 标签后再试 |
| 重拉后掉到完整 `/login` | 是否只杀了 Classic；是否过早 Browser.close；标准 Chrome 是否应用「复用/强制重同步」；日常窗是否仍勾选记住 |
| 绑到空 Chrome | Edge-only 用户未卸 Google Chrome |
| 注入失败 OpenProcess | 管理员 / SeDebugPrivilege 日志 |
| 注入成功但不进图 | `x.jsonl` Bootstrap / auto_enter；非进程外 3s settle 单独问题 |
| 守护卡 `status-not-ready` | `awaitingPlayable` + 进程是否已死；见 hangup_schedule 粘性死进程路径 |
| 想取消 7s 自动 | 顶栏「取消」或启动页「取消自动换票」；首页应能见倒计时蓝字 |
| 顶栏看不到进度 | 是否已含 `status_bar` 提示门禁包；`ui.status` 是否被更新条抢占（登录应优先） |

---

## 9. 非目标

- 把枫星 `WaitForIl2CppInjectionReady`（UnityPlayer + 20s）原样搬进本仓默认路径  
- Manual Map / stealth 作为产品默认  
- 清浏览器登录态或 refresh 刷票  
- 将本仓表述成「枫星项目」  
- 以「人手点同步」作为无人值守主路径（同步可自动；按钮仅排障可选）
