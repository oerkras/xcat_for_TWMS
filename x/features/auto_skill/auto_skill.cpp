// auto_skill — Classic TWMS 自动加技能点。
// 技能书跟转职走：新手只有 SkillRoot 0（冒險之技）。1/2 转独立开关、独立 ExtendSp 池。
// 已 2/3/4 转仍可加 1/2 转剩余点。未转职只等。不加 3 转书。
// 不要拿新手 CharacterStat.sp 去点 1/2 转书。只发当前书内技能（skillId/10000 == SkillRootID）。
// 前置未满足 / 读不到满级：跳过该技能，不得把另一本书的开关一起关掉。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "auto_skill.h"

#include "../../ipc/payload_auto_skill.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"
#include "../auto_lie/auto_lie.h"
#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../travel/travel.h"

#include "xcat_auto_skill.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace x::features::auto_skill {
namespace {

constexpr DWORD kCooldownMs = 900;
constexpr DWORD kPollMs = 400;
constexpr DWORD kIdleSleepMs = 500;
constexpr DWORD kActiveSleepMs = 50;
constexpr DWORD kJobWaitMs = 800;
constexpr int kVerifyRetry = 2;
constexpr int kMaxSkillLevelCap = 60;
constexpr int kExclTypeSkillUp = 500;
constexpr DWORD kSkipMs = 12000;
constexpr int kSkipCap = static_cast<int>(xcat::kAutoSkillOrderMax) * 2;
constexpr int kEntryMissCap = 2;
constexpr uint32_t kPersistLockMs = 0;

constexpr char kWmClassHash[] =
    "f05b942aeb569b2c37916e7ee710b3ba74011550adcb611a0b26981331a8321";
constexpr char kSendSpMethodHash[] =
    "faf55114b77847fe946cea5cc340d86c9ad01492f2c554bd61573f50ae5c2f6";
constexpr char kCanSendExclHash[] =
    "c515b8b4fb72ad8196f17b12a56326fd68c041265675ccb2029620aacd4a646";
constexpr char kHashGetMaxLevel[] =
    "a15adc1421992bfbd59d9dc4270e18f12873e4b6a8969d41039bc3aec77de3c";
constexpr char kHashSkillEntry[] =
    "d8dcbebeb55ab45d6b95cf1c860dd92448e9b5510193e0842d5ffbf5eb66ef2";

constexpr uint32_t kRvaSendSp = 0xE2CB30;
constexpr uint32_t kRvaCanSendExcl = 0xDFAED0;
constexpr uint32_t kRvaGetMaxLevel = 0x157BA60;

constexpr size_t kFbLevelDataList = 0x120;
// CharacterStat._extendSp @+0x60；ExtendSp._spSet List<SpSet> @+0x10；SpSet.JobLevel/Sp @+0x10/+0x11。
constexpr size_t kFbCsExtendSp = 0x60;
constexpr size_t kFbExtendSpList = 0x10;
constexpr size_t kFbSpSetJobLevel = 0x10;
constexpr size_t kFbSpSetSp = 0x11;

using FnSendSp = void (*)(void* wm, int skillId, const void* methodInfo);
using FnCanSendExcl = uint8_t (*)(void* wm, int type, const void* a, const void* b);
using FnGetMaxLevel = int (*)(void* self, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer = nullptr;
    void* virtualMethodPointer = nullptr;
};

xcat::AutoSkillConfig gCfg{};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};

void* gKlassWm = nullptr;
MethodInfoHead* gMiSendSp = nullptr;
MethodInfoHead* gMiCanSendExcl = nullptr;
MethodInfoHead* gMiGetMaxLevel = nullptr;
FnSendSp gFnSendSp = nullptr;
FnCanSendExcl gFnCanSendExcl = nullptr;
FnGetMaxLevel gFnGetMaxLevel = nullptr;

