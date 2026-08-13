// Kick / disconnect capture — data-plane only (GRAP: no INLINE HOOK).
// Watches SessionTcpLayer → Session._pendingErrorCode + SessionState.
// Logs: Dumps/runtime/kick.log (dev) or moduleDir/logs/kick.log

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "kick_sniff.h"

#include "../galaxy_token_probe/galaxy_token_probe.h"
#include "../ports/fly_fh_ban.h"
#include "../ports/foothold_port.h"
#include "../ports/ground_spoof.h"
#include "../channel_hop/channel_hop.h"
#include "../ports/world_port.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/main_thread_pump.h"

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

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// TW NetworkManager facade (df34ff16… TypeDef 13772 : Singleton<>) → Session* @0x10
// Session (TDI 13797 · 新 hash db2678aa…) 承载 Socket/seq/SendPacket；CALL_EDGE 挂 Session klass。
// 旧 CMS Session 哈希 cd7c86a4… 已并入 Session 类，不再单独 FindClass。

// NetworkManager facade + Session → il2cpp_network SSOT（勿抄 CMS RecvList/State）
#define kOffNmSession (x::runtime::il2cpp_network::OffNmSession())
#define kOffNmSessionState (x::runtime::il2cpp_network::OffNmSessionState())
#define kOffNmPacketQueue (x::runtime::il2cpp_network::OffNmPacketQueue())
#define kOffSessionPendingError (x::runtime::il2cpp_network::OffSessionPendingError())
#define kOffSessionState (x::runtime::il2cpp_network::OffSessionState())
#define kOffSessionRecvList (x::runtime::il2cpp_network::OffSessionRecvList())
#define kOffSessionClosed (x::runtime::il2cpp_network::OffSessionClosed())
#define kOffSessionSeqSend (x::runtime::il2cpp_network::OffSessionSeqSend())

// Packet / OutPacket fields：hash → field_get_offset（In/Out 同布局 · remount 2026-08-06）
constexpr char kPacketBaseClass[] =
    "bc38a59f64514be05547df152fb147799b9b47be10556655fe15485379f79f8";
constexpr char kHashPacketBuffer[] =
    "<eecde9fde4e63139526bc79e3dd104f14ac5c22e5746466cc884323b208497f>k__BackingField";
constexpr char kHashPacketOffset[] =
    "<fd14ad7f125e4854f90ee4410e2092e6dcc591f9489b916f3e9b396b0d3228f>k__BackingField";
// OutPacket.id@0x20（TDI 13775）— 勿用 InPacket TDI 13774 的 f4e004d8… backing
constexpr char kHashOutPacketId[] =
    "eba7d73821df86e3effbf761193b7b47a15019971597c1886c88a21d6742e63";
constexpr size_t kFbOutPacketId = 0x20;
constexpr size_t kFbPacketBuffer = 0x10;
constexpr size_t kFbPacketOffset = 0x18;
size_t gOffOutPacketId = kFbOutPacketId;
size_t gOffPacketBuffer = kFbPacketBuffer;
size_t gOffPacketOffset = kFbPacketOffset;
#define kOffOutPacketId (gOffOutPacketId)
#define kOffInPacketId (gOffOutPacketId)  // InPacket 同 ushort@0x20
#define kOffPacketBuffer (gOffPacketBuffer)
#define kOffPacketOffset (gOffPacketOffset)
bool gPktFieldTried = false;
int gPktFieldHits = -1;

// TW dump.cs RVAs — call-edge targets（Session TDI 13797 · remount 2026-08-06 按方法序对齐）。
// CloseSession=旧 CloseSocket；Disconnect=旧 Close；另挂 OnDisconnect / set_SessionState。
constexpr uintptr_t kRvaNmCloseSession = 0x1CFC240;  // remounted 2026-08-06
constexpr uintptr_t kRvaNmDisconnect = 0x1CED0A0;    // remounted 2026-08-06
constexpr uintptr_t kRvaSessionSetState = 0x1CFBDC0;  // remounted 2026-08-06: set_SessionState
constexpr uintptr_t kRvaSessionOnDisc = 0x1CFD410;  // remounted 2026-08-06: OnDisconnect
// Outbound funnel（Session.SendPacket）
constexpr uintptr_t kRvaSessionSend = 0x1CEF160;  // remounted 2026-08-06
// 方法哈希（Session 上 void() 极多，kind 不唯一；哈希漂 RVA 时仍可活）
constexpr char kHashCloseSession[] =
    "fed8f544dea4ed36fab81fce8888ce4eae488356a66ecc94a554c1a91a0c4fc";
constexpr char kHashDisconnect[] =
    "f92c6ead97c64b7cbec870270515e90f745d8b548483167233deebc597c08a7";
constexpr char kHashOnDisconnect[] =
    "ab1bcc45eefaa52c087210987a558fe4feb27ea2a0c472f3264314bd959a2aa";
constexpr char kHashSetSessionState[] =
    "ec8270518da3e3a940e0e4686d3d014d197277c8ad8b173b165d7ce6669314a";
constexpr char kHashSendPacket[] =
    "f9741df05df4a514fa1c509b0d209d4b098e4e1df5d244c21a7059d63308199";
// SEND OutPacket TDI 13775（勿用 13774 InPacket / b980769a…）
constexpr char kOutPacketClass[] =
    "a4c316b8f6223d2bd94628c2cfcfa1d7440b044c6a7043355d2177c60cafb9f";
// a480 local-disconnect（WM）：TryLocal 写 bool@0x2A0 + float@0x2A4 后 call DoLocal。
// 旁路 bool@0x290 仍在，HWBP 边沿以 0x2A0 为准。
// 08-13：DoLocal 唯一 code xref 在 FixedUpdate@0xDEF220 内 call @0xDF0002（旧 0xDDA277 已废）。
constexpr uintptr_t kRvaA480TryLocalDisc = 0xDE6BC0;  // remounted 2026-08-06
constexpr uintptr_t kRvaA480UpdateCallA480 = 0xDF0002;  // remounted 2026-08-13
constexpr uintptr_t kRvaA480DoLocalDisc = 0xDF0D40;  // remounted 2026-08-06
// CloseSession 直接调用方（runtime IDB 2026-08-12 · imagebase 0x7ff848c80000）
constexpr uintptr_t kRvaCsCaller1CC5520 = 0x1CEC6A0;
constexpr uintptr_t kRvaCsCaller1CD5570 = 0x1CFC6F0;  // MI/data only
constexpr uintptr_t kRvaCsCaller1CD92A0 = 0x1D00420;
constexpr uintptr_t kRvaCsParent1CC52C0 = 0x1CEC440;
constexpr uintptr_t kRvaCsParent1CC74C0 = 0x1CEE640;
constexpr uintptr_t kRvaCsParent1CD7870 = 0x1CFE9F0;
constexpr uintptr_t kRvaCsParent1CDA040 = 0x1D011C0;
// Session.CallbackRecv(IAsyncResult) — remount 后写 SessionState@+0x60=Disconnected
// dump hash aff6dcff…；写点 mov [rcx+60h],eax @ 0x1CD7796（rip 后一条 0x1CD7799）
constexpr uintptr_t kRvaSessionCallbackRecv = 0x1CFE620;
constexpr char kWorldManagerClass[] =
    "b8ea8013e52dada590b6003b130193bf382fb78e9581ae899270652538d4114";
constexpr char kHashA480ForceDisc[] =
    "a136e56515791b1482b6557868a420fba42c48e80e9784baa918078a349b24e";  // bool@0x2A0
constexpr char kHashA480ForceDiscAlt[] =
    "b8d8e29dc27af963986f9b0fcd695b7e901685bb690164945e99a0525af6e74";  // bool@0x290 旁路
constexpr char kHashA480DiscTimer[] =
    "f52d4ee10cccc5eabe17b610f4ac9f9ccf0ceb008263864b7e7b0f34747e2a8";  // float@0x2A4
constexpr size_t kFbA480ForceDiscFlag = 0x2A0;
constexpr size_t kFbA480DiscTimer = 0x2A4;
size_t gOffA480ForceDiscFlag = kFbA480ForceDiscFlag;
size_t gOffA480DiscTimer = kFbA480DiscTimer;
#define kOffA480ForceDiscFlag (gOffA480ForceDiscFlag)
#define kOffA480DiscTimer (gOffA480DiscTimer)
constexpr int kCallEdgeCap = 96;
constexpr int kStackFrames = 16;

#define kIl2cppArrayData (x::runtime::il2cpp_container::OffArrayData())
#define kIl2cppArrayLen (x::runtime::il2cpp_container::OffArrayMaxLength())

constexpr int kPacketDataPos = 6;

bool PlausiblePktOff(size_t off) { return off >= 0x10 && off < 0x400; }

