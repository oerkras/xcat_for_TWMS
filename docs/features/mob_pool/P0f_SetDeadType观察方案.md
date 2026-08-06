# mob_pool P0f · `SetDeadType` 运行时观察方案（设计 · 未实现）

> **状态**：📐 观察设计 · **本轮不改产品代码**  
> **产品**：经典版 TWMS · **不是**枫星  
> **约束**：**禁止** INLINE HOOK（E9 / `.text`）；优先 **MI `methodPointer` 换针** 或 **HWBP 执行断**；Frida attach 预期被 GRAP 拒（见 security 文档）  
> **静态真源**：[`P0e_SetDeadType与deadType.md`](P0e_SetDeadType与deadType.md)  
> **挂点范式对照**：[`P0b_MI观察与按需Collect.md`](P0b_MI观察与按需Collect.md)、`kick_sniff` HWBP

---

## 0. 一句话

对 **`Mob.SetDeadType` 入口**做只读观察，采 **`edx`（写入值）+ 返回址 + mobId**，钉死枚举与调用方；并回答 **`FillLite` 的 `deadType!=0` 门是否曾真正过滤「将死未 Leave」窗**。

---

## 1. 目标与非目标

| 要 | 不要 |
|---|---|
| 采样 `edx` 直方图（0/1/2/…） | 改 GA `.text` / E9 |
| 还原调用方（`rsp[0]` RVA） | 默认改打怪 / FillLite 语义 |
| 与 LeaveField 时序对照 | 解析全包、伪造发包 |
| 证明「从不命中」亦可（负结果有价值） | 占满 DR 槽影响 kick/其它 HWBP（见 §3.3） |
| 默认可关、可打 JSONL | 在 hook 里做 Collect / 选怪 |

---

## 2. 锚点（当前 IDB）

| 项 | 值 |
|---|---|
| 符号 | `Mob_SetDeadType` |
| RVA | **`0xF398A0`**（旧 `0xF37A30` · [`REMOUNT_20260806`](REMOUNT_20260806.md)） |
| 运行时 VA（本 dump） | `GameAssembly+0xF398A0`（imagebase `0x7FF848C80000`） |
| 约定 | `rcx` = `Mob*` · `edx` = 新 `_deadType` |
| 字段 | `_deadType@0x1B4` · `_mobId@0x134` · `_suspended@0x1B8` |
| Klass hash | `d8cb6fb7d6903613c27c6c663961d0c02d458f0191006f691706e9ef9783849`（`mob_pool_port` SSOT · 2026-08-06） |

对照挂点（时序，可选）：

| 方法 | RVA | 用途 |
|---|---:|---|
| `OnPacketMobLeaveField` | `0xF78230` | 同杀怪会话里是否「先 SetDeadType 再 Leave」 |
| `Mob_OnDie` | （随 remount 查 dump） | 是否与死亡动画同帧（静态未见写 deadType） |

---

## 3. 观察手段（三选一，按优先级）

### 3.1 ★ 推荐：MethodInfo 换针（与 P0b / drop_pool 同构）

```text
klass = FindClass("", kMobClassHash)
mi    = FindMethodByRva(klass, 0xF398A0)   // shape: void + (int) 或宽松
orig  = mi->methodPointer
mi->methodPointer = Hook_SetDeadType
```

**Hook 骨架（设计级 · 未落码）**：

```text
Hook_SetDeadType(Mob* self, int type, MethodInfo* mi):
  // 1) 采证（先于 orig，避免 orig 极短被优化感知差）
  log {
    t_ms,
    type,                    // edx
    mobId   = *(i32*)(self+0x134),
    old     = *(i32*)(self+0x1B4),
    ret_rva = rsp[0] - ga_base,
    suspended = *(u8*)(self+0x1B8),
    in_dict = ? optional pool lookup
  }
  // 2) 可选：浅栈 4~8 帧 RVA（与 kick.log 同风格）
  // 3) call orig(self, type, mi)
```

| 优点 | 风险 |
|---|---|
| 专打「走 MI / 虚派发」的调用（静态无 E8 正合适） | 若存在**绕过 MI 的直调**（本 build 未见）会漏 |
| 不改 `.text`；可 Uninstall | 需 `VirtualProtect` 写 MI 页 |
| 可复用 `il2cpp_method` / `FindMethodByRva` | shape 校验过严会找不到 MI |

**落点建议（未实现）**：旁路 `x/features/mob_pool_observe/` 或探针 flag，**默认关**。

```ini
[core]
; 设计名，未挂面板
mobDeadTypeObserve=0
```

### 3.2 备选：HWBP 执行断（入口）

对齐 `kick_sniff`：`DR*` 执行断在 `GA+0xF398A0`。

