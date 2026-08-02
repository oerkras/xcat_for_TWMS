// Classic TWMS — random channel hop: NO UI.
// Read WorldManager._channelID / _adultChannel.Count, then Field.SendTransferChannelRequest.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "channel_hop.h"

#include "../auto_lie/auto_lie.h"
#include "../ccu/ccu.h"
#include "../encounter/encounter.h"
#include "../kick_sniff/kick_sniff.h"
#include "../notify/notify.h"
#include "../ports/player_combat_port.h"
#include "../ports/world_port.h"
#include "../simple_combat/simple_combat.h"
#include "../../runtime/il2cpp_bind.h"
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

// Field.SendTransferChannelRequest(int) — TW dump.cs.restored
constexpr uint32_t kRvaSendTransferChannelRequest = 0xBB5200;  // remapped 2026-08-03
// User 短 IsAlertMode — CanPerformAction callee；读 LocalUser+0x114
constexpr uint32_t kRvaIsAlertMode = 0x12405C0;  // fixed 2026-08-03 (was wrongly 0x1242770/+0xC8)
// WorldManager.CanSendExclRequest — SendTransfer 发包前门控（BIN：未过则无 op=44）
constexpr uint32_t kRvaCanSendExclRequest = 0xDD7C10;  // IDA 2026-08-03
constexpr int kExclTypeTransferChannel = 500;          // SendTransfer 传入的 type（常量解混淆）

constexpr char kFieldClass[] =
    "f64c0f047e0d8cbf609bae658274d559e23bf0fef696f7f14bce93dbf6c3d2a";
// User 父类（短 IsAlertMode 宿主；非 UserLocal）
constexpr char kUserAlertClass[] =
    "fb641b6ed2c6220bf18c3f3c2f8a20b4f3e53702c3307c50c75c85dd2a2ef06";
constexpr char kHashSendTransfer[] =
    "c8432537e031443dc368d993b71c89d250d37178fda202b0e5e9f33484e38a7";
constexpr char kHashIsAlertMode[] =
    "f74b63202db3140d4b5918a8fd68733266491ef9287abfb0125260b485a10f0";
constexpr char kHashCanSendExcl[] =
    "b11b7f0d3ee12d95d993fca92ec1dcab807326397b5167cb7792d06380960ab";

// WorldManager TW fields (docs/features/world_manager/字段全表.md)
// SendTransferChannelRequest(int nIdx)：0-based；游戏 UI「ch.N」= nIdx+1。
// BIN：WM+0x68 换频后常卡死；+0x6C 更跟真实频；成功后再用 gKnownChannelIdx。
constexpr size_t kOffWmChannelId = 0x68;     // _channelID（0-based；可能陈旧）
constexpr size_t kOffWmChannelAlt = 0x6C;    // TW 旁邻 int（BIN：换频后常追上目标）
constexpr size_t kOffWmAdultChannel = 0x78;  // List<bool> _adultChannel → Count = 频道数
// excl（字段全表 + IDA CanSendExcl）：+0x98 是 CharacterId，勿当 excl
constexpr size_t kOffWmExclA0 = 0xA0;
constexpr size_t kOffWmExclA4 = 0xA4;
constexpr size_t kOffWmExclFlagA8 = 0xA8;  // CanSendExcl: movzx/mov [rdi+0A8h]
constexpr size_t kOffWmExclFlagA9 = 0xA9;
constexpr size_t kOffWmExclAC = 0xAC;
constexpr size_t kOffListItems = 0x10;  // IL2CPP List._items
constexpr size_t kOffListSize = 0x18;   // IL2CPP List._size
constexpr size_t kOffArrData = 0x20;    // Il2CppArray vector
constexpr int kAdultFlagCap = 128;

// KickSniff SessionState：Disconnecting=0 Disconnected=1 Connecting=2 Connected=3
constexpr int kSessDisconnecting = 0;
constexpr int kSessDisconnected = 1;