bool PktFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, hash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (PlausiblePktOff(off)) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}

void EnsureKickFieldOff() {
    constexpr int kExpect = 5;
    if (gPktFieldTried && gPktFieldHits >= kExpect) return;
    // FindClass / WM Resolve：泵已装好则跳泵；装泵前（极少）就地解析，避免 InvokeAndWait↔Ensure 死锁。
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { EnsureKickFieldOff(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return;
    }
    if (!x::runtime::il2cpp::Ensure()) return;
    void* outKlass = x::runtime::il2cpp::FindClass("", kOutPacketClass);
    void* baseKlass = x::runtime::il2cpp::FindClass("", kPacketBaseClass);
    void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!wmKlass) wmKlass = x::runtime::il2cpp::FindClass("", kWorldManagerClass);
    int hits = 0;
    // PacketId on OutPacket; Buffer/Offset on Packet base (also walk from OutPacket)
    if (PktFieldOffHit(outKlass, kHashOutPacketId, kFbOutPacketId, &gOffOutPacketId)) ++hits;
    if (PktFieldOffHit(outKlass ? outKlass : baseKlass, kHashPacketBuffer, kFbPacketBuffer,
                       &gOffPacketBuffer))
        ++hits;
    if (PktFieldOffHit(outKlass ? outKlass : baseKlass, kHashPacketOffset, kFbPacketOffset,
                       &gOffPacketOffset))
        ++hits;
    if (PktFieldOffHit(wmKlass, kHashA480ForceDisc, kFbA480ForceDiscFlag, &gOffA480ForceDiscFlag) ||
        PktFieldOffHit(wmKlass, kHashA480ForceDiscAlt, 0x290, &gOffA480ForceDiscFlag))
        ++hits;
    if (PktFieldOffHit(wmKlass, kHashA480DiscTimer, kFbA480DiscTimer, &gOffA480DiscTimer)) ++hits;
    gPktFieldTried = true;
    if (hits != gPktFieldHits) {
        gPktFieldHits = hits;
        x::runtime::LogI(
            "KickSniff",
            "pkt/a480 fields path=%s hits=%d/5 id=0x%zX buf=0x%zX off=0x%zX a480=0x%zX",
            hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, gOffOutPacketId,
            gOffPacketBuffer, gOffPacketOffset, gOffA480ForceDiscFlag);
    }
}


// 出站 opcode 名字，只为让 send.log 不用另开对照表。
//
// 编号取自 CMS ClientPacket，但「TW == CMS 同编号」这个前提**只在 <= 53 站得住**。>= 180 段实测
// 为 **+2 平移**，2026-08-12 用 IDA 逐个实读混淆常量钉死（runtime IDB `Dumps/runtime/
// GameAssembly.dll.i64`，imagebase 0x7ff848c80000）。opcode 是「IMM ⊕/+ 运行时种子」，静态直读
// 得不到，须实读种子——见 .cursor/rules/ga-const-obfuscation.mdc：
//
//   MovePath_Flush 的 4 个调用方（= 移动包家族，各带且仅带一个 opcode 常量）：
//     sub_7FF849E671E0  0xEFB7 ^ seed@7FF84F4D90C4=0xEF98      = 47   UserMove      CMS 47  偏移 0
//     sub_7FF849E5E810  0xFFFF958A + seed@7FF84F4D8E78=0x6B2C  = 182  PetMove       CMS 180  +2
//     sub_7FF849E635C0  0xFFFFC626 + seed@7FF84F4D8FBC=0x3A98  = 190  SummonedMove  CMS 188  +2
//     sub_7FF849C01030  0xD99C ^ seed@7FF84F4CEAE4=0xD945      = 217  NpcMove       CMS 215  +2
//   我方吸物实际调用的两个原生入口（IDB 里已命名）：
//     DropPool_SendDropPickUpRequest 0x1C4 ^ seed@7FF84F4CE31C=0x11A  = 222 CMS 220 +2
//     Pet_SendDropPickUpRequest      0x7D45 ^ seed@7FF84F4CE434=0x7DFC = 185 CMS 183 +2
//
// 已知的 47 由同一套方法解出且完全吻合 → 方法可信；另五点一致 +2。旁证：出站流量里抓到过 CMS 的
// 枚举块哨兵（222=BEGIN_REACTORPOOL 3207 次、217=END_NPC 231 次），哨兵不可能被发，独立证伪同编号。
//
// **207 仍是推论**（故带 `?`）：mob 移动不走 MovePath_Flush，没读到它的常量；但 CMS MobMove=205 属
// 上述同一移动家族、家族内三个兄弟均为 +2，且 08-02 实解 207 包体为「mobId + 每怪 seq + 起点 xy +
// 14 字节元素」= MovePath 形状（拾取请求没有元素数组）、速率随场上怪数走（我方是 mob 控制端）。
//
// 插入点落在 54..179 之间、具体位置未知，那一段的名字最多可能错 2 个号，一律带 `?`。反证信号：若
// send.log 出现 op=205 / 220 / 223（+2 下对应 CMS 哨兵 BEGIN_LIFEPOOL / END_LIFEPOOL /
// END_DROPPOOL，发不出来），说明 +2 模型不成立。复算脚本：Dumps/_opcode_name_audit*.py。
//
// 入站表（下方 CmsServerPacketHint）不受影响：41 项里 40 项与 CMS ServerPacket 精确同号，TW 只在尾部追加
// （433 vs 420）。这个方向不对称正是长期误读的来源——别拿入站的准确率去信任出站。
const char* OpName(int op) {
    switch (op) {
        // <= 53：同编号已对线，可直接采信
        case 23: return "AliveAck";
        case 43: return "TransferField";
        case 47: return "UserMove";
        case 50: return "MeleeAttack";
        case 51: return "ShootAttack";
        case 52: return "MagicAttack";
        case 53: return "BodyAttack";
        // 54..179：插入点在这一段内，名字可能整体错 2 个号
        case 64: return "SelectNpc?";
        case 113: return "PortalScript?";
        case 114: return "PortalTeleport?";
        case 155: return "EnterTownPortal?";
        // >= 180：+2 平移。以下五个已 IDA 实读种子验明，可直接采信
        case 182: return "PetMove";
        case 185: return "PetDropPickUp";
        case 190: return "SummonedMove";
        case 217: return "NpcMove";
        case 222: return "DropPickUp";
        // 同段但未直读到常量，按移动家族 +2 推得
        case 207: return "MobMove?";
        case 225: return "ReactorHit?";
        default: return nullptr;
    }
}

// SessionState (CMS): Disconnecting=0, Disconnected=1, Connecting=2, Connected=3
constexpr int kStateDisconnecting = 0;
constexpr int kStateDisconnected = 1;
constexpr int kStateConnecting = 2;
constexpr int kStateConnected = 3;

constexpr int kRingCap = 64;
constexpr int kScanPtrCap = 48;

// 放在 kick.log 同目录或 DLL 同目录即武装发包探针；见 InstallSendProbe。
constexpr wchar_t kSendMarkerName[] = L"send_probe.on";
constexpr wchar_t kHwbpMarkerName[] = L"kick_hwbp.on";
constexpr wchar_t kTeardownHwbpMarkerName[] = L"kick_teardown_hwbp.on";
constexpr wchar_t kCallEdgeMarkerName[] = L"kick_call_edge.on";

HANDLE gLog = INVALID_HANDLE_VALUE;
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};
std::atomic<int> gLastErr{-1};
std::atomic<int> gLastState{-1};
std::atomic<bool> gSawDisconnect{false};
// Session 连续存活起点（tick，0=当前无会话）。DumpRing 用它区分进图 churn 与长命会话丢失。
std::atomic<DWORD> gSessionAliveSince{0};
std::atomic<uint32_t> gDisconnectSeq{0};
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

using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnClassGetMethods = void* (*)(void* klass, void** iter);

// Minimal MethodInfo head (Unity IL2CPP): methodPointer + virtualMethodPointer.
struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

using FnVoidThis = void(__fastcall*)(void* self, const void* method);
using FnSetState = void(__fastcall*)(void* self, int state, const void* method);

FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
void* gFacadeKlass = nullptr;  // NetworkManager Singleton 壳
void* gSessionKlass = nullptr;  // Session（CALL_EDGE MethodInfo）
void* gStlKlass = nullptr;      // alias: 解析实例时用 facade（历史名 SessionTcpLayer）
uintptr_t gGaBase = 0;

std::atomic<int> gCallEdgeDumps{0};
std::atomic<bool> gCallEdgeInstalled{false};
std::atomic<bool> gHwbpInstalled{false};
std::atomic<bool> gInCallEdgeLog{false};
PVOID gVehHandle = nullptr;
HMODULE gSelfMod = nullptr;

