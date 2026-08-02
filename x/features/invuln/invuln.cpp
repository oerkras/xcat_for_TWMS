// TWMS Classic — data-plane invuln v2.6.3 (remount 2026-08-03).
//
// Hit gate: User+0x298 i-frame (~100ms worker top-up).
// Anti-blink hybrid: MainPump frame tick (before+after SendWill) + worker 8ms backup.
// Soft +0x228/+0x22C DISABLED. Optional layout probe: XCAT_INVULN_PROBE=1 (default off).
// Rebind 400ms + 1.5s ACCEPT grace; LU drop keeps SecondaryStat.
// No hotkey — panel / [core] invuln / XCAT_INVULN=1 only.
// Docs: docs/features/invuln/模块设计.md
// Remount 2026-08-03: dump MD5 B87DB932…; UserLocal=ac2e48cc…; field offs UNCHANGED
// (hit 0x298 / layer 0x2A8 / CurPos 0x240 / soft 0x228|0x22C); Unity FindAll via il2cpp_bind.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "invuln.h"

#include "../../ipc/payload_control.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../ports/world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"

#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace invuln {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::ReadPtr;

// True UserLocal → il2cpp_shape::ResolveUserLocalKlass（hash ac2e48cc… + Teleport@0x3C8）

constexpr size_t kOffWmSecondaryStat = 0xF0;  // type e9c12ac2… SecondaryStat (unchanged)
constexpr size_t kOffNInv = 0xEC;
constexpr size_t kOffRInv = 0xF0;
constexpr size_t kOffTInv = 0xF4;
constexpr size_t kOffNDojang = 0x2C4;
constexpr size_t kOffRDojang = 0x2C8;
constexpr size_t kOffTDojang = 0x2CC;
// User (a03443…): m_tHitPeriodRemain / _layerStateCounter — dump.cs.restored 2026-08-03 OK.
// Soft tick-gate +0x228/+0x22C exist but float consumers remain — do not write.
constexpr size_t kOffHitPeriodRemain = 0x298;
constexpr size_t kOffLayerStateCounter = 0x2A8;
constexpr uint32_t kLayerCounterOpaque = 2;
constexpr size_t kOffVisPos = 0x64;       // fad8… MonoBehaviour Vector2
constexpr size_t kOffLogicalPos = 0x240;  // User.CurPos
constexpr float kMinPosAbs = 1.0f;
constexpr size_t kOffCachedPtr = 0x10;

constexpr int kNInv = 1;
constexpr int kRInv = 1010;
constexpr int kNDojang = 1;
constexpr int kRDojang = 1010;
constexpr int kHitPeriodKeep = 5000;
constexpr DWORD kGateRefreshMs = 100;
// Hybrid anti-blink: frame tick is primary; worker backup covers Update races.
constexpr DWORD kAntiBlinkBackupMs = 8;
// Channel-hop / map-load windows often lack MyUser for ~1s; 3s rebind left long gaps.
constexpr DWORD kRebindMs = 400;
// After ACCEPT, spawn pos may be (0,0) briefly — keep bind so hit gate can arm.
constexpr DWORD kBindGraceMs = 1500;
constexpr DWORD kWorkerSleepOnMs = 8;
constexpr DWORD kWorkerSleepOffMs = 16;
constexpr DWORD kProbeMs = 1000;
constexpr DWORD kPumpRetryMs = 2000;
// CMS-named TW candidates (read-only; never write)
constexpr size_t kOffSoftTickA = 0x228;  // TimeEndBoomerangStep / float alias?
constexpr size_t kOffSoftTickB = 0x22C;  // TimeEndDojangBamboo
constexpr size_t kOffCurPosY = 0x244;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;

void* gLuType = nullptr;
void* gLocalUser = nullptr;
DWORD gLuBoundTick = 0;  // GetTickCount at MyUser ACCEPT (grace for spawn pos)
std::vector<void*> gSecondaryStats;  // all WM SS pointers

std::atomic<bool> gDesired{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gFrameBlink{false};
HANDLE gLog = INVALID_HANDLE_VALUE;
HANDLE gLogTemp = INVALID_HANDLE_VALUE;
DWORD gTickCount = 0;

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

void WriteLogHandle(HANDLE h, const char* buf, int n) {
    if (h == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, nullptr);
    FlushFileBuffers(h);
}

void Log(const char* fmt, ...) {
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1024];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond,
                     body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteLogHandle(gLog, buf, n);
    WriteLogHandle(gLogTemp, buf, n);
    OutputDebugStringA(buf);
    x::runtime::LogI("Invuln", "%s", body);
}