constexpr DWORD kTickMs = 80;
constexpr DWORD kJobWaitMs = 2500;
constexpr DWORD kWaitMigrateMs = 12000;
constexpr DWORD kWaitAlertMs = 20000;       // 警戒解除最长等待
constexpr DWORD kWaitExclMs = 20000;        // excl 独占解除最长等待
constexpr DWORD kAlertSampleMs = 200;
constexpr DWORD kExclSampleMs = 200;
constexpr DWORD kLandGraceMs = 4000;        // 进图后门控宽限，避免加载瞬间误发挂起 seq
constexpr DWORD kCooldownAfterOkMs = 30000;  // BIN：成功后再 hop ~20s 曾硬断；成功后加长冷却
constexpr DWORD kCooldownAfterFailMs = 3000;
constexpr DWORD kSettleReadyMs = 1500;     // 未离图时的最短观察
constexpr DWORD kStayConfirmMs = 1200;     // 从未离图且仍原频 → 才判拒绝
constexpr DWORD kPostLandGraceMs = 2500;   // 离图再落地后：等频道号追上
constexpr int kChannelCountFallbackMax = 128;  // Classic 可达 ~60 频；原 40 会误杀
constexpr int kMaxFireAttempts = 3;            // 含首次；满人/未进则换其它频重试

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
int gTargetChannel = -1;
int gFromChannel = -1;
int gChannelCount = 0;
int gFireAttempt = 0;
int gTried[128]{};
int gTriedN = 0;
DWORD gStaySince = 0;      // 从未离图且连续仍停原频
DWORD gLandedAt = 0;       // 离图后再进 PlayReady 的时刻；0=本轮未落地过
bool gSawLeavePlay = false;  // 发包后是否见过 !IsPlayReady（黑屏/InterStage）
bool gWasPlayReady = true;
int gKnownChannelIdx = -1;  // 上次成功换频后的 0-based 索引；跨 job 保留
bool gCombatPaused = false;   // 本模块暂停了 simple_combat
bool gAlertNotified = false;
DWORD gAlertSampleAt = 0;
bool gAlertCached = false;
bool gExclNotified = false;
DWORD gExclSampleAt = 0;
bool gExclCachedOk = true;
DWORD gPlayReadySince = 0;  // Idle 挂起时进图起点；0=未就绪
bool gWatchDisconnect = false;  // Waiting：盯 KickSniff 硬断（Connecting 是正常迁频）
int gLastRaw68 = -999;          // ReadInfo 前进检测；Login/Init 清
int gLastRaw6c = -999;
uint8_t gAdultFlag[kAdultFlagCap]{};  // WM List<bool> 快照；1=成人频
int gAdultFlagN = 0;
void* gKlassField = nullptr;
uint32_t gLastDeferNotifySeq = 0;

// UI / toast：0-based idx → 玩家看到的 ch.N
int DispCh(int idx) { return idx >= 0 ? idx + 1 : idx; }

struct JobCtx {
    enum class Kind : int { ReadInfo, FireTransfer, QueryAlert, QueryExcl } kind = Kind::ReadInfo;
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
    void* iter = nullptr;
    for (;;) {
        void* miRaw = nullptr;
        __try {
            miRaw = ex.classGetMethods(klass, &iter);
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
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
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

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (plain) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plain, shape.arity)) return mi;
    }
    if (hash) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hash, shape.arity)) return mi;
    }
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            Log("ResolveMi kind hit rva=0x%X plain=%s", rva, plain ? plain : "-");
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

void EnsureMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (!gKlassField) {
        gKlassField = FindClass("", kFieldClass);
        if (!gKlassField) gKlassField = FindClass("", "Field");
        if (!gKlassField) gKlassField = FindClass("Msc.Scene", "SceneField");
    }
    void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    void* userAlert = FindClass("", kUserAlertClass);
    if (!userAlert) userAlert = x::runtime::il2cpp_shape::ResolveUserLocalKlass();

    if (gKlassField && !gMiSendTransfer) {
        constexpr MethodShape kTr{1, TypeKind::Void, true, true, {TypeKind::I32}};
        gMiSendTransfer = ResolveMi(gKlassField, kRvaSendTransferChannelRequest, kTr,
                                    "SendTransferChannelRequest", kHashSendTransfer);
    }
    if (userAlert && !gMiIsAlertMode) {
        // bool() 不唯一 → 哈希主；walkParents 覆盖 UserLocal→User
        constexpr MethodShape kAl{0, TypeKind::Bool, false, true, {}};
        gMiIsAlertMode =
            ResolveMi(userAlert, kRvaIsAlertMode, kAl, "IsAlertMode", kHashIsAlertMode);
    }
    if (wmKlass && !gMiCanSendExcl) {
        constexpr MethodShape kEx{2, TypeKind::Bool, true, true, {TypeKind::I32, TypeKind::Any}};
        gMiCanSendExcl = ResolveMi(wmKlass, kRvaCanSendExclRequest, kEx, "CanSendExclRequest",
                                   kHashCanSendExcl);
    }
    const int n = (gMiSendTransfer ? 1 : 0) + (gMiIsAlertMode ? 1 : 0) + (gMiCanSendExcl ? 1 : 0);
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
    job->exclA0 = ReadI32(wm, kOffWmExclA0);
    job->exclA4 = ReadI32(wm, kOffWmExclA4);
    job->exclFlagA8 = ReadU8(wm, kOffWmExclFlagA8);
    job->exclFlagA9 = ReadU8(wm, kOffWmExclFlagA9);
    job->exclAC = ReadI32(wm, kOffWmExclAC);
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
    void* adult = ReadPtr(wm, kOffWmAdultChannel);
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
    void* adult = ReadPtr(wm, kOffWmAdultChannel);
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
        job->channelRaw68 = ReadI32(wm, kOffWmChannelId);
        job->channelRaw6c = ReadI32(wm, kOffWmChannelAlt);
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
        if (wm) job->exclFlagA8After = ReadU8(wm, kOffWmExclFlagA8);
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
    if (!x::runtime::main_thread::InvokeAndWait(&JobFn, &job, kJobWaitMs)) {
        strncpy_s(job.err, "job timeout", _TRUNCATE);
        return false;
    }
    return job.ok;
}