// DR0 = a480.TryLocalDisconnect (default) 或 Nm.Disconnect（teardown 模式）
// DR1 = a480.Update @ call a480 (default) 或 cs_caller_1CD5570（teardown）
// DR2 = WRITE Session+0x60 (SessionState)
// DR3 = Nm.CloseSession                  — teardown correlation
struct HwbpTarget {
    const char* name;
    uintptr_t rva;  // 0 = dynamic write watch (DR2)
    bool selfIsSession;
    bool selfIsA480;  // rcx = a480*/WM* → ForceDisc@0x2A0 + DiscTimer@0x2A4
};
HwbpTarget gHwbpTargets[4] = {
    {"a480.TryLocalDisconnect", kRvaA480TryLocalDisc, false, true},
    {"a480.Update->DoLocalDisconnect", kRvaA480UpdateCallA480, false, true},
    {"Session.SessionState@write", 0, true, false},
    {"Nm.CloseSession", kRvaNmCloseSession, false, false},
};
std::atomic<bool> gHwbpTeardownMode{false};
uintptr_t gHwbpAddr[4]{};
std::atomic<uintptr_t> gStateWatchAddr{0};
std::atomic<void*> gStateWatchSession{nullptr};

constexpr int kDrWriteSlot = 2;
// DR3 doubles as the outbound probe when the call-edge set is off (the common case).
constexpr int kDrSendSlot = 3;

// Which DR slots this module actually claimed this run. Tracking ownership lets the outbound
// probe take DR3 alone and leave the rest of the register file alone when call-edge HWBP is off.
bool gOwnSlot[4]{};
std::atomic<bool> gSendProbe{false};
std::atomic<uint32_t> gSendHits{0};
std::atomic<uint32_t> gSendUnreadable{0};

// Body capture for a short list of opcodes. Strictly quota'd.
constexpr int kDumpOpMax = 8;
// 配额是从会话开头往下扣的，所以它决定了我们抓到哪一头。排查掉线要看的是**断线前**那几个包，
// 24 个 UserMove 只够覆盖开局约 12 秒，恰好是没用的那一头。放大到能覆盖整场：UserMove ~2/s、
// 攻击 ~4/s，一场十分钟约 3600 个包，每行约 150B → send.log 几百 KB，可接受。
constexpr int kDumpQuotaDefault = 4000;
// 高频 opcode 单独给小配额。207 占了出站行数的 61%（89220/145118），按 4000 抓会把 send.log
// 撑大、把轮转提前，反而先冲掉断线前那段 op=47。取 400 只为回答一个是非题：包体到底是
// MovePath 形状（→ 确认 207=MobMove，+2 平移成立）还是拾取请求形状。拿到结论就该关掉。
constexpr int kDumpQuotaHighRate = 400;
constexpr int kDumpBodyBytes = 64;

// 只对已知高频的 opcode 收窄配额，其余照 Default。
int DumpQuotaForOp(int op) { return op == 207 ? kDumpQuotaHighRate : kDumpQuotaDefault; }
int gDumpOp[kDumpOpMax]{};
std::atomic<int> gDumpLeft[kDumpOpMax]{};
int gDumpOpN = 0;

struct HookSlot {
    const char* name;
    uintptr_t rva;
    void* hook;
    void* klass;  // filled at install
    MethodInfoHead* mi;
    void* orig;
    const char* methodHash;   // dump 方法名哈希；void() 不唯一时的主防漂
    const char* plainName;    // 偶发可读名
    int arity;
};

// Forward decls for hooks (defined after LogCallEdge).
void __fastcall HookNmCloseSession(void* self, const void* method);
void __fastcall HookNmDisconnect(void* self, const void* method);
void __fastcall HookNmOnDisc(void* self, const void* method);
void __fastcall HookNmSetState(void* self, int state, const void* method);

// Session TDI 13797：只挂 Session klass（勿挂错并入的 NM facade）。
// 旧 Session.Close / CloseSocket 与 Nm.Disconnect / CloseSession 同 RVA，不再重复安装。
HookSlot gHooks[] = {
    {"Nm.CloseSession", kRvaNmCloseSession, reinterpret_cast<void*>(&HookNmCloseSession), nullptr,
     nullptr, nullptr, kHashCloseSession, "CloseSession", 0},
    {"Nm.Disconnect", kRvaNmDisconnect, reinterpret_cast<void*>(&HookNmDisconnect), nullptr, nullptr,
     nullptr, kHashDisconnect, "Disconnect", 0},
    {"Nm.OnDisconnect", kRvaSessionOnDisc, reinterpret_cast<void*>(&HookNmOnDisc), nullptr, nullptr,
     nullptr, kHashOnDisconnect, "OnDisconnect", 0},
    {"Nm.set_SessionState", kRvaSessionSetState, reinterpret_cast<void*>(&HookNmSetState), nullptr,
     nullptr, nullptr, kHashSetSessionState, "set_SessionState", 1},
};

