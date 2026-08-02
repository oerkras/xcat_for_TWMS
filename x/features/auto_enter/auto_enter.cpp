// TWMS Classic — auto enter (world → least channel → character).
//
// FindAll / TypeObject / UI clicks ONLY on Unity main thread via main_thread_pump.
// Worker-thread FindAll caused "Fatal error in GC / Collecting from unknown thread".
// Prefer SceneLogin singleton (+0xC0/C8/D0) over FindObjectsOfTypeAll.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "auto_enter.h"

#include "../../ipc/payload_control.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../ccu/ccu.h"
#include "../ports/world_port.h"

#include "../../../common/xcat_world_names.h"
#include "../../../common/xcat_worlds_cache.h"
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

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaSceneLoginGet = 0xBFECB0;  // remounted 2026-08-03 SceneLogin.get_Instance
constexpr uint32_t kRvaOnClickWorldItem = 0xA99E10;  // remounted 2026-08-03
constexpr uint32_t kRvaSelectChannel = 0xA92CB0;  // remounted 2026-08-03 SelectChannel（勿用 0xA932F0=EnterChannel）
constexpr uint32_t kRvaOnClickGoWorld = 0xA96130;  // remounted 2026-08-03；仅此进频；勿再调 Trigger
constexpr uint32_t kRvaSelectCharacter = 0xA7F170;  // remounted 2026-08-03
constexpr uint32_t kRvaOnClickButtonSelect = 0xA805B0;  // remounted 2026-08-03
constexpr uint32_t kRvaGetAvatarCount = 0xA8ADC0;  // remounted 2026-08-03
constexpr uint32_t kRvaIsSlotEnable = 0xA86820;  // remounted 2026-08-03

constexpr char kClassSceneLogin[] =
    "c30b8b382878b8b79a97fc52c5f46c21aec217449e91717e6fad3b601d5d994";
constexpr char kClassUiLoginWorld[] =
    "bc2d8dd5edb59edb8044a2679de7c3c39dc8a2f96d17ac09dfd1f18236fe0c8";
constexpr char kClassUiLoginCharacter[] =
    "a7ac3c1c96ab938c4231e9add9f2afbf798ed528222dc4164d65a3c432934bf";
// DumpRestoredData / CMS：UILoginChannel；运行时哈希名
constexpr char kClassUiLoginChannel[] =
    "d1be53f8a11d2b72aa518225bd35495d9df43e7ab24b1a58dcf20dcd93bc60b";

constexpr size_t kOffSlChannelUi = 0xC0;
constexpr size_t kOffSlWorldUi = 0xC8;
constexpr size_t kOffSlCharUi = 0xD0;
constexpr size_t kOffWorldItems = 0x50;
constexpr size_t kOffWorldId = 0x10;
constexpr size_t kOffWorldName = 0x18;
constexpr size_t kOffWorldChannels = 0x38;
constexpr size_t kOffChUserNo = 0x18;
constexpr size_t kOffChChannelId = 0x1D;
constexpr size_t kOffChAdult = 0x1E;
constexpr size_t kOffChCapacity = 0x20;
// IDA OnClickButtonGoWorld / SelectChannel leaf: WorldItem @+0x78, selectedChannelId @+0x80.
constexpr size_t kOffChannelSelectedWorld = 0x78;
constexpr size_t kOffChannelSelectedId = 0x80;
constexpr size_t kOffCharAvatarList = 0x170;  // TW List<AvatarData>（CMS 同字段在 +0x178）
constexpr size_t kOffCharSlotCount = 0x1A8;   // TW SlotCount backing field
// 选中槽 index（IDA 2026-08-03 复核：get/set_SelectedIndex @ RVA A7D140/A7D150 读写 this+0x168；
// SelectCharacter 入口仍 cmp [this+168h], index；AvatarList@+0x170 / SlotCount@+0x1A8 未变）。
constexpr size_t kOffCharSelectedIndex = 0x168;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffArrLen = 0x18;
constexpr size_t kOffArrData = 0x20;

constexpr DWORD kTickMs = 80;
constexpr DWORD kJobWaitMs = 4000;
constexpr DWORD kPhaseTimeoutMs = 60000;
constexpr DWORD kLogThrottleMs = 3000;
constexpr DWORD kAfterWorldClickMs = 600;
constexpr DWORD kAfterSelectChannelMs = 250;
// 进频只点 Go 一次；自动重发会叠 SelectWorld 掉线（已实锤）。
constexpr DWORD kPumpFailBackoffMs = 800;
constexpr DWORD kLeftChannelHoldMs = 1200;
// 选角：avatars 刚变 1 时立刻 Select+Click 会假 ok 不进图（BIN 15:25/15:36 实锤）。
constexpr DWORD kCharReadySettleMs = 700;
constexpr DWORD kAfterSelectCharMs = 400;
constexpr DWORD kCharConfirmRetryMs = 1500;
constexpr int kMaxCharConfirmAttempts = 4;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnSceneLoginGet = void* (*)(const void* methodInfo);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnClickWorld = void (*)(void* self, int index, const void* methodInfo);
using FnSelectChannel = void (*)(void* self, int channelId, const void* methodInfo);
using FnGoWorld = void (*)(void* self, const void* methodInfo);
using FnSelectChar = void (*)(void* self, int index, bool first, const void* methodInfo);
using FnClickSelect = void (*)(void* self, const void* methodInfo);
using FnGetAvatarCount = int (*)(void* self, const void* methodInfo);
using FnIsSlotEnable = bool (*)(void* self, int index, const void* methodInfo);

