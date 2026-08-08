# il2cpp 托管调用线程规约（经典版 / TWMS）

> 产品 = 经典版（`Maplestory_Classic.exe`）。本文讲的是 **XCat 什么时候可以碰 il2cpp / 托管代码**，
> 以及违反规约会怎么死。2026-08-09 用三轮实机日志 + `GameAssembly.dll` 反汇编打穿，结论已落规则
> [`.cursor/rules/il2cpp-managed-call-on-pump.mdc`](../../../.cursor/rules/il2cpp-managed-call-on-pump.mdc)。

---

## 1. 铁律

**任何可能触发 il2cpp 类初始化的调用，必须在 MainPump（Unity 主线程）上执行。**

包括但不限于：

| 类别 | 例子 |
|---|---|
| 直接调托管方法 | `FnFromMi<...>(mi, rva)(mi)`、经 `MethodInfo->methodPointer` 的函数指针调用 |
| il2cpp 运行时 API | `il2cpp_runtime_class_init`、`il2cpp_object_new`、`il2cpp_string_new` |
| 间接触发 | 上面这些的**方法序言**——generated code 会自己去解元数据，绕过你在 API 层加的守卫 |

后台 worker 上只允许做**纯内存读**（已解析好的字段偏移、已缓存的指针）。要拿托管侧的答案，
一律 `x::runtime::main_thread::InvokeAndWait`，结果自己缓存，别按 tick 频率打爆泵。

`x/runtime/il2cpp_bind.h` 里 `RuntimeClassInit` / `objectNew` / `stringNew` 早就带了 off-pump 守卫
（日志 `skip runtimeClassInit off-pump → avoid GC unknown-thread`）。**那道守卫只挡显式 API 调用，
挡不住托管方法序言里的隐式初始化**——这正是本次事故的缺口。

---

## 2. 事故：换图黑屏（2026-08-09）

### 2.1 症状

进图或换图后客户端整个卡死、黑屏，不响应，CPU 持续爬升（实测 4 分钟烧到 700+ 秒）。
Unity `Player.log` 干净，外部调试器看主线程停在 `ntdll!NtWaitForAlertByThreadId`。

### 2.2 触发点

`auto_lie` 的 worker 线程按 30 Hz 直调三个托管静态谓词（`auto_lie.cpp` 里连续四行）：

```
StopWorker+0x7e2 → anti_macro_port::IsNonFiniteOpen
StopWorker+0x7e7 → anti_macro_port::IsTextCaptchaOpen
StopWorker+0x7ef → anti_macro_port::IsNonFiniteOpen
StopWorker+0x7f8 → anti_macro_port::IsOpenAntiMacro
```

### 2.3 故障链（IDB `Dumps/runtime/GameAssembly.dll.i64`，imagebase `0x7ff848c80000`）

```
UIAntiMacroNonFinite_IsInstantiated            RVA 0x9282a9
  sub_7FF84AFCD820        RVA 0x234d867   v2 = *(MethodInfo + 0x38)        // rgctx 表，惰性填
                                          v5 = sub_7FF84AFCE1D0(a1, *v2)   // 解出目标 klass
  sub_7FF84AFCE1D0        RVA 0x234e2aa
  sub_7FF84906CE00        RVA 0x3ece24
  fn @0x7ff849076eb0+0x30 RVA 0x3f6ee0
  sub_7FF849075660        RVA 0x3f571b    // Class::Init 外壳 + 全局元数据锁
  sub_7FF849078670        RVA 0x3f8769    // Class::Init 本体
  sub_7FF84905FBD0        RVA 0x3dfbd0    // ★ AV：读 0xFFFFFFFFFFFFFFFF
```

关键两段代码：