bool DirExists(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &self) ||
        !self)
        return L".";
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L".";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return s.substr(0, slash);
}

void OpenLogs() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"invuln.log");
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp)) {
        std::wstring t(tmp);
        while (!t.empty() && t.back() == L'\\') t.pop_back();
        gLogTemp = x::runtime::OpenRotatingDbgLog(t, L"xcat_invuln.log");
    }
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32(void* obj, size_t off, int v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteU32(void* obj, size_t off, uint32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint32_t ReadU32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float ReadF32(void* obj, size_t off) {
    const uint32_t bits = ReadU32(obj, off);
    float f = 0.f;
    memcpy(&f, &bits, sizeof(f));
    return f;
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

bool GetGoName(void* comp, char* out, size_t outCap) {
    out[0] = 0;
    if (!comp || !gCompGo || !gObjName) return false;
    void* go = nullptr;
    __try {
        go = gCompGo(comp, nullptr);
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

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

bool BindApis() {
    if (!x::runtime::il2cpp::Ensure()) {
        Log("BindApis: no GameAssembly");
        return false;
    }
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gCompGo = e.compGo;
    gObjName = e.objName;
    if (!gFindAll) {
        Log("BindApis: missing il2cpp export / RVA");
        return false;
    }
    Log("BindApis ok GA=%p FindAll=%p", gGA, gFindAll);
    return true;
}

int ExpireOrRemainMs() {
    const DWORD now = GetTickCount();
    const int expire = static_cast<int>(now + 3600u * 1000u);
    return expire > 0 ? expire : 0x3FFFFFFF;
}

void WriteSsFields(void* ss, bool on) {
    if (!ss) return;
    if (on) {
        const int t = ExpireOrRemainMs();
        WriteI32(ss, kOffNInv, kNInv);
        WriteI32(ss, kOffRInv, kRInv);
        WriteI32(ss, kOffTInv, t);
        WriteI32(ss, kOffNDojang, kNDojang);
        WriteI32(ss, kOffRDojang, kRDojang);
        WriteI32(ss, kOffTDojang, t);
    } else {
        WriteI32(ss, kOffNInv, 0);
        WriteI32(ss, kOffRInv, 0);
        WriteI32(ss, kOffTInv, 0);
        WriteI32(ss, kOffNDojang, 0);
        WriteI32(ss, kOffRDojang, 0);
        WriteI32(ss, kOffTDojang, 0);
    }
}

bool TryResolveWorldManagers() {
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    void* wm = x::features::ports::world::GetWorldManager();
    if (!wm) return false;
    void* ss = ReadPtr(wm, kOffWmSecondaryStat);
    gSecondaryStats.clear();
    if (ss) {
        gSecondaryStats.push_back(ss);
        Log("WM via world_port wm=%p ss@F0=%p", wm, ss);
    }
    return !gSecondaryStats.empty();
}

bool PosLooksAliveXY(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    return std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs;
}

bool ProbeSsAlive(void* ss) {
    if (!ss) return false;
    __try {
        if (!*reinterpret_cast<void**>(ss)) return false;
        (void)*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ss) + kOffNInv);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SecondaryStatsAlive() {
    if (gSecondaryStats.empty()) return false;
    for (void* ss : gSecondaryStats) {
        if (!ProbeSsAlive(ss)) return false;
    }
    return true;
}

bool InBindGrace() {
    if (!gLuBoundTick) return false;
    return (GetTickCount() - gLuBoundTick) < kBindGraceMs;
}

bool LocalUserStillAlive() {
    if (!gLocalUser) return false;
    __try {
        if (!*reinterpret_cast<void**>(gLocalUser)) return false;
        const intptr_t cached =
            *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffCachedPtr);
        // Do NOT call GetGoName here — it is managed/GC and this runs on a worker.
        // Name was verified at bind time; pos/cachedPtr catch teardown shells.
        // Spawn settle: allow brief (0,0)/cached=0 window after MyUser ACCEPT.
        if (cached == 0 && !InBindGrace()) return false;
        const float visX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos);
        const float visY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos + 4);
        const float logX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos);
        const float logY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos + 4);
        if (!PosLooksAliveXY(visX, visY) && !PosLooksAliveXY(logX, logY)) {
            if (InBindGrace()) return true;
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearLocalUser(const char* why) {
    if (gLocalUser) Log("drop LocalUser (%s) lu=%p", why, gLocalUser);
    gLocalUser = nullptr;
    gLuBoundTick = 0;
}

void ClearSecondaryStats(const char* why) {
    if (!gSecondaryStats.empty())
        Log("drop SS (%s) ssN=%zu", why, gSecondaryStats.size());
    gSecondaryStats.clear();
}

bool TryResolveLocalUser() {
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType || !gFindAll) return false;

    struct Ctx {
        bool ok = false;
    } ctx;
    auto job = [](void* user) {
        auto* c = reinterpret_cast<Ctx*>(user);
        void* arr = nullptr;
        __try {
            arr = gFindAll(gLuType, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("LocalUser FindAll SEH");
            c->ok = false;
            return;
        }
        const uintptr_t n = ArrayLen(arr);
        Log("LocalUser FindAll count=%llu", (unsigned long long)n);
        void* best = nullptr;
        for (uintptr_t i = 0; i < n && i < 64; ++i) {
            void* obj = ArrayAt(arr, i);
            if (!obj) continue;
            char name[96]{};
            GetGoName(obj, name, sizeof(name));
            Log("LocalUser[%llu]=%p name=\"%s\" hit298=%d", (unsigned long long)i, obj, name,
                ReadI32(obj, kOffHitPeriodRemain));
            if (name[0] && _stricmp(name, "MyUser") == 0) {
                best = obj;
                break;
            }
        }
        gLocalUser = best;
        if (!gLocalUser) {
            gLuBoundTick = 0;
            Log("LocalUser REJECT (no MyUser)");
            c->ok = false;
            return;
        }
        gLuBoundTick = GetTickCount();
        Log("LocalUser ACCEPT lu=%p hit298=%d grace=%ums", gLocalUser,
            ReadI32(gLocalUser, kOffHitPeriodRemain), (unsigned)kBindGraceMs);
        x::runtime::managed_main::SetLoginFreeze(false);
        c->ok = true;
    };
    if (!x::runtime::managed_main::Call(+job, &ctx, 2500)) {
        Log("LocalUser Resolve: main pump fail");
        return false;
    }
    return ctx.ok;
}

void ApplySsOnly(bool on) {
    if (on && !SecondaryStatsAlive()) gSecondaryStats.clear();
    if (!on && !SecondaryStatsAlive()) gSecondaryStats.clear();
    for (void* ss : gSecondaryStats) WriteSsFields(ss, on);
}

void ApplyHitGate(bool on) {
    if (!gLocalUser) return;
    WriteI32(gLocalUser, kOffHitPeriodRemain, on ? kHitPeriodKeep : 0);
}

void ApplyAntiBlink() {
    if (!gLocalUser) return;
    // Lightweight: no StillAlive / FindAll — safe for main-thread frame tick.
    WriteU32(gLocalUser, kOffLayerStateCounter, kLayerCounterOpaque);
}

// MainPump sticky tick (after SendWill/Update). Data-plane only.
void AntiBlinkFrameTick(void*) {
    if (!gDesired.load(std::memory_order_relaxed)) return;
    ApplyAntiBlink();
}

bool TryArmFrameBlink(const char* why) {
    if (!x::runtime::main_thread::Ensure()) {
        gFrameBlink.store(false);
        return false;
    }
    x::runtime::main_thread::SetFrameTick(&AntiBlinkFrameTick, nullptr);
    gFrameBlink.store(true);
    Log("anti-blink frame-tick armed (%s)", why ? why : "?");
    return true;
}

void DisarmFrameBlink() {
    x::runtime::main_thread::SetFrameTick(nullptr, nullptr);
    gFrameBlink.store(false);
}

bool ProbeEnabled() {
    char buf[16]{};
    if (GetEnvironmentVariableA("XCAT_INVULN_PROBE", buf, sizeof(buf)) == 0) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

// Read-only: interpret +0x228/+0x22C as both int tick and float; compare CurPos@+0x240.
void ProbeSoftSlots(const char* tag) {
    if (!gLocalUser || !LocalUserStillAlive()) return;
    static float sLastCx = 0.f, sLastCy = 0.f;
    static bool sHaveLast = false;

    const int i228 = ReadI32(gLocalUser, kOffSoftTickA);
    const int i22c = ReadI32(gLocalUser, kOffSoftTickB);
    const float f228 = ReadF32(gLocalUser, kOffSoftTickA);
    const float f22c = ReadF32(gLocalUser, kOffSoftTickB);
    const float cx = ReadF32(gLocalUser, kOffLogicalPos);
    const float cy = ReadF32(gLocalUser, kOffCurPosY);
    const float vx = ReadF32(gLocalUser, kOffVisPos);
    const float vy = ReadF32(gLocalUser, kOffVisPos + 4);
    const int hit = ReadI32(gLocalUser, kOffHitPeriodRemain);
    const uint32_t layer = ReadU32(gLocalUser, kOffLayerStateCounter);

    const char* motion = "idle";
    if (sHaveLast) {
        const float dx = cx - sLastCx;
        const float dy = cy - sLastCy;
        if (dx * dx + dy * dy > 1.0f) motion = "move";
    }
    sLastCx = cx;
    sLastCy = cy;
    sHaveLast = true;

    Log("probe[%s] %s 228 i=%d f=%.6g | 22c i=%d f=%.6g | cur240=(%.2f,%.2f) vis64=(%.2f,%.2f) "
        "hit298=%d layer2A8=%u",
        tag, motion, i228, (double)f228, i22c, (double)f22c, (double)cx, (double)cy, (double)vx,
        (double)vy, hit, layer);
}

void ApplyInvuln(bool on) {
    // LU teardown must not wipe SecondaryStat — SS is independent and still useful mid-hop.
    if (gLocalUser && !LocalUserStillAlive()) {
        ClearLocalUser(on ? "dead before invuln write" : "dead on disable");
    }
    ApplySsOnly(on);
    if (!gLocalUser) return;
    ApplyHitGate(on);
    if (on) ApplyAntiBlink();
}

void LogReadback(const char* tag) {
    Log("%s lu=%p gate=hit hit298=%d layer2A8=%u ssN=%zu desired=%d alive=%d", tag, gLocalUser,
        gLocalUser ? ReadI32(gLocalUser, kOffHitPeriodRemain) : -1,
        gLocalUser ? ReadU32(gLocalUser, kOffLayerStateCounter) : 0u, gSecondaryStats.size(),
        gDesired.load() ? 1 : 0, LocalUserStillAlive() ? 1 : 0);
    for (size_t i = 0; i < gSecondaryStats.size() && i < 4; ++i) {
        void* ss = gSecondaryStats[i];
        Log("  ss[%zu]=%p n/r/t=%d/%d/%d dojang_n=%d", i, ss, ReadI32(ss, kOffNInv),
            ReadI32(ss, kOffRInv), ReadI32(ss, kOffTInv), ReadI32(ss, kOffNDojang));
    }
}

bool EnvWantsOn() {
    char buf[16]{};
    if (GetEnvironmentVariableA("XCAT_INVULN", buf, sizeof(buf)) > 0) {
        if (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T')
            return true;
    }
    return false;
}

void WarnIfSoftEnvRequested() {
    char buf[32]{};
    if (GetEnvironmentVariableA("XCAT_INVULN_GATE", buf, sizeof(buf)) == 0) return;
    if (_stricmp(buf, "soft") == 0 || _stricmp(buf, "228") == 0) {
        Log("XCAT_INVULN_GATE=%s ignored — soft +0x228 writes vanish avatar; using hit gate", buf);
    }
}

// FindAll only when unbound / dead. While StillAlive, reuse the cached binding.
void EnsureBindings() {
    if (!SecondaryStatsAlive()) {
        ClearSecondaryStats("ensure");
        TryResolveWorldManagers();
    }
    if (!LocalUserStillAlive()) {
        ClearLocalUser("ensure");
        TryResolveLocalUser();
    }
}

DWORD WINAPI InvulnThread(LPVOID) {
    Beep(740, 80);
    WarnIfSoftEnvRequested();
    Log("Invuln worker v2.6.3 start (hit=+0x298; anti-blink=frame+backup8ms; rebind=%ums grace=%ums; "
        "probe228 %s)",
        (unsigned)kRebindMs, (unsigned)kBindGraceMs, ProbeEnabled() ? "on" : "off");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gWorkerStop.load(); ++i)
        Sleep(50);
    if (gWorkerStop.load()) {
        DisarmFrameBlink();
        return 0;
    }
    if (!BindApis()) {
        Beep(400, 300);
        DisarmFrameBlink();
        return 1;
    }

    TryArmFrameBlink("boot");

    if (EnvWantsOn()) {
        gDesired.store(true);
        Log("env XCAT_INVULN → desired=1");
    }

    Sleep(1500);
    EnsureBindings();
    if (ProbeEnabled() && gLocalUser) ProbeSoftSlots("boot");

    DWORD lastGate = 0;
    DWORD lastBlink = 0;
    DWORD lastRebind = 0;
    DWORD lastPoll = 0;
    DWORD lastProbe = 0;
    DWORD lastPumpTry = 0;
    DWORD lastHb = GetTickCount();

    while (!gWorkerStop.load()) {
        const DWORD now = GetTickCount();
        const bool on = gDesired.load();

        if (now - lastPoll >= 200) {
            lastPoll = now;
            x::ipc::PayloadControl_Poll();
        }

        if (on && !gFrameBlink.load() && now - lastPumpTry >= kPumpRetryMs) {
            lastPumpTry = now;
            TryArmFrameBlink("retry");
        }

        if (on) {
            const bool luOk = LocalUserStillAlive();
            const bool ssOk = SecondaryStatsAlive();
            if (!luOk || !ssOk) {
                if (!luOk && gLocalUser) ClearLocalUser("stillAlive false");
                if (!ssOk) ClearSecondaryStats("ss dead");
                if (now - lastRebind >= kRebindMs) {
                    lastRebind = now;
                    if (!SecondaryStatsAlive()) TryResolveWorldManagers();
                    if (!LocalUserStillAlive()) TryResolveLocalUser();
                }
            }
            if (now - lastGate >= kGateRefreshMs) {
                lastGate = now;
                ApplyInvuln(true);
            }
            // Always backup-pin while on (covers Update racing past a single frame tick).
            if (now - lastBlink >= kAntiBlinkBackupMs) {
                lastBlink = now;
                ApplyAntiBlink();
            }
            if ((++gTickCount % 200) == 0) LogReadback("refresh");
        }

        // C: read-only layout probe (no FindAll — reuse existing LocalUser bind)
        if (ProbeEnabled() && now - lastProbe >= kProbeMs) {
            lastProbe = now;
            if (gLocalUser && LocalUserStillAlive()) {
                ProbeSoftSlots(on ? "on" : "off");
            } else if (gLocalUser) {
                ClearLocalUser("probe stillAlive false");
            }
        }

        if (now - lastHb >= 5000) {
            lastHb = now;
            Log("heartbeat n=%lu desired=%d gate=hit blink=%s+8ms lu=%p hit298=%d alive=%d", gTickCount,
                on ? 1 : 0, gFrameBlink.load() ? "frame" : "worker", gLocalUser,
                gLocalUser ? ReadI32(gLocalUser, kOffHitPeriodRemain) : -1,
                LocalUserStillAlive() ? 1 : 0);
        }
        Sleep(on ? kWorkerSleepOnMs : kWorkerSleepOffMs);
    }
    if (gLocalUser && !gDesired.load()) ApplyInvuln(false);
    DisarmFrameBlink();
    Log("Invuln worker stop");
    return 0;
}

}  // namespace

void Init() {
    OpenLogs();
    Log("Invuln Init pid=%lu", GetCurrentProcessId());
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gWorkerThread.load() != nullptr) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, InvulnThread, nullptr, 0, nullptr);
    if (!th) {
        Log("CreateThread FAILED err=%lu", GetLastError());
        return;
    }
    gWorkerThread.store(th);
    Log("CreateThread ok");
}

void StopWorker() {
    gWorkerStop.store(true);
    DisarmFrameBlink();
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) CloseHandle(th);
}

void SetDesired(bool on) {
    const bool prev = gDesired.exchange(on);
    if (prev == on) return;
    Log("SetDesired %d", on ? 1 : 0);
    if (on) EnsureBindings();
    ApplyInvuln(on);
}

bool IsDesired() { return gDesired.load(); }
bool IsEnabled() { return gDesired.load() && LocalUserStillAlive(); }

}  // namespace invuln
}  // namespace features
}  // namespace x