// Snapshot filled only on Unity main thread (no FindAll/GC from worker).
struct UiSnap {
    void* sceneLogin = nullptr;
    void* worldUi = nullptr;
    void* channelUi = nullptr;
    void* charUi = nullptr;
    void* selectedWorld = nullptr;
    int selectedChannelId = 0;
    int worldItemCount = 0;
    int avatarCount = 0;
    int charSelectedIndex = -1;  // UILoginCharacter+0x168；未知/未绑定时 -1
    bool typesOk = false;
};

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

enum class Phase : uint8_t {
    Idle = 0,
    WaitWorldList,
    PickWorld,
    WaitChannelUi,
    PickChannel,       // SelectChannel only
    WaitChannelArmed,  // 等 UI+0x80 写上目标频道后再 Go
    WaitCharSelect,
    PickChar,          // SelectCharacter only
    WaitCharArmed,     // 等选中态生效再点确认
    ConfirmChar,       // OnClickButtonSelect
    WaitLeaveChar,     // 校验离开选角；失败则有限次重确认
    Done,
    Failed,
};

enum class JobKind : uint8_t {
    None = 0,
    ClickWorld,
    SelectChannel,
    GoWorld,  // OnClickButtonGoWorld only（禁止 Trigger / 禁止自动重发）
    SelectCharIndex,   // SelectCharacter(index) only
    ConfirmCharClick,  // OnClickButtonSelect only
};

HMODULE gGA = nullptr;
uintptr_t gGaBase = 0;
FnFindAll gFindAll = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;

void* gTypeWorld = nullptr;
void* gTypeChannel = nullptr;
void* gTypeChar = nullptr;
void* gKlassWorld = nullptr;
void* gKlassChannel = nullptr;
void* gKlassChar = nullptr;
void* gKlassSceneLogin = nullptr;

UiSnap gSnap{};

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
DWORD gWorldClickedAt = 0;
DWORD gChannelSelectedAt = 0;
DWORD gEnterAttemptAt = 0;
DWORD gLeftChannelAt = 0;
DWORD gCharReadyAt = 0;
DWORD gCharSelectedAt = 0;
DWORD gCharConfirmAt = 0;
DWORD gPumpFailUntil = 0;
int gEnterAttempts = 0;
int gCharConfirmAttempts = 0;
int gPickCharIndex = -1;
char gWorldsFp[512]{};
DWORD gLastWorldsScanMs = 0;

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
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"auto_enter.log");
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

void* FindClass(const char* ns, const char* name) {
    return x::runtime::il2cpp::FindClass(ns, name);
}

void* TypeObjectFromKlass(void* klass) {
    // ResolveTypes runs inside main-thread job — must not nest pump.
    return x::runtime::il2cpp::ClassTypeObjectOnMain(klass);
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

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethodFromName) return nullptr;
    MethodInfoHead* mi = nullptr;
    __try {
        mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mi = nullptr;
    }
    return (mi && mi->methodPointer) ? mi : nullptr;
}

