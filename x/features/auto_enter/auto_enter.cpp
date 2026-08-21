// TWMS Classic ? auto enter (world ? random open channel ? character).
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
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../ccu/ccu.h"
#include "../channel_hop/channel_hop.h"
#include "../kick_sniff/kick_sniff.h"
#include "../ports/world_port.h"

#include "../../../common/xcat_world_names.h"
#include "../../../common/xcat_worlds_cache.h"
#include "../../runtime/log.h"

#include <Psapi.h>
#include <atomic>
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
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Unity FindAll → x::runtime::il2cpp::kRvaFindObjectsOfTypeAll（il2cpp_bind.h SSOT）
// RVAs remounted 2026-08-06（dump.cs / script.json · ForceVersion 31）
constexpr uint32_t kRvaSceneLoginGet = 0xC1B4F0;  // SceneLogin static get_Instance
constexpr uint32_t kRvaOnClickWorldItem = 0xAB17F0;  // UILoginWorld void(int)
constexpr uint32_t kRvaSelectChannel = 0xAAA760;  // UILoginChannel.SelectChannel
constexpr uint32_t kRvaOnClickGoWorld = 0xAAD9A0;  // UILoginChannel.OnClickButtonGoWorld
constexpr uint32_t kRvaSelectCharacter = 0xA958B0;  // UILoginCharacter void(int,bool)
constexpr uint32_t kRvaOnClickButtonSelect = 0xA96C20;  // UILoginCharacter.OnClickButtonSelect
constexpr uint32_t kRvaGetAvatarCount = 0xAA16B0;  // UILoginCharacter public int()
constexpr uint32_t kRvaIsSlotEnable = 0xA9D2E0;  // UILoginCharacter bool(int)

constexpr char kClassSceneLogin[] =
    "bb1caf5f2d406c154f3dcdd919346fcfd41cdc6a87ec14c6b0571ebad0dca78";
constexpr char kClassUiLoginWorld[] =
    "b31f0d00e217d3bbe1b237a6d0fa6cf83f912af1258cd4e67daafea74a84301";
constexpr char kClassUiLoginCharacter[] =
    "fc5b7ecd41af37ce3038b1ff887ec5ba1c1ca4032df17a6f186b635bf47abc3";
constexpr char kClassUiLoginChannel[] =
    "f1c62fa13fca3ebb6aa2b2b5ac35752133b1104569d4c8d0a8ba2110410e636";

constexpr size_t kFbSlChannelUi = 0xC0;
constexpr size_t kFbSlWorldUi = 0xC8;
constexpr size_t kFbSlCharUi = 0xD0;
constexpr char kHashSlChannelUi[] =
    "<eaf29c47d079625ed8ef788293bf161d8acfc1621fa81a0e28cec608fac2bb7>k__BackingField";
constexpr char kHashSlWorldUi[] =
    "<d54bf505f41d14db037a8fe67fd790d0f19ee2bdc88fd28f875f591edc8a51e>k__BackingField";
constexpr char kHashSlCharUi[] =
    "<fb3fa3c29af1d92f626f778b8a6b4dd45e5c5e17ecc851a9290b1dd9669b57c>k__BackingField";
// WorldItems / WorldChannels / AvatarList + remaining login slots: hash -> field_get_offset
constexpr char kHashWorldItems[] =
    "<fbcb0424988d98f42962c5cf03d1d5c63eff4d90111c69d7dbe46601dfafead>k__BackingField";
constexpr char kHashWorldChannels[] =
    "c82322b7b68a679cbaf01361e8cbcca32b31b3cec56eed625cb1238656f7564";
constexpr char kHashCharAvatarList[] =
    "f74f5ea7ab26829ea5fbdaf304a5bd28159165b93380d617696d0a0b343413b";
constexpr char kHashCharSelectedIndex[] =
    "<c9652bbaf6274fc1e1e0a8a2da4e9842fbdef0f54b7eac7215df60d3d363e20>k__BackingField";
constexpr char kHashCharSlotCount[] =
    "<ded9bb562b9f7a3ecfdcc59c36dfdf0c906b6692d3af49f01affa5d815c490b>k__BackingField";
constexpr char kHashChannelSelectedWorld[] =
    "fec5df63ad835b22ffeaa1f7ac120dc1360f483bba7927087dd1150498ee01f";
constexpr char kHashChannelSelectedId[] =
    "dce1eb39cf80c18e39bd82ebfeaa6782092d4d13ea3c00c504255a48c326985";
constexpr char kHashWorldId[] =
    "e0ea4909b34a3fd16338102f3edb3ddb14fec5d32b3807a6a590413c6c558c6";
constexpr char kHashWorldName[] =
    "d8cf5dbd31e8822c48e8a8cdab1e152ff8ac5384140871ce795fc8b1ead01d6";
constexpr char kHashChUserNo[] =
    "d47a9a0336b5c955243ee2982804a538b85966b1d6d0c4aeb7e125255fce45e";
constexpr char kHashChChannelId[] =
    "ea9f3d2e57ddca0ab8f828814178d7a6fe2d323096d885b6437fc381ea75fc4";
constexpr char kHashChAdult[] =
    "eb1378337833531a59c728fd04186262127c9984a4809636a611dd6b5681688";
constexpr char kHashChCapacity[] =
    "c357c138bdc955cf6664d9d50db7da3c3499e728af6c8aa3edcb2b055896578";

// 方法哈希（dump.cs · remount 2026-08-06）
constexpr char kHashSceneLoginGet[] =
    "f6435272a9a666f019a810a4c597917b4d49d1b079bcd6026e97c23a027165c";
constexpr char kHashOnClickWorldItem[] =
    "c09a89a71472fb514d15630eac394057d2954b379ea2e0e7b8e86927f9b950d";
constexpr char kHashSelectChannel[] =
    "e99c5caa7230b91d1dd184eac32caf0149be4ba3cefc7c3e1b1ec7d2527b4a5";
constexpr char kHashSelectCharacter[] =
    "c33b0b31a48858a3dff0bab0cf01f779d799848a1a1dd3570f88a01c1e2e3ba";
constexpr char kHashGetAvatarCount[] =
    "a9e2b2f23b83ca9a58b8918342e7c20320eeb46f3f49df9352d4e079589a9f4";
constexpr char kHashIsSlotEnable[] =
    "f2b9c21788695f4d5bf7ff9a5ccfb60256dbcd7747962ccdb8ffb0e2ea6d95d";
constexpr char kClassWorldItem[] =
    "eac92a483b9fc0e8d22a602d3bbda0535ae32e56bd1efc08475da8523d9ad3d";
constexpr char kClassChannelEntry[] =
    "b5b45504335620cb54edc2c18eee7a7ab00111ff95128c253bdef22be74bba7";
constexpr size_t kFbWorldItems = 0x58;  // 08-13：0x50 插入 GameObject，List<WorldItem> 顺移
constexpr size_t kFbWorldChannels = 0x38;
constexpr size_t kFbCharAvatarList = 0x170;
constexpr size_t kFbCharSelectedIndex = 0x168;
constexpr size_t kFbCharSlotCount = 0x1A8;
constexpr size_t kFbChannelSelectedWorld = 0x78;
constexpr size_t kFbChannelSelectedId = 0x80;
constexpr size_t kFbWorldId = 0x10;
constexpr size_t kFbWorldName = 0x18;
constexpr size_t kFbChUserNo = 0x18;
constexpr size_t kFbChChannelId = 0x1D;
constexpr size_t kFbChAdult = 0x1E;
constexpr size_t kFbChCapacity = 0x20;
// SceneLogin 登录阶段枚举 @+0x98；OnClickButtonSelect→SL 方法要求 ==2（IDA 种子实算）
constexpr size_t kOffSlLoginPhase = 0x98;
constexpr size_t kOffSlBusyFlag = 0x28;
constexpr int kSlPhaseForCharConfirm = 2;
size_t gOffSlChannelUi = kFbSlChannelUi;
size_t gOffSlWorldUi = kFbSlWorldUi;
size_t gOffSlCharUi = kFbSlCharUi;
size_t gOffWorldItems = kFbWorldItems;
size_t gOffWorldChannels = kFbWorldChannels;
size_t gOffCharAvatarList = kFbCharAvatarList;
size_t gOffCharSelectedIndex = kFbCharSelectedIndex;
size_t gOffCharSlotCount = kFbCharSlotCount;
size_t gOffChannelSelectedWorld = kFbChannelSelectedWorld;
size_t gOffChannelSelectedId = kFbChannelSelectedId;
size_t gOffWorldId = kFbWorldId;
size_t gOffWorldName = kFbWorldName;
size_t gOffChUserNo = kFbChUserNo;
size_t gOffChChannelId = kFbChChannelId;
size_t gOffChAdult = kFbChAdult;
size_t gOffChCapacity = kFbChCapacity;
bool gHolderFieldTried = false;
int gLoginFieldHits = 0;
int gMethodHits = -1;  // -1 = not probed yet

