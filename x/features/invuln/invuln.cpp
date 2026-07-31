// TWMS Classic — data-plane invuln v2.3.
//
// Invuln: User+0x298 (m_tHitPeriodRemain) — SetDamaged early-out. Refresh ~100ms.
// Anti-blink: User+0x2A8 (_layerStateCounter) pin to opaque phase. Refresh ~16ms.
// SecondaryStat Invincible fields are dual-written but do NOT gate SetDamaged.
// Hotkey F10 (game-foreground only) / env XCAT_INVULN=1. F9 = panel minimize.
// FindAll only when binding lost (throttled); reuse while LocalUserStillAlive.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "invuln.h"

#include "../../ipc/payload_control.h"
#include "../../runtime/log.h"

#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <timeapi.h>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace invuln {
namespace {

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E413A0;
constexpr uint32_t kRvaCompGetTransform = 0x4E496A0;
constexpr uint32_t kRvaCompGetGo = 0x4E49780;
constexpr uint32_t kRvaObjGetName = 0x4E566E0;

constexpr char kWorldManagerClass[] =
    "a480358a12395b670df55f0b0ac5c5d89f6ba74b93fae115e43b4007e546a7a";
constexpr char kLocalUserClass[] =
    "c3f0cabae2a31347606a13c963e006f3d92084a7c7e957b1abf08adcddf59f9";

constexpr size_t kOffWmSecondaryStat = 0xF0;
constexpr size_t kOffNInv = 0xEC;
constexpr size_t kOffRInv = 0xF0;
constexpr size_t kOffTInv = 0xF4;
constexpr size_t kOffNDojang = 0x2C4;
constexpr size_t kOffRDojang = 0x2C8;
constexpr size_t kOffTDojang = 0x2CC;
// TW IDA: SetDamaged reads/writes [rsi+298h] as m_tHitPeriodRemain (cms was 0x280).
constexpr size_t kOffHitPeriodRemain = 0x298;
// User.Update blink: while hitPeriod>0, ++_layerStateCounter; color dimmed when (n&3)<2.
constexpr size_t kOffLayerStateCounter = 0x2A8;
// Keep (counter&3) >= 2 → full opaque (IDA opaque cmp resolves to 2).
constexpr uint32_t kLayerCounterOpaque = 2;
// Same LocalUser layout as fly (vis / CurPos) — used only for StillAlive probes.
constexpr size_t kOffVisPos = 0x64;
constexpr size_t kOffLogicalPos = 0x240;
constexpr float kMinPosAbs = 1.0f;
// UnityEngine.Object.m_CachedPtr after Il2CppObject header (klass+monitor).
constexpr size_t kOffCachedPtr = 0x10;

constexpr int kNInv = 1;
constexpr int kRInv = 1010;
constexpr int kNDojang = 1;
constexpr int kRDojang = 1010;
// Keep i-frame gate latched; game decrements each frame — top up infrequently.
constexpr int kHitPeriodKeep = 5000;
constexpr DWORD kHitPeriodRefreshMs = 100;
// Anti-blink: pin layer counter ~once per frame (not 1ms spam).
constexpr DWORD kAntiBlinkMs = 16;
// FindAll throttle when unbound / after map teardown (never while StillAlive).
constexpr DWORD kRebindMs = 3000;
constexpr DWORD kIdleSleepMs = 16;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnDomainGet = void* (*)();
using FnDomainAssemblies = void* (*)(void* domain, size_t* size);
using FnAsmImage = void* (*)(void* assembly);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);
using FnCompTf = void* (*)(void* comp, void* methodInfo);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnDomainGet gDomainGet = nullptr;
FnDomainAssemblies gDomainAssemblies = nullptr;
FnAsmImage gAsmImage = nullptr;
FnClassFromName gClassFromName = nullptr;
FnClassGetType gClassGetType = nullptr;
FnTypeGetObject gTypeGetObject = nullptr;
FnCompTf gCompTf = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;

void* gWmType = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
std::vector<void*> gSecondaryStats;  // all WM SS pointers

