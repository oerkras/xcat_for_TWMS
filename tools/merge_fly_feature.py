# -*- coding: utf-8 -*-
"""Merge FlyDll + MovePathFlushHook into x/features/fly (fengxing-style)."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(r"c:\Users\kras\Desktop\xcat_for_TWMS")
FLY = ROOT / "x" / "features" / "fly"
DOC = ROOT / "docs" / "features" / "fly"
OUT = ROOT / "Dumps" / "runtime" / "out_bin"
FLY.mkdir(parents=True, exist_ok=True)
DOC.mkdir(parents=True, exist_ok=True)

fly_src = (ROOT / "Dumps/runtime/FlyDll/FlyDll.cpp").read_text(encoding="utf-8", errors="replace")
hook_src = (ROOT / "Dumps/runtime/MovePathFlushHook/MovePathFlushHook.cpp").read_text(
    encoding="utf-8", errors="replace"
)
fly_lines = fly_src.splitlines(True)


def sl(a: int, b: int) -> str:
    return "".join(fly_lines[a - 1 : b])


# --- fly.h ---
(FLY / "fly.h").write_text(
    """#pragma once

#include <Windows.h>

// Classic TWMS · F6 mouse-fly feature
// Source: x/features/fly/  |  Inject: Dumps/runtime/out_bin/TwmsFly.dll

