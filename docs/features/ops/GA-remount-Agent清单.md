# GA remount · Agent 清单（经典版 / TWMS）

> 给 Agent 执行，不是散文。命令真源也可：`python scripts/ga_remount.py howto`  
> 产品 = 经典版。只换 GA 哈希/方法头 RVA，**不改业务判定**。

## 0. 硬停（违反即停）

1. 用户没说 `apply` / `写入` / `--apply` → **禁止** `map --apply`。
2. `map` 输出 `on_old_map=0` 且 `already_on_new>0` → 源码已在新 dump 上，**禁止**再 apply（数字重用会改坏活方法，例如 `0x166BD40` 已是 FuncKey.ctor）。
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
python scripts/ga_remount.py audit
```

读：

- `Dumps/runtime/_ga_remount_audit.txt`
- `Dumps/runtime/_ga_remount_apply.tsv`（空表 + `on_old_map=0` = 已 remount，到此可结束）
- `Dumps/runtime/_ga_remount_rva_collision.tsv`（活地址，禁止按表改）

仅当用户明确要求 **且** `on_old_map>0`：

```text
python scripts/ga_remount.py map --apply
python scripts/ga_remount.py audit
```

然后只编本职：`xcat_probe`（产出 `bin\XCat_data\xcat.dll`）。链接占用不要杀进程，等用户关。

## 3. 输出怎么判

| 看到 | 含义 | 动作 |
|---|---|---|
| `FAIL hash not in dump` | 源码哈希死了 | 用 apply 映射换；映射也没有 → IDA/dump 对类 |
| `FAIL kRva not in dump` | 方法头漂了或根本不是方法头 | 先看名字是否 Seed / catalog / grap-core；真方法头才换 |
| `IGN` | `ga_remount_ignore.txt` | 不当红灯 |
| `WARN ... dump=Ptr shape=ValueTypeApprox` | dump 把混淆 valuetype 写成 class | 不当红灯 |
| 体内 `have xx want yy` | 指令/常量框变了 | IDA 重钉，更新 tsv **和** 对应 `kRva*` |
| `REFUSE --apply` | 已经在新 dump | 停 |

## 4. 允许改 / 禁止改

允许：`x/**/*.cpp` `x/**/*.h` 里的哈希字面量、`constexpr … kRva* = 0x…`、`il2cpp_shape.cpp` 的哈希与字段 off；`scripts/data/ga_patch_sites.tsv` 的 rva/expect。

禁止：if/else、默认开关、补丁策略、CurFh 收成 cmov 这类**业务**；顺手修无关模块；改 `common/` 逻辑。

新发现的体内点：先加 tsv 一行再写 `kRva*`，下次 audit 会盯。不要为了绿灯往 `ga_remount_ignore.txt` 塞还没定性的槽。

## 5. 本清单不覆盖（下轮工具债，别在更新日临时做）

- 把 ForceDump / Il2CppDumper 收成一条脚本（要注入、要游戏在跑）。
- 按**方法哈希**对 `kRva*`（现在只验「这个数字是不是某个方法头」，验不出指错函数）。
- 自动从平坦化搜 jnz/cmov。
---
