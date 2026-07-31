# GRAP / NGS-X — 与枫星实测对齐

> 经典版 grap-core / BlackCat **与枫星同 MD5**，老项目 SSOT 可直接复用。  
> 本文只定调**工程威胁模型**，不重复 IDA 全文。  
> 权威细节：`xcat_for_fengxing/docs/research/grap/`（`grap_core_ida.md` · `grap_blackcat_ida.md` · `grap_dynamic_trace.md` · `grap_communicator_ida.md`）  
> 本仓二进制对照：`Dumps/ANALYSIS_NOTES.md` · 日期：2026-07-30

---

## 1. 一句话定调

| 说法 | 是否成立 |
|------|----------|
| 「GRAP 完全没有反作弊能力」 | ❌ |
| 「GRAP 对正规 `LoadLibraryW` + 行为克制的 payload **很弱**」 | ✅ 与枫星/创世实机一致 |
| 「经典版因此可以无视所有安全」 | ❌ 还有 **Msc 反宏 / ClientFileCRC / 行为侧** |

口语「GRAP 很弱」的准确工程含义：

> **拦不住**正规 loader 加载的业务 DLL + 游戏内逻辑；  
> **拦得住**（或至少很痛）匿名 mmap、手工映射、入口落在私有可执行页、外部调试器乱写、Init 窗脏内存。

---

## 2. 二进制一致（可复用前提）

| 文件 | 经典版 MD5 | 枫星笔记 |
|------|------------|----------|
| `grap-core64.aes` | `cf0439c3474ad5c8a9b1bfbeab29c65e` | 同（`grap_core_ida.md`） |
| `BlackCat64.sys` | `033723ca315cb04bd82005756bb8373c` | 同（`grap_blackcat_ida.md`） |

→ IDA 能力面、cmd 5/16、MemoryCrc / MemorySharedPage、DRBG 触发扫描等结论**地址级需按本机 ASLR 重算**，语义可搬。

命名易混：经典版磁盘 `grap64.dll` 多为 **interface**；运行时从 `.aes` 加载的 core 才是检测引擎（枫星文档里常把运行时 core 也叫 `grap64.dll`）。

---

## 3. 枫星威胁矩阵（实机纠偏后）

来源：`grap_core_ida.md` §13.3.1（2026-06-20）等。

| 手段 | 结果 | 说明 |
|------|------|------|
| Classic **`LoadLibraryW`** 注入固定 payload | 创世 **300s PASS**；产品收敛为 **LoadLibrary only** | PEB 可见 **不是**最强触发项 |
| Manual Map + `NtCreateThreadEx` | 1–2s 进程消失 | BlackCat **线程 notify** 快杀画像 |
| 晚 attach / 官方 Frida | 写入封锁 / 拒 agent | 外部动调窗口极窄 |
| 匿名可执行页 / 擦 PE 头 / 非 LDR 模块 | 高危 | `MemorySharedPage` / `MemoryCrc` 更易命中 |
| **INLINE HOOK（E9/FF25 改 `.text`）** | **禁止 · 高危** | 见 §4.1；含改 `GameAssembly` / `ntdll` / 任意模块代码段 |
| 游戏 Lua HWBP / 60s script hash | **与 GRAP 无关** | 那是 `SecurityService`；HWBP **躲不了** GRAP 云上报 |

**为何 LoadLibrary「弱杀」合理**：远程线程入口在 `kernel32!LoadLibraryW`，DLL 为正常 `SEC_IMAGE`/LDR；泛杀会误伤输入法、overlay、录屏、杀软。匿名映像 + 私有可执行入口误报成本低，才是优先打击面。

---

## 4. GRAP 实际在干什么（能力有，但面窄）

静态已证实存在（详见枫星 SSOT）：

- 复合检测器：MemoryCrc、MemorySharedPage、SecureDLL 等  
- 扫描节奏：挂 OpenSSL **DRBG** 回调 + 检测工作线程（**非**固定墙钟 60s）  
- 上报：core 组包 → communicator **HTTPS**（独立于游戏 Lua 上行）  
- 内核：`\Device\BlackCat1`（经典版句柄扫描实锤）；cmd 5 Init、cmd 16 进程映像路径等  

经典版本仓侧证：ScanOnly / CRC 采证 DLL 以正规注入存活、进程可见 `\Device\BlackCat1`——与「同包 + LoadLibrary 友好」模型合拍。

---

## 4.1 硬禁令 · 禁止 INLINE HOOK（改 `.text`）

