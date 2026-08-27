# MemoryCrc 派发与扫描节奏（经典版 · 2026-08-01）

> **产品** = TWMS 经典版；**枫星** = 对照来源（同 MD5 `grap-core64.aes`）。  
> 本文是本仓 MemoryCrc **派发 / 节奏** 的工程 SSOT；地址以 ImageBase `0x400000` 的 VA 书写，运行时用 `module+RVA`。  
> 样本：`Dumps/runtime/_grap_dig/grap-core64.aes` · IDB：`grap-core64.dll.i64` · Hex-Rays / IDA MCP 核验。

---

## 0. 三秒结论

| 问题 | 结论 |
|------|------|
| 改 GA / 任意模块 `.text` 会撞谁？ | **MemoryCrc**（及并行的 MemorySharedPage / Alt） |
| RPM 从哪几个函数出去？ | 仅三处虚方法：`RpmScan` / `MemoryPage_RpmScan_278` / `RpmScan_Alt`（IAT 无其它 caller） |
| 周期是固定 60s 吗？ | **否** |
| DRBG 换表 = 每次 generate 都跑 MemoryCrc？ | **否**。DRBG 路径主要是 OpenSSL 密码学；**旧「DRBG→Virt30→RpmScan」链已证伪** |
| Init 会不会碰到 RpmScan？ | **会**。`DetectorRegistry_Init` → `DetectorIter_MemoryCrc`（18×）→ `call [vptr+8]` = RpmScan |
| 周期派发在哪？ | **未打穿**。候选：`DetectWorker`（平坦化重）；须动调断 `RpmScan` 看调用栈 |

---

## 1. ERRATA · 对照仓旧结论（勿再沿用）

下列命名/链条来自枫星 `grap_core_ida.md` 会话六，**2026-08-01 经典版同 MD5 样本上已被 Hex-Rays 否定**：

| 旧名 / 旧说法 | 真相 |
|---------------|------|
| `GRAP_DRBG_RunDetectors` @ `0x209F410` | OpenSSL **CTR-DRBG generate**：16 字节 V 大端 `++` + cipher update，**不是**检测调度 |
| `GRAP_DRBG_FeedDetectors` @ `0x209F170` | DRBG **additional input** 吸收，不是「喂检测器」 |
| `GRAP_DetectorRunnerTable` @ `0x280CB80` + `Virt30` @ `0x20A14BD` | OpenSSL **CAPI / RSA / BN**（`e_capi.c`、`bn_blind.c` 等）；`call [r10+30h]` 是密码学方法表，**不是** MemoryCrc 派发 |
| 「GenerateCore 之后调 runner 跑 RpmScan」 | `0x2061000` = `RAND_DRBG_generate`（`drbg_lib.c`）；只走 `[ctx+320]+8` 的 generate hook |
| 「Iter **不**调 RpmScan」 | Iter **会** `call [rax+8]`；装在 `obj+0x6A0` 的 vptr=`0x22CCB28` 时，`[+0x08]` **就是** RpmScan |
| 绝对表 `0x22CCB00[+0x30]` vs 安装 vptr | Ctor 写入的是 **`&off_22CCB28`**；相对该 vptr 应用 **`[+0x08]`** 理解 |

对照仓原文已加 ERRATA 指针；本仓工程以**本文**为准。

---

## 2. RPM 出口（唯一集合）

`ReadProcessMemory` IAT（`0x26E60B8`）全部 xref：

| 函数 | VA | `nSize` | 失败码（对照） |
|------|-----|---------|----------------|
| `GRAP_MemoryCrc_RpmScan` | `0x142D610` | `0xE8` | 29 |
| `GRAP_MemoryPage_RpmScan_278` | `0x1432D00` | `0x278` | 79 |
| `GRAP_MemoryCrc_RpmScan_Alt` | `0x14382F0` | `0xE8` | （Alt / 并行） |

均 **无直接 `E8` caller**，只经虚表。

---

## 3. 复合对象与 vptr

`GRAP_DetectorObject_Ctor` @ `0xC17890`（节选）：

| 对象偏移 | 安装 vptr | 相对 vptr 的扫描槽 | 目标 |
|----------|-----------|-------------------|------|
| `+0x6A0` | `0x22CCB28` | **`[+0x08]`** → `0x142D610` | 主 MemoryCrc |
| `+0x7A8` | `0x22CCBE8` | **`[+0x08]`** → `0x1432D00` | MemorySharedPage |
| `+0xA40` | （Alt 表） | **`[+0x08]`** → Alt RpmScan | MemoryCrc 变体 |

表基 `0x22CCB00` 上仍可见 absolute `+0x30 = RpmScan`；**代码装的是 `+0x28` 起的 vptr**，阅读/hook 时不要混用两套偏移。

