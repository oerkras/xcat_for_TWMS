# GAMA PASS 账号卡 UIA 点选（经典版）

> **产品**：新楓之谷：經典版（TW · beanfun / Gamania Galaxy · `Maplestory_Classic.exe`）· **不是**枫星  
> **父文档**：[`GAMA_PASS与注入闭环.md`](GAMA_PASS与注入闭环.md) §3.1  
> **实现**：`launcher/win_uia.cpp`（`ClickAccountCardIndex` / `ActivateAccountCard`）、`launcher/gamapass_uia_login.cpp`（`WaitAcc` 门禁与 cooldown）  
> **对照**：CDP 旧路径 `launcher/gamapass_cdp_login.cpp` · `JsSelectAccount`（一键入口不再调用）

---

## 1. 结论（定稿）

| 控件 | 主路径 | 说明 |
|---|---|---|
| Sign in with Gama Pass | UIA **Invoke** | 真按钮，a11y 直调有效 |
| 「繼續」 | UIA **Invoke** | 同上 |
| **账号卡**（`accounts.gamania.com/login/select-account`） | **几何 `SendInput`**（矩形中心） | Invoke **不可靠**；见 §2 |

**一句话**：日常浏览器（无调试口）下，账号卡没有已证可用的「纯 Invoke 选号」路径；标准解法是前景窗 + BoundingRect 中心 + `SendInput`，点完还原光标。

---

## 2. 为何 Invoke「成功」却不跳转

### 2.1 本仓 BIN 实锤（2026-08-09）

| 观测 | 含义 |
|---|---|
| 卡 `ControlType=50000`（Button），约 `345×68` | UIA 树里**有** `InvokePattern` |
| `Invoke()` 返回成功 | a11y 桥调通了 |
| 地址栏仍停在 `select-account` | React **没当成有效点击** |
| 同坐标几何点击后进 `SelectGameAccount` | 处理器认的是**系统输入队列上的真鼠标事件** |

日志形态（新二进制）：

```text
click-account-card|slot1|<名字>|345x68|t50000|v0|pt|hit=<same|…>|card（待确认跳转）
```

- `t50000` + 曾出现 `|inv` 且不跳 → Invoke 假成功  
- `|pt|hit=…` 后进昵称页 → 几何有效  

### 2.2 业界对照（为何不是本仓特例）

