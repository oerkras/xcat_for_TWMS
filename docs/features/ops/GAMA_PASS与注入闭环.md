# GAMA PASS 无人值守与注入闭环（经典版）

> **产品**：新楓之谷：經典版（TW · beanfun / Gamania Galaxy · `Maplestory_Classic.exe`）· **不是**枫星  
> **状态**：✅ 产品路径已落地（build 56 实机闭环：冷启 → CDP 换票 → 接管/NGM → LoadLibrary → Bootstrap → auto_enter → Healthy）  
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
| GAMA PASS自动登录 | `GamaPassAuto` | CDP 点选 | 约 7s 后自动 **换票+开游戏+注入** |
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

入口：`msc::weblogin::StartOneClick`（策略 `GamaPassAuto`）→ `gamapass_cdp_login`。

```
StartOneClick
→ 探测默认浏览器（优先 Chrome++/Chrome，其次 Edge）
→ CDP 开 Galaxy 登录页
→ 单次点击 Gama Pass
→ select-account：按 **登录账号序号**（1=第一个，自上而下）点第 N 张卡 + 一次中心坐标 MouseEvent('click')
→ 昵称：按 **游戏昵称序号**（1=第一个）勾选 radio/label + 单次「繼續」
→ result 页拿 access_token / 等官网回跳
→ 见 NGM：立刻关登录用调试浏览器（票已在拉起链上；成功门禁仍等经典版 cmdline）
→ 经典版 cmdline 四元组匹配 → 接管；否则 NGM deep-link
→ InjectIntoClassic
→ （若浏览器仍在）收尾再关一次调试口
```

要点：

| 项 | 约定 |
|---|---|
| 点击风暴 | GP / 选号 / 昵称均为 **单次**点击（防卡在 select-account） |
| 账号 | UI「登录账号」→ 第几个（1=第一个）；落盘 `gamapass_account_slot.txt`；CDP 单邮箱去重后自上而下取第 N 张（父节点 ≥2 邮箱不向上扩）；越界钳末项；换票中禁用 |
| 昵称 | UI「游戏昵称」→ 第几个（1=第一个）；落盘 `gamapass_nick_slot.txt`；跳过「建立暱稱」后取第 N 个；越界钳末项；换票中禁用 |
| HTTP 死路径 | `http_gamapass_login.*` 非主路径；`TrySubmitSelectGameAccount` 已跟 `GetGamaPassNickSlot`（radio/select 第 N 项；仅 `__doPostBack` 时退回第一项并打日志）；禁止 `prompt=login→none` 改写 |
| WebView2 | **已拆除**；GamaPass / HK 均不依赖 |
| Edge-only 用户 | 须先卸 Google Chrome，否则会绑到空 Chrome 会话 |

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

---

## 6. 实机证据（摘录）

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

---

## 7. 关键文件

| 路径 | 职责 |
|---|---|
| `launcher/gamapass_cdp_login.*` | CDP 点选状态机；`Get/SetGamaPassAccountSlot`、`Get/SetGamaPassNickSlot` |
| `launcher/chromium_cdp.*` | 调试口 / Runtime.evaluate |
| `launcher/msc_webview_login.*` | 一键会话编排 → 注入 |
| `launcher/msc_launch.*` | NGM deep-link / 接管验票 |
| `injector/classic_loadlibrary.*` | CRT LoadLibraryW |
| `injector/inject_after_launch.*` | 等 GA + settle + 注入 |
| `xcat_app/attach_inject.*` | 启动模式 + 附着监视 |
| `xcat_app/launch_panel.*` | 自动启动 / 取消 / StartOneClick |
| `xcat_app/hangup_schedule.*` | 挂机时段 + 守护干净重拉 |
| `xcat_app/workspace_tabs.cpp` / `status_bar.cpp` | 启动页与顶栏 UX |
| `x/`（payload） | Bootstrap / MainPump / auto_enter / 功能 |

日志：`bin/launcher.log` · `XCat_data/logs/{inject,x,kick,auto_enter}.jsonl|log` · 上传见 `user_log_uploads/`。

---

## 8. 故障速查

| 现象 | 优先看 |
|---|---|
| 停在 select-account | `msc_launcher.log` 是否 `clicked-acc` 后仍不离页；应用单次坐标点击包 |
| 官网「登录阶段超时」 | GP 点击后是否长时间停在 Galaxy；见 NGM 后是否 park Main |
| 绑到空 Chrome | Edge-only 用户未卸 Google Chrome |
| 注入失败 OpenProcess | 管理员 / SeDebugPrivilege 日志 |
| 注入成功但不进图 | `x.jsonl` Bootstrap / auto_enter；非进程外 3s settle 单独问题 |
| 守护卡 `status-not-ready` | `awaitingPlayable` + 进程是否已死；见 hangup_schedule 粘性死进程路径 |
| 想取消 7s 自动 | 点「取消自动换票」或顶栏「取消」 |

---

## 9. 非目标

- 把枫星 `WaitForIl2CppInjectionReady`（UnityPlayer + 20s）原样搬进本仓默认路径  
- Manual Map / stealth 作为产品默认  
- 清浏览器登录态或 refresh 刷票  
- 将本仓表述成「枫星项目」