> **GRAP / BlackCat 不允许对本进程任意模块做 INLINE HOOK。**  
> 本仓功能、采证、运维脚本一律遵守；与「LoadLibrary 弱」**不矛盾**——弱的是正规映像加载，不是允许改代码段。

### 禁止的做法（非穷尽）

| 形态 | 典型实现 | 为何禁 |
|------|----------|--------|
| 入口 `E9` / `FF25` 跳板 | 偷 prologue + trampoline | 改 `.text` → MemoryCrc / 完整性画像 |
| 任意 RVA 打补丁 | `VirtualProtect` + 写 GA/ntdll/… | 同上；ntdll inline 已实测会杀进程 |
| 热补丁 / mid-function hook | detours / minhook / 自研 E9 | 同属改可执行页 |
| 对已 hook 的二次 inline | 「只 hook 游戏不管 ntdll」 | **不豁免**：GA `.text` 同样在扫描面内 |

### 允许的替代（本仓默认）

| 手段 | 说明 |
|------|------|
| 正规 `LoadLibraryW` 业务 DLL | 字段读写、状态机、协议侧逻辑 |
| 读内存 / 日志采证（不改 `.text`） | 观察 Flush 结果、CRC 清单等 |
| 游戏已有回调 / 托管字段 / 自然 Flush 路径 | 用 `_forcedFlush` 等**数据面**触发，而非 hook `Flush` 入口 |
| IAT/导出表改写（若必须） | **仍慎用**；优先数据面。对 ntdll 的 IAT 往往打空，且不等于「可以 inline」 |

### 本仓状态（INLINE HOOK）

- ~~`x/features/fly/fly_flush_hook.cpp` E9 `MovePath.Flush`~~ → **2026-07-30 已拆除**（`TwmsFly` v1.6.4+ 仅数据面；历史源码 `fly_flush_hook.cpp.archived`）。  
- 采证勿再恢复 E9；见 [`../fly/模块设计.md`](../fly/模块设计.md)。

---

## 5. 经典版相对枫星的差异（真正要盯的）

| 层 | 枫星 | 经典版 | 对工程的含义 |
|----|------|--------|--------------|
| GRAP 套件 | 上表同 MD5 | 同左 | 注入策略可沿用 LoadLibrary 经验；**同样禁 inline** |
| 游戏自研 | Lua `SecurityService` + script hash | **`Msc.Security`**（反宏 RawInput、CRC、MultiClient…） | **不能**说「只有弱 GRAP」 |
| 脚本面 | 有 XLua | **无** | 勿搬 HWBP 拦 Lua |
| 文件完整性 | （另论） | **ClientFileCRC** 登录 22/23，实抓 154 路径 | 改 GA/主程序/grap 文件有登录失败风险 |

分层图：

```
GRAP/BlackCat     → LoadLibrary 弱；匿名映像/乱写敏感；INLINE HOOK 一律禁止
Msc KeyMacro      → 反键盘宏/异常输入（挂机连键风险）
ClientFileCRC     → 安装树完整性（非扫挂）
MultiClient       → 弱防多开
```

专文：[`MscSecurity能力面.md`](MscSecurity能力面.md) · [`ClientFileCRC.md`](ClientFileCRC.md)

---

## 6. 工程建议（本仓）

1. 注入默认按枫星结论走 **正规 `LoadLibraryW`**；勿为「隐身」上 manual-map。  
2. **禁止 INLINE HOOK（改任意模块 `.text`）**——含 `GameAssembly` / `ntdll` / grap 组件；详见 §4.1。本仓 v4 对 ntdll inline 已踩过杀进程。  
3. 勿把「GRAP 弱」扩成「无 AC」：功能开发仍要避开 **反宏误杀** 与 **改文件触发 CRC**。  
4. 需要动调 GRAP 本体时，优先复用枫星 `grap_dynamic_trace.md`（Rusda / spawn），Frida attach 预期被拒。  
5. 经典版若未来复测 LoadLibrary 存活窗口，把结果补回本节（目前以同包 + 采证 DLL 存活为旁证）。

---

## 7. 证据指针

| 材料 | 路径 |
|------|------|
| 枫星 GRAP SSOT | `../xcat_for_fengxing/docs/research/grap/` |
| 本仓 hash / 句柄 | `Dumps/ANALYSIS_NOTES.md` · `Dumps/msc_security_ioctl_notes.md` |
| CRC 实抓 | `Dumps/client_file_crc_paths.{json,tsv}` |
