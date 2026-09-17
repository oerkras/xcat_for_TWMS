# GA remount · Agent 清单（经典版 / TWMS）

> 给 Agent 执行，不是散文。命令真源也可：`python scripts/ga_remount.py howto`  
> 产品 = 经典版。只换 GA 哈希/方法头 RVA，**不改业务判定**。

## 0. 硬停（违反即停）

1. 用户没说 `apply` / `写入` / `--apply` → **禁止** `map --apply*`。
2. `--apply` = `--apply-hashes` + `--apply-krva`，**禁止**改注释/体内点里的裸 `0xHEX`。`--apply-all-hex` 仅当用户点名；`on_old_map=0` 时脚本会 REFUSE。
3. `audit` 体内点 FAIL → 只改 catalog 对应 `kRva*` / 期望字节；**禁止**扫 `75 07` 猜点。
4. 禁止 `taskkill`、禁止打包发布、禁止动浏览器登录态。

## 1. 前置（没有就停，去找人 dump，不要编）

| 要有 | 路径 |
|---|---|
| 新 dump.cs | `Dumps/runtime/out/dump.cs` |
| 运行时 GA（≥100MB） | `Dumps/runtime/GameAssembly.dll` |
| 旧 dump.cs | `Dumps/runtime/_archive_<日期>_pre_<新日期>_update/out/dump.cs` |

没有新 dump：先按 `Dumps/runtime/REDUMP_20260803.md` 做 ForceDump + Il2CppDumper（`ForceVersion=31`），再回来。不要用游戏目录里 ~36MB 壳文件。

归档旧产物（有新 dump 之后、覆盖 `out/` 之前）：

```text
Dumps/runtime/_archive_YYYYMMDD_pre_YYYYMMDD_update/out/dump.cs
Dumps/runtime/_archive_YYYYMMDD_pre_YYYYMMDD_update/GameAssembly.dll   （可选，连 IDB 一起挪）
```

## 2. 命令（仓根 · 顺序不许跳）

```text
python scripts/ga_remount.py map
python scripts/ga_remount.py layout
python scripts/ga_remount.py krva
python scripts/ga_remount.py audit
```

读：

- `Dumps/runtime/_ga_remount_layout.tsv`（`TYPE_FLIP` / `FALLBACK_STALE` = 槽漂了或 `kFb*` 过期）
- `Dumps/runtime/_ga_remount_krva.tsv`（`MISMATCH` = 代码 kRva 与 dump 里该哈希的方法头不是同一 RVA）
- `Dumps/runtime/_ga_remount_audit.txt`
- `Dumps/runtime/_ga_remount_apply.tsv`（空表 + `on_old_map=0` = 哈希已 remount，仍要过 layout / krva）
- `Dumps/runtime/_ga_remount_rva_collision.tsv`（活地址，禁止按表改）

仅当用户明确要求 **且** `on_old_map>0`：

```text
python scripts/ga_remount.py map --apply-hashes
python scripts/ga_remount.py layout
python scripts/ga_remount.py map --apply-krva
python scripts/ga_remount.py krva
python scripts/ga_remount.py audit
```

注入后：`python scripts/ga_remount.py smoke`（对照 `scripts/data/ga_remount_smoke_expect.tsv`；只扫当前 `x.jsonl` 会漏轮转里的 bind 行）。空日志 = 红灯。

更新日前可跑：`python scripts/ga_remount.py dump-check`（不写 `dump.cs`）。`krva --write-bind` 刷新 `scripts/data/ga_krva_bind.tsv`，下次 `--apply-krva` 按方法哈希改 RVA，不再因为旧数字在新 dump 里仍是方法头而跳过。

然后只编本职：`xcat_probe`（产出 `bin\XCat_data\xcat.dll`）。链接占用不要杀进程，等用户关。

## 3. 输出怎么判