namespace x::features::fly {

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

}  // namespace x::features::fly
""",
    encoding="utf-8",
)

# Strip anonymous namespace from body chunks and wrap later
def strip_anon_open(s: str) -> str:
    return s.replace("namespace {\n", "").replace("}  // namespace\n", "")


# Constants + types from FlyDll lines 41-180 approx — rebuild fly_internal from source header
header_consts = sl(41, 183)  # through typedefs before globals
# Fix: FlyDll has constexpr then struct Vector3 — read 41-210

# Better: generate fly_internal.h as include of shared decls; put globals in fly_state.cpp

(FLY / "fly_internal.h").write_text(
    r'''#pragma once

#include "fly.h"

#include <Windows.h>
#include <Psapi.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "winmm.lib")

namespace x::features::fly {
namespace detail {

// --- RVAs (runtime GameAssembly) ---
constexpr uint32_t kRvaCamGetMain = 0x4DDF440;
constexpr uint32_t kRvaCompGetTransform = 0x4E496A0;
constexpr uint32_t kRvaCompGetGo = 0x4E48E30;
constexpr uint32_t kRvaGoGetTransform = 0x4E4EB40;
constexpr uint32_t kRvaGoFindWithTag = 0x4E4D9B0;
constexpr uint32_t kRvaGoFind = 0x4E50410;
constexpr uint32_t kRvaObjGetName = 0x4E5D8B0;
constexpr uint32_t kRvaObjGetCachedPtr = 0x4E5C820;
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E2F0B0;
constexpr uint32_t kRvaTfGetPos = 0x4E64110;
constexpr uint32_t kRvaTfSetPos = 0x4E64230;
constexpr uint32_t kRvaTfGetPosInjected = 0x4E641F0;
constexpr uint32_t kRvaTfSetPosInjected = 0x4E642E0;
constexpr uint32_t kFlushRvaDefault = 0x1198DC0;

constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kOffVisPos = 0x64;
constexpr size_t kOffVisMirror = 0x6C;
constexpr size_t kOffBasePosA = 0xFC;
constexpr size_t kOffBasePosB = 0x108;
constexpr size_t kOffLocalUserAvatar = 0x1B0;
constexpr size_t kOffLocalUserCam = 0x1C0;
constexpr size_t kOffLogicalPos = 0x240;
constexpr size_t kOffLogicalPos2 = 0x248;
constexpr size_t kOffLocalUserPos3 = 0x2E0;
constexpr size_t kOffVecCtrl = 0x50;
constexpr size_t kOffVcCurFh = 0x28;
constexpr size_t kOffVcLastFh = 0x30;
constexpr size_t kOffVcFallStart = 0x38;
constexpr size_t kOffVcInputX = 0x50;
constexpr size_t kOffVcInputY = 0x54;
constexpr size_t kOffVcJumpNext = 0x58;
constexpr size_t kOffVcTryJumpFly = 0x59;
constexpr size_t kOffVcFallDownValid = 0x60;
constexpr size_t kOffVcFallDownFh = 0x68;
constexpr size_t kOffVcWingsNext = 0x70;
constexpr size_t kOffVcWingsNow = 0x71;
constexpr size_t kOffVcWingsPrev = 0x72;
constexpr size_t kOffVcWingsParam = 0x74;
constexpr size_t kOffVcMovePath = 0x78;
constexpr size_t kOffVcActive = 0x80;
constexpr size_t kOffVcApX = 0x98;
constexpr size_t kOffVcApY = 0xA0;
constexpr size_t kOffVcApVx = 0xA8;
constexpr size_t kOffVcApVy = 0xB0;
constexpr size_t kOffVcAplX = 0xB8;
constexpr size_t kOffVcAplY = 0xC0;
constexpr size_t kOffMpForcedFlush = 0x48;
constexpr size_t kOffMpElem = 0x30;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffElemAttr = 0x10;
constexpr size_t kOffElemX = 0x12;
constexpr size_t kOffElemY = 0x14;
constexpr size_t kOffElemVx = 0x16;
constexpr size_t kOffElemVy = 0x18;
constexpr size_t kOffElemMoveAction = 0x1A;
constexpr size_t kOffElemFh = 0x1C;
constexpr size_t kOffElemFhFall = 0x1E;
constexpr size_t kOffElemElapse = 0x20;
constexpr size_t kOffElemStat = 0x22;
constexpr size_t kOffOutPacketId = 0x20;
constexpr size_t kOffAvatarTf = 0x70;
constexpr size_t kOffAvatarV3 = 0x78;
constexpr size_t kOffAvatarV2 = 0x90;
constexpr size_t kOffGroundA = 0x60;
constexpr size_t kOffGroundB = 0x74;
constexpr size_t kOffGroundC = 0x104;
constexpr size_t kOffGroundD = 0x110;
constexpr size_t kOffGroundE = 0x148;

constexpr float kFlySpeedDefault = 0.85f;
constexpr float kFlyMouseScaleK = 0.18f;
constexpr float kFlyMaxDK = 28.f;
constexpr int kFlyScreenMickey = 120;
constexpr int kFlyMouseDeadPx = 2;
constexpr float kTickDt = 1.f / 60.f;
constexpr float kMaxVisOffAbs = 80.f;
constexpr float kApVelScale = 90.f;
constexpr float kApVelMax = 670.f;
constexpr size_t kStealFlush = 5;
constexpr int kMaxElemsSnap = 8;

struct Vector3 {
    float x, y, z;
};
struct Vector2 {
    float x, y;
};

using FnCamMain = void* (*)(void*);
using FnCompTf = void* (*)(void*, void*);
using FnCompGo = void* (*)(void*, void*);
using FnGoTf = void* (*)(void*, void*);
using FnFindTag = void* (*)(void*, void*);
using FnFind = void* (*)(void*, void*);
using FnObjName = void* (*)(void*, void*);
using FnCached = intptr_t (*)(void*, void*);
using FnFindAll = void* (*)(void*, void*);
using FnGetPos = Vector3 (*)(void*, void*);
using FnSetPos = void (*)(void*, Vector3, void*);
using FnGetInj = void (*)(intptr_t, Vector3*, void*);
using FnSetInj = void (*)(intptr_t, Vector3*, void*);
using FnStrNew = void* (*)(const char*);
using FnDomainGet = void* (*)();
using FnDomainAssemblies = void** (*)(void*, size_t*);
using FnAsmImage = void* (*)(void*);
using FnClassFromName = void* (*)(void*, const char*, const char*);
using FnClassGetType = void* (*)(void*);
using FnTypeGetObject = void* (*)(void*);
using FnObjGetClass = void* (*)(void*);
using FnFlush = uint8_t (*)(void*, void*, uint32_t, void*, void*);

template <typename T>
inline T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(gGaBase + rva);
}

inline float Clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// --- shared state ---
extern HMODULE gGA;
extern uintptr_t gGaBase;
extern FnCamMain gCamMain;
extern FnCompTf gCompTf;
extern FnCompGo gCompGo;
extern FnGoTf gGoTf;
extern FnFindTag gFindTag;
extern FnFind gFind;
extern FnObjName gObjName;
extern FnCached gCachedPtr;
extern FnFindAll gFindAll;
extern FnGetPos gGetPos;
extern FnSetPos gSetPos;
extern FnGetInj gGetInj;
extern FnSetInj gSetInj;
extern FnStrNew gStrNew;
extern FnDomainGet gDomainGet;
extern FnDomainAssemblies gDomainAssemblies;
extern FnAsmImage gAsmImage;
extern FnClassFromName gClassFromName;
extern FnClassGetType gClassGetType;
extern FnTypeGetObject gTypeGetObject;
extern FnObjGetClass gObjGetClass;

extern HANDLE gLog;
extern HANDLE gLogTemp;
extern DWORD gTickCount;
extern void* gTransform;
extern void* gLocalUser;
extern void* gLocalUserType;
extern bool gFlyOn;
extern bool gSessionDead;
extern int gFailStreak;
extern bool f6WasDown;
extern POINT gPrevMouse;
extern float gPosX, gPosY, gPosZ;
extern float gCamOffX, gCamOffY;
extern float gVisOffX, gVisOffY;
extern float gLastFlyDx, gLastFlyDy;
extern bool gHaveLastFly;
extern float gLastFlyX, gLastFlyY;
extern bool gSyncBaseCopies;
extern bool gSyncPos3;
extern float gMouseCarryX, gMouseCarryY;
extern float gFlySpeed;
extern float gRestCamOffY;
extern char gResolveHow[64];
extern char gActorName[96];

// flush hook
extern bool gFlushSilent;
extern bool gForceBfly;
extern bool gAutoBfly;
extern bool gFixWings;
extern bool gBaseline;
extern void* gFlushTarget;
extern void* gFlushTramp;
extern FnFlush gRealFlush;
extern volatile LONG gFlushHits;
extern volatile LONG gFlushLogged;

void Log(const char* fmt, ...);
void OpenLogs();
void AbortSession(const char* why);

bool PosLooksAlive(float x, float y, float z);
intptr_t NativePtr(void* managedObj);
bool ReadIl2CppString(void* str, char* out, size_t outCap);
void DescribeTf(void* tf, char* out, size_t outCap);
bool ReadPos(void* tf, float& x, float& y, float& z);
bool WritePos(void* tf, float x, float y, float z);
void WriteV2(void* obj, size_t off, float x, float y);
void WriteV3Field(void* obj, size_t off, float x, float y, float z);
void WriteBool(void* obj, size_t off, bool v);
bool ReadBool(void* obj, size_t off);
void* ReadPtr(void* obj, size_t off);
Vector2 ReadV2(void* obj, size_t off);
void WritePtr(void* obj, size_t off, void* v);
void WriteF64(void* obj, size_t off, double v);
double ReadF64(void* obj, size_t off);

void* GetVecCtrl();
void ApplyAirState();
void WriteVecCtrlAbsPos(float x, float y);
void WriteLocalUserLogical(float x, float y);
bool WriteActor(float x, float y, float z);
bool ActorStillAlive();
void NoteWriteResult(bool ok);
void DumpPhysFields(const char* tag);
void SyncAvatarVisual(float vx, float vy, float z);

bool ResolveActor(bool forceRescan);
bool BindApis();
void ResetMouse();
void ArmFly(bool on);
bool SampleMouse(float& dx, float& dy);
void TickFly();

void PollF6();
void PollF7Rebind();
void PollF8PreferCamera();

bool InstallFlushHooks();
void FlushHookTickHeartbeat();

}  // namespace detail
}  // namespace x::features::fly
''',
    encoding="utf-8",
)

print("wrote fly_internal.h")

# Strategy: keep working FlyDll as fly_legacy_body.cpp wrapped, plus flush hook —
# NO — user wants proper modules. Copy FlyDll almost whole into fly_core.cpp under namespace,
# and fly_flush_hook separately, thin fly.cpp. Then we can refine.
# For fengxing-like NAMES, split into the files by concatenating extracted bodies.

state_cpp = r'''#include "fly_internal.h"

namespace x::features::fly {
namespace detail {

HMODULE gGA = nullptr;
uintptr_t gGaBase = 0;
FnCamMain gCamMain = nullptr;
FnCompTf gCompTf = nullptr;
FnCompGo gCompGo = nullptr;
FnGoTf gGoTf = nullptr;
FnFindTag gFindTag = nullptr;
FnFind gFind = nullptr;
FnObjName gObjName = nullptr;
FnCached gCachedPtr = nullptr;
FnFindAll gFindAll = nullptr;
FnGetPos gGetPos = nullptr;
FnSetPos gSetPos = nullptr;
FnGetInj gGetInj = nullptr;
FnSetInj gSetInj = nullptr;
FnStrNew gStrNew = nullptr;
FnDomainGet gDomainGet = nullptr;
FnDomainAssemblies gDomainAssemblies = nullptr;
FnAsmImage gAsmImage = nullptr;
FnClassFromName gClassFromName = nullptr;
FnClassGetType gClassGetType = nullptr;
FnTypeGetObject gTypeGetObject = nullptr;
FnObjGetClass gObjGetClass = nullptr;

HANDLE gLog = INVALID_HANDLE_VALUE;
HANDLE gLogTemp = INVALID_HANDLE_VALUE;
DWORD gTickCount = 0;
void* gTransform = nullptr;
void* gLocalUser = nullptr;
void* gLocalUserType = nullptr;
bool gFlyOn = false;
bool gSessionDead = false;
int gFailStreak = 0;
bool f6WasDown = false;
POINT gPrevMouse{-1, -1};
float gPosX = 0, gPosY = 0, gPosZ = 0;
float gCamOffX = 0, gCamOffY = 0;
float gVisOffX = 0, gVisOffY = 0;
float gLastFlyDx = 0.f, gLastFlyDy = 0.f;
bool gHaveLastFly = false;
float gLastFlyX = 0, gLastFlyY = 0;
bool gSyncBaseCopies = false;
bool gSyncPos3 = false;
float gMouseCarryX = 0, gMouseCarryY = 0;
float gFlySpeed = kFlySpeedDefault;
float gRestCamOffY = 120.f;
char gResolveHow[64] = "none";
char gActorName[96] = "";

bool gFlushSilent = false;
bool gForceBfly = false;
bool gAutoBfly = false;
bool gFixWings = true;
bool gBaseline = false;
void* gFlushTarget = nullptr;
void* gFlushTramp = nullptr;
FnFlush gRealFlush = nullptr;
volatile LONG gFlushHits = 0;
volatile LONG gFlushLogged = 0;

}  // namespace detail
}  // namespace x::features::fly
'''
(FLY / "fly_state.cpp").write_text(state_cpp, encoding="utf-8")

# Extract body from FlyDll: from OpenLogs through end of TickFly/BindApis/Polls — wrap in namespace
# Remove forward decls and globals and DllMain

body = sl(215, 1389)  # WriteLogHandle through end of FlyThread before closing namespace
# Remove "}  // namespace" if present at end
if body.rstrip().endswith("}  // namespace"):
    body = body.rsplit("}  // namespace", 1)[0]

# Remove duplicate forward-looking pieces — body starts at WriteLogHandle
# Prepend include + namespace

# Split body further by markers inside the extracted text using line numbers relative to original:
# log: 215-278
# mem+physics: 280-622
# resolve: 624-1028
# motion: 1030-1257
# bind+hotkey+thread: 1259-1389

chunks = {
    "fly_log.cpp": (215, 278),
    "fly_physics.cpp": (280, 622),
    "fly_resolve.cpp": (624, 1028),
    "fly_motion.cpp": (1030, 1257),
    "fly_bind_hotkey.cpp": (1259, 1338),  # BindApis + F7 + F8 — F6 is in motion area? PollF6 at 1178
}

# Fix: PollF6 is in 1178-1184 inside motion range 1030-1257 — good
# BindApis 1259-1300, F7 1302, F8 1326 — put BindApis in fly.cpp, hotkeys in fly_hotkey.cpp

chunks = {
    "fly_log.cpp": (215, 278),
    "fly_physics.cpp": (280, 622),
    "fly_resolve.cpp": (624, 1028),
    "fly_motion.cpp": (1030, 1257),  # ResetMouse ArmFly PollF6 SampleMouse TickFly
    "fly_hotkey.cpp": (1302, 1338),  # F7 F8 only
}

prologue = '#include "fly_internal.h"\n\nnamespace x::features::fly {\nnamespace detail {\n\n'
epilogue = "\n}  // namespace detail\n}  // namespace x::features::fly\n"

for name, (a, b) in chunks.items():
    chunk = sl(a, b)
    # drop forward decls if any leaked
    (FLY / name).write_text(prologue + chunk + epilogue, encoding="utf-8")
    print("wrote", name, "lines", b - a + 1)

# fly.cpp: BindApis + public API + FlyThread + DllMain
bind = sl(1259, 1300)
thread = sl(1340, 1389)
# Fix FlyThread closing
fly_cpp = (
    prologue
    + bind
    + r'''
}  // namespace detail

using namespace detail;

void Init() {
    OpenLogs();
    Log("TwmsFly Init pid=%lu", GetCurrentProcessId());
}

void Shutdown() {}

void SetDesired(bool on) {
    if (on != gFlyOn) ArmFly(on);
}
bool IsDesired() { return gFlyOn; }
bool IsEnabled() { return gFlyOn && !gSessionDead; }
float GetSpeed() { return gFlySpeed; }
void SetSpeed(float v) {
    if (v < 0.05f) v = 0.05f;
    if (v > 4.f) v = 4.f;
    gFlySpeed = v;
}
void TickRealtime() {
    if (gFlyOn) TickFly();
}
bool PollFlyHotkey() {
    const bool was = f6WasDown;
    PollF6();
    return gFlyOn != was && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;  // approximate
}
void ToggleFly() { ArmFly(!gFlyOn); }
void ForceRebind() { PollF7Rebind(); }
void PreferCameraBind() { PollF8PreferCamera(); }

namespace detail {

DWORD WINAPI FlyThread(LPVOID) {
    timeBeginPeriod(1);
    Beep(880, 120);
    Beep(1175, 120);
    Log("TwmsFly worker start");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll"); ++i) Sleep(50);

    if (!InstallFlushHooks()) {
        Log("Flush hook install failed — fly continues without UserMove patch");
    }

    if (!BindApis()) {
        Log("BindApis failed");
        Beep(400, 400);
        timeEndPeriod(1);
        return 1;
    }
    ResolveActor(false);
    Log("ready: F6 fly, F7 rebind, F8 force-camera. how=%s lu=%p", gResolveHow, gLocalUser);

    LARGE_INTEGER freq{}, last{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    DWORD lastHb = GetTickCount();

    while (true) {
        PollF6();
        PollF7Rebind();
        PollF8PreferCamera();
        FlushHookTickHeartbeat();
        if (gFlyOn) TickFly();

        const DWORD nowMs = GetTickCount();
        if (nowMs - lastHb >= 5000) {
            lastHb = nowMs;
            ++gTickCount;
            Log("heartbeat n=%lu fly=%d dead=%d how=%s tf=%p", gTickCount, gFlyOn ? 1 : 0,
                gSessionDead ? 1 : 0, gResolveHow, gTransform);
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        if (elapsed < kTickDt) {
            const DWORD ms = static_cast<DWORD>((kTickDt - elapsed) * 1000.0);
            if (ms > 0) Sleep(ms);
        }
        QueryPerformanceCounter(&last);
    }
}

}  // namespace detail

}  // namespace x::features::fly

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        x::features::fly::Init();
        HANDLE th = CreateThread(nullptr, 0, x::features::fly::detail::FlyThread, nullptr, 0, nullptr);
        if (th) {
            x::features::fly::detail::Log("CreateThread ok");
            CloseHandle(th);
        } else {
            x::features::fly::detail::Log("CreateThread FAILED err=%lu", GetLastError());
        }
    }
    return TRUE;
}
'''
)
# Remove broken duplicate FlyThread from bind section — we only included BindApis
fly_cpp = (
    '#include "fly_internal.h"\n#include <timeapi.h>\n\nnamespace x::features::fly {\nnamespace detail {\n\n'
    + bind
    + "\n}  // namespace detail\n\nusing namespace detail;\n\n"
    + r'''void Init() {
    OpenLogs();
    Log("TwmsFly Init pid=%lu", GetCurrentProcessId());
}
void Shutdown() {}
void SetDesired(bool on) { if (on != gFlyOn) ArmFly(on); }
bool IsDesired() { return gFlyOn; }
bool IsEnabled() { return gFlyOn && !gSessionDead; }
float GetSpeed() { return gFlySpeed; }
void SetSpeed(float v) {
    if (v < 0.05f) v = 0.05f;
    if (v > 4.f) v = 4.f;
    gFlySpeed = v;
}
void TickRealtime() { if (gFlyOn) TickFly(); }
bool PollFlyHotkey() {
    bool before = gFlyOn;
    PollF6();
    return before != gFlyOn;
}
void ToggleFly() { ArmFly(!gFlyOn); }
void ForceRebind() { PollF7Rebind(); }
void PreferCameraBind() { PollF8PreferCamera(); }

namespace detail {

DWORD WINAPI FlyThread(LPVOID) {
    timeBeginPeriod(1);
    Beep(880, 120);
    Beep(1175, 120);
    Log("TwmsFly worker start");
    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll"); ++i) Sleep(50);
    if (!InstallFlushHooks())
        Log("Flush hook install failed — fly continues without UserMove patch");
    if (!BindApis()) {
        Log("BindApis failed");
        Beep(400, 400);
        timeEndPeriod(1);
        return 1;
    }
    ResolveActor(false);
    Log("ready: F6 fly, F7 rebind, F8 cam. how=%s lu=%p", gResolveHow, gLocalUser);
    LARGE_INTEGER freq{}, last{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    DWORD lastHb = GetTickCount();
    while (true) {
        PollF6();
        PollF7Rebind();
        PollF8PreferCamera();
        FlushHookTickHeartbeat();
        if (gFlyOn) TickFly();
        const DWORD nowMs = GetTickCount();
        if (nowMs - lastHb >= 5000) {
            lastHb = nowMs;
            ++gTickCount;
            Log("heartbeat n=%lu fly=%d dead=%d how=%s tf=%p", gTickCount, gFlyOn ? 1 : 0,
                gSessionDead ? 1 : 0, gResolveHow, gTransform);
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        if (elapsed < kTickDt) {
            DWORD ms = static_cast<DWORD>((kTickDt - elapsed) * 1000.0);
            if (ms > 0) Sleep(ms);
        }
        QueryPerformanceCounter(&last);
    }
}

}  // namespace detail
}  // namespace x::features::fly

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        x::features::fly::Init();
        HANDLE th = CreateThread(nullptr, 0, x::features::fly::detail::FlyThread, nullptr, 0, nullptr);
        if (th) {
            x::features::fly::detail::Log("CreateThread ok");
            CloseHandle(th);
        } else {
            x::features::fly::detail::Log("CreateThread FAILED err=%lu", GetLastError());
        }
    }
    return TRUE;
}
'''
)
(FLY / "fly.cpp").write_text(fly_cpp, encoding="utf-8")
print("wrote fly.cpp")

# Adapt flush hook: take MovePathFlushHook.cpp, strip DllMain/Worker, wrap namespace, call Install from fly
# Write fly_flush_hook.cpp by transforming hook source

hook = hook_src
# Remove DllMain and Worker — keep InstallHooks + Hook_Flush + helpers
# Easier: write a cleaned version based on hook file structure

# Read hook and extract from namespace { to before DWORD WINAPI Worker
h_lines = hook_src.splitlines(True)
# find "namespace {" and "DWORD WINAPI Worker"
start = None
worker = None
dllmain = None
for i, l in enumerate(h_lines):
    if l.startswith("namespace {") and start is None:
        start = i
    if "DWORD WINAPI Worker" in l:
        worker = i
    if "BOOL APIENTRY DllMain" in l:
        dllmain = i

if start is None or worker is None:
    raise SystemExit(f"hook parse fail start={start} worker={worker}")

hook_body = "".join(h_lines[start + 1 : worker])
# Replace log paths / globals that conflict — rewrite to use fly_internal symbols

flush_cpp = r'''#include "fly_internal.h"

namespace x::features::fly {
namespace detail {

constexpr wchar_t kFlushLogPrimary[] =
    L"C:\\Users\\kras\\Desktop\\xcat_for_TWMS\\Dumps\\runtime\\movepath_flush.log";
constexpr wchar_t kFlushLogTemp[] = L"C:\\Users\\kras\\AppData\\Local\\Temp\\xcat_movepath_flush.log";
constexpr wchar_t kFlushBeacon[] =
    L"C:\\Users\\kras\\AppData\\Local\\Temp\\xcat_movepath_flush_loaded.txt";
constexpr wchar_t kFlushJson[] =
    L"C:\\Users\\kras\\Desktop\\xcat_for_TWMS\\Dumps\\runtime\\movepath_flush.jsonl";

HANDLE gFlushLog = INVALID_HANDLE_VALUE;
HANDLE gFlushLogTemp = INVALID_HANDLE_VALUE;
HANDLE gFlushJson = INVALID_HANDLE_VALUE;
uint8_t gFlushStolen[32]{};
CRITICAL_SECTION gFlushCs;
bool gFlushCsInit = false;

void FlushLogLine(const char* fmt, ...) {
    char buf[2048];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int p = snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds);
    if (p < 0) p = 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + p, sizeof(buf) - p, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n += p;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;
    auto write = [](HANDLE h, const char* b, int len) {
        if (h == INVALID_HANDLE_VALUE || len <= 0) return;
        DWORD w = 0;
        WriteFile(h, b, (DWORD)len, &w, nullptr);
    };
    write(gFlushLog, buf, n);
    write(gFlushLogTemp, buf, n);
    // also mirror short line into fly.log
    Log("%s", buf);
}

void JsonLine(const char* fmt, ...) {
    if (gFlushJson == INVALID_HANDLE_VALUE) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;
    DWORD w = 0;
    WriteFile(gFlushJson, buf, (DWORD)n, &w, nullptr);
}

bool EnvOn(const char* name) {
    char buf[8]{};
    return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0 && buf[0] != '0';
}

uint32_t EnvRva(const char* name, uint32_t def) {
    char buf[32]{};
    if (GetEnvironmentVariableA(name, buf, sizeof(buf)) <= 0) return def;
    return static_cast<uint32_t>(strtoul(buf, nullptr, 0));
}

bool Rel32Fits(void* from, void* to, size_t insnLen) {
    const intptr_t rel =
        reinterpret_cast<uint8_t*>(to) - (reinterpret_cast<uint8_t*>(from) + static_cast<ptrdiff_t>(insnLen));
    return rel >= INT32_MIN && rel <= INT32_MAX;
}

bool WriteRel32Jmp(void* target, void* dest, size_t stealLen) {
    uint8_t patch[32]{};
    if (stealLen < 5 || stealLen > sizeof(patch)) return false;
    patch[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(dest) -
                                             (static_cast<uint8_t*>(target) + 5));
    memcpy(patch + 1, &rel, 4);
    for (size_t i = 5; i < stealLen; ++i) patch[i] = 0x90;
    DWORD old = 0;
    if (!VirtualProtect(target, stealLen, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(target, patch, stealLen);
    VirtualProtect(target, stealLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stealLen);
    return true;
}

bool InstallRelHook(void* target, void* detour, size_t stealLen, uint8_t* stolenOut, void** trampOut,
                    void** realOut) {
    memcpy(stolenOut, target, stealLen);
    uint8_t* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, stolenOut, stealLen);
    tramp[stealLen] = 0xE9;
    {
        const int32_t rel = static_cast<int32_t>((static_cast<uint8_t*>(target) + stealLen) -
                                                 (tramp + stealLen + 5));
        memcpy(tramp + stealLen + 1, &rel, 4);
    }
    void* jmpTo = detour;
    if (!Rel32Fits(target, detour, 5)) {
        uint8_t* farStub = tramp + 32;
        farStub[0] = 0xFF;
        farStub[1] = 0x25;
        memset(farStub + 2, 0, 4);
        const uint64_t imm = reinterpret_cast<uint64_t>(detour);
        memcpy(farStub + 6, &imm, 8);
        jmpTo = farStub;
        if (!Rel32Fits(target, jmpTo, 5)) return false;
    }
    if (!WriteRel32Jmp(target, jmpTo, stealLen)) return false;
    *trampOut = tramp;
    *realOut = tramp;
    return true;
}

uintptr_t ModuleBase(const wchar_t* name) {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    const DWORD n = needed / sizeof(HMODULE);
    wchar_t path[MAX_PATH];
    for (DWORD i = 0; i < n; ++i) {
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        if (_wcsicmp(base, name) == 0) return reinterpret_cast<uintptr_t>(mods[i]);
    }
    return 0;
}

struct OneElem {
    int attr, x, y, vx, vy, moveAction, fh, fhFall, elapse, stat;
};
struct ElemSnap {
    int size, forced, nSnap;
    OneElem e[kMaxElemsSnap];
};

const char* AttrName(int a) {
    switch (a) {
        case 0: return "Normal";
        case 1: return "Jump";
        case 2: return "Impact";
        case 3: return "Immediate";
        case 4: return "Teleport";
        case 6: return "FlashJump";
        case 10: return "StatChange";
        case 14: return "StartFalldown";
        case 15: return "FallDown";
        case 16: return "StartWings";
        case 17: return "Wings";
        case 18: return "VerticalJump";
        default: return "?";
    }
}

bool AttrInteresting(int a) {
    return a == 1 || a == 14 || a == 15 || a == 16 || a == 17 || a == 18 || a == 6;
}

int ReadElemSnapSeH(void* movePath, ElemSnap* out) {
    out->size = -1;
    out->forced = -1;
    out->nSnap = 0;
    memset(out->e, 0, sizeof(out->e));
    if (!movePath) return 0;
    __try {
        auto* mp = static_cast<uint8_t*>(movePath);
        out->forced = mp[kOffMpForced] ? 1 : 0;
        void* list = *reinterpret_cast<void**>(mp + kOffMpElem);
        if (!list) {
            out->size = 0;
            return 1;
        }
        auto* lp = static_cast<uint8_t*>(list);
        const int size = *reinterpret_cast<int*>(lp + kOffListSize);
        out->size = size;
        void* items = *reinterpret_cast<void**>(lp + kOffListItems);
        if (!items || size <= 0) return 1;
        uint8_t* arr = static_cast<uint8_t*>(items);
        const int n = size < kMaxElemsSnap ? size : kMaxElemsSnap;
        for (int i = 0; i < n; ++i) {
            void* elem = *reinterpret_cast<void**>(arr + 0x20 + static_cast<size_t>(i) * 8);
            if (!elem) continue;
            auto* e = static_cast<uint8_t*>(elem);
            OneElem& o = out->e[out->nSnap++];
            o.attr = e[kOffElemAttr];
            o.x = *reinterpret_cast<int16_t*>(e + kOffElemX);
            o.y = *reinterpret_cast<int16_t*>(e + kOffElemY);
            o.vx = *reinterpret_cast<int16_t*>(e + kOffElemVx);
            o.vy = *reinterpret_cast<int16_t*>(e + kOffElemVy);
            o.moveAction = e[kOffElemMoveAction];
            o.fh = *reinterpret_cast<int16_t*>(e + kOffElemFh);
            o.fhFall = *reinterpret_cast<int16_t*>(e + kOffElemFhFall);
            o.elapse = *reinterpret_cast<int16_t*>(e + kOffElemElapse);
            o.stat = e[kOffElemStat];
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ReadPacketIdSeH(void* outPacket) {
    if (!outPacket) return -1;
    __try {
        return *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(outPacket) + kOffOutPacketId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool SnapHasInteresting(const ElemSnap& snap) {
    for (int i = 0; i < snap.nSnap; ++i) {
        if (AttrInteresting(snap.e[i].attr)) return true;
        if (snap.e[i].vy <= -80 || snap.e[i].vy >= 80) return true;
        if (snap.e[i].fhFall != 0) return true;
    }
    return false;
}

bool ShouldLogFlush(LONG hit, uint32_t bFly, const ElemSnap& snap) {
    if (gBaseline) {
        if (hit <= 500) return true;
        if (SnapHasInteresting(snap)) return true;
        return (hit % 10) == 0;
    }
    if (hit <= 200) return true;
    if (bFly) return true;
    if (SnapHasInteresting(snap)) return true;
    if (snap.forced == 1) return true;
    return (hit % 30) == 0;
}

void FormatElems(char* buf, int bufSz, const ElemSnap& snap) {
    int p = 0;
    for (int i = 0; i < snap.nSnap && p < bufSz - 8; ++i) {
        const OneElem& o = snap.e[i];
        const int n = snprintf(
            buf + p, bufSz - p,
            " E%d[%s(%d) xy=(%d,%d) v=(%d,%d) ma=%d fh=%d fhFall=%d el=%d st=%d]", i, AttrName(o.attr),
            o.attr, o.x, o.y, o.vx, o.vy, o.moveAction, o.fh, o.fhFall, o.elapse, o.stat);
        if (n < 0) break;
        p += n;
    }
    if (p == 0 && bufSz > 0) buf[0] = 0;
}

int RewriteWingsElemsSeH(void* movePath) {
    if (!movePath || !gFixWings) return 0;
    int nFix = 0;
    __try {
        auto* mp = static_cast<uint8_t*>(movePath);
        void* list = *reinterpret_cast<void**>(mp + kOffMpElem);
        if (!list) return 0;
        auto* lp = static_cast<uint8_t*>(list);
        const int size = *reinterpret_cast<int*>(lp + kOffListSize);
        void* items = *reinterpret_cast<void**>(lp + kOffListItems);
        if (!items || size <= 0) return 0;
        uint8_t* arr = static_cast<uint8_t*>(items);
        const int n = size < 32 ? size : 32;
        for (int i = 0; i < n; ++i) {
            void* elem = *reinterpret_cast<void**>(arr + 0x20 + static_cast<size_t>(i) * 8);
            if (!elem) continue;
            auto* e = static_cast<uint8_t*>(elem);
            const int a = e[kOffElemAttr];
            if (a == 16 || a == 17) {
                e[kOffElemAttr] = 0;
                ++nFix;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return nFix;
}

uint8_t Hook_Flush(void* self, void* outPacket, uint32_t bFly, void* oPath, void* methodInfo) {
    const LONG hit = InterlockedIncrement(&gFlushHits);
    const uint32_t inBfly = bFly & 1u;
    uint32_t useBfly = inBfly;
    const int wingsFixed = RewriteWingsElemsSeH(self);
    ElemSnap snap{};
    ReadElemSnapSeH(self, &snap);
    const int pktId = ReadPacketIdSeH(outPacket);
    const int attr0 = snap.nSnap > 0 ? snap.e[0].attr : -1;
    const bool autoFly =
        !gBaseline && gAutoBfly && (snap.forced == 1 || attr0 == 16 || attr0 == 17);
    if (!gBaseline && (gForceBfly || autoFly)) useBfly = 1;
    if (gBaseline) useBfly = inBfly;

    if (ShouldLogFlush(hit, useBfly, snap)) {
        InterlockedIncrement(&gFlushLogged);
        char elems[1400];
        FormatElems(elems, (int)sizeof(elems), snap);
        const OneElem z{};
        const OneElem& e0 = snap.nSnap > 0 ? snap.e[0] : z;
        FlushLogLine(
            "FLUSH#%ld this=%p out=%p pktId=%d(0x%04X) bFly_in=%u bFly_use=%u oPath=%p "
            "forced=%d elemN=%d attr0=%d(%s) xy=(%d,%d) v=(%d,%d) ma=%d fh=%d fhFall=%d el=%d "
            "forceEnv=%d auto=%d base=%d fixW=%d%s",
            (long)hit, self, outPacket, pktId, pktId < 0 ? 0 : pktId, inBfly, useBfly, oPath,
            snap.forced, snap.size, attr0, AttrName(attr0), e0.x, e0.y, e0.vx, e0.vy, e0.moveAction,
            e0.fh, e0.fhFall, e0.elapse, gForceBfly ? 1 : 0, autoFly ? 1 : 0, gBaseline ? 1 : 0,
            wingsFixed, elems);
        JsonLine(
            "{\"n\":%ld,\"pktId\":%d,\"bFly_in\":%u,\"bFly_use\":%u,\"forced\":%d,\"elemN\":%d,"
            "\"attr0\":%d,\"x\":%d,\"y\":%d,\"vx\":%d,\"vy\":%d,\"ma\":%d,\"fh\":%d,\"fhFall\":%d,"
            "\"el\":%d,\"auto\":%d,\"base\":%d,\"fixW\":%d}",
            (long)hit, pktId, inBfly, useBfly, snap.forced, snap.size, attr0, e0.x, e0.y, e0.vx,
            e0.vy, e0.moveAction, e0.fh, e0.fhFall, e0.elapse, autoFly ? 1 : 0, gBaseline ? 1 : 0,
            wingsFixed);
    }
    return gRealFlush(self, outPacket, useBfly, oPath, methodInfo);
}

void OpenFlushLogs() {
    if (gFlushLog != INVALID_HANDLE_VALUE) return;
    gFlushLog = CreateFileW(kFlushLogPrimary, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    gFlushLogTemp = CreateFileW(kFlushLogTemp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    gFlushJson = CreateFileW(kFlushJson, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE beacon = CreateFileW(kFlushBeacon, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (beacon != INVALID_HANDLE_VALUE) {
        const char* msg = "TwmsFly flush-hook v1.3 loaded\n";
        DWORD w = 0;
        WriteFile(beacon, msg, (DWORD)strlen(msg), &w, nullptr);
        CloseHandle(beacon);
    }
}

bool InstallFlushHooks() {
    OpenFlushLogs();
    if (!gFlushCsInit) {
        InitializeCriticalSection(&gFlushCs);
        gFlushCsInit = true;
    }
    gFlushSilent = EnvOn("FLUSH_HOOK_SILENT");
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
    FlushLogLine("TwmsFly flush-hook starting forceBfly=%d autoBfly=%d fixWings=%d baseline=%d",
                 gForceBfly ? 1 : 0, gAutoBfly ? 1 : 0, gFixWings ? 1 : 0, gBaseline ? 1 : 0);

    gGaBase = ModuleBase(L"GameAssembly.dll");
    if (!gGaBase) {
        FlushLogLine("ERR: GameAssembly.dll not found");
        return false;
    }
    const uint32_t rva = EnvRva("FLUSH_HOOK_RVA", kFlushRvaDefault);
    gFlushTarget = reinterpret_cast<void*>(gGaBase + rva);
    const auto* p = static_cast<const uint8_t*>(gFlushTarget);
    FlushLogLine("probe ga=0x%llX rva=0x%X target=%p bytes=%02X %02X %02X %02X %02X",
                 (unsigned long long)gGaBase, rva, gFlushTarget, p[0], p[1], p[2], p[3], p[4]);
    if (!(p[0] == 0x55 && p[1] == 0x41 && p[2] == 0x57 && p[3] == 0x41 && p[4] == 0x56)) {
        FlushLogLine("ERR: unexpected Flush prologue");
        return false;
    }
    void* real = nullptr;
    if (!InstallRelHook(gFlushTarget, reinterpret_cast<void*>(&Hook_Flush), kStealFlush, gFlushStolen,
                        &gFlushTramp, &real)) {
        FlushLogLine("ERR: InstallRelHook failed");
        return false;
    }
    gRealFlush = reinterpret_cast<FnFlush>(real);
    FlushLogLine("hooks OK tramp=%p forceBfly=%d", gFlushTramp, gForceBfly ? 1 : 0);
    return true;
}

void FlushHookTickHeartbeat() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (last && now - last < 5000) return;
    last = now;
    const LONG h = gFlushHits;
    const LONG l = gFlushLogged;
    if (h > 0) FlushLogLine("flush heartbeat hits=%ld logged=%ld", (long)h, (long)l);
}

}  // namespace detail
}  // namespace x::features::fly
'''
# Fix typo kOffMpForced -> kOffMpForcedFlush
flush_cpp = flush_cpp.replace("kOffMpForced", "kOffMpForcedFlush")
(FLY / "fly_flush_hook.cpp").write_text(flush_cpp, encoding="utf-8")
print("wrote fly_flush_hook.cpp")

# Fix fly_log OpenLogs paths — extracted code uses old names; patch fly_log.cpp
log_path = FLY / "fly_log.cpp"
log_t = log_path.read_text(encoding="utf-8")
# Ensure beacon mentions TwmsFly
log_t = log_t.replace("FlyDll", "TwmsFly")
log_t = log_t.replace("xcat_fly_loaded", "xcat_twms_fly_loaded")
log_path.write_text(log_t, encoding="utf-8")

# Fix fly_physics / others: may reference anonymous-only forward decls — check TryAcceptTf etc in resolve
# fly_resolve may still have local forward decls removed — should be ok via internal.h

# Add missing decls used by resolve that aren't in internal yet
# TryResolveLocalUser, TryScan..., MakeString, TryTag, TryFind, TryCamera, LocalUserTypeCached, etc.
# They're static in resolve file if defined there — good.

# fly_resolve starts at TypeObjectFromInstance — needs TryAcceptTf declared — in header we have ResolveActor only
# TryAcceptTf is used across resolve — defined in resolve — OK
# WriteLocalUser uses nothing from resolve

# physics uses SyncAvatar, DumpPhys — OK
# motion uses WriteActor, Resolve? ArmFly uses DumpPhys GetVecCtrl — OK
# hotkey uses ResolveActor DescribeTf ReadPos — need DescribeTf in header — YES

# BindApis in fly.cpp — OK

# Fix kOffMpForcedFlush double-replace bug: kOffMpForcedFlushFlush
ft = (FLY / "fly_flush_hook.cpp").read_text(encoding="utf-8")
ft = ft.replace("kOffMpForcedFlushFlush", "kOffMpForcedFlush")
(FLY / "fly_flush_hook.cpp").write_text(ft, encoding="utf-8")

# build.bat
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
  fly.cpp fly_state.cpp fly_log.cpp fly_physics.cpp fly_resolve.cpp fly_motion.cpp fly_hotkey.cpp fly_flush_hook.cpp ^
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
    """# fly · Classic TWMS F6 鼠标飞

