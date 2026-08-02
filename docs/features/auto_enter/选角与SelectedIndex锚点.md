# auto_enter · 选角与 SelectedIndex 锚点（2026-08-02）

> **状态**：✅ IDA 实锤（`UILoginCharacter+0x168` = 当前选中角色 index）  
> **产品**：经典版 TWMS · **不是**枫星  
> **IDB**：`GameAssembly.dll.i64` · imagebase `0x7FFB16B40000`  
> **源码**：`x/features/auto_enter/auto_enter.cpp`（`kOffCharSelectedIndex`）  
> **总览**：[`模块设计.md`](模块设计.md)

---

## 0. 要回答的问题

1. 自动选角是不是「鼠标点一下」？→ **否**，主线程直接调托管 UI handler。  
2. `+0x168` 是不是「当前选中槽 index」？→ **是**（本页证据）。  
3. 为何可跳过 `SelectCharacter`？→ 官方入口也会比对同一字段；已选中时再 Select 只重播动画。

---

## 1. 方法 / 字段对照

基址 `0x7FFB16B40000`；RVA = VA − base。

| 语义 | RVA | VA（本 IDB） | IDA 名（已重命名） |
|------|-----|--------------|-------------------|
| `get_SelectedIndex` | `0xA7D140` | `0x7FFB7549D140` | `UILoginCharacter_get_SelectedIndex` |
| `set_SelectedIndex` | `0xA7D150` | `0x7FFB7549D150` | `UILoginCharacter_set_SelectedIndex` |
| `SelectCharacter` | `0xA7F170` | `0x7FFB7549F170` | `UILoginCharacter_SelectCharacter` |
| `OnClickButtonSelect` | `0xA805B0` | `0x7FFB754A05B0` | `UILoginCharacter_OnClickButtonSelect` |
| `GetAvatarCount` | `0xA8ADC0` | — | — |
| `IsSlotEnable` | `0xA86820` | — | — |

> **2026-08-03 客户端更新**：上表已换新 RVA；字段 `+0x168/+0x170/+0x1A8` 未变。完整对照见 [`RVA重锚_20260803.md`](RVA重锚_20260803.md)。  
> 旧 IDB imagebase `0x7FFB16B40000` / 旧 RVA（`A7DFD0`/`A80000` 等）作废。

配套布局：

| 字段 | TW 偏移 | 备注 |
|------|---------|------|
| **SelectedIndex** | **`+0x168`** | `int32`；property backing |
| AvatarList | `+0x170` | `List<AvatarData>`；紧挨 SelectedIndex |
| SlotCount | `+0x1A8` | — |

CMS 对照：AvatarList 在 CMS 为 `+0x178`（TW 少 `0x8`）；SelectedIndex 以本 IDB 读写为准，勿硬套 CMS。

---

## 2. Hex-Rays / 反汇编证据

### 2.1 getter / setter（无混淆）

```c
// RVA A7DFD0 — get_SelectedIndex
__int64 __fastcall UILoginCharacter_get_SelectedIndex(__int64 this)
{
  return *(unsigned int *)(this + 360);  // 360 == 0x168
}

// RVA A7DFE0 — set_SelectedIndex
void __fastcall UILoginCharacter_set_SelectedIndex(__int64 this, int value)
{
  *(_DWORD *)(this + 360) = value;
}
```

反汇编等价：

```
mov eax, [rcx+168h]     ; getter
mov [rcx+168h], edx     ; setter
```

### 2.2 SelectCharacter 入口比对（CFF 壳内叶子）

`SelectCharacter` 序言：`mov r14d, edx`（index 入参）→ 后续：

```
cmp [rdi+168h], r14d    ; VA 0x7FFB175C08CF
setz al
...                     ; 结合 bool 参数做早退 / 继续选中动画
```

含义：当前选中 == 目标 index 时，官方也会走「无需再切槽」分支。我们 skip Select 与此对齐。

同函数内多次访问 `AvatarList`：

```
mov rax, [rdi+170h]
mov rcx, [rdi+170h]
```

### 2.3 GetSelectedCharacter

壳后真实路径读取：

```
mov edx, [rsi+168h]     ; VA 0x7FFB175C45CB
; 再与列表长度 / 哨兵比较，取 AvatarList[index]
```

### 2.4 IsSelectedIndex

```c
v2 = *(_DWORD *)(a1 + 360) == a2;  // this+0x168 == index ?
```

### 2.5 邻域其它读点（旁证）

下列函数均 `mov …, [reg+168h]`，落在 Char UI 同段（Update / Refresh 类）：

`…C3D95`、`…C5133`、`…C57D0`、`…C5AE6`、`…C5FCC`、`…C61BB`、`…C63D5`、`…C6D58`（含 `UpdateInfo` 路径）。

写点除 setter 外：`ResetSelectedIndex` 写 `*(this+360)`（常量经运行时解密，常见为哨兵 / −1 类初值）。

---

## 3. 实现映射（xcat）

| 常量 / 行为 | 值 / 位置 |
|-------------|-----------|
| `kOffCharSelectedIndex` | `0x168` |
| 读取时机 | `RefreshSnap` **主线程**写入 `UiSnap.charSelectedIndex` |
| 合法范围 | `0..31`；否则快照为 `-1`（未知 → 强制 Select） |
| PickChar | `curSel == index` → 日志 `already selected … (skip Select, confirm only)` → `ConfirmChar` |
| Confirm 重试 | 已选中则只重 Confirm；未知/不一致回 `PickChar` |

**禁止**：Worker 线程直接 `ReadI32(charUi, +0x168)`（托管堆 GC 竞态）。

时序常量（与假 Done 相关）：

| 常量 | 值 | 作用 |
|------|-----|------|
| `kCharReadySettleMs` | 700 | avatars 就绪后再选 |
| `kAfterSelectCharMs` | 400 | Select → Confirm 间隔 |
| `kCharConfirmRetryMs` | 1500 | Confirm 重试间隔 |
| `kMaxCharConfirmAttempts` | 4 | 上限 |

Done 条件：`!charUi` 或 `world::IsPlayReady()`——**不是** Job `ok=1`。

---

## 4. 验收日志

期望（已默认选中时）：

```
PickChar already selected index=0 slot=1 (skip Select, confirm only)
ConfirmChar click attempt=1 index=0
left char UI after confirm — Done
```

需强制切换槽时：

```
PickChar Select index=1 slot=2 count=… (wasSelected=0)
… WaitCharArmed …
ConfirmChar click attempt=1 index=1
left char UI after confirm — Done
```

回归：`wasSelected` 长期 `-1`（快照失败）或 skip 后 Confirm 空转 → 查泵 / UI 生命周期，勿先改偏移。

---

## 5. 复核清单（客户端更新时）

1. 打开当前 `GameAssembly.dll.i64`，确认 imagebase。  
2. 跳转 RVA `A7DFD0`：是否仍 `mov eax,[rcx+168h]`。  
3. `SelectCharacter`（`A80000`）内是否仍有 `cmp [reg+168h], <index>`。  
4. `GetSelectedCharacter` 是否仍从 `+168h` 取槽再索引 `+170h` 列表。  
5. 任一失败 → 重找 getter 叶子，更新 `kOffCharSelectedIndex` 与本文。

---

## 6. 非目标

- 不还原完整 CFF 控制流图。  
- 不调用 `TriggerEnterChannel` / 不改 GA `.text`。  
- 不把本仓偏移套到枫星 / MapleStory Worlds。
