# -*- coding: utf-8 -*-
"""Rebuild x/features/fly as: thin public API + full verified impl + flush hook."""
from pathlib import Path

ROOT = Path(r"c:\Users\kras\Desktop\xcat_for_TWMS")
FLY = ROOT / "x" / "features" / "fly"
DOC = ROOT / "docs" / "features" / "fly"
FLY.mkdir(parents=True, exist_ok=True)
DOC.mkdir(parents=True, exist_ok=True)

flydll = (ROOT / "Dumps/runtime/FlyDll/FlyDll.cpp").read_text(encoding="utf-8", errors="replace")
hook = (ROOT / "Dumps/runtime/MovePathFlushHook/MovePathFlushHook.cpp").read_text(
    encoding="utf-8", errors="replace"
)

(FLY / "fly.h").write_text(
    """#pragma once
#include <Windows.h>

namespace x {
namespace features {
namespace fly {

void Init();
void Shutdown();
void SetDesired(bool on);
bool IsDesired();
bool IsEnabled();
float GetSpeed();
void SetSpeed(float v);
void TickRealtime();
bool PollFlyHotkey();
void ToggleFly();
void ForceRebind();
void PreferCameraBind();

}  // namespace fly
}  // namespace features
}  // namespace x
""",
    encoding="utf-8",
)

# fly_impl.cpp = FlyDll with anonymous namespace kept, DllMain removed, export hooks for fly.cpp
impl = flydll
# Rename product strings
impl = impl.replace("FlyDll.dll", "TwmsFly.dll (fly feature)")
impl = impl.replace("FlyDll DllMain", "TwmsFly impl ready")
impl = impl.replace("xcat_fly_loaded.txt", "xcat_twms_fly_loaded.txt")
# Remove DllMain — fly.cpp owns it
idx = impl.find("BOOL APIENTRY DllMain")
if idx < 0:
    raise SystemExit("DllMain not found")
# Keep closing of anonymous namespace before DllMain
impl = impl[:idx].rstrip() + "\n\n" 
# Ensure namespace still closed
if "}  // namespace" not in impl[-80:]:
    impl += "\n}  // namespace\n"

# Add declarations callable from fly.cpp / flush via a small bridge header
bridge = r'''
#pragma once
// Internal bridge between fly.cpp (public) and fly_impl.cpp / fly_flush_hook.cpp
#include <Windows.h>

namespace twms_fly_impl {
void OpenLogs();
void Log(const char* fmt, ...);
bool BindApis();
bool ResolveActor(bool forceRescan);
void PollF6();
void PollF7Rebind();
void PollF8PreferCamera();
void TickFly();
void ArmFly(bool on);
bool IsFlyOn();
bool IsSessionDead();
float GetFlySpeed();
void SetFlySpeed(float v);
const char* ResolveHow();
void* LocalUserPtr();
void* TransformPtr();
DWORD& TickCountRef();
}

namespace twms_fly_flush {
bool Install();
void Heartbeat();
}
'''
(FLY / "fly_bridge.h").write_text(bridge, encoding="utf-8")

# Patch impl: wrap key functions into twms_fly_impl namespace aliases at end
# Easiest: change `namespace {` to stay, and at end of file add:
aliases = r'''
namespace twms_fly_impl {
void OpenLogs() { ::OpenLogs(); }
void Log(const char* fmt, ...) {
    // forward via vsnprintf into existing Log — call C-style
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ::Log("%s", buf);
}
bool BindApis() { return ::BindApis(); }
bool ResolveActor(bool forceRescan) { return ::ResolveActor(forceRescan); }
void PollF6() { ::PollF6(); }
void PollF7Rebind() { ::PollF7Rebind(); }
void PollF8PreferCamera() { ::PollF8PreferCamera(); }
void TickFly() { ::TickFly(); }
void ArmFly(bool on) { ::ArmFly(on); }
bool IsFlyOn() { return gFlyOn; }
bool IsSessionDead() { return gSessionDead; }
float GetFlySpeed() { return gFlySpeed; }
void SetFlySpeed(float v) {
    if (v < 0.05f) v = 0.05f;
    if (v > 4.f) v = 4.f;
    gFlySpeed = v;
}
const char* ResolveHow() { return gResolveHow; }
void* LocalUserPtr() { return gLocalUser; }
void* TransformPtr() { return gTransform; }
DWORD& TickCountRef() { return gTickCount; }
}
'''
# Problem: anonymous namespace symbols aren't ::OpenLogs from outside.
# Better approach: rename `namespace {` to `namespace twms_fly_impl {` throughout impl.

