// TwmsFly.dll — Classic TWMS F6 mouse-fly (v1.7.0).
// Inject with iDbg after entering a map. F6 toggles fly; mouse delta moves actor.
//
// v1.4: logical@0x240 + visual offset (fengxing-style dual space).
// v1.4.1: remove GetComponent/get_parent/Rigidbody2D that deadlocked Unity.
// v1.5: CMS Rosetta — FieldActorBase+0x50=VecCtrl; clear CurFootHold@0x28;
//       write AbsPos Ap (double X/Y @0x98/0xA0). CurPos@0x240 confirmed.
// v1.5.1: unify CurPos/Ap/tf while flying; don't pulse TryJumpedInFly (jump SFX);
//         bump speed. Server kick still needs MovePath.Flush(bFly).
// v1.5.2: anchor on Ap (physics), keep camOff=CurPos-Ap; re-arm from lastFly if Ap fallen.
//         Fix Y snap/kick from teleporting Ap up to camera CurPos (+120).
// v1.5.3: Phase A — set WingsNext + MovePath._forcedFlush so natural EndUpdateActive
//         may Flush with fly path. Still NO managed Flush call from worker thread.
// v1.5.4: after kick/teardown stop poking managed heap (prevents Unity GC
//         "Collecting from unknown thread" from worker writes on freed objects).
// v1.5.5: re-F6 must NOT use LASTFLY when body already landed (Ap≈vis on ground).
//         Old apFallen treated "Ap back on floor" as freefall → teleport to old sky.
//         LASTFLY only for true freefall: Ap dropped while VIS still near lastFly.
// v1.6.0: baseline 10:52 — legal air = Normal+fh=0+Vy, NOT Wings/bFly.
//         Clear Wings*; keep fh clear + forcedFlush; write Ap Vx/Vy from mouse.
// v1.6.1: kick@~5s — Flush showed |v|~669 (walk cap ~125). Clamp step/vel to
//         baseline; fix stale CurPos camOff on re-F6 after land.
// v1.6.2: user clarified 11:16 was map-change crash (not kick). Abort when VecCtrl/
//         MovePath gone; refuse F6 on zombie bind; stop DumpPhys learning dirty camOff;
//         clear _forcedFlush on F6 OFF.
// v1.6.3: kick@11:25 — Flush elemN=27/33 fh thrash + camOff 16→150. Stop per-tick
//         forcedFlush; clear fh only when stuck; lock restCamOff once (fh>0); F6 OFF
//         sync CurPos only; block re-F6 until landed.
// v1.6.4: GRAP ban — remove MovePath.Flush E9 INLINE HOOK. Data-plane only.
// v1.7.0: kick root cause — the emitted MoveElem stream contradicted itself.
//         Flushed air path had v=(0,-120) while Y rose, v=(124,-11) while X moved 5,
//         and sign flips every 30ms: any server-side "delta ≈ v*elapse" check fails.
//         Causes: (a) velocity was raw mouse jitter / dt, (b) idle faked vy=-60 with a
//         frozen position, (c) AbsPos-last was written equal to AbsPos (client delta 0),
//         (d) upward clamp 670 exceeded the baseline jump peak 555.
//         Fly is now a velocity integrator: mouse drags a target point, a rate-limited
//         tracker produces velocity, position integrates from that velocity — so
//         delta == v*dt holds by construction. A constant glide-sink keeps airborne Vy
//         non-zero (baseline air never has Vy==0).
//
// Logs (dual):
//   C:\Users\kras\Desktop\xcat_for_TWMS\Dumps\runtime\fly.log
//   %TEMP%\xcat_fly.log   + beacon %TEMP%\xcat_twms_fly_loaded.txt
// Env:
//   FLY_TRANSFORM=0x...   force managed Transform*
//   FLY_SPEED=0.70        mouse scale base (default 0.70; was 0.45)
//   FLY_SINK=25           glide descent u/s (default 25; 0=hover, 55=heavier)
// Load signal: two Beeps from worker thread (no MessageBox in DllMain).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../kick_sniff/kick_sniff.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/log.h"
#include <timeapi.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>

#pragma comment(lib, "User32.lib")

namespace twms_fly_impl {

// Dev checkout keeps its familiar log location; anywhere else the payload writes beside
// whichever module it was loaded from. No baked-in user name or drive letter.
constexpr wchar_t kLogDirDev[] = L"C:\\Users\\kras\\Desktop\\xcat_for_TWMS\\Dumps\\runtime";

// RVAs from Dumps/fly_position_notes.md (runtime ForceDump build)
constexpr uint32_t kRvaCamGetMain        = 0x4DDF440;
constexpr uint32_t kRvaCompGetTransform  = 0x4E496A0;
constexpr uint32_t kRvaCompGetGo         = 0x4E49780;
constexpr uint32_t kRvaGoGetTransform    = 0x4E4EB40;
constexpr uint32_t kRvaGoFindWithTag     = 0x4E4D9B0;
constexpr uint32_t kRvaGoFind            = 0x4E50410;
constexpr uint32_t kRvaObjGetName        = 0x4E566E0;
constexpr uint32_t kRvaObjGetCachedPtr   = 0x4E566D0;
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E413A0;
constexpr uint32_t kRvaTfGetPos          = 0x4E64110;
constexpr uint32_t kRvaTfSetPos          = 0x4E64230;
constexpr uint32_t kRvaTfGetPosInjected  = 0x4E641F0;
constexpr uint32_t kRvaTfSetPosInjected  = 0x4E642E0;
// Reject origin dummies like GameObject.Find("User") at (0,0,0).
constexpr float kMinPosAbs = 1.0f;
// Suspected LocalUser (c3f0caba…): Vector2 map pos + Camera ref.
constexpr char kLocalUserClass[] =
    "c3f0cabae2a31347606a13c963e006f3d92084a7c7e957b1abf08adcddf59f9";
constexpr size_t kOffLocalUserCam = 0x198;
constexpr size_t kOffLocalUserAvatar = 0x180;  // bc85 body/sprite wrapper
// aa4320: visual/world Vector2@0x64; @0x6C mirrors it at rest (NOT velocity).
constexpr size_t kOffVisPos = 0x64;
constexpr size_t kOffVisMirror = 0x6C;
constexpr size_t kOffBasePosA = 0xFC;   // fb575 (often 0 at rest)
constexpr size_t kOffBasePosB = 0x108;  // fb575
constexpr size_t kOffLogicalPos = 0x240;   // CurPos (CMS User.CurPos+0x18)
constexpr size_t kOffLogicalPos2 = 0x248;  // PrevPos
constexpr size_t kOffLocalUserPos3 = 0x2E0;  // usually 0 — don't force
// FieldActorBase (aa4320) owns VecCtrl* — CMS VecCtrlOwner.VecCtrl @0x50
constexpr size_t kOffVecCtrl = 0x50;
// VecCtrl (fc6568…) — CMS layout, same offsets on TW
constexpr size_t kOffVcCurFh = 0x28;   // CurFootHold*
constexpr size_t kOffVcLastFh = 0x30;  // LastFootHold*
constexpr size_t kOffVcFallStart = 0x38;
constexpr size_t kOffVcInputX = 0x50;
constexpr size_t kOffVcInputY = 0x54;
constexpr size_t kOffVcJumpNext = 0x58;
constexpr size_t kOffVcTryJumpFly = 0x59;  // TryJumpedInFly
constexpr size_t kOffVcFallDownValid = 0x60;  // FalldownNext.Valid (embedded struct)
constexpr size_t kOffVcFallDownFh = 0x68;     // FalldownNext.FhFallStart*
constexpr size_t kOffVcWingsNext = 0x70;
constexpr size_t kOffVcWingsNow = 0x71;
constexpr size_t kOffVcWingsPrev = 0x72;
constexpr size_t kOffVcWingsParam = 0x74;
constexpr size_t kOffVcMovePath = 0x78;  // MovePath*
constexpr size_t kOffVcActive = 0x80;
constexpr size_t kOffVcApX = 0x98;   // AbsPos.X double
constexpr size_t kOffVcApY = 0xA0;
constexpr size_t kOffVcApVx = 0xA8;
constexpr size_t kOffVcApVy = 0xB0;
constexpr size_t kOffVcAplX = 0xB8;  // AbsPos last
constexpr size_t kOffVcAplY = 0xC0;
constexpr size_t kOffMpForcedFlush = 0x48;  // MovePath._forcedFlush
// Velocity envelope from BASELINE_JUMP_FALL.md — asymmetric: rising tops out at the
// jump impulse (+555), falling at freefall terminal (-670).
constexpr float kApVelMaxX = 125.f;     // baseline walk / air horizontal (hard ref)
constexpr float kApVelMaxUp = 555.f;    // baseline Jump peak (+Y is up)
constexpr float kApVelMaxDown = 670.f;  // baseline freefall peak
// Fly cruise may exceed walk 125 briefly; keep under typical air |Vx| burst.
constexpr float kFlyVelMaxX = 160.f;
constexpr float kFlySpeedDefault = 0.70f;  // was 0.45 — snappier mouse→target
constexpr float kFlyMouseScaleK = 0.12f;
constexpr float kFlyMaxDK = 5.f;  // maxD/tick ≈ 3.5 @0.70 → ~210 u/s demand (vel still capped)
constexpr int kFlyScreenMickey = 48;
constexpr int kFlyMouseDeadPx = 2;
constexpr float kTickDt = 1.f / 60.f;  // nominal cadence; real dt is measured per tick
constexpr float kTickDtMin = 1.f / 250.f;
constexpr float kTickDtMax = 1.f / 20.f;
// Target-tracking integrator. The mouse drags a target point; velocity chases it under
// an acceleration limit, and position is integrated from that velocity so the flushed
// MoveElem always satisfies delta == v * elapse.
constexpr float kFlyTrackGain = 7.5f;     // 1/s — was 6; snappier chase
constexpr float kFlyLeash = 260.f;        // was 220; allow farther mouse lead at higher speed
constexpr float kFlyAccelX = 1100.f;      // u/s^2 — was 900
constexpr float kFlyAccelY = 1700.f;      // u/s^2 — was 1500
constexpr float kFlySinkDefault = 25.f;   // u/s glide (option C: 20–30 compromise)
// A jump reaches +555 for an instant and then decays; holding that for seconds is not a
// shape the baseline ever produces, so steering demands a far tighter envelope than the
// hard clamps above.
constexpr float kFlyClimbMax = 320.f;  // was 260
constexpr float kFlyDiveMax = 480.f;   // was 420
// Reversing direction makes Vy sweep through zero; the baseline never reports a
// motionless airborne sample, so hold a floor across the crossing.
constexpr float kFlyMinAirVy = 10.f;
constexpr float kMaxVisOffAbs = 80.f;  // beyond this = freefall/desync junk
// bc85 / c75f94 avatar internals
constexpr size_t kOffAvatarTf = 0x70;
constexpr size_t kOffAvatarV3 = 0x78;
constexpr size_t kOffAvatarV2 = 0x90;
// Grounded / contact bools (heuristic; cleared while flying).
constexpr size_t kOffGroundA = 0x60;
constexpr size_t kOffGroundB = 0x74;
constexpr size_t kOffGroundC = 0x104;
constexpr size_t kOffGroundD = 0x110;
constexpr size_t kOffGroundE = 0x148;

struct Vector3 {
    float x, y, z;
};

struct Vector2 {
    float x, y;
};

using FnCamMain = void* (*)(const void* method);
using FnCompTf  = void* (*)(void* self, const void* method);
using FnCompGo  = void* (*)(void* self, const void* method);
using FnGoTf    = void* (*)(void* self, const void* method);
using FnFindTag = void* (*)(void* str, const void* method);
using FnFind    = void* (*)(void* str, const void* method);
using FnObjName = void* (*)(void* self, const void* method);
using FnCached  = intptr_t (*)(void* self, const void* method);
using FnFindAll = void* (*)(void* type, const void* method);
// MSVC x64: 12-byte Vector3 returned via hidden pointer as first arg.
using FnGetPos  = void (*)(Vector3* ret, void* self, const void* method);
using FnSetPos  = void (*)(void* self, Vector3* value, const void* method);
using FnGetInj  = void (*)(intptr_t native, Vector3* ret, const void* method);
using FnSetInj  = void (*)(intptr_t native, Vector3* value, const void* method);
using FnStrNew  = void* (*)(const char* str);
using FnDomainGet = void* (*)();
using FnDomainAssemblies = void* (*)(void* domain, size_t* size);
using FnAsmImage = void* (*)(void* assembly);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);
using FnObjGetClass = void* (*)(void* obj);