int PickRandomChannel(int currentIdx, int count) {
    if (count <= 1) return -1;
    int prefer[128]{};
    int neutral[128]{};
    int avoid[128]{};
    int nPrefer = 0, nNeutral = 0, nAvoid = 0;
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
        if (hint == ccu::ChannelPickHint::Prefer) {
            prefer[nPrefer++] = id;
        } else if (hint == ccu::ChannelPickHint::Avoid) {
            avoid[nAvoid++] = id;
        } else {
            neutral[nNeutral++] = id;
        }
    }
    const int* pool = nullptr;
    int n = 0;
    const char* poolName = "none";
    if (nPrefer > 0) {
        pool = prefer;
        n = nPrefer;
        poolName = "prefer";
    } else if (nNeutral > 0) {
        pool = neutral;
        n = nNeutral;
        poolName = "neutral";
    } else if (nAvoid > 0) {
        pool = avoid;
        n = nAvoid;
        poolName = "avoid";
    }
    if (n <= 0 || !pool) return -1;
    const uint32_t mix =
        GetTickCount() ^ (gActiveSeq.load() * 2654435761u) ^ (static_cast<uint32_t>(gFireAttempt) * 97u);
    const int picked = pool[mix % static_cast<uint32_t>(n)];
    Log("pick-pool %s size=%d (prefer=%d neutral=%d avoid=%d adultSnap=%d) → idx=%d ch=%d",
        poolName, n, nPrefer, nNeutral, nAvoid, gAdultFlagN, picked, DispCh(picked));
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
    gExclNotified = false;
    gExclSampleAt = 0;
    gExclCachedOk = true;
    gWatchDisconnect = false;
}

void PauseCombatForHop() {
    if (gCombatPaused) return;
    simple_combat::SetExternalPause(true);
    gCombatPaused = true;
    Log("combat pause for hop");
}

void ResumeCombatAfterHop() {
    if (!gCombatPaused) return;
    gCombatPaused = false;
    // BIN：遇人仍有人时 ChannelHop Fail/OK 的 resume 会清掉 encounter pause → 继续打怪刷警戒
    if (encounter::HoldsCombatPause()) {
        Log("combat resume skipped (encounter holding pause)");
        return;
    }
    simple_combat::SetExternalPause(false);
    Log("combat resume after hop");
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
    ResumeCombatAfterHop();
    gActiveSeq.store(0);
    ClearAttemptState();
    SetState(State::Idle);
    if (cooldownMs > 0) gCooldownUntil = now + cooldownMs;
}

void Fail(const char* why) {
    Log("fail seq=%u why=%s pending=%u attempts=%d", gActiveSeq.load(), why ? why : "?",
        gPendingSeq.load(), gFireAttempt);
    Notify(notify::NotificationKind::Warning, "manual-rejoin-fail", "随机换频失败",
           why ? why : "未知错误");
    FinishActive(kCooldownAfterFailMs, GetTickCount());
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
    FinishActive(kCooldownAfterOkMs, now);
}