| 优点 | 风险 |
|---|---|
| 凡落地入口的 call（含 `call rax`）都中 | **与 kick / 其它 DR 争槽**；文档要求简单功能勿占 DR |
| 不改 MI | VEH/线程安装成本；多线程需覆盖 |

**仅**在 MI 换针长时间 0 命中、又要证伪「直调」时启用；用完卸。

### 3.3 不推荐作主路径

| 手段 | 原因 |
|---|---|
| Frida `Interceptor` | GRAP 常拒 attach（见 `security/GRAP与枫星对齐.md`） |
| 对任意 `Mob+0x1B4` 写断 | 需先有实例地址；刷怪多时噪声大 |
| IDA 静态 BP 长期挂着 | ASLR；且不写 JSONL 难做直方图 |

x64dbg / IDA **人工**短时 BP 可用于 §5 手工验收，不算产品路径。

---

## 4. 日志契约（建议）

文件：`artifacts/ops_logs/mob_deadtype.jsonl`（或探针统一目录）

每行一事件：

```json
{
  "op": "SetDeadType",
  "t": 0,
  "type": 1,
  "old": 0,
  "mobId": 12345,
  "templateId": 0,
  "sus": 0,
  "ret": "0xF76xxx",
  "stack": ["0xF76xxx", "0xF7xxxx"]
}
```

| 字段 | 含义 |
|---|---|
| `type` | 本次写入值（`edx`） |
| `old` | 写前 `@0x1B4` |
| `ret` | `rsp[0]` 相对 GA 的 RVA（**主调用方线索**） |
| `stack` | 可选浅栈 |
| `sus` | 写前 `_suspended` |

聚合脚本（人工即可）：按 `type` 计数；按 `ret` 聚类 → 回 IDA 点名。

---

## 5. 实机场景矩阵

每场景至少杀/离 **3～5** 只；记是否命中 SetDeadType。

| # | 场景 | 期望验证 |
|---:|---|---|
| A | 普通图普攻打死 | 有无 `SetDeadType`？`type`？是否先于 Leave |
| B | 技能 AOE 清怪 | 同上；是否批量同 `ret` |
| C | 怪走远 / 换频道 Leave | **仅 Leave、无 SetDeadType**？（静态倾向） |
| D | 延死动画怪（疑 leaveMode≠0） | DelayedDead 与 type 是否同现 |
| E | Boss / 召唤复活 | `type>1`？随后是否 `GetReviveList` 有动静 |
| F | `OnDestructByMiss` 类（未打中自毁） | 是否走 SetDeadType |

**负结果也算结案**：全场景 0 命中 → 本 build 业务不走该 API；`FillLite` 的 deadType 门对「官方死亡」基本无效，过滤的是**其它写源或脏内存**（若永远 0 则门恒真）。

---

## 6. 与 Leave 的时序判据

同会话并挂（或短时 HWBP）`LeaveField@0xF763C0`，只记 `mobId` + 时间戳：

```text
T0  SetDeadType(mobId, type=k)
T1  LeaveField(mobId)     // Δt = T1-T0
```

| 模式 | 含义 | 对 FillLite |
|---|---|---|
| 有 T0，且 Δt 较大、怪仍在 dict | 「将死未离」窗存在 | deadType 门**有意义** |
| 有 T0，Δt≈0 且紧接 Remove | 窗极短 | 门几乎无收益 |
| 无 T0，仅 Leave | 死亡不写 deadType | 门对官方死亡**无意义** |

---

## 7. 验收清单（完成门禁）

声称「枚举已钉 / 调用方已钉 / 门无意义」前必须具备：

- [ ] 至少一种手段（§3.1 或 §3.2）在目标进程真实安装成功日志  
- [ ] §5 中 A + C 必做；B/D/E 按时间盒选做  
- [ ] `type` 直方图 + Top `ret` RVA 列表（可空）  
- [ ] 明确写出三种时序模式何者被观察到  
- [ ] 未改 `.text`；未默认打开影响战斗的逻辑  

回填位置：更新 [`P0e`](P0e_SetDeadType与deadType.md) §6/§7，或在本文件追加「§8 实机结果」。

---

## 8. 实现备忘（将来落码时）

| 项 | 建议 |
|---|---|
| 解析 | `FindMethodByRva(gMobKlass, 0xF398A0)`；失败则扫 `methodPointer==GA+RVA` |
| Shape | `void` + 1×`I4`（`this` 隐含）；过严则先按 RVA 裸匹配 |
| 节流 | 同 `(mobId,type)` 10ms 去重，防异常刷屏 |
| 卸载 | 恢复 `methodPointer`；HWBP 清 DR |
| 与 P0b | 可同模块 `mob_pool_observe`，**独立开关**，勿绑死 ImmediateScan |

---

## 9. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-08-06 | 初稿：MI 换针优先 + HWBP 备选 + 日志/场景矩阵 + Leave 时序判据；未落码 |
