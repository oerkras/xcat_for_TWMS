# ClientFileCRC — 客户端文件完整性校验

> 结论先行：**这是登录阶段的服务端下发清单 → 客户端算 CRC 回传**，不是全盘/AppData 外挂扫描。  
> 采证日期：2026-07-30 · TW《新楓之谷：經典版》

---

## 1. 结论

| 判断 | 证据 |
|------|------|
| **只做安装目录相对路径的完整性校验** | 实抓 `_fileList` 154 条，全部相对游戏根；无 `%AppData%`、无临时目录、无任意盘符漫游 |
| **清单由服务端下发** | `ServerPacket ClientFileCRCFileList = 22` → `ClientFileCRC.OnFileList(InPacket)` |
| **客户端算 CRC 后回传** | `ComputeAllCrc*` → `SendAnswer`；`ClientFileCRCResult = 23` |
| **主要发生在登录阶段** | opcode 22 **不在**进图后的 `opcode_handler_map.tsv`；进图后对象可仍停留在 `CrcState.Verified` |
| **与 GRAP/BlackCat「扫外挂」是另一条线** | 清单里反倒包含 `grap/*`、`BlackCat64.sys` 自身（防篡改反作弊组件） |
| **失败可挡登录** | `SceneLogin.LoginResponseCode.ClientFileCRCFailed = 150` |

非目标（本机制不做）：进程枚举、窗口标题扫外挂、用户目录漫游、任意路径 CreateFile 猎杀。

---

## 2. 类型与流程（代码真源）

混淆类：`a45ed966…`（CMS 还原名 **`ClientFileCRC`**，`Singleton<ClientFileCRC>`）。

| 字段 | 偏移 | 含义 |
|------|------|------|
| `_fileList` | 0x10 | 当前校验路径列表 |
| `_pendingFileList` | 0x18 | 待切换清单 |
| `_fileIndexMap` / `_pendingFileIndexMap` | 0x20 / 0x28 | 路径 → short 索引 |
| `_answer` | 0x30 | short → CRC `uint` |
| `_stateRaw` | 0x50 | `CrcState` |
| `_lockedFiles` | 0x58 | `LockFiles` 打开的 `FileStream` |

`CrcState`：`None → Computing → PendingSend → WaitingServer → Verified / Failed`（另有 Cancel 态）。

主路径：

```
S→C opcode 22 (ClientFileCRCFileList)
  → OnFileList
  → LockFiles
  → ComputeAllCrcSingle / ComputeAllCrcParallel
  → SendAnswer
  → S→C opcode 23 (ClientFileCRCResult) / 登录码 150 失败
```

相关 LiveValue：`ClientFileCRC_ForceOff`、`ClientFileCRC_SkipWithAID`、`ClientFileCRC_SendTableBlock`（可跳过/关闭，需结合运营配置理解「为何某号未下发」）。

### TW RVA（GameAssembly.dll）

| 符号 | RVA |
|------|-----|
| `OnFileList` | `0x3C12E50` |
| SceneLogin 相关包处理（CFF 壳，勿当唯一跳板乱钩） | `0xC13E60` |

CMS CW dump 同名方法 RVA 不同（`0x2A908D0`），对照结构可复用，**地址不可照搬**。

---

## 3. 实抓清单（2026-07-30）

| 项 | 值 |
|----|----|
| 进程 | `Maplestory_Classic` pid=2616 |
| 采证 DLL | `Dumps/runtime/out_bin/ClientFileCrcTrace.dll` **v7** |
| 方式 | 残留堆扫描 `ClientFileCRC` 对象（`state=4` Verified）；非 OnFileList 钩子命中 |
| 条数 | **154** |
| JSON | [`Dumps/client_file_crc_paths.json`](../../../Dumps/client_file_crc_paths.json) |
| 分类 TSV | [`Dumps/client_file_crc_paths.tsv`](../../../Dumps/client_file_crc_paths.tsv) |
| 运行日志 | `Dumps/runtime/client_file_crc_trace.log`（`11:02:39 RESIDUAL hit … files=154`） |