| 来源 | 要点 |
|---|---|
| [SO: Chrome UIA Invoke no result](https://stackoverflow.com/questions/28477479/chrome-net-ui-automation-automatic-sign-on-to-browser-automationelement-inv) | Invoke 实现在 AUT/Chrome provider；客户端改不了 → 兜底 BoundingRect + 模拟点击 |
| [cua#1623](https://github.com/trycua/cua/issues/1623) | Chromium **不处理** `PostMessage(WM_LBUTTON*)`；坐标点击须 `SendInput` 进系统输入队列 |
| [pywinauto#1478](https://github.com/pywinauto/pywinauto/issues/1478) | Chrome Custom 常 `LegacyIAccessible.DefaultAction` 为空，`DoDefaultAction` 无效 |
| React / CDP 生态（如 [kuri#164](https://github.com/justrach/kuri/issues/164)） | SPA 常要引擎级 `Input.dispatchMouseEvent`（trusted）；纯 a11y / 部分 JS `click()` 不够 |

Chromium 会把很多节点标成可 Invoke，但走的是 **a11y 默认动作**，不一定进入 React 根委派认的指针管线。

### 2.3 对照：CDP 旧路径也不是 Invoke

`JsSelectAccount` 注释写明：React 外层 div **需坐标点击**——`elementFromPoint` + `dispatchEvent(new MouseEvent('click', { clientX, clientY, … }))`，失败再 `top.click()`。  
那是 **DOM 合成带坐标的 MouseEvent**，不是 `InvokePattern`。日常正道不开 `--remote-debugging-port`，不能依赖这条。

---

## 3. 产品约束下的可选路径

| 路径 | 能否用于一键正道 | 结论 |
|---|---|---|
| UIA `InvokePattern` | 试过 | **否**（假成功） |
| `LegacyIAccessible.DoDefaultAction` | 可试一次 | 未 BIN 证成功；DefaultAction 常空 |
| `SetFocus` + Enter | 可试一次 | 未证 |
| `PostMessage` 假鼠标到 Chrome HWND | — | **禁止指望**（Chromium 吞掉） |
| BoundingRect + `SendInput` | **是** | **主路径**（已证） |
| CDP `Input.dispatchMouseEvent` / `JsSelectAccount` | 与「日常无调试口」冲突 | 仅旧路径/特例；不默认开 |

若将来必须「零光标」：需单独立项放开 CDP（与 SSO/日常同罐策略一并评审），**不是**再抠 UIA Invoke。

---

## 4. 实现（正道）

### 4.1 选卡（对齐 CDP `resolveCard`）

`Session::ClickAccountCardIndex`（`win_uia.cpp`）：

1. 全树找种子：Name 含 `@` / `default-user-avatar` / 常见邮箱域  
2. `ResolveAccountCard`：单 `@` 祖先里取紧卡；**禁止**爬到多邮箱列表壳 / 过大 Custom·Group  
3. 种子本身已是单卡尺寸（高约 40–160、宽约 100–640）则不再上溯  
4. 过大候选丢弃（高 >180 或宽 >680）  
5. 按邮箱去重、自上而下取 slot N  

### 4.2 激活（`ActivateAccountCard`）

**几何优先**（BIN 06:47）：

1. `ClickPointAt(card, xFrac, 0.5, useOfficialCp=false)` —— 忌官方 ClickablePoint 偏空（BIN 05:37）  
2. 落点变体：`0.50` → 重试 `0.22` / `0.72`  
3. 几何失败才兜底：Invoke → Legacy 默认动作 → 卡内可 Invoke 后代 → Focus+Enter  
4. `DescribeHitAt`：`ElementFromPoint` vs 所选卡 `CompareElements`，日志 `hit=same|t…#…±inv`（验「选错元素」假说；采证后可删）  
5. `ClickPointAt`：**点完 `SetCursorPos` 还原**，降低鼠标被接管体感  

### 4.3 WaitAcc 门禁与 cooldown（`gamapass_uia_login.cpp`）

| 机制 | 作用 | BIN 教训 |
|---|---|---|
| `selectUrlSeenAt` + `kAccSettleMs`（320ms） | 地址栏真到 `select-account` 后再首点 | 06:15/06:38：卡已进树但 URL 仍 `oauth2/authorize`，首点必废 |
| 计时器只记不清 | 避免 URL/标题抖动清零饿死首点 | 07:13/07:28：全程无 `click-account-card`，页面自跳才过 |
| `selectDomSeenAt` + `kAccStarveMs`（1.2s） | DOM 见选账号页最迟必点 | 同上 |
| 选出候选即记 `lastAccClickAt`（成功/失败皆然） | 禁止 40ms 狂点 | 07:49：诊断后缀把 `hitName` 撑过 junk 阈值 → 不记 cooldown → 狂点列表 |
| 账号卡路径**不再**用 `IsJunkClickName` 否决已发出的点击 | 卡已有尺寸/种子过滤 | 07:49 假「未解析出账号卡」 |
| `kAccRetryMs`（2.5s） | 仍停选账号页才重点 | — |

日志锚点：

```text
[gamapass-uia] 首点被门禁挡住：url=… urlAge=… domAge=…
[gamapass-uia] click-account-card|slotN|<名>|WxH|t…|vN|pt|hit=…|card（待确认跳转）
[gamapass-uia] click-account-card|fail|…（已记 cooldown）
[gamapass-uia] 首点未发出：选账号页未解析出账号卡…
```

### 4.4 与 GP / 昵称 / 繼續的分工

| 阶段 | API | 鼠标 |
|---|---|---|
| WaitGp | `ClickLargestExactName` / Invoke 优先；重试可 `forceMouse` | 兜底 |
| WaitAcc | `ClickAccountCardIndex` → 几何主路径 | **主** |
| WaitNick | Radio/ListItem/CheckBox 第 N 项 | 尽量 UIA |
| WaitContinue | 精确「繼續」Invoke 优先 | 兜底 |

---

## 5. 验收清单（BIN）

- [ ] 进程 mtime 为含本逻辑的 `bin/xcat.exe`（日志须含 `|t` / `|pt|hit=` 或 `|fail|`）  
- [ ] 选账号阶段**不会** 40ms 级狂点；两次点卡间隔 ≥ ~2.5s  
- [ ] 首点不落在 `oauth2/authorize` 在途页（先见 `SelectAccount` URL 或 starve 后再点）  
- [ ] 成功行类似：`…|345x68|t50000|v0|pt|hit=…|card` 后出现 `NickOrGame` / `已进入选昵称页`  
- [ ] GP / 繼續仍可为 `|uia`  
- [ ] 无 `/v1/refresh/token`、无清 Cookie、无改写 `prompt`

`hit=same` → 可结案「元素未选错，Invoke 语义不够」。  
`hit=t…+inv` 且异元素 → 再评估「改 Invoke 命中者」；在业界结论下预期仍可能假成功，几何仍为底线。

---

## 6. 相关文件

| 路径 | 角色 |
|---|---|
| `launcher/gamapass_uia_login.cpp` | 阶段机、门禁、cooldown、日志 |
| `launcher/win_uia.cpp` / `win_uia.h` | 选卡、激活、几何点击、光标还原 |
| `launcher/gamapass_cdp_login.cpp` | 旧 CDP `JsSelectAccount`（对照，非一键正道） |
| [`GAMA_PASS与注入闭环.md`](GAMA_PASS与注入闭环.md) | 整条换票/注入闭环 |