std::atomic<bool> gDesired{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
bool gF10Down = false;
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
    const std::wstring path = dir + L"\\invuln.log";
    gLog = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp)) {
        std::wstring t = std::wstring(tmp) + L"xcat_invuln.log";
        gLogTemp = CreateFileW(t.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
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

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

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

bool BindApis() {
    gGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!gGA) {
        Log("BindApis: no GameAssembly");
        return false;
    }
    gFindAll = AtRva<FnFindAll>(kRvaFindObjectsOfTypeAll);
    gCompTf = AtRva<FnCompTf>(kRvaCompGetTransform);
    gCompGo = AtRva<FnCompGo>(kRvaCompGetGo);
    gObjName = AtRva<FnObjName>(kRvaObjGetName);
    gDomainGet = reinterpret_cast<FnDomainGet>(GetProcAddress(gGA, "il2cpp_domain_get"));
    gDomainAssemblies =
        reinterpret_cast<FnDomainAssemblies>(GetProcAddress(gGA, "il2cpp_domain_get_assemblies"));
    gAsmImage = reinterpret_cast<FnAsmImage>(GetProcAddress(gGA, "il2cpp_assembly_get_image"));
    gClassFromName =
        reinterpret_cast<FnClassFromName>(GetProcAddress(gGA, "il2cpp_class_from_name"));
    gClassGetType = reinterpret_cast<FnClassGetType>(GetProcAddress(gGA, "il2cpp_class_get_type"));
    gTypeGetObject =
        reinterpret_cast<FnTypeGetObject>(GetProcAddress(gGA, "il2cpp_type_get_object"));
    if (!gFindAll || !gDomainGet || !gDomainAssemblies || !gAsmImage || !gClassFromName ||
        !gClassGetType || !gTypeGetObject) {
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
    if (!gWmType) gWmType = FindClassTypeObject(kWorldManagerClass);
    if (!gWmType || !gFindAll) return false;

    void* arr = nullptr;
    __try {
        arr = gFindAll(gWmType, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("WM FindAll SEH");
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    gSecondaryStats.clear();
    for (uintptr_t i = 0; i < n && i < 16; ++i) {
        void* wm = ArrayAt(arr, i);
        if (!wm) continue;
        void* ss = ReadPtr(wm, kOffWmSecondaryStat);
        Log("WM[%llu]=%p ss@F0=%p", (unsigned long long)i, wm, ss);
        if (ss) gSecondaryStats.push_back(ss);
    }
    Log("WM Resolve count=%llu ssN=%zu", (unsigned long long)n, gSecondaryStats.size());
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

bool LocalUserStillAlive() {
    if (!gLocalUser) return false;
    __try {
        if (!*reinterpret_cast<void**>(gLocalUser)) return false;
        const intptr_t cached =
            *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffCachedPtr);
        if (cached == 0) return false;
        char name[96]{};
        if (!GetGoName(gLocalUser, name, sizeof(name))) return false;
        if (_stricmp(name, "MyUser") != 0) return false;
        const float visX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos);
        const float visY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos + 4);
        const float logX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos);
        const float logY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos + 4);
        // Map teardown often leaves a readable shell at (0,0); reject like fly.
        if (!PosLooksAliveXY(visX, visY) && !PosLooksAliveXY(logX, logY)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DropStaleBindings(const char* why) {
    if (gLocalUser || !gSecondaryStats.empty())
        Log("drop bindings (%s) lu=%p ssN=%zu", why, gLocalUser, gSecondaryStats.size());
    gLocalUser = nullptr;
    gSecondaryStats.clear();
}

bool TryResolveLocalUser() {
    if (!gLuType) gLuType = FindClassTypeObject(kLocalUserClass);
    if (!gLuType || !gFindAll) return false;

    void* arr = nullptr;
    __try {
        arr = gFindAll(gLuType, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("LocalUser FindAll SEH");
        return false;
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
    // No first-object fallback — wrong actor writes are worse than waiting.
    gLocalUser = best;
    if (!gLocalUser) {
        Log("LocalUser REJECT (no MyUser)");
        return false;
    }
    Log("LocalUser ACCEPT lu=%p hit298=%d", gLocalUser, ReadI32(gLocalUser, kOffHitPeriodRemain));
    return true;
}

void ApplyHitPeriodAndSs(bool on) {
    if (on) {
        if (!LocalUserStillAlive()) {
            DropStaleBindings("dead before hit write");
            return;
        }
        if (!SecondaryStatsAlive()) {
            // SS is non-gating; drop stale list but still latch hit period.
            gSecondaryStats.clear();
        }
    } else {
        // Clear only live objects — never write into freed shells.
        if (!LocalUserStillAlive()) gLocalUser = nullptr;
        if (!SecondaryStatsAlive()) gSecondaryStats.clear();
    }
    for (void* ss : gSecondaryStats) WriteSsFields(ss, on);
    if (!gLocalUser) return;
    WriteI32(gLocalUser, kOffHitPeriodRemain, on ? kHitPeriodKeep : 0);
}

void ApplyAntiBlink() {
    if (!LocalUserStillAlive()) {
        DropStaleBindings("dead before anti-blink");
        return;
    }
    WriteU32(gLocalUser, kOffLayerStateCounter, kLayerCounterOpaque);
}

void ApplyInvuln(bool on) {
    ApplyHitPeriodAndSs(on);
    if (on) ApplyAntiBlink();
}

void LogReadback(const char* tag) {
    Log("%s lu=%p hit298=%d layer2A8=%u ssN=%zu desired=%d alive=%d", tag, gLocalUser,
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

// FindAll only when unbound / dead. While StillAlive, reuse — same discipline as fly.
void EnsureBindings() {
    if (!SecondaryStatsAlive()) {
        gSecondaryStats.clear();
        TryResolveWorldManagers();
    }
    if (!LocalUserStillAlive()) {
        gLocalUser = nullptr;
        TryResolveLocalUser();
    }
}

bool GameWindowFocused() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void PollF10() {
    // Process-wide GetAsyncKeyState — without foreground gate, F10 in other apps toggles.
    if (!GameWindowFocused()) {
        gF10Down = false;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !gF10Down) {
        const bool next = !gDesired.load();
        Log("F10 toggle desired=%d", next ? 1 : 0);
        Beep(next ? 1000 : 600, 80);
        EnsureBindings();
        x::ipc::PayloadControl_PublishInvuln(next);
        LogReadback(next ? "after_on" : "after_off");
    }
    gF10Down = down;
}

DWORD WINAPI InvulnThread(LPVOID) {
    timeBeginPeriod(1);
    Beep(740, 80);
    Log("Invuln worker v2.3 start (StillAlive reuse; FindAll only when unbound; F10 fg)");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gWorkerStop.load(); ++i)
        Sleep(50);
    if (gWorkerStop.load()) {
        timeEndPeriod(1);
        return 0;
    }
    if (!BindApis()) {
        Beep(400, 300);
        timeEndPeriod(1);
        return 1;
    }

    if (EnvWantsOn()) {
        gDesired.store(true);
        Log("env XCAT_INVULN → desired=1");
    }

    Sleep(1500);
    EnsureBindings();

    DWORD lastHit = 0;
    DWORD lastBlink = 0;
    DWORD lastRebind = 0;
    DWORD lastPoll = 0;
    DWORD lastHb = GetTickCount();

    while (!gWorkerStop.load()) {
        PollF10();
        const DWORD now = GetTickCount();
        const bool on = gDesired.load();

        if (now - lastPoll >= 200) {
            lastPoll = now;
            x::ipc::PayloadControl_Poll();
        }

        if (on) {
            const bool luOk = LocalUserStillAlive();
            const bool ssOk = SecondaryStatsAlive();
            if (!luOk || !ssOk) {
                if (!luOk) gLocalUser = nullptr;
                if (!ssOk) gSecondaryStats.clear();
                // Throttle FindAll — fly warns worker FindAll can freeze Unity main loop.
                if (now - lastRebind >= kRebindMs) {
                    lastRebind = now;
                    if (!ssOk) TryResolveWorldManagers();
                    if (!LocalUserStillAlive()) TryResolveLocalUser();
                }
            }
            if (now - lastHit >= kHitPeriodRefreshMs) {
                lastHit = now;
                ApplyHitPeriodAndSs(true);
            }
            if (now - lastBlink >= kAntiBlinkMs) {
                lastBlink = now;
                ApplyAntiBlink();
                if ((++gTickCount % 300) == 0) LogReadback("refresh");
            }
        }

        if (now - lastHb >= 5000) {
            lastHb = now;
            Log("heartbeat n=%lu desired=%d lu=%p hit298=%d layer=%u ssN=%zu alive=%d", gTickCount,
                on ? 1 : 0, gLocalUser,
                gLocalUser ? ReadI32(gLocalUser, kOffHitPeriodRemain) : -1,
                gLocalUser ? ReadU32(gLocalUser, kOffLayerStateCounter) : 0u, gSecondaryStats.size(),
                LocalUserStillAlive() ? 1 : 0);
        }
        Sleep(kIdleSleepMs);
    }
    if (gLocalUser && !gDesired.load()) ApplyInvuln(false);
    Log("Invuln worker stop");
    timeEndPeriod(1);
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
void Toggle() { SetDesired(!IsDesired()); }

}  // namespace invuln
}  // namespace features
}  // namespace x