impl2 = flydll
impl2 = impl2.replace("FlyDll.dll", "TwmsFly.dll")
impl2 = impl2.replace("// FlyDll.dll", "// TwmsFly / x/features/fly")
impl2 = impl2.replace("xcat_fly_loaded.txt", "xcat_twms_fly_loaded.txt")
impl2 = impl2.replace("FlyDll DllMain", "TwmsFly")
impl2 = impl2.replace("namespace {\n", "namespace twms_fly_impl {\n")
impl2 = impl2.replace("}  // namespace\n", "}  // namespace twms_fly_impl\n")
# Remove DllMain and FlyThread — move to fly.cpp
# Find FlyThread and DllMain
i_thread = impl2.find("DWORD WINAPI FlyThread")
i_dll = impl2.find("BOOL APIENTRY DllMain")
if i_thread < 0 or i_dll < 0:
    raise SystemExit(f"markers missing thread={i_thread} dll={i_dll}")
# Keep everything before FlyThread; close namespace
head = impl2[:i_thread].rstrip()
# Add thin helpers for public API
head += r'''

bool IsFlyOn() { return gFlyOn; }
bool IsSessionDead() { return gSessionDead; }
float GetFlySpeed() { return gFlySpeed; }
void SetFlySpeed(float v) {
    if (v < 0.05f) v = 0.05f;
    if (v > 4.f) v = 4.f;
    gFlySpeed = v;
}
const char* ResolveHow() { return gResolveHow; }
void* LocalUserPtr() { return gLocalUser; }
void* TransformPtr() { return gTransform; }
DWORD& TickCountRef() { return gTickCount; }

}  // namespace twms_fly_impl
'''
(FLY / "fly_impl.cpp").write_text(head, encoding="utf-8")
print("fly_impl.cpp", len(head))

# Update bridge to match
(FLY / "fly_bridge.h").write_text(
    """#pragma once
#include <Windows.h>

namespace twms_fly_impl {
void OpenLogs();
void Log(const char* fmt, ...);
bool BindApis();
bool ResolveActor(bool forceRescan);
void PollF6();
void PollF7Rebind();
void PollF8PreferCamera();
void TickFly();
void ArmFly(bool on);
bool IsFlyOn();
bool IsSessionDead();
float GetFlySpeed();
void SetFlySpeed(float v);
const char* ResolveHow();
void* LocalUserPtr();
void* TransformPtr();
DWORD& TickCountRef();
}

namespace twms_fly_flush {
bool Install();
void Heartbeat();
}
""",
    encoding="utf-8",
)

# fly_flush_hook.cpp — adapt MovePathFlushHook: rename namespace, remove DllMain/Worker
h = hook
h = h.replace("namespace {\n", "namespace twms_fly_flush {\n")
# Fix closing comment
h = h.replace("}  // namespace\n", "}  // namespace twms_fly_flush\n")
# Cut Worker and DllMain
iw = h.find("DWORD WINAPI Worker")
idm = h.find("BOOL APIENTRY DllMain")
if iw < 0:
    raise SystemExit("Worker not found")
h = h[:iw].rstrip() + "\n\n"
# Rename InstallHooks -> used as Install; expose Install/Heartbeat
h += r'''
bool Install() {
    OpenLogs();
    InitializeCriticalSection(&gCs);
    gSilent = EnvOn("FLUSH_HOOK_SILENT");
    gForceBfly = EnvOn("FLUSH_FORCE_BFLY");
    gAutoBfly = EnvOn("FLUSH_AUTO_BFLY");
    gFixWings = true;
    {
        char buf[8]{};
        if (GetEnvironmentVariableA("FLUSH_FIX_WINGS", buf, sizeof(buf)) > 0)
            gFixWings = !(buf[0] == '0' && buf[1] == '\0');
    }
    gBaseline = EnvOn("FLUSH_BASELINE");
    if (gBaseline) {
        gAutoBfly = false;
        gForceBfly = false;
    }
    // v1.3 defaults already in Hook_Flush
    // Ensure noAuto path: gNoAutoBfly may not exist in v1.3 — check
    Log("TwmsFly flush-hook Install forceBfly=%d baseline=%d", gForceBfly ? 1 : 0, gBaseline ? 1 : 0);
    for (int i = 0; i < 100 && !ModuleBase(L"GameAssembly.dll"); ++i) Sleep(100);
    return InstallHooks();
}

void Heartbeat() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (last && now - last < 5000) return;
    last = now;
    if (gHits > 0) Log("flush heartbeat hits=%ld logged=%ld", (long)gHits, (long)gLogged);
}

}  // namespace twms_fly_flush
'''
# Fix v1.3: may still have gNoAutoBfly in older — check generated file for symbols
# The hook file was updated to gAutoBfly — read if Hook uses gNoAutoBfly
if "gNoAutoBfly" in h and "bool gNoAutoBfly" not in h:
    # still referenced from old? 
    pass
# Hook file on disk is v1.3 with gAutoBfly — but Install() above assumes gAutoBfly exists
# Original hook Worker set gNoAutoBfly — our current MovePathFlushHook has gAutoBfly
# The extracted h still has whatever is in MovePathFlushHook.cpp

