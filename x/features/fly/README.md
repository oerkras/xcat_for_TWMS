# fly · Classic TWMS F6

对齐枫星 `x/features/fly/`：本目录即 F6 feature 源码根。

| 文件 | 职责 |
|---|---|
| `fly.h` / `fly.cpp` | 公开 API、DllMain、60Hz worker |
| `fly_impl.cpp` | 坐标/物理/解析/热键（数据面） |
| `fly_bridge.h` | impl ↔ 公开层桥接 |
| `fly_flush_hook.cpp` | **已拆除**（GRAP 禁 INLINE HOOK）；历史源码见 `.archived` |

## 构建 / 注入
```bat
x\features\fly\build.bat
```
产物：`Dumps\runtime\out_bin\TwmsFly.dll`（**只注这一支**；无 E9）

设计：[`docs/features/fly/模块设计.md`](../../../docs/features/fly/模块设计.md)  
安全：[`docs/features/security/GRAP与枫星对齐.md`](../../../docs/features/security/GRAP与枫星对齐.md) §4.1