```c
// sub_7FF849075660 —— 取全局元数据锁再进 Init
result = klass[309];                       // flags
if ((result & 2) == 0) {                   // 位 2 = 已初始化；置上就整段跳过
    tid = Baselib_Thread_GetCurrentThreadId();
    if (tid == owner) ++recursion; else { CAS 抢锁 / Baselib_SystemFutex_Wait }
    sub_7FF849078670(klass, &lock);        // ← 干活
    ...解锁 / Futex_Notify...
}

// sub_7FF849078670 —— Init 本体
klass[309] = v3 | 0x40;                    // ★ 先置「正在初始化」位
++klass[305];
v12 = sub_7FF84905FBD0();                  // ★ 在这里炸；那 8 字节函数 = return f(*a1)
sub_7FF849078670(v12, lock);               // 递归初始化泛型定义 / 父类
...
klass[309] = ... | 2;                      // 只有走到底才置「已初始化」
```

`sub_7FF84905FBD0(a1) { return sub_7FF84907D710(*a1); }` 解引用的是 `klass->generic_class`
（`*(klass + 0x60)`，上面刚用 `if (v6)` 判过非 0）。读到 `-1` 说明**传进 Init 的 klass 本身就是垃圾**，
而它是从 `MethodInfo + 0x38` 的 rgctx 表惰性解出来的。在非泵线程上解，解出来的就是垃圾。

### 2.4 为什么会死透（不只是崩一次）

Init 在炸之前已经把 `klass[309] |= 0x40`（正在初始化）置上，位 2 永远置不上。异常被 XCat 的
`__except` 吞掉后，**再没有人清那个 0x40**。此后主线程每次碰到这个类：

1. 外壳看 `(flags & 2) == 0` → 每次都去抢全局元数据锁；
2. 进 Init 本体 → 看到 `0x40` 已置 → 立刻返回，什么都不做；
3. 位 2 永远置不上 → 下次再来一遍。

主线程就此在 `GameAssembly+0x3f5751` 一带出不来，客户端黑屏、CPU 空转。

---

## 3. 走过的三个弯路（后来的 AGENT 别再走一遍）

| 假设 | 怎么被证伪 |
|---|---|
| **① 元数据锁泄漏是病根**，归还锁就能救 | 加了定点抢修，`06:29:40.688` 1.2 s 内把锁还了回去，取证文件里 `lock: 空闲 word=0 recursion=0`——**主线程照样黑屏空转**。锁是陪葬品，不是病根 |
| **② 是一次性类初始化**，在泵上预热一次即可 | `06:39:41.792` 在泵上把三个谓词各跑一次，干干净净；**0.8 秒后** worker 调同样三个方法，四次全炸。这是每次调用都要走的路，跟第几次无关 |
| **③ 是换图/InterStage 时序问题** | 泵上成功那次（41.79）和 worker 失败那次（42.62）**处在同一个 `MainPump phase Bootstrap` 窗口**。相位一样，只有线程不一样 |

唯一站得住的变量是**线程**：泵上从没炸过，worker 上必炸。

---

## 4. 正确的修法

`x/features/auto_lie/anti_macro_port.cpp`：三个谓词**每次都投到泵上求值**，一个 job 一次算完三个，
结果缓存 150 ms 以免 30 Hz 打爆泵；泵在换图 quiesce 期拒 job 时按「没开测谎」返回。

```cpp
constexpr DWORD kPredCacheMs = 150;
struct PredSnapshot { bool open = false, text = false, nonFinite = false; };

void PredJob(void* user) {                 // 只在泵上跑
    auto* out = static_cast<PredSnapshot*>(user);
    out->open      = (gMiIsOpen && GaBase()) ? RawIsOpen() : false;
    out->text      = gMiTextIsInst ? RawTextInst() : false;
    out->nonFinite = gMiNonIsInst ? RawNonInst() : false;
}

PredSnapshot Predicates() {
    std::lock_guard<std::mutex> lk(gPredMtx);
    const DWORD now = GetTickCount();
    if (gPredAtMs && static_cast<int>(now - gPredAtMs) < static_cast<int>(kPredCacheMs))
        return gPredCache;
    PredSnapshot snap;
    if (x::runtime::main_thread::IsOnPumpThread()) PredJob(&snap);
    else if (!x::runtime::main_thread::InvokeAndWait(&PredJob, &snap, 800)) snap = PredSnapshot{};
    gPredAtMs = now;
    gPredCache = snap;
    return snap;
}
```

副作用是好的：每轮 tick 从 4 次托管调用降到最多 1 个泵 job。