HANDLE gLog = INVALID_HANDLE_VALUE;
HANDLE gLogTemp = INVALID_HANDLE_VALUE;
HMODULE gGA = nullptr;
uintptr_t gGaBase = 0;
DWORD gTickCount = 0;

FnCamMain gCamMain = nullptr;
FnCompTf  gCompTf = nullptr;
FnCompGo  gCompGo = nullptr;
FnGoTf    gGoTf = nullptr;
FnFindTag gFindTag = nullptr;
FnFind    gFind = nullptr;
FnObjName gObjName = nullptr;
FnCached  gCachedPtr = nullptr;
FnFindAll gFindAll = nullptr;
FnGetPos  gGetPos = nullptr;
FnSetPos  gSetPos = nullptr;
FnGetInj  gGetInj = nullptr;
FnSetInj  gSetInj = nullptr;
FnStrNew  gStrNew = nullptr;
FnDomainGet gDomainGet = nullptr;
FnDomainAssemblies gDomainAssemblies = nullptr;
FnAsmImage gAsmImage = nullptr;
FnClassFromName gClassFromName = nullptr;
FnClassGetType gClassGetType = nullptr;
FnTypeGetObject gTypeGetObject = nullptr;
FnObjGetClass gObjGetClass = nullptr;
int gPreferCamera = 0;  // F8 toggles: 0=auto, 1=force camera

void* gTransform = nullptr;  // managed Transform* (MyUser visual)
void* gLocalUser = nullptr;  // managed c3f0caba*
void* gLocalUserType = nullptr;  // cached System.Type — avoid re-scan assemblies
bool gFlyOn = false;
bool gSessionDead = false;  // map teardown / kick — stop all managed writes
int gFailStreak = 0;
bool f6WasDown = false;
POINT gPrevMouse{-1, -1};
float gPosX = 0, gPosY = 0, gPosZ = 0;       // PHYSICS fly anchor (Ap / visual space)
float gCamOffX = 0, gCamOffY = 0;             // CurPos = Ap + camOff (resting dual-space)
float gVisOffX = 0, gVisOffY = 0;             // kept 0 while flying (tf follows Ap)
float gVelX = 0.f, gVelY = 0.f;               // integrator velocity (u/s) → Ap Vx/Vy
float gPrevPosX = 0.f, gPrevPosY = 0.f;       // last tick anchor → AbsPos-last
float gTargetX = 0.f, gTargetY = 0.f;         // mouse-dragged goal the body chases
float gFlySink = kFlySinkDefault;
LARGE_INTEGER gQpcFreq{};
LARGE_INTEGER gLastTickQpc{};
bool gHaveLastFly = false;
float gLastFlyX = 0, gLastFlyY = 0;           // last in-flight Ap; used on re-arm
bool gSyncBaseCopies = false;                 // sync 0xFC/0x108 when they were live
bool gSyncPos3 = false;                       // sync 0x2E0 when it was live
float gMouseCarryX = 0, gMouseCarryY = 0;
float gFlySpeed = kFlySpeedDefault;
float gRestCamOffY = 0.f;  // 0=unset; learn ONCE at rest with fh>0 + X aligned
bool gRestCamOffLearned = false;
bool gAwaitLand = false;          // after F6 OFF, refuse re-arm until foothold
DWORD gLastForcedFlushMs = 0;     // rate-limit _forcedFlush (kick: elemN=27 spam)
char gResolveHow[64] = "none";
char gActorName[96] = "";

bool TryAcceptTf(void* tf, const char* howTag, const char* detail);
bool ReadPos(void* tf, float& x, float& y, float& z);
bool ReadIl2CppString(void* str, char* out, size_t outCap);
bool BindingHealthy();
void WatchBinding();
void TryLearnRestCamOff(float apx, float apy, float curX, float curY, void* fh);
void ApplyAirState(bool wantForcedFlush);
void SyncCurPosToAp(float apx, float apy);
float RestCamOffYOrDefault();
void SyncAvatarVisual(float vx, float vy, float z);
void AbortSession(const char* why);
void Log(const char* fmt, ...);

void WriteLogHandle(HANDLE h, const char* buf, int n) {
    if (h == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, nullptr);
    FlushFileBuffers(h);
}

