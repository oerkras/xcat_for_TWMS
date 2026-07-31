// Kick / disconnect capture — data-plane only (GRAP: no INLINE HOOK).
// Watches SessionTcpLayer → Session._pendingErrorCode + SessionState.
// Logs: Dumps/runtime/kick.log (dev) or moduleDir/logs/kick.log

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "kick_sniff.h"

#include "../../runtime/log.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "User32.lib")

namespace x {
namespace features {
namespace kick_sniff {
namespace {

// TW SessionTcpLayer (= CMS NetworkManager), restored dump TypeDef 13772.
constexpr char kSessionTcpLayerClass[] =
    "d418de852f7af74b3618f37ea237eba34d0d7b7dfb53c8b0a4461bef3964232";
// TW Session (= CMS Framework.Network.Session), TypeDef 13797.
constexpr char kSessionClass[] =
    "cd7c86a41e028c570b85b628371d78d55a230ae6e0f4784a1a0eeaaecb7f47e";

// SessionTcpLayer fields (TW dump.cs TypeDef 13772 = CMS NetworkManager):
//   Session* @0x10, SessionState enum @0x18, Queue<InPacket> @0x28, … (+HashSet@0x48 vs CMS)
constexpr size_t kOffNmSession = 0x10;       // Session*
constexpr size_t kOffNmSessionState = 0x18;  // SessionState (int enum)
constexpr size_t kOffNmPacketQueue = 0x28;   // Queue<InPacket>*

// Session (= TW cd7c86a4… TypeDef 13797). TW inserts an extra object @0x50 vs CMS:
//   CMS: List@0x50 SessionState@0x58
//   TW:  object@0x50 List@0x58 SessionState@0x60
constexpr size_t kOffSessionPendingError = 0x40;  // int _pendingErrorCode
constexpr size_t kOffSessionState = 0x60;         // SessionState backing field (TW)
constexpr size_t kOffSessionRecvList = 0x58;      // List<InPacket>* (TW)
constexpr size_t kOffSessionClosed = 0x20;        // bool _isClosed

// TW dump.cs RVAs — call-edge targets (CMS semantic names). MethodInfo swap only (no .text).
constexpr uintptr_t kRvaNmCloseSession = 0x1CB8550;   // NetworkManager.CloseSession
constexpr uintptr_t kRvaNmDisconnect = 0x1CB9200;     // NetworkManager.Disconnect
constexpr uintptr_t kRvaSessionClose = 0x1CB8E40;     // Session.Close
constexpr uintptr_t kRvaSessionSetState = 0x1CC7A50;  // Session.set_SessionState
constexpr uintptr_t kRvaSessionOnDisc = 0x1CC9270;    // Session.OnDisconnect
constexpr uintptr_t kRvaSessionCloseSock = 0x1CC7F10; // Session.CloseSocket
// a480 local-disconnect fork (IDA 2026-07-31): distinguish DC7E70 vs Update→a480.
constexpr uintptr_t kRvaA480TryLocalDisc = 0xDC7E70;   // a480_TryLocalDisconnect_c596
constexpr uintptr_t kRvaA480UpdateCallA480 = 0xDD46B8; // Update: call a480_DoLocalNetworkDisconnect
constexpr uintptr_t kRvaA480DoLocalDisc = 0xDDC320;    // a480_DoLocalNetworkDisconnect (ref)
constexpr size_t kOffA480ForceDiscFlag = 0x298;        // bool force-local-disconnect
constexpr size_t kOffA480DiscTimer = 0x29C;            // float throttle
constexpr int kCallEdgeCap = 48;
constexpr int kStackFrames = 16;

// InPacket.PacketId @ +0x20 (ushort); Packet.Buffer @ +0x10 (byte[]*).
constexpr size_t kOffInPacketId = 0x20;
constexpr size_t kOffPacketBuffer = 0x10;

// SessionState (CMS): Disconnecting=0, Disconnected=1, Connecting=2, Connected=3
constexpr int kStateDisconnecting = 0;
constexpr int kStateDisconnected = 1;
constexpr int kStateConnecting = 2;
constexpr int kStateConnected = 3;

constexpr int kRingCap = 64;
constexpr int kScanPtrCap = 48;

constexpr wchar_t kLogDirDev[] = L"C:\\Users\\kras\\Desktop\\xcat_for_TWMS\\Dumps\\runtime";

HANDLE gLog = INVALID_HANDLE_VALUE;
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};
std::atomic<int> gLastErr{-1};
std::atomic<int> gLastState{-1};
std::atomic<bool> gSawDisconnect{false};
void* gNmCached = nullptr;

struct RingEntry {
    DWORD tick;
    uint16_t op;
    int blen;
    uint8_t head[4];
    char src;  // 'L'=recvList 'Q'=packetQueue
};
RingEntry gRing[kRingCap]{};
int gRingCount = 0;
int gRingNext = 0;

void* gScanPrev[kScanPtrCap]{};
int gScanPrevN = 0;

using FnDomainGet = void* (*)();
using FnDomainAssemblies = void* (*)(void* domain, size_t* size);
using FnAsmImage = void* (*)(void* assembly);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);
using FnFindAll = void* (*)(void* type, const void* method);
using FnObjGetClass = void* (*)(void* obj);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnClassGetMethods = void* (*)(void* klass, void** iter);

// Minimal MethodInfo head (Unity IL2CPP): methodPointer + virtualMethodPointer.
struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

using FnVoidThis = void(__fastcall*)(void* self, const void* method);
using FnSetState = void(__fastcall*)(void* self, int state, const void* method);

FnDomainGet gDomainGet = nullptr;
FnDomainAssemblies gDomainAssemblies = nullptr;
FnAsmImage gAsmImage = nullptr;
FnClassFromName gClassFromName = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
void* gStlKlass = nullptr;
void* gSessionKlass = nullptr;
uintptr_t gGaBase = 0;

std::atomic<int> gCallEdgeDumps{0};
std::atomic<bool> gCallEdgeInstalled{false};
std::atomic<bool> gHwbpInstalled{false};
std::atomic<bool> gInCallEdgeLog{false};
PVOID gVehHandle = nullptr;
HMODULE gSelfMod = nullptr;

// DR0 = a480.TryLocalDisconnect (DC7E70)  — MethodInfo-only callers
// DR1 = a480.Update @ call a480 (DD46B8) — Update timer/flag path
// DR2 = WRITE Session+0x60 (SessionState)
// DR3 = Nm.CloseSession                  — teardown correlation
struct HwbpTarget {
    const char* name;
    uintptr_t rva;  // 0 = dynamic write watch (DR2)
    bool selfIsSession;
    bool selfIsA480;  // rcx = a480* → dump +0x298/+0x29C
};
HwbpTarget gHwbpTargets[4] = {
    {"a480.TryLocalDisconnect", kRvaA480TryLocalDisc, false, true},
    {"a480.Update->DoLocalDisconnect", kRvaA480UpdateCallA480, false, true},
    {"Session.SessionState@write", 0, true, false},
    {"Nm.CloseSession", kRvaNmCloseSession, false, false},
};
uintptr_t gHwbpAddr[4]{};
std::atomic<uintptr_t> gStateWatchAddr{0};
std::atomic<void*> gStateWatchSession{nullptr};

constexpr int kDrWriteSlot = 2;

struct HookSlot {
    const char* name;
    uintptr_t rva;
    void* hook;
    void* klass;  // filled at install
    MethodInfoHead* mi;
    void* orig;
};
// Forward decls for hooks (defined after LogCallEdge).
void __fastcall HookNmCloseSession(void* self, const void* method);
void __fastcall HookNmDisconnect(void* self, const void* method);
void __fastcall HookSessionClose(void* self, const void* method);
void __fastcall HookSessionOnDisc(void* self, const void* method);
void __fastcall HookSessionCloseSock(void* self, const void* method);
void __fastcall HookSessionSetState(void* self, int state, const void* method);

HookSlot gHooks[] = {
    {"Nm.CloseSession", kRvaNmCloseSession, reinterpret_cast<void*>(&HookNmCloseSession), nullptr, nullptr,
     nullptr},
    {"Nm.Disconnect", kRvaNmDisconnect, reinterpret_cast<void*>(&HookNmDisconnect), nullptr, nullptr, nullptr},
    {"Session.Close", kRvaSessionClose, reinterpret_cast<void*>(&HookSessionClose), nullptr, nullptr, nullptr},
    {"Session.OnDisconnect", kRvaSessionOnDisc, reinterpret_cast<void*>(&HookSessionOnDisc), nullptr, nullptr,
     nullptr},
    {"Session.CloseSocket", kRvaSessionCloseSock, reinterpret_cast<void*>(&HookSessionCloseSock), nullptr,
     nullptr, nullptr},
    {"Session.set_SessionState", kRvaSessionSetState, reinterpret_cast<void*>(&HookSessionSetState), nullptr,
     nullptr, nullptr},
};

void WriteAll(HANDLE h, const char* buf, int n) {
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
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteAll(gLog, buf, n);
    OutputDebugStringA(buf);
    x::runtime::LogI("KickSniff", "%s", body);
}

bool DirExists(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&DirExists), &self) ||
        !self)
        return {};
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring s(path, n);
    const size_t cut = s.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut);
}

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    std::wstring dir = DirExists(kLogDirDev) ? kLogDirDev : ModuleDir();
    if (!DirExists(kLogDirDev) && !dir.empty()) {
        const std::wstring logs = dir + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        if (DirExists(logs)) dir = logs;
    }
    if (dir.empty()) return;
    const std::wstring full = dir + L"\\kick.log";
    gLog = CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