uint32_t gLastCharId = 0;
DWORD gLastUseMs = 0;
DWORD gLastPoll = 0;
int gFailStreak = 0;
bool gPendingVerify = false;
int gLastSkillId = 0;
int gLastJobLevel = 0;
int gLevelBefore = 0;
bool gLoggedResolve = false;
bool gLoggedOffs = false;
bool gLieBusy = false;
bool gTravelBusy = false;
bool gLoggedFamily = false;
bool gLoggedWaitBook = false;
bool gNeedPersist = false;
uint64_t gPersistTick = 0;
int gPersistJobLevel = 0;
char gPersistWhy[64]{};

struct SkillSkip {
    int id = 0;
    DWORD until = 0;
};
SkillSkip gSkip[kSkipCap]{};

struct SendJobCtx {
    int skillId = 0;
    int levelBefore = 0;
    int want = 0;
    bool invoked = false;
    bool seh = false;
    bool exclBusy = false;
    bool alreadyMax = false;
    bool noMax = false;
    bool noEntry = false;
    int maxLv = 0;
};

enum class SendResult {
    Transient = 0,
    HardFail = 1,
    Ok = 2,
    SkipMax = 3,
    SkipBlocked = 4,
    SkipNoEntry = 5
};

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

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
        return *(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int ExtendSpCount(void* ext) {
    if (!LooksLikeHeapPtr(ext)) return 0;
    void* list = ReadPtr(ext, kFbExtendSpList);
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const int n = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    return (n > 0 && n < 16) ? n : 0;
}

int ExtendSpGet(void* ext, int jobLevel) {
    if (!LooksLikeHeapPtr(ext) || jobLevel <= 0 || jobLevel > 4) return 0;
    void* list = ReadPtr(ext, kFbExtendSpList);
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const int n = ExtendSpCount(ext);
    if (n <= 0) return 0;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    if (!LooksLikeHeapPtr(items)) return 0;
    const size_t data = x::runtime::il2cpp_container::OffArrayData();
    for (int i = 0; i < n; ++i) {
        void* set = ReadPtr(items, data + static_cast<size_t>(i) * sizeof(void*));
        if (!LooksLikeHeapPtr(set)) continue;
        if (static_cast<int>(ReadU8(set, kFbSpSetJobLevel)) != jobLevel) continue;
        return static_cast<int>(ReadU8(set, kFbSpSetSp));
    }
    return 0;
}

void* CharacterExtendSp() {
    void* cs = x::ui::player::LocalCharacterStat();
    if (!LooksLikeHeapPtr(cs)) return nullptr;
    void* ext = ReadPtr(cs, kFbCsExtendSp);
    return LooksLikeHeapPtr(ext) ? ext : nullptr;
}

// 1/2 转走 ExtendSp.Get(JobLevel)。分池还没建时：
// 已是对应转职 → CharacterStat.sp 就是这本书的点（1 转未 2 转常见）；
// 初心者 → 0，禁止拿新手点去点 1/2 转书。
int RemainSpForJobLevel(int jobLevel, int legacySp, int charJob) {
    void* ext = CharacterExtendSp();
    if (ExtendSpCount(ext) > 0) return ExtendSpGet(ext, jobLevel);
    if (jobLevel == 1 && xcat::AutoSkillIsExplorerJob1(charJob)) {
        return legacySp > 0 ? legacySp : 0;
    }
    if (jobLevel == 2 && xcat::AutoSkillIsExplorerJob2(charJob)) {
        return legacySp > 0 ? legacySp : 0;
    }
    return 0;
}

int DictLevel(void* dict, int skillId) {
    if (!dict || skillId <= 0) return -1;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, x::runtime::il2cpp_container::OffDictEntries());
    if (!entries) return -1;
    const int capacity = ReadI32(entries, x::runtime::il2cpp_container::OffArrayMaxLength());
    if (capacity <= 0) return -1;
    const size_t strides[] = {x::runtime::il2cpp_container::DictEntryStrideIntIntTight(),
                              x::runtime::il2cpp_container::DictEntryStrideIntPtr()};
    const size_t valOffs[] = {x::runtime::il2cpp_container::OffDictEntryValueIntTight(),
                              x::runtime::il2cpp_container::OffDictEntryValueIntAlign()};
    const size_t hashOff = x::runtime::il2cpp_container::OffDictEntryHash();
    const size_t keyOff = x::runtime::il2cpp_container::OffDictEntryKey();
    for (int pass = 0; pass < 2; ++pass) {
        const int n = capacity < 4096 ? capacity : 4096;
        for (int i = 0; i < n; ++i) {
            uint8_t* e =
                x::runtime::il2cpp_container::DictEntryAt(entries, i, strides[pass]);
            if (!e) continue;
            __try {
                if (*reinterpret_cast<int*>(e + hashOff) < 0) continue;
                if (*reinterpret_cast<int*>(e + keyOff) != skillId) continue;
                const int v = *reinterpret_cast<int*>(e + valOffs[pass]);
                if (v >= 0 && v <= kMaxSkillLevelCap) return v;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    return -1;
}

int CurrentSkillLevel(int skillId) {
    void* cd = x::ui::player::LocalCharacterData();
    if (!LooksLikeHeapPtr(cd)) return -1;
    void* rec = ReadPtr(cd, x::ui::player::OffSkillRecord());
    void* recEx = ReadPtr(cd, x::ui::player::OffSkillRecordEx());
    int lv = DictLevel(rec, skillId);
    if (lv >= 0) return lv;
    lv = DictLevel(recEx, skillId);
    if (lv >= 0) return lv;
    if (!LooksLikeHeapPtr(rec) && !LooksLikeHeapPtr(recEx)) return -1;
    return 0;
}

int ListSize(void* list) {
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const int n = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    return n > 0 && n < 256 ? n : 0;
}

int LevelDataListSize(void* entry) {
    void* list = ReadPtr(entry, kFbLevelDataList);
    const int n = ListSize(list);
    return (n > 0 && n <= kMaxSkillLevelCap) ? n : 0;
}

void ResetPending() {
    gPendingVerify = false;
    gLastSkillId = 0;
    gLastJobLevel = 0;
    gLevelBefore = 0;
}

void ResetFail() { gFailStreak = 0; }

void ClearSkips() {
    for (int i = 0; i < kSkipCap; ++i) gSkip[i] = {};
}

bool SkillIsSkipped(int id, DWORD now) {
    if (id <= 0) return false;
    for (int i = 0; i < kSkipCap; ++i) {
        if (gSkip[i].id != id) continue;
        if (gSkip[i].until != 0 && static_cast<int>(now - gSkip[i].until) < 0) return true;
    }
    return false;
}

void MarkSkillSkip(int id, DWORD now, const char* why) {
    if (id <= 0) return;
    int slot = 0;
    for (int i = 0; i < kSkipCap; ++i) {
        if (gSkip[i].id == id) {
            slot = i;
            break;
        }
        if (gSkip[i].id == 0 || gSkip[i].until == 0 || static_cast<int>(now - gSkip[i].until) >= 0) {
            slot = i;
        }
    }
    gSkip[slot].id = id;
    gSkip[slot].until = now + kSkipMs;
    x::runtime::LogWThrottled(243, 4000, "AutoSkill", "跳过 skill=%d（%s），%u ms 后再试", id,
                             why ? why : "?", static_cast<unsigned>(kSkipMs));
}

void TryPersistDisabled() {
    if (!gNeedPersist) return;
    xcat::AutoSkillConfig disk{};
    if (gPersistTick != 0 && xcat::ReadAutoSkill(x::runtime::GetBinDir(), disk) &&
        disk.writeTickMs > gPersistTick) {
        gNeedPersist = false;
        return;
    }
    gCfg.writeTickMs = GetTickCount64();
    gPersistTick = gCfg.writeTickMs;
    if (!xcat::WriteAutoSkill(x::runtime::GetBinDir(), gCfg, kPersistLockMs)) {
        x::runtime::LogW("AutoSkill", "停 %d 转写盘失败 (%s)，将重试", gPersistJobLevel,
                         gPersistWhy[0] ? gPersistWhy : "?");
        return;
    }
    gNeedPersist = false;
    x::runtime::LogW("AutoSkill", "已关闭自动加技能点（%d 转发包异常，%s）", gPersistJobLevel,
                     gPersistWhy[0] ? gPersistWhy : "?");
}

void PersistDisabledBook(int jobLevel, const char* why) {
    gCfg.enabled = 0;
    ResetPending();
    gPersistJobLevel = jobLevel;
    gPersistWhy[0] = 0;
    if (why && why[0]) {
        strncpy_s(gPersistWhy, why, _TRUNCATE);
    }
    gNeedPersist = true;
    gPersistTick = 0;
    TryPersistDisabled();
}

bool ResolveSendOnMain() {
    const bool haveSend = gFnSendSp && gMiSendSp;
    const bool haveExcl = gFnCanSendExcl && gMiCanSendExcl;
    const bool haveMax = gFnGetMaxLevel && gMiGetMaxLevel;
    if (haveSend && haveExcl && haveMax) return true;
    if (!x::runtime::il2cpp::Ensure()) return haveSend;

    if (!gKlassWm) {
        gKlassWm = x::runtime::il2cpp::FindClass("", kWmClassHash);
        if (!gKlassWm) gKlassWm = x::runtime::il2cpp::FindClass("", "WorldManager");
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kSend{1, TypeKind::Void, false, true, {TypeKind::I32}};
    constexpr MethodShape kExcl{2, TypeKind::Bool, true, true, {TypeKind::I32, TypeKind::Any}};
    constexpr MethodShape kMax{0, TypeKind::I32, false, true, {}};

    if (gKlassWm && (!haveSend || !haveExcl)) {
        if (!haveSend) {
            const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
                gKlassWm, kRvaSendSp, kSend, nullptr, kSendSpMethodHash);
            if (mr.method) {
                gMiSendSp = reinterpret_cast<MethodInfoHead*>(mr.method);
                if (gMiSendSp && gMiSendSp->methodPointer) {
                    gFnSendSp = reinterpret_cast<FnSendSp>(gMiSendSp->methodPointer);
                }
            }
        }
        if (!haveExcl) {
            const auto ex = x::runtime::il2cpp_method::FindMethodResolved(
                gKlassWm, kRvaCanSendExcl, kExcl, "CanSendExclRequest", kCanSendExclHash);
            if (ex.method) {
                gMiCanSendExcl = reinterpret_cast<MethodInfoHead*>(ex.method);
                if (gMiCanSendExcl && gMiCanSendExcl->methodPointer) {
                    gFnCanSendExcl = reinterpret_cast<FnCanSendExcl>(gMiCanSendExcl->methodPointer);
                }
            }
        }
    }
    if (!haveMax) {
        void* se = x::runtime::il2cpp::FindClass("", kHashSkillEntry);
        if (se) {
            const auto mx = x::runtime::il2cpp_method::FindMethodResolved(
                se, kRvaGetMaxLevel, kMax, "GetMaxLevel", kHashGetMaxLevel);
            if (mx.method) {
                gMiGetMaxLevel = reinterpret_cast<MethodInfoHead*>(mx.method);
                if (gMiGetMaxLevel && gMiGetMaxLevel->methodPointer) {
                    gFnGetMaxLevel = reinterpret_cast<FnGetMaxLevel>(gMiGetMaxLevel->methodPointer);
                }
            }
        }
    }
    if (!gFnSendSp || !gMiSendSp) {
        gFnSendSp = nullptr;
        gMiSendSp = nullptr;
        return false;
    }
    if (!gFnCanSendExcl || !gMiCanSendExcl) {
        gFnCanSendExcl = nullptr;
        gMiCanSendExcl = nullptr;
    }
    if (!gFnGetMaxLevel || !gMiGetMaxLevel) {
        gFnGetMaxLevel = nullptr;
        gMiGetMaxLevel = nullptr;
    }

    if (!gLoggedResolve) {
        gLoggedResolve = true;
        x::runtime::LogI("AutoSkill", "SendSp fn=%p MI=%p wmKlass=%p rva=0x%X excl=%p max=%p",
                         reinterpret_cast<void*>(gFnSendSp), static_cast<void*>(gMiSendSp), gKlassWm,
                         kRvaSendSp, reinterpret_cast<void*>(gFnCanSendExcl),
                         reinterpret_cast<void*>(gFnGetMaxLevel));
    }
    return true;
}

int CallGetMaxLevel(void* entry) {
    if (!LooksLikeHeapPtr(entry)) return 0;
    int mx = 0;
    if (gFnGetMaxLevel && gMiGetMaxLevel) {
        __try {
            mx = gFnGetMaxLevel(entry, gMiGetMaxLevel);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("auto_skill.GetMaxLevel");
            mx = 0;
        }
    }
    if (mx <= 0 || mx > kMaxSkillLevelCap) mx = LevelDataListSize(entry);
    return (mx > 0 && mx <= kMaxSkillLevelCap) ? mx : 0;
}

void SendJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<SendJobCtx*>(user);
    if (!ctx) return;
    ctx->invoked = false;
    ctx->seh = false;
    ctx->exclBusy = false;
    ctx->alreadyMax = false;
    ctx->noMax = false;
    ctx->noEntry = false;
    ctx->maxLv = 0;
    __try {
        if (!ResolveSendOnMain() || !gFnSendSp || !gMiSendSp) return;
        void* wm = x::features::ports::world::PeekWorldManager();
        if (!wm) wm = x::features::ports::world::GetWorldManager();
        if (!LooksLikeHeapPtr(wm)) return;
        if (!gFnCanSendExcl || !gMiCanSendExcl) {
            ctx->exclBusy = true;
            return;
        }
        const uint8_t ok = gFnCanSendExcl(wm, kExclTypeSkillUp, nullptr, nullptr);
        if (!ok) {
            ctx->exclBusy = true;
            return;
        }
        void* entry = x::features::ports::skill::GetSkillEntry(ctx->skillId);
        if (!LooksLikeHeapPtr(entry)) {
            ctx->noEntry = true;
            return;
        }
        ctx->maxLv = CallGetMaxLevel(entry);
        const int live = CurrentSkillLevel(ctx->skillId);
        ctx->levelBefore = live;
        if (live < 0) return;
        if (ctx->maxLv <= 0) {
            ctx->noMax = true;
            return;
        }
        if (live >= ctx->maxLv) {
            ctx->alreadyMax = true;
            return;
        }
        if (ctx->want > 0 && live >= ctx->want) {
            ctx->alreadyMax = true;
            return;
        }
        gFnSendSp(wm, ctx->skillId, gMiSendSp);
        ctx->invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread("auto_skill.SendSp");
        ctx->seh = true;
        ctx->invoked = false;
        x::runtime::LogW("AutoSkill", "SendSp SEH skill=%d", ctx->skillId);
    }
}

SendResult SendSp(int skillId, int levelBefore, int want) {
    if (skillId <= 0) return SendResult::Transient;
    SendJobCtx ctx{};
    ctx.skillId = skillId;
    ctx.levelBefore = levelBefore;
    ctx.want = want;
    if (!x::runtime::main_thread::InvokeAndWait(&SendJobOnMain, &ctx, kJobWaitMs,
                                                x::runtime::main_thread::JobPrio::Low)) {
        x::runtime::LogWThrottled(240, 3000, "AutoSkill", "SendSp pump timeout/reject id=%d",
                                  skillId);
        return SendResult::Transient;
    }
    gLevelBefore = ctx.levelBefore;
    if (ctx.seh) return SendResult::HardFail;
    if (ctx.exclBusy) {
        x::runtime::LogWThrottled(241, 4000, "AutoSkill", "excl busy，跳过本拍 id=%d", skillId);
        return SendResult::Transient;
    }
    if (ctx.alreadyMax) return SendResult::SkipMax;
    if (ctx.noMax) return SendResult::SkipBlocked;
    if (ctx.noEntry) return SendResult::SkipNoEntry;
    if (!ctx.invoked) {
        x::runtime::LogWThrottled(242, 3000, "AutoSkill", "SendSp resolve miss id=%d", skillId);
        return SendResult::Transient;
    }
    return SendResult::Ok;
}

enum class SpendKind { Idle = 0, Sent = 1, Blocked = 2 };

SpendKind TrySpendBook(DWORD now, int bookJob, int jobLevel, const int32_t* order,
                       const int32_t* tgt, uint32_t n, int remain) {
    if (bookJob <= 0 || jobLevel <= 0 || !order || n == 0 || remain <= 0) return SpendKind::Idle;
    int entryMiss = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const int id = order[i];
        if (id <= 0) continue;
        if (xcat::AutoSkillBookJobOfSkill(id) != bookJob) continue;
        if (SkillIsSkipped(id, now)) continue;
        const int lv = CurrentSkillLevel(id);
        if (lv < 0) return SpendKind::Blocked;
        const int want = tgt ? tgt[i] : 0;
        if (want > 0 && lv >= want) continue;
        gLastUseMs = now;
        gLastSkillId = id;
        gLastJobLevel = jobLevel;
        gLevelBefore = lv;
        const SendResult sent = SendSp(id, lv, want);
        if (sent == SendResult::SkipMax) {
            gLastUseMs = 0;
            ResetPending();
            entryMiss = 0;
            continue;
        }
        if (sent == SendResult::SkipBlocked) {
            gLastUseMs = 0;
            ResetPending();
            MarkSkillSkip(id, now, "读不到满级，不发包");
            entryMiss = 0;
            continue;
        }
        if (sent == SendResult::SkipNoEntry) {
            gLastUseMs = 0;
            ResetPending();
            ++entryMiss;
            x::runtime::LogWThrottled(244, 4000, "AutoSkill",
                                      "无 SkillEntry id=%d，试下一个（连miss=%d）", id, entryMiss);
            if (entryMiss >= kEntryMissCap) return SpendKind::Blocked;
            continue;
        }
        if (sent == SendResult::Ok) {
            gPendingVerify = true;
            x::runtime::LogI("AutoSkill",
                             "+1 skill=%d（remain=%d lv=%d want=%d jobLv=%d book=%d）", id, remain,
                             lv, want, jobLevel, bookJob);
            return SpendKind::Sent;
        }
        ResetPending();
        if (sent == SendResult::Transient) return SpendKind::Blocked;
        PersistDisabledBook(jobLevel, "发包异常");
        return SpendKind::Blocked;
    }
    return SpendKind::Idle;
}

void Tick(DWORD now) {
    if (!gCfg.enabled) return;
    if (!xcat::AutoSkillReady(gCfg)) return;
    if (!x::features::ports::world::IsPlayReady()) return;
    // SendSp 与进门同一把 WM 独占锁 type=500。加点让路后技能点不能接着占锁。
    if (x::features::travel::IsActive()) {
        if (!gTravelBusy) {
            gTravelBusy = true;
            ResetPending();
            x::runtime::LogI("AutoSkill", "赶路中，暂停加技能点（独占锁让给发门）");
        }
        return;
    }
    if (gTravelBusy) {
        gTravelBusy = false;
        gLastUseMs = now;
        x::runtime::LogI("AutoSkill", "赶路结束，恢复加技能点");
        return;
    }
    if (auto_lie::IsBusy()) {
        if (!gLieBusy) {
            gLieBusy = true;
            ResetPending();
            x::runtime::LogI("AutoSkill", "测谎中，暂停加技能点");
        }
        return;
    }
    if (gLieBusy) {
        gLieBusy = false;
        gLastUseMs = now;
        x::runtime::LogI("AutoSkill", "测谎结束，恢复加技能点");
        return;
    }
    if (gLastPoll && static_cast<int>(now - gLastPoll) < static_cast<int>(kPollMs)) return;
    gLastPoll = now;
    if (gLastUseMs && static_cast<int>(now - gLastUseMs) < static_cast<int>(kCooldownMs)) return;

    x::ui::player::BaseSpStats st{};
    if (!x::ui::player::ReadBaseSpStats(st) || !st.ok) return;

    if (!gLoggedOffs) {
        gLoggedOffs = true;
        void* ext = CharacterExtendSp();
        x::runtime::LogI("AutoSkill", "read ok cid=%u lv=%d job=%d sp=%d ext1=%d ext2=%d extN=%d",
                         st.characterId, st.level, st.job, st.sp, ExtendSpGet(ext, 1),
                         ExtendSpGet(ext, 2), ExtendSpCount(ext));
    }

    if (st.characterId != 0 && gLastCharId != 0 && st.characterId != gLastCharId) {
        ResetPending();
        ResetFail();
        ClearSkips();
        gLoggedFamily = false;
        gLoggedWaitBook = false;
        x::runtime::LogI("AutoSkill", "换角色 cid %u → %u", gLastCharId, st.characterId);
    }
    if (st.characterId != 0) gLastCharId = st.characterId;

    if (st.job >= 430 && st.job <= 434) {
        if (!gLoggedFamily) {
            gLoggedFamily = true;
            x::runtime::LogW("AutoSkill", "双刀不在范围内 charJob=%d，跳过", st.job);
        }
        return;
    }
    if (!xcat::AutoSkillIsExplorerAdvancement(st.job)) {
        if (!gLoggedWaitBook) {
            gLoggedWaitBook = true;
            x::runtime::LogI("AutoSkill", "初心者或非冒险家五职（job=%d lv=%d），不发包", st.job,
                             st.level);
        }
        return;
    }

    const int family = xcat::AutoSkillJobFamily(st.job);
    int cfgFamily = 0;
    if (gCfg.job1Enabled && xcat::AutoSkillJob1Configured(gCfg)) {
        cfgFamily = xcat::AutoSkillJobFamily(gCfg.job1);
    } else if (gCfg.job2Enabled && xcat::AutoSkillJob2Configured(gCfg)) {
        cfgFamily = xcat::AutoSkillJobFamily(gCfg.job2);
    }
    if (family != 0 && cfgFamily != 0 && family != cfgFamily) {
        if (!gLoggedFamily) {
            gLoggedFamily = true;
            x::runtime::LogW("AutoSkill", "职业族对不上 charJob=%d cfg=%d/%d，跳过", st.job,
                             gCfg.job1, gCfg.job2);
        }
        return;
    }

    if (gPendingVerify) {
        const int cur = CurrentSkillLevel(gLastSkillId);
        if (cur < 0) {
            gLastUseMs = now;
            return;
        }
        if (cur > gLevelBefore) {
            gPendingVerify = false;
            ResetFail();
        } else {
            ++gFailStreak;
            x::runtime::LogW("AutoSkill", "上次 +%d 后等级未 +1（%d→%d）streak=%d jobLv=%d",
                             gLastSkillId, gLevelBefore, cur, gFailStreak, gLastJobLevel);
            if (gFailStreak < kVerifyRetry) {
                gLastUseMs = now;
                return;
            }
            MarkSkillSkip(gLastSkillId, now, "前置未满足或服务器拒包");
            ResetPending();
            ResetFail();
        }
    }

    if (st.level < 10) return;

    int remain1 = 0;
    int remain2 = 0;
    if (gCfg.job1Enabled && xcat::AutoSkillJob1Configured(gCfg) &&
        family == xcat::AutoSkillJobFamily(gCfg.job1)) {
        remain1 = RemainSpForJobLevel(1, st.sp, st.job);
        const SpendKind r =
            TrySpendBook(now, gCfg.job1, 1, gCfg.job1Order, gCfg.job1Target, gCfg.job1Count, remain1);
        if (r != SpendKind::Idle) return;
    }
    if (gCfg.job2Enabled && xcat::AutoSkillJob2Configured(gCfg) &&
        xcat::AutoSkillSameJob2Branch(st.job, gCfg.job2)) {
        remain2 = RemainSpForJobLevel(2, st.sp, st.job);
        const SpendKind r =
            TrySpendBook(now, gCfg.job2, 2, gCfg.job2Order, gCfg.job2Target, gCfg.job2Count, remain2);
        if (r != SpendKind::Idle) return;
    }
    if (remain1 <= 0 && remain2 <= 0) {
        void* ext = CharacterExtendSp();
        x::runtime::LogWThrottled(245, 8000, "AutoSkill",
                                  "没有可花的技能点 job=%d lv=%d sp=%d remain1=%d remain2=%d extN=%d",
                                  st.job, st.level, st.sp, remain1, remain2, ExtendSpCount(ext));
    }
}

DWORD WINAPI WorkerProc(void*) {
    timeBeginPeriod(1);
    x::runtime::LogI("AutoSkill", "worker start");
    DWORD lastCfgPoll = 0;
    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        const bool want = gCfg.enabled != 0 && xcat::AutoSkillReady(gCfg);
        const DWORD cfgGap = want ? kPollMs : kIdleSleepMs;
        if (!lastCfgPoll || static_cast<int>(now - lastCfgPoll) >= static_cast<int>(cfgGap)) {
            lastCfgPoll = now;
            x::ipc::PayloadAutoSkill_Poll();
        }
        TryPersistDisabled();
        if (!(gCfg.enabled != 0 && xcat::AutoSkillReady(gCfg))) {
            Sleep(kIdleSleepMs);
            continue;
        }
        Tick(GetTickCount());
        Sleep(kActiveSleepMs);
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    xcat::AutoSkillSetDefaults(gCfg);
    ResetPending();
    ResetFail();
    ClearSkips();
    gLastCharId = 0;
    gLastUseMs = 0;
    gLastPoll = 0;
    gKlassWm = nullptr;
    gMiSendSp = nullptr;
    gMiCanSendExcl = nullptr;
    gMiGetMaxLevel = nullptr;
    gFnSendSp = nullptr;
    gFnCanSendExcl = nullptr;
    gFnGetMaxLevel = nullptr;
    gLoggedResolve = false;
    gLoggedOffs = false;
    gLieBusy = false;
    gTravelBusy = false;
    gLoggedFamily = false;
    gLoggedWaitBook = false;
    gNeedPersist = false;
    gPersistTick = 0;
    gPersistJobLevel = 0;
    gPersistWhy[0] = 0;
    x::runtime::LogI("Feature", "auto_skill ready (off until enabled + order)");
}

void Shutdown() { StopWorker(); }

void ApplyConfig(const xcat::AutoSkillConfig& cfg) {
    xcat::AutoSkillConfig incoming = cfg;
    xcat::AutoSkillNormalize(incoming);
    if (gNeedPersist) {
        if (gPersistTick != 0 && incoming.writeTickMs > gPersistTick) {
            gNeedPersist = false;
        } else {
            incoming.enabled = 0;
        }
    }
    const bool was = gCfg.enabled != 0;
    const uint32_t was1 = gCfg.job1Enabled;
    const uint32_t was2 = gCfg.job2Enabled;
    gCfg = incoming;
    xcat::AutoSkillNormalize(gCfg);
    const bool on = gCfg.enabled != 0;
    if (was != on || was1 != gCfg.job1Enabled || was2 != gCfg.job2Enabled) {
        ResetPending();
        ResetFail();
        x::runtime::LogI("AutoSkill", "开关=%s j1=%d/%u j2=%d/%u n1=%u n2=%u", on ? "开" : "关",
                         gCfg.job1, gCfg.job1Enabled, gCfg.job2, gCfg.job2Enabled, gCfg.job1Count,
                         gCfg.job2Count);
    }
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE h = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    gWorkerThread.store(h);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

}  // namespace x::features::auto_skill