void Log(const char* fmt, ...) {
    char body[1900];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[2048];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond,
                     body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteLogHandle(gLog, buf, n);
    WriteLogHandle(gLogTemp, buf, n);
    OutputDebugStringA(buf);
    // 统一 JSONL（x.jsonl）；本地 fly.log 仍保留高频诊断副本。
    x::runtime::LogI("Fly", "%s", body);
}

void AbortSession(const char* why) {
    if (gSessionDead && !gFlyOn) return;
    Log("ABORT session why=%s (stop managed writes — map teardown / avoid GC crash)",
        why ? why : "?");
    // Capture server disconnect / pendingError before we drop pointers.
    x::features::kick_sniff::DumpNow(why ? why : "AbortSession");
    gFlyOn = false;
    gSessionDead = true;
    gFailStreak = 0;
    gHaveLastFly = false;
    gRestCamOffLearned = false;
    gRestCamOffY = 0.f;
    gAwaitLand = false;
    gLastForcedFlushMs = 0;
    // Drop managed pointers; do not touch them again until F7 rebind after new map.
    gTransform = nullptr;
    gLocalUser = nullptr;
    Beep(400, 120);
}

bool DirExists(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Directory of the module holding this code — works whether fly is the standalone
// TwmsFly.dll or linked into the injected payload.
std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&DirExists), &self) ||
        !self) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring s(path, n);
    const size_t cut = s.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut);
}

std::wstring TempDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring s(buf, n);
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring PrimaryLogDir() {
    if (DirExists(kLogDirDev)) return kLogDirDev;
    const std::wstring mod = ModuleDir();
    if (mod.empty()) return TempDir();
    const std::wstring logs = mod + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    return DirExists(logs) ? logs : mod;
}

HANDLE OpenLogFile(const std::wstring& dir, const wchar_t* leaf) {
    if (dir.empty()) return INVALID_HANDLE_VALUE;
    const std::wstring full = dir + L"\\" + leaf;
    return CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void OpenLogs() {
    const std::wstring primaryDir = PrimaryLogDir();
    const std::wstring tempDir = TempDir();
    gLog = OpenLogFile(primaryDir, L"fly.log");
    gLogTemp = primaryDir == tempDir ? INVALID_HANDLE_VALUE
                                     : OpenLogFile(tempDir, L"xcat_fly.log");
    HANDLE beacon = OpenLogFile(tempDir, L"xcat_twms_fly_loaded.txt");
    if (beacon != INVALID_HANDLE_VALUE) {
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "FlyDll loaded pid=%lu\r\n", GetCurrentProcessId());
        DWORD w = 0;
        if (n > 0) WriteFile(beacon, msg, (DWORD)n, &w, nullptr);
        CloseHandle(beacon);
    }
    // Narrow path for ASCII log line only (dev / XCat_data paths are ASCII-safe).
    char narrow[MAX_PATH]{};
    const int nn = WideCharToMultiByte(CP_UTF8, 0, primaryDir.c_str(), -1, narrow, sizeof(narrow),
                                       nullptr, nullptr);
    if (nn > 0) Log("fly log dir=%s", narrow);
}

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(gGaBase + rva);
}

inline float Clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

