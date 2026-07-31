# docs/features — 模块索引

> **当前目标**：新楓之谷：經典版（TW / beanfun · Gamania Galaxy）。**不是**枫星。
> 本文是 `docs/features/` 下设计文档的总索引。
> 逆向笔记见 [`Dumps/ANALYSIS_NOTES.md`](../../Dumps/ANALYSIS_NOTES.md)。
> 工程对照仓（仅复用设计/代码模式）：`xcat_for_fengxing/docs/features/`。

---

## 1. 运维与架构（ops/）

| 文档 | 主题 |
|------|------|
| [`ops/启动系统实现.md`](ops/启动系统实现.md) | Galaxy 换票、NGM deep-link、启动骨架（对照枫星注入器启动文档） |
| [`ops/架构总览.md`](ops/架构总览.md) | TWMS 分层 DAG；`xcat_app` 内嵌 WebView 换票 |
| [`ops/日志系统.md`](ops/日志系统.md) | 统一 `xcat_log`：launcher / inject / payload JSONL + GUI callback |

---

## 2. 协议与移动（protocol/）

| 文档 | 主题 |
|------|------|
| [`protocol/移动协议.md`](protocol/移动协议.md) | MovePath API、S→C opcode（`0x00D9` 等）、C→S Flush、飞天关系 |
| [`protocol/MoveElem字段.md`](protocol/MoveElem字段.md) | MoveElem / MovePathType / MovePathRect 布局；Encode wire 待采 |

工作草稿仍在 `Dumps/opcode_move_notes.md`、`Dumps/move_elem_notes.md`（以本目录正式文档为准）。

---

## 3. 有模块设计文档的 Feature

| 文档 | 主题 |
|------|------|
| [`fly/模块设计.md`](fly/模块设计.md) | F6 鼠标飞（数据面；**无** Flush E9）→ 已挂入 `xcat.dll`；面板 `[core] fly` |
| [`invuln/模块设计.md`](invuln/模块设计.md) | 无敌：`LocalUser+0x298` 硬直门 + 去闪；面板 `[core] invuln`；**无** inline |
| [`kick_sniff/断线错误码.md`](kick_sniff/断线错误码.md) | Session `_pendingErrorCode` / 断线边沿；**TW≠CMS 偏移**；`kick.log` |
| [`titlebar/模块设计.md`](titlebar/模块设计.md) | Win32 标题栏 vitals + 金/经每分；锚点 `UIStatusBar→CharacterStat`（DumpRestoredData） |

---

## 4. 安全与完整性（security/）

| 文档 | 主题 |
|------|------|
| [`security/ClientFileCRC.md`](security/ClientFileCRC.md) | 登录阶段客户端文件 CRC：**安装树完整性校验**，非 AppData 外挂扫描；附 154 条实抓清单 |
| [`security/MscSecurity能力面.md`](security/MscSecurity能力面.md) | RawInput 反宏、窗口子类化、DriveType SSD IOCTL、MultiClient 单实例；与 BlackCat 边界 |
| [`security/GRAP与枫星对齐.md`](security/GRAP与枫星对齐.md) | 同 MD5 套件；LoadLibrary 弱；**禁止 INLINE HOOK**；勿扩成「无 AC」 |
| [`security/客户端Hack标志与服端推断.md`](security/客户端Hack标志与服端推断.md) | Float/AB（**VecCtrlMob**）+ `ClientHacksType` → 服端举报链；**≠** 玩家飞天校验 |

原始数据：`Dumps/client_file_crc_paths.{json,tsv}` · 采证 DLL：`Dumps/runtime/out_bin/ClientFileCrcTrace.dll`

---

## 5. 相关代码入口

| 路径 | 说明 |
|------|------|
| `xcat_app/` | 产品 ImGui 壳 → `bin/xcat.exe` |
| `launcher/msc_launch.{h,cpp}` | NGM 启动骨架 |
| `launcher/msc_webview_login.*` | WebView 一键换票会话（链进 `xcat.exe`） |
| `Dumps/` | dump.cs / opcode / Msc.Security 笔记 |
| `x/features/fly/` | F6 feature；主产物 `bin/XCat_data/xcat.dll` |
| `x/features/invuln/` | 无敌（硬直门 + 去闪）；见 [`invuln/模块设计.md`](invuln/模块设计.md) |
| `x/ipc/payload_control.*` | 面板 ↔ payload：`user.ini [core]` |
| `x/features/kick_sniff/` | 断线 / pendingError 轮询；见 [`kick_sniff/断线错误码.md`](kick_sniff/断线错误码.md) |
| `x/features/titlebar/` | 标题栏 vitals + 收益；见 [`titlebar/模块设计.md`](titlebar/模块设计.md) |
| `DumpRestoredData/` | dump.cs 符号恢复分档（titlebar 偏移锚点） |
