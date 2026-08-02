# XCatKrw — 本仓自研内核读写工具

> 路径：`tools/krw/`  
> **对照**：桌面 `KraSlayer/Tools/RW_Driver` 只读参考，**不修改、不拷贝进仓**。  
> **复刻范围**：**隐蔽主路径 = ETW-only**（物理 RW + `NtCreateFile` 隐蔽 IPC + APC Inject + Compat ABI）。**无 Device / 无 IOCTL 回退**。**不做 VMP**。

## 定位

| | |
|--|--|
| 用途 | 本机跨进程 R/W / 注入研究 / 采证 |
| 默认产品路径 | **不**链入 `xcat.exe` / payload |
| 与 MemoryCrc | 改页内容仍会被 grap RPM 读到；**不是**完整性解法 |
| 与 GA `.text` 探针 | 探针在 payload 进程内完成；不依赖本驱动。结论见 [`GA文本探针.md`](GA文本探针.md) |
| 与 BlackCat | 有 APC 注入能力（仅 lab 验收）；默认不绑游戏进程 |

## 与对照项目

| RW_Driver（对照） | XCatKrw（本仓 · 隐蔽） |
|--|--|
| ETW 钩 `NtCreateFile` 传参 | **唯一 IPC**；`DriverEntry` fail-closed |
| `Anti4heatExpert` 物理页/PTE | covert `RealCr3` / phys_pte |
| VMP 壳 | **无**（`vmp_stub` 空宏） |
| APC `Inject` | `Type_Inject` |
| Device 无 | **对齐**：不创建 `\Device\XCatKrw` |
| Client SCM | **对齐**：`SVC_` + UUID 随机服务名 |

## 入口

- 说明：`tools/krw/README.md`
- Compat key：`Init(0x7654321)`
- 构建：`tools/krw/build_driver.bat` / `tools/krw/xcat_krw.sln`
- 研究用手搓固定服务（暴露固定名，非隐蔽路径）：`scripts/load.ps1` / `unload.ps1`
- 隐蔽加载：compat `Init` 按需 `CreateService(SVC_<uuid>)`
- 交叉签名：仓库根目录 `sign_krw.bat`（自动找 `DriverTools\GigaDevice*.pfx` + 提权回拨时钟）；或 `scripts/sign_cross.ps1 -BackdateForExpired -SkipTimestamp`
- 冒烟：`krw_compat_smoke.exe` / `krw_smoke.exe`（均走 ETW）

## 能力与验收

| 项 | 命令 / 标准 |
|----|-------------|
| Compat Init/Read/Write | `bin\krw\krw_compat_smoke.exe` → OK（须 ETW 钩生效） |
| Inject（lab only） | `krw_inject_target` + `krw_compat_smoke --inject ...` → `%TEMP%\xcat_krw_inject.ok` |
| 对象管理器 | 加载后 **无** `\Device\XCatKrw` |
| 对照仓 | `KraSlayer/Tools/RW_Driver` **零 diff** |

> **安全提示**：ETW/PMC 钩子系统调用，错误实现易蓝屏。请在隔离调试机验证；勿在生产机反复 load/unload 试错。

## 明确不做

- 修改桌面 `RW_Driver`
- Vendoring VMP Ultimate / SDK 二进制
- 默认绑入 `xcat.exe` 启动链
- 把本驱动表述为 NGS/MemoryCrc bypass
- Device/IOCTL 双通道回退
