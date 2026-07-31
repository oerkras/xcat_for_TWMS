// TWMS Classic — titlebar feature (port from fengxing, data via DumpRestoredData).
//
// Reads UIStatusBar → CharacterStat / BasicStat (IL2CPP field offsets).
// Writes UnityWndClass title with SendMessageTimeoutW.
// Meso/exp per-minute sliding window (~63s) — no combat feature yet (always sample in-game).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "titlebar.h"

#include "../../runtime/log.h"

#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace titlebar {
namespace {

// UnityEngine.Object.FindObjectsOfTypeAll — same RVA as fly/invuln.
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E413A0;

// DumpRestoredData · restore_tokens_B / runtime dump.cs
constexpr char kUIStatusBarClass[] =
    "af621d5ef51d1e2986b9dbbf2b1bdb56127b019a8fd59b678f35ef11340607e";
constexpr char kLocalUserClass[] =
    "c3f0cabae2a31347606a13c963e006f3d92084a7c7e957b1abf08adcddf59f9";

// UIStatusBar (TW ≠ CMS：BasicStat/CharacterStat 相对 CMS +8)
constexpr size_t kOffBarBasicStat = 0x218;
constexpr size_t kOffBarCharacterStat = 0x220;

// CharacterStat (TypeDefIndex 1833) — DumpRestoredData B
constexpr size_t kOffCsName = 0x18;
constexpr size_t kOffCsLevel = 0x38;
constexpr size_t kOffCsJob = 0x3A;
constexpr size_t kOffCsHp = 0x44;
constexpr size_t kOffCsMhp = 0x46;
constexpr size_t kOffCsMp = 0x48;
constexpr size_t kOffCsMmp = 0x4A;
constexpr size_t kOffCsExp = 0x50;
constexpr size_t kOffCsMoney = 0x58;
constexpr size_t kOffCsNextLevel = 0x80;

// BasicStat — nMHP/nMMP are equipment-adjusted max
constexpr size_t kOffBsNmhp = 0x30;
constexpr size_t kOffBsNmmp = 0x34;

// Nextlevel._nextLevelExp int[]
constexpr size_t kOffNextArr = 0x10;

// LocalUser fallback name
constexpr size_t kOffLuName = 0x1B8;

constexpr DWORD kUpdateIntervalMs = 1000;
constexpr DWORD kRebindMs = 3000;
constexpr DWORD kIdleSleepMs = 50;
constexpr int kRingCap = 64;
constexpr DWORD kRateMinDtMs = 3000;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnDomainGet = void* (*)();
using FnDomainAssemblies = void* (*)(void* domain, size_t* size);
using FnAsmImage = void* (*)(void* assembly);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnDomainGet gDomainGet = nullptr;
FnDomainAssemblies gDomainAssemblies = nullptr;
FnAsmImage gAsmImage = nullptr;
FnClassFromName gClassFromName = nullptr;
FnClassGetType gClassGetType = nullptr;
FnTypeGetObject gTypeGetObject = nullptr;

void* gBarType = nullptr;
void* gLuType = nullptr;
void* gStatusBar = nullptr;
void* gLocalUser = nullptr;

std::atomic<bool> gEnabled{true};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

HWND gHwnd = nullptr;
bool gSavedTitle = false;
std::wstring gOrigTitle;
std::string gLastFullTitle;
bool gHaveLastFullTitle = false;
DWORD gLastUpdate = 0;
DWORD gLastRebind = 0;

struct Sample {
    DWORD tick = 0;
    double expCum = 0.0;
    double meso = 0.0;
};
Sample gRing[kRingCap]{};
int gRingCount = 0;
int gRingHead = 0;
double gExpCum = 0.0;
double gLastExp = 0.0;
double gLastMaxExp = 0.0;
bool gHaveLast = false;
bool gRateActive = false;
DWORD gRateStartTick = 0;
bool gHaveCachedRate = false;
double gCachedExpPer = 0.0;
double gCachedMesoPer = 0.0;

struct Vitals {
    bool ok = false;
    int level = 0;
    int job = 0;
    int hp = 0;
    int mhp = 0;
    int mp = 0;
    int mmp = 0;
    int exp = 0;
    int maxExp = 0;
    long long meso = 0;
    char name[64]{};
};

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int16_t ReadI16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t ReadU8(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int64_t ReadI64(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
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

bool ReadIl2CppStringUtf8(void* str, char* out, size_t outCap) {
    if (!str || !out || outCap < 2) return false;
    __try {
        const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 64) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        const int n = WideCharToMultiByte(CP_UTF8, 0, chars, len, out, static_cast<int>(outCap) - 1,
                                          nullptr, nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* FindClassTypeObject(const char* className) {
    if (!gDomainGet || !gDomainAssemblies || !gAsmImage || !gClassFromName || !gClassGetType ||
        !gTypeGetObject)
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
            if (typeObj) return typeObj;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

bool BindApis() {
    gGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!gGA) {
        x::runtime::LogW("Titlebar", "BindApis: no GameAssembly");
        return false;
    }
    gFindAll = AtRva<FnFindAll>(kRvaFindObjectsOfTypeAll);
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
        x::runtime::LogW("Titlebar", "BindApis: missing export/RVA");
        return false;
    }
    x::runtime::LogI("Titlebar", "BindApis ok GA=%p FindAll=%p", gGA, gFindAll);
    return true;
}

struct FindCtx {
    DWORD pid = 0;
    HWND unity = nullptr;
    HWND fallback = nullptr;
};

BOOL CALLBACK EnumCb(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != c->pid) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    char cls[64]{};
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "UnityWndClass") == 0) {
        c->unity = h;
        return FALSE;
    }
    if (!c->fallback) {
        RECT r{};
        GetWindowRect(h, &r);
        if ((r.right - r.left) > 200 && (r.bottom - r.top) > 200) c->fallback = h;
    }
    return TRUE;
}

HWND FindGameWindow() {
    FindCtx c{GetCurrentProcessId(), nullptr, nullptr};
    EnumWindows(EnumCb, reinterpret_cast<LPARAM>(&c));
    return c.unity ? c.unity : c.fallback;
}

void SetTitleSafe(HWND hwnd, const wchar_t* text, UINT timeoutMs) {
    if (!hwnd || !IsWindow(hwnd) || !text) return;
    DWORD_PTR res = 0;
    SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text),
                        SMTO_ABORTIFHUNG | SMTO_NORMAL, timeoutMs, &res);
}

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string FormatNum(long long v) {
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

std::string FormatCompactAbs(double v) {
    const double av = std::fabs(v);
    if (av < 10000.0) return FormatNum(static_cast<long long>(av + 0.5));
    const char* unit = "万";
    double scaled = av / 10000.0;
    if (av >= 100000000.0) {
        unit = "亿";
        scaled = av / 100000000.0;
    }
    char buf[48]{};
    if (scaled >= 100.0)
        snprintf(buf, sizeof(buf), "%.0f%s", scaled, unit);
    else if (scaled >= 10.0)
        snprintf(buf, sizeof(buf), "%.1f%s", scaled, unit);
    else
        snprintf(buf, sizeof(buf), "%.2f%s", scaled, unit);
    return buf;
}

int ReadNextLevelExp(void* cs, int level) {
    if (!cs || level < 1) return 0;
    void* next = ReadPtr(cs, kOffCsNextLevel);
    if (!next) return 0;
    void* arr = ReadPtr(next, kOffNextArr);
    if (!arr) return 0;
    const uintptr_t n = ArrayLen(arr);
    // Classic table is usually indexed by level (or level-1). Try level first.
    auto at = [&](uintptr_t i) -> int {
        if (i >= n) return 0;
        __try {
            return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(arr) + 0x20 + i * 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    };
    int v = at(static_cast<uintptr_t>(level));
    if (v > 0) return v;
    return at(static_cast<uintptr_t>(level - 1));
}

bool TryResolveStatusBar() {
    if (!gBarType) gBarType = FindClassTypeObject(kUIStatusBarClass);
    if (!gBarType || !gFindAll) return false;
    void* arr = nullptr;
    __try {
        arr = gFindAll(gBarType, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::LogW("Titlebar", "UIStatusBar FindAll SEH");
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    void* best = nullptr;
    for (uintptr_t i = 0; i < n && i < 8; ++i) {
        void* bar = ArrayAt(arr, i);
        if (!bar) continue;
        void* cs = ReadPtr(bar, kOffBarCharacterStat);
        if (!cs) continue;
        const int lv = static_cast<int>(ReadU8(cs, kOffCsLevel));
        const int mhp = static_cast<int>(ReadI16(cs, kOffCsMhp));
        if (lv >= 1 && mhp > 0) {
            best = bar;
            break;
        }
        if (!best) best = bar;
    }
    gStatusBar = best;
    x::runtime::LogI("Titlebar", "UIStatusBar Resolve count=%llu bar=%p",
                     (unsigned long long)n, gStatusBar);
    return gStatusBar != nullptr;
}

bool TryResolveLocalUser() {
    if (!gLuType) gLuType = FindClassTypeObject(kLocalUserClass);
    if (!gLuType || !gFindAll) return false;
    void* arr = nullptr;
    __try {
        arr = gFindAll(gLuType, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    void* best = nullptr;
    for (uintptr_t i = 0; i < n && i < 8; ++i) {
        void* lu = ArrayAt(arr, i);
        if (!lu) continue;
        void* name = ReadPtr(lu, kOffLuName);
        char tmp[64]{};
        if (name && ReadIl2CppStringUtf8(name, tmp, sizeof(tmp))) {
            best = lu;
            break;
        }
        if (!best) best = lu;
    }
    gLocalUser = best;
    return gLocalUser != nullptr;
}

bool ReadVitals(Vitals& out) {
    out = {};
    if (!gStatusBar) return false;
    void* cs = ReadPtr(gStatusBar, kOffBarCharacterStat);
    if (!cs) return false;
    out.level = static_cast<int>(ReadU8(cs, kOffCsLevel));
    out.job = static_cast<int>(ReadI16(cs, kOffCsJob));
    out.hp = static_cast<int>(ReadI16(cs, kOffCsHp));
    out.mhp = static_cast<int>(ReadI16(cs, kOffCsMhp));
    out.mp = static_cast<int>(ReadI16(cs, kOffCsMp));
    out.mmp = static_cast<int>(ReadI16(cs, kOffCsMmp));
    out.exp = ReadI32(cs, kOffCsExp);
    out.meso = ReadI64(cs, kOffCsMoney);
    out.maxExp = ReadNextLevelExp(cs, out.level);

    void* bs = ReadPtr(gStatusBar, kOffBarBasicStat);
    if (bs) {
        const int nmhp = ReadI32(bs, kOffBsNmhp);
        const int nmmp = ReadI32(bs, kOffBsNmmp);
        if (nmhp > out.mhp) out.mhp = nmhp;
        if (nmmp > out.mmp) out.mmp = nmmp;
    }

    void* nameObj = ReadPtr(cs, kOffCsName);
    if (!nameObj || !ReadIl2CppStringUtf8(nameObj, out.name, sizeof(out.name))) {
        if (gLocalUser) {
            void* luName = ReadPtr(gLocalUser, kOffLuName);
            (void)ReadIl2CppStringUtf8(luName, out.name, sizeof(out.name));
        }
    }

    out.ok = out.level >= 1 && out.mhp > 0;
    return out.ok;
}

void ResetRates() {
    gRingCount = 0;
    gRingHead = 0;
    gExpCum = 0.0;
    gLastExp = 0.0;
    gLastMaxExp = 0.0;
    gHaveLast = false;
    gRateActive = false;
    gRateStartTick = 0;
    gHaveCachedRate = false;
    gCachedExpPer = 0.0;
    gCachedMesoPer = 0.0;
}

void PushSample(DWORD now, double meso) {
    gRing[gRingHead] = Sample{now, gExpCum, meso};
    gRingHead = (gRingHead + 1) % kRingCap;
    if (gRingCount < kRingCap) ++gRingCount;
}

bool ComputeRates(DWORD now, double& expPer, double& mesoPer) {
    if (gRingCount < 2) return false;
    const int oldestIdx = (gRingHead - gRingCount + kRingCap) % kRingCap;
    const Sample& oldest = gRing[oldestIdx];
    const DWORD dt = now - oldest.tick;
    if (dt < kRateMinDtMs) return false;
    const double dtMs = static_cast<double>(dt);
    expPer = (gExpCum - oldest.expCum) / dtMs * 60000.0;
    mesoPer = (gRing[(gRingHead + kRingCap - 1) % kRingCap].meso - oldest.meso) / dtMs * 60000.0;
    if (mesoPer < 0.0) mesoPer = 0.0;
    if (expPer < 0.0) expPer = 0.0;
    return true;
}

void UpdateRates(DWORD now, const Vitals& v) {
    if (!v.ok || v.maxExp <= 0) {
        if (gRateActive) {
            x::runtime::LogI("Titlebar", "rate stop invalid_stats samples=%d", gRingCount);
            ResetRates();
        }
        return;
    }
    const double meso = static_cast<double>(v.meso);
    const double exp = static_cast<double>(v.exp);
    const double maxExp = static_cast<double>(v.maxExp);

    if (!gRateActive) {
        gRateActive = true;
        gRateStartTick = now;
        gExpCum = 0.0;
        gLastExp = exp;
        gLastMaxExp = maxExp;
        gHaveLast = true;
        gRingCount = 0;
        gRingHead = 0;
        PushSample(now, meso);
        x::runtime::LogI("Titlebar", "rate start meso=%lld exp=%d maxExp=%d", v.meso, v.exp,
                         v.maxExp);
        return;
    }

    if (gHaveLast) {
        if (maxExp > 0.0 && gLastMaxExp > 0.0 && maxExp != gLastMaxExp) {
            // Level-up: previous max was remaining-to-level budget consumed.
            if (exp < gLastExp) gExpCum += gLastMaxExp - gLastExp + exp;
            else gExpCum += exp - gLastExp;
        } else if (exp >= gLastExp) {
            gExpCum += exp - gLastExp;
        } else {
            // Unexpected drop without maxExp change — treat as reset, don't subtract.
        }
    }
    gLastExp = exp;
    gLastMaxExp = maxExp;
    gHaveLast = true;
    PushSample(now, meso);

    double expPer = 0.0, mesoPer = 0.0;
    if (ComputeRates(now, expPer, mesoPer)) {
        gCachedExpPer = expPer;
        gCachedMesoPer = mesoPer;
        gHaveCachedRate = true;
    }
}

std::string BuildTitle(DWORD now, const Vitals& v) {
    char buf[512]{};
    const char* name = v.name[0] ? v.name : "?";
    snprintf(buf, sizeof(buf),
             "Lv.%d %s  职业:%d    HP %d/%d    MP %d/%d    EXP %d/%d    背包金 %lld", v.level, name,
             v.job, v.hp, v.mhp, v.mp, v.mmp, v.exp, v.maxExp, v.meso);

    std::string title(buf);
    if (gHaveCachedRate) {
        title += "    +";
        title += FormatCompactAbs(gCachedMesoPer);
        title += " 金/分    +";
        title += FormatCompactAbs(gCachedExpPer);
        title += " 经/分";
    } else if (gRateActive) {
        const DWORD elapsed = now - gRateStartTick;
        if (elapsed < kRateMinDtMs) {
            char wait[48]{};
            snprintf(wait, sizeof(wait), "    收益采样 %u/3s",
                     static_cast<unsigned>((elapsed + 999) / 1000));
            title += wait;
        } else {
            title += "    收益等待数据";
        }
    }
    return title;
}

void Tick(DWORD now) {
    if (!gEnabled.load()) return;
    if (gLastUpdate && now - gLastUpdate < kUpdateIntervalMs) return;
    gLastUpdate = now;

    if (!gHwnd || !IsWindow(gHwnd)) {
        gHwnd = FindGameWindow();
        if (!gHwnd) return;
        wchar_t cur[256]{};
        GetWindowTextW(gHwnd, cur, 256);
        gOrigTitle = cur;
        gSavedTitle = true;
    }

    if ((!gStatusBar) || (now - gLastRebind >= kRebindMs)) {
        gLastRebind = now;
        TryResolveStatusBar();
        TryResolveLocalUser();
    }

    Vitals v{};
    if (!ReadVitals(v)) {
        // Lobby / loading: keep last title if any.
        return;
    }

    UpdateRates(now, v);
    const std::string full = BuildTitle(now, v);
    if (gHaveLastFullTitle && full == gLastFullTitle) return;
    gLastFullTitle = full;
    gHaveLastFullTitle = true;
    SetTitleSafe(gHwnd, Utf8ToWide(full.c_str()).c_str(), 200);
}

void RestoreTitle() {
    if (gHwnd && IsWindow(gHwnd) && gSavedTitle) {
        SetTitleSafe(gHwnd, gOrigTitle.c_str(), 500);
    }
    gHwnd = nullptr;
    gSavedTitle = false;
    gHaveLastFullTitle = false;
    gLastFullTitle.clear();
}

DWORD WINAPI TitlebarThread(LPVOID) {
    timeBeginPeriod(1);
    x::runtime::LogI("Titlebar", "worker start (UIStatusBar→CharacterStat)");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gWorkerStop.load(); ++i)
        Sleep(50);
    if (gWorkerStop.load()) {
        timeEndPeriod(1);
        return 0;
    }
    if (!BindApis()) {
        timeEndPeriod(1);
        return 1;
    }

    Sleep(2000);
    TryResolveStatusBar();
    TryResolveLocalUser();

    while (!gWorkerStop.load()) {
        Tick(GetTickCount());
        Sleep(kIdleSleepMs);
    }
    RestoreTitle();
    x::runtime::LogI("Titlebar", "worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    gEnabled.store(true);
    ResetRates();
    gStatusBar = nullptr;
    gLocalUser = nullptr;
    gHwnd = nullptr;
    gSavedTitle = false;
    gHaveLastFullTitle = false;
    gLastFullTitle.clear();
    gLastUpdate = 0;
    gLastRebind = 0;
}

void Shutdown() {
    gEnabled.store(false);
    RestoreTitle();
    ResetRates();
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, TitlebarThread, nullptr, 0, nullptr);
    gWorkerThread.store(th);
}

void StopWorker() {
    gWorkerStop.store(true);
    // Do not join under loader lock (DllMain detach).
}

void SetEnabled(bool on) {
    const bool prev = gEnabled.exchange(on);
    if (prev == on) return;
    if (!on) {
        RestoreTitle();
        ResetRates();
    } else {
        ResetRates();
        gLastUpdate = 0;
        gHaveLastFullTitle = false;
        gLastFullTitle.clear();
    }
    x::runtime::LogI("Titlebar", "enabled=%d", on ? 1 : 0);
}

bool IsEnabled() { return gEnabled.load(); }

}  // namespace titlebar
}  // namespace features
}  // namespace x