| 看到 | 含义 | 动作 |
|---|---|---|
| `FAIL hash not in dump` | 源码哈希死了 | 用 apply 映射换；映射也没有 → IDA/dump 对类 |
| `layout TYPE_FLIP` / `MOVED_TYPE` | 同字段类型变了（int→Dictionary、Vector2→数组） | 按**新类型**重钉哈希和 `kFb*`，禁止沿用旧偏移 |
| `layout FALLBACK_STALE` | dump 新偏移 ≠ 源码 `kFb*` | 只改 fallback / 哈希，不改业务 |
| `FAIL kRva not in dump` | 方法头漂了或根本不是方法头 | 先看名字是否 Seed / catalog / grap-core；真方法头才换 |
| `IGN` | `ga_remount_ignore.txt` | 不当红灯 |
| `WARN ... dump=Ptr shape=ValueTypeApprox` | dump 把混淆 valuetype 写成 class | 不当红灯 |
| 体内 `have xx want yy` | 指令/常量框变了 | IDA 重钉，更新 tsv **和** 对应 `kRva*` |
| `krva MISMATCH` | 同后缀 kHash 在 dump 里的方法头 ≠ 源码 kRva | IDA 确认是不是 dump 误标名（如 TryDoingTeleport）；真指错才改 kRva |
| `krva MATCH` | 源码 RVA 是该哈希的任一 override 头 | 绿灯 |
| `krva DUMP_ID` | 无 kHash，但 dump 该 RVA 只对应一个方法哈希 | 写入 bind，不当红灯 |
| `smoke FAIL missing` / 空 hits | 当前 x.jsonl 被截断或没扫轮转 | 扫 `x.jsonl*`；对照 expect |
| `REFUSE --apply-all-hex` | 哈希已在新 dump | 停；用 `--apply-hashes` / `--apply-krva` |

## 4. 允许改 / 禁止改

允许：`x/**/*.cpp` `x/**/*.h` 里的哈希字面量、`constexpr … kRva* = 0x…`、`il2cpp_shape.cpp` 的哈希与字段 off；`scripts/data/ga_patch_sites.tsv` 的 rva/expect。

禁止：if/else、默认开关、补丁策略、CurFh 收成 cmov 这类**业务**；顺手修无关模块；改 `common/` 逻辑。

新发现的体内点：先加 tsv 一行再写 `kRva*`，下次 audit 会盯。不要为了绿灯往 `ga_remount_ignore.txt` 塞还没定性的槽。

## 5. 本清单不覆盖（下轮工具债，别在更新日临时做）

- 自动从平坦化搜 jnz/cmov。
- ForceDump 仍要人注入 `GaRuntimeDump.dll`；脚本只预检（`dump-check`）+ `process_runtime_dump.py`。

## 6. 2026-09-04 IDA 抽检（F5 热路径 · 09-03 dump）

IDB：`Dumps/runtime/GameAssembly.dll.i64`，imagebase `0x7FF86BA70000`。只核方法头身份 / ABI / klass 槽，不改业务。

| 锚点 | 源码 RVA | IDA 结论 |
|---|---|---|
| `OnFuncKey` | `0x10A74D0` | 序言 `rcx=this edx=type r8=FuncKey* r9=scan`，哈希 OK |
| `GetBodyRect` | `0xF2BAA0` | `rsi=Rect*`，两处 `movups [rsi], xmm0`（16 字节），哈希 OK |
| `SetImpactNext` | `0x11C9310` | `xmm1/xmm2` 双精度，哈希 OK |
| `SetInput` | `0x11DCAA0` | `edx/r8d` → `+0x50/+0x54`，哈希 OK |
| `WUA` 禁台槽 | klass `+0x208/+0x218` | 仍在：`r9=0` 调 `+208`，`r9=1` 调 `+218` |
| Camera `get_main` / STW(Vector3) / `get_transform` / `get_position` | `0x4E2ECD0` / `0x4E2E800` / `0x4E98F30` / `0x4EB39A0` | icall 包装字符串对上；STW 硬编码 eye=2 |
| `set/get_targetFrameRate` / `set_vSyncCount` | `0x4E28A50` / `0x4E28A10` / `0x4E43DB0` | Unity 明文包装对上 |
| **`GetKeyByFunc`** | 旧 `0x1671DD0` → **`0x1676590`** | 旧 RVA 是 FKM 4 字节 setter（`mov [rcx+20h], edx; ret`）。哈希仍在，真方法 dump.cs `0x1676590`。`FindMethodResolved` 先走哈希，03:33 BIN 的合成 5/52 也不依赖它。 |

F5 出刀成功后硬崩：上表方法头对得上，**不是**「OnFuncKey / SetImpactNext / GetBodyRect 指错函数」。
---