对齐枫星 `x/features/fly/` 目录组织；本客户端为 **Unity 经典版**（UserMove / MovePath）。

## 产物
- `Dumps/runtime/out_bin/TwmsFly.dll`（飞 + Flush hook 合一）

## 构建
```bat
x\\features\\fly\\build.bat
```

## 注入
进图后 iDbg 注入 **仅这一支** `TwmsFly.dll`。

| 键 | 作用 |
|---|---|
| F6 | 开关飞 |
| F7 | 重绑 LocalUser |
| F8 | 强制相机对照 |

日志：`Dumps/runtime/fly.log`、`movepath_flush.log`

设计文档：[`docs/features/fly/模块设计.md`](../../../docs/features/fly/模块设计.md)
""",
    encoding="utf-8",
)

(DOC / "模块设计.md").write_text(
    """# fly Feature 模块设计（Classic TWMS）

> **状态**：🚧 联调中（v1.6 空中 Normal 伪装）  
> **范围**：经典版 Unity · F6 鼠标飞 + `MovePath.Flush` hook  
> **源码根**：`x/features/fly/`  
> **对照**：枫星 `xcat_for_fengxing/x/features/fly/`（目录同构；协议不同）

---

## 1. 职责边界

- **F6** 开关飞；鼠标像素驱动世界坐标（60Hz）
- 写 `VecCtrl.Ap` / `LocalUser.CurPos` / Transform；清 foothold
- E9 hook `MovePath.Flush`：采证 + 禁 Wings/bFly 默认路径；Attr 16/17 → Normal
- **不**在 worker 线程调用托管 Flush / Unity API（主线程 Flush 回调里可改 Elem）