### 配套（防御纵深，但都**不是**根治手段）

| 机制 | 位置 | 作用 |
|---|---|---|
| 归还屏障 `ReturnLeakedMetadataLock` | 所有碰 il2cpp 的 `__except` 块 | 异常吞掉后立刻归还本线程漏掉的元数据锁 |
| 定点抢修 `RepairAfterExceptionIfStillHeld` | `x/runtime/il2cpp_metadata_lock.cpp` | 持锁时抛异常后 1.2 s 仍没还 → 强制归还 |
| pump-stall 兜底 | `hang_autopsy` | 主泵僵死 12 s 且锁挂在非泵线程 → 强制归还 |

**这三条都救不回被打断的类初始化**，只能把「漏锁」这一类问题压住。别把它们当成可以放心 off-pump 的理由。

---

## 5. 排障手册

| 工具 | 位置 | 说明 |
|---|---|---|
| `il2cpp_fault_probe` | `x/runtime/il2cpp_fault_probe.cpp` | VEH 抓 il2cpp 内部异常；持锁时抛的一律详记 32 帧深栈。VEH 里只写无锁 ring buffer，格式化/落盘交给 flusher 线程。杀手锏文件：`state\no_fault_probe` |
| `hang_autopsy` | `x/features/.../hang_autopsy` | 主泵静默 12 s → dump 全部线程栈 + 模块表 + 元数据锁状态到 `bin\XCat_data\logs\hang\` |
| `symbolize_hang.py` | `scripts\symbolize_hang.py` | `python scripts\symbolize_hang.py <hang文件>` 或 `... x --rva <rva...>` 用 `xcat.pdb` 反查符号 |

典型判读：

- 日志出现 `★持锁时抛异常★` → 有人在非泵线程上碰了托管代码，栈里 `xcat.dll` 那几帧就是元凶，直接 `symbolize_hang.py x --rva` 反查。
- 取证文件里 `lock: 空闲` 但主线程仍在 `GameAssembly+0x3f5xxx` 空转 → 就是本文这个「初始化被打断」的坑，别再去查锁。
- `Il2Cpp skip runtimeClassInit off-pump` 是**正常**的守卫命中，不是错误。

### 已验证的修复效果

`06:48–06:53` 一整段：连过 **6 张图**（`104000200→100→200→300→400→104010000`），全部 0.6 s 内落
`play_ready`；`Il2cppFault` **0 次**、`Il2cppLock` 告警 **0 次**、hang 取证 **0 份**。对照修前同样时长
必现 4 次持锁异常 + 黑屏。

---

## 6. `anti_macro_port` 全模块收口结果

同日把整个 port 审了一遍，托管调用现在**全部**只在泵上执行：

| 调用点 | 入口 | 走法 |
|---|---|---|
| `IsOpenAntiMacro` / `IsTextCaptchaOpen` / `IsNonFiniteOpen` | `Predicates()` → `PredJob` | 泵，一 job 算完，缓存 150 ms |
| `GetTextCaptcha` / `GetNonFinite`（`GetAntiMacro` 静态取实例） | 同上，搭 `PredJob` 顺风车 | 泵，不额外占 job |
| `ReadNonFiniteTargetRect`（`Component.get_transform`） | `RectJob` | 泵，一次一 job |
| `DumpTextCaptchaImage` / `SubmitTextCaptchaAnswer`（`EncodeToPNG` / `set_text` / `OnOk`） | `MainJob` | 泵（本来就是） |
| `MapWinCursorBatch`（面板仿射 / `TryGetWinCursorPos`） | `MainJob` | 泵（本来就是） |

`ReadNonFiniteTickFrame` / `ReadMouseSampleCount` / `ReadNonFiniteIsResultRecv` /
`PeekNonFiniteMouseList` 是**纯字段读**，按铁律允许留在 worker 上。

### 仍未收口

- 其它 feature 是否存在 off-pump 触发托管代码的路径，**尚未全仓审计**。
- 真实测谎 UI（TextCaptcha / NonFinite）弹出后的端到端行为**未经实机验证**——收口后只做过
  静态审计与编译，几轮实机日志里都没有真弹过题。
