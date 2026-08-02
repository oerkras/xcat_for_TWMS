# tools/krw — XCat Kernel RW（本仓自研）

> 产品 = 经典版 TWMS。  
> **独立研究工具**，不链进 `xcat.exe` / `xcat.dll` 默认注入路径。  
> **不修改、不 vendoring** 桌面 `KraSlayer/Tools/RW_Driver`。  
> **隐蔽主路径 = ETW-only**（无 Device / 无 IOCTL 回退 / 无 VMP）。

## 目标

| 做 | 不做 |
|----|------|
| ETW→`NtCreateFile` 隐蔽 IPC（唯一通道） | `\Device\XCatKrw` / IOCTL |
| Phys/PTE 跨进程 R/W（covert） | VMP 壳 / 商业 SDK |
| APC DLL Inject（covert `Type_Inject`） | 默认绑游戏进程做 Inject |
| Compat：`Init/Read/Write/Inject` + UUID SCM | 修改对照仓 `RW_Driver` |

`DriverEntry`：`EtwHookManager::init` 失败则 **直接返回失败**（对齐 RW）。

## 目录

```
tools/krw/
  include/xcat_krw_abi.h      # 历史 IOCTL ABI 废案（驱动不再创建设备）
  driver/                     # ETW-only + inject + phys_pte
  client/                     # xcat_krw_compat（无 IOCTL client）
  test/                       # smoke / compat / inject lab / hp_watch
  scripts/load.ps1 | unload.ps1 | sign_cross.ps1
```

## 构建

```bat
tools\krw\build_driver.bat
```

签名（过期交叉证路径，按需）：

```bat
sign_krw.bat
```

或：

```powershell
.\tools\krw\scripts\sign_cross.ps1 -BackdateForExpired -SkipTimestamp
```

Lab 手搓固定服务名（非隐蔽；需管理员）：

```bat
tools\krw\scripts\load.bat
tools\krw\scripts\unload.bat
tools\krw\scripts\unload.bat /image
```

`unload.bat /image` 会顺带清掉 ImagePath 含 `xcat_krw.sys` 的 `SVC_<uuid>` 服务。

> **勿在本机反复加载调试**：ETW/PMC 钩子易蓝屏。优先隔离 VM；手搓固定服务见 `load.bat`（非隐蔽）。

## 隐蔽使用

```text
bin\krw\krw_compat_smoke.exe
bin\krw\krw_smoke.exe
bin\krw\krw_smoke.exe --phys
```

无 XCat、测驱动读/写游戏进程（内存一律走 KRW；不用 SNAPMODULE）：

```text
bin\krw\krw_game_read.exe
bin\krw\krw_game_write.exe
bin\krw\krw_game_read.exe --phys
bin\krw\krw_game_write.exe --phys
bin\krw\krw_game_vitals.exe
bin\krw\krw_game_vitals.exe --watch
```

`Init(0x7654321)` 失败时会按需 `CreateService(SVC_<uuid>)` 加载同目录 `xcat_krw.sys`，**不再**回退 IOCTL。

Lab 注入（勿对游戏进程）：

```text
bin\krw\krw_inject_target.exe
bin\krw\krw_compat_smoke.exe --inject <pid> <abs\bin\krw\krw_inject_payload.dll>
```

血量 watch：

```text
bin\krw\krw_hp_watch.exe --auto
```

## 要点

- Compat key：`Init(0x7654321)`（对齐对照 Client）
- SCM：随机 `SVC_<uuid>`（对齐对照）；`load.ps1` 固定名 `XCatKrw` 仅研究用手搓
- 卸载随机服务：`unload.ps1` 可停固定名；随机名需按镜像路径在服务列表中查找后 `sc stop/delete`

详表：`docs/features/security/KRW驱动.md`。