bool DirExists(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
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

std::wstring ResolveLogDir() {
    const std::wstring dev = x::runtime::OptionalRepoRuntimeDumpDir();
    if (!dev.empty()) return dev;
    std::wstring dir = ModuleDir();
    if (!dir.empty()) {
        const std::wstring logs = dir + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        if (DirExists(logs)) dir = logs;
    }
    return dir;
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
    const std::wstring dir = ResolveLogDir();
    if (!dir.empty()) (void)x::runtime::AppendDbgLog(dir + L"\\kick.log", buf, (DWORD)n);
    OutputDebugStringA(buf);
    x::runtime::LogI("KickSniff", "%s", body);
}

void OpenLog() {
    // kick.log 经 AppendDbgLog 写入；此处只确保目录存在。
    (void)ResolveLogDir();
    gLog = reinterpret_cast<HANDLE>(1);  // non-null sentinel so Init checks pass
}

HANDLE gSendLog = INVALID_HANDLE_VALUE;
CRITICAL_SECTION gSendLock;
bool gSendLockReady = false;
std::wstring gSendLogPath;

void OpenSendLog() {
    if (gSendLockReady && !gSendLogPath.empty()) return;
    const std::wstring dir = ResolveLogDir();
    if (dir.empty()) return;
    gSendLogPath = dir + L"\\send.log";
    if (!gSendLockReady) {
        InitializeCriticalSection(&gSendLock);
        gSendLockReady = true;
    }
    gSendLog = reinterpret_cast<HANDLE>(1);  // sentinel: path ready
}

// Runs inside the VEH on every outbound packet, so it never flushes — several game threads
// can reach Session.Send, hence the lock; the worker forces the flush every couple of seconds.
void SendLog(const char* fmt, ...) {
    if (gSendLogPath.empty() || !gSendLockReady) return;
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[320];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                           st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    EnterCriticalSection(&gSendLock);
    (void)x::runtime::AppendDbgLog(gSendLogPath, buf, (DWORD)n);
    LeaveCriticalSection(&gSendLock);
}

void FlushSendLog() {
    if (gSendLogPath.empty() || !gSendLockReady) return;
    EnterCriticalSection(&gSendLock);
    x::runtime::FlushDbgLog(gSendLogPath);
    LeaveCriticalSection(&gSendLock);
}

// Reads the payload at the Session.Send entry, i.e. before EncodeForSend runs, so the bytes
// should still be plaintext — the inbound ring's heads are ciphertext, which is what a capture
// taken too late would look like. Everything here runs inside the VEH: no allocation, no locks
// beyond SendLog's own, and every dereference behind SEH.
void DumpSendBody(const uint8_t* pkt, int op) {
    if (!pkt) return;
    int idx = -1;
    for (int i = 0; i < gDumpOpN; ++i) {
        if (gDumpOp[i] == op) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;
    int left = gDumpLeft[idx].load();
    while (left > 0 && !gDumpLeft[idx].compare_exchange_weak(left, left - 1)) {
    }
    if (left <= 0) return;

    char hex[3 * kDumpBodyBytes + 8];
    int hn = 0;
    int total = -1;
    __try {
        const auto* buf = *reinterpret_cast<const uint8_t* const*>(pkt + kOffPacketBuffer);
        total = *reinterpret_cast<const int*>(pkt + kOffPacketOffset);
        if (buf && total > kPacketDataPos) {
            const uint8_t* d = buf + kIl2cppArrayData + kPacketDataPos;
            int n = total - kPacketDataPos;
            if (n > kDumpBodyBytes) n = kDumpBodyBytes;
            for (int i = 0; i < n && hn + 4 < (int)sizeof(hex); ++i) {
                hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X ", d[i]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hn = 0;
    }
    hex[hn > 0 ? hn : 0] = '\0';
    SendLog("op=%d BODY off=%d [%d left] %s", op, total, left - 1, hex);
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

// CMS Framework.Network.HackingAutoBlock — 日志 hint 用；TW Session+0x40 实锤只写 204/205，
// 从未观察到 pendingError=22/24（见 docs/features/kick_sniff/断线错误码.md §3）。
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
//
// 入站方向的同编号假设**已核过**（2026-08-12）：本表 41 项里 40 项与 CMS ServerPacket 精确同号，
// 唯一例外 432 本就带 `?`（CMS 无此值，属 TW 尾部追加：TW 433 项 vs CMS 420 项）。所以这里不要
// 套出站表那条 +2 平移——出站才有平移，入站没有（原因见 OpName 注释）。
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
        return "SummonedEnterField";  // opcode hint only；勿与 sticky pendingError=205（Session 哨兵）混读
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
    void* k = x::runtime::il2cpp::FindClass("", name);
    if (!k) k = x::runtime::il2cpp::FindClass("Framework.Network", name);
    return k;
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

bool LooksLikeNetworkManagerFacade(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gFacadeKlass && !ObjKlassIs(cand, gFacadeKlass)) return false;
    void* sess = ReadPtr(cand, kOffNmSession);
    const int st = ReadI32(cand, kOffNmSessionState);
    // +0x18 是 SessionState（0..3）；勿与 Session.seq@+0x18 混淆。
    if (st < 0 || st > 3) return false;
    if (sess && !LooksLikeHeapPtr(sess)) return false;
    if (LooksLikeHeapPtr(sess)) return true;
    return st == kStateConnecting || st == kStateConnected;
}

void* ResolveSessionTcpLayer() {
    if (gNmCached) {
        if (LooksLikeNetworkManagerFacade(gNmCached)) return gNmCached;
        gNmCached = nullptr;
    }
    if (!gFacadeKlass) gFacadeKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    if (!gFacadeKlass) return nullptr;
    gStlKlass = gFacadeKlass;  // 兼容旧日志字段名

    // NEVER il2cpp_runtime_class_init on this worker — allocates → GC fatal
    // "Collecting from unknown thread". Game cctor already ran once Session exists;
    // if statics empty, poll again later.

    // Prefer parent Singleton<> statics (Lazy<T> _instance @ first slot).
    void* staticsKlass = gFacadeKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gFacadeKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) staticsKlass = parent;
    }
    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gFacadeKlass);
    if (!statics) return nullptr;

    void* best = nullptr;
    for (size_t s = 0; s < 8; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeNetworkManagerFacade(cand)) continue;
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
    const int blen = bufObj ? ReadI32(bufObj, x::runtime::il2cpp_container::OffArrayMaxLength()) : -1;
    uint8_t head[4] = {0, 0, 0, 0};
    if (bufObj && blen >= 4) {
        __try {
            auto* d = reinterpret_cast<uint8_t*>(bufObj) + kIl2cppArrayData;
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
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    const int size = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    if (!items || size <= 0) return;
    const int start = size > 16 ? size - 16 : 0;
    for (int i = start; i < size; ++i) {
        void* pkt = ReadPtr(items, kIl2cppArrayData + (size_t)i * sizeof(void*));
        ObservePacket(pkt, src, seen, seenN);
    }
}

void SampleQueue(void* queue, char src, void** seen, int* seenN) {
    if (!LooksLikeHeapPtr(queue) || !seen || !seenN) return;
    x::runtime::il2cpp_container::RefineFromQueueInstance(queue);
    void* arr = ReadPtr(queue, x::runtime::il2cpp_container::OffQueueArray());
    const int head = ReadI32(queue, x::runtime::il2cpp_container::OffQueueHead());
    const int qsize = ReadI32(queue, x::runtime::il2cpp_container::OffQueueSize());
    if (!arr || qsize <= 0 || qsize > 512) return;
    const int alen = ReadI32(arr, x::runtime::il2cpp_container::OffArrayMaxLength());
    if (alen <= 0 || alen > 4096) return;
    const int n = qsize < 16 ? qsize : 16;
    for (int i = 0; i < n; ++i) {
        const int idx = (head + (qsize - n) + i) % alen;
        if (idx < 0) continue;
        void* pkt = ReadPtr(arr, x::runtime::il2cpp_container::OffArrayData() +
                                     (size_t)idx * sizeof(void*));
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

int CountKickHintsInRing(DWORD now, int windowMs) {
    int hits = 0;
    const int start = (gRingNext - gRingCount + kRingCap) % kRingCap;
    for (int i = 0; i < gRingCount; ++i) {
        const RingEntry& e = gRing[(start + i) % kRingCap];
        const int age = static_cast<int>(now - e.tick);
        if (age < 0 || age > windowMs) continue;
        if (LooksLikeKickRelatedOp(e.op)) ++hits;
    }
    return hits;
}

void DumpRing(const char* why) {
    // 全量 RING+hist 很贵：每行 Log 都会进 kick.log + x.jsonl(LogI)。
    // 进图切会话时 SessionTcpLayer 常短暂空窗 → lost_session；环里通常没有踢线提示，
    // 却会一次倾倒 HIST+最多 24 条 ring（实测进场秒 ~47 条 KickSniff）。软路径只留一行。
    static DWORD s_lastFull = 0;
    static DWORD s_lastSoftLost = 0;
    static char s_lastWhy[48]{};
    constexpr DWORD kDumpRingCooldownMs = 15000;
    constexpr DWORD kSoftLostCooldownMs = 3000;
    const DWORD now = GetTickCount();
    const bool lostSession = why && strcmp(why, "lost_session") == 0;
    const int kickHitsEarly = CountKickHintsInRing(now, 3000);

    // churn 判据是「会话刚站起来就丢」：换图/迁频的空窗是秒级。站够 kChurnMaxAliveMs 再丢
    // 就不是 churn —— 卡登录期尤其如此：那段全程没有 why=disconnect，这一次 lost_session
    // 是唯一能拿到服务端到底回了什么的机会，精简掉就彻底没证据了。
    constexpr DWORD kChurnMaxAliveMs = 60000;
    const DWORD aliveSince = gSessionAliveSince.load();
    const DWORD aliveMs = aliveSince ? (now - aliveSince) : 0;
    const bool churnLike = aliveSince == 0 || aliveMs < kChurnMaxAliveMs;

    // 进图/迁频 churn：无踢线提示的 lost_session → 一行带过。不占用 full-dump 冷却，
    // 以免紧随其后的真踢（why=disconnect / 环内有 kick hint）被 15s 冷却误吞。
    if (lostSession && kickHitsEarly == 0 && churnLike) {
        if (s_lastSoftLost && now - s_lastSoftLost < kSoftLostCooldownMs) {
            return;  // 抖动窗口内静默去重（连 soft 一行也不再打）
        }
        s_lastSoftLost = now;
        Log("RING why=lost_session soft count=%d (no kick hint in 3s — field/session churn; "
            "skip HIST/ring)",
            gRingCount);
        return;
    }

    if (s_lastFull && now - s_lastFull < kDumpRingCooldownMs) {
        Log("RING why=%s skipped (cooldown %ums since last=%s count=%d)", why ? why : "?",
            (unsigned)(now - s_lastFull), s_lastWhy[0] ? s_lastWhy : "?", gRingCount);
        return;
    }
    s_lastFull = now;
    if (why && why[0]) {
        strncpy_s(s_lastWhy, why, _TRUNCATE);
    } else {
        s_lastWhy[0] = '\0';
    }

    Log("RING why=%s count=%d aliveMs=%u (newest last; src L=recvList Q=nmQueue)", why ? why : "?",
        gRingCount, (unsigned)aliveMs);
    if (gRingCount <= 0) {
        Log("  ring empty — S→C not sampled (consumed between polls) OR local self-disconnect");
        Log("  verdict=lean_local_or_missed (no kick-notice opcode in window)");
        return;
    }
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

// runtime IDB 2026-08-12：把栈帧 RVA 标成已知 CloseSession 链，方便 lean_local 一眼归因。
const char* TagCloseSessionChainRva(uintptr_t rva) {
    struct Range {
        uintptr_t lo;
        uintptr_t hi;  // exclusive
        const char* tag;
    };
    static constexpr Range kRanges[] = {
        {kRvaNmCloseSession, kRvaNmCloseSession + 0x334, "Nm.CloseSession"},
        {kRvaNmDisconnect, kRvaNmDisconnect + 0xf6, "Nm.Disconnect"},
        {kRvaSessionCallbackRecv, kRvaSessionCallbackRecv + 0x30f, "Session.CallbackRecv"},
        {kRvaCsCaller1CC5520, kRvaCsCaller1CC5520 + 0x2fc, "cs_via_1CC5520"},
        {kRvaCsCaller1CD5570, kRvaCsCaller1CD5570 + 0x584, "cs_via_1CD5570"},
        {kRvaCsCaller1CD92A0, kRvaCsCaller1CD92A0 + 0xb0f, "cs_via_1CD92A0"},
        {kRvaCsParent1CC52C0, kRvaCsParent1CC52C0 + 0x25b, "parent_1CC52C0"},
        {kRvaCsParent1CC74C0, kRvaCsParent1CC74C0 + 0x39d, "parent_1CC74C0"},
        {kRvaCsParent1CDA040, kRvaCsParent1CDA040 + 0x330, "parent_1CDA040"},
        {kRvaCsParent1CD7870, kRvaCsParent1CD7870 + 0x193b, "parent_1CD7870"},
    };
    for (const Range& r : kRanges) {
        if (rva >= r.lo && rva < r.hi) return r.tag;
    }
    return nullptr;
}

bool EdgeIsTeardown(const char* edge) {
    if (!edge) return false;
    return strstr(edge, "CloseSession") != nullptr || strstr(edge, "Disconnect") != nullptr ||
           strstr(edge, "1CD5570") != nullptr || strstr(edge, "1CD92A0") != nullptr;
}

// 纯内存读：CurFh / ban / ground_spoof。worker 与 HWBP VEH 均可调（勿碰托管方法）。
void LogTeardownPrestate() {
    const uint32_t curFh = x::features::ports::foothold::PeekCurFhId();
    const unsigned banMask = x::features::ports::fly_fh_ban::ActiveMask();
    const int banOn = x::features::ports::fly_fh_ban::IsBanActive() ? 1 : 0;
    const int spoofOn = x::features::ports::ground_spoof::IsEnabled() ? 1 : 0;
    int fireSp = 0;
    uint32_t fireFh = 0;
    int castSp = 0;
    uint32_t castFh = 0;
    x::features::ports::ground_spoof::FireDebug(&fireSp, &fireFh);
    x::features::ports::ground_spoof::CastDebug(&castSp, &castFh);
    Log("  prestate curFh=%u banOn=%d banMask=0x%x spoofOn=%d fireSp=%d fireFh=%u castSp=%d "
        "castFh=%u",
        curFh, banOn, banMask, spoofOn, fireSp, fireFh, castSp, castFh);
}

bool MarkerPresent(const wchar_t* name) {
    if (!name || !name[0]) return false;
    const std::wstring logDir = ResolveLogDir();
    const std::wstring modDir = ModuleDir();
    return (!logDir.empty() && FileExists(logDir + L"\\" + name)) ||
           (!modDir.empty() && FileExists(modDir + L"\\" + name));
}

// KICK_HWBP=1 / kick_hwbp.on → 默认 a480+CloseSession
// KICK_HWBP=2 / kick_teardown_hwbp.on → Disconnect + 1CD5570 + CloseSession（状态错乱踢线）
enum class HwbpWant : int { Off = 0, Default = 1, Teardown = 2 };

HwbpWant ResolveHwbpWant(const char** outHow) {
    char env[16]{};
    const DWORD n = GetEnvironmentVariableA("KICK_HWBP", env, sizeof(env));
    if (n > 0) {
        if (env[0] == '2' || _stricmp(env, "teardown") == 0) {
            if (outHow) *outHow = "KICK_HWBP=2";
            return HwbpWant::Teardown;
        }
        if (env[0] == '1') {
            if (outHow) *outHow = "KICK_HWBP=1";
            return HwbpWant::Default;
        }
    }
    if (MarkerPresent(kTeardownHwbpMarkerName)) {
        if (outHow) *outHow = "marker kick_teardown_hwbp.on";
        return HwbpWant::Teardown;
    }
    if (MarkerPresent(kHwbpMarkerName)) {
        if (outHow) *outHow = "marker kick_hwbp.on";
        return HwbpWant::Default;
    }
    if (outHow) *outHow = nullptr;
    return HwbpWant::Off;
}

void ApplyTeardownHwbpTargets() {
    gHwbpTargets[0] = {"Nm.Disconnect", kRvaNmDisconnect, false, false};
    gHwbpTargets[1] = {"cs_caller_1CD5570", kRvaCsCaller1CD5570, false, false};
    // [2] SessionState write + [3] CloseSession 不变
}

void LogCallEdge(const char* edge, void* self, bool selfIsSession, bool selfIsA480, int extraState,
                 const char* via) {
    // BIN 17:08：图内战 CloseSession → lost_session 被 inMap 吞、无 Disconnected → soft 永不 attempt。
    // 选角进图 CloseSession（!inMap）不粘；仅图内拆会话粘住，等回大厅再软重连。
    // 必须在取证预算（kCallEdgeCap）与重入锁之前 —— 这是功能逻辑，不能随日志配额一起断供：
    // 挂机数小时后 cap 耗尽，粘性武装会静默失效（连日志都没有）。
    if (edge && strstr(edge, "CloseSession") != nullptr &&
        x::features::ports::world::IsInMapScene()) {
        x::features::soft_login_probe::RequestDeferredAttempt("close_session_inmap");
    }

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

    if (EdgeIsTeardown(edge)) LogTeardownPrestate();

    if (selfIsA480 && self) {
        const int flag = ReadU8(self, kOffA480ForceDiscFlag);
        const float timer = ReadF32(self, kOffA480DiscTimer);
        Log("  a480 flag+0x%zX=%d timer+0x%zX=%.4f", kOffA480ForceDiscFlag, flag,
            kOffA480DiscTimer, (double)timer);
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
        const char* tag = nullptr;
        if (base && _stricmp(name, "GameAssembly.dll") == 0) tag = TagCloseSessionChainRva(rva);
        if (tag)
            Log("  #%u %s+0x%llX [%s]", (unsigned)i, name, (unsigned long long)rva, tag);
        else
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
            const char* name = BasenamePath(modPath[0] ? modPath : "?");
            const uintptr_t rva = base ? (ret - base) : ret;
            const char* tag =
                (base && _stricmp(name, "GameAssembly.dll") == 0) ? TagCloseSessionChainRva(rva)
                                                                  : nullptr;
            if (tag)
                Log("  rsp[%d] %s+0x%llX [%s]", i, name, (unsigned long long)rva, tag);
            else
                Log("  rsp[%d] %s+0x%llX", i, name, (unsigned long long)rva);
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

// Local-enable bit plus the R/W+LEN nibble belonging to one slot.
DWORD64 SlotDr7Mask(int i) { return (1ull << (2 * i)) | (0xFull << (16 + 4 * i)); }

DWORD64 OwnedDr7Mask() {
    DWORD64 m = 0;
    for (int i = 0; i < 4; ++i)
        if (gOwnSlot[i]) m |= SlotDr7Mask(i);
    return m;
}

DWORD64 BuildDr7Owned() {
    // Dr0/1/3: local enable + execute(00) + len1(00)
    // Dr2: local enable + write(01) + len4(11) when watch armed
    DWORD64 dr7 = 0;
    if (gOwnSlot[0]) dr7 |= (1ull << 0);
    if (gOwnSlot[1]) dr7 |= (1ull << 2);
    if (gOwnSlot[3]) dr7 |= (1ull << 6);
    if (gOwnSlot[kDrWriteSlot] && gStateWatchAddr.load() != 0) {
        dr7 |= (1ull << 4);           // local enable Dr2
        dr7 |= (1ull << 24);          // R/W = write
        dr7 |= (3ull << 26);          // LEN = 4 bytes (SessionState int)
    }
    return dr7;
}

void FillHwbpContext(CONTEXT* ctx, bool enable) {
    if (!ctx) return;
    // Read-modify-write: every slot we did not claim keeps whatever another feature put there.
    const DWORD64 mask = OwnedDr7Mask();
    if (enable) {
        if (gOwnSlot[0]) ctx->Dr0 = gHwbpAddr[0];
        if (gOwnSlot[1]) ctx->Dr1 = gHwbpAddr[1];
        if (gOwnSlot[kDrWriteSlot]) {
            ctx->Dr2 = gStateWatchAddr.load();
            gHwbpAddr[kDrWriteSlot] = ctx->Dr2;
        }
        if (gOwnSlot[3]) ctx->Dr3 = gHwbpAddr[3];
        ctx->Dr6 = 0;
        ctx->Dr7 = (ctx->Dr7 & ~mask) | BuildDr7Owned();
    } else {
        if (gOwnSlot[0]) ctx->Dr0 = 0;
        if (gOwnSlot[1]) ctx->Dr1 = 0;
        if (gOwnSlot[kDrWriteSlot]) ctx->Dr2 = 0;
        if (gOwnSlot[3]) ctx->Dr3 = 0;
        ctx->Dr6 = 0;
        ctx->Dr7 &= ~mask;
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
    // Only slots we claimed this run.
    int hit = -1;
    for (int i = 0; i < 4; ++i) {
        if (!gOwnSlot[i]) continue;
        if ((dr6 & (1ull << i)) && (gHwbpAddr[i] || (i == kDrWriteSlot && gStateWatchAddr.load()))) {
            hit = i;
            break;
        }
    }
    if (hit < 0) {
        for (int i = 0; i < 4; ++i) {
            if (!gOwnSlot[i] || i == kDrWriteSlot) continue;
            if (gHwbpAddr[i] && rip == gHwbpAddr[i]) {
                hit = i;
                break;
            }
        }
    }
    if (hit < 0) return EXCEPTION_CONTINUE_SEARCH;

    if (hit == kDrSendSlot && gSendProbe.load()) {
        // Entry of Session.Send(OutPacket): rcx = Session*, rdx = OutPacket*. The id alone is
        // enough to tell a UserMove apart from anything the client volunteers by itself, and a
        // line per packet stays readable at the client's natural rate.
        int op = -1;
        __try {
            auto* pkt = reinterpret_cast<const uint8_t*>(ctx->Rdx);
            if (pkt) op = *reinterpret_cast<const uint16_t*>(pkt + kOffOutPacketId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            op = -1;
        }
        if (op < 0) gSendUnreadable.fetch_add(1);
        gSendHits.fetch_add(1);
        if (op >= 0 && gDumpOpN) DumpSendBody(reinterpret_cast<const uint8_t*>(ctx->Rdx), op);
        if (const char* nm = OpName(op)) {
            SendLog("op=%d %s", op, nm);
        } else {
            SendLog("op=%d", op);
        }
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

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
    // Off by default: FillHwbpContext rewrites Dr0..Dr7 wholesale every 2s; keep call-edge DR
    // probes opt-in so other modules can share the register file. Session polling (STATE /
    // pendingError / RING) is unaffected.
    const char* how = nullptr;
    const HwbpWant want = ResolveHwbpWant(&how);
    if (want == HwbpWant::Off) {
        Log("CALL_EDGE_HWBP skip: set KICK_HWBP=1|2, or drop %ls / %ls", kHwbpMarkerName,
            kTeardownHwbpMarkerName);
        return;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallHwbpCallEdge), &gSelfMod);

    const bool teardown = (want == HwbpWant::Teardown);
    gHwbpTeardownMode.store(teardown);
    if (teardown) ApplyTeardownHwbpTargets();

    gHwbpAddr[0] = gGaBase + gHwbpTargets[0].rva;
    gHwbpAddr[1] = gGaBase + gHwbpTargets[1].rva;
    gHwbpAddr[kDrWriteSlot] = gStateWatchAddr.load();
    gHwbpAddr[3] = gGaBase + gHwbpTargets[3].rva;
    for (int i = 0; i < 4; ++i) gOwnSlot[i] = true;
    Log("CALL_EDGE_HWBP mode=%s via=%s", teardown ? "teardown" : "default", how ? how : "?");
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

// Outbound capture. Independent of the call-edge set: that one needs all four slots and is
// opt-in (KICK_HWBP=1); this one takes DR3 alone when KICK_SEND=1 (default off for production).
void InstallSendProbe();
MethodInfoHead* ResolveSendPacketMi(void* sessionKlass,
                                    x::runtime::il2cpp_method::ResolvePath* outPath = nullptr);
MethodInfoHead* ResolveSessionMi(void* klass, uintptr_t rva, int arity, const char* plain,
                                 const char* hash,
                                 x::runtime::il2cpp_method::ResolvePath* outPath = nullptr);

// 两种打开方式，任一命中即武装：
//   · `KICK_SEND=1` —— 启动器 CreateProcessW 传的是 nullptr 环境，客户端会继承，但整条链
//     （xcat → 启动器 → 客户端）必须在设好之后全部重启才生效，排查现场很容易踩空。
//   · 标记文件 `send_probe.on` —— 放在 kick.log 同目录（`ResolveLogDir()`）或 DLL 同目录，
//     重进游戏即生效，删文件即关。不必动系统环境变量，也不受启动顺序影响。
// 单独成函数是因为 InstallSendProbe 里有 __try，MSVC 不许同一函数内出现需展开的对象（C2712）。
bool SendProbeRequested(bool* outByEnv) {
    char env[8]{};
    const bool byEnv = GetEnvironmentVariableA("KICK_SEND", env, sizeof(env)) > 0 && env[0] == '1';
    if (outByEnv) *outByEnv = byEnv;
    const std::wstring logDir = ResolveLogDir();
    const std::wstring modDir = ModuleDir();
    const bool byFile = (!logDir.empty() && FileExists(logDir + L"\\" + kSendMarkerName)) ||
                        (!modDir.empty() && FileExists(modDir + L"\\" + kSendMarkerName));
    if (byEnv || byFile) return true;
    // 把实际探过的两个路径打出来：标记文件放错目录是最常见的「开了却没生效」。
    Log("SEND_PROBE skip: set KICK_SEND=1, or drop %ls to arm SendPacket DR → send.log",
        kSendMarkerName);
    Log("SEND_PROBE marker probed: %ls\\%ls | %ls\\%ls", logDir.c_str(), kSendMarkerName,
        modDir.c_str(), kSendMarkerName);
    return false;
}

void InstallSendProbe() {
    if (gSendProbe.load()) return;
    if (!gGaBase) {
        Log("SEND_PROBE skip: ga base 0");
        return;
    }
    // Default off: DR + VEH + send.log are diagnostic only; Session poll / 守护 disconnectSeq 不依赖。
    bool byEnv = false;
    if (!SendProbeRequested(&byEnv)) return;
    if (gOwnSlot[kDrSendSlot]) {
        Log("SEND_PROBE skip: DR%d already holds %s (KICK_HWBP=1)", kDrSendSlot,
            gHwbpTargets[kDrSendSlot].name);
        return;
    }
    OpenSendLog();
    if (gSendLogPath.empty()) {
        Log("SEND_PROBE skip: send.log open failed err=%lu", GetLastError());
        return;
    }

    // 47 是 UserMove——服端校验位移的真入口（见 docs/features/protocol/移动协议.md），排查
    // 「飞行被判位移外挂」时它是主证据，所以放在默认列表首位。50-53 是攻击族，用来对照攻击包
    // 自报的坐标。
    //
    // 拾取洪泛（`xcat_pet_loot.h` 两个硬上限 burst 8 / 盒 1500 就是为「拾取洪泛 → 静默掐线
    // verdict=lean_local_or_soft」这条实机事故设的）要看的是 **222 = DropPickUp 与 185 =
    // PetDropPickUp**，不是 207。207 按 CMS 同编号读会读成 MobDropPickUp，但那个前提在 >53 段
    // 已被证伪（见 OpName 注释），它实际是 MobMove——引擎作为 mob 控制端自发的，跟我方吸物无关。
    // 207 仍留在名单里只为拿包体确认这一点（小配额，见 DumpQuotaForOp），确认完即可删。
    // 该变量取逗号列表，填 "0" 表示只记 op 不抓体；上限 kDumpOpMax=8。
    char dumpEnv[64]{};
    if (GetEnvironmentVariableA("KICK_SEND_DUMP", dumpEnv, sizeof(dumpEnv)) == 0) {
        strcpy_s(dumpEnv, "47,50,51,52,53,222,185,207");
    }
    if (dumpEnv[0] != '0' || dumpEnv[1] != '\0') {
        for (const char* s = dumpEnv; *s && gDumpOpN < kDumpOpMax;) {
            while (*s == ' ' || *s == ',') ++s;
            if (!*s) break;
            const int v = atoi(s);
            if (v > 0) {
                gDumpOp[gDumpOpN] = v;
                gDumpLeft[gDumpOpN].store(DumpQuotaForOp(v));
                ++gDumpOpN;
            }
            while (*s && *s != ',') ++s;
        }
    }
    if (gDumpOpN) {
        char list[96];
        int ln = 0;
        for (int i = 0; i < gDumpOpN && ln + 12 < (int)sizeof(list); ++i) {
            ln += snprintf(list + ln, sizeof(list) - ln, i ? ",%d:%d" : "%d:%d", gDumpOp[i],
                           gDumpLeft[i].load());
        }
        Log("SEND_PROBE body dump op:quota=%s (KICK_SEND_DUMP=0 disables)", list);
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallSendProbe), &gSelfMod);

    // 优先 MethodInfo（hash / OutPacket kind）；失败再退 ga+RVA。
    if (!gSessionKlass) gSessionKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    x::runtime::il2cpp_method::ResolvePath sendPath =
        x::runtime::il2cpp_method::ResolvePath::Miss;
    MethodInfoHead* sendMi =
        gSessionKlass ? ResolveSendPacketMi(gSessionKlass, &sendPath) : nullptr;
    void* sendFn = nullptr;
    if (sendMi) {
        __try {
            sendFn = sendMi->virtualMethodPointer ? sendMi->virtualMethodPointer : sendMi->methodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sendFn = nullptr;
        }
    }
    gHwbpAddr[kDrSendSlot] = sendFn ? reinterpret_cast<uintptr_t>(sendFn) : (gGaBase + kRvaSessionSend);
    gOwnSlot[kDrSendSlot] = true;
    gSendProbe.store(true);
    Log("SEND_PROBE SendPacket method path=%s mi=%d",
        x::runtime::il2cpp_method::PathName(sendPath), sendMi ? 1 : 0);

    if (!gVehHandle) {
        gVehHandle = AddVectoredExceptionHandler(1, CallEdgeVeh);
        if (!gVehHandle) {
            Log("SEND_PROBE FAIL AddVectoredExceptionHandler err=%lu", GetLastError());
            gOwnSlot[kDrSendSlot] = false;
            gHwbpAddr[kDrSendSlot] = 0;
            gSendProbe.store(false);
            return;
        }
    }

    const int n = ApplyHwbpAllThreads(true);
    gHwbpInstalled.store(true);
    Log("SEND_PROBE armed DR%d NM.SendPacket ga+0x%llX -> %p threads=%d via=%s (outbound -> send.log)",
        kDrSendSlot, (unsigned long long)kRvaSessionSend, (void*)gHwbpAddr[kDrSendSlot], n,
        byEnv ? "KICK_SEND" : "marker");
    SendLog("---- send probe armed pid=%lu ga+0x%llX ----", GetCurrentProcessId(),
            (unsigned long long)kRvaSessionSend);
}

void UninstallHwbpCallEdge() {
    if (!gHwbpInstalled.exchange(false)) return;
    gStateWatchAddr.store(0);
    gStateWatchSession.store(nullptr);
    gSendProbe.store(false);
    ApplyHwbpAllThreads(false);
    if (gVehHandle) {
        RemoveVectoredExceptionHandler(gVehHandle);
        gVehHandle = nullptr;
    }
    for (int i = 0; i < 4; ++i) {
        gHwbpAddr[i] = 0;
        gOwnSlot[i] = false;  // after the disarm pass — that pass reads ownership
    }
    FlushSendLog();
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

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
    }
    if (mi && mi->methodPointer) return mi;
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            const char* nm = e.methodGetName(raw);
            if (nm && strcmp(nm, name) == 0) {
                mi = reinterpret_cast<MethodInfoHead*>(raw);
                if (mi && mi->methodPointer) return mi;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

MethodInfoHead* ResolveSessionMi(void* klass, uintptr_t rva, int arity, const char* plain,
                                 const char* hash,
                                 x::runtime::il2cpp_method::ResolvePath* outPath) {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    MethodShape shape{};
    shape.arity = arity;
    shape.ret = TypeKind::Void;
    shape.unique = true;
    shape.walkParents = false;
    if (arity == 1) shape.param[0] = TypeKind::Any;  // SessionState enum ≠ 纯 I32
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        klass, static_cast<uint32_t>(rva), shape, plain, hash);
    if (outPath) *outPath = mr.path;
    if (mr.method) return reinterpret_cast<MethodInfoHead*>(mr.method);
    return FindMethodByRva(klass, rva);
}

MethodInfoHead* ResolveSendPacketMi(void* sessionKlass,
                                    x::runtime::il2cpp_method::ResolvePath* outPath) {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    void* outKlass = x::runtime::il2cpp::FindClass("", "OutPacket");
    if (!outKlass) outKlass = x::runtime::il2cpp::FindClass("", kOutPacketClass);
    MethodShape kSend{};
    kSend.arity = 1;
    kSend.ret = TypeKind::Bool;
    kSend.unique = true;
    kSend.walkParents = true;
    kSend.param[0] = TypeKind::Ptr;
    if (outKlass) kSend.paramKlass[0] = outKlass;
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        sessionKlass, static_cast<uint32_t>(kRvaSessionSend), kSend, "SendPacket",
        kHashSendPacket);
    if (outPath) *outPath = mr.path;
    if (mr.method) return reinterpret_cast<MethodInfoHead*>(mr.method);
    return FindMethodByRva(sessionKlass, kRvaSessionSend);
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
    // Default off: MethodInfo swap is kick attribution only; 守护用 Session 轮询 disconnectSeq。
    char env[8]{};
    const bool byEnv =
        GetEnvironmentVariableA("KICK_CALL_EDGE", env, sizeof(env)) > 0 && env[0] == '1';
    const bool byFile = MarkerPresent(kCallEdgeMarkerName);
    if (!byEnv && !byFile) {
        Log("CALL_EDGE skip: set KICK_CALL_EDGE=1, or drop %ls", kCallEdgeMarkerName);
        return;
    }
    if (!gClassGetMethods) {
        Log("CALL_EDGE skip: il2cpp_class_get_methods missing");
        return;
    }
    if (!gSessionKlass) gSessionKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    // No RuntimeClassInit here (worker thread). MethodInfo walk/patch is data-plane;
    // class_init from worker caused user GC fatal (upload 6ee38d / CALL_EDGE 6/6 then die).
    Log("CALL_EDGE install Session=%p via=%s (MethodInfo swap, no .text)", gSessionKlass,
        byEnv ? "KICK_CALL_EDGE=1" : "marker kick_call_edge.on");

    int ok = 0;
    int hashHits = 0;
    for (HookSlot& h : gHooks) {
        h.klass = gSessionKlass;
        if (!h.klass) {
            Log("CALL_EDGE FAIL %s — klass null", h.name);
            continue;
        }
        x::runtime::il2cpp_method::ResolvePath path =
            x::runtime::il2cpp_method::ResolvePath::Miss;
        h.mi = ResolveSessionMi(h.klass, h.rva, h.arity, h.plainName, h.methodHash, &path);
        if (!h.mi) {
            Log("CALL_EDGE FAIL %s — MethodInfo RVA 0x%llX / hash miss", h.name,
                (unsigned long long)h.rva);
            continue;
        }
        if (path == x::runtime::il2cpp_method::ResolvePath::Hash) ++hashHits;
        if (!PatchMethodInfo(h.mi, h.hook, &h.orig)) {
            Log("CALL_EDGE FAIL %s — patch MethodInfo %p", h.name, (void*)h.mi);
            h.mi = nullptr;
            continue;
        }
        ++ok;
        Log("CALL_EDGE OK %s mi=%p orig=%p rva=0x%llX path=%s", h.name, (void*)h.mi, h.orig,
            (unsigned long long)h.rva, x::runtime::il2cpp_method::PathName(path));
    }
    gCallEdgeInstalled.store(ok > 0);
    const int total = (int)(sizeof(gHooks) / sizeof(gHooks[0]));
    Log("CALL_EDGE methods path=%s hits=%d/%d hash=%d",
        hashHits == total ? "meta" : (hashHits ? "meta-partial" : "rva/kind"), ok, total,
        hashHits);
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
void __fastcall HookNmOnDisc(void* self, const void* method) {
    LogCallEdge("Nm.OnDisconnect", self, true, false, -1, "MI");
    if (gHooks[2].orig) OrigAsVoid(gHooks[2])(self, method);
}
void __fastcall HookNmSetState(void* self, int state, const void* method) {
    if (state == kStateDisconnecting || state == kStateDisconnected)
        LogCallEdge("Nm.set_SessionState", self, true, false, state, "MI");
    auto* orig = reinterpret_cast<FnSetState>(gHooks[3].orig);
    if (orig) orig(self, state, method);
}

void DumpRecvList(void* session, int maxN) {
    void* list = ReadPtr(session, kOffSessionRecvList);
    if (!list) {
        Log("  recvList=null");
        return;
    }
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    const int size = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    Log("  recvList size=%d items=%p", size, items);
    if (!items || size <= 0) return;
    const int n = size < maxN ? size : maxN;
    for (int i = size - n; i < size; ++i) {
        if (i < 0) continue;
        void* pkt = ReadPtr(items, x::runtime::il2cpp_container::OffArrayData() +
                                       (size_t)i * sizeof(void*));
        if (!pkt) continue;
        const uint16_t op = ReadU16(pkt, kOffInPacketId);
        const char* hint = CmsServerPacketHint(op);
        void* bufObj = ReadPtr(pkt, kOffPacketBuffer);
        int blen =
            bufObj ? ReadI32(bufObj, x::runtime::il2cpp_container::OffArrayMaxLength()) : -1;
        unsigned b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        if (bufObj && blen >= 4) {
            __try {
                auto* d = reinterpret_cast<uint8_t*>(bufObj) +
                          x::runtime::il2cpp_container::OffArrayData();
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
        // Soft-relogin：先 RequestAttempt（同步 softLoginHold），再 bump disconnectSeq，
        // 避免宿主读到「seq+1 且 hold=0」立刻干净重拉。
        x::features::galaxy_token_probe::RequestSample(
            now == kStateDisconnecting ? "disconnecting" : "disconnected");
        x::features::soft_login_probe::RequestAttempt(
            now == kStateDisconnecting ? "disconnecting" : "disconnected");
        gDisconnectSeq.fetch_add(1, std::memory_order_relaxed);
        // Mark the drop inside the outbound trace too, so the last packets we sent before it
        // can be read off without cross-referencing timestamps against kick.log.
        SendLog("==== STATE %s(%d) pendingError=%d ====", StateName(now), now, err);
        FlushSendLog();
        Snapshot("disconnect");
    } else if (now == kStateConnected && prev != kStateConnected) {
        x::features::galaxy_token_probe::RequestSample("connected");
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
    if (!x::runtime::il2cpp::Ensure()) {
        Log("il2cpp_bind Ensure failed — kick_sniff idle");
        while (!gStop.load()) Sleep(200);
        Log("kick_sniff worker stop");
        return 1;
    }
    const auto& e = x::runtime::il2cpp::Get();
    ga = e.ga;
    gGaBase = x::runtime::il2cpp::GaBase();
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gClassGetMethods = e.classGetMethods;
    if (!gClassStaticData) Log("warn: il2cpp_class_get_static_field_data missing — probing klass offsets");

    gFacadeKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    gSessionKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    gStlKlass = gFacadeKlass;
    // Facade=Singleton 壳；Session=方法宿主。实例解析走 facade → Session*@0x10。
    Log("NetworkManager facade=%p Session=%p ga=%p ringCap=%d", gFacadeKlass, gSessionKlass,
        (void*)gGaBase, kRingCap);
    InstallCallEdgeHooks();
    InstallHwbpCallEdge();
    InstallSendProbe();  // after the call-edge set, so it only claims DR3 if that one passed

    // 卡登录期一次性取证：auto_enter 的世界列表在 60s 超时，取 90s 保证落在其后。
    constexpr DWORD kStallDumpAfterMs = 90000;
    bool stallDumped = false;
    DWORD lastHb = GetTickCount();
    DWORD lastHwbp = GetTickCount();
    DWORD lastSend = GetTickCount();
    uint32_t lastSendHits = 0;
    uint32_t lastSeqSend = 0;
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
                if (lastState >= 0) {
                    OnStateChange(lastState, st, err);
                } else {
                    gSessionAliveSince.store(GetTickCount());
                    Log("STATE initial %s(%d) pendingError=%d", StateName(st), st, err);
                }
                lastState = st;
                lastErr = err;
                gLastErr.store(err);
                gLastState.store(st);
            }
        } else if (lastState >= 0) {
            const int was = lastState;
            Log("STATE lost SessionTcpLayer (was %s)", StateName(was));
            // 清零前采样（与 DumpRing churn 同口径）；其后 store(0) 会抹掉 aliveSince。
            const DWORD nowLost = GetTickCount();
            const DWORD aliveSinceLost = gSessionAliveSince.load();
            const DWORD aliveMsLost = aliveSinceLost ? (nowLost - aliveSinceLost) : 0;
            const int kickHitsLost = CountKickHintsInRing(nowLost, 3000);
            DumpRing("lost_session");  // 读 gSessionAliveSince，故清零必须在其后
            gSessionAliveSince.store(0);
            lastState = -1;
            gNmCached = nullptr;
            gScanPrevN = 0;
            UpdateSessionStateWatch(nullptr);
            // BIN 15:20：安全强制关闭 → layer 瞬失、无 Disconnected → 需软路径。
            // BIN 15:49：冷启/进图秒级空窗 = soft churn，勿 RequestAttempt。
            // BIN 16:05：药店图内长会话 Session 空窗 aliveMs>60s 仍武装 → hold 闪一下像「强制软重连」。
            // 策略：图内一律不武装（安全踢回大厅靠 stuck_lobby）；仅 !inMap 且非短 churn 才 lost_session。
            constexpr DWORD kChurnMaxAliveMs = 60000;
            const bool churnLike =
                aliveSinceLost == 0 || aliveMsLost < kChurnMaxAliveMs;
            const bool inMapNow = x::features::ports::world::IsInMapScene();
            if ((was == kStateConnected || was == kStateConnecting ||
                 was == kStateDisconnecting) &&
                !inMapNow && !(kickHitsLost == 0 && churnLike)) {
                x::features::soft_login_probe::RequestAttempt("lost_session");
            }
        }
        const DWORD now = GetTickCount();
        // BIN 19:38：图内 SessionTcpLayer 持久丢失、无 Disconnected 边沿 → 怪 AbsPos 钉死。
        // lost_session 在 inMap 被吞；CALL_EDGE 默认关所以 CloseSession 粘性也不会武装。
        // 换图 blip 通常 150ms 内重绑；满 3s 仍无 facade 且不在换频途中才软重连。
        {
            static DWORD sNmGoneSince = 0;
            static bool sNmGoneFired = false;
            if (nm) {
                sNmGoneSince = 0;
                sNmGoneFired = false;
            } else {
                if (!sNmGoneSince) sNmGoneSince = now ? now : 1;
                constexpr DWORD kNmGonePersistMs = 3000;
                const bool inMap = x::features::ports::world::IsInMapScene();
                const bool hopBusy = x::features::channel_hop::HasPending();
                if (!sNmGoneFired && inMap && !hopBusy && now - sNmGoneSince >= kNmGonePersistMs) {
                    sNmGoneFired = true;
                    Log("nm_gone_inmap persist=%ums — RequestAttempt",
                        static_cast<unsigned>(now - sNmGoneSince));
                    x::features::soft_login_probe::RequestAttempt("nm_gone_inmap");
                }
            }
        }
        // 卡登录期取证：会话连上却迟迟没有进图流量时，主动倒一次环。
        // 不能只挂在拆除路径上 —— 实测 18 轮卡登录里只有 1 轮走到了 lost_session，
        // 其余进程是被硬结束的，worker 根本没机会观察到会话消失。
        // 判据用环饱和度而非 IsPlayReady()：后者会改 pump phase / map-transit-block，
        // 探针不该有副作用；而在图里入站环一直是打满的（历次 dump 均为 count=64），
        // 卡登录时 10 分钟才攒到十几条。
        {
            const DWORD aliveSince = gSessionAliveSince.load();
            if (aliveSince == 0) {
                stallDumped = false;
            } else if (!stallDumped && now - aliveSince >= kStallDumpAfterMs &&
                       gRingCount < kRingCap) {
                stallDumped = true;
                DumpRing("login_stall");
            }
        }
        if (gHwbpInstalled.load() && now - lastHwbp >= 2000) {
            lastHwbp = now;
            ApplyHwbpAllThreads(true);  // refresh DR on new threads
        }
        if (gSendProbe.load() && now - lastSend >= 2000) {
            const uint32_t hits = gSendHits.load();
            // _seqSend feeds EncodeSeqBase, so it is a cipher seed rather than a packet count:
            // it moves by a different amount every send. Only "did it move at all" is readable,
            // and that is enough to notice packets leaving by a path DR3 never sees.
            void* nmNow = gNmCached;
            void* sessNow = nmNow ? ReadPtr(nmNow, kOffNmSession) : nullptr;
            if (sessNow && !LooksLikeHeapPtr(sessNow)) sessNow = nullptr;
            const uint32_t seq = sessNow ? (uint32_t)ReadI32(sessNow, kOffSessionSeqSend) : 0;
            SendLog("-- %.1f/s hits=%u seqRaw=%u seqMoved=%d unreadable=%u --",
                    (hits - lastSendHits) * 1000.0 / (now - lastSend), hits - lastSendHits, seq,
                    seq != lastSeqSend ? 1 : 0, gSendUnreadable.load());
            FlushSendLog();
            lastSend = now;
            lastSendHits = hits;
            lastSeqSend = seq;
        }
        if (now - lastHb >= 10000) {
            lastHb = now;
            Log("heartbeat nm=%p state=%d err=%d sawDisc=%d ring=%d hwbp=%d send=%u", gNmCached,
                gLastState.load(), gLastErr.load(), gSawDisconnect.load() ? 1 : 0, gRingCount,
                gHwbpInstalled.load() ? 1 : 0, gSendHits.load());
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
    EnsureKickFieldOff();
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
bool HasResolvedSession() { return gNmCached != nullptr; }
uint32_t DisconnectSeq() { return gDisconnectSeq.load(std::memory_order_relaxed); }

}  // namespace kick_sniff
}  // namespace features
}  // namespace x