// RVA + kind；可选明文名（dump 残留 OnClick*）。kind 不唯一时仍可靠 RVA 校验形。
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* nameHint = nullptr) {
    if (nameHint) {
        if (MethodInfoHead* byName = FindMethodByName(klass, nameHint, shape.arity)) return byName;
    }
    if (!klass) return FindMethodByRva(klass, rva);
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            LogThrottled("ResolveMi kind hit rva=0x%X name=%s", rva, nameHint ? nameHint : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

bool ResolveTypes() {
    if (!gKlassSceneLogin) {
        gKlassSceneLogin = FindClass("", kClassSceneLogin);
        if (!gKlassSceneLogin) gKlassSceneLogin = FindClass("", "SceneLogin");
    }
    if (!gKlassWorld) gKlassWorld = FindClass("", kClassUiLoginWorld);
    if (!gKlassChannel) {
        gKlassChannel = FindClass("", kClassUiLoginChannel);
        if (!gKlassChannel) gKlassChannel = FindClass("", "UILoginChannel");  // restored fallback
    }
    if (!gKlassChar) gKlassChar = FindClass("", kClassUiLoginCharacter);
    // TypeGetObject allocates — only call on Unity main thread.
    if (!gTypeWorld && gKlassWorld) gTypeWorld = TypeObjectFromKlass(gKlassWorld);
    if (!gTypeChannel && gKlassChannel) gTypeChannel = TypeObjectFromKlass(gKlassChannel);
    if (!gTypeChar && gKlassChar) gTypeChar = TypeObjectFromKlass(gKlassChar);
    // SceneLogin path does not require TypeObjects; FindAll fallback does.
    if (gKlassSceneLogin) return true;
    if (!(gTypeWorld && gTypeChannel && gTypeChar)) {
        LogThrottled("ResolveTypes sl=%p world=%p ch=%p char=%p typeW=%p typeC=%p typeCh=%p",
                     gKlassSceneLogin, gKlassWorld, gKlassChannel, gKlassChar, gTypeWorld,
                     gTypeChannel, gTypeChar);
        return false;
    }
    return true;
}

bool BindApis() {
    if (!x::runtime::il2cpp::Ensure()) {
        Log("BindApis: no GameAssembly");
        return false;
    }
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gGaBase = x::runtime::il2cpp::GaBase();
    gFindAll = e.findAll;
    gClassGetMethods = e.classGetMethods;
    if (!gFindAll || !gClassGetMethods) {
        Log("BindApis: missing export/RVA");
        return false;
    }
    Log("BindApis ok GA=%p", gGA);
    return true;
}

void CacheWorldItemsFromUi(void* worldUi) {
    if (!worldUi) return;
    void* items = ReadPtr(worldUi, kOffWorldItems);
    const int n = ListSize(items);
    if (n <= 0) return;

    xcat::WorldsCacheEntry entries[xcat::kWorldsCacheMax]{};
    uint32_t count = 0;
    char fp[512]{};
    size_t fpUsed = 0;
    for (int i = 0; i < n && count < xcat::kWorldsCacheMax; ++i) {
        void* w = ListAt(items, i);
        if (!w) continue;
        const int32_t id = ReadI32(w, kOffWorldId);
        char nameRaw[xcat::kWorldsCacheNameCap]{};
        (void)ReadIl2CppStringUtf8(ReadPtr(w, kOffWorldName), nameRaw, sizeof(nameRaw));
        const xcat::WorldNamesPack& wn = xcat::GetSharedWorldNames(x::runtime::GetBinDir());
        const std::string pretty = xcat::WorldNamePreferDisplay(wn, nameRaw);
        entries[count].worldId = id;
        strncpy_s(entries[count].name, pretty.c_str(), _TRUNCATE);
        ++count;

        if (fpUsed + 48 < sizeof(fp)) {
            const int wrote = snprintf(fp + fpUsed, sizeof(fp) - fpUsed, "%d:%s/%s;", id,
                                      nameRaw[0] ? nameRaw : "-", pretty.empty() ? "-" : pretty.c_str());
            if (wrote > 0) fpUsed += (size_t)wrote;
        }
    }
    if (count == 0) return;
    if (gWorldsFp[0] && strcmp(gWorldsFp, fp) == 0) return;

    if (!xcat::WriteWorldsCache(x::runtime::GetBinDir(), entries, count)) {
        LogThrottled("WorldsCache write fail");
        return;
    }
    strncpy_s(gWorldsFp, fp, _TRUNCATE);
    Log("WorldsCache saved count=%u fp=%s", count, fp);
}

int AvatarCountOnMain(void* charUi) {
    if (!charUi) return 0;
    // 1) List<AvatarData> @+0x170
    const int listN = ListSize(ReadPtr(charUi, kOffCharAvatarList));
    if (listN > 0) return listN;
    // 2) SlotCount @+0x1A8（列表尚未填完时也能用）
    const int slotN = ReadI32(charUi, kOffCharSlotCount);
    if (slotN > 0 && slotN <= 32) return slotN;
    // 3) 托管 GetAvatarCount（仅主线程，失败忽略）
    // int() 在 Char 上不唯一 → kind 只验；优先明文名。
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kAv{0, TypeKind::I32, true, false, {}};
    auto* mi = ResolveMi(gKlassChar, kRvaGetAvatarCount, kAv, "GetAvatarCount");
    auto fn = FnFromMi<FnGetAvatarCount>(mi, kRvaGetAvatarCount);
    if (fn) {
        __try {
            const int n = fn(charUi, mi);
            if (n > 0 && n <= 32) return n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0;
}

bool SlotEnabledOnMain(void* charUi, int index) {
    // bool(int) 在 Char 上唯一。
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kSlot{1, TypeKind::Bool, true, false, {TypeKind::I32}};
    auto* mi = ResolveMi(gKlassChar, kRvaIsSlotEnable, kSlot, "IsSlotEnable");
    auto fn = FnFromMi<FnIsSlotEnable>(mi, kRvaIsSlotEnable);
    if (!fn) return true;
    __try {
        return fn(charUi, index, mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void ProbeOnMain(void*) {
    UiSnap snap{};
    if (!gGA && !BindApis()) {
        gSnap = snap;
        return;
    }
    snap.typesOk = ResolveTypes();

    // static 返回 SceneLogin* 的 () 唯一。
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kGet{0, TypeKind::Ptr, true, false, {}};
    auto* miGet = ResolveMi(gKlassSceneLogin, kRvaSceneLoginGet, kGet, "get_Instance");
    auto getSl = FnFromMi<FnSceneLoginGet>(miGet, kRvaSceneLoginGet);
    void* sl = nullptr;
    if (getSl) {
        __try {
            sl = getSl(miGet);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sl = nullptr;
        }
    }
    snap.sceneLogin = sl;
    if (sl) {
        snap.channelUi = ReadPtr(sl, kOffSlChannelUi);
        snap.worldUi = ReadPtr(sl, kOffSlWorldUi);
        snap.charUi = ReadPtr(sl, kOffSlCharUi);
    }

    // FindAll 很重：有 SceneLogin 时少扫；但选角等待期必须能兜到 UILoginCharacter。
    const bool light = (gPhase == Phase::WaitChannelArmed);
    const bool needChar = (gPhase == Phase::WaitCharSelect || gPhase == Phase::PickChar ||
                           gPhase == Phase::WaitCharArmed || gPhase == Phase::ConfirmChar ||
                           gPhase == Phase::WaitLeaveChar);
    if (!light || !sl) {
        if (!snap.worldUi && gTypeWorld) snap.worldUi = FindFirstOfType(gTypeWorld);
        if (!snap.channelUi && gTypeChannel) snap.channelUi = FindFirstOfType(gTypeChannel);
        if (!snap.charUi && gTypeChar) snap.charUi = FindFirstOfType(gTypeChar);
    } else if (needChar && !snap.charUi && gTypeChar) {
        snap.charUi = FindFirstOfType(gTypeChar);
    }
    if (needChar && !snap.charUi && gTypeChar) {
        snap.charUi = FindFirstOfType(gTypeChar);
    }

    if (snap.channelUi) {
        snap.selectedWorld = ReadPtr(snap.channelUi, kOffChannelSelectedWorld);
        snap.selectedChannelId = ReadI32(snap.channelUi, kOffChannelSelectedId);
    }
    if (snap.worldUi && !light) {
        snap.worldItemCount = ListSize(ReadPtr(snap.worldUi, kOffWorldItems));
        const DWORD now = GetTickCount();
        if (now - gLastWorldsScanMs >= 1000) {
            gLastWorldsScanMs = now;
            CacheWorldItemsFromUi(snap.worldUi);
        }
    } else if (snap.worldUi) {
        snap.worldItemCount = ListSize(ReadPtr(snap.worldUi, kOffWorldItems));
    }
    if (snap.charUi) {
        snap.avatarCount = AvatarCountOnMain(snap.charUi);
        // 选中槽必须在主线程读；worker 直读托管堆有 GC 竞态。
        const int sel = ReadI32(snap.charUi, kOffCharSelectedIndex);
        snap.charSelectedIndex =
            (sel >= 0 && sel < 32) ? sel : -1;
    }

    gSnap = snap;
}

bool RefreshSnap() {
    const DWORD now = GetTickCount();
    if (gPumpFailUntil && now < gPumpFailUntil) {
        LogThrottled("waiting main pump… (backoff)");
        return false;
    }
    // 选角等待给更长超时；失败后退避，避免 80ms 狂塞队列拖死泵。
    const DWORD waitMs = (gPhase == Phase::WaitCharSelect) ? 3000 : 1500;
    if (!x::runtime::main_thread::InvokeAndWait(&ProbeOnMain, nullptr, waitMs)) {
        gPumpFailUntil = GetTickCount() + kPumpFailBackoffMs;
        LogThrottled("waiting main pump…");
        return false;
    }
    gPumpFailUntil = 0;
    return true;
}

void RunJobOnMain() {
    EnsureCs();
    EnterCriticalSection(&gJobCs);
    const JobKind kind = gJobKind;
    void* ui = gJobUi;
    const int a = gJobA;
    const int b = gJobB;
    LeaveCriticalSection(&gJobCs);

    bool ok = false;
    __try {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        switch (kind) {
        case JobKind::ClickWorld: {
            // void(int) 在 World 上唯一。
            constexpr MethodShape kClick{1, TypeKind::Void, true, false, {TypeKind::I32}};
            auto* mi = ResolveMi(gKlassWorld, kRvaOnClickWorldItem, kClick, "OnClickWorldItem");
            auto fn = FnFromMi<FnClickWorld>(mi, kRvaOnClickWorldItem);
            if (ui && fn) {
                fn(ui, a, mi);
                ok = true;
            }
            break;
        }
        case JobKind::SelectChannel: {
            // void(int) 在 Channel 上不唯一（含 EnterChannel）→ RVA 主路径 + kind 验形。
            constexpr MethodShape kSel{1, TypeKind::Void, true, false, {TypeKind::I32}};
            auto* miSel = ResolveMi(gKlassChannel, kRvaSelectChannel, kSel, "SelectChannel");
            auto fnSel = FnFromMi<FnSelectChannel>(miSel, kRvaSelectChannel);
            if (ui && fnSel) {
                fnSel(ui, a, miSel);
                const int got = ReadI32(ui, kOffChannelSelectedId);
                ok = (got == a);
                if (!ok) Log("SelectChannel write miss want=%d got=%d", a, got);
            }
            break;
        }
        case JobKind::GoWorld: {
            // 只点 OnClickButtonGoWorld。禁止 TriggerEnterChannel/SendSelectWorld(0xA96D20)：
            // Go 内部已发 SelectWorld；再发一次 = 重复进频包 → 服端断线。
            (void)b;
            constexpr MethodShape kGo{0, TypeKind::Void, true, false, {}};
            auto* miGo = ResolveMi(gKlassChannel, kRvaOnClickGoWorld, kGo, "OnClickButtonGoWorld");
            auto fnGo = FnFromMi<FnGoWorld>(miGo, kRvaOnClickGoWorld);
            if (ui && fnGo) {
                const int armed = ReadI32(ui, kOffChannelSelectedId);
                Log("GoWorld armedCh=%d (single click, no Trigger)", armed);
                fnGo(ui, miGo);
                ok = true;
            }
            break;
        }
        case JobKind::SelectCharIndex: {
            // void(int,bool) 在 Char 上唯一。
            constexpr MethodShape kSel{2, TypeKind::Void, true, false, {TypeKind::I32, TypeKind::Bool}};
            auto* miSel = ResolveMi(gKlassChar, kRvaSelectCharacter, kSel, "SelectCharacter");
            auto fnSel = FnFromMi<FnSelectChar>(miSel, kRvaSelectCharacter);
            if (ui && fnSel) {
                if (!SlotEnabledOnMain(ui, a)) {
                    Log("SelectCharIndex slot disabled index=%d", a);
                    break;
                }
                fnSel(ui, a, false, miSel);
                ok = true;
            }
            break;
        }
        case JobKind::ConfirmCharClick: {
            constexpr MethodShape kClick{0, TypeKind::Void, true, false, {}};
            auto* miClick =
                ResolveMi(gKlassChar, kRvaOnClickButtonSelect, kClick, "OnClickButtonSelect");
            auto fnClick = FnFromMi<FnClickSelect>(miClick, kRvaOnClickButtonSelect);
            if (ui && fnClick) {
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

void AeMainJob(void*) { RunJobOnMain(); }

bool EnqueueJobAndWait(JobKind kind, void* ui, int a, int b = 0) {
    EnsureCs();
    EnterCriticalSection(&gJobCs);
    gJobKind = kind;
    gJobUi = ui;
    gJobA = a;
    gJobB = b;
    gJobDone.store(false);
    gJobOk.store(false);
    gJobPending.store(true);
    LeaveCriticalSection(&gJobCs);

    if (!x::runtime::main_thread::InvokeAndWait(&AeMainJob, nullptr, kJobWaitMs)) {
        Log("EnqueueJob: pump fail/timeout kind=%u", (unsigned)kind);
        gJobPending.store(false);
        gPumpFailUntil = GetTickCount() + kPumpFailBackoffMs;
        return false;
    }
    Log("Job kind=%u ok=%d direct=%d", (unsigned)kind, gJobOk.load() ? 1 : 0,
        x::runtime::main_thread::IsDirectMode() ? 1 : 0);
    return gJobOk.load();
}

void SetPhase(Phase p) {
    gPhase = p;
    gPhaseSince = GetTickCount();
    // Done / Failed 都解冻：否则卡在选角/等 UI 时全端口 FindAll 永久死锁。
    if (p == Phase::Done || p == Phase::Failed) {
        x::runtime::managed_main::SetLoginFreeze(false);
    }
}

bool PhaseTimedOut() { return GetTickCount() - gPhaseSince > kPhaseTimeoutMs; }

bool WorldMatches(void* world, int32_t wantId, const char* wantName) {
    if (!world) return false;
    if (wantId != 0) return ReadI32(world, kOffWorldId) == wantId;
    if (!wantName || !wantName[0]) return false;
    char name[128]{};
    if (!ReadIl2CppStringUtf8(ReadPtr(world, kOffWorldName), name, sizeof(name))) return false;
    if (_stricmp(name, wantName) == 0) return true;
    const xcat::WorldNamesPack& wn = xcat::GetSharedWorldNames(x::runtime::GetBinDir());
    return xcat::WorldNameEquals(wn, name, wantName);
}

int PickLeastChannelId(void* worldItem) {
    void* list = ReadPtr(worldItem, kOffWorldChannels);
    const int n = ListSize(list);
    int bestId = -1;
    int bestUsers = INT_MAX;
    // 审计：首屏 1..20 的人数快照（-1=表里没有该 id）。
    int u20[21];
    char flag20[21];  // ' '=eligible, 'A'=adult, 'F'=full, '?'=missing/bad
    for (int id = 1; id <= 20; ++id) {
        u20[id] = -1;
        flag20[id] = '?';
    }
    // CCU：全表合法频道人数之和（与选最少人过滤略不同：满员也计入在线）。
    // 同时喂每频填表，供进图后 channel_hop 优先未满。
    {
        long long ccuSum = 0;
        int ccuN = 0;
        x::features::ccu::ChannelFillRow fillRows[64]{};
        int fillN = 0;
        for (int i = 0; i < n; ++i) {
            void* ch = ListAt(list, i);
            if (!ch) continue;
            const int id = (int)ReadU8(ch, kOffChChannelId);
            const int users = ReadI32(ch, kOffChUserNo);
            const int cap = ReadI32(ch, kOffChCapacity);
            const int adult = (int)ReadU8(ch, kOffChAdult);
            if (id >= 1 && id <= 20) {
                u20[id] = users;
                if (adult != 0)
                    flag20[id] = 'A';
                else if (users < 0)
                    flag20[id] = '?';
                else if (cap > 0 && users >= cap)
                    flag20[id] = 'F';
                else
                    flag20[id] = ' ';
            }
            if (id >= 1 && id <= 64 && fillN < 64) {
                fillRows[fillN].channelId = static_cast<uint8_t>(id);
                fillRows[fillN].users = static_cast<int16_t>(users > 32767 ? 32767 : users);
                fillRows[fillN].cap = static_cast<int16_t>(cap > 32767 ? 32767 : cap);
                fillRows[fillN].adult = adult != 0 ? 1 : 0;
                ++fillN;
            }
            if (id < 1 || id > 64) continue;
            if (adult != 0) continue;
            if (users < 0) continue;
            ccuSum += users;
            ++ccuN;
        }
        if (ccuN > 0) {
            x::features::ccu::NotifyWorldChannelSnapshot(ccuSum, ccuN, "auto_enter");
        }
        if (fillN > 0) {
            x::features::ccu::NotifyChannelFillTable(fillRows, fillN, "auto_enter");
        }
    }
    // 两行打全 1..20；后缀 A=成人 F=满员；无后缀=可入选；?=表里没有。
    {
        char line[320];
        int pos = snprintf(line, sizeof(line), "  ch1-10:");
        for (int id = 1; id <= 10 && pos > 0 && (size_t)pos + 28 < sizeof(line); ++id) {
            if (u20[id] < 0)
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %d=?", id);
            else if (flag20[id] == ' ')
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %d=%d", id, u20[id]);
            else
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %d=%d%c", id, u20[id],
                                flag20[id]);
        }
        Log("%s", line);
        pos = snprintf(line, sizeof(line), "  ch11-20:");
        for (int id = 11; id <= 20 && pos > 0 && (size_t)pos + 28 < sizeof(line); ++id) {
            if (u20[id] < 0)
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %d=?", id);
            else if (flag20[id] == ' ')
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %d=%d", id, u20[id]);
            else
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %d=%d%c", id, u20[id],
                                flag20[id]);
        }
        Log("%s", line);
    }
    // 前三少（仅 pass0 可入选：非成人、未满员、1..20）。
    {
        int topId[3] = {-1, -1, -1};
        int topUsers[3] = {INT_MAX, INT_MAX, INT_MAX};
        for (int id = 1; id <= 20; ++id) {
            if (flag20[id] != ' ' || u20[id] < 0) continue;
            const int users = u20[id];
            for (int slot = 0; slot < 3; ++slot) {
                if (users < topUsers[slot] ||
                    (users == topUsers[slot] && (topId[slot] < 0 || id < topId[slot]))) {
                    for (int k = 2; k > slot; --k) {
                        topUsers[k] = topUsers[k - 1];
                        topId[k] = topId[k - 1];
                    }
                    topUsers[slot] = users;
                    topId[slot] = id;
                    break;
                }
            }
        }
        Log("  top3 least: %d=%d  %d=%d  %d=%d", topId[0],
            topId[0] >= 0 ? topUsers[0] : -1, topId[1], topId[1] >= 0 ? topUsers[1] : -1, topId[2],
            topId[2] >= 0 ? topUsers[2] : -1);
    }
    // 先在 1..20 里找最少人（对齐首屏可见频道）；没有再扫全表。
    for (int pass = 0; pass < 2; ++pass) {
        bestId = -1;
        bestUsers = INT_MAX;
        for (int i = 0; i < n; ++i) {
            void* ch = ListAt(list, i);
            if (!ch) continue;
            const int id = (int)ReadU8(ch, kOffChChannelId);
            const int users = ReadI32(ch, kOffChUserNo);
            const int cap = ReadI32(ch, kOffChCapacity);
            const int adult = (int)ReadU8(ch, kOffChAdult);
            if (id < 1 || id > 64) continue;
            if (pass == 0 && id > 20) continue;
            if (adult != 0) continue;
            if (cap > 0 && users >= cap) continue;
            if (users < 0) continue;
            if (users < bestUsers || (users == bestUsers && (bestId < 0 || id < bestId))) {
                bestUsers = users;
                bestId = id;
            }
        }
        if (bestId >= 0) {
            Log("PickLeast → id=%d users=%d of %d channels (pass=%d)", bestId, bestUsers, n, pass);
            return bestId;
        }
    }
    Log("PickLeast → none of %d channels", n);
    return -1;
}

int AvatarCount(void* charUi) {
    // Prefer last main-thread snap; do not call managed GetAvatarCount from worker.
    if (charUi && charUi == gSnap.charUi) return gSnap.avatarCount;
    return ListSize(ReadPtr(charUi, kOffCharAvatarList));
}

bool StillOnTargetChannelUi(int32_t wantId, const char* wantName) {
    return gSnap.channelUi && gSnap.selectedWorld && WorldMatches(gSnap.selectedWorld, wantId, wantName);
}

void ResetRuntime() {
    gPickedWorld = nullptr;
    gPickedWorldIndex = -1;
    gPickedChannelId = -1;
    gWorldClickedAt = 0;
    gChannelSelectedAt = 0;
    gEnterAttemptAt = 0;
    gLeftChannelAt = 0;
    gCharReadyAt = 0;
    gCharSelectedAt = 0;
    gCharConfirmAt = 0;
    gPumpFailUntil = 0;
    gEnterAttempts = 0;
    gCharConfirmAttempts = 0;
    gPickCharIndex = -1;
    gJobPending.store(false);
    gJobDone.store(true);
    gSnap = {};
}

void Tick() {
    if (!gDesired.load()) {
        if (gPhase != Phase::Idle) {
            Log("desired off → Idle");
            SetPhase(Phase::Idle);
            ResetRuntime();
        }
        // Still probe occasionally to refresh worlds cache while on login UI.
        static DWORD sLastIdleProbe = 0;
        const DWORD now = GetTickCount();
        if (now - sLastIdleProbe > 1000) {
            sLastIdleProbe = now;
            (void)RefreshSnap();
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

    if (!RefreshSnap()) return;
    if (!gSnap.typesOk && !gSnap.sceneLogin && !gSnap.worldUi && !gSnap.channelUi) {
        LogThrottled("waiting SceneLogin / class types…");
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
        // 已在频道页（手动点过分区）：直接续跑选频道。
        {
            void* chUi = gSnap.channelUi;
            void* sel = gSnap.selectedWorld;
            if (chUi && sel && WorldMatches(sel, wantId, wantName)) {
                gPickedWorld = sel;
                Log("resume from channel UI worldId=%d", ReadI32(sel, kOffWorldId));
                SetPhase(Phase::PickChannel);
                break;
            }
        }
        void* worldUi = gSnap.worldUi;
        if (!worldUi) {
            LogThrottled("waiting UILoginWorld… (sl=%p)", gSnap.sceneLogin);
            return;
        }
        if (gSnap.worldItemCount <= 0) {
            LogThrottled("waiting WorldItems…");
            return;
        }
        SetPhase(Phase::PickWorld);
        break;
    }
    case Phase::PickWorld: {
        void* worldUi = gSnap.worldUi;
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
        gWorldClickedAt = GetTickCount();
        SetPhase(Phase::WaitChannelUi);
        break;
    }
    case Phase::WaitChannelUi: {
        void* chUi = gSnap.channelUi;
        void* sel = gSnap.selectedWorld;
        if (!chUi || !sel) {
            LogThrottled("waiting UILoginChannel+WorldItem…");
            return;
        }
        // 必须等 SetWorldItem 把目标分区写进频道页，避免点完分区立刻误用旧 WorldItem。
        if (!WorldMatches(sel, wantId, wantName)) {
            LogThrottled("channel UI world mismatch (want=%d got=%d) — wait SetWorldItem", wantId,
                         ReadI32(sel, kOffWorldId));
            return;
        }
        if (gWorldClickedAt && GetTickCount() - gWorldClickedAt < kAfterWorldClickMs) {
            return;
        }
        const int chN = ListSize(ReadPtr(sel, kOffWorldChannels));
        if (chN <= 0) {
            LogThrottled("waiting channel list on WorldItem…");
            return;
        }
        gPickedWorld = sel;
        Log("WaitChannelUi ready worldId=%d channels=%d", ReadI32(sel, kOffWorldId), chN);
        SetPhase(Phase::PickChannel);
        break;
    }
    case Phase::PickChannel: {
        void* chUi = gSnap.channelUi;
        void* sel = gSnap.selectedWorld;
        if (sel && WorldMatches(sel, wantId, wantName)) gPickedWorld = sel;
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
        Log("PickChannel Select id=%d", chId);
        if (!EnqueueJobAndWait(JobKind::SelectChannel, chUi, chId)) {
            SetPhase(Phase::Failed);
            return;
        }
        gPickedChannelId = chId;
        gChannelSelectedAt = GetTickCount();
        gEnterAttempts = 0;
        SetPhase(Phase::WaitChannelArmed);
        break;
    }
    case Phase::WaitChannelArmed: {
        void* chUi = gSnap.channelUi;
        if (!chUi || !StillOnTargetChannelUi(wantId, wantName)) {
            LogThrottled("WaitChannelArmed: channel UI gone early — wait char");
            gLeftChannelAt = GetTickCount();
            SetPhase(Phase::WaitCharSelect);
            return;
        }
        const int armed = gSnap.selectedChannelId;
        if (armed != gPickedChannelId) {
            LogThrottled("waiting channel armed want=%d got=%d", gPickedChannelId, armed);
            if (gChannelSelectedAt && GetTickCount() - gChannelSelectedAt > 2000) {
                Log("re-SelectChannel id=%d", gPickedChannelId);
                (void)EnqueueJobAndWait(JobKind::SelectChannel, chUi, gPickedChannelId);
                gChannelSelectedAt = GetTickCount();
            }
            return;
        }
        if (gChannelSelectedAt && GetTickCount() - gChannelSelectedAt < kAfterSelectChannelMs) {
            return;
        }
        const int worldIdLog =
            gPickedWorld ? ReadI32(gPickedWorld, kOffWorldId) : (wantId > 0 ? wantId : 0);
        Log("GoWorld id=%d worldId=%d (Select armed)", gPickedChannelId, worldIdLog);
        if (!EnqueueJobAndWait(JobKind::GoWorld, chUi, gPickedChannelId)) {
            SetPhase(Phase::Failed);
            return;
        }
        gEnterAttemptAt = GetTickCount();
        gEnterAttempts = 1;
        gLeftChannelAt = 0;
        SetPhase(Phase::WaitCharSelect);
        break;
    }
    case Phase::WaitCharSelect: {
        // 选角页已出来时，SceneLogin 往往仍挂着 channelUi 指针——不能当成「还在频道页」。
        if (gSnap.charUi) {
            const int count = gSnap.avatarCount;
            LogThrottled("char UI present avatars=%d chLinger=%p sl=%p", count, gSnap.channelUi,
                         gSnap.sceneLogin);
            if (count <= 0) {
                gCharReadyAt = 0;
                return;
            }
            if (!gCharReadyAt) {
                gCharReadyAt = GetTickCount();
                Log("char UI ready avatars=%d — settle %ums before Select", count,
                    (unsigned)kCharReadySettleMs);
            }
            if (GetTickCount() - gCharReadyAt < kCharReadySettleMs) return;
            Log("char UI settled avatars=%d (ignore lingering channel ptr)", count);
            SetPhase(Phase::PickChar);
            break;
        }
        // 尚无选角 UI：若频道指针还在，只等不重发。
        if (StillOnTargetChannelUi(wantId, wantName)) {
            gLeftChannelAt = 0;
            gCharReadyAt = 0;
            LogThrottled("waiting leave channel UI… (goSent=%d armed=%d, no auto-retry)",
                         gEnterAttempts, gSnap.selectedChannelId);
            return;
        }

        if (!gLeftChannelAt) gLeftChannelAt = GetTickCount();
        if (GetTickCount() - gLeftChannelAt < kLeftChannelHoldMs) {
            LogThrottled("left channel UI, settling…");
            return;
        }

        LogThrottled("waiting UILoginCharacter… (char=null sl=%p ch=%p)", gSnap.sceneLogin,
                     gSnap.channelUi);
        return;
    }
    case Phase::PickChar: {
        void* charUi = gSnap.charUi;
        if (!charUi) {
            SetPhase(Phase::WaitCharSelect);
            return;
        }
        uint32_t slot = gCharSlot.load();
        if (slot < 1) slot = 1;
        const int index = (int)slot - 1;
        const int count = gSnap.avatarCount > 0 ? gSnap.avatarCount : AvatarCount(charUi);
        if (count <= 0) {
            LogThrottled("PickChar: avatars still 0");
            gCharReadyAt = 0;
            SetPhase(Phase::WaitCharSelect);
            return;
        }
        if (index < 0 || index >= count) {
            Log("char slot %u out of range (count=%d)", slot, count);
            SetPhase(Phase::Failed);
            return;
        }
        gPickCharIndex = index;

        // 游戏进选角页常已默认选中第一个 / 上次角色；再 Select 同 index = 视觉「选两次」。
        // charSelectedIndex 来自主线程快照；未知 (-1) 时保守走 Select。
        const int curSel = gSnap.charSelectedIndex;
        if (curSel == index) {
            Log("PickChar already selected index=%d slot=%u (skip Select, confirm only)", index,
                slot);
            gCharSelectedAt = GetTickCount();
            SetPhase(Phase::ConfirmChar);
            break;
        }

        Log("PickChar Select index=%d slot=%u count=%d (wasSelected=%d)", index, slot, count,
            curSel);
        if (!EnqueueJobAndWait(JobKind::SelectCharIndex, charUi, index)) {
            SetPhase(Phase::Failed);
            return;
        }
        gCharSelectedAt = GetTickCount();
        SetPhase(Phase::WaitCharArmed);
        break;
    }
    case Phase::WaitCharArmed: {
        if (!gSnap.charUi) {
            // 选完人后 UI 已拆——可能已点进；转等待离开/进图。
            SetPhase(Phase::WaitLeaveChar);
            break;
        }
        if (GetTickCount() - gCharSelectedAt < kAfterSelectCharMs) return;
        SetPhase(Phase::ConfirmChar);
        break;
    }
    case Phase::ConfirmChar: {
        void* charUi = gSnap.charUi;
        if (!charUi) {
            SetPhase(Phase::WaitLeaveChar);
            break;
        }
        Log("ConfirmChar click attempt=%d index=%d", gCharConfirmAttempts + 1, gPickCharIndex);
        if (!EnqueueJobAndWait(JobKind::ConfirmCharClick, charUi, gPickCharIndex)) {
            SetPhase(Phase::Failed);
            return;
        }
        ++gCharConfirmAttempts;
        gCharConfirmAt = GetTickCount();
        SetPhase(Phase::WaitLeaveChar);
        break;
    }
    case Phase::WaitLeaveChar: {
        // 真进图 / 选角 UI 消失才算 Done；假 ok 会一直停在 charUi。
        if (!gSnap.charUi) {
            Log("left char UI after confirm — Done");
            SetPhase(Phase::Done);
            Log("Done — latched until autoEnter off");
            break;
        }
        if (x::features::ports::world::IsPlayReady()) {
            Log("play ready while char UI linger — Done");
            SetPhase(Phase::Done);
            Log("Done — latched until autoEnter off");
            break;
        }
        if (gCharConfirmAttempts >= kMaxCharConfirmAttempts) {
            Log("ConfirmChar exhausted attempts=%d — Failed (still on char UI)",
                gCharConfirmAttempts);
            SetPhase(Phase::Failed);
            return;
        }
        if (gCharConfirmAt && GetTickCount() - gCharConfirmAt >= kCharConfirmRetryMs) {
            Log("still on char UI — retry ConfirmChar (%d/%d)", gCharConfirmAttempts + 1,
                kMaxCharConfirmAttempts);
            // 已在选角页：优先只重确认；仅当选中槽不对/未知才回 PickChar 重选。
            const int curSel = gSnap.charSelectedIndex;
            if (gSnap.charUi && curSel == gPickCharIndex) {
                SetPhase(Phase::ConfirmChar);
            } else {
                SetPhase(Phase::PickChar);
            }
            break;
        }
        LogThrottled("waiting leave char UI… (confirm=%d)", gCharConfirmAttempts);
        return;
    }
    default:
        break;
    }
}

DWORD WINAPI WorkerProc(LPVOID) {
    OpenLogs();
    Log("worker start (main-thread probe only; no worker FindAll)");
    while (!gWorkerStop.load()) {
        x::ipc::PayloadControl_Poll();
        if (gGA || BindApis()) {
            Tick();
        }
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
        x::runtime::managed_main::SetLoginFreeze(true);
        SetPhase(Phase::Idle);
        ResetRuntime();
    }
    if (!on && was) {
        x::runtime::managed_main::SetLoginFreeze(false);
        SetPhase(Phase::Idle);
        ResetRuntime();
    }
}

bool IsDesired() { return gDesired.load(); }

}  // namespace auto_enter
}  // namespace features
}  // namespace x
