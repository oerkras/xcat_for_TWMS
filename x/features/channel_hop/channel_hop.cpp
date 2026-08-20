// Classic TWMS — random channel hop: NO UI.
// Read WorldManager._channelID / _adultChannel.Count, then Field.SendTransferChannelRequest.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "channel_hop.h"

#include "../auto_enter/auto_enter.h"
#include "../auto_lie/auto_lie.h"
#include "../ccu/ccu.h"
#include "../encounter/encounter.h"
#include "../invuln/invuln.h"
#include "../kick_sniff/kick_sniff.h"
#include "../notify/notify.h"
#include "../ports/attack_input_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/mob_gather_port.h"
#include "../ports/teleport_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../travel/travel.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace x::features::channel_hop {
namespace {

using x::runtime::il2cpp::AtRva;
using x::runtime::il2cpp::FindClass;
using x::runtime::il2cpp::ReadPtr;

// Field.SendTransferChannelRequest(int) — remount 2026-08-04 dump.cs
constexpr uint32_t kRvaSendTransferChannelRequest = 0xBCD7E0;
// UserBase 短 IsAlertMode — CanPerformAction callee；读 LocalUser+0x118
constexpr uint32_t kRvaIsAlertMode = 0x124DC50;
// WorldManager.CanSendExclRequest — SendTransfer 发包前门控（BIN：未过则无 op=44）
constexpr uint32_t kRvaCanSendExclRequest = 0xDF8980;
constexpr int kExclTypeTransferChannel = 500;  // SendTransfer 传入的 type（常量解混淆）

constexpr char kFieldClass[] =
    "abb895b02ae65d2a1aa33910b7c654d1137ec50f28de45896061822e7745311";
// UserBase（短 IsAlertMode 宿主；非 UserLocal）
constexpr char kUserAlertClass[] =
    "a484ffac0ec2820f7d3cb62ddd233330e4c2613af7446a96b81d316db06bc44";
constexpr char kHashSendTransfer[] =
    "c6a800b39a433a273cfff7db8e992758cfc043f2fc057bf1865a56f5d311dd5";
constexpr char kHashIsAlertMode[] =
    "e757c4c413f82f697bee6ed0b780bbd3ef58512dbbec1f4f99ffd4cc759b1bf";
constexpr char kHashCanSendExcl[] =
    "dfd38249ceafcba4fdfb0a5ec33e43d27045e6fa44d4afee959f99c93f978d9";

// WorldManager 字段 Hint（docs + 08-04 dump 复核）；运行时 field_hash 覆盖
// SendTransferChannelRequest(int nIdx)：0-based；游戏 UI「ch.N」= nIdx+1。
// BIN：WM+0x68 换频后常卡死；+0x6C 更跟真实频；成功后再用 gKnownChannelIdx。
constexpr size_t kOffWmChannelIdHint = 0x68;
constexpr size_t kOffWmChannelAltHint = 0x6C;
constexpr size_t kOffWmAdultChannelHint = 0x78;
constexpr size_t kOffWmExclA0Hint = 0xA0;
constexpr size_t kOffWmExclA4Hint = 0xA4;
constexpr size_t kOffWmExclFlagA8Hint = 0xA8;  // CanSendExcl: [rdi+0A8h]
constexpr size_t kOffWmExclFlagA9Hint = 0xA9;
constexpr size_t kOffWmExclACHint = 0xAC;
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())

// WM 字段哈希（08-04 dump；backing 用内嵌 hash，strstr 匹配）
constexpr char kHashWmChannelId[] =
    "<b58795eccc13956bb4ded56c32c64ae90a654bc59ce192cf79b0330c7ebdf39>k__BackingField";
constexpr char kHashWmChannelAlt[] =
    "<fcd86899698d9aeacdf5f49df5392c0f00851ec7cb59f50dfc5b1f7fb938bbb>k__BackingField";
constexpr char kHashWmAdultChannel[] =
    "de689693724d5df8f7332c53284fd4959b96adc893e091d3a0f5ee46d34473c";
constexpr char kHashWmExclA0[] =
    "f73a7e4220891ae9bba9a4bdbb80132082f0b9f30887313fc5e5341d46fa842";
constexpr char kHashWmExclA4[] =
    "ce49f7df7cdeaf874359c6a99dcc9b8aed11e4841a83c8a71e9889ed6392f7f";
constexpr char kHashWmExclA8[] =
    "fb1b8b21ee9f55af591345f68ee7733de856b7a29f59d0fb702a0809f80f1a5";
constexpr char kHashWmExclA9[] =
    "cdc289634221a09c24eaae2df778209d35e24c3e7a0f104556079fc47e7979b";
constexpr char kHashWmExclAC[] =
    "e52423b38196522663224e03ea790471e7c0df66c4af48f38bab145bb703ccc";

size_t gOffWmChannelId = kOffWmChannelIdHint;
size_t gOffWmChannelAlt = kOffWmChannelAltHint;
size_t gOffWmAdultChannel = kOffWmAdultChannelHint;
size_t gOffWmExclA0 = kOffWmExclA0Hint;
size_t gOffWmExclA4 = kOffWmExclA4Hint;
size_t gOffWmExclFlagA8 = kOffWmExclFlagA8Hint;
size_t gOffWmExclFlagA9 = kOffWmExclFlagA9Hint;
size_t gOffWmExclAC = kOffWmExclACHint;
std::atomic<bool> gFieldOffResolved{false};
char gFieldOffPath[48]{};

constexpr int kAdultFlagCap = 128;

// KickSniff SessionState：Disconnecting=0 Disconnected=1 Connecting=2 Connected=3
constexpr int kSessDisconnecting = 0;
constexpr int kSessDisconnected = 1;
constexpr int kSessConnecting = 2;

constexpr DWORD kTickMs = 80;
constexpr DWORD kJobWaitMs = 2500;
constexpr DWORD kObserveWmIntervalMs = 2000;  // 官方 UI 换频：图内轻量读 WM 同步 sticky
constexpr DWORD kObserveWmJobMs = 400;       // 仅两字段，短超时；失败跳过不刷泵

constexpr DWORD kWaitMigrateMs = 12000;
constexpr DWORD kWaitAlertMs = 20000;       // 警戒解除最长等待
constexpr DWORD kWaitExclMs = 20000;        // excl 独占解除最长等待
constexpr DWORD kPreFireSettleMs = 800;     // 立马响应：4s→0.8s（BIN 曾 3s 仍踢；脏断风险自负）
constexpr DWORD kPostAlertGraceMs = 800;    // 与 PreFire 对齐，脱战后不另拖 1.5s
constexpr DWORD kFireIdleTimeoutMs = 2000;  // WaitFireIdle 上限（排空在途攻击键）
constexpr DWORD kFireIdleSettleMs = 400;    // 末次开火后再静默
// 必须与 PreFire 同窗。BIN a164e3：settle=4s 但 ForceCd 仍 6.5s → tpCdRem≈2.5s 拖满旧体感。
constexpr DWORD kTeleportForceCdMs = kPreFireSettleMs;
constexpr DWORD kAlertSampleMs = 200;
constexpr DWORD kExclSampleMs = 200;
constexpr DWORD kLandGraceMs = 4000;        // 进图后门控宽限，避免加载瞬间误发挂起 seq
constexpr DWORD kCooldownAfterOkMs = 0;    // 用户：遇人/手动换频不要成功冷却（脏断风险自负）
constexpr DWORD kCooldownAfterFailMs = 0;  // 同上：失败也不挡立刻再试
constexpr DWORD kPostHopQuietMs = 4000;    // 结算后暂缓恢复战斗（BIN：settle 后立刻 resume → 会话层仍在撕；无敌已不在 hop 中关闭）
constexpr DWORD kSettleReadyMs = 1500;     // 未离图时的最短观察
constexpr DWORD kStayConfirmMs = 1200;     // 从未离图且仍原频 → 才判拒绝
constexpr DWORD kPostLandGraceMs = 2500;   // 离图再落地后：等频道号追上
// BIN 0.1.109：Connecting/A8 后常仍 play≈1～2s，才闪 InterStage；提前 settle
//（migrate_seen_no_leave / channel_match）→ 黑屏窗无人等待、泵空闲卡住。
constexpr DWORD kPostFireLeaveExpectMs = 5500;  // 发包后等晚到离图的观察窗
constexpr DWORD kDisconnectingGraceMs = 3000;  // Disconnecting/Disconnected 短闪不立刻 Fail
constexpr DWORD kNoPacketBackoffMs = 1000;     // A8 未置位后同目标退避
constexpr DWORD kWaitNoPacketMs = 15000;       // 同目标 no-packet 总窗，防空转
constexpr int kNoPacketMaxStreak = 8;          // 同目标连续 A8=0 次数上限
constexpr int kChannelCountFallbackMax = 128;  // Classic 可达 ~60 频；原 40 会误杀
constexpr int kMaxFireAttempts = 5;            // 含首次；满人/未进则换其它频重试

using FnSendTransferChannel = void (*)(void* self, int channelId, const void* methodInfo);
using FnIsAlertMode = uint8_t (*)(void* self, const void* methodInfo);
// WM.CanSendExclRequest(int type, bool) — 调用约定：r8=bool、r9=MethodInfo（可空）
using FnCanSendExcl = uint8_t (*)(void* wm, int type, const void* a, const void* b);

struct MethodInfoHead {
    void* methodPointer = nullptr;
    void* virtualMethodPointer = nullptr;
};

MethodInfoHead* gMiSendTransfer = nullptr;
MethodInfoHead* gMiIsAlertMode = nullptr;
MethodInfoHead* gMiCanSendExcl = nullptr;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<uint32_t> gPendingSeq{0};
std::atomic<uint32_t> gActiveSeq{0};
std::atomic<unsigned> gState{static_cast<unsigned>(State::Idle)};

DWORD gPhaseAt = 0;
DWORD gCooldownUntil = 0;
DWORD gResumeAt = 0;  // 结算后延迟 Resume；0=无需/已恢复
int gTargetChannel = -1;
int gFromChannel = -1;
int gChannelCount = 0;
int gFireAttempt = 0;
int gTried[128]{};
int gTriedN = 0;
// 本图「落地仍挤 → 刚离开」的频道：选池 prefer 时优先避开；池空再放宽。
constexpr DWORD kSoftAvoidTtlMs = 5 * 60 * 1000;
DWORD gSoftAvoidAt[128]{};  // 0=未拉黑；非 0=标记时刻（GetTickCount）
int gSoftAvoidMapId = -1;
int gSoftAvoidLive = 0;  // 诊断：当前未过期条数（Note/Pick 时刷新）
DWORD gStaySince = 0;      // 从未离图且连续仍停原频
DWORD gLandedAt = 0;       // 离图后再进 PlayReady 的时刻；0=本轮未落地过
bool gSawLeavePlay = false;  // 发包后是否见过 !IsPlayReady（黑屏/InterStage）
bool gWasPlayReady = true;
int gKnownChannelIdx = -1;  // 上次成功换频后的 0-based 索引；跨 job 保留
bool gInvulnHeld = false;     // 本模块临时关了 Invuln（仅 BeginActive 后）
bool gInvulnWasOn = false;    // 关之前的 desired，Finish 时还原
std::atomic<bool> gCombatPaused{false};  // ChannelHop 硬闸；Request/Tick 跨线程
std::atomic<bool> gEarlyHoldFromRequest{false};  // Request 边沿已停刀，尚未 BeginActive
bool gSoftReloginHop = false;  // 遇人：sticky 新频 + CloseSession，禁止 SendTransfer
bool gHopFailRecoverPending = false;  // 换频失败仍在图内：停刀 + CloseSession 清脏
int gHopFailRecoverTries = 0;
DWORD gHopFailRecoverRetryAt = 0;
constexpr int kHopFailRecoverMaxTries = 5;
constexpr DWORD kHopFailRecoverRetryMs = 2500;
bool gAlertNotified = false;
DWORD gAlertSampleAt = 0;
bool gAlertCached = false;
std::atomic<DWORD> gFireReadyAt{0};  // Confirming：到此时才允许 FireTransfer（Request 可预武装）
bool gExclNotified = false;
DWORD gExclSampleAt = 0;
bool gExclCachedOk = true;
DWORD gPlayReadySince = 0;  // Idle 挂起时进图起点；0=未就绪
bool gWatchDisconnect = false;  // Waiting：盯 KickSniff 硬断（Connecting 是正常迁频）
DWORD gDisconnectingSince = 0;  // Disconnecting 起算；0=未处于该态
bool gSawConnecting = false;    // Waiting 内见过 Connecting（真迁频）
bool gExclArmed = false;        // 本轮发包后 A8 已置位（包大概率出去了）
DWORD gNoPacketSince = 0;       // 同目标首次 A8=0；0=尚未 no-packet
int gNoPacketStreak = 0;        // 同目标连续 A8=0 次数
int gLastRaw68 = -999;          // ReadInfo 前进检测；Login/Init 清
int gLastRaw6c = -999;
uint8_t gAdultFlag[kAdultFlagCap]{};  // WM List<bool> 快照；1=成人频
int gAdultFlagN = 0;
void* gKlassField = nullptr;
uint32_t gLastDeferNotifySeq = 0;

