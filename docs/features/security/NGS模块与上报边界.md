# NGS / GRAP / BlackCat — 模块与内存上报边界

> 回答：「会不会收集内存中的 DLL 上报？」  
> 真源：枫星 SSOT `xcat_for_fengxing/docs/research/grap/` + 本仓同 MD5 对齐笔记。  
> 日期：2026-08-13

---

## 1. 一句话

| 说法 | 成立？ |
|------|--------|
| 会扫内存里的模块/映像（RPM、hash、SecureDLL） | ✅ |
| 检测命中后会经 communicator **HTTPS 上报** | ✅（wire 密文已抓到） |
| 会把进程 **PEB/LDR 全部 DLL 整表**打包上传 | ❌ **未证实**；现有证据不支持此模型 |
| BlackCat 负责「收集内存 DLL 清单」 | ❌；已确认能力偏 **进程控制/映像路径查询** |

口语正确版：

> **会扫、会报异常；不是（至少未被证明是）「内存 DLL 普查遥测」。**

---

## 2. 家族分工

```
grap-core64.aes     → 扫描 / 组 DetectLog（MemoryCrc、MemorySharedPage、SecureDLL…）
grap-communicator   → HTTPS 上报（x-csauth / x-init / x-config）
BlackCat64.sys      → 内核 syscall 旁路、进程/线程 notify、cmd 查询
NGService / NGS-X   → 控制面/服务；MemoryCrc 不在服务进程里扫游戏
```

同套件 MD5 见 [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md)。

---

## 3. 各层证据

### 3.1 grap-core（扫）

| 组件 | 行为 | 与「DLL 列表上报」的关系 |
|------|------|-------------------------|
| **MemoryCrc** | `ReadProcessMemory` 块 `0xE8` → hash；失败码 29；RTTI `MemoryCrcDetected` | 完整性命中 → 进 DetectLog，**不是**枚举模块名表 |
| **MemorySharedPage** | RPM `0x278` + PE 映像 hash；失败码 79 | 偏匿名/映射页；同样是命中上报 |
| **SecureDLL_Scanner** | `GetModuleHandleA` / `LoadLibraryA`；名串加密 | 更像查**已知名单/安全组件**，**≠**「枚举 PEB 后上报所有新 DLL」 |
| **FileScan** | 独立线程，与内存扫描并行 | 文件面，非内存模块表 |

实机纠偏（枫星 §13.3.1）：正规 `LoadLibraryW` 进 PEB **不是**最强杀因 → 侧面否定「看见任意新 DLL 就整表上报/瞬杀」的强模型。

### 3.2 grap-communicator（报）

| 项 | 状态 |
|----|------|
| 管线 | `DetectLogBuilder` → JSON → ApiBroker → HTTPS（libcurl/OpenSSL） |
| 域名 | `x-init.ngs.nexon.com` · `x-csauth.ngs.nexon.com` · `x-config.ngs.nexon.com` |
| 大包 | `POST …/v2`（CsauthReport）≈36KB 首报 / ≈13KB 周期 |
| Wire | `AE:G` / `CV:v1` + `Content-Type: application/json`；body = **Base64 密文** |
| 明文 schema | **未打开**（须逆向加密层）；故 **不能**声称内含 `modules[]` |

检测证据（如 `MemoryCrcDetected`）嵌入上报体是高置信推断；**全量模块清单字段无证据**。

### 3.3 BlackCat（内核）

| 项 | 结论 |
|----|------|
| cmd 16 | `ZwQueryInformationProcess` **class 27**（`ProcessImageFileName`）→ **主进程映像路径** |
| 完整 VAD / 已加载 DLL 枚举 | **未证实**（笔记显式保留为开放项） |
| 主威胁 | 线程/进程 notify 快杀、syscall 旁路 Ring3 hook |

→ BlackCat **不是**用户态「DLL 收集器」。

---

## 4. 工程含义（经典版）

```
正规 LoadLibrary 业务 DLL
  → SecureDLL「可能看见」
  → 实证：非「一见即杀 / 整表上报」主路径

匿名页 / 手工映射 / 改 .text
  → MemoryCrc / MemorySharedPage 更易命中
  → 更可能 DetectLog → HTTPS，或本地快杀
```

策略仍见 [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md)：默认正规加载；禁止 INLINE HOOK；盯 Msc 反宏与 ClientFileCRC。

---

## 5. 开放项（要钉死 `modules[]` 才做）

1. 复抓 `/v2` 完整 body（枫星曾有 `captures/CsauthReport__v2_*.bin`；本机未必仍在）  
2. 逆向 communicator 加密层（`XorStreamEncryptor` / SecureStr / OpenSSL 包装）  
3. 解密后对照 JSON 是否含模块路径数组、仅含检测码、或仅含 hash blob  

在完成前：**禁止**把「全量 DLL 上报」写进威胁模型。

---

## 6. 证据指针

| 材料 | 路径 |
|------|------|
| core 扫描 / SecureDLL | `xcat_for_fengxing/docs/research/grap/grap_core_ida.md` §5 · §8 |
| HTTPS / DetectLog | `…/grap_communicator_ida.md` §5 · §10 |
| BlackCat cmd 16 | `…/grap_blackcat_ida.md` §4.1 |
| 动调抓包 | `…/grap_dynamic_trace.md`（`/v2` 尺寸与导出脚本） |
| 本仓对齐 | [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) · [`MemoryCrc派发与节奏.md`](MemoryCrc派发与节奏.md) |
