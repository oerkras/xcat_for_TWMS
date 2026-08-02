# NGS 单文件 CRC 与补丁（服务控制面）

> 产品 = 经典版 TWMS。  
> 同行约束：**只有一份** `NGS.EXE.CRC`，目标二进制 = `NGService.exe`（不是 grap-core 多文件表）。  
> 真目标 = 在游戏进程改 **GameAssembly 内存**；本文件描述前置的单文件 CRC / 服务侧 patch 门禁。

---

## 1. 结论

| 判断 | 说明 |
|------|------|
| CRC 锚只有 **一个文件** | `NGService.exe`（同行名 `NGS.EXE`） |
| MemoryCrc 不在该服务里 | 扫描在游戏进程 **grap-core**；服务是控制面（`GamePid` / BlackCipher / Poco IPC） |
| 默认只改 ProgramData 副本 | `C:\ProgramData\Nexon\NGS\NGService.exe`；游戏树 `…\grap\NGService.exe` **保持干净** → 登录 ClientFileCRC 通常不过 150 |
| GA 写要探针证明 | 进程内可回滚 GA `.text` 探测写；**空补丁 / 服务不常驻时可先探针**；仍死再升级 grap-core（**不**预建多文件 CRC） |
| 填充位点探针（2026-08-01） | **PASS**：脏 10min 后还原仍存活（见 [`GA文本探针.md`](GA文本探针.md)）；**≠** 业务 E9 放行 |

---

## 2. 落盘约定

| 路径 | 用途 |
|------|------|
| `bin/XCat_data/state/ngs_fingerprint.tsv` | 双路径指纹 baseline（版本门禁） |
| `bin/XCat_data/state/ngs_status.txt` | 最近一次指纹检测 |
| `bin/XCat_data/state/ngs_official_crc.tsv` | **单行**官方 CRC 真源 |
| `bin/XCat_data/state/NGS.EXE.CRC` | 同行旁路名兼容（默认 4 字节 LE zlib CRC32） |
| `bin/XCat_data/state/ngs_patch.tsv` | 服务映像补丁表（RVA / before / after）；空表 = 不写盘 |
| `bin/XCat_data/state/ngs_backup/` | apply 前备份的官方 `NGService.exe` |

脚本：

- `scripts/ngs_fingerprint.py` — 比对 / `--write` baseline；`--freeze` 写官方 CRC + `NGS.EXE.CRC`
- `scripts/ngs_patch_apply.py` — fingerprint 门禁后对 **ProgramData** 副本 apply/restore（默认 dry-run）
- `Dumps/runtime/_ngs_unpack_dump.py` — 调试器脱壳 dump（`.winlice` VirtualProtect 后）

---

## 3. 与 §4.1 的关系

见 [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) §4.1 **有条件例外**：

- 默认仍禁止游戏进程内对 GA / ntdll / grap-core 的 inline。
- 在「单文件指纹匹配 + 服务侧补丁已按表加载 + GA 探针通过」后，才允许为业务改 GA 内存。  
  - **纠偏（2026-08-01）**：`ngs_patch.tsv` 空或 NGS 服务不常驻时，允许**先**跑进程内 GA 探针拿杀证（见 [`GA文本探针.md`](GA文本探针.md)）；有补丁行后再 apply 做对照。
- 未通过探针前，**禁止**恢复业务 E9。
- 填充页探针 PASS **不能**代替业务位点试验。

---

## 4. 操作顺序

```text
python scripts/ngs_fingerprint.py --write --freeze
python Dumps/runtime/_ngs_unpack_dump.py   # 产出 NGService.unpacked.bin
# 在 dump 上 RE → 填 ngs_patch.tsv
python scripts/ngs_patch_apply.py --dry-run
python scripts/ngs_patch_apply.py --apply
# 启动游戏 → GA 探测写
python scripts/ngs_patch_apply.py --restore
```

---

## 5. 相关文档

- [`ClientFileCRC.md`](ClientFileCRC.md) — 登录 22/23；清单含游戏树 `NGService.exe`
- [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) — MemoryCrc；§4.1 例外
- [`GA文本探针.md`](GA文本探针.md) — 2026-08-01 进程内 GA `.text` 探针 PASS / 边界
- 对照仓 `xcat_for_fengxing/docs/research/grap/grap_core_ida.md` §5 — MemoryCrc 细节（地址需重算）
- 挖掘笔记：`Dumps/runtime/_ngs_dig/CANDIDATES.md` · `Dumps/runtime/_grap_dig/`

---

## 6. 挖掘摘要（2026-08-01）

- 静态：WinLicense；业务串无磁盘 xref。
- **已直接脱壳**：`NGService.unpacked.bin`（SizeOfImage `0xA33000`）；`.winlice` 首次 RWX `VirtualProtect` 后整镜像。
- dump 上 `.text` 出现大量 `FF15`；`GamePid`/`WinVerifyTrust` imm xref 仍为 0；`.winlice` 露出 `OpenProcess` 等 API 名表。
- 下一步：在 dump 上跟 IAT / `OpenProcess` 调用链，再填 `ngs_patch.tsv`。
- **GA 探针（并行）**：空表未 apply 时已做进程内填充位点试验 → **PASS**；详见 [`GA文本探针.md`](GA文本探针.md)。
- mempe 间接 IAT 扫描（OpenProcess / WinVerifyTrust）：imm/`FF15`/近窗 `FF Dx` 均为 0 → **仍不写** `ngs_patch.tsv`（`Dumps/runtime/_ngs_dig/IAT_INDIRECT_SCAN.md`）。