// UI / toast：0-based idx → 玩家看到的 ch.N
int DispCh(int idx) { return idx >= 0 ? idx + 1 : idx; }

bool IsEncounterHopSeq(uint32_t seq) { return seq >= kEncounterHopSeqBase; }

bool WantEncounterSoftHop() {
    return gSoftReloginHop || IsEncounterHopSeq(gActiveSeq.load());
}

int KnownDisp1Based() {
    if (gKnownChannelIdx < 0) return 0;
    return DispCh(gKnownChannelIdx);
}

// 把 known 推到 auto_enter sticky（遇人换频后 soft 必须粘新频，不能只靠进图 Done）。
void PushStickyFromKnown(const char* why) {
    // sticky 与 SelectChannel / WM+0x6C 同口径（0-based 列表 id）。BIN 08-15：id=39 时 UI 为 40。
    if (gKnownChannelIdx >= 1 && gKnownChannelIdx <= 64)
        auto_enter::NoteStickyChannel(gKnownChannelIdx, why);
}

struct JobCtx {
    enum class Kind : int {
        ReadInfo,
        FireTransfer,
        QueryAlert,
        QueryExcl,
        ObserveWm  // 轻量：只读 +0x6C/+0x68，供官方 UI 换频同步 sticky
    } kind = Kind::ReadInfo;
    bool ok = false;
    int channelId = 0;       // 0-based 当前（选频/排除用）
    int channelRaw68 = 0;    // WM+0x68 原值
    int channelRaw6c = 0;    // WM+0x6C 原值
    int channelCount = 0;
    int adultCount = 0;
    int ccuCount = 0;
    bool alert = false;
    bool exclOk = true;  // CanSendExcl(500) != 0
    int exclA0 = 0;
    int exclA4 = 0;
    uint8_t exclFlagA8 = 0;
    uint8_t exclFlagA9 = 0;
    int exclAC = 0;
    uint8_t exclFlagA8After = 0;
    const char* countSrc = "";
    const char* channelSrc = "";
    char err[96]{};
};

void SetState(State s) { gState.store(static_cast<unsigned>(s)); }
State GetStateLocal() { return static_cast<State>(gState.load()); }

void Log(const char* fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    x::runtime::LogI("ChannelHop", "%s", body);
}