bool TryRetryOtherChannel(const char* why, int stayChannel, DWORD now) {
    MarkTried(gTargetChannel);
    ccu::MarkChannelRejected(gTargetChannel);
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
        "why=%s",
        gActiveSeq.load(), gFireAttempt + 1, kMaxFireAttempts, stay, DispCh(stay), gTargetChannel,
        next, DispCh(next), why ? why : "?");
    char body[96]{};
    snprintf(body, sizeof(body), "ch.%d 进不去，改试 ch.%d（%d/%d）", DispCh(gTargetChannel),
             DispCh(next), gFireAttempt + 1, kMaxFireAttempts);
    Notify(notify::NotificationKind::Info, "manual-rejoin-retry", "随机换频重试", body);
    gTargetChannel = next;
    gStaySince = 0;
    gLandedAt = 0;
    gSawLeavePlay = false;
    gWasPlayReady = true;
    gPhaseAt = now;
    SetState(State::Confirming);
    return true;
}

void SucceedQueued() {
    Log("transfer fired seq=%u fromIdx=%d fromCh=%d toIdx=%d toCh=%d attempt=%d/%d (no UI)",
        gActiveSeq.load(), gFromChannel, DispCh(gFromChannel), gTargetChannel,
        DispCh(gTargetChannel), gFireAttempt, kMaxFireAttempts);
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
    // 正常迁频：Connected→Connecting→Connected；硬断：→Disconnected（BIN seq=18）
    gWatchDisconnect = true;
    SetState(State::Waiting);
    gPhaseAt = GetTickCount();
}

// 每 Tick 更新：进图冷却只约束「刚进图 / 刚落地」窗口，不是「点 F10 才开始计时」。
// BIN：Idle 无 pending 时早期 return，若不持续记时，图里挂很久再 hop 也会误报「进图冷却」。
void UpdatePlayReadyClock(DWORD now) {
    if (!ports::world::IsPlayReady()) {
        gPlayReadySince = 0;
        return;
    }
    if (gPlayReadySince == 0) gPlayReadySince = now;
}

const char* DeferReason() {
    if (!ports::world::IsPlayReady() || gPlayReadySince == 0) return "未进图";
    const DWORD now = GetTickCount();
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
    snprintf(body, sizeof(body), "延后：%s", reason ? reason : "?");
    Notify(notify::NotificationKind::Info, "manual-rejoin-defer", "随机换频排队中", body);
}

void BeginActive(uint32_t seq, DWORD now) {
    gActiveSeq.store(seq);
    // Consume only this seq; keep a newer pending that raced in after Idle check.
    uint32_t expected = seq;
    (void)gPendingSeq.compare_exchange_strong(expected, 0);
    ClearAttemptState();
    PauseCombatForHop();
    gPhaseAt = now;
    gLastDeferNotifySeq = 0;
    SetState(State::Selecting);
    Log("begin seq=%u (direct SendTransfer, no menu)", seq);
}

void TickSelecting(DWORD now) {
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
    SetState(State::Confirming);
    gPhaseAt = GetTickCount();
}

void TickConfirming(DWORD now) {
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
        // 刚脱战：给 excl 门控单独 20s，勿与警戒共用倒计时
        gPhaseAt = now;
    }
    gAlertNotified = false;

    // BIN：CanSendExcl(500)==0 则无 op=44。等独占清，不烧 attempt。
    if (SampleExclBusy(now)) {
        MaybeNotifyExcl(now);
        if (now - gPhaseAt > kWaitExclMs) {
            Fail("独占请求未清，无法换频");
        }
        return;
    }
    gExclNotified = false;

    JobCtx fire{};
    fire.kind = JobCtx::Kind::FireTransfer;
    fire.channelId = gTargetChannel;
    ++gFireAttempt;
    if (!RunJob(fire)) {
        Log("transfer blocked/fail seq=%u targetIdx=%d err=%s A8=%u→%u canSend=%d",
            gActiveSeq.load(), gTargetChannel, fire.err[0] ? fire.err : "?", fire.exclFlagA8,
            fire.exclFlagA8After, fire.exclOk ? 1 : 0);
        // excl busy 不应烧满人重试；回 Confirming 同目标等（attempt 已+1，用下一轮 Sample 挡）
        if (std::strcmp(fire.err, "excl busy") == 0) {
            --gFireAttempt;
            gPhaseAt = now;  // 重新起算等待窗
            return;
        }
        if (!TryRetryOtherChannel(fire.err[0] ? fire.err : "SendTransfer 失败", gFromChannel,
                                  GetTickCount())) {
            // Fail already
        }
        return;
    }
    Log("transfer fired seq=%u targetIdx=%d targetCh=%d attempt=%d A8=%u→%u A0=%d A4=%d AC=%d",
        gActiveSeq.load(), gTargetChannel, DispCh(gTargetChannel), gFireAttempt, fire.exclFlagA8,
        fire.exclFlagA8After, fire.exclA0, fire.exclA4, fire.exclAC);
    if (fire.exclFlagA8 == 0 && fire.exclFlagA8After == 0) {
        Log("warn excl A8 still 0 after SendTransfer — other early-gate may block op=44");
    }
    MarkTried(gTargetChannel);
    SucceedQueued();
}