#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffArrLen (x::runtime::il2cpp_container::OffArrayMaxLength())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())

constexpr DWORD kTickMs = 80;
// 软重连进频后：把 worker 睡扁，尽快看见 charUi（bc23b1：Go→char≈250ms，探测别再加一截）。
constexpr DWORD kTickSoftWaitCharMs = 16;
constexpr DWORD kJobWaitMs = 4000;
constexpr DWORD kPhaseTimeoutMs = 60000;
// 选角页 phase=2 但 SL busy 粘死（BIN c1b2bd：avatars=1 busy=1 等到 60s Failed 再 GoWorld）。
constexpr DWORD kBusyStaleMs = 8000;
constexpr DWORD kLogThrottleMs = 3000;
constexpr DWORD kAfterWorldClickMs = 300;  // 点分区→等频道表；600 偏钝（BIN 09a8a2）
constexpr DWORD kAfterSelectChannelMs = 250;
// ???? Go ????????? SelectWorld ????????
constexpr DWORD kPumpFailBackoffMs = 1500;
constexpr DWORD kLeftChannelHoldMs = 800;
// 选角页：phase/busy 已就绪时少等；BIN 18:04 显示 avatars/phase 一帧就齐，700ms 偏肉。
constexpr DWORD kCharReadySettleMs = 200;
// Select→Confirm：0=同 Tick 连点。非 0 时仍会多吃 1 轮 worker Sleep(80)（BIN 18:17：设 100 实测 260ms）。
constexpr DWORD kAfterSelectCharMs = 0;
constexpr DWORD kCharConfirmRetryMs = 1500;
constexpr int kMaxCharConfirmAttempts = 4;
constexpr int kMaxSelectCharAttempts = 8;
// Idle 扫 WorldsCache / 活跃期写缓存的最小间隔（主线程 FindAll 高压期勿密扫）。
constexpr DWORD kIdleProbeMinMs = 4000;
constexpr DWORD kActiveProbeMinMs = 500;  // 有 UI 后的活跃探频下限
constexpr DWORD kWaitWorldProbeMinMs = 500;  // 等选区；原 1500 被 Connected settle 盖住后仍偏钝
constexpr DWORD kWorldsCacheScanMinMs = 2500;
constexpr DWORD kProbePumpTickMaxAgeMs = 2000;
// SessionState Connected=3；连上后略等选区 UI 灌表，勿抢泵（BIN be7fff）。
// 0.1.66+ 已无 job timeout；3s 过长（BIN 09a8a2：Connected→PickWorld≈3.2s 体感拖沓）→ 收至 800ms。
constexpr int kSessionConnected = 3;
constexpr DWORD kAfterConnectedSettleMs = 800;
// 软重连：登录页/分区表多半还在；再压 settle，尽快落到选角（bc23b1 已证 sticky 稳）。
constexpr DWORD kAfterConnectedSettleSoftMs = 100;
constexpr DWORD kAfterWorldClickSoftMs = 50;
constexpr DWORD kAfterSelectChannelSoftMs = 0;  // armed 即 GoWorld
// 0 在弱网/重进偶发「仍停选角页」→ 二次 Confirm（BIN 19:00/19:03）；给一短窗对齐冷启体感。
constexpr DWORD kCharReadySettleSoftMs = 80;
constexpr DWORD kWaitWorldProbeMinSoftMs = 100;
constexpr DWORD kLeftChannelHoldSoftMs = 0;  // charUi 一到就走；不必空等离频 settle
constexpr DWORD kPumpFailBackoffLoadMs = 4000;  // WaitWorldList 超时用更长退避
constexpr DWORD kWaitWorldProbeWaitMs = 400;    // 加载窗短等，勿 1.5s 占坑

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
    int charSelectedIndex = -1;  // UILoginCharacter+0x168；未知=-1
    int slLoginPhase = -1;       // SceneLogin+0x98
    int slBusy = -1;             // SceneLogin+0x28 bool
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
    WaitChannelArmed,  // ? UI+0x80 ???????? Go
    WaitCharSelect,
    PickChar,          // SelectCharacter only
    WaitCharArmed,     // ??????????
    ConfirmChar,       // OnClickButtonSelect
    WaitLeaveChar,     // ????????????????
    Done,
    Failed,
};

enum class JobKind : uint8_t {
    None = 0,
    ClickWorld,
    SelectChannel,
    GoWorld,  // OnClickButtonGoWorld only??? Trigger / ???????
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
// 软重连快轨：RequestRestart(soft_login) 置位；Done/Failed/关自动进 清。
std::atomic<bool> gSoftFastTrack{false};
// 上次成功进图的列表 id（SelectChannel 参数，非 UI ch.N）；跨 soft Restart 保留。
int gStickyChannelId = -1;
// NoteSticky 前一次 id。PickSticky 未命中时从随机池排除，避免遇人软重连又抽回原频。
int gStickyPrevId = -1;
DWORD gLastLogMs = 0;
DWORD gWorldClickedAt = 0;
DWORD gChannelSelectedAt = 0;
DWORD gEnterAttemptAt = 0;
DWORD gLeftChannelAt = 0;
DWORD gCharReadyAt = 0;
DWORD gCharSelectedAt = 0;
DWORD gCharConfirmAt = 0;
DWORD gBusyStuckSince = 0;
int gCharSelectTimeoutStreak = 0;
DWORD gPumpFailUntil = 0;
int gEnterAttempts = 0;
int gCharConfirmAttempts = 0;
int gSelectCharAttempts = 0;
int gPickCharIndex = -1;
char gWorldsFp[512]{};
DWORD gLastWorldsScanMs = 0;
DWORD gLastActiveProbeMs = 0;
DWORD gConnectedSinceMs = 0;
xcat::WorldsCacheEntry gPendingWorlds[xcat::kWorldsCacheMax]{};
uint32_t gPendingWorldsCount = 0;
char gPendingWorldsFp[512]{};
bool gPendingWorldsDirty = false;
// ProbeOnMain 模式：由 RefreshSnap 在排队前写入（主线程读）。
enum class ProbeMode : int { Active = 0, IdleCache = 1 };
ProbeMode gProbeMode = ProbeMode::Active;

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


bool PlausibleHolderOff(size_t off) { return off >= 0x10 && off < 0x400; }

bool FieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        field = nullptr;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!PlausibleHolderOff(off)) return false;
    *out = off;
    return true;
}

void EnsureHolderFieldOff() {
    if (gHolderFieldTried) return;
    gHolderFieldTried = true;
    void* scene = x::runtime::il2cpp::FindClass("", kClassSceneLogin);
    void* worldUi = x::runtime::il2cpp::FindClass("", kClassUiLoginWorld);
    void* worldItem = x::runtime::il2cpp::FindClass("", kClassWorldItem);
    void* charUi = x::runtime::il2cpp::FindClass("", kClassUiLoginCharacter);
    void* channelUi = x::runtime::il2cpp::FindClass("", kClassUiLoginChannel);
    void* chEntry = x::runtime::il2cpp::FindClass("", kClassChannelEntry);
    int hits = 0;
    if (FieldOffHit(scene, kHashSlChannelUi, kFbSlChannelUi, &gOffSlChannelUi)) ++hits;
    if (FieldOffHit(scene, kHashSlWorldUi, kFbSlWorldUi, &gOffSlWorldUi)) ++hits;
    if (FieldOffHit(scene, kHashSlCharUi, kFbSlCharUi, &gOffSlCharUi)) ++hits;
    if (FieldOffHit(worldUi, kHashWorldItems, kFbWorldItems, &gOffWorldItems)) ++hits;
    if (FieldOffHit(worldItem, kHashWorldChannels, kFbWorldChannels, &gOffWorldChannels)) ++hits;
    if (FieldOffHit(worldItem, kHashWorldId, kFbWorldId, &gOffWorldId)) ++hits;
    if (FieldOffHit(worldItem, kHashWorldName, kFbWorldName, &gOffWorldName)) ++hits;
    if (FieldOffHit(charUi, kHashCharAvatarList, kFbCharAvatarList, &gOffCharAvatarList)) ++hits;
    if (FieldOffHit(charUi, kHashCharSelectedIndex, kFbCharSelectedIndex, &gOffCharSelectedIndex))
        ++hits;
    if (FieldOffHit(charUi, kHashCharSlotCount, kFbCharSlotCount, &gOffCharSlotCount)) ++hits;
    if (FieldOffHit(channelUi, kHashChannelSelectedWorld, kFbChannelSelectedWorld,
                    &gOffChannelSelectedWorld))
        ++hits;
    if (FieldOffHit(channelUi, kHashChannelSelectedId, kFbChannelSelectedId,
                    &gOffChannelSelectedId))
        ++hits;
    if (FieldOffHit(chEntry, kHashChUserNo, kFbChUserNo, &gOffChUserNo)) ++hits;
    if (FieldOffHit(chEntry, kHashChChannelId, kFbChChannelId, &gOffChChannelId)) ++hits;
    if (FieldOffHit(chEntry, kHashChAdult, kFbChAdult, &gOffChAdult)) ++hits;
    if (FieldOffHit(chEntry, kHashChCapacity, kFbChCapacity, &gOffChCapacity)) ++hits;
    gLoginFieldHits = hits;
    constexpr int kExpect = 16;
    Log("login slots path=%s hits=%d/%d SL={c=0x%zX w=0x%zX ch=0x%zX} Avatar=0x%zX ChSel=0x%zX",
        hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
        gOffSlChannelUi, gOffSlWorldUi, gOffSlCharUi, gOffCharAvatarList, gOffChannelSelectedId);
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
    // ResolveTypes runs inside main-thread job ? must not nest pump.
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

// hash → 明文 → RVA/kind；dump OnClick* 无 hash 时走明文/kind。
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* nameHint = nullptr, const char* hashName = nullptr,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, nameHint, hashName);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