# Fix beacon message
h = h.replace("MovePathFlushHook v1.3 loaded", "TwmsFly flush-hook v1.3 loaded")
h = h.replace("MovePathFlushHook v1.2 loaded", "TwmsFly flush-hook v1.3 loaded")

# Remove duplicate OpenLogs call issues — InstallHooks also logs
# Need gAutoBfly in hook file — verify
if "gAutoBfly" not in h and "gNoAutoBfly" in h:
    # older API
    h = h.replace("gAutoBfly = EnvOn(\"FLUSH_AUTO_BFLY\");", "gNoAutoBfly = !EnvOn(\"FLUSH_AUTO_BFLY\");")
    h = h.replace("gAutoBfly = false;", "gNoAutoBfly = true;")

(FLY / "fly_flush_hook.cpp").write_text(h, encoding="utf-8")
print("fly_flush_hook.cpp written", len(h))

# fly.cpp — public API + thread + DllMain
(FLY / "fly.cpp").write_text(
    r'''#include "fly.h"
#include "fly_bridge.h"

#include <timeapi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace fly {

void Init() {
    twms_fly_impl::OpenLogs();
    twms_fly_impl::Log("TwmsFly Init pid=%lu", GetCurrentProcessId());
}
void Shutdown() {}

void SetDesired(bool on) {
    if (on != twms_fly_impl::IsFlyOn()) twms_fly_impl::ArmFly(on);
}
bool IsDesired() { return twms_fly_impl::IsFlyOn(); }
bool IsEnabled() { return twms_fly_impl::IsFlyOn() && !twms_fly_impl::IsSessionDead(); }
float GetSpeed() { return twms_fly_impl::GetFlySpeed(); }
void SetSpeed(float v) { twms_fly_impl::SetFlySpeed(v); }
void TickRealtime() {
    if (twms_fly_impl::IsFlyOn()) twms_fly_impl::TickFly();
}
bool PollFlyHotkey() {
    const bool before = twms_fly_impl::IsFlyOn();
    twms_fly_impl::PollF6();
    return before != twms_fly_impl::IsFlyOn();
}
void ToggleFly() { twms_fly_impl::ArmFly(!twms_fly_impl::IsFlyOn()); }
void ForceRebind() { twms_fly_impl::PollF7Rebind(); }
void PreferCameraBind() { twms_fly_impl::PollF8PreferCamera(); }

}  // namespace fly
}  // namespace features
}  // namespace x

static DWORD WINAPI FlyThread(LPVOID) {
    timeBeginPeriod(1);
    Beep(880, 120);
    Beep(1175, 120);
    twms_fly_impl::Log("TwmsFly worker start");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll"); ++i) Sleep(50);

    if (!twms_fly_flush::Install()) {
        twms_fly_impl::Log("Flush hook install failed — fly continues without UserMove patch");
    }

    if (!twms_fly_impl::BindApis()) {
        twms_fly_impl::Log("BindApis failed");
        Beep(400, 400);
        timeEndPeriod(1);
        return 1;
    }
    twms_fly_impl::ResolveActor(false);
    twms_fly_impl::Log("ready: F6 fly, F7 rebind, F8 cam. how=%s lu=%p",
                       twms_fly_impl::ResolveHow(), twms_fly_impl::LocalUserPtr());

    LARGE_INTEGER freq{}, last{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    DWORD lastHb = GetTickCount();
    for (;;) {
        twms_fly_impl::PollF6();
        twms_fly_impl::PollF7Rebind();
        twms_fly_impl::PollF8PreferCamera();
        twms_fly_flush::Heartbeat();
        if (twms_fly_impl::IsFlyOn()) twms_fly_impl::TickFly();

        const DWORD nowMs = GetTickCount();
        if (nowMs - lastHb >= 5000) {
            lastHb = nowMs;
            ++twms_fly_impl::TickCountRef();
            twms_fly_impl::Log("heartbeat n=%lu fly=%d dead=%d how=%s tf=%p",
                               twms_fly_impl::TickCountRef(), twms_fly_impl::IsFlyOn() ? 1 : 0,
                               twms_fly_impl::IsSessionDead() ? 1 : 0, twms_fly_impl::ResolveHow(),
                               twms_fly_impl::TransformPtr());
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        constexpr double kDt = 1.0 / 60.0;
        if (elapsed < kDt) {
            const DWORD ms = static_cast<DWORD>((kDt - elapsed) * 1000.0);
            if (ms > 0) Sleep(ms);
        }
        QueryPerformanceCounter(&last);
    }
}

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        x::features::fly::Init();
        HANDLE th = CreateThread(nullptr, 0, FlyThread, nullptr, 0, nullptr);
        if (th) {
            twms_fly_impl::Log("CreateThread ok");
            CloseHandle(th);
        } else {
            twms_fly_impl::Log("CreateThread FAILED err=%lu", GetLastError());
        }
    }
    return TRUE;
}
''',
    encoding="utf-8",
)