void TickWaiting(DWORD now) {
    const bool play = ports::world::IsPlayReady();
    const auto ss = ports::world::GetSceneState();

    // BIN seq=18：transfer 后 Connected→Disconnected（非 Connecting）；勿挂死到进程被杀。
    if (gWatchDisconnect) {
        const int sess = kick_sniff::LastSessionState();
        if (sess == kSessDisconnected || sess == kSessDisconnecting) {
            char why[96]{};
            snprintf(why, sizeof(why), "会话已断开 sess=%d（换频未完成）", sess);
            Fail(why);
            return;
        }
        if (ss == ports::world::SceneState::Login) {
            Fail("回到登录（换频断线）");
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
            Fail("换频超时（迁频未回图）");
        }
        return;
    }

    void* wm = ports::world::PeekWorldManager();
    if (!wm) {
        if (now - gPhaseAt > kWaitMigrateMs) Fail("换频后无 WorldManager");
        return;
    }
    const int cur68 = ReadI32(wm, kOffWmChannelId);
    const int cur6c = ReadI32(wm, kOffWmChannelAlt);
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

    // 硬成功：内存频道号已追上目标（0-based）
    if (gTargetChannel >= 0 && (cur6c == gTargetChannel || cur68 == gTargetChannel)) {
        SettleOk(cur6c == gTargetChannel ? "channel_match_6c" : "channel_match_68",
                 cur6c == gTargetChannel ? cur6c : cur68, now);
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
    // 若仍警戒 → 视为警戒拒收：回 Confirming 同目标再发（不换频、不烧满人逻辑）。
    if (SampleAlert(now)) {
        MaybeNotifyAlert(now);
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
        gPhaseAt = now;  // 重新起算警戒等待
        SetState(State::Confirming);
        return;
    }

    if (now - gPhaseAt < kSettleReadyMs + kStayConfirmMs) return;
    char why[96]{};
    snprintf(why, sizeof(why), "未离图判拒收 raw68=%d raw6c=%d fromIdx=%d targetIdx=%d", cur68,
             cur6c, gFromChannel, gTargetChannel);
    (void)TryRetryOtherChannel(why, gFromChannel, now);
}

}  // namespace

void Init() {
    gWorkerStop.store(false);
    SetState(State::Idle);
    gPendingSeq.store(0);
    gActiveSeq.store(0);
    gLastDeferNotifySeq = 0;
    gPlayReadySince = 0;
    gCombatPaused = false;
    gKnownChannelIdx = -1;
    gLastRaw68 = -999;
    gLastRaw6c = -999;
    gAdultFlagN = 0;
    gCooldownUntil = 0;
    gWatchDisconnect = false;
    Log("Init (direct transfer, no UIChannelShift/GameMenu)");
}

void Shutdown() { StopWorker(); }

void RequestManualRejoin(uint32_t seq) {
    if (seq == 0) return;
    if (GetStateLocal() != State::Idle) {
        Log("busy queue seq=%u state=%u", seq, gState.load());
        gPendingSeq.store(seq);
        return;
    }
    gPendingSeq.store(seq);
    Log("request seq=%u", seq);
}

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

DWORD CooldownRemainingMs() {
    const DWORD now = GetTickCount();
    if (now >= gCooldownUntil) return 0;
    return gCooldownUntil - now;
}

void Tick(DWORD now) {
    UpdatePlayReadyClock(now);

    // 登录场景清 known，避免跨角色/重登串缓存
    {
        const auto ss = ports::world::GetSceneState();
        if (ss == ports::world::SceneState::Login) {
            if (gKnownChannelIdx >= 0) {
                Log("clear knownIdx=%d on Login", gKnownChannelIdx);
            }
            gKnownChannelIdx = -1;
            gLastRaw68 = -999;
            gLastRaw6c = -999;
            gAdultFlagN = 0;
            gPlayReadySince = 0;
        }
    }

    if (GetStateLocal() == State::Idle) {
        const uint32_t seq = gPendingSeq.load();
        if (seq == 0) return;
        if (const char* defer = DeferReason()) {
            MaybeNotifyDefer(seq, defer, now);
            return;
        }
        BeginActive(seq, now);
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
