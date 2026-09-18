# GA remount · Agent 清单（经典版 / TWMS）

> 给 Agent 执行，不是散文。命令真源也可：`python scripts/ga_remount.py howto`  
> 产品 = 经典版。只换 GA 哈希/方法头 RVA，**不改业务判定**。

## 0. 硬停（违反即停）

1. 用户没说 `apply` / `写入` / `--apply` → **禁止** `map --apply*` 和 `apply --apply`。
2. `--apply` = `--apply-hashes` + `--apply-krva`，**禁止**改注释/体内点里的裸 `0xHEX`。`--apply-all-hex` 仅当用户点名；`on_old_map=0` 时脚本会 REFUSE。
3. `audit` 体内点 FAIL → 跑 `catalog`（方法头窗口内唯一命中）再 `--write`；**禁止**全模块扫 `75 07` 猜点。`catalog --write` 改 tsv 的 rva/target **和** 该行 `cpp` 列的 `constexpr kRva*`（如 `kRvaMagicCmov`），不改注释里的裸 `0xHEX`。`--apply-krva` 仍跳过 catalog 体内点。
4. `layout --write` 仅当用户当轮说 write；只改 `FALLBACK_STALE` 的 `constexpr kFb*`。`TYPE_FLIP` / `MOVED_TYPE` / `DEAD` 拒绝写入。
5. `dump --process` 仅当用户当轮说 process/ForceDump；禁止与 `--archive` 同时。`verify` / `dump-check` / 默认 `dump` 都是干跑，不覆盖 `dump.cs`。
6. 禁止 `taskkill`、禁止打包发布、禁止动浏览器登录态。

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
python scripts/ga_remount.py dump --archive    # 注入前；用户说 archive
# 人注 GaRuntimeDump.dll
python scripts/ga_remount.py dump --process    # 仅当用户说 process/ForceDump
python scripts/ga_remount.py map
python scripts/ga_remount.py layout
python scripts/ga_remount.py krva
python scripts/ga_remount.py catalog
python scripts/ga_remount.py audit
python scripts/ga_remount.py verify            # 干跑，含 map/layout，不 apply
python scripts/ga_remount.py apply             # 干跑按序写入计划
python scripts/ga_remount.py apply --apply     # 用户说 apply：hashes→kFb→kRva→catalog，TYPE_FLIP/catalog FAIL 停
```

读：

- `Dumps/runtime/_ga_remount_layout.tsv`（`TYPE_FLIP` / `FALLBACK_STALE` = 槽漂了或 `kFb*` 过期）
- `Dumps/runtime/_ga_remount_krva.tsv`（`MISMATCH` = 代码 kRva 与 dump 里该哈希的方法头不是同一 RVA）
- `Dumps/runtime/_ga_remount_audit.txt`
- `Dumps/runtime/_ga_remount_catalog.tsv`（体内点 follow 父方法头；MOVE 才改 tsv）
- `Dumps/runtime/_ga_remount_apply.tsv`（空表 + `on_old_map=0` = 哈希已 remount，仍要过 layout / krva）
- `Dumps/runtime/_ga_remount_rva_collision.tsv`（活地址，禁止按表改）

仅当用户明确要求 **且** `on_old_map>0`（或 `apply --apply` 一次走完）：

```text
python scripts/ga_remount.py apply --apply
# 等价拆开：
python scripts/ga_remount.py map --apply-hashes
python scripts/ga_remount.py layout
python scripts/ga_remount.py map --apply-krva
python scripts/ga_remount.py krva
python scripts/ga_remount.py catalog
python scripts/ga_remount.py audit
```

注入后：`python scripts/ga_remount.py smoke`（对照 `scripts/data/ga_remount_smoke_expect.tsv`；`need=req` 缺组才红，`opt` 缺组 SKIP。只扫当前 `x.jsonl` 会漏轮转里的 bind 行。必过组若被二次 Bind 冲掉，回退全日志 last-wins 并打 `WARN req prior-session`，不当红灯）。空日志 = 红灯。更新日前可先 `python scripts/ga_remount.py verify`（一次干跑 selftest+dump-check+map+layout+krva+catalog+audit+smoke）。

更新日前可跑：`python scripts/ga_remount.py dump-check`（不写 `dump.cs`）。注 dump 前：`dump --archive`。人注 `GaRuntimeDump.dll` 之后、且用户说 process/ForceDump：`dump --process`（拷客户端 metadata + `process_runtime_dump.py`）。禁止 `--archive` 和 `--process` 一起。`krva --write-bind` 刷新 `scripts/data/ga_krva_bind.tsv`（`kind=hash|plain`，`shared=1` 的虚方法/重载禁止唯一改写）。下次 `--apply-krva` 按身份改 RVA，明文 Unity/TW 方法也能走 bind，不再因为旧数字仍是方法头而跳过。

`catalog` 用 `ga_patch_sites.tsv` 的 `parent` / `anchor` 在**父方法窗口**里唯一重钉体内点（text 锚点 / call 的 E8 / data 的 RIP→期望字节）。`75 07` 在 TryDoingMagicAttack 里有上百处，无父窗口必 FAIL。默认 dry-run；用户说 write 才 `--write`（tsv 的 rva/target/parent_rva **加上** `cpp` 列那些 `constexpr kRva*`，例如 `kRvaMagicCmov`；不改注释）。Kick 的 E8 不是 dump 方法头，父函数哈希 `e030bc59…` @`0x87CFC0`。`--apply-krva` 跳过 catalog 体内点 / seed，禁止用 `rva_map` 改 `kRvaByPetRectPackFallback` 这类槽。

然后只编本职：`xcat_probe`（产出 `bin\XCat_data\xcat.dll`）。链接占用不要杀进程，等用户关。

## 3. 输出怎么判

| 看到 | 含义 | 动作 |
|---|---|---|
| `FAIL hash not in dump` | 源码哈希死了 | 用 apply 映射换；映射也没有 → IDA/dump 对类 |
| `layout FALLBACK_STALE` | dump 新偏移 ≠ 源码 `kFb*` | 用户说 write 才 `layout --write`；禁止改注释 |
| `layout TYPE_FLIP` / `MOVED_TYPE` | 同字段类型变了 | 按新类型重钉哈希和 `kFb*`，`--write` 拒绝 |
| `FAIL kRva not in dump` | 方法头漂了或根本不是方法头 | 先看名字是否 Seed / catalog / grap-core；真方法头才换 |
| `IGN` | `ga_remount_ignore.txt` | 不当红灯 |
| `WARN ... dump=Ptr shape=ValueTypeApprox` | dump 把混淆 valuetype 写成 class | 不当红灯 |
| 体内 `have xx want yy` | 指令/常量框变了 | 先 `catalog`；唯一命中则 `--write` 再改对应 `kRva*`；仍 FAIL 才 IDA |
| `catalog FAIL text hits=` | 父窗口内锚点不唯一/失踪 | 加长 `anchor`（`??` 掉 RIP/call rel32）；禁止 flatten 搜 `75 07` |
| `catalog STALE` | GA 点已对上，constexpr kRva 还是旧数 | `--write` 改声明；禁止改注释 |
| `krva MISMATCH` | 同后缀 kHash 在 dump 里的方法头 ≠ 源码 kRva | IDA 确认是不是 dump 误标名（如 TryDoingTeleport）；真指错才改 kRva |
| `krva MATCH` | 源码 RVA 是该哈希的任一 override 头 | 绿灯 |
| `krva DUMP_ID` | 无 kHash，该 RVA 只对应一个方法哈希 | 写入 bind（`shared=1` 若该哈希有多个 override） |
| `krva PLAIN_ID` | 无 kHash，dump 明文 `Class::Method` 唯一 | 写入 bind；下次 apply 按类名/类哈希重锚 |
| `krva DUMP_MULTI` / `PLAIN_MULTI` | 一址多名 | 不写 bind，不当红灯 |
| `smoke FAIL missing` | 必过组（Bind/AutoEnter/Combat/Teleport/Invuln/KickSniff）没出现 | 扫 `x.jsonl*`；对照 expect |
| `smoke WARN req prior-session` | 必过组不在最后一次 Bind 会话，更早日志有 | 不当红灯；二次 Bind 冲掉了 Teleport 等 |
| `smoke SKIP optional missing` | 进店/F6/旅行等选过组没打 | 不当红灯 |
| `BIND_STALE` / `BIND_NEW` | 当前 dump 身份与 `ga_krva_bind.tsv` 不一致 | 用户说 write 才 `krva --write-bind` |
| `REFUSE remaining writes` | `apply --apply` 碰到 TYPE_FLIP / catalog FAIL | 停；前面已写下的 hashes 保留，后面的 kRva/catalog 不写 |

## 4. 允许改 / 禁止改

允许：`x/**/*.cpp` `x/**/*.h` 里的哈希字面量、`constexpr … kRva* = 0x…`、`constexpr … kFb* = 0x…`、`il2cpp_shape.cpp` 的哈希与字段 off；`scripts/data/ga_patch_sites.tsv` 的 rva/expect/**cpp**。

禁止：if/else、默认开关、补丁策略、CurFh 收成 cmov 这类**业务**；顺手修无关模块；改 `common/` 逻辑。

新发现的体内点：先加 tsv 一行再写 `kRva*`，下次 audit 会盯。不要为了绿灯往 `ga_remount_ignore.txt` 塞还没定性的槽。

## 5. 本清单不覆盖（下轮工具债，别在更新日临时做）

- 自动从平坦化搜 jnz/cmov。
- 前半段 dump 仍要人注入 `GaRuntimeDump.dll`。后半段用 `dump --archive` / `dump --process`，不要把 ForceDump 塞进自定义注入。

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