void LogMethodHitsOnce() {
    static bool sLogged = false;
    if (sLogged) return;
    if (!gKlassSceneLogin || !gKlassWorld || !gKlassChannel || !gKlassChar) return;
    sLogged = true;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    int hits = 0;
    ResolvePath p{};
    constexpr MethodShape kGet{0, TypeKind::Ptr, true, false, {}};
    if (ResolveMi(gKlassSceneLogin, kRvaSceneLoginGet, kGet, "get_Instance", kHashSceneLoginGet, &p) &&
        p != ResolvePath::Miss)
        ++hits;
    constexpr MethodShape kClick{1, TypeKind::Void, true, false, {TypeKind::I32}};
    if (ResolveMi(gKlassWorld, kRvaOnClickWorldItem, kClick, "OnClickWorldItem",
                  kHashOnClickWorldItem, &p) &&
        p != ResolvePath::Miss)
        ++hits;
    constexpr MethodShape kSelCh{1, TypeKind::Void, true, false, {TypeKind::I32}};
    if (ResolveMi(gKlassChannel, kRvaSelectChannel, kSelCh, "SelectChannel", kHashSelectChannel,
                  &p) &&
        p != ResolvePath::Miss)
        ++hits;
    constexpr MethodShape kSelChar{2, TypeKind::Void, true, false, {TypeKind::I32, TypeKind::Bool}};
    if (ResolveMi(gKlassChar, kRvaSelectCharacter, kSelChar, "SelectCharacter",
                  kHashSelectCharacter, &p) &&
        p != ResolvePath::Miss)
        ++hits;
    constexpr MethodShape kSlot{1, TypeKind::Bool, true, false, {TypeKind::I32}};
    if (ResolveMi(gKlassChar, kRvaIsSlotEnable, kSlot, "IsSlotEnable", kHashIsSlotEnable, &p) &&
        p != ResolvePath::Miss)
        ++hits;
    gMethodHits = hits;
    Log("methods path=%s hits=%d/5", hits == 5 ? "meta" : (hits ? "meta-partial" : "fallback"),
        hits);
}

bool ResolveTypes();

// 选区/选频/选角会调用托管方法；hash/RVA 全 miss 时 AtRva 旧兜底会 SEH 闪退。
bool LoginActionsArmed() {
    EnsureHolderFieldOff();
    (void)ResolveTypes();
    LogMethodHitsOnce();
    // SL 三个 holder 字段命中（或整表 ≥12）才允许点选；否则只探针不动作。
    if (gLoginFieldHits < 12) {
        LogThrottled("login actions blocked fieldHits=%d (need>=12)", gLoginFieldHits);
        return false;
    }
    if (!gKlassWorld || !gKlassChannel || !gKlassChar) {
        LogThrottled("login actions blocked missing UI klass w=%p ch=%p char=%p", gKlassWorld,
                     gKlassChannel, gKlassChar);
        return false;
    }
    if (gMethodHits >= 0 && gMethodHits < 3) {
        LogThrottled("login actions blocked methodHits=%d (need>=3)", gMethodHits);
        return false;
    }
    return true;
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
    // TypeGetObject 分配：推迟到真正要 FindAll 时（EnsureFindAllTypes）。
    // SceneLogin path does not require TypeObjects; FindAll fallback does.
    if (gKlassSceneLogin) return true;
    if (!(gKlassWorld && gKlassChannel && gKlassChar)) {
        LogThrottled("ResolveTypes sl=%p world=%p ch=%p char=%p", gKlassSceneLogin, gKlassWorld,
                     gKlassChannel, gKlassChar);
        return false;
    }
    return true;
}

void EnsureFindAllTypes() {
    if (!gTypeWorld && gKlassWorld) gTypeWorld = TypeObjectFromKlass(gKlassWorld);
    if (!gTypeChannel && gKlassChannel) gTypeChannel = TypeObjectFromKlass(gKlassChannel);
    if (!gTypeChar && gKlassChar) gTypeChar = TypeObjectFromKlass(gKlassChar);
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
    EnsureHolderFieldOff();
    if (!worldUi) return;
    void* items = ReadPtr(worldUi, gOffWorldItems);
    const int n = ListSize(items);
    if (n <= 0) return;

    // 主线程只采 id/名；写 user.ini 放到 worker，避免盘 IO 堵帧。
    xcat::WorldsCacheEntry entries[xcat::kWorldsCacheMax]{};
    uint32_t count = 0;
    char fp[512]{};
    size_t fpUsed = 0;
    for (int i = 0; i < n && count < xcat::kWorldsCacheMax; ++i) {
        void* w = ListAt(items, i);
        if (!w) continue;
        const int32_t id = ReadI32(w, gOffWorldId);
        char nameRaw[xcat::kWorldsCacheNameCap]{};
        (void)ReadIl2CppStringUtf8(ReadPtr(w, gOffWorldName), nameRaw, sizeof(nameRaw));
        entries[count].worldId = id;
        strncpy_s(entries[count].name, nameRaw, _TRUNCATE);
        ++count;

        if (fpUsed + 48 < sizeof(fp)) {
            const int wrote =
                snprintf(fp + fpUsed, sizeof(fp) - fpUsed, "%d:%s;", id, nameRaw[0] ? nameRaw : "-");
            if (wrote > 0) fpUsed += (size_t)wrote;
        }
    }
    if (count == 0) return;
    if (gWorldsFp[0] && strcmp(gWorldsFp, fp) == 0) return;
    if (gPendingWorldsDirty && gPendingWorldsFp[0] && strcmp(gPendingWorldsFp, fp) == 0) return;

    for (uint32_t i = 0; i < count; ++i) gPendingWorlds[i] = entries[i];
    gPendingWorldsCount = count;
    strncpy_s(gPendingWorldsFp, fp, _TRUNCATE);
    gPendingWorldsDirty = true;
}

void FlushPendingWorldsCache() {
    if (!gPendingWorldsDirty) return;
    xcat::WorldsCacheEntry entries[xcat::kWorldsCacheMax]{};
    const uint32_t count = gPendingWorldsCount;
    char fp[512]{};
    for (uint32_t i = 0; i < count && i < xcat::kWorldsCacheMax; ++i) {
        entries[i] = gPendingWorlds[i];
        const xcat::WorldNamesPack& wn = xcat::GetSharedWorldNames(x::runtime::GetBinDir());
        const std::string pretty = xcat::WorldNamePreferDisplay(wn, entries[i].name);
        if (!pretty.empty()) strncpy_s(entries[i].name, pretty.c_str(), _TRUNCATE);
    }
    strncpy_s(fp, gPendingWorldsFp, _TRUNCATE);
    gPendingWorldsDirty = false;
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
    EnsureHolderFieldOff();
    if (!charUi) return 0;
    // 只认 AvatarList@+0x170 实有角色数。
    // SlotCount@+0x1A8 = 解锁槽位数（非角色数）；列表未灌时误用 → IsSlotEnable 必假。
    return ListSize(ReadPtr(charUi, gOffCharAvatarList));
}