intptr_t NativePtr(void* managedObj) {
    if (!managedObj) return 0;
    if (gCachedPtr) {
        __try {
            return gCachedPtr(managedObj, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // Fallback: UnityEngine.Object.m_CachedPtr after Il2CppObject (klass+monitor)
    return *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(managedObj) + 0x10);
}

bool PosLooksAlive(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    // Origin dummies / unused roots are almost always (0,0,0) in this client.
    return std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs || std::fabs(z) >= kMinPosAbs;
}

bool ReadIl2CppString(void* str, char* out, size_t outCap) {
    if (!str || !out || outCap < 2) return false;
    __try {
        const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            out[n++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DescribeTf(void* tf, char* out, size_t outCap) {
    if (!out || outCap < 8) return;
    snprintf(out, outCap, "tf=%p", tf);
    if (!tf || !gCompGo || !gObjName) return;
    void* go = nullptr;
    __try {
        go = gCompGo(tf, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!go) return;
    void* nameObj = nullptr;
    __try {
        nameObj = gObjName(go, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    char name[96]{};
    if (ReadIl2CppString(nameObj, name, sizeof(name))) {
        snprintf(out, outCap, "go=\"%s\" tf=%p", name, tf);
    }
}

bool ReadPos(void* tf, float& x, float& y, float& z) {
    if (!tf) return false;
    Vector3 v{};
    __try {
        const intptr_t native = NativePtr(tf);
        if (native && gGetInj) {
            gGetInj(native, &v, nullptr);
        } else if (gGetPos) {
            gGetPos(&v, tf, nullptr);
        } else {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    x = v.x;
    y = v.y;
    z = v.z;
    return true;
}

bool WritePos(void* tf, float x, float y, float z) {
    if (!tf) return false;
    Vector3 v{x, y, z};
    __try {
        const intptr_t native = NativePtr(tf);
        if (native && gSetInj) {
            gSetInj(native, &v, nullptr);
            return true;
        }
        if (gSetPos) {
            gSetPos(tf, &v, nullptr);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

void WriteV2(void* obj, size_t off, float x, float y) {
    if (!obj) return;
    __try {
        auto* p = reinterpret_cast<Vector2*>(reinterpret_cast<uint8_t*>(obj) + off);
        p->x = x;
        p->y = y;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteV3Field(void* obj, size_t off, float x, float y, float z) {
    if (!obj) return;
    __try {
        auto* p = reinterpret_cast<Vector3*>(reinterpret_cast<uint8_t*>(obj) + off);
        p->x = x;
        p->y = y;
        p->z = z;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteBool(void* obj, size_t off, bool v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReadBool(void* obj, size_t off) {
    if (!obj) return false;
    __try {
        return *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

Vector2 ReadV2(void* obj, size_t off) {
    Vector2 z{0, 0};
    if (!obj) return z;
    __try {
        return *reinterpret_cast<Vector2*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return z;
    }
}

void WritePtr(void* obj, size_t off, void* v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteF64(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* GetVecCtrl() {
    if (!gLocalUser) return nullptr;
    return ReadPtr(gLocalUser, kOffVecCtrl);
}

float RestCamOffYOrDefault() {
    return gRestCamOffLearned ? gRestCamOffY : 11.f;
}

// Learn exactly once per map session: standing on foothold, Ap≈vis, CurPos X≈Ap X.
void TryLearnRestCamOff(float apx, float apy, float curX, float curY, void* fh) {
    if (gRestCamOffLearned || gFlyOn || !fh) return;
    if (!PosLooksAlive(apx, apy, 0) || !PosLooksAlive(curX, curY, 0)) return;
    const float ox = curX - apx;
    const float oy = curY - apy;
    if (std::fabs(ox) > 40.f || oy <= 5.f || oy > 120.f) return;
    gRestCamOffY = oy;
    gRestCamOffLearned = true;
    Log("learned restCamOffY=%.2f (ox=%.2f fh=%p)", gRestCamOffY, ox, fh);
}

void SyncCurPosToAp(float apx, float apy) {
    if (gSessionDead || !gLocalUser) return;
    const float coy = RestCamOffYOrDefault();
    const float cx = apx + gCamOffX;
    const float cy = apy + coy;
    gCamOffY = coy;
    WriteV2(gLocalUser, kOffLogicalPos, cx, cy);
    WriteV2(gLocalUser, kOffLogicalPos2, cx, cy);
    if (gTransform) WritePos(gTransform, cx, cy, gPosZ);
}

// wantForcedFlush: only on arm pulse or while mouse is moving (rate-limited).
// Per-tick forcedFlush + fh clear caused Flush elemN=27/33 foothold thrash → kick.
void ApplyAirState(bool wantForcedFlush) {
    if (gSessionDead || !gLocalUser) return;
    WriteBool(gLocalUser, kOffGroundA, false);
    WriteBool(gLocalUser, kOffGroundB, false);
    WriteBool(gLocalUser, kOffGroundC, false);
    WriteBool(gLocalUser, kOffGroundD, false);
    WriteBool(gLocalUser, kOffGroundE, false);

    void* vc = GetVecCtrl();
    if (!vc) return;
    // Clear foothold only while stuck to one — continuous clear+reattach spam is what
    // produced elemN=27 oscillate fh=27↔0 at same xy.
    void* curFh = ReadPtr(vc, kOffVcCurFh);
    if (curFh) {
        WritePtr(vc, kOffVcCurFh, nullptr);
        WritePtr(vc, kOffVcLastFh, nullptr);
        WritePtr(vc, kOffVcFallStart, nullptr);
    }
    WriteBool(vc, kOffVcJumpNext, false);
    WriteBool(vc, kOffVcTryJumpFly, false);
    WriteBool(vc, kOffVcWingsNext, false);
    WriteBool(vc, kOffVcWingsNow, false);
    WriteBool(vc, kOffVcWingsPrev, false);
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcWingsParam) = 0;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(vc) + kOffVcFallDownValid) = 0;
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(vc) + kOffVcFallDownFh) = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    WriteBool(vc, kOffVcActive, true);
    void* mp = ReadPtr(vc, kOffVcMovePath);
    if (mp && wantForcedFlush) {
        const DWORD now = GetTickCount();
        if (gLastForcedFlushMs == 0 || now - gLastForcedFlushMs >= 480) {
            WriteBool(mp, kOffMpForcedFlush, true);
            gLastForcedFlushMs = now;
        }
    }
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcInputX) = 0;
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcInputY) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Velocity is the integrator state, never a back-computed mouse delta, so the pair
// (AbsPos, AbsPos-velocity) the client gathers into MoveElem stays self-consistent.
// AbsPos-last must lag by one tick — writing it equal to AbsPos told the client the
// body had not moved while we simultaneously reported a large velocity.
void WriteVecCtrlAbsPos(float x, float y) {
    void* vc = GetVecCtrl();
    if (!vc) return;
    WriteF64(vc, kOffVcApX, static_cast<double>(x));
    WriteF64(vc, kOffVcApY, static_cast<double>(y));
    WriteF64(vc, kOffVcApVx, static_cast<double>(Clampf(gVelX, -kFlyVelMaxX, kFlyVelMaxX)));
    WriteF64(vc, kOffVcApVy, static_cast<double>(Clampf(gVelY, -kApVelMaxDown, kApVelMaxUp)));
    WriteF64(vc, kOffVcAplX, static_cast<double>(gPrevPosX));
    WriteF64(vc, kOffVcAplY, static_cast<double>(gPrevPosY));
}

// Fengxing SetPos analogue:
//   Ap / Transform / vis64  = physics fly pos (gPos)
//   CurPos / PrevPos        = gPos + camOff  (preserve resting camera dual-space)
void WriteLocalUserLogical(float x, float y) {
    if (gSessionDead || !gLocalUser) return;
    const bool moving = std::fabs(gVelX) > 1.f || std::fabs(gVelY) > 1.f;
    ApplyAirState(moving);
    WriteVecCtrlAbsPos(x, y);

    const float cx = x + gCamOffX;
    const float cy = y + gCamOffY;
    WriteV2(gLocalUser, kOffLogicalPos, cx, cy);
    WriteV2(gLocalUser, kOffLogicalPos2, cx, cy);
    if (gSyncBaseCopies) {
        WriteV2(gLocalUser, kOffBasePosA, cx, cy);
        WriteV2(gLocalUser, kOffBasePosB, cx, cy);
    }
    if (gSyncPos3) WriteV2(gLocalUser, kOffLocalUserPos3, cx, cy);

    const float vx = x + gVisOffX;
    const float vy = y + gVisOffY;
    WriteV2(gLocalUser, kOffVisPos, vx, vy);
    WriteV2(gLocalUser, kOffVisMirror, vx, vy);
}

bool WriteActor(float x, float y, float z) {
    if (gSessionDead) return false;
    WriteLocalUserLogical(x, y);
    const float vx = x + gVisOffX;
    const float vy = y + gVisOffY;
    SyncAvatarVisual(vx, vy, z);
    gLastFlyX = x;
    gLastFlyY = y;
    gHaveLastFly = true;
    return WritePos(gTransform, vx, vy, z);
}

bool BindingHealthy() {
    if (gSessionDead || !gLocalUser || !gTransform) return false;
    __try {
        if (NativePtr(gTransform) == 0) return false;
        void* vc = GetVecCtrl();
        if (!vc) return false;
        void* mp = ReadPtr(vc, kOffVcMovePath);
        if (!mp) return false;
        const float apx = static_cast<float>(ReadF64(vc, kOffVcApX));
        const float apy = static_cast<float>(ReadF64(vc, kOffVcApY));
        const Vector2 vis = ReadV2(gLocalUser, kOffVisPos);
        // Map change tears VecCtrl first; leftover TF coords (e.g. 1170,-300) must not count.
        if (!PosLooksAlive(apx, apy, 0) && !PosLooksAlive(vis.x, vis.y, 0)) return false;
        float tx = 0, ty = 0, tz = 0;
        if (!ReadPos(gTransform, tx, ty, tz)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ActorStillAlive() {
    if (!BindingHealthy()) return false;
    float x = 0, y = 0, z = 0;
    if (!ReadPos(gTransform, x, y, z)) return false;
    // After map teardown Transform often reads (0,0,0) while CachedPtr still briefly non-null.
    if (!PosLooksAlive(x, y, z) && gFlyOn) return false;
    return true;
}

void WatchBinding() {
    if (gSessionDead || !gLocalUser) return;
    if (!BindingHealthy()) AbortSession("binding lost (map change / actor teardown)");
}

void NoteWriteResult(bool ok) {
    if (ok) {
        gFailStreak = 0;
        return;
    }
    ++gFailStreak;
    if (gFailStreak >= 3) AbortSession("write/read fail streak");
}

void DumpPhysFields(const char* tag) {
    if (gSessionDead || !gLocalUser) return;
    const Vector2 p64 = ReadV2(gLocalUser, kOffVisPos);
    const Vector2 p6c = ReadV2(gLocalUser, kOffVisMirror);
    const Vector2 pfc = ReadV2(gLocalUser, kOffBasePosA);
    const Vector2 p108 = ReadV2(gLocalUser, kOffBasePosB);
    const Vector2 p240 = ReadV2(gLocalUser, kOffLogicalPos);
    const Vector2 p248 = ReadV2(gLocalUser, kOffLogicalPos2);
    const Vector2 p2e0 = ReadV2(gLocalUser, kOffLocalUserPos3);
    void* vc = GetVecCtrl();
    void* fh = vc ? ReadPtr(vc, kOffVcCurFh) : nullptr;
    void* mp = vc ? ReadPtr(vc, kOffVcMovePath) : nullptr;
    const int forced = mp && ReadBool(mp, kOffMpForcedFlush) ? 1 : 0;
    const int wNext = vc && ReadBool(vc, kOffVcWingsNext) ? 1 : 0;
    const int wNow = vc && ReadBool(vc, kOffVcWingsNow) ? 1 : 0;
    const int wPrev = vc && ReadBool(vc, kOffVcWingsPrev) ? 1 : 0;
    const double apx = vc ? ReadF64(vc, kOffVcApX) : 0;
    const double apy = vc ? ReadF64(vc, kOffVcApY) : 0;
    // Learn-once only (never overwrite). Requires foothold — DumpPhys must not
    // poison restY from stale CurPos while Ap already moved/fallen.
    TryLearnRestCamOff(static_cast<float>(apx), static_cast<float>(apy), p240.x, p240.y, fh);
    Log("%s vis64=(%.2f,%.2f) mir6c=(%.2f,%.2f) fc=(%.2f,%.2f) 108=(%.2f,%.2f) "
        "CurPos=(%.2f,%.2f) PrevPos=(%.2f,%.2f) 2e0=(%.2f,%.2f) camOff=(%.2f,%.2f) "
        "vc=%p fh=%p mp=%p forced=%d wings=%d/%d/%d Ap=(%.2f,%.2f)",
        tag, p64.x, p64.y, p6c.x, p6c.y, pfc.x, pfc.y, p108.x, p108.y, p240.x, p240.y, p248.x,
        p248.y, p2e0.x, p2e0.y, gCamOffX, gCamOffY, vc, fh, mp, forced, wNext, wNow, wPrev, apx,
        apy);
}

void SyncAvatarVisual(float vx, float vy, float z) {
    if (gSessionDead || !gLocalUser) return;
    void* avatar = ReadPtr(gLocalUser, kOffLocalUserAvatar);
    if (!avatar) return;
    // Bare field writes only — avoid extra Transform.set_position from worker thread.
    WriteV2(avatar, kOffAvatarV2, vx, vy);
    WriteV3Field(avatar, kOffAvatarV3, vx, vy, z);
}

void* TypeObjectFromInstance(void* managedObj) {
    if (!managedObj || !gObjGetClass || !gClassGetType || !gTypeGetObject) return nullptr;
    __try {
        void* klass = gObjGetClass(managedObj);
        if (!klass) return nullptr;
        void* type = gClassGetType(klass);
        if (!type) return nullptr;
        return gTypeGetObject(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* FindClassTypeObject(const char* className) {
    if (!gDomainGet || !gDomainAssemblies || !gAsmImage || !gClassFromName || !gClassGetType ||
        !gTypeGetObject || !className)
        return nullptr;
    __try {
        void* domain = gDomainGet();
        if (!domain) return nullptr;
        size_t n = 0;
        void** asms = reinterpret_cast<void**>(gDomainAssemblies(domain, &n));
        if (!asms || n == 0) return nullptr;
        for (size_t i = 0; i < n; ++i) {
            void* img = gAsmImage(asms[i]);
            if (!img) continue;
            void* klass = gClassFromName(img, "", className);
            if (!klass) continue;
            void* type = gClassGetType(klass);
            if (!type) continue;
            void* typeObj = gTypeGetObject(type);
            if (typeObj) {
                Log("FindClassType ok class=%s", className);
                return typeObj;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    Log("FindClassType miss class=%s", className);
    return nullptr;
}

void* LocalUserTypeCached() {
    if (!gLocalUserType) gLocalUserType = FindClassTypeObject(kLocalUserClass);
    return gLocalUserType;
}

bool LocalUserStillAlive() {
    return BindingHealthy();
}

bool NameLooksUiOrCam(const char* name) {
    if (!name || !name[0]) return false;
    if (_stricmp(name, "Main Camera") == 0) return true;
    if (_stricmp(name, "MainCamera") == 0) return true;
    if (strstr(name, "Canvas") || strstr(name, "UI") || strstr(name, "Event")) return true;
    if (strstr(name, "Camera")) return true;
    return false;
}

bool GetGoName(void* tf, char* out, size_t outCap) {
    out[0] = 0;
    if (!tf || !gCompGo || !gObjName) return false;
    void* go = nullptr;
    __try {
        go = gCompGo(tf, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!go) return false;
    void* nameObj = nullptr;
    __try {
        nameObj = gObjName(go, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ReadIl2CppString(nameObj, out, outCap);
}

// Il2CppArray: max_length @+0x18, elements @+0x20 (x64).
uintptr_t ArrayLen(void* arr) {
    if (!arr) return 0;
    __try {
        return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* ArrayAt(void* arr, uintptr_t i) {
    if (!arr) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 + i * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool TryResolveLocalUser(void* mainCam, float camX, float camY) {
    void* typeObj = LocalUserTypeCached();
    if (!typeObj || !gFindAll) return false;
    Log("LocalUser FindAll begin");
    void* arr = nullptr;
    __try {
        arr = gFindAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("LocalUser FindAll SEH");
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    Log("LocalUser FindAll count=%llu", (unsigned long long)n);
    void* best = nullptr;
    float bestScore = 1e9f;
    for (uintptr_t i = 0; i < n && i < 64; ++i) {
        void* obj = ArrayAt(arr, i);
        if (!obj) continue;
        __try {
            void* camField =
                *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + kOffLocalUserCam);
            Vector2 pos240 = ReadV2(obj, kOffLogicalPos);
            Vector2 pos64 = ReadV2(obj, kOffVisPos);
            void* tf = gCompTf ? gCompTf(obj, nullptr) : nullptr;
            float tx = 0, ty = 0, tz = 0;
            char name[96]{};
            if (tf) {
                ReadPos(tf, tx, ty, tz);
                GetGoName(tf, name, sizeof(name));
            }
            // Score against LOGICAL (camera-follow) first — visual Y often lags by ~100+.
            const float refX = PosLooksAlive(pos240.x, pos240.y, 0) ? pos240.x : pos64.x;
            const float refY = PosLooksAlive(pos240.x, pos240.y, 0) ? pos240.y : pos64.y;
            const float dx = refX - camX;
            const float dy = refY - camY;
            float dist = std::sqrt(dx * dx + dy * dy);
            const bool camMatch = mainCam && camField == mainCam;
            const bool isMine = name[0] && _stricmp(name, "MyUser") == 0;
            float score = dist;
            if (camMatch) score -= 500.f;
            if (isMine) score -= 1000.f;
            Log("LocalUser[%llu] name=\"%s\" cam=%p vis64=(%.2f,%.2f) log240=(%.2f,%.2f) "
                "tf=(%.2f,%.2f) dist=%.2f score=%.2f",
                (unsigned long long)i, name, camField, pos64.x, pos64.y, pos240.x, pos240.y, tx, ty,
                dist, score);
            if (score < bestScore) {
                bestScore = score;
                best = obj;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!best) return false;
    gLocalUser = best;
    void* tf = nullptr;
    __try {
        tf = gCompTf(best, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tf = nullptr;
    }
    DumpPhysFields("LocalUser pick");
    if (tf && TryAcceptTf(tf, "localuser", "MyUser")) return true;
    if (tf) {
        Vector2 pos = ReadV2(best, kOffLogicalPos);
        if (!PosLooksAlive(pos.x, pos.y, 0)) pos = ReadV2(best, kOffVisPos);
        gTransform = tf;
        snprintf(gResolveHow, sizeof(gResolveHow), "localuser:tf");
        GetGoName(tf, gActorName, sizeof(gActorName));
        Log("LocalUser ACCEPT tf=%p name=%s log=(%.2f,%.2f)", tf, gActorName, pos.x, pos.y);
        return true;
    }
    return false;
}

bool TryScanTransformsNearCamera(void* camTf, float camX, float camY, float camZ) {
    void* typeObj = TypeObjectFromInstance(camTf);
    if (!typeObj || !gFindAll) {
        Log("scan: no Transform type");
        return false;
    }
    void* arr = nullptr;
    __try {
        arr = gFindAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("scan: FindAll SEH");
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    Log("scan Transform count=%llu cam=(%.2f,%.2f,%.2f)", (unsigned long long)n, camX, camY, camZ);

    void* best = nullptr;
    float bestScore = 1e9f;
    char bestName[96]{};
    int logged = 0;

    for (uintptr_t i = 0; i < n && i < 4096; ++i) {
        void* tf = ArrayAt(arr, i);
        if (!tf || tf == camTf) continue;
        float x, y, z;
        if (!ReadPos(tf, x, y, z) || !PosLooksAlive(x, y, z)) continue;
        const float dx = x - camX;
        const float dy = y - camY;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > 120.f) continue;

        char name[96]{};
        GetGoName(tf, name, sizeof(name));
        if (NameLooksUiOrCam(name)) continue;

        // Prefer character plane (z near 0) over camera plane (z≈-10).
        float score = dist + std::fabs(z) * 0.25f;
        if (name[0] && (strstr(name, "Player") || strstr(name, "Avatar") || strstr(name, "Char") ||
                        strstr(name, "User") || strstr(name, "Hero")))
            score -= 40.f;

        if (logged < 12) {
            Log("scan hit name=\"%s\" pos=(%.2f,%.2f,%.2f) dist=%.2f score=%.2f", name, x, y, z, dist,
                score);
            ++logged;
        }
        if (score < bestScore) {
            bestScore = score;
            best = tf;
            snprintf(bestName, sizeof(bestName), "%s", name);
        }
    }

    if (!best) {
        Log("scan: no near-camera actor");
        return false;
    }
    gTransform = best;
    snprintf(gResolveHow, sizeof(gResolveHow), "scan:%s", bestName[0] ? bestName : "anon");
    snprintf(gActorName, sizeof(gActorName), "%s", bestName);
    float x, y, z;
    ReadPos(best, x, y, z);
    Log("scan ACCEPT name=\"%s\" tf=%p pos=(%.2f,%.2f,%.2f) score=%.2f", bestName, best, x, y, z,
        bestScore);
    return true;
}

void* MakeString(const char* s) {
    if (!gStrNew || !s) return nullptr;
    __try {
        return gStrNew(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* TryTag(const char* tag) {
    if (!gFindTag) return nullptr;
    void* str = MakeString(tag);
    if (!str) return nullptr;
    void* go = nullptr;
    __try {
        go = gFindTag(str, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!go || !gGoTf) return nullptr;
    void* tf = nullptr;
    __try {
        tf = gGoTf(go, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return tf;
}

void* TryFind(const char* name) {
    if (!gFind) return nullptr;
    void* str = MakeString(name);
    if (!str) return nullptr;
    void* go = nullptr;
    __try {
        go = gFind(str, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!go || !gGoTf) return nullptr;
    void* tf = nullptr;
    __try {
        tf = gGoTf(go, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return tf;
}

void* TryCamera() {
    if (!gCamMain || !gCompTf) return nullptr;
    void* cam = nullptr;
    __try {
        cam = gCamMain(nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!cam) return nullptr;
    void* tf = nullptr;
    __try {
        tf = gCompTf(cam, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return tf;
}

bool TryAcceptTf(void* tf, const char* howTag, const char* detail) {
    float x, y, z;
    if (!tf || !ReadPos(tf, x, y, z)) return false;
    char desc[160]{};
    DescribeTf(tf, desc, sizeof(desc));
    const bool alive = PosLooksAlive(x, y, z);
    Log("candidate how=%s detail=%s %s pos=(%.2f,%.2f,%.2f) alive=%d", howTag, detail ? detail : "-",
        desc, x, y, z, alive ? 1 : 0);
    if (!alive) return false;
    gTransform = tf;
    snprintf(gResolveHow, sizeof(gResolveHow), "%s:%s", howTag, detail ? detail : "?");
    GetGoName(tf, gActorName, sizeof(gActorName));
    Log("resolve ACCEPT how=%s %s pos=(%.2f,%.2f,%.2f)", gResolveHow, desc, x, y, z);
    return true;
}

bool ResolveActor(bool forceRescan) {
    // Reuse cached LocalUser/Transform whenever possible.
    // FindObjectsOfTypeAll from a worker thread can deadlock the Unity main loop
    // (freeze + BGM still playing) — especially on a second call right after inject.
    if (!forceRescan && LocalUserStillAlive()) {
        Log("resolve reuse cached how=%s lu=%p tf=%p", gResolveHow, gLocalUser, gTransform);
        return true;
    }

    void* keepLu = gLocalUser;
    void* keepTf = gTransform;
    gLocalUser = nullptr;

    char env[64]{};
    if (GetEnvironmentVariableA("FLY_TRANSFORM", env, sizeof(env)) > 0) {
        void* p = reinterpret_cast<void*>(_strtoui64(env, nullptr, 0));
        if (TryAcceptTf(p, "env", env)) return true;
        Log("resolve env FLY_TRANSFORM rejected: %s", env);
    }

    void* mainCam = nullptr;
    if (gCamMain) {
        __try {
            mainCam = gCamMain(nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mainCam = nullptr;
        }
    }
    void* camTf = TryCamera();
    float camX = 0, camY = 0, camZ = 0;
    const bool haveCam = camTf && ReadPos(camTf, camX, camY, camZ);
    if (haveCam) Log("resolve cam pos=(%.2f,%.2f,%.2f)", camX, camY, camZ);

    if (gPreferCamera) {
        if (TryAcceptTf(camTf, "force", "camera")) return true;
        Log("force camera failed");
    }

    // 1) LocalUser class instance whose Camera matches main / pos near cam.
    if (haveCam && TryResolveLocalUser(mainCam, camX, camY)) return true;

    // Restore previous binding if scan failed (avoid wiping a good cache).
    if (keepLu && keepTf) {
        gLocalUser = keepLu;
        gTransform = keepTf;
        if (BindingHealthy()) {
            Log("resolve scan miss — keep previous binding");
            return true;
        }
        gLocalUser = nullptr;
        gTransform = nullptr;
        Log("resolve scan miss — previous binding also dead");
    }

    // 2) Scan all Transforms near camera (character plane).
    if (haveCam && TryScanTransformsNearCamera(camTf, camX, camY, camZ)) return true;

    static const char* kTags[] = {"Player", "LocalPlayer", "Hero", "Character", "player"};
    for (const char* t : kTags) {
        if (TryAcceptTf(TryTag(t), "tag", t)) return true;
    }
    static const char* kNames[] = {"Player", "LocalPlayer", "Hero", "Character", "player",
                                   "PlayerRoot", "Avatar", "LocalAvatar"};
    for (const char* n : kNames) {
        if (TryAcceptTf(TryFind(n), "find", n)) return true;
    }

    // Camera only as explicit last resort (rubber-bands under follow script).
    if (TryAcceptTf(camTf, "fallback", "camera")) {
        Log("resolve WARNING using camera — expect rubber-band until LocalActor found");
        return true;
    }

    Log("resolve FAILED");
    gTransform = nullptr;
    snprintf(gResolveHow, sizeof(gResolveHow), "none");
    return false;
}

void ResetMouse() {
    if (!GetCursorPos(&gPrevMouse)) gPrevMouse = {-1, -1};
    gMouseCarryX = gMouseCarryY = 0.f;
}

// The worker paces itself with Sleep(), so the real gap drifts around 1/60. Integrating
// with the nominal constant would leave displacement short of the velocity the client
// reports alongside it, which is exactly the divergence we are removing.
float MeasureTickDt() {
    LARGE_INTEGER now{};
    if (!gQpcFreq.QuadPart || !QueryPerformanceCounter(&now)) return kTickDt;
    if (!gLastTickQpc.QuadPart) {
        gLastTickQpc = now;
        return kTickDt;
    }
    const double dt =
        double(now.QuadPart - gLastTickQpc.QuadPart) / double(gQpcFreq.QuadPart);
    gLastTickQpc = now;
    return Clampf(static_cast<float>(dt), kTickDtMin, kTickDtMax);
}

void ResetTickClock() {
    if (!gQpcFreq.QuadPart) QueryPerformanceFrequency(&gQpcFreq);
    gLastTickQpc.QuadPart = 0;
}

void ArmFly(bool on) {
    if (on) {
        if (gSessionDead) {
            Log("F6 ON ignored — session dead; press F7 to rebind after re-enter map");
            Beep(400, 80);
            return;
        }
        if (!ResolveActor(false)) {
            Log("F6 ON but no actor");
            gFlyOn = false;
            return;
        }
        if (!BindingHealthy()) {
            AbortSession("F6 ON but VecCtrl/MovePath dead (map loading?)");
            Beep(400, 80);
            return;
        }
        // After OFF, wait until foothold + Ap≈vis so CurPos/rest aren't learned mid-fall.
        if (gAwaitLand) {
            void* vc = GetVecCtrl();
            void* fh = vc ? ReadPtr(vc, kOffVcCurFh) : nullptr;
            const Vector2 vis = ReadV2(gLocalUser, kOffVisPos);
            const float apx = vc ? static_cast<float>(ReadF64(vc, kOffVcApX)) : 0.f;
            const float apy = vc ? static_cast<float>(ReadF64(vc, kOffVcApY)) : 0.f;
            const bool landed = fh && PosLooksAlive(apx, apy, 0) && PosLooksAlive(vis.x, vis.y, 0) &&
                                std::fabs(apx - vis.x) < 25.f && std::fabs(apy - vis.y) < 25.f;
            if (!landed) {
                Log("F6 ON blocked — wait land fh=%p ap=(%.1f,%.1f) vis=(%.1f,%.1f)", fh, apx, apy,
                    vis.x, vis.y);
                Beep(400, 50);
                return;
            }
            SyncCurPosToAp(apx, apy);
            gAwaitLand = false;
            Log("F6 land gate passed — CurPos synced restY=%.2f learned=%d", RestCamOffYOrDefault(),
                gRestCamOffLearned ? 1 : 0);
        }
        DumpPhysFields("F6 before");
        gVisOffX = gVisOffY = 0.f;
        gSyncBaseCopies = false;
        gSyncPos3 = false;
        float tx = 0, ty = 0, tz = 0;
        ReadPos(gTransform, tx, ty, tz);
        gPosZ = tz;

        if (gLocalUser) {
            const Vector2 logp = ReadV2(gLocalUser, kOffLogicalPos);
            const Vector2 vis = ReadV2(gLocalUser, kOffVisPos);
            void* vc = GetVecCtrl();
            const float apx = vc ? static_cast<float>(ReadF64(vc, kOffVcApX)) : 0.f;
            const float apy = vc ? static_cast<float>(ReadF64(vc, kOffVcApY)) : 0.f;
            void* fh = vc ? ReadPtr(vc, kOffVcCurFh) : nullptr;
            const Vector2 fc = ReadV2(gLocalUser, kOffBasePosA);
            const Vector2 p2e0 = ReadV2(gLocalUser, kOffLocalUserPos3);
            gSyncBaseCopies = PosLooksAlive(fc.x, fc.y, 0);
            gSyncPos3 = PosLooksAlive(p2e0.x, p2e0.y, 0);

            float anchorX = 0, anchorY = 0;
            const bool apOk = PosLooksAlive(apx, apy, 0);
            const bool visOk = PosLooksAlive(vis.x, vis.y, 0);
            const bool lastOk = gHaveLastFly && PosLooksAlive(gLastFlyX, gLastFlyY, 0);
            const bool apNearVis = apOk && visOk && std::fabs(apx - vis.x) < 25.f &&
                                  std::fabs(apy - vis.y) < 25.f;
            const bool visNearLast =
                lastOk && visOk && std::fabs(vis.x - gLastFlyX) < 40.f &&
                std::fabs(vis.y - gLastFlyY) < 40.f;
            const bool apDroppedFromLast = lastOk && apOk && (apy < gLastFlyY - 80.f);
            const bool trueFreefall =
                lastOk && apDroppedFromLast && visNearLast && !apNearVis;

            if (trueFreefall) {
                anchorX = gLastFlyX;
                anchorY = gLastFlyY;
                Log("F6 anchor LASTFLY (%.2f,%.2f) (true freefall ap=(%.2f,%.2f) vis=(%.2f,%.2f))",
                    anchorX, anchorY, apx, apy, vis.x, vis.y);
            } else if (apNearVis) {
                anchorX = apx;
                anchorY = apy;
                gHaveLastFly = false;
                Log("F6 anchor AP/VIS (%.2f,%.2f)", anchorX, anchorY);
            } else if (apOk) {
                anchorX = apx;
                anchorY = apy;
                Log("F6 anchor AP (%.2f,%.2f)", anchorX, anchorY);
            } else if (visOk) {
                anchorX = vis.x;
                anchorY = vis.y;
                Log("F6 anchor VIS (%.2f,%.2f)", anchorX, anchorY);
            } else if (PosLooksAlive(tx, ty, tz)) {
                anchorX = tx;
                anchorY = ty;
                Log("F6 anchor TF (%.2f,%.2f)", anchorX, anchorY);
            } else {
                Log("F6 ON but no sane physics anchor");
                gFlyOn = false;
                return;
            }

            gPosX = anchorX;
            gPosY = anchorY;

            // camOff: locked rest once learned — never adopt stale CurPos oy.
            TryLearnRestCamOff(apx, apy, logp.x, logp.y, fh);
            gCamOffX = 0.f;
            gCamOffY = RestCamOffYOrDefault();
            Log("F6 camOff=(%.2f,%.2f) restY=%.2f learned=%d CurPos was (%.2f,%.2f) Ap was (%.2f,%.2f)",
                gCamOffX, gCamOffY, gRestCamOffY, gRestCamOffLearned ? 1 : 0, logp.x, logp.y, apx,
                apy);
        } else if (!ReadPos(gTransform, gPosX, gPosY, gPosZ)) {
            Log("F6 ON but ReadPos failed");
            gFlyOn = false;
            return;
        }

        gLastForcedFlushMs = 0;  // allow one arm pulse immediately
        // Seed the integrator from the body's live velocity so arming does not emit a
        // velocity step the previous MoveElem cannot explain.
        {
            void* vc = GetVecCtrl();
            gVelX = vc ? Clampf(static_cast<float>(ReadF64(vc, kOffVcApVx)), -kFlyVelMaxX,
                                kFlyVelMaxX)
                       : 0.f;
            gVelY = vc ? Clampf(static_cast<float>(ReadF64(vc, kOffVcApVy)), -kApVelMaxDown,
                                kApVelMaxUp)
                       : 0.f;
        }
        gPrevPosX = gPosX;
        gPrevPosY = gPosY;
        gTargetX = gPosX;
        gTargetY = gPosY;
        ApplyAirState(true);  // single leave-platform forced flush
        WriteActor(gPosX, gPosY, gPosZ);
        ResetMouse();
        ResetTickClock();
        gFlyOn = true;
        Beep(1500, 60);
        Log("F6 ON how=%s ap=(%.2f,%.2f,%.2f) camOff=(%.2f,%.2f) speed=%.2f lu=%p", gResolveHow,
            gPosX, gPosY, gPosZ, gCamOffX, gCamOffY, gFlySpeed, gLocalUser);
        DumpPhysFields("F6 after-arm");
    } else {
        gFlyOn = false;
        gAwaitLand = true;
        gLastForcedFlushMs = 0;
        if (!gSessionDead && gLocalUser) {
            void* vc = GetVecCtrl();
            void* mp = vc ? ReadPtr(vc, kOffVcMovePath) : nullptr;
            if (mp) WriteBool(mp, kOffMpForcedFlush, false);
            // Do NOT pin Ap to lastFly (keeps sky hover). Sync CurPos to live Ap+rest only.
            const float apx = vc ? static_cast<float>(ReadF64(vc, kOffVcApX)) : gLastFlyX;
            const float apy = vc ? static_cast<float>(ReadF64(vc, kOffVcApY)) : gLastFlyY;
            if (PosLooksAlive(apx, apy, 0)) {
                SyncCurPosToAp(apx, apy);
                Log("F6 OFF synced CurPos to Ap=(%.2f,%.2f)+restY=%.2f", apx, apy,
                    RestCamOffYOrDefault());
            }
            const Vector2 vis = ReadV2(gLocalUser, kOffVisPos);
            if (PosLooksAlive(apx, apy, 0) && PosLooksAlive(vis.x, vis.y, 0) &&
                std::fabs(apx - vis.x) < 25.f && std::fabs(apy - vis.y) < 25.f) {
                gHaveLastFly = false;
                Log("F6 OFF cleared lastFly (Ap≈vis)");
            }
        }
        Beep(700, 60);
        if (!gSessionDead) DumpPhysFields("F6 OFF");
        Log("F6 OFF lastFly=(%.2f,%.2f) haveLast=%d awaitLand=1", gLastFlyX, gLastFlyY,
            gHaveLastFly ? 1 : 0);
    }
}

// GetAsyncKeyState is process-wide: without this gate, F6 pressed in any other
// application would arm fly, and cursor motion outside the game would steer the body.
bool GameWindowFocused() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void PollF6() {
    if (!GameWindowFocused()) {
        f6WasDown = false;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool edge = down && !f6WasDown;
    f6WasDown = down;
    if (!edge) return;
    ArmFly(!gFlyOn);
    x::ipc::PayloadControl_PublishFly(gFlyOn);
}

bool SampleMouse(float& dx, float& dy) {
    POINT cur{};
    if (!GetCursorPos(&cur)) return false;
    // Alt-tabbed away: stop steering and drop the anchor so returning to the game does
    // not replay the whole excursion as one huge delta. The integrator keeps ticking, so
    // the body simply glides down under the sink term.
    if (!GameWindowFocused()) {
        gPrevMouse = cur;
        gMouseCarryX = gMouseCarryY = 0.f;
        dx = dy = 0.f;
        return false;
    }
    if (gPrevMouse.x < 0) {
        gPrevMouse = cur;
        dx = dy = 0;
        return false;
    }
    float sx = static_cast<float>(cur.x - gPrevMouse.x) + gMouseCarryX;
    float sy = static_cast<float>(cur.y - gPrevMouse.y) + gMouseCarryY;
    gPrevMouse = cur;

    const float mickeyMax = static_cast<float>(kFlyScreenMickey) * (gFlySpeed / 0.25f);
    const float rawSx = sx, rawSy = sy;
    sx = Clampf(sx, -mickeyMax, mickeyMax);
    sy = Clampf(sy, -mickeyMax, mickeyMax);
    gMouseCarryX = Clampf(rawSx - sx, -mickeyMax * 2.f, mickeyMax * 2.f);
    gMouseCarryY = Clampf(rawSy - sy, -mickeyMax * 2.f, mickeyMax * 2.f);
    if (std::fabs(sx) < kFlyMouseDeadPx) {
        sx = 0;
        if (std::fabs(gMouseCarryX) < kFlyMouseDeadPx) gMouseCarryX = 0;
    }
    if (std::fabs(sy) < kFlyMouseDeadPx) {
        sy = 0;
        if (std::fabs(gMouseCarryY) < kFlyMouseDeadPx) gMouseCarryY = 0;
    }

    const float scale = gFlySpeed * kFlyMouseScaleK;
    dx = sx * scale;
    dy = -sy * scale;  // screen Y down -> world Y up
    const float maxD = gFlySpeed * kFlyMaxDK;
    dx = Clampf(dx, -maxD, maxD);
    dy = Clampf(dy, -maxD, maxD);
    return dx != 0.f || dy != 0.f;
}

void TickFly() {
    if (!gFlyOn || gSessionDead || !gTransform) return;
    if (!ActorStillAlive()) {
        AbortSession("transform/binding dead while flying (map change?)");
        return;
    }
    const float dt = MeasureTickDt();
    float dx = 0, dy = 0;
    SampleMouse(dx, dy);

    // 1) Mouse drags the goal, not the body. A leash keeps the goal within reach so a
    //    fast swipe cannot demand a velocity the baseline envelope forbids.
    gTargetX += dx;
    gTargetY += dy;
    gTargetY -= gFlySink * dt;  // glide: airborne Vy must never settle at 0
    gTargetX = Clampf(gTargetX, gPosX - kFlyLeash, gPosX + kFlyLeash);
    gTargetY = Clampf(gTargetY, gPosY - kFlyLeash, gPosY + kFlyLeash);

    // 2) Velocity chases the goal under an acceleration limit — this is what turns
    //    per-tick cursor jitter into a smooth, plausible speed curve.
    const float wantVx = Clampf((gTargetX - gPosX) * kFlyTrackGain, -kFlyVelMaxX, kFlyVelMaxX);
    const float wantVy = Clampf((gTargetY - gPosY) * kFlyTrackGain, -kFlyDiveMax, kFlyClimbMax);
    const float stepX = kFlyAccelX * dt;
    const float stepY = kFlyAccelY * dt;
    gVelX += Clampf(wantVx - gVelX, -stepX, stepX);
    gVelY += Clampf(wantVy - gVelY, -stepY, stepY);
    gVelX = Clampf(gVelX, -kFlyVelMaxX, kFlyVelMaxX);
    gVelY = Clampf(gVelY, -kApVelMaxDown, kApVelMaxUp);
    // Prefer downward floor when sink is on — idle +10 up caused instant kick at FLY_SINK=0.
    if (std::fabs(gVelY) < kFlyMinAirVy) {
        gVelY = (gFlySink > 0.f || gVelY <= 0.f) ? -kFlyMinAirVy : kFlyMinAirVy;
    }

    // 3) Position comes from velocity, so the flushed delta always matches v * elapse.
    gPrevPosX = gPosX;
    gPrevPosY = gPosY;
    gPosX += gVelX * dt;
    gPosY += gVelY * dt;

    float beforeX = 0, beforeY = 0, beforeZ = 0;
    ReadPos(gTransform, beforeX, beforeY, beforeZ);
    const Vector2 beforeLog = gLocalUser ? ReadV2(gLocalUser, kOffLogicalPos) : Vector2{0, 0};
    const bool ok = WriteActor(gPosX, gPosY, gPosZ);
    NoteWriteResult(ok);
    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (now - lastLog > 500) {
        lastLog = now;
        float rx = 0, ry = 0, rz = 0;
        const bool rd = ReadPos(gTransform, rx, ry, rz);
        const Vector2 p64 = gLocalUser ? ReadV2(gLocalUser, kOffVisPos) : Vector2{0, 0};
        const Vector2 p240 = gLocalUser ? ReadV2(gLocalUser, kOffLogicalPos) : Vector2{0, 0};
        Log("tick write=%d read=%d wantLog=(%.2f,%.2f) beforeLog=(%.2f,%.2f) gotLog=(%.2f,%.2f) "
            "beforeTf=(%.2f,%.2f) gotTf=(%.2f,%.2f) vis64=(%.2f,%.2f) v=(%.1f,%.1f) "
            "tgt=(%.1f,%.1f) d=(%.2f,%.2f) how=%s",
            ok ? 1 : 0, rd ? 1 : 0, gPosX, gPosY, beforeLog.x, beforeLog.y, p240.x, p240.y, beforeX,
            beforeY, rx, ry, p64.x, p64.y, gVelX, gVelY, gTargetX, gTargetY, dx, dy, gResolveHow);
    }
}

bool BindApis() {
    gGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!gGA) {
        Log("GameAssembly.dll not loaded");
        return false;
    }
    gGaBase = reinterpret_cast<uintptr_t>(gGA);
    gCamMain = AtRva<FnCamMain>(kRvaCamGetMain);
    gCompTf = AtRva<FnCompTf>(kRvaCompGetTransform);
    gCompGo = AtRva<FnCompGo>(kRvaCompGetGo);
    gGoTf = AtRva<FnGoTf>(kRvaGoGetTransform);
    gFindTag = AtRva<FnFindTag>(kRvaGoFindWithTag);
    gFind = AtRva<FnFind>(kRvaGoFind);
    gObjName = AtRva<FnObjName>(kRvaObjGetName);
    gCachedPtr = AtRva<FnCached>(kRvaObjGetCachedPtr);
    gFindAll = AtRva<FnFindAll>(kRvaFindObjectsOfTypeAll);
    gGetPos = AtRva<FnGetPos>(kRvaTfGetPos);
    gSetPos = AtRva<FnSetPos>(kRvaTfSetPos);
    gGetInj = AtRva<FnGetInj>(kRvaTfGetPosInjected);
    gSetInj = AtRva<FnSetInj>(kRvaTfSetPosInjected);
    gStrNew = reinterpret_cast<FnStrNew>(GetProcAddress(gGA, "il2cpp_string_new"));
    gDomainGet = reinterpret_cast<FnDomainGet>(GetProcAddress(gGA, "il2cpp_domain_get"));
    gDomainAssemblies =
        reinterpret_cast<FnDomainAssemblies>(GetProcAddress(gGA, "il2cpp_domain_get_assemblies"));
    gAsmImage = reinterpret_cast<FnAsmImage>(GetProcAddress(gGA, "il2cpp_assembly_get_image"));
    gClassFromName =
        reinterpret_cast<FnClassFromName>(GetProcAddress(gGA, "il2cpp_class_from_name"));
    gClassGetType = reinterpret_cast<FnClassGetType>(GetProcAddress(gGA, "il2cpp_class_get_type"));
    gTypeGetObject =
        reinterpret_cast<FnTypeGetObject>(GetProcAddress(gGA, "il2cpp_type_get_object"));
    gObjGetClass = reinterpret_cast<FnObjGetClass>(GetProcAddress(gGA, "il2cpp_object_get_class"));
    Log("GA base=%p str_new=%p findAll=%p domain=%p", (void*)gGaBase, (void*)gStrNew,
        (void*)gFindAll, (void*)gDomainGet);

    char spd[32]{};
    if (GetEnvironmentVariableA("FLY_SPEED", spd, sizeof(spd)) > 0) {
        gFlySpeed = static_cast<float>(atof(spd));
        if (gFlySpeed < 0.05f) gFlySpeed = 0.05f;
        if (gFlySpeed > 1.5f) gFlySpeed = 1.5f;
        Log("FLY_SPEED=%.2f", gFlySpeed);
    }
    char sink[32]{};
    if (GetEnvironmentVariableA("FLY_SINK", sink, sizeof(sink)) > 0) {
        gFlySink = Clampf(static_cast<float>(atof(sink)), 0.f, kApVelMaxDown);
        Log("FLY_SINK=%.1f u/s", gFlySink);
    }
    return true;
}

void PollF7Rebind() {
    static bool was = false;
    if (!GameWindowFocused()) {
        was = false;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool edge = down && !was;
    was = down;
    if (!edge) return;
    Log("F7 rebind (forced FindAll) — clearing session-dead");
    Beep(1200, 80);
    gSessionDead = false;
    gFailStreak = 0;
    gFlyOn = false;
    gAwaitLand = false;
    gLastForcedFlushMs = 0;
    // Explicit user request — allow one FindAll; still risky on worker thread.
    gLocalUser = nullptr;
    gTransform = nullptr;
    ResolveActor(false);  // caches cleared → will scan once
    if (gTransform) {
        float x, y, z;
        char desc[160]{};
        DescribeTf(gTransform, desc, sizeof(desc));
        if (ReadPos(gTransform, x, y, z))
            Log("F7 pos=(%.2f,%.2f,%.2f) how=%s %s", x, y, z, gResolveHow, desc);
    }
}

void PollF8PreferCamera() {
    static bool was = false;
    if (!GameWindowFocused()) {
        was = false;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool edge = down && !was;
    was = down;
    if (!edge) return;
    gPreferCamera = gPreferCamera ? 0 : 1;
    Log("F8 preferCamera=%d", gPreferCamera);
    Beep(gPreferCamera ? 1600 : 900, 80);
    gLocalUser = nullptr;
    gTransform = nullptr;
    ResolveActor(false);
}

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
