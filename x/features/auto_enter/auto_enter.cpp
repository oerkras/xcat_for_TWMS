// TWMS Classic — auto enter (world → least channel → character).
//
// Calls official UI methods on the Unity main thread via MethodInfo swap of
// Canvas.SendWillRenderCanvases (no E9 / no GA .text patch).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "auto_enter.h"

#include "../../ipc/payload_control.h"
#include "../../runtime/log.h"

#include <Psapi.h>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace auto_enter {
namespace {

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E413A0;
constexpr uint32_t kRvaSendWillRenderCanvases = 0x523C830;

constexpr uint32_t kRvaOnClickWorldItem = 0xA9ACA0;
constexpr uint32_t kRvaSelectChannel = 0xA93B40;
constexpr uint32_t kRvaEnterChannel = 0xA94180;
constexpr uint32_t kRvaSelectCharacter = 0xA80000;
constexpr uint32_t kRvaOnClickButtonSelect = 0xA81440;
constexpr uint32_t kRvaGetAvatarCount = 0xA8BC50;
constexpr uint32_t kRvaIsSlotEnable = 0xA876B0;

constexpr char kClassUiLoginWorld[] =
    "d77914da56b5e08caf0f9d5c5f01400d3239d1936044f62b51c89fba85563e5";
constexpr char kClassUiLoginCharacter[] =
    "fe8f1a75891cefc067b2a016b38e58975e2dcb6551f371ab1fe6958aa76f93c";
constexpr char kClassUiLoginChannel[] = "UILoginChannel";

constexpr size_t kOffWorldItems = 0x50;
constexpr size_t kOffWorldId = 0x10;
constexpr size_t kOffWorldName = 0x18;
constexpr size_t kOffWorldChannels = 0x38;
constexpr size_t kOffChUserNo = 0x18;
constexpr size_t kOffChChannelId = 0x1D;
constexpr size_t kOffChAdult = 0x1E;
constexpr size_t kOffChCapacity = 0x20;
constexpr size_t kOffChannelSelectedWorld = 0x78;
constexpr size_t kOffCharAvatarList = 0x170;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffArrLen = 0x18;
constexpr size_t kOffArrData = 0x20;

constexpr DWORD kTickMs = 80;
constexpr DWORD kJobWaitMs = 4000;
constexpr DWORD kPhaseTimeoutMs = 45000;
constexpr DWORD kLogThrottleMs = 3000;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnDomainGet = void* (*)();
using FnDomainAssemblies = void* (*)(void* domain, size_t* size);
using FnAsmImage = void* (*)(void* assembly);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnSendWill = void (*)(const void* methodInfo);
using FnClickWorld = void (*)(void* self, int index, const void* methodInfo);
using FnSelectChannel = void (*)(void* self, int channelId, const void* methodInfo);
using FnEnterChannel = void (*)(void* self, int channelId, const void* methodInfo);
using FnSelectChar = void (*)(void* self, int index, bool first, const void* methodInfo);
using FnClickSelect = void (*)(void* self, const void* methodInfo);
using FnGetAvatarCount = int (*)(void* self, const void* methodInfo);
using FnIsSlotEnable = bool (*)(void* self, int index, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

enum class Phase : uint8_t {
    Idle = 0,
    WaitWorldList,
    PickWorld,
    WaitChannelUi,
    PickChannel,
    WaitCharSelect,
    PickChar,
    Done,
    Failed,
};

enum class JobKind : uint8_t {
    None = 0,
    ClickWorld,
    EnterChannel,
    SelectChar,
};

HMODULE gGA = nullptr;
uintptr_t gGaBase = 0;
FnFindAll gFindAll = nullptr;
FnDomainGet gDomainGet = nullptr;
FnDomainAssemblies gDomainAssemblies = nullptr;
FnAsmImage gAsmImage = nullptr;
FnClassFromName gClassFromName = nullptr;
FnClassGetType gClassGetType = nullptr;
FnTypeGetObject gTypeGetObject = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;

void* gTypeWorld = nullptr;
void* gTypeChannel = nullptr;
void* gTypeChar = nullptr;
void* gKlassWorld = nullptr;
void* gKlassChannel = nullptr;
void* gKlassChar = nullptr;
void* gKlassCanvas = nullptr;

MethodInfoHead* gMiSendWill = nullptr;
FnSendWill gOrigSendWill = nullptr;
std::atomic<bool> gPumpInstalled{false};
std::atomic<bool> gInPump{false};

std::atomic<bool> gDesired{false};
std::atomic<int32_t> gWorldId{0};
std::atomic<uint32_t> gCharSlot{1};
char gWorldName[64]{};
CRITICAL_SECTION gNameCs{};
bool gNameCsInit = false;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

Phase gPhase = Phase::Idle;
DWORD gPhaseSince = 0;
void* gPickedWorld = nullptr;
int gPickedWorldIndex = -1;
int gPickedChannelId = -1;
DWORD gLastLogMs = 0;

JobKind gJobKind = JobKind::None;
void* gJobUi = nullptr;
int gJobA = 0;
int gJobB = 0;
std::atomic<bool> gJobPending{false};
std::atomic<bool> gJobDone{false};
std::atomic<bool> gJobOk{false};
CRITICAL_SECTION gJobCs{};
bool gJobCsInit = false;

HANDLE gLog = INVALID_HANDLE_VALUE;

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
    OutputDebugStringA(buf);
    x::runtime::LogI("AutoEnter", "%s", body);
}

void LogThrottled(const char* fmt, ...) {
    const DWORD now = GetTickCount();
    if (now - gLastLogMs < kLogThrottleMs) return;
    gLastLogMs = now;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("%s", buf);
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
    const std::wstring path = dir + L"\\auto_enter.log";
    gLog = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void EnsureCs() {
    if (!gNameCsInit) {
        InitializeCriticalSection(&gNameCs);
        gNameCsInit = true;
    }
    if (!gJobCsInit) {
        InitializeCriticalSection(&gJobCs);
        gJobCsInit = true;
    }
}

void CopyWorldNameLocked(const char* src) {
    EnsureCs();
    EnterCriticalSection(&gNameCs);
    gWorldName[0] = 0;
    if (src && src[0]) strncpy_s(gWorldName, src, _TRUNCATE);
    LeaveCriticalSection(&gNameCs);
}

void SnapshotWorldName(char* out, size_t cap) {
    EnsureCs();
    EnterCriticalSection(&gNameCs);
    strncpy_s(out, cap, gWorldName, _TRUNCATE);
    LeaveCriticalSection(&gNameCs);
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
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

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadIl2CppStringUtf8(void* str, char* out, size_t outCap) {
    if (!str || !out || outCap < 2) return false;
    __try {
        const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        const int n = WideCharToMultiByte(CP_UTF8, 0, chars, len, out, (int)outCap - 1, nullptr, nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ListSize(void* list) {
    if (!list) return 0;
    return ReadI32(list, kOffListSize);
}

void* ListAt(void* list, int index) {
    if (!list || index < 0) return nullptr;
    void* items = ReadPtr(list, kOffListItems);
    if (!items) return nullptr;
    const int size = ListSize(list);
    if (index >= size) return nullptr;
    __try {
        const uintptr_t len = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(items) + kOffArrLen);
        if ((uintptr_t)index >= len) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + kOffArrData +
                                         (uintptr_t)index * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uintptr_t ArrayLen(void* arr) {
    if (!arr) return 0;
    __try {
        return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + kOffArrLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* ArrayAt(void* arr, uintptr_t i) {
    if (!arr) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData + i * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* FindClass(const char* ns, const char* name) {
    if (!gDomainGet || !gDomainAssemblies || !gAsmImage || !gClassFromName || !name) return nullptr;
    __try {
        void* domain = gDomainGet();
        if (!domain) return nullptr;
        size_t n = 0;
        void** asms = reinterpret_cast<void**>(gDomainAssemblies(domain, &n));
        if (!asms || n == 0) return nullptr;
        for (size_t i = 0; i < n; ++i) {
            void* image = gAsmImage(asms[i]);
            if (!image) continue;
            void* klass = gClassFromName(image, ns ? ns : "", name);
            if (klass) return klass;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void* TypeObjectFromKlass(void* klass) {
    if (!klass || !gClassGetType || !gTypeGetObject) return nullptr;
    __try {
        void* type = gClassGetType(klass);
        if (!type) return nullptr;
        return gTypeGetObject(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* FindFirstOfType(void* typeObj) {
    if (!typeObj || !gFindAll) return nullptr;
    void* arr = nullptr;
    __try {
        arr = gFindAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    const uintptr_t n = ArrayLen(arr);
    for (uintptr_t i = 0; i < n; ++i) {
        void* o = ArrayAt(arr, i);
        if (o) return o;
    }
    return nullptr;
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGaBase || !rva) return nullptr;
    void* target = reinterpret_cast<void*>(gGaBase + rva);
    void* iter = nullptr;
    for (;;) {
        void* miRaw = nullptr;
        __try {
            miRaw = gClassGetMethods(klass, &iter);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!miRaw) break;
        auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
        void* mp = nullptr;
        void* vp = nullptr;
        __try {
            mp = mi->methodPointer;
            vp = mi->virtualMethodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (mp == target || vp == target) return mi;
    }
    return nullptr;
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        if (mi->virtualMethodPointer == orig) mi->virtualMethodPointer = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return ok;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    __try {
        void* cur = mi->methodPointer;
        mi->methodPointer = orig;
        if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

bool ResolveTypes() {
    if (!gKlassWorld) gKlassWorld = FindClass("", kClassUiLoginWorld);
    if (!gKlassChannel) gKlassChannel = FindClass("", kClassUiLoginChannel);
    if (!gKlassChar) gKlassChar = FindClass("", kClassUiLoginCharacter);
    if (!gTypeWorld && gKlassWorld) gTypeWorld = TypeObjectFromKlass(gKlassWorld);
    if (!gTypeChannel && gKlassChannel) gTypeChannel = TypeObjectFromKlass(gKlassChannel);
    if (!gTypeChar && gKlassChar) gTypeChar = TypeObjectFromKlass(gKlassChar);
    return gTypeWorld && gTypeChannel && gTypeChar;
}

bool BindApis() {
    gGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!gGA) {
        Log("BindApis: no GameAssembly");
        return false;
    }
    gGaBase = reinterpret_cast<uintptr_t>(gGA);
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
    gClassGetMethods =
        reinterpret_cast<FnClassGetMethods>(GetProcAddress(gGA, "il2cpp_class_get_methods"));
    if (!gFindAll || !gDomainGet || !gDomainAssemblies || !gAsmImage || !gClassFromName ||
        !gClassGetType || !gTypeGetObject || !gClassGetMethods) {
        Log("BindApis: missing export/RVA");
        return false;
    }
    Log("BindApis ok GA=%p", gGA);
    return true;
}

void RunJobOnMain() {
    EnsureCs();
    EnterCriticalSection(&gJobCs);
    const JobKind kind = gJobKind;
    void* ui = gJobUi;
    const int a = gJobA;
    LeaveCriticalSection(&gJobCs);

    bool ok = false;
    __try {
        switch (kind) {
        case JobKind::ClickWorld: {
            auto* mi = FindMethodByRva(gKlassWorld, kRvaOnClickWorldItem);
            auto fn = AtRva<FnClickWorld>(kRvaOnClickWorldItem);
            if (ui && fn) {
                fn(ui, a, mi);
                ok = true;
            }
            break;
        }
        case JobKind::EnterChannel: {
            auto* miSel = FindMethodByRva(gKlassChannel, kRvaSelectChannel);
            auto* miEnt = FindMethodByRva(gKlassChannel, kRvaEnterChannel);
            auto fnSel = AtRva<FnSelectChannel>(kRvaSelectChannel);
            auto fnEnt = AtRva<FnEnterChannel>(kRvaEnterChannel);
            if (ui && fnSel && fnEnt) {
                fnSel(ui, a, miSel);
                fnEnt(ui, a, miEnt);
                ok = true;
            }
            break;
        }
        case JobKind::SelectChar: {
            auto* miSel = FindMethodByRva(gKlassChar, kRvaSelectCharacter);
            auto* miClick = FindMethodByRva(gKlassChar, kRvaOnClickButtonSelect);
            auto fnSel = AtRva<FnSelectChar>(kRvaSelectCharacter);
            auto fnClick = AtRva<FnClickSelect>(kRvaOnClickButtonSelect);
            if (ui && fnSel && fnClick) {
                // Second arg defaults to false in dump; confirm is OnClickButtonSelect.
                fnSel(ui, a, false, miSel);
                fnClick(ui, miClick);
                ok = true;
            }
            break;
        }
        default:
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        Log("RunJob SEH kind=%u", (unsigned)kind);
    }

    gJobOk.store(ok);
    gJobDone.store(true);
    gJobPending.store(false);
}

void HookSendWill(const void* methodInfo) {
    if (!gInPump.exchange(true)) {
        if (gJobPending.load() && !gJobDone.load()) RunJobOnMain();
        gInPump.store(false);
    }
    if (gOrigSendWill) gOrigSendWill(methodInfo);
}

bool InstallPump() {
    if (gPumpInstalled.load()) return true;
    if (!gKlassCanvas) gKlassCanvas = FindClass("UnityEngine", "Canvas");
    if (!gKlassCanvas) {
        Log("InstallPump: Canvas klass miss");
        return false;
    }
    gMiSendWill = FindMethodByRva(gKlassCanvas, kRvaSendWillRenderCanvases);
    if (!gMiSendWill) {
        Log("InstallPump: SendWillRenderCanvases MI miss");
        return false;
    }
    void* orig = nullptr;
    if (!PatchMethodInfo(gMiSendWill, reinterpret_cast<void*>(&HookSendWill), &orig)) {
        Log("InstallPump: patch fail");
        return false;
    }
    gOrigSendWill = reinterpret_cast<FnSendWill>(orig);
    gPumpInstalled.store(true);
    Log("InstallPump ok MI=%p orig=%p", (void*)gMiSendWill, orig);
    return true;
}

void UninstallPump() {
    if (!gPumpInstalled.exchange(false)) return;
    if (gMiSendWill && gOrigSendWill) RestoreMethodInfo(gMiSendWill, reinterpret_cast<void*>(gOrigSendWill));
    gMiSendWill = nullptr;
    gOrigSendWill = nullptr;
    Log("UninstallPump");
}

bool EnqueueJobAndWait(JobKind kind, void* ui, int a) {
    if (!InstallPump()) return false;
    EnsureCs();
    EnterCriticalSection(&gJobCs);
    gJobKind = kind;
    gJobUi = ui;
    gJobA = a;
    gJobB = 0;
    gJobDone.store(false);
    gJobOk.store(false);
    gJobPending.store(true);
    LeaveCriticalSection(&gJobCs);

    const DWORD start = GetTickCount();
    while (!gJobDone.load()) {
        if (GetTickCount() - start > kJobWaitMs) {
            Log("Job timeout kind=%u", (unsigned)kind);
            gJobPending.store(false);
            return false;
        }
        Sleep(5);
    }
    return gJobOk.load();
}

void SetPhase(Phase p) {
    gPhase = p;
    gPhaseSince = GetTickCount();
}

bool PhaseTimedOut() { return GetTickCount() - gPhaseSince > kPhaseTimeoutMs; }

bool WorldMatches(void* world, int32_t wantId, const char* wantName) {
    if (!world) return false;
    if (wantId != 0) return ReadI32(world, kOffWorldId) == wantId;
    if (!wantName || !wantName[0]) return false;
    char name[128]{};
    if (!ReadIl2CppStringUtf8(ReadPtr(world, kOffWorldName), name, sizeof(name))) return false;
    return _stricmp(name, wantName) == 0;
}

int PickLeastChannelId(void* worldItem) {
    void* list = ReadPtr(worldItem, kOffWorldChannels);
    const int n = ListSize(list);
    int bestId = -1;
    int bestUsers = INT_MAX;
    for (int i = 0; i < n; ++i) {
        void* ch = ListAt(list, i);
        if (!ch) continue;
        if (ReadU8(ch, kOffChAdult) != 0) continue;
        const int users = ReadI32(ch, kOffChUserNo);
        const int cap = ReadI32(ch, kOffChCapacity);
        if (cap > 0 && users >= cap) continue;
        const int id = (int)ReadU8(ch, kOffChChannelId);
        if (users < bestUsers || (users == bestUsers && (bestId < 0 || id < bestId))) {
            bestUsers = users;
            bestId = id;
        }
    }
    return bestId;
}

int AvatarCount(void* charUi) {
    if (!charUi) return 0;
    auto fn = AtRva<FnGetAvatarCount>(kRvaGetAvatarCount);
    auto* mi = FindMethodByRva(gKlassChar, kRvaGetAvatarCount);
    if (fn) {
        __try {
            return fn(charUi, mi);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return ListSize(ReadPtr(charUi, kOffCharAvatarList));
}

bool SlotEnabled(void* charUi, int index) {
    auto fn = AtRva<FnIsSlotEnable>(kRvaIsSlotEnable);
    auto* mi = FindMethodByRva(gKlassChar, kRvaIsSlotEnable);
    if (!fn) return true;
    __try {
        return fn(charUi, index, mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void ResetRuntime() {
    gPickedWorld = nullptr;
    gPickedWorldIndex = -1;
    gPickedChannelId = -1;
    gJobPending.store(false);
    gJobDone.store(true);
}

void Tick() {
    if (!gDesired.load()) {
        if (gPhase != Phase::Idle) {
            Log("desired off → Idle");
            SetPhase(Phase::Idle);
            ResetRuntime();
        }
        return;
    }

    if (gPhase == Phase::Idle) {
        ResetRuntime();
        SetPhase(Phase::WaitWorldList);
        Log("start WaitWorldList worldId=%d slot=%u", gWorldId.load(), gCharSlot.load());
        return;
    }
    if (gPhase == Phase::Done || gPhase == Phase::Failed) return;

    if (!ResolveTypes()) {
        LogThrottled("waiting class types…");
        return;
    }
    if (!InstallPump()) {
        LogThrottled("waiting canvas pump…");
        return;
    }

    if (PhaseTimedOut()) {
        Log("phase timeout → Failed (phase=%u)", (unsigned)gPhase);
        SetPhase(Phase::Failed);
        return;
    }

    char wantName[64]{};
    SnapshotWorldName(wantName, sizeof(wantName));
    const int32_t wantId = gWorldId.load();

    switch (gPhase) {
    case Phase::WaitWorldList: {
        if (wantId == 0 && wantName[0] == 0) {
            LogThrottled("need worldId or worldName in panel");
            return;
        }
        void* worldUi = FindFirstOfType(gTypeWorld);
        if (!worldUi) {
            LogThrottled("waiting UILoginWorld…");
            return;
        }
        void* items = ReadPtr(worldUi, kOffWorldItems);
        if (ListSize(items) <= 0) {
            LogThrottled("waiting WorldItems…");
            return;
        }
        SetPhase(Phase::PickWorld);
        break;
    }
    case Phase::PickWorld: {
        void* worldUi = FindFirstOfType(gTypeWorld);
        void* items = worldUi ? ReadPtr(worldUi, kOffWorldItems) : nullptr;
        const int n = ListSize(items);
        int idx = -1;
        void* world = nullptr;
        for (int i = 0; i < n; ++i) {
            void* w = ListAt(items, i);
            if (WorldMatches(w, wantId, wantName)) {
                idx = i;
                world = w;
                break;
            }
        }
        if (idx < 0 || !world) {
            LogThrottled("world not found id=%d name=%s count=%d", wantId, wantName, n);
            return;
        }
        Log("PickWorld index=%d id=%d", idx, ReadI32(world, kOffWorldId));
        if (!EnqueueJobAndWait(JobKind::ClickWorld, worldUi, idx)) {
            SetPhase(Phase::Failed);
            return;
        }
        gPickedWorld = world;
        gPickedWorldIndex = idx;
        SetPhase(Phase::WaitChannelUi);
        break;
    }
    case Phase::WaitChannelUi: {
        void* chUi = FindFirstOfType(gTypeChannel);
        void* sel = chUi ? ReadPtr(chUi, kOffChannelSelectedWorld) : nullptr;
        if (!chUi || !sel) {
            LogThrottled("waiting UILoginChannel+WorldItem…");
            return;
        }
        // Prefer the world object the channel UI holds after SetWorldItem.
        gPickedWorld = sel;
        SetPhase(Phase::PickChannel);
        break;
    }
    case Phase::PickChannel: {
        void* chUi = FindFirstOfType(gTypeChannel);
        if (!chUi || !gPickedWorld) {
            SetPhase(Phase::WaitChannelUi);
            return;
        }
        const int chId = PickLeastChannelId(gPickedWorld);
        if (chId < 0) {
            Log("no eligible channel");
            SetPhase(Phase::Failed);
            return;
        }
        Log("PickChannel id=%d (least users)", chId);
        if (!EnqueueJobAndWait(JobKind::EnterChannel, chUi, chId)) {
            SetPhase(Phase::Failed);
            return;
        }
        gPickedChannelId = chId;
        SetPhase(Phase::WaitCharSelect);
        break;
    }
    case Phase::WaitCharSelect: {
        void* charUi = FindFirstOfType(gTypeChar);
        const int count = AvatarCount(charUi);
        if (!charUi || count <= 0) {
            LogThrottled("waiting UILoginCharacter avatars…");
            return;
        }
        SetPhase(Phase::PickChar);
        break;
    }
    case Phase::PickChar: {
        void* charUi = FindFirstOfType(gTypeChar);
        if (!charUi) {
            SetPhase(Phase::WaitCharSelect);
            return;
        }
        uint32_t slot = gCharSlot.load();
        if (slot < 1) slot = 1;
        const int index = (int)slot - 1;
        const int count = AvatarCount(charUi);
        if (index < 0 || index >= count) {
            Log("char slot %u out of range (count=%d)", slot, count);
            SetPhase(Phase::Failed);
            return;
        }
        if (!SlotEnabled(charUi, index)) {
            Log("char slot %u disabled", slot);
            SetPhase(Phase::Failed);
            return;
        }
        Log("PickChar slot=%u index=%d", slot, index);
        if (!EnqueueJobAndWait(JobKind::SelectChar, charUi, index)) {
            SetPhase(Phase::Failed);
            return;
        }
        SetPhase(Phase::Done);
        Log("Done — latched until autoEnter off");
        break;
    }
    default:
        break;
    }
}

DWORD WINAPI WorkerProc(LPVOID) {
    OpenLogs();
    Log("worker start");
    while (!gWorkerStop.load()) {
        x::ipc::PayloadControl_Poll();
        if (gGA || BindApis()) Tick();
        Sleep(kTickMs);
    }
    Log("worker stop");
    return 0;
}

}  // namespace

void Init() {
    EnsureCs();
    OpenLogs();
    Log("Init");
    (void)BindApis();
}

void Shutdown() {
    StopWorker();
    UninstallPump();
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE t = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    gWorkerThread.store(t);
    Log("StartWorker t=%p", t);
}

void StopWorker() {
    gWorkerStop.store(true);
    gDesired.store(false);
    // Signal only under loader lock — do not join.
    UninstallPump();
}

void SetDesired(bool on, int32_t worldId, const char* worldName, uint32_t charSlot) {
    EnsureCs();
    const bool was = gDesired.load();
    gWorldId.store(worldId);
    if (charSlot < 1) charSlot = 1;
    if (charSlot > 32) charSlot = 32;
    gCharSlot.store(charSlot);
    CopyWorldNameLocked(worldName);
    gDesired.store(on);
    if (on && !was) {
        SetPhase(Phase::Idle);
        ResetRuntime();
    }
    if (!on && was) {
        SetPhase(Phase::Idle);
        ResetRuntime();
    }
}

bool IsDesired() { return gDesired.load(); }

}  // namespace auto_enter
}  // namespace features
}  // namespace x