void Notify(x::features::notify::NotificationKind kind, const char* key, const char* title,
            const char* body) {
    x::features::notify::PublishNotification(
        x::features::notify::NotificationEvent{kind, key, title, body, 4200});
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !rva) return nullptr;
    const auto& ex = x::runtime::il2cpp::Get();
    if (!ex.classGetMethods || !x::runtime::il2cpp::GaBase()) return nullptr;
    void* target = AtRva<void*>(rva);
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        for (;;) {
            void* miRaw = nullptr;
            __try {
                miRaw = ex.classGetMethods(cur, &iter);
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
        if (!ex.classParent) break;
        void* parent = nullptr;
        __try {
            parent = ex.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        MethodInfoHead* mi = nullptr;
        if (e.classGetMethodFromName) {
            const int tryArgc[] = {argc, -1};
            for (int ac : tryArgc) {
                __try {
                    mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(cur, name, ac));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    mi = nullptr;
                }
                if (mi && mi->methodPointer) return mi;
            }
        }
        if (e.classGetMethods && e.methodGetName) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* raw = e.classGetMethods(cur, &iter);
                    if (!raw) break;
                    const char* nm = e.methodGetName(raw);
                    if (nm && strcmp(nm, name) == 0) {
                        mi = reinterpret_cast<MethodInfoHead*>(raw);
                        if (mi && mi->methodPointer) return mi;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

uint32_t MiRva(MethodInfoHead* mi) {
    if (!mi || !mi->methodPointer) return 0;
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (!base) return 0;
    const auto a = reinterpret_cast<uintptr_t>(mi->methodPointer);
    if (a < base) return 0;
    const uint64_t d = static_cast<uint64_t>(a - base);
    return d > 0x7FFFFFFFull ? 0u : static_cast<uint32_t>(d);
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (outPath) *outPath = mr.path;
    if (!mr.method) return nullptr;
    auto* mi = reinterpret_cast<MethodInfoHead*>(mr.method);
    if (!mi->methodPointer) return nullptr;
    const uint32_t got = MiRva(mi);
    if (rva && got && got != rva) {
        Log("ResolveMi reject path=%d wantRva=0x%X got=0x%X plain=%s", (int)mr.path, rva, got,
            plain ? plain : "-");
        if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
        return nullptr;
    }
    return mi;
}

size_t FieldOffsetByHash(void* klass, const char* nameHash) {
    if (!klass || !nameHash || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    for (void* k = klass; k; ) {
        if (e.classGetFieldFromName && e.fieldGetOffset) {
            void* field = nullptr;
            __try {
                field = e.classGetFieldFromName(k, nameHash);
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
                if (off) return off;
            }
        }
        if (e.classGetFields && e.fieldGetName && e.fieldGetOffset) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* field = e.classGetFields(k, &iter);
                    if (!field) break;
                    const char* nm = nullptr;
                    __try {
                        nm = e.fieldGetName(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        nm = nullptr;
                    }
                    if (!nm) continue;
                    if (strcmp(nm, nameHash) != 0 && !strstr(nm, nameHash)) continue;
                    size_t off = 0;
                    __try {
                        off = e.fieldGetOffset(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        off = 0;
                    }
                    if (off) return off;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return 0;
}

size_t PickOff(size_t resolved, size_t hint, bool* usedHash) {
    if (resolved) {
        if (usedHash) *usedHash = true;
        return resolved;
    }
    return hint;
}

void EnsureFieldOffsets() {
    if (gFieldOffResolved.load(std::memory_order_acquire)) return;
    void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!wmKlass || !x::runtime::il2cpp::Ensure()) return;

    bool chId = false, chAlt = false, adult = false;
    bool eA0 = false, eA4 = false, eA8 = false, eA9 = false, eAC = false;
    gOffWmChannelId =
        PickOff(FieldOffsetByHash(wmKlass, kHashWmChannelId), kOffWmChannelIdHint, &chId);
    gOffWmChannelAlt =
        PickOff(FieldOffsetByHash(wmKlass, kHashWmChannelAlt), kOffWmChannelAltHint, &chAlt);
    gOffWmAdultChannel =
        PickOff(FieldOffsetByHash(wmKlass, kHashWmAdultChannel), kOffWmAdultChannelHint, &adult);
    gOffWmExclA0 = PickOff(FieldOffsetByHash(wmKlass, kHashWmExclA0), kOffWmExclA0Hint, &eA0);
    gOffWmExclA4 = PickOff(FieldOffsetByHash(wmKlass, kHashWmExclA4), kOffWmExclA4Hint, &eA4);
    gOffWmExclFlagA8 =
        PickOff(FieldOffsetByHash(wmKlass, kHashWmExclA8), kOffWmExclFlagA8Hint, &eA8);
    gOffWmExclFlagA9 =
        PickOff(FieldOffsetByHash(wmKlass, kHashWmExclA9), kOffWmExclFlagA9Hint, &eA9);
    gOffWmExclAC = PickOff(FieldOffsetByHash(wmKlass, kHashWmExclAC), kOffWmExclACHint, &eAC);

    const int hits = (chId ? 1 : 0) + (chAlt ? 1 : 0) + (adult ? 1 : 0) + (eA0 ? 1 : 0) +
                     (eA4 ? 1 : 0) + (eA8 ? 1 : 0) + (eA9 ? 1 : 0) + (eAC ? 1 : 0);
    const bool chOk = chId && chAlt;
    const bool exclOk = eA0 && eA4 && eA8 && eA9 && eAC;
    snprintf(gFieldOffPath, sizeof(gFieldOffPath), "%s hits=%d/8 ch=%s adult=%s excl=%s",
             hits == 8 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
             chOk ? "hash" : "hint", adult ? "hash" : "hint", exclOk ? "hash" : "hint");
    gFieldOffResolved.store(true, std::memory_order_release);
    Log("WM field off path=%s ch=0x%zX/0x%zX adult=0x%zX exclA8=0x%zX", gFieldOffPath,
        gOffWmChannelId, gOffWmChannelAlt, gOffWmAdultChannel, gOffWmExclFlagA8);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

void EnsureMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    EnsureFieldOffsets();
    if (!gKlassField) {
        gKlassField = FindClass("", kFieldClass);
        if (!gKlassField) gKlassField = FindClass("", "Field");
        if (!gKlassField) gKlassField = FindClass("Msc.Scene", "SceneField");
    }
    void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    void* userAlert = FindClass("", kUserAlertClass);
    if (!userAlert) userAlert = x::runtime::il2cpp_shape::ResolveUserLocalKlass();

    using x::runtime::il2cpp_method::ResolvePath;
    int methodHashHits = 0;
    auto noteHash = [&](ResolvePath path) {
        if (path == ResolvePath::Hash) ++methodHashHits;
    };
    ResolvePath pTr = ResolvePath::Miss, pAl = ResolvePath::Miss, pEx = ResolvePath::Miss;

    if (gKlassField && !gMiSendTransfer) {
        constexpr MethodShape kTr{1, TypeKind::Void, true, true, {TypeKind::I32}};
        gMiSendTransfer = ResolveMi(gKlassField, kRvaSendTransferChannelRequest, kTr,
                                    "SendTransferChannelRequest", kHashSendTransfer, &pTr);
        noteHash(pTr);
    }
    if (userAlert && !gMiIsAlertMode) {
        // bool() 不唯一 → 哈希主；walkParents 覆盖 UserLocal→UserBase
        constexpr MethodShape kAl{0, TypeKind::Bool, false, true, {}};
        gMiIsAlertMode =
            ResolveMi(userAlert, kRvaIsAlertMode, kAl, "IsAlertMode", kHashIsAlertMode, &pAl);
        noteHash(pAl);
    }
    if (wmKlass && !gMiCanSendExcl) {
        constexpr MethodShape kEx{2, TypeKind::Bool, true, true, {TypeKind::I32, TypeKind::Any}};
        gMiCanSendExcl = ResolveMi(wmKlass, kRvaCanSendExclRequest, kEx, "CanSendExclRequest",
                                   kHashCanSendExcl, &pEx);
        noteHash(pEx);
    }
    const int n = (gMiSendTransfer ? 1 : 0) + (gMiIsAlertMode ? 1 : 0) + (gMiCanSendExcl ? 1 : 0);
    static bool sMethodHitsLogged = false;
    if (!sMethodHitsLogged && n > 0) {
        sMethodHitsLogged = true;
        Log("methods path=%s hits=%d/3",
            methodHashHits == 3 ? "meta" : (methodHashHits ? "meta-partial" : "fallback"),
            methodHashHits);
    }
    char detail[48]{};
    snprintf(detail, sizeof(detail), "mi %d/3", n);
    x::runtime::anchor_lamps::Set(
        "ChanHop",
        n == 3   ? x::runtime::anchor_lamps::AnchorLampCode::Ok
        : n > 0  ? x::runtime::anchor_lamps::AnchorLampCode::Degraded
                 : x::runtime::anchor_lamps::AnchorLampCode::Miss,
        detail);
}

int ReadI32(void* base, size_t off) {
    if (!base) return 0;
    int v = 0;
    __try {
        v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return v;
}

uint8_t ReadU8(void* base, size_t off) {
    if (!base) return 0;
    uint8_t v = 0;
    __try {
        v = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return v;
}

void SnapshotExcl(void* wm, JobCtx* job) {
    if (!wm || !job) return;
    job->exclA0 = ReadI32(wm, gOffWmExclA0);
    job->exclA4 = ReadI32(wm, gOffWmExclA4);
    job->exclFlagA8 = ReadU8(wm, gOffWmExclFlagA8);
    job->exclFlagA9 = ReadU8(wm, gOffWmExclFlagA9);
    job->exclAC = ReadI32(wm, gOffWmExclAC);
}

int ReadListCount(void* listObj) {
    if (!listObj) return 0;
    return ReadI32(listObj, kOffListSize);
}

// List<bool>：IL2CPP 存成 byte[]（非 BitArray）
void SnapshotAdultFlags(void* wm, int count) {
    gAdultFlagN = 0;
    memset(gAdultFlag, 0, sizeof(gAdultFlag));
    if (!wm || count <= 0) return;
    void* adult = ReadPtr(wm, gOffWmAdultChannel);
    if (!adult) return;
    const int n = ReadListCount(adult);
    void* items = ReadPtr(adult, kOffListItems);
    if (!items || n <= 0) return;
    const int take = n < count ? n : count;
    const int cap = take < kAdultFlagCap ? take : kAdultFlagCap;
    __try {
        const uint8_t* data =
            reinterpret_cast<const uint8_t*>(items) + kOffArrData;
        for (int i = 0; i < cap; ++i) {
            gAdultFlag[i] = data[i] ? 1 : 0;
        }
        gAdultFlagN = cap;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gAdultFlagN = 0;
    }
}

bool IsAdultIdx(int idx) {
    if (idx < 0 || idx >= gAdultFlagN) return false;
    return gAdultFlag[idx] != 0;
}

void ResolveChannelCount(void* wm, JobCtx* job) {
    void* adult = ReadPtr(wm, gOffWmAdultChannel);
    const int fromList = ReadListCount(adult);
    const auto ccu = ccu::GetCcuStatus();
    const int fromCcu = ccu.worldChannelCount;
    job->adultCount = fromList;
    job->ccuCount = fromCcu;

    if (fromList > 0 && fromList <= kChannelCountFallbackMax) {
        job->channelCount = fromList;
        job->countSrc = "adult";
        return;
    }
    if (fromCcu > 0 && fromCcu <= kChannelCountFallbackMax) {
        job->channelCount = fromCcu;
        job->countSrc = "ccu";
        return;
    }
    // 超 cap 仍尽量用成人列表/CCU（只防离谱脏值）
    if (fromList > 0 && fromList <= 512) {
        job->channelCount = fromList;
        job->countSrc = "adult_hi";
        return;
    }
    if (fromCcu > 0 && fromCcu <= 512) {
        job->channelCount = fromCcu;
        job->countSrc = "ccu_hi";
        return;
    }
    job->channelCount = 0;
    job->countSrc = "none";
}

void JobFn(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("channel_hop.Job");
    auto* job = reinterpret_cast<JobCtx*>(user);
    if (!job) return;
    job->ok = false;
    job->err[0] = 0;
    if (!x::runtime::il2cpp::Ensure()) {
        strncpy_s(job->err, "il2cpp", _TRUNCATE);
        return;
    }

    switch (job->kind) {
    case JobCtx::Kind::ReadInfo: {
        void* wm = ports::world::GetWorldManager();
        if (!wm) {
            strncpy_s(job->err, "no WorldManager", _TRUNCATE);
            return;
        }
        ResolveChannelCount(wm, job);
        if (job->channelCount <= 1) {
            snprintf(job->err, sizeof(job->err), "channelCount<=1 adult=%d ccu=%d",
                     job->adultCount, job->ccuCount);
            return;
        }
        job->channelRaw68 = ReadI32(wm, gOffWmChannelId);
        job->channelRaw6c = ReadI32(wm, gOffWmChannelAlt);
        const int count = job->channelCount;
        auto inRange = [count](int v) { return v >= 0 && v < count; };

        // 选频真源优先级（BIN 2026-08-01）：
        // 1) +0x6C 相对上次读数前进 → 最跟真实频
        // 2) +0x68 前进 → 字段恢复自更新
        // 3) gKnownChannelIdx（软/硬成功缓存）
        // 4) 冷启动：优先 6c，再 68
        if (inRange(job->channelRaw6c) && gLastRaw6c != -999 &&
            job->channelRaw6c != gLastRaw6c) {
            Log("wm6c advanced %d→%d known=%d → trust wm6c", gLastRaw6c, job->channelRaw6c,
                gKnownChannelIdx);
            job->channelId = job->channelRaw6c;
            job->channelSrc = "wm6c_adv";
            gKnownChannelIdx = job->channelRaw6c;
        } else if (inRange(job->channelRaw68) && gLastRaw68 != -999 &&
                   job->channelRaw68 != gLastRaw68) {
            Log("wm68 advanced %d→%d known=%d → trust wm68", gLastRaw68, job->channelRaw68,
                gKnownChannelIdx);
            job->channelId = job->channelRaw68;
            job->channelSrc = "wm68_adv";
            gKnownChannelIdx = job->channelRaw68;
        } else if (gKnownChannelIdx >= 0 && gKnownChannelIdx < count) {
            job->channelId = gKnownChannelIdx;
            job->channelSrc = "known";
        } else if (inRange(job->channelRaw6c)) {
            job->channelId = job->channelRaw6c;
            job->channelSrc = "wm6c";
        } else if (inRange(job->channelRaw68)) {
            job->channelId = job->channelRaw68;
            job->channelSrc = "wm68";
        } else {
            job->channelId = 0;
            job->channelSrc = "fallback0";
            Log("warn raw68=%d raw6c=%d count=%d known=%d — use idx0", job->channelRaw68,
                job->channelRaw6c, count, gKnownChannelIdx);
        }
        gLastRaw68 = job->channelRaw68;
        gLastRaw6c = job->channelRaw6c;
        SnapshotAdultFlags(wm, count);
        job->ok = true;
        break;
    }
    case JobCtx::Kind::ObserveWm: {
        // 官方 UIChannelShift 等绕过本状态机：只读两字段，不扫成人表/CCU。
        void* wm = ports::world::GetWorldManager();
        if (!wm) {
            strncpy_s(job->err, "no WorldManager", _TRUNCATE);
            return;
        }
        job->channelRaw68 = ReadI32(wm, gOffWmChannelId);
        job->channelRaw6c = ReadI32(wm, gOffWmChannelAlt);
        constexpr int kMaxCh = 64;
        auto inRange = [](int v) { return v >= 0 && v < kMaxCh; };
        if (inRange(job->channelRaw6c) && gLastRaw6c != -999 &&
            job->channelRaw6c != gLastRaw6c) {
            job->channelId = job->channelRaw6c;
            job->channelSrc = "wm6c_adv";
        } else if (inRange(job->channelRaw68) && gLastRaw68 != -999 &&
                   job->channelRaw68 != gLastRaw68) {
            job->channelId = job->channelRaw68;
            job->channelSrc = "wm68_adv";
        } else if (gKnownChannelIdx >= 0 && gKnownChannelIdx < kMaxCh) {
            job->channelId = gKnownChannelIdx;
            job->channelSrc = "known";
        } else if (inRange(job->channelRaw6c)) {
            job->channelId = job->channelRaw6c;
            job->channelSrc = "wm6c";
        } else if (inRange(job->channelRaw68)) {
            job->channelId = job->channelRaw68;
            job->channelSrc = "wm68";
        } else {
            strncpy_s(job->err, "observe raw OOR", _TRUNCATE);
            return;
        }
        gLastRaw68 = job->channelRaw68;
        gLastRaw6c = job->channelRaw6c;
        job->ok = true;
        break;
    }
    case JobCtx::Kind::QueryAlert: {
        job->alert = false;
        ports::player_combat::CombatCtx ctx{};
        if (!ports::player_combat::QueryCombatCtx(ctx) || !ctx.localUser) {
            // 无角色体时不当警戒，避免永久卡住
            job->ok = true;
            break;
        }
        EnsureMethodInfos();
        auto fn = FnFromMi<FnIsAlertMode>(gMiIsAlertMode, kRvaIsAlertMode);
        if (!fn) {
            strncpy_s(job->err, "IsAlertMode RVA", _TRUNCATE);
            return;
        }
        __try {
            job->alert = fn(ctx.localUser, gMiIsAlertMode) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strncpy_s(job->err, "IsAlertMode SEH", _TRUNCATE);
            return;
        }
        job->ok = true;
        break;
    }
    case JobCtx::Kind::QueryExcl: {
        void* wm = ports::world::GetWorldManager();
        if (!wm) {
            strncpy_s(job->err, "no WorldManager", _TRUNCATE);
            return;
        }
        SnapshotExcl(wm, job);
        EnsureMethodInfos();
        auto fn = FnFromMi<FnCanSendExcl>(gMiCanSendExcl, kRvaCanSendExclRequest);
        if (!fn) {
            // RVA 缺失时只靠 +0xA8：非 0 视为 busy
            job->exclOk = (job->exclFlagA8 == 0);
            job->ok = true;
            break;
        }
        __try {
            job->exclOk = fn(wm, kExclTypeTransferChannel, nullptr, gMiCanSendExcl) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strncpy_s(job->err, "CanSendExcl SEH", _TRUNCATE);
            return;
        }
        job->ok = true;
        break;
    }
    case JobCtx::Kind::FireTransfer: {
        // WaitFireIdle 可堵 2s，其间赶路可能已开工——发包前再挡一次。
        if (x::features::travel::IsActive()) {
            strncpy_s(job->err, "travel active", _TRUNCATE);
            return;
        }
        if (WantEncounterSoftHop()) {
            strncpy_s(job->err, "encounter_soft_hop no SendTransfer", _TRUNCATE);
            return;
        }
        void* field = ports::world::GetMapScene();
        if (!field) {
            strncpy_s(job->err, "no Field", _TRUNCATE);
            return;
        }
        void* wm = ports::world::GetWorldManager();
        SnapshotExcl(wm, job);
        EnsureMethodInfos();
        auto exclFn = FnFromMi<FnCanSendExcl>(gMiCanSendExcl, kRvaCanSendExclRequest);
        if (exclFn && wm) {
            __try {
                job->exclOk = exclFn(wm, kExclTypeTransferChannel, nullptr, gMiCanSendExcl) != 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                strncpy_s(job->err, "CanSendExcl SEH", _TRUNCATE);
                return;
            }
        } else if (wm) {
            job->exclOk = (job->exclFlagA8 == 0);
        } else {
            job->exclOk = true;  // 无 WM 仍尝试发包（后续可能另报错）
        }
        if (!job->exclOk) {
            strncpy_s(job->err, "excl busy", _TRUNCATE);
            return;
        }
        if (!gKlassField) {
            const auto& ex = x::runtime::il2cpp::Get();
            if (ex.objectGetClass) {
                __try {
                    gKlassField = ex.objectGetClass(field);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    gKlassField = nullptr;
                }
            }
            if (!gKlassField) {
                gKlassField = FindClass("", kFieldClass);
                if (!gKlassField) gKlassField = FindClass("", "Field");
                if (!gKlassField) gKlassField = FindClass("Msc.Scene", "SceneField");
            }
            EnsureMethodInfos();
        }
        auto* mi = gMiSendTransfer;
        if (!mi && gKlassField) mi = FindMethodByRva(gKlassField, kRvaSendTransferChannelRequest);
        auto fn = FnFromMi<FnSendTransferChannel>(mi ? mi : gMiSendTransfer,
                                                  kRvaSendTransferChannelRequest);
        if (!fn) {
            strncpy_s(job->err, "SendTransfer RVA", _TRUNCATE);
            return;
        }
        __try {
            fn(field, job->channelId, mi ? mi : gMiSendTransfer);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strncpy_s(job->err, "SendTransfer SEH", _TRUNCATE);
            return;
        }
        if (wm) job->exclFlagA8After = ReadU8(wm, gOffWmExclFlagA8);
        job->ok = true;
        break;
    }
    }
}

bool RunJob(JobCtx& job) {
    if (!x::runtime::main_thread::Ensure()) {
        strncpy_s(job.err, "main_thread", _TRUNCATE);
        return false;
    }
    // High：InterStage quiesce 仍可入队（读频道/警戒/发包）；Normal 会被泵拒。
    if (!x::runtime::main_thread::InvokeAndWait(&JobFn, &job, kJobWaitMs,
                                               x::runtime::main_thread::JobPrio::High)) {
        strncpy_s(job.err, "job timeout", _TRUNCATE);
        return false;
    }
    return job.ok;
}

bool RunObserveJob(JobCtx& job) {
    if (!x::runtime::main_thread::Ensure()) {
        strncpy_s(job.err, "main_thread", _TRUNCATE);
        return false;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&JobFn, &job, kObserveWmJobMs,
                                               x::runtime::main_thread::JobPrio::High)) {
        strncpy_s(job.err, "job timeout", _TRUNCATE);
        return false;
    }
    return job.ok;
}

// Idle+playReady：观测 WM 频变（官方 UI 换频），写 known + sticky。失败跳过。
void MaybeObserveNativeChannel(DWORD now) {
    if (GetStateLocal() != State::Idle) return;
    if (!ports::world::IsPlayReady()) return;
    // soft 重进 / 落地静默 / deferred：WM+0x6C 会抖，勿当官方换频推 sticky。
    if (soft_login_probe::IsReconnectInFlight()) return;
    static DWORD sLastObserveMs = 0;
    if (sLastObserveMs && now - sLastObserveMs < kObserveWmIntervalMs) return;
    sLastObserveMs = now;

    JobCtx job{};
    job.kind = JobCtx::Kind::ObserveWm;
    if (!RunObserveJob(job)) return;  // 超时/失败：不重试刷泵

    if (job.channelId < 0) return;
    const int stickyApi = auto_enter::StickyChannel1Based();
    // BIN 08-15：sticky/raw6c=39，玩家 UI=40。两者都是 0-based 列表 id，不是 ch.N。
    if (stickyApi > 0 && job.channelId == stickyApi) {
        gKnownChannelIdx = job.channelId;
        Log("native_wm keep sticky id=%d ui=%d (src=%s raw6c=%d raw68=%d)", stickyApi,
            DispCh(stickyApi), job.channelSrc ? job.channelSrc : "?", job.channelRaw6c,
            job.channelRaw68);
        return;
    }
    if (gKnownChannelIdx == job.channelId) {
        PushStickyFromKnown("native_wm");
        return;
    }
    if (stickyApi <= 0) {
        if (job.channelId >= 1 && job.channelId <= 64) {
            gKnownChannelIdx = job.channelId;
            Log("native_wm cold id=%d ui=%d (src=%s raw6c=%d raw68=%d)", job.channelId,
                DispCh(job.channelId), job.channelSrc ? job.channelSrc : "?", job.channelRaw6c,
                job.channelRaw68);
            auto_enter::NoteStickyChannel(job.channelId, "native_wm_cold");
        }
        return;
    }
    const int from = gKnownChannelIdx;
    gKnownChannelIdx = job.channelId;
    Log("native_wm id %d→%d ui %d→%d (src=%s raw6c=%d raw68=%d) — sticky", from, job.channelId,
        DispCh(from), DispCh(job.channelId), job.channelSrc ? job.channelSrc : "?", job.channelRaw6c,
        job.channelRaw68);
    if (job.channelId >= 1 && job.channelId <= 64)
        auto_enter::NoteStickyChannel(job.channelId, "native_wm");
}

bool SoftAvoidActive(int id, DWORD now) {
    if (id < 0 || id >= 128) return false;
    if (gSoftAvoidAt[id] == 0) return false;
    return (now - gSoftAvoidAt[id]) < kSoftAvoidTtlMs;
}

int CountSoftAvoidLive(DWORD now) {
    int n = 0;
    for (int i = 0; i < 128; ++i) {
        if (SoftAvoidActive(i, now)) ++n;
    }
    gSoftAvoidLive = n;
    return n;
}

void ClearSoftAvoid(const char* why) {
    const int hadMap = gSoftAvoidMapId;
    const int hadLive = gSoftAvoidLive;
    std::memset(gSoftAvoidAt, 0, sizeof(gSoftAvoidAt));
    gSoftAvoidLive = 0;
    gSoftAvoidMapId = -1;
    if (hadMap >= 0 || hadLive > 0) {
        Log("soft-avoid clear map=%d liveWas=%d why=%s", hadMap, hadLive, why ? why : "?");
    }
}

void EnsureSoftAvoidMap(int mapId) {
    if (mapId <= 0) return;
    if (gSoftAvoidMapId == mapId) return;
    // 会话级（map=0）升格为真图号：保留已有 mark，勿静默清空
    if (gSoftAvoidMapId == 0) {
        Log("soft-avoid map adopt 0→%d (keep marks live=%d)", mapId, gSoftAvoidLive);
        gSoftAvoidMapId = mapId;
        return;
    }
    if (gSoftAvoidMapId > 0) {
        Log("soft-avoid map change %d→%d — flush", gSoftAvoidMapId, mapId);
    }
    std::memset(gSoftAvoidAt, 0, sizeof(gSoftAvoidAt));
    gSoftAvoidLive = 0;
    gSoftAvoidMapId = mapId;
}

bool TryRefreshKnownIdx(const char* why) {
    if (gKnownChannelIdx >= 0 && gKnownChannelIdx < 128) return true;
    JobCtx job{};
    job.kind = JobCtx::Kind::ObserveWm;
    if (!RunObserveJob(job) || !job.ok || job.channelId < 0 || job.channelId >= 128) {
        Log("soft-avoid known refresh fail why=%s err=%s", why ? why : "?",
            job.err[0] ? job.err : "unset");
        return false;
    }
    gKnownChannelIdx = job.channelId;
    Log("soft-avoid known refresh src=%s idx=%d ch=%d why=%s",
        job.channelSrc ? job.channelSrc : "?", job.channelId, DispCh(job.channelId),
        why ? why : "?");
    return true;
}

int PickRandomChannel(int currentIdx, int count) {
    if (count <= 1) return -1;
    const DWORD now = GetTickCount();
    EnsureSoftAvoidMap(ports::world::GetMapId());
    const int softLive = CountSoftAvoidLive(now);

    int preferClean[128]{};
    int preferSoft[128]{};
    int neutralClean[128]{};
    int neutralSoft[128]{};
    int avoid[128]{};
    int nPreferClean = 0, nPreferSoft = 0, nNeutralClean = 0, nNeutralSoft = 0, nAvoid = 0;
    auto excluded = [&](int id) {
        if (id == currentIdx) return true;
        for (int i = 0; i < gTriedN; ++i) {
            if (gTried[i] == id) return true;
        }
        return false;
    };
    // 0-based：合法 nIdx ∈ [0, count)
    for (int id = 0; id < count && id < 128; ++id) {
        if (excluded(id)) continue;
        // 直播成人标记硬降级；登录填表再分 Prefer/Avoid
        ccu::ChannelPickHint hint = ccu::GetChannelPickHint(id);
        if (IsAdultIdx(id) && hint == ccu::ChannelPickHint::Prefer) {
            hint = ccu::ChannelPickHint::Avoid;
        } else if (IsAdultIdx(id) && hint == ccu::ChannelPickHint::Neutral) {
            hint = ccu::ChannelPickHint::Avoid;
        }
        const bool soft = SoftAvoidActive(id, now);
        if (hint == ccu::ChannelPickHint::Prefer) {
            if (soft)
                preferSoft[nPreferSoft++] = id;
            else
                preferClean[nPreferClean++] = id;
        } else if (hint == ccu::ChannelPickHint::Avoid) {
            avoid[nAvoid++] = id;
        } else if (soft) {
            neutralSoft[nNeutralSoft++] = id;
        } else {
            neutralClean[nNeutralClean++] = id;
        }
    }
    const int* pool = nullptr;
    int n = 0;
    const char* poolName = "none";
    // prefer → prefer_soft → neutral → neutral_soft → avoid
    if (nPreferClean > 0) {
        pool = preferClean;
        n = nPreferClean;
        poolName = "prefer";
    } else if (nPreferSoft > 0) {
        pool = preferSoft;
        n = nPreferSoft;
        poolName = "prefer_soft";
    } else if (nNeutralClean > 0) {
        pool = neutralClean;
        n = nNeutralClean;
        poolName = "neutral";
    } else if (nNeutralSoft > 0) {
        pool = neutralSoft;
        n = nNeutralSoft;
        poolName = "neutral_soft";
    } else if (nAvoid > 0) {
        pool = avoid;
        n = nAvoid;
        poolName = "avoid";
    }
    if (n <= 0 || !pool) return -1;
    const uint32_t mix =
        now ^ (gActiveSeq.load() * 2654435761u) ^ (static_cast<uint32_t>(gFireAttempt) * 97u);
    const int picked = pool[mix % static_cast<uint32_t>(n)];
    Log("pick-pool %s size=%d (prefer=%d soft=%d softLive=%d neutral=%d nSoft=%d avoid=%d "
        "adultSnap=%d) → idx=%d ch=%d",
        poolName, n, nPreferClean, nPreferSoft, softLive, nNeutralClean, nNeutralSoft, nAvoid,
        gAdultFlagN, picked, DispCh(picked));
    return picked;
}

void MarkTried(int channelId) {
    if (channelId < 0) return;
    for (int i = 0; i < gTriedN; ++i) {
        if (gTried[i] == channelId) return;
    }
    if (gTriedN < 128) gTried[gTriedN++] = channelId;
}

void ClearAttemptState() {
    gTargetChannel = -1;
    gFromChannel = -1;
    gChannelCount = 0;
    gFireAttempt = 0;
    gTriedN = 0;
    gStaySince = 0;
    gLandedAt = 0;
    gSawLeavePlay = false;
    gWasPlayReady = true;
    gAlertNotified = false;
    gAlertSampleAt = 0;
    gAlertCached = false;
    gFireReadyAt.store(0, std::memory_order_release);
    gExclNotified = false;
    gExclSampleAt = 0;
    gExclCachedOk = true;
    gWatchDisconnect = false;
    gDisconnectingSince = 0;
    gSawConnecting = false;
    gExclArmed = false;
    gNoPacketSince = 0;
    gNoPacketStreak = 0;
}

// BIN seq=8：A8 已武装仍 Connected→Disconnected。停刀/压键后再 settle 发包（不再关无敌）。
// 硬闸用位掩码：每拍可重钉 ChannelHop 位，不被 lie/encounter/supply 的 false 互踩清掉。
// 出刀抑制由 simple_combat::RefreshExternalPauseEffective 随硬闸同步，勿再盲调 SetFireSuppressed。
// holdInvuln 保留形参兼容调用方；换频不再关无敌（BIN 0.1.40：关无敌用户感知失效，停刀已够）。
void PauseCombatForHop(bool holdInvuln) {
    (void)holdInvuln;
    // 始终重钉：即使已 paused，也防其它模块误清位后漏闸。
    simple_combat::SetHardPause(simple_combat::HardPauseHolder::ChannelHop, true);
    if (!gCombatPaused.exchange(true, std::memory_order_acq_rel)) {
        Log("combat pause for hop");
    }
    // F5 战中刚停就换频：强制瞬移自冷，避免 settle 内仍有 tp 态叠 Transfer。
    ports::teleport::ForceNativeCooldownMs(kTeleportForceCdMs);
}

void ResumeCombatAfterHop() {
    // 历史路径可能关过无敌；若仍持有则还原（新路径不再 hold）。
    if (gInvulnHeld) {
        gInvulnHeld = false;
        if (gInvulnWasOn) {
            invuln::SetDesired(true);
            Log("invuln restored after hop");
        }
        gInvulnWasOn = false;
    }
    if (!gCombatPaused.load(std::memory_order_acquire)) {
        // 仍清本模块位，避免 Init 异常路径留下脏位。
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::ChannelHop, false);
        return;
    }
    gCombatPaused.store(false, std::memory_order_release);
    // 只清 ChannelHop 位；遇人/测谎/补给持有的硬闸不受影响（不再用单 bool 互踩）。
    simple_combat::SetHardPause(simple_combat::HardPauseHolder::ChannelHop, false);
    if (encounter::HoldsCombatPause()) {
        Log("combat ChannelHop bit cleared (encounter still holding pause)");
    } else {
        Log("combat resume after hop");
    }
}

bool SampleAlert(DWORD now) {
    if (gAlertSampleAt != 0 && now - gAlertSampleAt < kAlertSampleMs) return gAlertCached;
    JobCtx job{};
    job.kind = JobCtx::Kind::QueryAlert;
    if (!RunJob(job)) {
        gAlertSampleAt = now;
        gAlertCached = false;
        return false;
    }
    gAlertSampleAt = now;
    gAlertCached = job.alert;
    return gAlertCached;
}

// true = 独占未清，暂不可发 TransferChannel
bool SampleExclBusy(DWORD now) {
    if (gExclSampleAt != 0 && now - gExclSampleAt < kExclSampleMs) return !gExclCachedOk;
    JobCtx job{};
    job.kind = JobCtx::Kind::QueryExcl;
    if (!RunJob(job)) {
        gExclSampleAt = now;
        gExclCachedOk = true;  // 探针失败不堵死；FireTransfer 内再拦一次
        return false;
    }
    gExclSampleAt = now;
    gExclCachedOk = job.exclOk;
    if (!job.exclOk) {
        Log("excl busy A8=%u A9=%u A0=%d A4=%d AC=%d canSend=0 seq=%u", job.exclFlagA8,
            job.exclFlagA9, job.exclA0, job.exclA4, job.exclAC, gActiveSeq.load());
    }
    return !gExclCachedOk;
}

void MaybeNotifyAlert(DWORD now) {
    (void)now;
    if (gAlertNotified) return;
    gAlertNotified = true;
    Notify(notify::NotificationKind::Info, "manual-rejoin-alert", "随机换频等待中",
           "战斗警戒中，脱战后再换频");
    Log("waiting alert clear seq=%u", gActiveSeq.load());
}

void MaybeNotifyExcl(DWORD now) {
    (void)now;
    if (gExclNotified) return;
    gExclNotified = true;
    Notify(notify::NotificationKind::Info, "manual-rejoin-excl", "随机换频等待中",
           "独占请求未清，稍后自动换频");
    Log("waiting excl clear seq=%u", gActiveSeq.load());
}

// End active job; never wipe a newer pending seq queued while we were busy.
void FinishActive(DWORD cooldownMs, DWORD now) {
    gActiveSeq.store(0);
    gSoftReloginHop = false;
    ClearAttemptState();
    SetState(State::Idle);
    if (cooldownMs > 0) gCooldownUntil = now + cooldownMs;
    // BIN 11:02：settle 后立刻 resume，~0.5s Session 层重建，~9s 硬断被踢。
    // 静默期内保持停刀/压键，再恢复（无敌不再由 hop hold）。
    gResumeAt = now + kPostHopQuietMs;
    Log("post-hop quiet %ums cooldownMs=%u (hold combat/fire)",
        static_cast<unsigned>(kPostHopQuietMs), static_cast<unsigned>(cooldownMs));
}

void ClearHopFailRecover(const char* why);

// 超级赶路：整段停换频（含 Waiting）。
// 不走 Fail：会 toast「换频失败」并可能 RequestAttempt 软重连，把赶路截断。
// Waiting 包若已出门撤不回，只停本地结算——避免把赶路换图当成迁频 SettleOk。
void AbortHopForTravel() {
    if (!x::features::travel::IsActive()) return;
    const State st = GetStateLocal();
    const uint32_t pend = gPendingSeq.exchange(0);
    const uint32_t active = gActiveSeq.load();
    const bool held = gCombatPaused.load(std::memory_order_acquire) || gResumeAt != 0 ||
                     gHopFailRecoverPending;
    if (st == State::Idle && pend == 0 && !held) return;
    if (st != State::Idle) {
        gActiveSeq.store(0);
        gSoftReloginHop = false;
        ClearAttemptState();
        SetState(State::Idle);
        Log("abort travel seq=%u was=%u (no more transfer)", active,
            static_cast<unsigned>(st));
    }
    gResumeAt = 0;
    gEarlyHoldFromRequest.store(false, std::memory_order_release);
    ClearHopFailRecover("travel");
    ResumeCombatAfterHop();
    ports::teleport::ClearNativeSelfCd();
    if (pend) Log("drop pending seq=%u travel active", pend);
}

void ClearHopFailRecover(const char* why) {
    if (!gHopFailRecoverPending && gHopFailRecoverTries == 0) return;
    gHopFailRecoverPending = false;
    gHopFailRecoverTries = 0;
    gHopFailRecoverRetryAt = 0;
    Log("hop-fail recover clear (%s)", why ? why : "?");
}

bool TryHopFailInMapRecover(const char* why) {
    if (gHopFailRecoverTries >= kHopFailRecoverMaxTries) {
        static DWORD sLog = 0;
        const DWORD now = GetTickCount();
        if (!sLog || now - sLog > 8000) {
            sLog = now;
            Log("hop-fail in-map recover give up tries=%d (%s) — keep pause",
                gHopFailRecoverTries, why ? why : "?");
        }
        return false;
    }
    ++gHopFailRecoverTries;
    gHopFailRecoverRetryAt = GetTickCount() + kHopFailRecoverRetryMs;
    if (soft_login_probe::IsReconnectInFlight()) {
        Log("hop-fail recover skip in_flight try=%d", gHopFailRecoverTries);
        return true;
    }
    if (!soft_login_probe::IsArmed()) {
        Log("hop-fail recover skip not_armed try=%d — keep pause", gHopFailRecoverTries);
        Notify(notify::NotificationKind::Warning, "manual-rejoin-fail-soft",
               "换频失败·请开软重连", "首页「软重连试连」未开，无法拆会话清脏，已停刀。");
        return false;
    }
    soft_login_probe::ClearBreaker("hop_fail_inmap");
    if (x::features::ports::mob_gather::FireProactiveHangup("encounter_hop_fail")) {
        Log("hop-fail CloseSession issued try=%d why=%s", gHopFailRecoverTries,
            why ? why : "?");
        return true;
    }
    Log("hop-fail CloseSession fail try=%d/%d why=%s", gHopFailRecoverTries,
        kHopFailRecoverMaxTries, why ? why : "?");
    return false;
}

void ArmHopFailInMapRecover(const char* why) {
    gHopFailRecoverPending = true;
    gHopFailRecoverTries = 0;
    gHopFailRecoverRetryAt = 0;
    gResumeAt = 0;
    PauseCombatForHop(/*holdInvuln=*/false);
    Log("hop-fail in-map recover arm (%s) — hold combat, CloseSession", why ? why : "?");
    Notify(notify::NotificationKind::Warning, "manual-rejoin-fail-soft", "换频失败·软重连清脏",
           "仍在图内，已停刀并主动拆会话。");
    TryHopFailInMapRecover(why);
}

void TickHopFailRecover(DWORD now) {
    if (!gHopFailRecoverPending) return;
    if (!ports::world::IsPlayReady() || !ports::world::IsInMapScene()) return;
    if (soft_login_probe::IsReconnectInFlight()) return;
    if (now < gHopFailRecoverRetryAt) return;
    TryHopFailInMapRecover("retry");
}

void TickPostHopResume(DWORD now) {
    if (gHopFailRecoverPending) {
        if (!ports::world::IsPlayReady()) {
            ClearHopFailRecover("left_map");
            gResumeAt = now + kPostHopQuietMs;
            Log("post-hop quiet arm after hop-fail leave");
        } else {
            return;
        }
    }
    if (gResumeAt == 0 || now < gResumeAt) return;
    // settle 后晚到的 InterStage：静默期满仍勿 resume，等回图再放刀。
    if (!ports::world::IsPlayReady()) {
        static DWORD s_holdResumeLog = 0;
        if (!s_holdResumeLog || now - s_holdResumeLog > 2000) {
            s_holdResumeLog = now;
            Log("post-hop resume hold: not play-ready scene=%d",
                static_cast<int>(ports::world::GetSceneState()));
        }
        return;
    }
    gResumeAt = 0;
    ResumeCombatAfterHop();
}

void Fail(const char* why, bool recoverSoft = false) {
    const bool encounterHop = gSoftReloginHop;
    const bool inMapReady =
        ports::world::IsInMapScene() && ports::world::IsPlayReady();
    Log("fail seq=%u why=%s pending=%u attempts=%d recoverSoft=%d encounter=%d inMap=%d",
        gActiveSeq.load(), why ? why : "?", gPendingSeq.load(), gFireAttempt,
        recoverSoft ? 1 : 0, encounterHop ? 1 : 0, inMapReady ? 1 : 0);
    Notify(notify::NotificationKind::Warning, "manual-rejoin-fail", "随机换频失败",
           why ? why : "未知错误");
    FinishActive(kCooldownAfterFailMs, GetTickCount());
    // 遇人换频失败（或 recoverSoft）仍在图内：禁止 4s 后原地出刀，抬熔断并 CloseSession 清脏。
    if ((encounterHop || recoverSoft) && inMapReady) {
        ArmHopFailInMapRecover(why);
        return;
    }
    // KickSniff 在 IsMigrateInFlight 时不抢 RequestAttempt。Fail 先 Idle 再拉 soft，
    // 避免与 hop Waiting 双主（BIN 13:59：transfer→117ms Disc→soft 抢跑→hop Fail）。
    // 不改 sticky 到目标频：脏断时包多半没落地，粘回当前 known。
    // TickWaiting 须等 Disc 宽限：C97 19:34 hop1 322ms Disc 立刻走这里 → sticky 旧频。
    if (!recoverSoft && !encounterHop) return;
    if (!soft_login_probe::IsArmed()) return;
    if (soft_login_probe::IsHoldActive() || soft_login_probe::IsAttemptBusy()) {
        Log("soft recover skip after fail: already hold/busy (%s)", why ? why : "?");
        return;
    }
    if (encounterHop) soft_login_probe::ClearBreaker("hop_fail_offmap");
    Log("soft recover after hop fail (%s)", why ? why : "?");
    soft_login_probe::RequestAttempt("disconnected");
    kick_sniff::BumpDisconnectSeq();
}

// 迁频超时仍黑屏：粘 sticky + 拉 soft_login（已 Connected 则 dismiss+RequestRestart；
// 仍不回图则 soft 失败交守护）。soft_login 未开则只记 sticky。
void RecoverMigrateTimeout(const char* why) {
    if (gTargetChannel >= 0 && gTargetChannel <= 64) {
        auto_enter::NoteStickyChannel(gTargetChannel, why);
    }
    if (!soft_login_probe::IsArmed()) {
        Log("soft recover skip: soft_login not armed (%s)", why ? why : "?");
        return;
    }
    if (soft_login_probe::IsHoldActive()) {
        Log("soft recover skip: soft_login already hold (%s)", why ? why : "?");
        return;
    }
    Log("soft recover → RequestAttempt (%s) stickyCh=%d", why ? why : "?",
        DispCh(gTargetChannel));
    soft_login_probe::RequestAttempt(why ? why : "channel_hop_timeout");
    Notify(notify::NotificationKind::Info, "manual-rejoin-soft", "换频超时·软重连",
           "黑屏未回图，已触发软重连试进");
}

void RequestNmGoneSoft() {
    if (soft_login_probe::IsHoldActive()) return;
    if (!soft_login_probe::IsArmed()) return;
    Log("soft recover → RequestAttempt (nm_gone_inmap)");
    soft_login_probe::RequestAttempt("nm_gone_inmap");
}

// A8 假置位 + 内存频道号抖动 ≠ 真迁频。无 Connecting / 离图且 NM 已空时禁止 settle。
bool RejectDeadSessionSettle() {
    if (gSawConnecting || gSawLeavePlay) return false;
    if (kick_sniff::HasResolvedSession()) return false;
    Fail("换频时会话已断（未见真实迁频）", /*recoverSoft=*/true);
    RequestNmGoneSoft();
    return true;
}

void SettleOk(const char* how, int curIdx, DWORD now) {
    // 优先采信「已变化的内存 idx」；否则用目标（+0x68 常卡死）
    if (curIdx >= 0 && gFromChannel >= 0 && curIdx != gFromChannel) {
        gKnownChannelIdx = curIdx;
    } else if (gTargetChannel >= 0) {
        gKnownChannelIdx = gTargetChannel;
    } else if (curIdx >= 0) {
        gKnownChannelIdx = curIdx;
    }

    const bool idLag = gFromChannel >= 0 && (curIdx < 0 || curIdx == gFromChannel);
    const int shownToIdx = (!idLag && curIdx >= 0) ? curIdx
                                                   : (gTargetChannel >= 0 ? gTargetChannel : curIdx);
    Log("settle ok seq=%u idx=%d ch=%d fromIdx=%d fromCh=%d targetIdx=%d targetCh=%d how=%s "
        "idLag=%d known=%d attempts=%d pending=%u cooldownMs=%u",
        gActiveSeq.load(), curIdx, DispCh(curIdx), gFromChannel, DispCh(gFromChannel),
        gTargetChannel, DispCh(gTargetChannel), how ? how : "?", idLag ? 1 : 0, gKnownChannelIdx,
        gFireAttempt, gPendingSeq.load(), static_cast<unsigned>(kCooldownAfterOkMs));
    if (idLag) {
        Log("settle soft-warn channel_id_lag rawIdx=%d still_from=%d target=%d how=%s", curIdx,
            gFromChannel, gTargetChannel, how ? how : "?");
    }
    char body[96]{};
    if (idLag && gTargetChannel >= 0) {
        snprintf(body, sizeof(body), "ch.%d → ch.%d（频道号滞后）", DispCh(gFromChannel),
                 DispCh(gTargetChannel));
    } else {
        snprintf(body, sizeof(body), "ch.%d → ch.%d", DispCh(gFromChannel), DispCh(shownToIdx));
    }
    Notify(notify::NotificationKind::Info, "manual-rejoin-ok", "随机换频成功", body);
    // sticky 与 SelectChannel 同口径（0-based 列表 id）；UI 显示再 +1。
    {
        const int api = gKnownChannelIdx >= 0 ? gKnownChannelIdx : shownToIdx;
        if (api >= 1 && api <= 64) auto_enter::NoteStickyChannel(api, "channel_hop");
    }
    FinishActive(kCooldownAfterOkMs, now);
}

bool TryRetryOtherChannel(const char* why, int stayChannel, DWORD now, bool markRejected) {
    MarkTried(gTargetChannel);
    if (markRejected) ccu::MarkChannelRejected(gTargetChannel);
    if (gFireAttempt >= kMaxFireAttempts) {
        char body[128]{};
        snprintf(body, sizeof(body), "%s（已试 %d 次）", why ? why : "换频失败", gFireAttempt);
        Fail(body);
        return false;
    }
    const int stay = (stayChannel >= 0) ? stayChannel : gFromChannel;
    const int next = PickRandomChannel(stay, gChannelCount);
    if (next < 0) {
        Fail("无更多可试频道（可能均满）");
        return false;
    }
    Log("retry seq=%u attempt=%d/%d stayIdx=%d stayCh=%d failedTargetIdx=%d nextIdx=%d nextCh=%d "
        "markReject=%d why=%s",
        gActiveSeq.load(), gFireAttempt + 1, kMaxFireAttempts, stay, DispCh(stay), gTargetChannel,
        next, DispCh(next), markRejected ? 1 : 0, why ? why : "?");
    char body[96]{};
    snprintf(body, sizeof(body), "ch.%d 进不去，改试 ch.%d（%d/%d）", DispCh(gTargetChannel),
             DispCh(next), gFireAttempt + 1, kMaxFireAttempts);
    Notify(notify::NotificationKind::Info, "manual-rejoin-retry", "随机换频重试", body);
    gTargetChannel = next;
    gStaySince = 0;
    gLandedAt = 0;
    gSawLeavePlay = false;
    gWasPlayReady = true;
    gExclArmed = false;
    gSawConnecting = false;
    gDisconnectingSince = 0;
    gNoPacketSince = 0;
    gNoPacketStreak = 0;
    gPhaseAt = now;
    SetState(State::Confirming);
    return true;
}

void SucceedQueued() {
    Log("transfer fired seq=%u fromIdx=%d fromCh=%d toIdx=%d toCh=%d attempt=%d/%d exclArmed=%d (no UI)",
        gActiveSeq.load(), gFromChannel, DispCh(gFromChannel), gTargetChannel,
        DispCh(gTargetChannel), gFireAttempt, kMaxFireAttempts, gExclArmed ? 1 : 0);
    // 重试中不刷「已触发」；最终 settle ok / Fail 再通知。
    if (gFireAttempt <= 1) {
        char body[96]{};
        snprintf(body, sizeof(body), "ch.%d → ch.%d", DispCh(gFromChannel), DispCh(gTargetChannel));
        Notify(notify::NotificationKind::Info, "manual-rejoin", "随机换频已触发", body);
    }
    gStaySince = 0;
    gLandedAt = 0;
    gSawLeavePlay = false;
    gWasPlayReady = ports::world::IsPlayReady();
    gSawConnecting = false;
    gDisconnectingSince = 0;
    // 正常迁频：Connected→Connecting→Connected；硬断：→Disconnected（BIN seq=18）
    gWatchDisconnect = true;
    SetState(State::Waiting);
    gPhaseAt = GetTickCount();
}

// 每 Tick 更新：进图冷却只约束「刚进图 / 刚落地」窗口，不是「点 F10 才开始计时」。
// BIN：Idle 无 pending 时早期 return，若不持续记时，图里挂很久再 hop 也会误报「进图冷却」。
void UpdatePlayReadyClock(DWORD now) {
    // soft hold / land_quiet / attempt / deferred：PlayReady 可能已真，但会话未稳。
    // 冻结进图时钟，等 IsReconnectInFlight 结束后再起算满 kLandGraceMs（BIN 23:48：
    // soft 途中 playReadySince 已走过，land_quiet 一结束立刻 BeginActive）。
    if (!ports::world::IsPlayReady() || soft_login_probe::IsReconnectInFlight()) {
        gPlayReadySince = 0;
        return;
    }
    if (gPlayReadySince == 0) gPlayReadySince = now;
}

const char* DeferReason() {
    if (soft_login_probe::IsReconnectInFlight()) return "软重连中";
    if (!ports::world::IsPlayReady() || gPlayReadySince == 0) return "未进图";
    const DWORD now = GetTickCount();
    // 换频静默只挡自动 resume 出刀，不挡立刻再 hop（BeginActive 会清 gResumeAt 并续持闸）
    if (now - gPlayReadySince < kLandGraceMs) return "进图冷却";
    const auto ss = ports::world::GetSceneState();
    if (ss == ports::world::SceneState::CashShop) return "商城中";
    if (ss == ports::world::SceneState::Login) return "登录中";
    if (auto_lie::IsBusy()) return "测谎中";
    if (now < gCooldownUntil) return "冷却中";
    // 警戒不在 Idle defer：BeginActive 停手后再 Confirming 等解除（避免一直打怪刷警戒）
    return nullptr;
}

void MaybeNotifyDefer(uint32_t seq, const char* reason, DWORD now) {
    static DWORD s_lastDeferLog = 0;
    if (now - s_lastDeferLog > 2000) {
        s_lastDeferLog = now;
        Log("defer seq=%u reason=%s", seq, reason);
    }
    if (gLastDeferNotifySeq == seq) return;
    gLastDeferNotifySeq = seq;
    char body[96]{};
    const bool autoAfter =
        reason && (std::strcmp(reason, "冷却中") == 0 || std::strcmp(reason, "换频静默") == 0);
    if (autoAfter)
        snprintf(body, sizeof(body), "延后：%s（结束后自动换）", reason);
    else
        snprintf(body, sizeof(body), "延后：%s", reason ? reason : "?");
    Notify(notify::NotificationKind::Info, "manual-rejoin-defer", "随机换频排队中", body);
}

void BeginActive(uint32_t seq, DWORD now) {
    if (x::features::travel::IsActive()) {
        Log("begin skip seq=%u travel active", seq);
        return;
    }
    gActiveSeq.store(seq);
    gSoftReloginHop = IsEncounterHopSeq(seq);
    // Consume only this seq; keep a newer pending that raced in after Idle check.
    uint32_t expected = seq;
    (void)gPendingSeq.compare_exchange_strong(expected, 0);
    gResumeAt = 0;  // 取消未到期的延迟 resume；Pause 会续持闸
    // Request 预武装的 settle 截止；ClearAttemptState 会清零，先取出。
    const DWORD armedReady = gFireReadyAt.load(std::memory_order_acquire);
    gEarlyHoldFromRequest.store(false, std::memory_order_release);
    ClearAttemptState();
    PauseCombatForHop(/*holdInvuln=*/true);
    // settle 从 Request 起算；defer 过久已到期则从 now 重新武装满窗。
    DWORD ready = armedReady;
    if (ready == 0 || ready <= now) ready = now + kPreFireSettleMs;
    gFireReadyAt.store(ready, std::memory_order_release);
    gPhaseAt = now;
    gLastDeferNotifySeq = 0;
    SetState(State::Selecting);
    Log("begin seq=%u (%s, no menu) preFireSettle=%ums readyIn=%ums", seq,
        gSoftReloginHop ? "soft-hop CloseSession" : "direct SendTransfer",
        static_cast<unsigned>(kPreFireSettleMs), static_cast<unsigned>(ready - now));
}

void CommitEncounterSoftHop() {
    const int from = gFromChannel;
    int to = gTargetChannel;
    if (to < 0 || to == from) {
        Fail("遇人软重连无可用新频");
        return;
    }
    // NoteStickyChannel / PickSticky 拒 0（列表 id 0 = UI 频道1）。再抽一次。
    if (to < 1) {
        MarkTried(to);
        to = PickRandomChannel(from, gChannelCount);
        gTargetChannel = to;
        if (to < 1 || to == from) {
            Fail("遇人软重连目标频无法粘（列表id=0）");
            return;
        }
    }
    // 必须先改 known：Login 的 PushStickyFromKnown 会用 known 盖 sticky，不改就会回原频。
    gKnownChannelIdx = to;
    // sticky 还没同步时先钉原频再钉目标，PickSticky miss 才能排除原频。
    if (from >= 1 && auto_enter::StickyChannel1Based() <= 0)
        auto_enter::NoteStickyChannel(from, "encounter_soft_hop_from");
    auto_enter::NoteStickyChannel(to, "encounter_soft_hop");
    Log("soft-hop commit seq=%u fromIdx=%d fromCh=%d toIdx=%d toCh=%d", gActiveSeq.load(), from,
        DispCh(from), to, DispCh(to));
    char body[96]{};
    snprintf(body, sizeof(body), "ch.%d → ch.%d（软重连，不回原频）", DispCh(from), DispCh(to));
    Notify(notify::NotificationKind::Info, "manual-rejoin", "遇人换频·软重连", body);
    if (!x::features::ports::mob_gather::FireProactiveHangup("encounter_soft_hop")) {
        if (from >= 0) gKnownChannelIdx = from;
        if (from >= 1) auto_enter::NoteStickyChannel(from, "encounter_soft_hop_revert");
        Fail("遇人软重连拆会话失败");
        return;
    }
    FinishActive(kCooldownAfterOkMs, GetTickCount());
}

void TickSelecting(DWORD now) {
    if (x::features::travel::IsActive()) return;
    (void)now;
    JobCtx info{};
    info.kind = JobCtx::Kind::ReadInfo;
    if (!RunJob(info)) {
        Fail(info.err[0] ? info.err : "读频道信息失败");
        return;
    }
    gFromChannel = info.channelId;
    gChannelCount = info.channelCount;
    gTargetChannel = PickRandomChannel(info.channelId, info.channelCount);
    if (gTargetChannel < 0) {
        Fail("仅有一个频道");
        return;
    }
    Log("pick curIdx=%d curCh=%d count=%d countSrc=%s chSrc=%s raw68=%d raw6c=%d targetIdx=%d "
        "targetCh=%d",
        info.channelId, DispCh(info.channelId), info.channelCount, info.countSrc, info.channelSrc,
        info.channelRaw68, info.channelRaw6c, gTargetChannel, DispCh(gTargetChannel));
    if (WantEncounterSoftHop()) {
        CommitEncounterSoftHop();
        return;
    }
    SetState(State::Confirming);
    gPhaseAt = GetTickCount();
}

void TickConfirming(DWORD now) {
    if (x::features::travel::IsActive()) return;
    if (WantEncounterSoftHop()) {
        Log("confirm redirect soft-hop (no SendTransfer) seq=%u", gActiveSeq.load());
        CommitEncounterSoftHop();
        return;
    }
    if (gTargetChannel < 0) {
        Fail("无目标频道");
        return;
    }
    // BIN：警戒态 SendTransfer 静默失败（未离图）。等解除再发，不烧 attempt。
    if (SampleAlert(now)) {
        MaybeNotifyAlert(now);
        if (now - gPhaseAt > kWaitAlertMs) {
            Fail("警戒中无法换频（已停手仍超时）");
        }
        return;
    }
    if (gAlertNotified) {
        // 刚脱战：重设 phase，并推迟 fireReady（BIN seq=7）
        gPhaseAt = now;
        const DWORD ready = now + kPostAlertGraceMs;
        const DWORD cur = gFireReadyAt.load(std::memory_order_acquire);
        if (ready > cur) gFireReadyAt.store(ready, std::memory_order_release);
        Log("post-alert grace %ums seq=%u", static_cast<unsigned>(kPostAlertGraceMs),
            gActiveSeq.load());
    }
    gAlertNotified = false;
    {
        const DWORD readyAt = gFireReadyAt.load(std::memory_order_acquire);
        if (readyAt != 0 && now < readyAt) return;
        gFireReadyAt.store(0, std::memory_order_release);
    }

    // 空中点换频：硬闸已触发安全落台；站稳前勿发 Transfer（否则仍可能半空切频掉穿）。
    if (simple_combat::IsSafeLandActive()) {
        static DWORD sLandDeferLog = 0;
        if (!sLandDeferLog || now - sLandDeferLog > 1000) {
            sLandDeferLog = now;
            Log("defer transfer safe_land seq=%u", gActiveSeq.load());
        }
        return;
    }

    // 瞬移自冷未清：再推迟，避免 tp 刚落地立刻 Transfer（BIN：停火后仍踢）
    {
        const DWORD tpRem = ports::teleport::NativeCooldownRemainingMs();
        if (tpRem > 0) {
            const DWORD ready = now + tpRem;
            const DWORD cur = gFireReadyAt.load(std::memory_order_acquire);
            if (ready > cur) gFireReadyAt.store(ready, std::memory_order_release);
            static DWORD s_tpDeferLog = 0;
            if (!s_tpDeferLog || now - s_tpDeferLog > 1000) {
                s_tpDeferLog = now;
                Log("defer transfer tpCdRem=%ums seq=%u", static_cast<unsigned>(tpRem),
                    gActiveSeq.load());
            }
            return;
        }
    }

    // BIN：CanSendExcl(500)==0 则无 op=44。等独占清，不烧 attempt。
    if (SampleExclBusy(now)) {
        MaybeNotifyExcl(now);
        if (now - gPhaseAt > kWaitExclMs) {
            Fail("独占请求未清，无法换频");
        }
        return;
    }
    gExclNotified = false;

    // BIN 19:38：NM 已空仍 SendTransfer → A8 假置位、27→28 假 settle、怪继续站桩。
        if (!kick_sniff::HasResolvedSession()) {
            Fail("会话已断，换频发不出", /*recoverSoft=*/true);
            RequestNmGoneSoft();
            return;
        }

    // 再收一次在途攻击键；settle 跟墙钟对齐，防末火刚过就叠 Transfer
    (void)ports::attack::WaitFireIdle(kFireIdleTimeoutMs, kFireIdleSettleMs);
    if (x::features::travel::IsActive()) {
        Log("confirm skip transfer travel seq=%u", gActiveSeq.load());
        return;
    }

    JobCtx fire{};
    fire.kind = JobCtx::Kind::FireTransfer;
    fire.channelId = gTargetChannel;
    ++gFireAttempt;
    if (!RunJob(fire)) {
        Log("transfer blocked/fail seq=%u targetIdx=%d err=%s A8=%u→%u canSend=%d",
            gActiveSeq.load(), gTargetChannel, fire.err[0] ? fire.err : "?", fire.exclFlagA8,
            fire.exclFlagA8After, fire.exclOk ? 1 : 0);
        if (std::strcmp(fire.err, "travel active") == 0) {
            --gFireAttempt;
            return;
        }
        // excl busy 不应烧满人重试；回 Confirming 同目标等（attempt 已+1，用下一轮 Sample 挡）
        if (std::strcmp(fire.err, "excl busy") == 0) {
            --gFireAttempt;
            gPhaseAt = now;  // 重新起算等待窗
            return;
        }
        if (!TryRetryOtherChannel(fire.err[0] ? fire.err : "SendTransfer 失败", gFromChannel,
                                  GetTickCount(), /*markRejected=*/false)) {
            // Fail already
        }
        return;
    }
    Log("transfer fired seq=%u targetIdx=%d targetCh=%d attempt=%d A8=%u→%u A0=%d A4=%d AC=%d "
        "quietMs=%u",
        gActiveSeq.load(), gTargetChannel, DispCh(gTargetChannel), gFireAttempt, fire.exclFlagA8,
        fire.exclFlagA8After, fire.exclA0, fire.exclA4, fire.exclAC,
        static_cast<unsigned>(now - gPhaseAt));
    // BIN：A8 未置位 ≈ 包未进独占/未真正发出。勿进 Waiting，勿 MarkRejected（假满人）。
    if (fire.exclFlagA8After == 0) {
        if (gNoPacketSince == 0) gNoPacketSince = now;
        ++gNoPacketStreak;
        const DWORD elapsed = now - gNoPacketSince;
        const bool giveUp =
            gNoPacketStreak >= kNoPacketMaxStreak || elapsed >= kWaitNoPacketMs;
        Log("no-packet A8 still 0 after SendTransfer streak=%d elapsed=%ums giveUp=%d — %s",
            gNoPacketStreak, static_cast<unsigned>(elapsed), giveUp ? 1 : 0,
            giveUp ? "retry other / fail, no reject" : "soft wait, no reject");
        if (giveUp) {
            // 本发射计入 attempt（不回滚），换频但不 MarkRejected
            if (!TryRetryOtherChannel("A8 未置位（疑似未发包）", gFromChannel, now,
                                      /*markRejected=*/false)) {
                // Fail already
            }
            return;
        }
        --gFireAttempt;
        gPhaseAt = now;
        gFireReadyAt.store(now + kNoPacketBackoffMs, std::memory_order_release);
        gExclCachedOk = false;
        gExclSampleAt = 0;
        return;
    }
    gNoPacketSince = 0;
    gNoPacketStreak = 0;
    gExclArmed = true;
    MarkTried(gTargetChannel);
    SucceedQueued();
}

void TickWaiting(DWORD now) {
    if (WantEncounterSoftHop()) {
        Log("waiting abort: encounter must not use native migrate seq=%u", gActiveSeq.load());
        Fail("遇人换频误入原生迁频", /*recoverSoft=*/true);
        return;
    }
    const bool play = ports::world::IsPlayReady();
    const auto ss = ports::world::GetSceneState();

    // BIN：正常迁频 Connected→Connecting→Connected；硬断 →Disconnected。
    // Disconnecting / Disconnected 短闪都不立刻 Fail：C97 19:34 hop1 fire→322ms Disc
    // 立刻 recoverSoft 粘回旧频；同窗 hop2 要 241ms 才见 Connecting。已见 Connecting/离图
    // 则 Disc 是迁频途中的正常抖，交给后面 leave/reland。
    if (gWatchDisconnect) {
        const int sess = kick_sniff::LastSessionState();
        if (sess == kSessConnecting) {
            if (!gSawConnecting) {
                gSawConnecting = true;
                Log("sess Connecting (migrate) seq=%u", gActiveSeq.load());
            }
            gDisconnectingSince = 0;
        } else if (sess == kSessDisconnected || sess == kSessDisconnecting) {
            if (gSawConnecting || gSawLeavePlay) {
                gDisconnectingSince = 0;
            } else {
                if (gDisconnectingSince == 0) {
                    gDisconnectingSince = now;
                    Log("sess %s (migrate flash) seq=%u wait %ums",
                        sess == kSessDisconnected ? "Disconnected" : "Disconnecting",
                        gActiveSeq.load(), (unsigned)kDisconnectingGraceMs);
                }
                if (now - gDisconnectingSince >= kDisconnectingGraceMs) {
                    char why[96]{};
                    snprintf(why, sizeof(why),
                             sess == kSessDisconnected ? "会话已断开 sess=%d（换频未完成）"
                                                       : "会话断开中超时 sess=%d（换频未完成）",
                             sess);
                    Fail(why, /*recoverSoft=*/true);
                    return;
                }
            }
        } else {
            gDisconnectingSince = 0;
        }
        if (ss == ports::world::SceneState::Login) {
            if (gSawConnecting || gSawLeavePlay) {
                RecoverMigrateTimeout("channel_hop_login");
                Fail("回到登录（换频断线）");
            } else {
                Fail("回到登录（换频断线）", /*recoverSoft=*/true);
            }
            return;
        }
    }

    // 电平采样（非仅边沿）：任一 tick 见 InterStage/非玩法就绪即记迁频，降低 80ms 漏检。
    const bool leftScene =
        !play || ss == ports::world::SceneState::InterStage || ss == ports::world::SceneState::None;
    if (leftScene) {
        if (!gSawLeavePlay) {
            Log("leave play (migrate) seq=%u target=%d scene=%d play=%d", gActiveSeq.load(),
                gTargetChannel, static_cast<int>(ss), play ? 1 : 0);
        }
        gSawLeavePlay = true;
        gStaySince = 0;
        gLandedAt = 0;
    }
    if (!gWasPlayReady && play) {
        gLandedAt = now;
        gStaySince = 0;
        Log("reland play seq=%u scene=%d", gActiveSeq.load(), static_cast<int>(ss));
    }
    gWasPlayReady = play;

    if (!play) {
        if (now - gPhaseAt > kWaitMigrateMs) {
            RecoverMigrateTimeout("channel_hop_interstage");
            Fail("换频超时（迁频未回图）");
        }
        return;
    }

    void* wm = ports::world::PeekWorldManager();
    if (!wm) {
        if (now - gPhaseAt > kWaitMigrateMs) Fail("换频后无 WorldManager");
        return;
    }
    const int cur68 = ReadI32(wm, gOffWmChannelId);
    const int cur6c = ReadI32(wm, gOffWmChannelAlt);
    // 结算优先 6c（BIN：换频后常追上目标；68 常滞后）
    int cur = cur68;
    if (gTargetChannel >= 0 && cur6c == gTargetChannel) {
        cur = cur6c;
    } else if (gTargetChannel >= 0 && cur68 == gTargetChannel) {
        cur = cur68;
    } else if (gFromChannel >= 0 && cur6c >= 0 && cur6c != gFromChannel) {
        cur = cur6c;
    } else if (cur6c >= 0 && (cur68 < 0 || cur68 == gFromChannel)) {
        cur = cur6c;
    }

    // 已发包/见 Connecting 但尚未离图：晚到 InterStage 仍在路上，禁止提前 settle。
    const bool awaitLateLeave =
        (gExclArmed || gSawConnecting) && !gSawLeavePlay &&
        (now - gPhaseAt < kPostFireLeaveExpectMs);
    if (awaitLateLeave) {
        static DWORD s_awaitLeaveLog = 0;
        if (!s_awaitLeaveLog || now - s_awaitLeaveLog > 1500) {
            s_awaitLeaveLog = now;
            Log("await late leave seq=%u target=%d armed=%d sawConn=%d raw68=%d raw6c=%d elapsed=%ums",
                gActiveSeq.load(), gTargetChannel, gExclArmed ? 1 : 0, gSawConnecting ? 1 : 0,
                cur68, cur6c, static_cast<unsigned>(now - gPhaseAt));
        }
        return;
    }

    // 硬成功：内存频道号已追上目标（0-based）
    // 过晚窗仍未离图 + 频道已变：真软迁频；频道已追上也可 settle。
    if (gTargetChannel >= 0 && (cur6c == gTargetChannel || cur68 == gTargetChannel)) {
        const char* how = cur6c == gTargetChannel ? "channel_match_6c" : "channel_match_68";
        if ((gExclArmed || gSawConnecting) && !gSawLeavePlay) {
            how = cur6c == gTargetChannel ? "channel_match_6c_stable" : "channel_match_68_stable";
        }
        SettleOk(how, cur6c == gTargetChannel ? cur6c : cur68, now);
        return;
    }

    // BIN：黑屏 InterStage→Field 时 _channelID 常仍短暂为旧值。
    // 见过离图再落地 = 迁频已发生 → 软成功（可再等频道号，但不重试）。
    if (gSawLeavePlay) {
        if (gLandedAt == 0) gLandedAt = now;
        if (cur >= 0 && gFromChannel >= 0 && cur != gFromChannel) {
            SettleOk("left_and_channel_changed", cur, now);
            return;
        }
        if (now - gLandedAt >= kPostLandGraceMs) {
            SettleOk("left_and_reland", cur, now);
            return;
        }
        return;  // 落地宽限内只等，绝不 retry
    }

    // 从未离图：+0x68 常陈旧，不能依赖 cur==from。
    // 若仍警戒 → 未真正发出时可回 Confirming 同目标再发；
    // 已 A8 武装 / 见过 Connecting：迁频可能已在路上，绝回 Confirming 重发（BIN 0.1.40 首发成功仍二次换频）。
    if (SampleAlert(now)) {
        MaybeNotifyAlert(now);
        if (gExclArmed || gSawConnecting) {
            static DWORD s_alertHoldLog = 0;
            if (!s_alertHoldLog || now - s_alertHoldLog > 1500) {
                s_alertHoldLog = now;
                Log("waiting alert hold (no re-fire) seq=%u target=%d armed=%d sawConn=%d",
                    gActiveSeq.load(), gTargetChannel, gExclArmed ? 1 : 0, gSawConnecting ? 1 : 0);
            }
            if (cur >= 0 && gFromChannel >= 0 && cur != gFromChannel) {
                if (RejectDeadSessionSettle()) return;
                SettleOk("alert_hold_channel_changed", cur, now);
                return;
            }
            // 警戒持有时也不再假设目标已达：等到迁频总窗再 Fail，避免黑屏假成功。
            if (now - gPhaseAt > kWaitMigrateMs) {
                RecoverMigrateTimeout("channel_hop_alert_timeout");
                Fail(gSawConnecting ? "警戒中换频超时（未见离图）" : "警戒中换频未确认落地");
            }
            return;
        }
        Log("waiting re-fire after alert reject seq=%u target=%d attempt=%d", gActiveSeq.load(),
            gTargetChannel, gFireAttempt);
        if (now - gPhaseAt > kWaitAlertMs) {
            Fail("警戒中换频被拒");
            return;
        }
        // 回退一次 attempt，允许警戒解除后重发同一目标
        if (gFireAttempt > 0) --gFireAttempt;
        gStaySince = 0;
        gLandedAt = 0;
        gSawLeavePlay = false;
        gWasPlayReady = true;
        gWatchDisconnect = false;
        gExclArmed = false;
        gSawConnecting = false;
        gDisconnectingSince = 0;
        gFireReadyAt.store(now + kPostAlertGraceMs, std::memory_order_release);
        gPhaseAt = now;  // 重新起算警戒等待
        SetState(State::Confirming);
        return;
    }

    if (now - gPhaseAt < kSettleReadyMs + kStayConfirmMs) return;
    // 已见 Connecting / A8 武装：禁止 migrate_seen_no_leave 假成功。
    // 过晚窗仍 play：仅当内存频道已变才软成功；否则等到 migrate 窗 Fail。
    if (gSawConnecting || gExclArmed) {
        if (cur >= 0 && gFromChannel >= 0 && cur != gFromChannel) {
            if (RejectDeadSessionSettle()) return;
            SettleOk(gSawConnecting ? "migrate_stable_channel_changed" : "armed_channel_changed",
                     cur, now);
            return;
        }
        static DWORD s_armedWaitLog = 0;
        if (!s_armedWaitLog || now - s_armedWaitLog > 1500) {
            s_armedWaitLog = now;
            Log("waiting migrate land (no early settle) seq=%u target=%d sawConn=%d armed=%d "
                "raw68=%d raw6c=%d from=%d",
                gActiveSeq.load(), gTargetChannel, gSawConnecting ? 1 : 0, gExclArmed ? 1 : 0,
                cur68, cur6c, gFromChannel);
        }
        if (now - gPhaseAt > kWaitMigrateMs) {
            RecoverMigrateTimeout(gSawConnecting ? "channel_hop_no_leave"
                                                : "channel_hop_armed_timeout");
            Fail(gSawConnecting ? "换频超时（迁频未见离图）" : "换频超时（已发包未见频道变化）");
        }
        return;
    }
    // 未见发包：可换目标，不污染 prefer
    char why[96]{};
    snprintf(why, sizeof(why), "未离图（未确认发包） raw68=%d raw6c=%d fromIdx=%d targetIdx=%d",
             cur68, cur6c, gFromChannel, gTargetChannel);
    (void)TryRetryOtherChannel(why, gFromChannel, now, /*markRejected=*/false);
}

}  // namespace

int LastKnownChannel1Based() { return KnownDisp1Based(); }

int DisplayChannel1Based() {
    // 玩家 UI = 列表 id / WM+0x6C + 1。BIN 08-15：sticky=39 raw6c=39 → 頻道 40。
    const int api = auto_enter::StickyChannel1Based();
    if (api > 0) return api + 1;
    return KnownDisp1Based();
}

void SyncKnownAfterEnter(int channelId1Based, const char* why) {
    if (channelId1Based < 1 || channelId1Based > 64) return;
    const int idx = channelId1Based;
    const int prev = gKnownChannelIdx;
    gKnownChannelIdx = idx;
    // 清前进基线：下一拍 ObserveWm 走 known，勿把进图后 +0x6C 抖动当 wm6c_adv。
    gLastRaw68 = -999;
    gLastRaw6c = -999;
    if (prev != idx) {
        Log("sync known after enter %d→%d (ui=%d) why=%s — wm baseline reset", prev, idx,
            DispCh(idx), why ? why : "?");
    } else {
        Log("sync known after enter keep idx=%d (ui=%d) why=%s — wm baseline reset", idx,
            DispCh(idx), why ? why : "?");
    }
}

void NoteCrowdedChannel() {
    if (!TryRefreshKnownIdx("note_crowded")) {
        Log("soft-avoid skip: knownIdx unset");
        return;
    }
    const int idx = gKnownChannelIdx;
    if (idx < 0 || idx >= 128) {
        Log("soft-avoid skip: knownIdx unset");
        return;
    }
    const int mapId = ports::world::GetMapId();
    if (mapId > 0) {
        EnsureSoftAvoidMap(mapId);
    } else if (gSoftAvoidMapId < 0) {
        gSoftAvoidMapId = 0;  // 无图号：会话级，换到真图号时 adopt 保留 mark
    }
    const DWORD now = GetTickCount();
    const bool wasLive = SoftAvoidActive(idx, now);
    gSoftAvoidAt[idx] = now;
    CountSoftAvoidLive(now);
    Log("soft-avoid mark idx=%d ch=%d map=%d refresh=%d live=%d ttl=%ums", idx, DispCh(idx),
        gSoftAvoidMapId, wasLive ? 1 : 0, gSoftAvoidLive, (unsigned)kSoftAvoidTtlMs);
}

void OnMapChanged(int mapId) {
    if (mapId <= 0) {
        if (gSoftAvoidMapId >= 0 || gSoftAvoidLive > 0) ClearSoftAvoid("map_unknown");
        return;
    }
    EnsureSoftAvoidMap(mapId);
}

void Init() {
    // PLAY 冷启动可能晚于 Control Apply：保留已排队 seq，否则首点/F10 会被 Init 清掉。
    const uint32_t keepPending = gPendingSeq.load(std::memory_order_acquire);
    const DWORD keepReady = gFireReadyAt.load(std::memory_order_acquire);
    const bool keepEarly = gEarlyHoldFromRequest.load(std::memory_order_acquire);

    gWorkerStop.store(false);
    SetState(State::Idle);
    gPendingSeq.store(0);
    gActiveSeq.store(0);
    gLastDeferNotifySeq = 0;
    gPlayReadySince = 0;
    gCombatPaused.store(false, std::memory_order_release);
    gEarlyHoldFromRequest.store(false, std::memory_order_release);
    gInvulnHeld = false;
    gInvulnWasOn = false;
    simple_combat::SetHardPause(simple_combat::HardPauseHolder::ChannelHop, false);
    gKnownChannelIdx = -1;
    gLastRaw68 = -999;
    gLastRaw6c = -999;
    gAdultFlagN = 0;
    gCooldownUntil = 0;
    gResumeAt = 0;
    ClearHopFailRecover("init");
    ClearSoftAvoid("init");
    gWatchDisconnect = false;
    gDisconnectingSince = 0;
    gSawConnecting = false;
    gExclArmed = false;
    gNoPacketSince = 0;
    gNoPacketStreak = 0;
    gFireReadyAt.store(0, std::memory_order_release);
    Log("Init (direct transfer, no UIChannelShift/GameMenu)");

    if (keepPending != 0) {
        gPendingSeq.store(keepPending, std::memory_order_release);
        const DWORD now = GetTickCount();
        DWORD ready = keepReady;
        if (ready == 0 || ready <= now) ready = now + kPreFireSettleMs;
        gFireReadyAt.store(ready, std::memory_order_release);
        gEarlyHoldFromRequest.store(true, std::memory_order_release);
        PauseCombatForHop(/*holdInvuln=*/false);
        Log("Init keep pending seq=%u early=%d readyIn=%ums", keepPending, keepEarly ? 1 : 0,
            static_cast<unsigned>(ready - now));
    }
}

void Shutdown() { StopWorker(); }

void RequestRejoin(uint32_t seq, bool encounterSoftHop) {
    if (seq == 0) return;
    // 超级赶路中禁止新换频：PauseCombatForHop 会 ForceNativeCooldown 4s，贴门 ↑ 吃不到门
    // （BIN 06:04：enter-armed 同拍 hop → kbd Up timeout + fake soft）。
    if (x::features::travel::IsActive()) {
        Log("request skip seq=%u travel active", seq);
        return;
    }
    // BIN 0.1.37：request→BeginActive 可隔数十 ms，其间仍 fire/MoveTo。边沿立刻硬闸停刀。
    // 无敌留到 BeginActive，避免 defer（测谎/进图）久等无无敌。
    PauseCombatForHop(/*holdInvuln=*/false);
    gEarlyHoldFromRequest.store(true, std::memory_order_release);
    if (GetStateLocal() != State::Idle) {
        // 进行中的遇人 hop 禁止被 F10 把 flag 改回 SendTransfer。
        const bool wantSoft = encounterSoftHop || IsEncounterHopSeq(seq);
        if (WantEncounterSoftHop() && !wantSoft) {
            Log("busy queue seq=%u state=%u keep softHop (encounter in flight)", seq,
                gState.load());
        } else if (!WantEncounterSoftHop()) {
            gSoftReloginHop = wantSoft;
        }
        Log("busy queue seq=%u state=%u softHop=%d (early pause held)", seq, gState.load(),
            WantEncounterSoftHop() ? 1 : 0);
        gPendingSeq.store(seq);
        return;
    }
    gSoftReloginHop = encounterSoftHop || IsEncounterHopSeq(seq);
    // settle 从点击起算（BeginActive 会保留未到期的 armedReady）
    gFireReadyAt.store(GetTickCount() + kPreFireSettleMs, std::memory_order_release);
    gPendingSeq.store(seq);
    Log("request seq=%u softHop=%d (early combat pause, settle=%ums)", seq, encounterSoftHop ? 1 : 0,
        static_cast<unsigned>(kPreFireSettleMs));
}

void RequestManualRejoin(uint32_t seq) { RequestRejoin(seq, false); }

void RequestEncounterSoftHop(uint32_t seq) { RequestRejoin(seq, true); }

bool IsEncounterSoftHop() { return WantEncounterSoftHop(); }

State GetState() { return GetStateLocal(); }

const char* GetStateName() {
    switch (GetStateLocal()) {
    case State::Idle:
        return "Idle";
    case State::Selecting:
        return "Selecting";
    case State::Confirming:
        return "Confirming";
    case State::Waiting:
        return "Waiting";
    }
    return "?";
}

bool HasPending() {
    return gPendingSeq.load() != 0 || GetStateLocal() != State::Idle;
}

bool IsMigrateInFlight() {
    const State st = GetStateLocal();
    if (st == State::Waiting && gWatchDisconnect) return true;
    // A8=0 仍停 Confirming：包可能已出门、会话已抖，KickSniff 同样不得抢 soft。
    if (st == State::Confirming && gFireAttempt > 0) return true;
    return false;
}

void AbortHopForHangup() {
    const State st = GetStateLocal();
    const uint32_t pend = gPendingSeq.exchange(0);
    const uint32_t active = gActiveSeq.load();
    const bool held = gCombatPaused.load(std::memory_order_acquire) || gResumeAt != 0 ||
                     gHopFailRecoverPending;
    if (st == State::Idle && pend == 0 && !held) return;
    // 包已出门：hangup CloseSession 会撕迁频。粘目标频，避免 C97 粘回挤的旧频。
    if (st == State::Waiting && gTargetChannel >= 0 &&
        (gWatchDisconnect || gSawConnecting || gSawLeavePlay || gExclArmed)) {
        auto_enter::NoteStickyChannel(gTargetChannel, "hangup_preempt_hop");
        Log("abort hangup sticky targetIdx=%d targetCh=%d seq=%u", gTargetChannel,
            DispCh(gTargetChannel), active);
    }
    if (st != State::Idle) {
        gActiveSeq.store(0);
        gSoftReloginHop = false;
        ClearAttemptState();
        SetState(State::Idle);
        Log("abort hangup seq=%u was=%u (hangup preempts hop)", active,
            static_cast<unsigned>(st));
    }
    gResumeAt = 0;
    gEarlyHoldFromRequest.store(false, std::memory_order_release);
    ClearHopFailRecover("hangup");
    ResumeCombatAfterHop();
    ports::teleport::ClearNativeSelfCd();
    if (pend) Log("drop pending seq=%u hangup preempt", pend);
}

DWORD CooldownRemainingMs() {
    const DWORD now = GetTickCount();
    if (now >= gCooldownUntil) return 0;
    return gCooldownUntil - now;
}

void Tick(DWORD now) {
    UpdatePlayReadyClock(now);
    if (x::features::travel::IsActive()) {
        AbortHopForTravel();
        return;
    }
    TickPostHopResume(now);
    TickHopFailRecover(now);

    // 持闸整段（活跃换频 / 换后静默 / Request 边沿）每拍重钉，防其它模块误清位后重新出刀。
    if (gCombatPaused.load(std::memory_order_acquire) || gResumeAt != 0 ||
        gHopFailRecoverPending || GetStateLocal() != State::Idle) {
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::ChannelHop, true);
        gCombatPaused.store(true, std::memory_order_release);
    }

    // 登录场景：先把 known 推进 sticky，再清缓存。
    // soft hold 期间保留 known，供 RequestRestart 再同步（遇人换频后软重连粘旧频根因）。
    {
        const auto ss = ports::world::GetSceneState();
        if (ss == ports::world::SceneState::Login) {
            if (gKnownChannelIdx >= 0) {
                PushStickyFromKnown("pre_login_clear");
                Log("clear knownIdx=%d on Login (sticky preserved) softHold=%d", gKnownChannelIdx,
                    soft_login_probe::IsHoldActive() ? 1 : 0);
            }
            if (!soft_login_probe::IsHoldActive() &&
                !soft_login_probe::IsReconnectInFlight()) {
                // Done 可能发生在仍 Login（WaitLeaveChar）；勿把 SyncKnown 清成 -1，
                // 否则随后冷读 wm6c=sticky → DispCh 把 sticky +1（BIN 02:30 ch.6→7）。
                // 软重连途中 hold 往往还没置上（图内 CloseSession 不 SetHold），
                // 用 inFlight 挡住，否则会清掉刚拉黑的挤频，下一跳又抽回原频。
                const int st = auto_enter::StickyChannel1Based();
                if (st > 0) {
                    gKnownChannelIdx = st;
                    Log("login clear keep known from sticky id=%d ui=%d", st, DispCh(st));
                } else {
                    gKnownChannelIdx = -1;
                }
                gLastRaw68 = -999;
                gLastRaw6c = -999;
                gAdultFlagN = 0;
                gPlayReadySince = 0;
                ClearSoftAvoid("login");
            }
        } else {
            // 图内观测官方 UI 换频；活跃 hop 状态机内跳过（SettleOk 已写 sticky）。
            MaybeObserveNativeChannel(now);
        }
    }

    if (GetStateLocal() == State::Idle) {
        const uint32_t seq = gPendingSeq.load();
        if (seq == 0) {
            // fall through to switch (Idle no-op)
        } else if (const char* defer = DeferReason()) {
            MaybeNotifyDefer(seq, defer, now);
            // 冷却/静默内连点：保留 pending，到期后自动 Begin（勿丢弃，否则体感「点了没触发」）。
            // 仍等满 cooldown 再开火，不缩短冷却窗（BIN 踢号风险）。
            // 静默已结束（仅剩冷却）则放刀；勿因排队把战斗停到冷却满。
            if ((std::strcmp(defer, "冷却中") == 0 || std::strcmp(defer, "换频静默") == 0) &&
                gResumeAt == 0 &&
                gEarlyHoldFromRequest.exchange(false, std::memory_order_acq_rel)) {
                gFireReadyAt.store(0, std::memory_order_release);
                ResumeCombatAfterHop();
                Log("hold pending seq=%u during %s (combat resume, fire after cool)", seq, defer);
            }
        } else if (x::features::travel::IsActive()) {
            gPendingSeq.store(0);
            ResumeCombatAfterHop();
            ports::teleport::ClearNativeSelfCd();
            Log("begin skip seq=%u travel active", seq);
        } else {
            BeginActive(seq, now);
        }
    }

    switch (GetStateLocal()) {
    case State::Selecting:
        TickSelecting(now);
        break;
    case State::Confirming:
        TickConfirming(now);
        break;
    case State::Waiting:
        TickWaiting(now);
        break;
    case State::Idle:
    default:
        break;
    }
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE t = CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD {
            Log("worker start");
            while (!gWorkerStop.load()) {
                Tick(GetTickCount());
                Sleep(kTickMs);
            }
            Log("worker stop");
            return 0;
        },
        nullptr, 0, nullptr);
    if (!t) {
        Log("StartWorker CreateThread failed");
        return;
    }
    gWorkerThread.store(t);
}

void StopWorker() {
    // Signal only — never WaitForSingleObject (DllMain DETACH / loader lock).
    gWorkerStop.store(true);
    HANDLE t = gWorkerThread.exchange(nullptr);
    if (t) CloseHandle(t);
}

}  // namespace x::features::channel_hop