const char* StateName(int s) {
    switch (s) {
    case kStateDisconnecting:
        return "Disconnecting";
    case kStateDisconnected:
        return "Disconnected";
    case kStateConnecting:
        return "Connecting";
    case kStateConnected:
        return "Connected";
    default:
        return "?";
    }
}

// CMS Framework.Network.HackingAutoBlock — may appear as pendingError / notice payload.
const char* HackingAutoBlockName(int code) {
    switch (code) {
    case 12:
        return "Data";
    case 20:
        return "Start/No";
    case 21:
        return "Hit";
    case 22:
        return "Move";
    case 23:
        return "Packet";
    case 24:
        return "Position";
    case 25:
        return "Unrandomizer";
    case 26:
        return "ItemVac";
    case 27:
        return "ZAPIHack";
    case 28:
        return "MissHack";
    case 29:
        return "LowDamageHack";
    case 30:
        return "StanceHack";
    case 31:
        return "ReflectHack";
    case 32:
        return "SummonHack";
    case 33:
        return "ZeroHack";
    case 34:
        return "Warp_SkillHack";
    case 35:
        return "SkillHack";
    case 36:
        return "MobVec";
    case 37:
        return "MobVelocity";
    case 38:
        return "KickHack";
    case 39:
        return "UndeadCheat";
    case 40:
        return "NoDelayPacketRequest";
    case 251:
        return "Manual_MacroDetect";
    case 254:
        return "HackLog";
    case 255:
        return "Manual_NGSBlockSystem";
    default:
        return nullptr;
    }
}

// CMS ServerPacket names for TW opcodes that share the same numeric table (0..432).
// Used as hints only — TW numeric slots may drift; empty ring ≠ proof of local disconnect.
const char* CmsServerPacketHint(int op) {
    switch (op) {
    case 0:
        return "CheckPasswordResult";
    case 2:
        return "WorldInformation";
    case 9:
        return "MigrateCommand";
    case 10:
        return "AliveReq";
    case 13:
        return "SecurityPacket";
    case 20:
        return "LatestConnectedWorld";
    case 21:
        return "RecommendWorldMessage";
    case 71:
        return "ExpeditionNoti";
    case 72:
        return "FriendResult";
    case 74:
        return "GuildResult";
    case 76:
        return "TownPortal";
    case 77:
        return "BroadcastMsg";
    case 89:
        return "SetPotionDiscountRate";
    case 112:
        return "FamilyFamousPointIncResult";
    case 121:
        return "ScriptProgressMessage";
    case 131:
        return "GetServerTime";
    case 142:
        return "SetRedAccount";
    case 143:
        return "SystemBlockAlert";
    case 147:
        return "UseStellaRandomBoxResult";
    case 148:
        return "StellaRewardTradeResult";
    case 155:
        return "SetField";
    case 161:
        return "TransferFieldReqIgnored";
    case 205:
        return "SummonedEnterField";  // also sticky pendingError=205 observed
    case 215:
        return "UserMove";
    case 261:
        return "UserNoticeMsg";
    case 269:
        return "ReLoginKickNotice";
    case 272:
        return "UserPassiveMove";
    case 276:
        return "SkillCooltimeSet";
    case 278:
        return "MobEnterField";
    case 279:
        return "MobLeaveField";
    case 280:
        return "MobChangeController";
    case 281:
        return "MobMove";
    case 282:
        return "MobCtrlAck";
    case 283:
        return "MobCtrlHint";
    case 379:
        return "UIContext_s";
    case 398:
        return "GlobalMarketTerminated";
    case 401:
        return "AuctionHackLogUserInfoForNxLog";
    case 408:
        return "Warn";
    case 425:
        return "ServerPacket_425";  // TW OpRecv_01A9
    case 426:
        return "ServerPacket_426";  // TW OpRecv_01AA
    case 432:
        return "ForceDisconnect?";  // TW OpRecv_01B0 → Nm.Disconnect
    default:
        return nullptr;
    }
}