# Delete broken split files that confuse build
for name in [
    "fly_state.cpp",
    "fly_log.cpp",
    "fly_physics.cpp",
    "fly_resolve.cpp",
    "fly_motion.cpp",
    "fly_hotkey.cpp",
    "fly_internal.h",
]:
    p = FLY / name
    if p.exists():
        p.unlink()
        print("removed", name)

(FLY / "build.bat").write_text(
    r'''@echo off
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo vcvars64.bat not found
  exit /b 1
)
call "%VCVARS%"
cd /d "%~dp0"
if not exist "..\..\..\Dumps\runtime\out_bin" mkdir "..\..\..\Dumps\runtime\out_bin"
cl /nologo /O2 /LD /EHsc /W3 /DUNICODE /D_UNICODE /I. ^
  fly.cpp fly_impl.cpp fly_flush_hook.cpp ^
  /Fe:..\..\..\Dumps\runtime\out_bin\TwmsFly.dll ^
  /link /DLL /MACHINE:X64 winmm.lib Psapi.lib User32.lib
if errorlevel 1 exit /b 1
echo.
echo Built: %~dp0..\..\..\Dumps\runtime\out_bin\TwmsFly.dll
dir "%~dp0..\..\..\Dumps\runtime\out_bin\TwmsFly.dll"
''',
    encoding="utf-8",
)

(FLY / "README.md").write_text(
    """# fly · Classic TWMS F6

对齐枫星 `x/features/fly/`：本目录即 F6 feature 源码根。

| 文件 | 职责 |
|---|---|
| `fly.h` / `fly.cpp` | 公开 API、DllMain、60Hz worker |
| `fly_impl.cpp` | 坐标/物理/解析/热键（原 FlyDll） |
| `fly_flush_hook.cpp` | `MovePath.Flush` E9（原 MovePathFlushHook） |
| `fly_bridge.h` | impl ↔ flush ↔ 公开层桥接 |

## 构建 / 注入
```bat
x\\features\\fly\\build.bat
```
产物：`Dumps\\runtime\\out_bin\\TwmsFly.dll`（**只注这一支**）

设计：[`docs/features/fly/模块设计.md`](../../../docs/features/fly/模块设计.md)
""",
    encoding="utf-8",
)

(DOC / "模块设计.md").write_text(
    """# fly Feature 模块设计（Classic TWMS）

> **状态**：🚧 联调中（空中 Normal 伪装 v1.6）  
> **源码根**：`x/features/fly/`  
> **产物**：`Dumps/runtime/out_bin/TwmsFly.dll`  
> **对照目录**：枫星 `xcat_for_fengxing/x/features/fly/`（组织同构；协议不同）

## 目录

```
x/features/fly/
├── fly.h / fly.cpp           # 公开 API + DllMain + worker
├── fly_bridge.h              # 内部桥接
├── fly_impl.cpp              # F6 / 鼠标 / Ap·CurPos / LocalUser
├── fly_flush_hook.cpp        # MovePath.Flush hook
└── build.bat
```

## 职责
- F6 鼠标飞（60Hz worker）
- 清 foothold；禁 Wings；`forcedFlush` 逼自然 UserMove
- Hook Flush：默认不 bFly；Wings Attr→Normal

## 与枫星差异
枫星 = 客户端位置权威；本项目 = UserMove 校验。基线见 `Dumps/runtime/BASELINE_JUMP_FALL.md`。
""",
    encoding="utf-8",
)

# Update features README
idx = ROOT / "docs/features/README.md"
t = idx.read_text(encoding="utf-8")
if "fly/模块设计" not in t:
    t = t.replace(
        "## 3. 有模块设计文档的 Feature\n\n（payload feature 尚未迁入本仓；落地后在此登记。）\n",
        "## 3. 有模块设计文档的 Feature\n\n"
        "| 文档 | 主题 |\n|------|------|\n"
        "| [`fly/模块设计.md`](fly/模块设计.md) | F6 鼠标飞 + MovePath.Flush（`x/features/fly` → `TwmsFly.dll`） |\n\n",
    )
    t = t.replace(
        "| `Dumps/` | dump.cs / opcode / Msc.Security 笔记 |\n",
        "| `Dumps/` | dump.cs / opcode / Msc.Security 笔记 |\n"
        "| `x/features/fly/` | F6 feature 源码；产物 `Dumps/runtime/out_bin/TwmsFly.dll` |\n",
    )
    idx.write_text(t, encoding="utf-8")

print("OK")
for p in sorted(FLY.glob("*")):
    print(p.name, p.stat().st_size)