bool SlotEnabledOnMain(void* charUi, int index) {
    // bool(int) 在 Char 上唯一。
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kSlot{1, TypeKind::Bool, true, false, {TypeKind::I32}};
    auto* mi = ResolveMi(gKlassChar, kRvaIsSlotEnable, kSlot, "IsSlotEnable", kHashIsSlotEnable);
    auto fn = FnFromMi<FnIsSlotEnable>(mi, kRvaIsSlotEnable);
    if (!fn) return true;
    __try {
        return fn(charUi, index, mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void ProbeOnMain(void*) {
    (void)x::runtime::main_thread::AssertOnPumpThread("auto_enter.Probe");
    const ProbeMode mode = gProbeMode;
    UiSnap snap{};
    if (!gGA && !BindApis()) {
        gSnap = snap;
        return;
    }
    snap.typesOk = ResolveTypes();
    EnsureHolderFieldOff();
    LogMethodHitsOnce();

    // static 返回 SceneLogin* 的 () 唯一。
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kGet{0, TypeKind::Ptr, true, false, {}};
    auto* miGet = ResolveMi(gKlassSceneLogin, kRvaSceneLoginGet, kGet, "get_Instance", kHashSceneLoginGet);
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
        snap.channelUi = ReadPtr(sl, gOffSlChannelUi);
        snap.worldUi = ReadPtr(sl, gOffSlWorldUi);
        snap.charUi = ReadPtr(sl, gOffSlCharUi);
    }

    const bool needChar = (gPhase == Phase::WaitCharSelect || gPhase == Phase::PickChar ||
                           gPhase == Phase::WaitCharArmed || gPhase == Phase::ConfirmChar ||
                           gPhase == Phase::WaitLeaveChar);
    // FindAll 登录早期极重（BIN 8d294e：sl=null 时三件套 → 18:35:08 job timeout → pump idle）。
    // IdleCache：禁 FindAll。无 SceneLogin：只等实例，绝不 FindAll。
    // 有 SL：选角过渡可兜底 Char；World/Channel 仅 phase 饿 ≥8s 且泵健康时补洞。
    if (mode == ProbeMode::IdleCache) {
        // 只扫选区缓存：禁止任何 FindAll。
    } else if (!sl) {
        // 等 SceneLogin.get_Instance；Connecting/灌表窗口禁止 FindAll。
    } else {
        EnsureFindAllTypes();
        if (needChar && !snap.charUi && gTypeChar) {
            snap.charUi = FindFirstOfType(gTypeChar);
        }
        const bool starved = (GetTickCount() - gPhaseSince) >= 8000;
        if (starved && x::runtime::main_thread::IsPumpTicking(1500)) {
            const bool needWorld =
                (gPhase == Phase::WaitWorldList || gPhase == Phase::PickWorld);
            const bool needChannel =
                (gPhase == Phase::WaitChannelUi || gPhase == Phase::PickChannel ||
                 gPhase == Phase::WaitChannelArmed);
            if (needWorld && !snap.worldUi && gTypeWorld) {
                snap.worldUi = FindFirstOfType(gTypeWorld);
            }
            if (needChannel && !snap.channelUi && gTypeChannel) {
                snap.channelUi = FindFirstOfType(gTypeChannel);
            }
        }
    }

    if (snap.channelUi) {
        snap.selectedWorld = ReadPtr(snap.channelUi, gOffChannelSelectedWorld);
        snap.selectedChannelId = ReadI32(snap.channelUi, gOffChannelSelectedId);
    }
    if (snap.worldUi) {
        EnsureHolderFieldOff();
        snap.worldItemCount = ListSize(ReadPtr(snap.worldUi, gOffWorldItems));
        const DWORD now = GetTickCount();
        if (now - gLastWorldsScanMs >= kWorldsCacheScanMinMs) {
            gLastWorldsScanMs = now;
            CacheWorldItemsFromUi(snap.worldUi);
        }
    }
    if (snap.sceneLogin) {
        snap.slLoginPhase = ReadI32(snap.sceneLogin, kOffSlLoginPhase);
        snap.slBusy = static_cast<int>(ReadU8(snap.sceneLogin, kOffSlBusyFlag));
    }
    if (snap.charUi && mode != ProbeMode::IdleCache) {
        snap.avatarCount = AvatarCountOnMain(snap.charUi);
        const int sel = ReadI32(snap.charUi, gOffCharSelectedIndex);
        snap.charSelectedIndex = (sel >= 0 && sel < 32) ? sel : -1;
    }

    gSnap = snap;
}

bool SoftFastTrack() { return gSoftFastTrack.load(std::memory_order_acquire); }

DWORD AfterConnectedSettleMs() {
    return SoftFastTrack() ? kAfterConnectedSettleSoftMs : kAfterConnectedSettleMs;
}
DWORD AfterWorldClickMs() { return SoftFastTrack() ? kAfterWorldClickSoftMs : kAfterWorldClickMs; }
DWORD AfterSelectChannelMs() {
    return SoftFastTrack() ? kAfterSelectChannelSoftMs : kAfterSelectChannelMs;
}
DWORD CharReadySettleMs() { return SoftFastTrack() ? kCharReadySettleSoftMs : kCharReadySettleMs; }
DWORD WaitWorldProbeMinMs() {
    return SoftFastTrack() ? kWaitWorldProbeMinSoftMs : kWaitWorldProbeMinMs;
}
DWORD LeftChannelHoldMs() {
    return SoftFastTrack() ? kLeftChannelHoldSoftMs : kLeftChannelHoldMs;
}

// softFast 卡在「进频后等选角 / 确认后等离页」时加快 Tick，少吃 Sleep(80) 探测滞后。
DWORD WorkerTickSleepMs() {
    if (!SoftFastTrack()) return kTickMs;
    switch (gPhase) {
    case Phase::WaitChannelArmed:
    case Phase::WaitCharSelect:
    case Phase::PickChar:
    case Phase::WaitCharArmed:
    case Phase::ConfirmChar:
    case Phase::WaitLeaveChar:
        return kTickSoftWaitCharMs;
    default:
        return kTickMs;
    }
}

bool LoginNetReadyForWorldProbe() {
    const int st = kick_sniff::LastSessionState();
    if (st != kSessionConnected) {
        gConnectedSinceMs = 0;
        return false;
    }
    const DWORD now = GetTickCount();
    if (!gConnectedSinceMs) gConnectedSinceMs = now;
    return (now - gConnectedSinceMs) >= AfterConnectedSettleMs();
}

bool RefreshSnap(bool idleCache = false) {
    const DWORD now = GetTickCount();
    if (gPumpFailUntil && now < gPumpFailUntil) {
        LogThrottled("waiting main pump? (backoff)");
        return false;
    }
    // 泵未在跳 / 队列拥堵：宁可不探，避免压死帧再 job timeout 死循环。
    if (!x::runtime::main_thread::IsPumpTicking(kProbePumpTickMaxAgeMs)) {
        gPumpFailUntil = now + kPumpFailBackoffLoadMs;
        LogThrottled("waiting main pump? (idle)");
        return false;
    }
    if (x::runtime::main_thread::IsCongested()) {
        LogThrottled("pump congested — skip probe");
        return false;
    }
    gProbeMode = idleCache ? ProbeMode::IdleCache : ProbeMode::Active;
    const bool waitWorld =
        !idleCache && (gPhase == Phase::WaitWorldList || gPhase == Phase::PickWorld) &&
        !gSnap.worldUi;
    DWORD waitMs = 1500;
    if (idleCache) waitMs = 500;
    else if (gPhase == Phase::WaitCharSelect) waitMs = 3000;
    else if (waitWorld) waitMs = kWaitWorldProbeWaitMs;
    if (!x::runtime::main_thread::InvokeAndWait(&ProbeOnMain, nullptr, waitMs)) {
        // 选区加载窗超时：加长退避，别每 1.5s 再灌（BIN be7fff 32→41）。
        const DWORD back =
            waitWorld ? kPumpFailBackoffLoadMs : kPumpFailBackoffMs;
        gPumpFailUntil = GetTickCount() + back;
        LogThrottled("waiting main pump?");
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
            // void(int) ? World ????
            constexpr MethodShape kClick{1, TypeKind::Void, true, false, {TypeKind::I32}};
            auto* mi = ResolveMi(gKlassWorld, kRvaOnClickWorldItem, kClick, "OnClickWorldItem", kHashOnClickWorldItem);
            auto fn = FnFromMi<FnClickWorld>(mi, kRvaOnClickWorldItem);
            if (ui && fn) {
                fn(ui, a, mi);
                ok = true;
            }
            break;
        }
        case JobKind::SelectChannel: {
            // void(int) ? Channel ?????? EnterChannel?? RVA ??? + kind ???
            constexpr MethodShape kSel{1, TypeKind::Void, true, false, {TypeKind::I32}};
            auto* miSel = ResolveMi(gKlassChannel, kRvaSelectChannel, kSel, "SelectChannel", kHashSelectChannel);
            auto fnSel = FnFromMi<FnSelectChannel>(miSel, kRvaSelectChannel);
            if (ui && fnSel) {
                fnSel(ui, a, miSel);
                const int got = ReadI32(ui, gOffChannelSelectedId);
                ok = (got == a);
                if (!ok) Log("SelectChannel write miss want=%d got=%d", a, got);
            }
            break;
        }
        case JobKind::GoWorld: {
            // ?? OnClickButtonGoWorld??? TriggerEnterChannel/SendSelectWorld(0xA96D20)?
            // Go ???? SelectWorld????? = ????? ? ?????
            (void)b;
            constexpr MethodShape kGo{0, TypeKind::Void, true, false, {}};
            auto* miGo = ResolveMi(gKlassChannel, kRvaOnClickGoWorld, kGo, "OnClickButtonGoWorld");
            auto fnGo = FnFromMi<FnGoWorld>(miGo, kRvaOnClickGoWorld);
            if (ui && fnGo) {
                const int armed = ReadI32(ui, gOffChannelSelectedId);
                Log("GoWorld armedCh=%d (single click, no Trigger)", armed);
                fnGo(ui, miGo);
                ok = true;
            }
            break;
        }
        case JobKind::SelectCharIndex: {
            // void(int,bool) ? Char ????
            constexpr MethodShape kSel{2, TypeKind::Void, true, false, {TypeKind::I32, TypeKind::Bool}};
            auto* miSel = ResolveMi(gKlassChar, kRvaSelectCharacter, kSel, "SelectCharacter", kHashSelectCharacter);
            auto fnSel = FnFromMi<FnSelectChar>(miSel, kRvaSelectCharacter);
            if (ui && fnSel) {
                if (!SlotEnabledOnMain(ui, a)) {
                    const int listN = ListSize(ReadPtr(ui, gOffCharAvatarList));
                    const int slotN = ReadI32(ui, gOffCharSlotCount);
                    Log("SelectCharIndex slot disabled index=%d list=%d slotCount=%d", a, listN,
                        slotN);
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

void AeMainJob(void*) {
    (void)x::runtime::main_thread::AssertOnPumpThread("auto_enter.Main");
    RunJobOnMain();
}

bool EnqueueJobAndWait(JobKind kind, void* ui, int a, int b = 0) {
    switch (kind) {
    case JobKind::ClickWorld:
    case JobKind::SelectChannel:
    case JobKind::GoWorld:
    case JobKind::SelectCharIndex:
    case JobKind::ConfirmCharClick:
        if (!LoginActionsArmed()) {
            Log("EnqueueJob blocked kind=%u (login meta miss)", (unsigned)kind);
            return false;
        }
        break;
    default:
        break;
    }
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
    if (p == Phase::Failed) gSoftFastTrack.store(false, std::memory_order_release);
    // 注意：Done/Failed 时不要 SetLoginFreeze(false)。
    // 选角刚结束、尚未 play-ready 时解冻，会放开 titlebar/ports 的 lobby FindAll；
    // 解冻交给 titlebar 读到 vitals / invuln bind / drop_pool（进图后）。
}

bool PhaseTimedOut() { return GetTickCount() - gPhaseSince > kPhaseTimeoutMs; }

// 选角链路收尾：play-ready，或已 Confirm / 已在 WaitLeave 时 charUi 消失 → Done。
// 注意：WaitCharSelect 早期（刚离频道、选角页尚未出来）charUi 合法为 null，不能当 Done。
bool TryMarkEnterDone(const char* why) {
    if (gPhase == Phase::Done || gPhase == Phase::Failed) return true;
    const bool playReady = x::features::ports::world::IsPlayReady();
    if (playReady) {
        Log("enter Done (%s): play ready charUi=%d confirm=%d", why ? why : "?",
            gSnap.charUi ? 1 : 0, gCharConfirmAttempts);
        if (gPickedChannelId > 0) gStickyChannelId = gPickedChannelId;
        gSoftFastTrack.store(false, std::memory_order_release);
        gCharSelectTimeoutStreak = 0;
        gBusyStuckSince = 0;
        SetPhase(Phase::Done);
        if (gPickedChannelId > 0)
            channel_hop::SyncKnownAfterEnter(gPickedChannelId, why ? why : "enter_done");
        Log("Done — latched until autoEnter off stickyCh=%d", gStickyChannelId);
        return true;
    }
    const bool leftCharOk = !gSnap.charUi &&
                            (gCharConfirmAttempts > 0 || gPhase == Phase::WaitLeaveChar ||
                             gPhase == Phase::ConfirmChar);
    if (leftCharOk) {
        Log("enter Done (%s): left char UI confirm=%d", why ? why : "?", gCharConfirmAttempts);
        if (gPickedChannelId > 0) gStickyChannelId = gPickedChannelId;
        gSoftFastTrack.store(false, std::memory_order_release);
        gCharSelectTimeoutStreak = 0;
        gBusyStuckSince = 0;
        SetPhase(Phase::Done);
        if (gPickedChannelId > 0)
            channel_hop::SyncKnownAfterEnter(gPickedChannelId, why ? why : "enter_done_left_char");
        Log("Done — latched until autoEnter off stickyCh=%d", gStickyChannelId);
        return true;
    }
    return false;
}

bool WorldMatches(void* world, int32_t wantId, const char* wantName) {
    if (!world) return false;
    if (wantId != 0) return ReadI32(world, gOffWorldId) == wantId;
    if (!wantName || !wantName[0]) return false;
    char name[128]{};
    if (!ReadIl2CppStringUtf8(ReadPtr(world, gOffWorldName), name, sizeof(name))) return false;
    if (_stricmp(name, wantName) == 0) return true;
    const xcat::WorldNamesPack& wn = xcat::GetSharedWorldNames(x::runtime::GetBinDir());
    return xcat::WorldNameEquals(wn, name, wantName);
}

// ??????? PID/QPC????????????
uint32_t ChannelPickSeed(int poolN) {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    uint32_t s = GetTickCount();
    s ^= GetCurrentProcessId() * 2654435761u;
    s ^= static_cast<uint32_t>(qpc.LowPart);
    s ^= static_cast<uint32_t>(qpc.HighPart) * 97u;
    s ^= static_cast<uint32_t>(poolN) * 0x9E3779B9u;
    return s ? s : 0xA5A5A5A5u;
}

// 从当前 WorldItem.ci 喂 CCU 总和 + 填表（手动登录 idle / auto_enter 选频共用）。
// 同分区已 latch 则跳过；换分区可覆盖。allowRefresh：软重连同区也可覆盖。
// 填表仅在总和快照写入成功后更新，避免人数与 hop 表错位。
void FeedCcuFromWorldItem(void* worldItem, const char* src, bool allowRefresh = false) {
    if (!worldItem) return;
    EnsureHolderFieldOff();
    const int32_t worldId = ReadI32(worldItem, gOffWorldId);
    if (x::features::ccu::ShouldSkipFeed(worldId, allowRefresh)) return;

    void* list = ReadPtr(worldItem, gOffWorldChannels);
    const int n = ListSize(list);
    if (n <= 0) return;

    long long ccuSum = 0;
    int ccuN = 0;
    x::features::ccu::ChannelFillRow fillRows[64]{};
    int fillN = 0;
    for (int i = 0; i < n; ++i) {
        void* ch = ListAt(list, i);
        if (!ch) continue;
        const int id = (int)ReadU8(ch, gOffChChannelId);
        const int users = ReadI32(ch, gOffChUserNo);
        const int cap = ReadI32(ch, gOffChCapacity);
        const int adult = (int)ReadU8(ch, gOffChAdult);
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
    if (ccuN <= 0) return;
    const char* tag = (src && src[0]) ? src : "login";
    if (!x::features::ccu::NotifyWorldChannelSnapshot(ccuSum, ccuN, tag, worldId, allowRefresh))
        return;
    if (fillN > 0) {
        x::features::ccu::NotifyChannelFillTable(fillRows, fillN, tag, worldId);
    }
}

// ?? + ????????? id 1..64 ????????? 1..20??
// ?????????????????????????????
int PickOpenChannelId(void* worldItem) {
    EnsureHolderFieldOff();
    void* list = ReadPtr(worldItem, gOffWorldChannels);
    const int n = ListSize(list);
    // ????? 1..20 ??????-1=????? id??
    int u20[21];
    char flag20[21];  // ' '=eligible, 'A'=adult, 'F'=full, '?'=missing/bad
    for (int id = 1; id <= 20; ++id) {
        u20[id] = -1;
        flag20[id] = '?';
    }
    const bool softFast = SoftFastTrack();
    FeedCcuFromWorldItem(worldItem, softFast ? "soft_login" : "auto_enter", softFast);
    for (int i = 0; i < n; ++i) {
        void* ch = ListAt(list, i);
        if (!ch) continue;
        const int id = (int)ReadU8(ch, gOffChChannelId);
        const int users = ReadI32(ch, gOffChUserNo);
        const int cap = ReadI32(ch, gOffChCapacity);
        const int adult = (int)ReadU8(ch, gOffChAdult);
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
    }
    // ???? 1..20??? A=?? F=??????=?????=?????
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
    int candId[64];
    int candUsers[64];
    int candN = 0;
    for (int i = 0; i < n && candN < 64; ++i) {
        void* ch = ListAt(list, i);
        if (!ch) continue;
        const int id = (int)ReadU8(ch, gOffChChannelId);
        const int users = ReadI32(ch, gOffChUserNo);
        const int cap = ReadI32(ch, gOffChCapacity);
        const int adult = (int)ReadU8(ch, gOffChAdult);
        if (id < 1 || id > 64) continue;
        if (adult != 0) continue;
        if (cap > 0 && users >= cap) continue;
        if (users < 0) continue;
        candId[candN] = id;
        candUsers[candN] = users;
        ++candN;
    }
    if (candN <= 0) {
        Log("PickOpen ? none of %d channels", n);
        return -1;
    }
    // 仅 softFast：优先粘回上次进图/换频后的频道（仍空闲且非成人）；冷启仍 PickOpen。
    // sticky 满员/成人/不在表：与冷启同池随机，禁止就近/偏人少（避免扎堆）；禁止硬粘满频。
    const int sticky = gStickyChannelId;
    if (SoftFastTrack() && sticky > 0) {
        for (int i = 0; i < candN; ++i) {
            if (candId[i] == sticky) {
                Log("PickSticky id=%d users=%d (softFast)", sticky, candUsers[i]);
                return sticky;
            }
        }
        const uint32_t seedMiss = ChannelPickSeed(candN);
        int pickMiss = static_cast<int>(seedMiss % static_cast<uint32_t>(candN));
        const int avoid = gStickyPrevId;
        if (avoid >= 1 && avoid != sticky && candN > 1) {
            int filtered[64];
            int filteredUsers[64];
            int nF = 0;
            for (int i = 0; i < candN && nF < 64; ++i) {
                if (candId[i] == avoid) continue;
                filtered[nF] = candId[i];
                filteredUsers[nF] = candUsers[i];
                ++nF;
            }
            if (nF > 0) {
                pickMiss = static_cast<int>(seedMiss % static_cast<uint32_t>(nF));
                Log("PickSticky miss id=%d avoidPrev=%d → random open id=%d users=%d pool=%d/%d "
                    "(softFast full/adult/absent seed=0x%08X)",
                    sticky, avoid, filtered[pickMiss], filteredUsers[pickMiss], pickMiss + 1, nF,
                    seedMiss);
                return filtered[pickMiss];
            }
        }
        Log("PickSticky miss id=%d → random open id=%d users=%d pool=%d/%d "
            "(softFast full/adult/absent seed=0x%08X)",
            sticky, candId[pickMiss], candUsers[pickMiss], pickMiss + 1, candN, seedMiss);
        return candId[pickMiss];
    }
    const uint32_t seed = ChannelPickSeed(candN);
    const int pick = static_cast<int>(seed % static_cast<uint32_t>(candN));
    Log("PickOpen ? id=%d users=%d pool=%d/%d (seed=0x%08X)", candId[pick], candUsers[pick],
        pick + 1, candN, seed);
    return candId[pick];
}

int AvatarCount(void* charUi) {
    EnsureHolderFieldOff();
    // Prefer last main-thread snap; do not call managed GetAvatarCount from worker.
    if (charUi && charUi == gSnap.charUi) return gSnap.avatarCount;
    return ListSize(ReadPtr(charUi, gOffCharAvatarList));
}

bool StillOnTargetChannelUi(int32_t wantId, const char* wantName) {
    return gSnap.channelUi && gSnap.selectedWorld && WorldMatches(gSnap.selectedWorld, wantId, wantName);
}

bool CharUiReadyForPick() {
    return gSnap.charUi && gSnap.avatarCount > 0 &&
           gSnap.slLoginPhase == kSlPhaseForCharConfirm;
}

bool BusyFlagStale() {
    if (gSnap.slBusy == 0) {
        gBusyStuckSince = 0;
        return false;
    }
    const DWORD now = GetTickCount();
    if (!gBusyStuckSince) gBusyStuckSince = now ? now : 1;
    return (now - gBusyStuckSince) >= kBusyStaleMs;
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
    gBusyStuckSince = 0;
    gPumpFailUntil = 0;
    gEnterAttempts = 0;
    gCharConfirmAttempts = 0;
    gSelectCharAttempts = 0;
    gPickCharIndex = -1;
    gLastActiveProbeMs = 0;
    gConnectedSinceMs = 0;
    gJobPending.store(false);
    gJobDone.store(true);
    gSnap = {};
}

void Tick() {
    if (!gDesired.load()) {
        if (gPhase != Phase::Idle) {
            Log("desired off ? Idle");
            gSoftFastTrack.store(false, std::memory_order_release);
            gCharSelectTimeoutStreak = 0;
            SetPhase(Phase::Idle);
            ResetRuntime();
        }
        // Still probe occasionally to refresh worlds cache while on login UI.
        // 须 Connected+settle；Connecting 窗口禁探（BIN be7fff）。
        static DWORD sLastIdleProbe = 0;
        const DWORD now = GetTickCount();
        if (!LoginNetReadyForWorldProbe()) {
            LogThrottled("idle: wait Connected+settle before WorldsCache probe");
            return;
        }
        if (now - sLastIdleProbe > kIdleProbeMinMs) {
            sLastIdleProbe = now;
            if (RefreshSnap(/*idleCache=*/true)) {
                FlushPendingWorldsCache();
                // 手动登录：停在分区频道页时喂一次 CCU（不依赖自动进游戏）。
                if (gSnap.channelUi && gSnap.selectedWorld) {
                    FeedCcuFromWorldItem(gSnap.selectedWorld, "login");
                }
            }
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

    // Connecting / 刚连上选区灌表：完全不探，把主线程留给游戏（BIN be7fff）。
    if (gPhase == Phase::WaitWorldList && !gSnap.worldUi) {
        if (!LoginNetReadyForWorldProbe()) {
            LogThrottled("waiting Connected+settle before world probe (st=%d)",
                         kick_sniff::LastSessionState());
            return;
        }
        if (!x::runtime::main_thread::IsPumpTicking(kProbePumpTickMaxAgeMs)) {
            LogThrottled("waiting main pump? (pre-world, no queue)");
            return;
        }
    }

    // 等 SceneLogin / 选区 UI：降频探。
    const DWORD nowTick = GetTickCount();
    const bool waitingLoginUi =
        (gPhase == Phase::WaitWorldList && (!gSnap.sceneLogin || !gSnap.worldUi));
    const DWORD probeMin = waitingLoginUi ? WaitWorldProbeMinMs() : kActiveProbeMinMs;
    if (waitingLoginUi && gLastActiveProbeMs && (nowTick - gLastActiveProbeMs) < probeMin) {
        return;
    }

    if (!RefreshSnap()) return;
    gLastActiveProbeMs = GetTickCount();
    FlushPendingWorldsCache();
    if (!gSnap.typesOk && !gSnap.sceneLogin && !gSnap.worldUi && !gSnap.channelUi) {
        LogThrottled("waiting SceneLogin / class types?");
        return;
    }

    if (PhaseTimedOut()) {
        if (gPhase == Phase::WaitCharSelect) {
            ++gCharSelectTimeoutStreak;
            Log("phase timeout WaitCharSelect streak=%d busy=%d avatars=%d slPhase=%d",
                gCharSelectTimeoutStreak, gSnap.slBusy, gSnap.avatarCount, gSnap.slLoginPhase);
        } else {
            Log("phase timeout ? Failed (phase=%u)", (unsigned)gPhase);
        }
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
        // 已在选角页：再 resume 频道 → GoWorld 会把 SL busy 粘死（BIN c1b2bd）。
        if (CharUiReadyForPick()) {
            Log("WaitWorldList already on char UI avatars=%d busy=%d — skip world/channel",
                gSnap.avatarCount, gSnap.slBusy);
            SetPhase(Phase::WaitCharSelect);
            break;
        }
        // ??????????????????????
        {
            void* chUi = gSnap.channelUi;
            void* sel = gSnap.selectedWorld;
            if (chUi && sel && WorldMatches(sel, wantId, wantName)) {
                gPickedWorld = sel;
                Log("resume from channel UI worldId=%d softFast=%d armedCh=%d sticky=%d",
                    ReadI32(sel, gOffWorldId), SoftFastTrack() ? 1 : 0, gSnap.selectedChannelId,
                    gStickyChannelId);
                SetPhase(Phase::PickChannel);
                break;
            }
        }
        void* worldUi = gSnap.worldUi;
        if (!worldUi) {
            LogThrottled("waiting UILoginWorld? (sl=%p)", gSnap.sceneLogin);
            return;
        }
        if (gSnap.worldItemCount <= 0) {
            // softFast：空列表久等会拖满 soft 150s 墙钟（BIN 21:44）；提前 Failed 让 soft cycle。
            if (SoftFastTrack() && (GetTickCount() - gPhaseSince) >= 12000) {
                Log("WaitWorldList WorldItems empty %ums softFast — Failed (soft cycle)",
                    static_cast<unsigned>(GetTickCount() - gPhaseSince));
                SetPhase(Phase::Failed);
                return;
            }
            LogThrottled("waiting WorldItems?");
            return;
        }
        SetPhase(Phase::PickWorld);
        break;
    }
    case Phase::PickWorld: {
        void* worldUi = gSnap.worldUi;
        void* items = worldUi ? ReadPtr(worldUi, gOffWorldItems) : nullptr;
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
        Log("PickWorld index=%d id=%d", idx, ReadI32(world, gOffWorldId));
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
            LogThrottled("waiting UILoginChannel+WorldItem?");
            return;
        }
        // ??? SetWorldItem ?????????????????????? WorldItem?
        if (!WorldMatches(sel, wantId, wantName)) {
            LogThrottled("channel UI world mismatch (want=%d got=%d) ? wait SetWorldItem", wantId,
                         ReadI32(sel, gOffWorldId));
            return;
        }
        if (gWorldClickedAt && GetTickCount() - gWorldClickedAt < AfterWorldClickMs()) {
            return;
        }
        const int chN = ListSize(ReadPtr(sel, gOffWorldChannels));
        if (chN <= 0) {
            LogThrottled("waiting channel list on WorldItem?");
            return;
        }
        gPickedWorld = sel;
        Log("WaitChannelUi ready worldId=%d channels=%d", ReadI32(sel, gOffWorldId), chN);
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
        const int chId = PickOpenChannelId(gPickedWorld);
        if (chId < 0) {
            Log("no eligible channel");
            SetPhase(Phase::Failed);
            return;
        }
        // softFast：频道 UI 已武装到目标频 → 跳过 SelectChannel，下一拍直接 GoWorld。
        if (SoftFastTrack() && gSnap.selectedChannelId == chId) {
            Log("PickChannel already-armed id=%d softFast — skip SelectChannel → GoWorld", chId);
            gPickedChannelId = chId;
            gChannelSelectedAt = 0;  // AfterSelectChannel 门禁：0 = 不等
            gEnterAttempts = 0;
            SetPhase(Phase::WaitChannelArmed);
            break;
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
        if (CharUiReadyForPick()) {
            Log("WaitChannelArmed already on char UI avatars=%d busy=%d — skip GoWorld",
                gSnap.avatarCount, gSnap.slBusy);
            SetPhase(Phase::WaitCharSelect);
            break;
        }
        void* chUi = gSnap.channelUi;
        if (!chUi || !StillOnTargetChannelUi(wantId, wantName)) {
            LogThrottled("WaitChannelArmed: channel UI gone early ? wait char");
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
        if (gChannelSelectedAt && GetTickCount() - gChannelSelectedAt < AfterSelectChannelMs()) {
            return;
        }
        const int worldIdLog =
            gPickedWorld ? ReadI32(gPickedWorld, gOffWorldId) : (wantId > 0 ? wantId : 0);
        Log("GoWorld id=%d worldId=%d (Select armed)", gPickedChannelId, worldIdLog);
        if (!EnqueueJobAndWait(JobKind::GoWorld, chUi, gPickedChannelId)) {
            SetPhase(Phase::Failed);
            return;
        }
        if (gPickedChannelId > 0) gStickyChannelId = gPickedChannelId;
        gEnterAttemptAt = GetTickCount();
        gEnterAttempts = 1;
        gLeftChannelAt = 0;
        SetPhase(Phase::WaitCharSelect);
        // softFast：Go 后同 Tick 再探；服端已回包则少睡一轮再进 PickChar。
        if (SoftFastTrack() && RefreshSnap()) {
            gLastActiveProbeMs = GetTickCount();
            if (gSnap.charUi && gSnap.avatarCount > 0 &&
                gSnap.slLoginPhase == kSlPhaseForCharConfirm && gSnap.slBusy == 0) {
                Log("softFast GoWorld→char same-tick avatars=%d +%ums", gSnap.avatarCount,
                    static_cast<unsigned>(GetTickCount() - gEnterAttemptAt));
                gCharReadyAt = GetTickCount();
                SetPhase(Phase::PickChar);
            }
        }
        break;
    }
    case Phase::WaitCharSelect: {
        // 已发过 Confirm / 进图途中：charUi 可能先消失，勿当成「还在等选角页」。
        if (TryMarkEnterDone("WaitCharSelect")) break;

        if (gSnap.charUi) {
            const int count = gSnap.avatarCount;
            LogThrottled("char UI present avatars=%d chLinger=%p sl=%p phase=%d busy=%d", count,
                         gSnap.channelUi, gSnap.sceneLogin, gSnap.slLoginPhase, gSnap.slBusy);
            if (count <= 0) {
                gCharReadyAt = 0;
                return;
            }
            // OnClickButtonSelect 硬门：SceneLogin+0x98 必须已是选角阶段(=2)
            if (gSnap.slLoginPhase != kSlPhaseForCharConfirm) {
                gCharReadyAt = 0;
                gBusyStuckSince = 0;
                LogThrottled("waiting SL loginPhase=%d (need %d) before PickChar",
                             gSnap.slLoginPhase, kSlPhaseForCharConfirm);
                return;
            }
            if (gSnap.slBusy != 0 && !BusyFlagStale()) {
                gCharReadyAt = 0;
                LogThrottled("waiting SL busy=0 (got %d)", gSnap.slBusy);
                return;
            }
            if (gSnap.slBusy != 0) {
                Log("char UI busy stale %ums avatars=%d — PickChar anyway",
                    static_cast<unsigned>(GetTickCount() - gBusyStuckSince), count);
                SetPhase(Phase::PickChar);
                break;
            }
            if (!gCharReadyAt) {
                gCharReadyAt = GetTickCount();
                Log("char UI ready avatars=%d phase=%d — settle %ums before Select", count,
                    gSnap.slLoginPhase, (unsigned)CharReadySettleMs());
            }
            if (GetTickCount() - gCharReadyAt < CharReadySettleMs()) return;
            Log("char UI settled avatars=%d phase=%d (ignore lingering channel ptr)", count,
                gSnap.slLoginPhase);
            SetPhase(Phase::PickChar);
            break;
        }
        if (StillOnTargetChannelUi(wantId, wantName)) {
            gLeftChannelAt = 0;
            gCharReadyAt = 0;
            LogThrottled("waiting leave channel UI? (goSent=%d armed=%d, no auto-retry)",
                         gEnterAttempts, gSnap.selectedChannelId);
            return;
        }

        if (!gLeftChannelAt) gLeftChannelAt = GetTickCount();
        if (GetTickCount() - gLeftChannelAt < LeftChannelHoldMs()) {
            LogThrottled("left channel UI, settling before char UI?");
            return;
        }

        LogThrottled("waiting UILoginCharacter? (char=null sl=%p ch=%p confirm=%d)",
                     gSnap.sceneLogin, gSnap.channelUi, gCharConfirmAttempts);
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

        // 始终走 SelectCharacter：仅读 selectedIndex==目标 就 skip 时，Confirm 可能空转
        //（BIN 17:34：skip 后 OnClickButtonSelect×4 仍停在选角页）。
        const int curSel = gSnap.charSelectedIndex;
        Log("PickChar Select index=%d slot=%u count=%d (wasSelected=%d phase=%d busy=%d)", index,
            slot, count, curSel, gSnap.slLoginPhase, gSnap.slBusy);
        if (!EnqueueJobAndWait(JobKind::SelectCharIndex, charUi, index)) {
            ++gSelectCharAttempts;
            if (gSelectCharAttempts >= kMaxSelectCharAttempts) {
                Log("SelectChar failed %d times — give up", gSelectCharAttempts);
                SetPhase(Phase::Failed);
                return;
            }
            Log("SelectChar soft-fail %d/%d — re-settle", gSelectCharAttempts,
                kMaxSelectCharAttempts);
            gCharReadyAt = 0;
            SetPhase(Phase::WaitCharSelect);
            return;
        }
        gSelectCharAttempts = 0;
        gCharSelectedAt = GetTickCount();
        SetPhase(Phase::WaitCharArmed);
        // delay=0：不要 break，同 Tick 落入 WaitCharArmed→Confirm（否则再 Sleep 一轮才点）。
        if (kAfterSelectCharMs > 0) break;
        [[fallthrough]];
    }
    case Phase::WaitCharArmed: {
        if (TryMarkEnterDone("WaitCharArmed")) break;
        if (!gSnap.charUi) {
            // 选角 UI 已拆：可能正在进图，转 WaitLeaveChar 等 play-ready / Done
            SetPhase(Phase::WaitLeaveChar);
            break;
        }
        if (GetTickCount() - gCharSelectedAt < kAfterSelectCharMs) return;
        SetPhase(Phase::ConfirmChar);
        [[fallthrough]];
    }
    case Phase::ConfirmChar: {
        if (TryMarkEnterDone("ConfirmChar")) break;
        void* charUi = gSnap.charUi;
        if (!charUi) {
            SetPhase(Phase::WaitLeaveChar);
            break;
        }
        // busy/phase 未就绪：就地等，禁止打回 WaitCharSelect（BIN 17:56：Confirm 已发出后
        // busy=1 → re-Pick → 进图后卡在「left channel settling」永不 Done）。
        if (gSnap.slLoginPhase != kSlPhaseForCharConfirm || gSnap.slBusy != 0) {
            if (gCharConfirmAttempts > 0) {
                LogThrottled("ConfirmChar already sent — wait leave (phase=%d busy=%d)",
                             gSnap.slLoginPhase, gSnap.slBusy);
                SetPhase(Phase::WaitLeaveChar);
                break;
            }
            if (gSnap.slLoginPhase == kSlPhaseForCharConfirm && BusyFlagStale()) {
                Log("ConfirmChar busy stale %ums — click anyway index=%d",
                    static_cast<unsigned>(GetTickCount() - gBusyStuckSince), gPickCharIndex);
            } else {
                LogThrottled("ConfirmChar wait gate phase=%d busy=%d (need %d/0)",
                             gSnap.slLoginPhase, gSnap.slBusy, kSlPhaseForCharConfirm);
                return;
            }
        }
        Log("ConfirmChar click attempt=%d index=%d phase=%d busy=%d", gCharConfirmAttempts + 1,
            gPickCharIndex, gSnap.slLoginPhase, gSnap.slBusy);
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
        if (TryMarkEnterDone("WaitLeaveChar")) break;
        if (gCharConfirmAttempts >= kMaxCharConfirmAttempts) {
            Log("ConfirmChar exhausted attempts=%d — Failed (still on char UI)",
                gCharConfirmAttempts);
            SetPhase(Phase::Failed);
            return;
        }
        if (gCharConfirmAt && GetTickCount() - gCharConfirmAt >= kCharConfirmRetryMs) {
            // 进图途中 busy=1：再点 Select/Confirm 会打乱状态机；等 busy 清或 play-ready。
            if (gSnap.slBusy != 0) {
                LogThrottled("Confirm in flight busy=%d — hold retry (confirm=%d)", gSnap.slBusy,
                             gCharConfirmAttempts);
                return;
            }
            Log("still on char UI — retry via PickChar (%d/%d) phase=%d busy=%d",
                gCharConfirmAttempts + 1, kMaxCharConfirmAttempts, gSnap.slLoginPhase,
                gSnap.slBusy);
            SetPhase(Phase::PickChar);
            break;
        }
        LogThrottled("waiting leave char UI? (confirm=%d phase=%d busy=%d)", gCharConfirmAttempts,
                     gSnap.slLoginPhase, gSnap.slBusy);
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
        Sleep(WorkerTickSleepMs());
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
    // Signal only under loader lock ? do not join.
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
        // 开自动进：强制冻住 lobby FindAll（与进程默认 freeze=1 对齐，防中途被解冻）。
        x::runtime::managed_main::SetLoginFreeze(true);
        SetPhase(Phase::Idle);
        ResetRuntime();
    }
    if (!on && was) {
        // 关自动进：仅已进图才解冻；大厅保持冻结，避免 titlebar/ports 抢跑 FindAll。
        if (ports::world::IsPlayReady()) {
            x::runtime::managed_main::SetLoginFreeze(false);
        }
        gSoftFastTrack.store(false, std::memory_order_release);
        SetPhase(Phase::Idle);
        ResetRuntime();
    }
}

bool IsDesired() { return gDesired.load(); }

void NoteStickyChannel(int channelId1Based, const char* why) {
    if (channelId1Based < 1 || channelId1Based > 64) return;
    if (gStickyChannelId == channelId1Based) return;
    if (gStickyChannelId >= 1 && gStickyChannelId <= 64) gStickyPrevId = gStickyChannelId;
    Log("stickyCh %d→%d (%s)", gStickyChannelId, channelId1Based, why ? why : "?");
    gStickyChannelId = channelId1Based;
}

int StickyChannel1Based() {
    return (gStickyChannelId >= 1 && gStickyChannelId <= 64) ? gStickyChannelId : 0;
}

void RequestRestart(const char* why) {
    if (!gDesired.load()) {
        Log("RequestRestart skip: autoEnter off (%s)", why ? why : "?");
        return;
    }
    EnsureCs();
    // 软重进仍在大厅：保持 freeze，避免 titlebar/ports 抢跑 FindAll。
    x::runtime::managed_main::SetLoginFreeze(true);
    const bool soft = why && std::strcmp(why, "soft_login") == 0;
    gSoftFastTrack.store(soft, std::memory_order_release);
    SetPhase(Phase::Idle);
    ResetRuntime();  // 不碰 gStickyChannelId / gSoftFastTrack
    Log("RequestRestart → Idle (%s) worldId=%d slot=%u softFast=%d stickyCh=%d settle=%ums",
        why ? why : "?", gWorldId.load(), gCharSlot.load(), soft ? 1 : 0, gStickyChannelId,
        (unsigned)AfterConnectedSettleMs());
}

bool IsFailed() { return gPhase == Phase::Failed; }

int CharSelectTimeoutStreak() { return gCharSelectTimeoutStreak; }

int LastCharAvatarCount() { return gSnap.avatarCount; }

bool CharUiVisible() { return gSnap.charUi != nullptr; }

bool IsDone() { return gPhase == Phase::Done; }

void SoftHallSampleOnPump(void* user) {
    auto* ctx = static_cast<SoftHallCtx*>(user);
    if (!ctx) return;
    *ctx = {};
    if (!x::runtime::main_thread::AssertOnPumpThread("auto_enter.SoftHall")) return;
    if (!gGA && !BindApis()) return;
    if (!ResolveTypes()) return;
    EnsureHolderFieldOff();

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kGet{0, TypeKind::Ptr, true, false, {}};
    auto* miGet =
        ResolveMi(gKlassSceneLogin, kRvaSceneLoginGet, kGet, "get_Instance", kHashSceneLoginGet);
    auto getSl = FnFromMi<FnSceneLoginGet>(miGet, kRvaSceneLoginGet);
    void* sl = nullptr;
    if (getSl) {
        __try {
            sl = getSl(miGet);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sl = nullptr;
        }
    }
    ctx->ok = 1;
    if (!sl) return;

    void* ch = ReadPtr(sl, gOffSlChannelUi);
    void* wu = ReadPtr(sl, gOffSlWorldUi);
    if (ch && LooksLikeHeapPtr(ch)) {
        ctx->channelUi = 1;
        void* sel = ReadPtr(ch, gOffChannelSelectedWorld);
        if (sel && LooksLikeHeapPtr(sel)) ctx->selectedWorld = 1;
    }
    if (wu && LooksLikeHeapPtr(wu)) {
        ctx->worldUi = 1;
        ctx->worldItems = ListSize(ReadPtr(wu, gOffWorldItems));
        if (ctx->worldItems < 0) ctx->worldItems = 0;
    }
    // SL 槽可挂空壳 UILoginWorld（items=0）；BIN 01:18 ConnectLogin 空转时仍 world=1。
    // items 空则再 FindFirst，取列表更满的实例。
    if (ctx->worldItems <= 0 && gTypeWorld) {
        void* found = FindFirstOfType(gTypeWorld);
        if (found && LooksLikeHeapPtr(found)) {
            const int n = ListSize(ReadPtr(found, gOffWorldItems));
            if (n > ctx->worldItems) {
                ctx->worldUi = 1;
                ctx->worldItems = n;
            } else if (!ctx->worldUi) {
                ctx->worldUi = 1;
                if (n > 0) ctx->worldItems = n;
            }
        }
    }
    if (ctx->worldItems > 0 || (ctx->channelUi && ctx->selectedWorld)) ctx->ready = 1;
}

bool IsWorldItemsStarve(DWORD minAgeMs) {
    if (gPhase != Phase::WaitWorldList) return false;
    if (gSnap.worldItemCount > 0) return false;
    // 已能从频道页续进则不算饿死。
    if (gSnap.channelUi && gSnap.selectedWorld) return false;
    if (minAgeMs == 0) minAgeMs = 1;
    return (GetTickCount() - gPhaseSince) >= minAgeMs;
}

}  // namespace auto_enter
}  // namespace features
}  // namespace x
