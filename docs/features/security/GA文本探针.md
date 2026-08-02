# GA `.text` 探针结论（经典版）

> 产品 = TWMS 经典版；枫星 = 对照来源（同 MD5 grap-core）。  
> 日期：2026-08-01。  
> 实现：`x/features/ga_text_probe/`（默认 **OFF**）。  
> 采证原稿：`Dumps/runtime/_grap_dig/GA_TEXT_PROBE.md` · `GA_TEXT_PROBE_RUN.md` · `MEMORYCRC_STATIC.md`。

---

## 1. 一句话

**本机构建下：进程内可回滚地改 `GameAssembly.dll` `.text` 填充页，脏窗 10 分钟后还原，未被秒杀；不等于业务 E9 已放行。**

---

## 2. 实机结论表

| 判断 | 结果 | 证据 |
|------|------|------|
| 改 GA `.text` 是否「立刻必死」 | **否**（填充位点） | Run B：`DIRTY` 12:40:31 → `RESTORE`/`PASS` 12:50:31；会话继续 ≥16 分钟仍 Connected |
| 是否依赖 NGS ProgramData patch | **否（本轮）** | `ngs_patch.tsv` 空；NGS 服务常 `STOPPED`/`0xe5000100`；探针仍 PASS |
| 是否已做 MemoryCrc 灭火 | **否** | `crc_extinguish=0`；未开 `ga_text_probe_crc.enable` |
| 位点性质 | **非业务指令** | `.text` 尾部 `INT3` 填充 `rva=0x4846BA`：`CCCCCCCC`→`90909090`→还原 |
| 早场 ~2 分钟进程消失 | **不能当 MemoryCrc 实锤** | Run A 无 `RESTORE`；与 Run B 矛盾；更像关端/偶发 |
| 外部 `ReadProcessMemory` 读游戏 | **被拒** | High IL 管理员仍 err 5（BlackCat）；探针必须进程内 |

---

## 3. 能推出 / 不能推出

**能推出**

- 「一改 GA 代码段就秒死」在本机、该填充位点上不成立。  
- 写 GA `.text` 的安全管道可先用进程内探针拿杀证；空 NGS 补丁表时不必干等 apply。  
- 当前**不必**先开 grap-core MemoryCrc 早退复测（无 KILL 实锤）。

**不能推出**

- 业务 inline（E9/`MovePath.Flush` 等）已安全。  
- MemoryCrc 不存在或不扫 GA（可能扫了但未对该页判死）。  
- 可默认恢复 archived 业务 hook。  
- KRW / 外部 RPM 能骗过完整性（KRW 文档已写明不是完整性解法）。

---

## 4. MemoryCrc 静态锚（对照枫星，本机已核）

| 项 | 值 |
|----|-----|
| 样本 | `grap-core64.aes`（**明文 PE**，非整文件加密） |
| MD5 | `CF0439C3474AD5C8A9B1BFBEAB29C65E`（= 枫星 / [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md)） |
| ImageBase | `0x400000`；动调用 **RVA** |
| `GRAP_MemoryCrc_RpmScan` | VA `0x142D610` / RVA `0x102D610` |
| vtable / vptr | 表基 `0x22CCB00[+0x30]` = RpmScan；Ctor 装 `0x22CCB28`，相对 **`[+0x08]`** 同槽（见 [`MemoryCrc派发与节奏.md`](MemoryCrc派发与节奏.md)） |

升级灭火仅在探针 **KILL** 后：进程内早退 RpmScan（开关 `GA_TEXT_PROBE_CRC=1` / `state/ga_text_probe_crc.enable`）；不改游戏树落盘、不预建多文件 CRC。

---

## 5. 开关与产物

| 项 | 路径 / 用法 |
|----|-------------|
| 开探针 | `bin/XCat_data/state/ga_text_probe.enable` 首字节 `1`，或环境 `GA_TEXT_PROBE=1`；**需重新注入** |
| 关探针 | enable → `0`（2026-08-01 已关） |
| 脏窗 | 默认 600000ms；`GA_TEXT_PROBE_MS` 可改 |
| 日志 | `bin/XCat_data/ga_text_probe.log`（在 `XCat_data/` 根，不在 `logs/`） |
| 状态 | `bin/XCat_data/state/ga_text_probe_status.txt`（`PASS`/`DIRTY`/…） |

---

## 6. 与 NGS / §4.1 门禁的关系

见 [`NGS补丁与CRC.md`](NGS补丁与CRC.md) · [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) §4.1。

| 文档原顺序 | 本轮纠偏 |
|------------|----------|
| 指纹 → apply NGS → GA 探针 → 再业务写 | 空 `ngs_patch.tsv` / 服务不常驻时：**先做进程内 GA 探针**拿杀证；NGS RE 并行，有行再 apply 对照 |
| 探针通过后才允许业务改 GA | **仍成立**；本轮仅证明填充位点 PASS，**未**授权业务 E9 |
| 探针死再升级 MemoryCrc | **仍成立**；本轮未触发 |

---

## 7. 下一步（工程）

1. 指定真实业务 RVA 做可回滚短窗试验（禁止批量恢复 `fly_flush_hook`）。  
2. 需要更硬证据时拉长脏窗（30–60min）或换热路径位点。  
3. NGS：mempe 上 WinVerifyTrust/OpenProcess 仍无可用 call-site → 继续不写空补丁表。