bool LooksLikeKickRelatedOp(int op) {
    switch (op) {
    case 143:  // SystemBlockAlert
    case 261:  // UserNoticeMsg
    case 269:  // ReLoginKickNotice
    case 408:  // Warn
    case 432:  // OpRecv_01B0 → local Nm.Disconnect (IDA 2026-07-31)
        return true;
    default:
        return false;
    }
}

bool LooksLikeHeapPtr(void* p) {
    const uintptr_t u = reinterpret_cast<uintptr_t>(p);
    // User-mode heap on Win64; reject small integers mistaken for pointers (e.g. 0x8).
    return u >= 0x10000 && u < 0x00007FFFFFFFFFFFULL;
}

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return -1;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ReadU8(void* obj, size_t off) {
    if (!obj) return -1;
    __try {
        return static_cast<int>(*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

float ReadF32(void* obj, size_t off) {
    if (!obj) return 0.f;
    __try {
        return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

uint16_t ReadU16(void* obj, size_t off) {
    if (!obj) return 0xFFFF;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFF;
    }
}

void* FindClass(const char* name) {
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
            void* klass = gClassFromName(image, "", name);
            if (!klass) klass = gClassFromName(image, "Framework.Network", name);
            if (klass) return klass;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Singleton<T> holds Lazy<T> _instance as first static field. Lazy.value layouts vary;
// try a few offsets for m_value after the object header.
void* TryLazyValue(void* lazy) {
    if (!lazy || !LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    if (gClassStaticData) {
        __try {
            void* p = gClassStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // Il2CppClass.static_fields offset differs by Unity/IL2CPP version — probe a few.
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    void* k = ReadPtr(obj, 0);  // Il2CppObject.klass
    return k == expectKlass;
}

bool LooksLikeSessionTcpLayer(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gStlKlass && !ObjKlassIs(cand, gStlKlass)) return false;
    void* sess = ReadPtr(cand, kOffNmSession);
    const int st = ReadI32(cand, kOffNmSessionState);
    // Must have a plausible Session* OR a sane Connected/Connecting state with null session
    // during handshake — never accept small-integer "pointers" like 0x8.
    if (sess && !LooksLikeHeapPtr(sess)) return false;
    if (st < 0 || st > 3) return false;
    // Prefer instances that actually hold a Session (in-game).
    if (LooksLikeHeapPtr(sess)) return true;
    // Allow Connecting with null session; reject Disconnecting/Disconnected without session
    // as sole acceptance criterion (that was how we cached garbage).
    return st == kStateConnecting || st == kStateConnected;
}

void* ResolveSessionTcpLayer() {
    if (gNmCached) {
        if (LooksLikeSessionTcpLayer(gNmCached)) return gNmCached;
        gNmCached = nullptr;
    }
    if (!gStlKlass) gStlKlass = FindClass(kSessionTcpLayerClass);
    if (!gStlKlass) gStlKlass = FindClass("SessionTcpLayer");
    if (!gStlKlass) return nullptr;

    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gStlKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Prefer parent Singleton<> statics (Lazy<T> _instance @ first slot).
    void* staticsKlass = gStlKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gStlKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) {
                __try {
                    gRuntimeClassInit(parent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            staticsKlass = parent;
        }
    }
    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gStlKlass);
    if (!statics) return nullptr;

    void* best = nullptr;
    for (size_t s = 0; s < 4; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeSessionTcpLayer(cand)) continue;
        void* sess = ReadPtr(cand, kOffNmSession);
        const int st = ReadI32(cand, kOffNmSessionState);
        if (LooksLikeHeapPtr(sess) && st == kStateConnected) {
            best = cand;
            break;
        }
        if (!best) best = cand;
    }
    if (!best) return nullptr;
    gNmCached = best;
    return best;
}

void RingPush(uint16_t op, int blen, const uint8_t head[4], char src) {
    if (op == 0xFFFF) return;
    RingEntry& e = gRing[gRingNext];
    e.tick = GetTickCount();
    e.op = op;
    e.blen = blen;
    e.head[0] = head[0];
    e.head[1] = head[1];
    e.head[2] = head[2];
    e.head[3] = head[3];
    e.src = src;
    gRingNext = (gRingNext + 1) % kRingCap;
    if (gRingCount < kRingCap) ++gRingCount;
}

bool ScanHad(void* pkt) {
    for (int i = 0; i < gScanPrevN; ++i) {
        if (gScanPrev[i] == pkt) return true;
    }
    return false;
}

void ObservePacket(void* pkt, char src, void** seen, int* seenN) {
    if (!LooksLikeHeapPtr(pkt) || !seen || !seenN) return;
    if (ScanHad(pkt)) return;
    for (int i = 0; i < *seenN; ++i) {
        if (seen[i] == pkt) return;
    }
    const uint16_t op = ReadU16(pkt, kOffInPacketId);
    void* bufObj = ReadPtr(pkt, kOffPacketBuffer);
    const int blen = bufObj ? ReadI32(bufObj, 0x18) : -1;
    uint8_t head[4] = {0, 0, 0, 0};
    if (bufObj && blen >= 4) {
        __try {
            auto* d = reinterpret_cast<uint8_t*>(bufObj) + 0x20;
            head[0] = d[0];
            head[1] = d[1];
            head[2] = d[2];
            head[3] = d[3];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    RingPush(op, blen, head, src);
    if (*seenN < kScanPtrCap) seen[(*seenN)++] = pkt;
}

void SampleList(void* list, char src, void** seen, int* seenN) {
    if (!LooksLikeHeapPtr(list) || !seen || !seenN) return;
    void* items = ReadPtr(list, 0x10);
    const int size = ReadI32(list, 0x18);
    if (!items || size <= 0) return;
    const int start = size > 16 ? size - 16 : 0;
    for (int i = start; i < size; ++i) {
        void* pkt = ReadPtr(items, 0x20 + (size_t)i * sizeof(void*));
        ObservePacket(pkt, src, seen, seenN);
    }
}

void SampleQueue(void* queue, char src, void** seen, int* seenN) {
    if (!LooksLikeHeapPtr(queue) || !seen || !seenN) return;
    // System.Collections.Generic.Queue<T>: _array@0x10, _head@0x18, _size@0x20 (typical)
    void* arr = ReadPtr(queue, 0x10);
    const int head = ReadI32(queue, 0x18);
    const int qsize = ReadI32(queue, 0x20);
    if (!arr || qsize <= 0 || qsize > 512) return;
    const int alen = ReadI32(arr, 0x18);  // array length
    if (alen <= 0 || alen > 4096) return;
    const int n = qsize < 16 ? qsize : 16;
    for (int i = 0; i < n; ++i) {
        const int idx = (head + (qsize - n) + i) % alen;
        if (idx < 0) continue;
        void* pkt = ReadPtr(arr, 0x20 + (size_t)idx * sizeof(void*));
        ObservePacket(pkt, src, seen, seenN);
    }
}

void SampleInbound(void* nm, void* session) {
    void* seen[kScanPtrCap]{};
    int seenN = 0;
    if (session) {
        void* list = ReadPtr(session, kOffSessionRecvList);
        SampleList(list, 'L', seen, &seenN);
    }
    if (nm) {
        void* q = ReadPtr(nm, kOffNmPacketQueue);
        SampleQueue(q, 'Q', seen, &seenN);
    }
    gScanPrevN = seenN < kScanPtrCap ? seenN : kScanPtrCap;
    for (int i = 0; i < gScanPrevN; ++i) gScanPrev[i] = seen[i];
}

void DumpRingHistogram(DWORD now, int windowMs) {
    // Count opcodes in the full ring and in the recent window.
    struct Buck {
        uint16_t op;
        int all;
        int win;
    };
    Buck bucks[kRingCap]{};
    int nb = 0;
    const int start = (gRingNext - gRingCount + kRingCap) % kRingCap;
    for (int i = 0; i < gRingCount; ++i) {
        const RingEntry& e = gRing[(start + i) % kRingCap];
        const int age = static_cast<int>(now - e.tick);
        int bi = -1;
        for (int j = 0; j < nb; ++j) {
            if (bucks[j].op == e.op) {
                bi = j;
                break;
            }
        }
        if (bi < 0) {
            if (nb >= kRingCap) continue;
            bi = nb++;
            bucks[bi].op = e.op;
            bucks[bi].all = 0;
            bucks[bi].win = 0;
        }
        ++bucks[bi].all;
        if (age >= 0 && age <= windowMs) ++bucks[bi].win;
    }
    // Simple descending sort by win then all.
    for (int i = 0; i < nb; ++i) {
        for (int j = i + 1; j < nb; ++j) {
            if (bucks[j].win > bucks[i].win ||
                (bucks[j].win == bucks[i].win && bucks[j].all > bucks[i].all)) {
                Buck t = bucks[i];
                bucks[i] = bucks[j];
                bucks[j] = t;
            }
        }
    }
    Log("  HIST window=%dms unique=%d (fmt: op count_win/count_all hint)", windowMs, nb);
    const int show = nb < 16 ? nb : 16;
    for (int i = 0; i < show; ++i) {
        const char* hint = CmsServerPacketHint(bucks[i].op);
        const bool kickish = LooksLikeKickRelatedOp(bucks[i].op);
        if (hint)
            Log("  hist[%d] op=%u(0x%04X) %d/%d hint=%s%s", i + 1, (unsigned)bucks[i].op,
                (unsigned)bucks[i].op, bucks[i].win, bucks[i].all, hint, kickish ? " *" : "");
        else
            Log("  hist[%d] op=%u(0x%04X) %d/%d%s", i + 1, (unsigned)bucks[i].op,
                (unsigned)bucks[i].op, bucks[i].win, bucks[i].all, kickish ? " *" : "");
    }
}

void DumpRing(const char* why) {
    Log("RING why=%s count=%d (newest last; src L=recvList Q=nmQueue)", why ? why : "?",
        gRingCount);
    if (gRingCount <= 0) {
        Log("  ring empty — S→C not sampled (consumed between polls) OR local self-disconnect");
        Log("  verdict=lean_local_or_missed (no kick-notice opcode in window)");
        return;
    }
    const DWORD now = GetTickCount();
    DumpRingHistogram(now, 3000);
    int kickHits = 0;
    int shown = 0;
    // Walk oldest→newest
    const int start = (gRingNext - gRingCount + kRingCap) % kRingCap;
    for (int i = 0; i < gRingCount; ++i) {
        const RingEntry& e = gRing[(start + i) % kRingCap];
        const int age = static_cast<int>(now - e.tick);
        const char* hint = CmsServerPacketHint(e.op);
        const bool kickish = LooksLikeKickRelatedOp(e.op);
        if (kickish && age <= 3000) ++kickHits;
        if (gRingCount > 24 && i < gRingCount - 24) continue;  // print last 24
        ++shown;
        if (hint)
            Log("  ring[%d] age=%dms src=%c op=%u(0x%04X) hint=%s%s len=%d head=%02X %02X %02X %02X",
                shown, age, e.src, (unsigned)e.op, (unsigned)e.op, hint, kickish ? " *" : "",
                e.blen, e.head[0], e.head[1], e.head[2], e.head[3]);
        else
            Log("  ring[%d] age=%dms src=%c op=%u(0x%04X)%s len=%d head=%02X %02X %02X %02X", shown,
                age, e.src, (unsigned)e.op, (unsigned)e.op, kickish ? " *" : "", e.blen, e.head[0],
                e.head[1], e.head[2], e.head[3]);
    }
    if (kickHits > 0)
        Log("  verdict=lean_server (kick-related S→C in last 3s, hits=%d)", kickHits);
    else
        Log("  verdict=lean_local_or_soft (ring has traffic but no notice/warn/kick hint in 3s)");
}

void DumpRecvList(void* session, int maxN);  // defined below

const char* BasenamePath(const char* path) {
    if (!path) return "?";
    const char* slash = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') slash = p + 1;
    }
    return slash;
}

void LogCallEdge(const char* edge, void* self, bool selfIsSession, bool selfIsA480, int extraState,
                 const char* via) {
    if (gInCallEdgeLog.exchange(true)) return;
    const int n = gCallEdgeDumps.fetch_add(1) + 1;
    if (n > kCallEdgeCap) {
        gInCallEdgeLog.store(false);
        return;
    }

    void* session = nullptr;
    int stNm = -1;
    if (selfIsSession) {
        session = self;
    } else if (selfIsA480) {
        // a480 this — resolve Session via cached SessionTcpLayer, not Nm offsets on a480.
        if (gNmCached) {
            session = ReadPtr(gNmCached, kOffNmSession);
            stNm = ReadI32(gNmCached, kOffNmSessionState);
        }
    } else if (self) {
        session = ReadPtr(self, kOffNmSession);
        stNm = ReadI32(self, kOffNmSessionState);
    }
    if (session && !LooksLikeHeapPtr(session)) session = nullptr;
    const int stSess = session ? ReadI32(session, kOffSessionState) : -1;
    const int err = session ? ReadI32(session, kOffSessionPendingError) : -1;
    const int closed = session ? ReadI32(session, kOffSessionClosed) : -1;
    const char* hack = HackingAutoBlockName(err);

    Log("CALL_EDGE #%d via=%s edge=%s tid=%lu self=%p session=%p stateNm=%d(%s) stateSess=%d(%s) "
        "pendingError=%d%s%s closed=%d extra=%d",
        n, via ? via : "?", edge ? edge : "?", GetCurrentThreadId(), self, session, stNm, StateName(stNm),
        stSess, StateName(stSess), err, hack ? " hackHint=" : "", hack ? hack : "", closed, extraState);

    if (selfIsA480 && self) {
        const int flag = ReadU8(self, kOffA480ForceDiscFlag);
        const float timer = ReadF32(self, kOffA480DiscTimer);
        Log("  a480 flag+0x298=%d timer+0x29C=%.4f", flag, (double)timer);
    }

    void* frames[kStackFrames]{};
    const USHORT depth = CaptureStackBackTrace(1, kStackFrames, frames, nullptr);
    for (USHORT i = 0; i < depth; ++i) {
        HMODULE mod = nullptr;
        char modPath[MAX_PATH]{};
        uintptr_t base = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &mod) &&
            mod) {
            base = reinterpret_cast<uintptr_t>(mod);
            GetModuleFileNameA(mod, modPath, MAX_PATH);
        }
        const char* name = BasenamePath(modPath[0] ? modPath : "?");
        const uintptr_t abs = reinterpret_cast<uintptr_t>(frames[i]);
        const uintptr_t rva = base ? (abs - base) : abs;
        Log("  #%u %s+0x%llX", (unsigned)i, name, (unsigned long long)rva);
    }
    gInCallEdgeLog.store(false);
}

void LogCallEdgeFromCtx(const char* edge, bool selfIsSession, bool selfIsA480, CONTEXT* ctx,
                        int extraState, void* selfOverride, const char* via) {
    if (!ctx) return;
    void* self = selfOverride ? selfOverride : reinterpret_cast<void*>(ctx->Rcx);
    LogCallEdge(edge, self, selfIsSession, selfIsA480, extraState, via ? via : "HWBP");
    // Writer RIP (for data BP, Rip is the instruction after the store).
    {
        HMODULE mod = nullptr;
        char modPath[MAX_PATH]{};
        uintptr_t base = 0;
        const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &mod) &&
            mod) {
            base = reinterpret_cast<uintptr_t>(mod);
            GetModuleFileNameA(mod, modPath, MAX_PATH);
        }
        Log("  rip %s+0x%llX", BasenamePath(modPath[0] ? modPath : "?"),
            (unsigned long long)(base ? (rip - base) : rip));
    }
    __try {
        auto* sp = reinterpret_cast<uintptr_t*>(ctx->Rsp);
        for (int i = 0; i < 8; ++i) {
            const uintptr_t ret = sp[i];
            if (!ret) continue;
            HMODULE mod = nullptr;
            char modPath[MAX_PATH]{};
            uintptr_t base = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(ret), &mod) &&
                mod) {
                base = reinterpret_cast<uintptr_t>(mod);
                GetModuleFileNameA(mod, modPath, MAX_PATH);
            }
            Log("  rsp[%d] %s+0x%llX", i, BasenamePath(modPath[0] ? modPath : "?"),
                (unsigned long long)(base ? (ret - base) : ret));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool AddrInModule(void* p, HMODULE mod) {
    if (!p || !mod) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= base && a < base + nt->OptionalHeader.SizeOfImage;
}

DWORD64 BuildDr7Mixed() {
    // Dr0/1/3: local enable + execute(00) + len1(00)
    // Dr2: local enable + write(01) + len4(11) when watch armed
    DWORD64 dr7 = (1ull << 0) | (1ull << 2) | (1ull << 6);
    if (gStateWatchAddr.load() != 0) {
        dr7 |= (1ull << 4);           // local enable Dr2
        dr7 |= (1ull << 24);          // R/W = write
        dr7 |= (3ull << 26);          // LEN = 4 bytes (SessionState int)
    }
    return dr7;
}

void FillHwbpContext(CONTEXT* ctx, bool enable) {
    if (!ctx) return;
    if (enable) {
        ctx->Dr0 = gHwbpAddr[0];
        ctx->Dr1 = gHwbpAddr[1];
        ctx->Dr2 = gStateWatchAddr.load();
        ctx->Dr3 = gHwbpAddr[3];
        gHwbpAddr[kDrWriteSlot] = ctx->Dr2;
        ctx->Dr6 = 0;
        ctx->Dr7 = BuildDr7Mixed();
    } else {
        ctx->Dr0 = ctx->Dr1 = ctx->Dr2 = ctx->Dr3 = 0;
        ctx->Dr6 = 0;
        ctx->Dr7 = 0;
    }
}

bool ApplyHwbpToThread(DWORD tid, bool enable) {
    const DWORD access = THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION;
    HANDLE th = OpenThread(access, FALSE, tid);
    if (!th) return false;

    const DWORD self = GetCurrentThreadId();
    bool suspended = false;
    if (tid != self) {
        if (SuspendThread(th) == (DWORD)-1) {
            CloseHandle(th);
            return false;
        }
        suspended = true;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(th, &ctx)) {
        FillHwbpContext(&ctx, enable);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(th, &ctx) != 0;
    }

    if (suspended) ResumeThread(th);
    CloseHandle(th);
    return ok;
}

int ApplyHwbpAllThreads(bool enable) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (ApplyHwbpToThread(te.th32ThreadID, enable)) ++n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

void UpdateSessionStateWatch(void* session) {
    uintptr_t want = 0;
    void* sess = nullptr;
    if (session && LooksLikeHeapPtr(session)) {
        want = reinterpret_cast<uintptr_t>(session) + kOffSessionState;
        sess = session;
    }
    const uintptr_t prev = gStateWatchAddr.exchange(want);
    gStateWatchSession.store(sess);
    if (want == prev) return;
    if (gHwbpInstalled.load()) {
        ApplyHwbpAllThreads(true);
        Log("CALL_EDGE_HWBP watch SessionState@%p (session=%p was=%p)", (void*)want, sess, (void*)prev);
    }
}

LONG CALLBACK CallEdgeVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!gHwbpInstalled.load()) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* ctx = ep->ContextRecord;
    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
    const DWORD64 dr6 = ctx->Dr6;

    // Prefer Dr6 bits: write BP never has Rip == watched data address.
    int hit = -1;
    for (int i = 0; i < 4; ++i) {
        if ((dr6 & (1ull << i)) && (gHwbpAddr[i] || (i == kDrWriteSlot && gStateWatchAddr.load()))) {
            hit = i;
            break;
        }
    }
    if (hit < 0) {
        for (int i = 0; i < 4; ++i) {
            if (i == kDrWriteSlot) continue;
            if (gHwbpAddr[i] && rip == gHwbpAddr[i]) {
                hit = i;
                break;
            }
        }
    }
    if (hit < 0) return EXCEPTION_CONTINUE_SEARCH;

    int extra = -1;
    void* selfOverride = nullptr;
    const char* via = "HWBP";

    if (hit == kDrWriteSlot) {
        via = "HWBP_WRITE";
        const uintptr_t watch = gStateWatchAddr.load();
        void* session = gStateWatchSession.load();
        selfOverride = session;
        if (watch) {
            __try {
                extra = *reinterpret_cast<int*>(watch);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                extra = -1;
            }
        }
        // Only care Disconnecting/Disconnected — Connecting spam would flood.
        if (extra != kStateDisconnecting && extra != kStateDisconnected) {
            ctx->Dr6 = 0;
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    bool fromSelf = false;
    if (hit != kDrWriteSlot) {
        __try {
            const uintptr_t ret = *reinterpret_cast<uintptr_t*>(ctx->Rsp);
            fromSelf = gSelfMod && AddrInModule(reinterpret_cast<void*>(ret), gSelfMod);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    if (!fromSelf) {
        LogCallEdgeFromCtx(gHwbpTargets[hit].name, gHwbpTargets[hit].selfIsSession,
                           gHwbpTargets[hit].selfIsA480, ctx, extra, selfOverride, via);
    }

    ctx->Dr6 = 0;
    ctx->EFlags |= 0x10000;  // RF — execute/write once without re-trap
    return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallHwbpCallEdge() {
    if (gHwbpInstalled.load()) return;
    if (!gGaBase) {
        Log("CALL_EDGE_HWBP skip: ga base 0");
        return;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallHwbpCallEdge), &gSelfMod);

    gHwbpAddr[0] = gGaBase + gHwbpTargets[0].rva;
    gHwbpAddr[1] = gGaBase + gHwbpTargets[1].rva;
    gHwbpAddr[kDrWriteSlot] = gStateWatchAddr.load();
    gHwbpAddr[3] = gGaBase + gHwbpTargets[3].rva;
    Log("CALL_EDGE_HWBP arm[0] %s ga+0x%llX -> %p", gHwbpTargets[0].name,
        (unsigned long long)gHwbpTargets[0].rva, (void*)gHwbpAddr[0]);
    Log("CALL_EDGE_HWBP arm[1] %s ga+0x%llX -> %p", gHwbpTargets[1].name,
        (unsigned long long)gHwbpTargets[1].rva, (void*)gHwbpAddr[1]);
    Log("CALL_EDGE_HWBP arm[2] %s WRITE Session+0x%zX (dynamic)", gHwbpTargets[2].name,
        kOffSessionState);
    Log("CALL_EDGE_HWBP arm[3] %s ga+0x%llX -> %p", gHwbpTargets[3].name,
        (unsigned long long)gHwbpTargets[3].rva, (void*)gHwbpAddr[3]);

    if (!gVehHandle) {
        gVehHandle = AddVectoredExceptionHandler(1, CallEdgeVeh);
        if (!gVehHandle) {
            Log("CALL_EDGE_HWBP FAIL AddVectoredExceptionHandler err=%lu", GetLastError());
            return;
        }
    }

    const int n = ApplyHwbpAllThreads(true);
    gHwbpInstalled.store(true);
    Log("CALL_EDGE_HWBP installed threads=%d veh=%p (DR exec+write, no .text)", n, gVehHandle);
}

void UninstallHwbpCallEdge() {
    if (!gHwbpInstalled.exchange(false)) return;
    gStateWatchAddr.store(0);
    gStateWatchSession.store(nullptr);
    ApplyHwbpAllThreads(false);
    if (gVehHandle) {
        RemoveVectoredExceptionHandler(gVehHandle);
        gVehHandle = nullptr;
    }
    for (int i = 0; i < 4; ++i) gHwbpAddr[i] = 0;
    Log("CALL_EDGE_HWBP uninstalled");
}

MethodInfoHead* FindMethodByRva(void* klass, uintptr_t rva) {
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

void InstallCallEdgeHooks() {
    if (gCallEdgeInstalled.load()) return;
    if (!gClassGetMethods) {
        Log("CALL_EDGE skip: il2cpp_class_get_methods missing");
        return;
    }
    if (!gStlKlass) gStlKlass = FindClass(kSessionTcpLayerClass);
    if (!gSessionKlass) gSessionKlass = FindClass(kSessionClass);
    if (gRuntimeClassInit) {
        if (gStlKlass) gRuntimeClassInit(gStlKlass);
        if (gSessionKlass) gRuntimeClassInit(gSessionKlass);
    }
    Log("CALL_EDGE install SessionTcpLayer=%p Session=%p (MethodInfo swap, no .text)", gStlKlass,
        gSessionKlass);

    int ok = 0;
    for (HookSlot& h : gHooks) {
        const bool nm = (h.rva == kRvaNmCloseSession || h.rva == kRvaNmDisconnect);
        h.klass = nm ? gStlKlass : gSessionKlass;
        if (!h.klass) {
            Log("CALL_EDGE FAIL %s — klass null", h.name);
            continue;
        }
        h.mi = FindMethodByRva(h.klass, h.rva);
        if (!h.mi) {
            Log("CALL_EDGE FAIL %s — MethodInfo RVA 0x%llX not found", h.name,
                (unsigned long long)h.rva);
            continue;
        }
        if (!PatchMethodInfo(h.mi, h.hook, &h.orig)) {
            Log("CALL_EDGE FAIL %s — patch MethodInfo %p", h.name, (void*)h.mi);
            h.mi = nullptr;
            continue;
        }
        ++ok;
        Log("CALL_EDGE OK %s mi=%p orig=%p rva=0x%llX", h.name, (void*)h.mi, h.orig,
            (unsigned long long)h.rva);
    }
    gCallEdgeInstalled.store(ok > 0);
    Log("CALL_EDGE installed %d/%d", ok, (int)(sizeof(gHooks) / sizeof(gHooks[0])));
}

void UninstallCallEdgeHooks() {
    if (!gCallEdgeInstalled.exchange(false)) return;
    for (HookSlot& h : gHooks) {
        if (h.mi && h.orig) RestoreMethodInfo(h.mi, h.orig);
        h.mi = nullptr;
        h.orig = nullptr;
    }
    Log("CALL_EDGE uninstalled");
}

FnVoidThis OrigAsVoid(const HookSlot& h) { return reinterpret_cast<FnVoidThis>(h.orig); }

void __fastcall HookNmCloseSession(void* self, const void* method) {
    LogCallEdge("Nm.CloseSession", self, false, false, -1, "MI");
    if (gHooks[0].orig) OrigAsVoid(gHooks[0])(self, method);
}
void __fastcall HookNmDisconnect(void* self, const void* method) {
    LogCallEdge("Nm.Disconnect", self, false, false, -1, "MI");
    if (gHooks[1].orig) OrigAsVoid(gHooks[1])(self, method);
}
void __fastcall HookSessionClose(void* self, const void* method) {
    LogCallEdge("Session.Close", self, true, false, -1, "MI");
    if (gHooks[2].orig) OrigAsVoid(gHooks[2])(self, method);
}
void __fastcall HookSessionOnDisc(void* self, const void* method) {
    LogCallEdge("Session.OnDisconnect", self, true, false, -1, "MI");
    if (gHooks[3].orig) OrigAsVoid(gHooks[3])(self, method);
}
void __fastcall HookSessionCloseSock(void* self, const void* method) {
    LogCallEdge("Session.CloseSocket", self, true, false, -1, "MI");
    if (gHooks[4].orig) OrigAsVoid(gHooks[4])(self, method);
}
void __fastcall HookSessionSetState(void* self, int state, const void* method) {
    if (state == kStateDisconnecting || state == kStateDisconnected)
        LogCallEdge("Session.set_SessionState", self, true, false, state, "MI");
    auto* orig = reinterpret_cast<FnSetState>(gHooks[5].orig);
    if (orig) orig(self, state, method);
}

void DumpRecvList(void* session, int maxN) {
    void* list = ReadPtr(session, kOffSessionRecvList);
    if (!list) {
        Log("  recvList=null");
        return;
    }
    void* items = ReadPtr(list, 0x10);
    const int size = ReadI32(list, 0x18);
    Log("  recvList size=%d items=%p", size, items);
    if (!items || size <= 0) return;
    const int n = size < maxN ? size : maxN;
    for (int i = size - n; i < size; ++i) {
        if (i < 0) continue;
        void* pkt = ReadPtr(items, 0x20 + (size_t)i * sizeof(void*));
        if (!pkt) continue;
        const uint16_t op = ReadU16(pkt, kOffInPacketId);
        const char* hint = CmsServerPacketHint(op);
        void* bufObj = ReadPtr(pkt, kOffPacketBuffer);
        int blen = bufObj ? ReadI32(bufObj, 0x18) : -1;
        unsigned b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        if (bufObj && blen >= 4) {
            __try {
                auto* d = reinterpret_cast<uint8_t*>(bufObj) + 0x20;
                b0 = d[0];
                b1 = d[1];
                b2 = d[2];
                b3 = d[3];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (hint)
            Log("  pendingIn[%d] op=%u(0x%04X) hint=%s len=%d head=%02X %02X %02X %02X", i,
                (unsigned)op, (unsigned)op, hint, blen, b0, b1, b2, b3);
        else
            Log("  pendingIn[%d] op=%u(0x%04X) len=%d head=%02X %02X %02X %02X", i, (unsigned)op,
                (unsigned)op, blen, b0, b1, b2, b3);
    }
}

void Snapshot(const char* why) {
    void* nm = ResolveSessionTcpLayer();
    if (!nm) {
        Log("SNAP why=%s SessionTcpLayer=null (not ready / klass miss / bad singleton)",
            why ? why : "?");
        DumpRing(why);
        return;
    }
    void* session = ReadPtr(nm, kOffNmSession);
    if (session && !LooksLikeHeapPtr(session)) {
        Log("SNAP why=%s nm=%p session=BAD(%p) — treating as null", why ? why : "?", nm, session);
        session = nullptr;
    }
    // One last sample before dump — catch packets still sitting at the edge.
    SampleInbound(nm, session);
    const int stNm = ReadI32(nm, kOffNmSessionState);
    const int stSess = session ? ReadI32(session, kOffSessionState) : -1;
    const int err = session ? ReadI32(session, kOffSessionPendingError) : -1;
    gLastErr.store(err);
    gLastState.store(stSess >= 0 ? stSess : stNm);

    const char* hack = HackingAutoBlockName(err);
    Log("SNAP why=%s nm=%p session=%p stateNm=%d(%s) stateSess=%d(%s) pendingError=%d%s%s",
        why ? why : "?", nm, session, stNm, StateName(stNm), stSess, StateName(stSess), err,
        hack ? " hackHint=" : "", hack ? hack : "");
    if (session) DumpRecvList(session, 8);
    DumpRing(why);
}

void OnStateChange(int prev, int now, int err) {
    const char* hack = HackingAutoBlockName(err);
    Log("STATE %s(%d) -> %s(%d) pendingError=%d%s%s", StateName(prev), prev, StateName(now), now,
        err, hack ? " hackHint=" : "", hack ? hack : "");
    if (now == kStateDisconnected || now == kStateDisconnecting) {
        gSawDisconnect.store(true);
        Snapshot("disconnect");
    }
}

DWORD WINAPI Worker(LPVOID) {
    Log("kick_sniff worker start (data-plane Session poll + S→C ring, no .text hook)");
    int lastState = -1;
    int lastErr = -1;
    for (int i = 0; i < 300 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll"); ++i)
        Sleep(50);

    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) {
        Log("GameAssembly.dll missing after wait — kick_sniff idle");
        while (!gStop.load()) Sleep(200);
        Log("kick_sniff worker stop");
        return 0;
    }
    gGaBase = reinterpret_cast<uintptr_t>(ga);
    gDomainGet = reinterpret_cast<FnDomainGet>(GetProcAddress(ga, "il2cpp_domain_get"));
    gDomainAssemblies =
        reinterpret_cast<FnDomainAssemblies>(GetProcAddress(ga, "il2cpp_domain_get_assemblies"));
    gAsmImage = reinterpret_cast<FnAsmImage>(GetProcAddress(ga, "il2cpp_assembly_get_image"));
    gClassFromName =
        reinterpret_cast<FnClassFromName>(GetProcAddress(ga, "il2cpp_class_from_name"));
    gClassStaticData =
        reinterpret_cast<FnClassStaticData>(GetProcAddress(ga, "il2cpp_class_get_static_field_data"));
    gClassParent = reinterpret_cast<FnClassParent>(GetProcAddress(ga, "il2cpp_class_get_parent"));
    gRuntimeClassInit =
        reinterpret_cast<FnRuntimeClassInit>(GetProcAddress(ga, "il2cpp_runtime_class_init"));
    gClassGetMethods =
        reinterpret_cast<FnClassGetMethods>(GetProcAddress(ga, "il2cpp_class_get_methods"));
    if (!gDomainGet || !gDomainAssemblies || !gAsmImage || !gClassFromName) {
        Log("il2cpp exports missing — kick_sniff cannot resolve Session");
        while (!gStop.load()) Sleep(200);
        Log("kick_sniff worker stop");
        return 1;
    }
    if (!gClassStaticData) Log("warn: il2cpp_class_get_static_field_data missing — probing klass offsets");

    gStlKlass = FindClass(kSessionTcpLayerClass);
    gSessionKlass = FindClass(kSessionClass);
    Log("SessionTcpLayer klass=%p Session klass=%p ga=%p ringCap=%d", gStlKlass, gSessionKlass,
        (void*)gGaBase, kRingCap);
    InstallCallEdgeHooks();
    InstallHwbpCallEdge();

    DWORD lastHb = GetTickCount();
    DWORD lastHwbp = GetTickCount();
    while (!gStop.load()) {
        void* nm = ResolveSessionTcpLayer();
        if (nm) {
            void* session = ReadPtr(nm, kOffNmSession);
            if (session && !LooksLikeHeapPtr(session)) session = nullptr;
            SampleInbound(nm, session);
            UpdateSessionStateWatch(session);
            const int st = session ? ReadI32(session, kOffSessionState)
                                   : ReadI32(nm, kOffNmSessionState);
            const int err = session ? ReadI32(session, kOffSessionPendingError) : -1;
            if (st != lastState || (err != lastErr && err != 0 && err != -1)) {
                if (lastState >= 0) OnStateChange(lastState, st, err);
                else
                    Log("STATE initial %s(%d) pendingError=%d", StateName(st), st, err);
                lastState = st;
                lastErr = err;
                gLastErr.store(err);
                gLastState.store(st);
            }
        } else if (lastState >= 0) {
            Log("STATE lost SessionTcpLayer (was %s)", StateName(lastState));
            DumpRing("lost_session");
            lastState = -1;
            gNmCached = nullptr;
            gScanPrevN = 0;
            UpdateSessionStateWatch(nullptr);
        }
        const DWORD now = GetTickCount();
        if (gHwbpInstalled.load() && now - lastHwbp >= 2000) {
            lastHwbp = now;
            ApplyHwbpAllThreads(true);  // refresh DR on new threads
        }
        if (now - lastHb >= 10000) {
            lastHb = now;
            Log("heartbeat nm=%p state=%d err=%d sawDisc=%d ring=%d hwbp=%d", gNmCached,
                gLastState.load(), gLastErr.load(), gSawDisconnect.load() ? 1 : 0, gRingCount,
                gHwbpInstalled.load() ? 1 : 0);
        }
        // 50→25ms when connected: catch short-lived InPackets before dispatch drains them.
        Sleep(lastState == kStateConnected ? 25 : 50);
    }
    Log("kick_sniff worker stop");
    UninstallHwbpCallEdge();
    UninstallCallEdgeHooks();
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    Log("kick_sniff Init pid=%lu", GetCurrentProcessId());
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!th) {
        Log("CreateThread FAILED err=%lu", GetLastError());
        return;
    }
    gThread.store(th);
    Log("CreateThread ok");
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
    UninstallHwbpCallEdge();
    UninstallCallEdgeHooks();
}

void DumpNow(const char* why) {
    if (gLog == INVALID_HANDLE_VALUE) OpenLog();
    Snapshot(why ? why : "DumpNow");
}

int LastPendingErrorCode() { return gLastErr.load(); }
int LastSessionState() { return gLastState.load(); }
bool SawDisconnect() { return gSawDisconnect.load(); }

}  // namespace kick_sniff
}  // namespace features
}  // namespace x