### 分类统计

| category | 数量 | 说明 |
|----------|------|------|
| `vuplex_webview` | 72 | Vuplex/Chromium 本地化与渲染资源 |
| `streamingassets_aa` | 35 | Addressables catalog / `.bundle` |
| `unity_data` | 22 | assets / level / globalgamemanagers / resS 等 |
| `install_root` | 7 | 根目录 exe/dll（含 `.bak`） |
| `grap_anticheat` | 6 | GRAP / BlackCat 组件 |
| `il2cpp_data` | 6 | metadata 与 resources.dat |
| `plugins_native` | 6 | Firebase / CrashReporter / GPKit / Burst 等 |

### 高信号路径（完整性关注点）

```
GameAssembly.dll
GameAssembly.dll.bak
Maplestory_Classic.exe
Maplestory_Classic.exe.bak
UnityPlayer.dll
UnityCrashHandler64.exe
baselib.dll
Maplestory_Classic_Data/Plugins/x86_64/grap/BlackCat64.sys
Maplestory_Classic_Data/Plugins/x86_64/grap/grap-core64.aes
Maplestory_Classic_Data/Plugins/x86_64/grap/grap-communicator64.aes
Maplestory_Classic_Data/Plugins/x86_64/grap/grap-updater.aes
Maplestory_Classic_Data/Plugins/x86_64/grap/NGService.exe
Maplestory_Classic_Data/Plugins/x86_64/grap64.dll
Maplestory_Classic_Data/Plugins/x86_64/GPKitClt64.dll
Maplestory_Classic_Data/il2cpp_data/Metadata/global-metadata.dat
```

完整 154 条见 JSON/TSV，此处不重复粘贴。

---

## 4. 与「扫外挂」的边界

| 机制 | 扫什么 | 谁驱动 |
|------|--------|--------|
| **ClientFileCRC** | 服务端点名的安装树文件 CRC | 登录包 22/23 |
| **GRAP / NGS-X / BlackCat** | 内核/用户态反作弊（另线） | grap 套件 |
| **Msc.Security 其它** | RawInput / 窗口钩子 / 部分 IOCTL 等 | 游戏自研层 |

历史误判纠正：`IOCTL 0x2D1400` 是 Windows `IOCTL_STORAGE_QUERY_PROPERTY`（SSD seek-penalty 探测），**不是** BlackCat 自研 IOCTL；勿与 ClientFileCRC 绑死。详见 `Dumps/msc_security_ioctl_notes.md` / `ANALYSIS_NOTES.md`。

---

## 5. 采证工具（可复现）

| 产物 | 用途 |
|------|------|
| `Dumps/runtime/out_bin/ClientFileCrcTrace.dll`（v7） | 注入采证；beacon `CRC-TRACE v7`；三声短 beep；登录全程重扫 + 残留 `_fileList` 轮询 |
| `Dumps/client_file_crc_frida.js` | Frida 挂 `OnFileList` / `ReadString`（GRAP 可能拒 attach） |

注意：

- **不要**全内存改写 `0xC13E60`（CFF 公共壳 → v5 曾狂打近 2 万次后抛异常）。
- opcode 22 只在登录窗口出现；进图后再钩 `OnFileList` 通常打空，应依赖残留对象或重登。
- v1–v6 仅作历史对照，正式用 **v7+**。

---

## 6. 对项目的含义

1. 改 `GameAssembly.dll` / 主程序 / 关键插件 / grap 文件 → 可能触发 CRC 失败（登录码 150），与「会不会被 GRAP 扫到」是不同问题。  
2. 清单含 `.bak`：磁盘上若保留官方备份文件，也会被纳入校验集合。  
3. 业务功能（飞天、移动协议等）不依赖绕过本机制；本笔记只定界 **Msc.Security 文件面在校什么**。