---

## 4. Init 派发（已静态闭合）

```
GRAP_DetectorRegistry_Init @ 0xC0F140
  ├─ GRAP_DetectorObject_Ctor
  ├─ GRAP_ScanOrchestrator_Init @ 0x1422050   ← 页列表 Init 一次
  │     └─ sub_1418750 / sub_1422F40（写 inner 页缓冲；非运行时 VAD 步进）
  └─ GRAP_DetectorIter_MemoryCrc @ 0x1417D80  ×18  (E8 全在 Registry_Init 内)
        例 @ 0xC1522A:
          lea  rcx, [rdi+6A0h]          ; MemoryCrc 子对象
          mov  rdx, rbx
          call DetectorIter_MemoryCrc
        Iter 内:
          call [rax+8]                  ; → RpmScan(a1, ctx)
          call [rax+28h]                ; 取上下文
```

含义：

- **登录/GRAP Init 窗**内就可能跑 RPM；脏 `.text` 不必等到「某个 60s」。  
- Iter 的 18 个 caller **全部**在 Registry_Init → **不是**周期调度器。

---

## 5. OpenSSL DRBG（仍存在，但语义已纠偏）

| VA | 宜用名 | 作用 |
|----|--------|------|
| `0x209ED40` | AttachCallbackTable | `*(drbg_ctx+0x140)=CallbackTable`（0x140=320） |
| `0x2061000` | `OpenSSL_RAND_DRBG_generate` | `..\crypto\rand\drbg_lib.c` |
| `0x209EB30` | GenerateHook | 薄包装 → `0x209F410` |
| `0x209F410` | CtrDrbgGenerate（旧误名 RunDetectors） | V 计数器 ++ / 密钥流 |
| `0x209F170` | FeedAdditionalInput | 附加输入 |

**仍成立**：core 把 DRBG 回调表换成自有指针；generate 频率随随机数消耗变化。  
**不成立**：该路径经「DetectorRunner/Virt30」直接派发 MemoryCrc。

另有 `GRAP_DetectWorker_Init` @ `0x10517C0`（`Client_WorkerMain` 调用）——控制流平坦化严重，**周期 RpmScan 是否由此出尚未静态证明**。

---

## 6. 工程含义（经典版 XCAT）

1. **禁止**游戏进程内 INLINE HOOK / 改 GA·ntdll·grap-core `.text`（见 [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) §4.1）。
   闸门 = `XCAT_ALLOW_TEXT_PATCH=1`，且**只许用户显式给**：2026-08-13 盘点发现 `melee_veto` /
   `movepath_flush_probe`（及已拆除的 `infinite_stars`）曾在启用时自设该变量绕闸，已修（[`检测面盘点与187秒墙.md`](检测面盘点与187秒墙.md) §6）。  
2. Init 窗与运行中都可能 RPM；探针脏窗（如 `ga_text_probe` 默认 10min）是**观察窗**，不是官方周期常量。  
3. 灭火目标若是 MemoryCrc：对准 **RpmScan / 其 vptr+8 / 页表数据面**，不要去 patch OpenSSL「Virt30」。  
4. 周期调用栈未闭合前，**不要**把「DRBG 忙碌 = MemoryCrc 更勤」写成已证因果；最多作弱相关假设。  
5. 填充位点探针 PASS ≠ 业务 E9 放行（[`GA文本探针.md`](GA文本探针.md)）。

---

## 7. 未决 / 下一跳

| 项 | 状态 |
|----|------|
| `DetectWorker` 线程入口 → 是否 `call` RpmScan | BLOCKED（平坦化）；建议动调 |
| 是否存在第二套非 Iter 的周期虚调 | 未知 |
| 页表是否会在运行期重建 | 静态倾向「Init 一次」；需动调复核 |

动调建议断点（RVA）：`+0x102D610`（RpmScan）、`+0x1017D80`（Iter）、`+0xC1522A`（Init 调用点）。

---

## 8. 证据指针

| 材料 | 路径 |
|------|------|
| 本仓静态锚（旧文，部分措辞已过时处见本文 ERRATA） | `Dumps/runtime/_grap_dig/MEMORYCRC_STATIC.md` |
| 威胁模型 | [`GRAP与枫星对齐.md`](GRAP与枫星对齐.md) |
| NGS / GA 写顺序 | [`NGS补丁与CRC.md`](NGS补丁与CRC.md) · [`GA文本探针.md`](GA文本探针.md) |
| 对照仓（已加 ERRATA） | `xcat_for_fengxing/docs/research/grap/grap_core_ida.md` |
| IDB | `Dumps/runtime/_grap_dig/grap-core64.dll.i64` |