与 launcher / 其他 feature 独立；当前以单 DLL 注入交付。

---

## 2. 目录结构

```
x/features/fly/
├── fly.h / fly.cpp          # 公开 API、DllMain、worker、BindApis
├── fly_internal.h           # RVA / 偏移 / 共享状态
├── fly_state.cpp            # 全局定义
├── fly_log.cpp              # fly.log
├── fly_hotkey.cpp           # F7 / F8（F6 在 motion）
├── fly_motion.cpp           # ArmFly、TickFly、鼠标采样
├── fly_physics.cpp          # ApplyAirState、写 Ap/CurPos
├── fly_resolve.cpp          # LocalUser / Transform 解析
├── fly_flush_hook.cpp       # MovePath.Flush E9
└── build.bat
```

---

## 3. 与枫星的差异（勿混）

| | 枫星 MSW | 本项目经典 TWMS |
|---|---|---|
| 权威 | 客户端位置 / SyncState | UserMove + MovePath 校验 |
| 飞实现 | Rigidbody AirPos + customMove | Ap + 清 fh + Flush 包语义 |
| 伪装 | 地面 IsOnGround spoof（施法） | **Normal + fh=0**（基线）；禁 Wings |

基线结论见 `Dumps/runtime/BASELINE_JUMP_FALL.md`。

---

## 4. 构建与验收

```bat
x\\features\\fly\\build.bat
```

注入 `TwmsFly.dll` → F6 → 查 `movepath_flush.log`：`attr0=0(Normal) fh=0 bFly_use=0`。
""",
    encoding="utf-8",
)

# Deprecate old folders
for old, name in [
    (ROOT / "Dumps/runtime/FlyDll/README.md", "FlyDll"),
    (ROOT / "Dumps/runtime/MovePathFlushHook/README.md", "MovePathFlushHook"),
]:
    old.write_text(
        f"""# DEPRECATED · 已合并

`{name}` 已并入 **`x/features/fly/`**，产物为 `Dumps/runtime/out_bin/TwmsFly.dll`。

请改用：
```bat
x\\features\\fly\\build.bat
```

见 [`docs/features/fly/模块设计.md`](../../../docs/features/fly/模块设计.md)。
""",
        encoding="utf-8",
    )

# Update docs/features/README.md index — done outside if needed
print("DONE")
for p in sorted(FLY.glob("*")):
    print(p.name, p.stat().st_size)
