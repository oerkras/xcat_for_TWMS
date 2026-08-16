// char_boot — Classic TWMS 一键起号（v1 法师）。独立编排器：HardPause / Goto / Talk / 等终态。
// 对话点选：FindAll + GameObject.activeSelf。YesNo 走 OnClickBtYes（SetRet=6）。
// 下頁是 type=0 OnClickBtOk（SetRet=1）。桑克斯上船固定 3 拍：Yes + 下頁 + 下頁。
// 最后一页带「上頁/下頁」是 sendNextPrev，再点一下就 warp，没有第四页正文。
// 禁止把 +0x360/+0x370 的 Action 当 Button 存活条件。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "char_boot.h"

#include "../auto_lie/auto_lie.h"
#include "../auto_supply/auto_supply.h"
#include "../channel_hop/channel_hop.h"
#include "../fly/fly.h"
#include "../invuln/invuln.h"
#include "../notify/notify.h"
#include "../ports/shop_port.h"
#include "../ports/attack_input_port.h"
#include "../ports/fly_fh_ban.h"
#include "../ports/foothold_path.h"
#include "../ports/teleport_port.h"
#include "../ports/travel_port.h"
#include "../ports/world_port.h"
#include "../sellbag/sellbag.h"
#include "../simple_combat/heli_rotor.h"
#include "../simple_combat/simple_combat.h"
#include "../travel/travel.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_prefab.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../ui/player_vitals.h"
#include "../../ipc/payload_control.h"
#include "xcat_auto_skill.h"
#include "xcat_auto_stat.h"
#include "xcat_char_boot.h"
#include "xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "winmm.lib")

namespace x::features::char_boot {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

namespace runtime = x::runtime;
namespace notify = x::features::notify;
namespace travel = x::features::travel;
namespace ports = x::features::ports;
namespace shop = x::features::ports::shop;
namespace simple_combat = x::features::simple_combat;
namespace auto_supply = x::features::auto_supply;
namespace sellbag = x::features::sellbag;
namespace auto_lie = x::features::auto_lie;
namespace invuln = x::features::invuln;
namespace channel_hop = x::features::channel_hop;
namespace fly = x::features::fly;
namespace heli = x::features::simple_combat::heli;

constexpr DWORD kTickMs = 100;
constexpr DWORD kApproachTickMs = 16;
constexpr DWORD kStatusMs = 500;
constexpr DWORD kMapStableMs = 400;
constexpr DWORD kGotoRetryMs = 400;
constexpr DWORD kClickGapMs = 400;
constexpr DWORD kTalkGapMs = 3500;
// StickToStand 会堵 1~2s。gLastTalkMs 必须用 GetTickCount，不能用 tick 入口的 now。
// 00:12：贴脸开始 now 已过 2s，Talk 刚完 settle 被当成到期，+107ms 点空壳 Yes 把脚本掐掉。
constexpr DWORD kDlgSettleMs = 2500;
// Yes / 下頁之后要等脚本切页。00:27 在 +422ms 对幽灵 type=0 点 Ok，窗变 0i0、金币未扣。
constexpr DWORD kDlgAfterYesMs = 2000;
// Cosmic/HeavenMS scripts/npc/22000.js：start=sendYesNo，status1=sendNext，
// status2=sendNext/sendNextPrev，status3=扣 150 + warp(104000000)。
// 付费正文：无聊/先给 150 → 確實收到150/出发。推荐函路径也是 Yes+两下頁。
constexpr int kShipNextNeed = 2;
// n=0 时才靠这扇窗等脚本建对象；有 1A 活窗就点，不等 say。
constexpr DWORD kTalkWaitDlgMs = 8000;
constexpr float kTalkRangePx = 200.f;
constexpr float kTalkArrivePx = 160.f;
constexpr float kTalkLeavePx = 280.f;
constexpr DWORD kGotoIslandMs = 180000;
constexpr DWORD kGotoVicMs = 600000;
constexpr DWORD kBoardShipMs = 90000;
constexpr DWORD kJobTalkMs = 90000;
constexpr DWORD kWaitJobMs = 15000;
constexpr DWORD kWaitSpendMs = 20000;
constexpr DWORD kEnterLibMs = 120000;
constexpr DWORD kExitLibMs = 60000;
constexpr DWORD kDoneMs = 2000;
constexpr DWORD kFiredNoWarpMs = 8000;
constexpr int kMaxTalk = 8;
constexpr int kMaxDismiss = 8;
constexpr int kJobMage = 200;
constexpr int kMapHarbor = 60000;
constexpr int kMapVictoria = 104000000;
constexpr int kMapEllinia = 101000000;
constexpr int kMapLib = 101000003;
constexpr int kNpcSancks = 22000;
constexpr int kNpcHans = 1032001;
constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kFbUiDlgType = 0xA0;
constexpr size_t kFbUiDlgMenuTexts = 0xE0;
// get_SayText 读 +0x120，但 BIN 23:57 真窗 dump=1A0：Type/Active 对，string 恒空。
// getter 几乎无 xref，脚本写的是 Unity Text，不写这个 backing field。
constexpr size_t kFbUiDlgSayText = 0x120;
// 0x360/0x370 是 readonly Action 委托，不是 Button。OnClickBtYes 在 +0x370
// 为空时走空 Action 分支；拿它当「窗还活着」会把真 YesNo 滤掉。
constexpr size_t kOffActYes = 0x370;
constexpr size_t kFbUiDlgRet = 0xA4;
constexpr int kUiDlgTypeText = 0;
constexpr int kUiDlgTypeYesNo = 1;
constexpr int kUiDlgTypeList = 4;
constexpr int kDlgScanCap = 32;
constexpr DWORD kDlgScanLogMs = 1500;
constexpr uint32_t kRvaUiDlgSelectMenu = 0x7978c0;
constexpr uint32_t kRvaUiDlgOnClickBtOk = 0x7A02E0;
constexpr uint32_t kRvaUiDlgOnClickBtYes = 0x7A0390;
constexpr uint32_t kRvaFindAll = x::runtime::il2cpp::kRvaFindObjectsOfTypeAll;
constexpr uint32_t kRvaCompGetGo = x::runtime::il2cpp::kRvaCompGetGo;
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E95430;

constexpr char kUiUtilDialogExClass[] =
    "f4640aac0fff8dde1646abb7ec3d20c681502597a5e52c5c9ae46bf94d6b949";
constexpr char kPrefabUtilDialogEx[] = "UIUtilDialogEx";
constexpr char kHashSetKeyFocus[] =
    "f5336ee2ad0ce3cf113df9cddd45720e35fb55a0efc107ab375042f1789d3f3";
constexpr char kHashUiDlgType[] =
    "f20229ee348e34de1da65eeada6aa5706cd64571f2f0e28f409f50cdb8c1254";
constexpr char kHashUiDlgMenuTexts[] =
    "<c0e6e2686767fb10a3bce2976dd8a7bd50455762371360d38f2eb058673cee7>k__BackingField";
constexpr char kHashOnClickBtYes[] =
    "d7ce60b0fab48a779c6e5a31f11fbadc5e90d742d46062befaa4eb6be8d653e";

const char* kShipKeys[] = {"維多利亞", "维多", "乘船", "搭船", "前往", "150", "楓幣", nullptr};
const char* kHansKeys[] = {"魔法師", "法师", "轉職", "转职", "成為", "成为", nullptr};

enum class State : int {
    Idle = 0,
    Arm,
    GotoFarm,
    Farm,
    GotoHarbor,
    BoardShip,
    GotoEllinia,
    EnterLib,
    JobTalk,
    WaitJob,
    WaitSpend,
    ExitLib,
    GotoHangup,
    Done,
};

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

using FnUiDlgOnClickOk = void (*)(void* self, void* mi);
using FnUiDlgSelectMenu = void (*)(void* self, int32_t idx, void* mi);
using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnGetGameObject = void* (*)(void* self, const void* mi);
using FnGoGetActiveSelf = bool (*)(void* self, const void* mi);

struct Trip {
    int farmMap = 40000;
    int hangupMap = 101010000;
    uint32_t departKind = 0;
    uint32_t levelMin = 8;
    uint32_t mesoMin = 1000;
    uint32_t farmTimeoutMin = 0;
};

struct VitalsSnap {
    int mapId = 0;
    int level = 0;
    int job = 0;
    int intel = 0;
    int ap = 0;
    int sp = 0;
    int64_t meso = -1;
    bool playReady = false;
    bool ok = false;
};

std::atomic<HANDLE> gThread{nullptr};
std::atomic<bool> gStop{false};
std::atomic<int> gState{static_cast<int>(State::Idle)};

xcat::CharBootConfig gCfg{};
uint64_t gSeenCfgTick = 0;
uint32_t gHandledManualSeq = 0;
bool gManualSeqBootstrapped = false;

Trip gTrip{};
int gArmChannel = 0;
bool gHangupHanded = false;
bool gDidExitLib = false;
bool gShipFareLocked = false;

char gMsg[160]{};
char gLastWhy[96]{};
DWORD gStateEnterMs = 0;
DWORD gLastTickMs = 0;
DWORD gFrozenSince = 0;
DWORD gFrozenAcc = 0;
DWORD gLastGotoMs = 0;
DWORD gStableSince = 0;
DWORD gLastClickMs = 0;
DWORD gLastTalkMs = 0;
DWORD gLastYesMs = 0;
int gShipNextN = 0;
void* gYesDlg = nullptr;
void* gSayDlg = nullptr;
bool gSawDlgThisTalk = false;
bool gCombatArmed = false;
DWORD gTalkLandSince = 0;
bool gApproachFly = false;
bool gNpcStuck = false;
DWORD gLastStatusMs = 0;
DWORD gLastFarmLogMs = 0;
DWORD gFarmAccMs = 0;
DWORD gFiredAtMs = 0;
DWORD gApZeroSince = 0;
DWORD gSpHoldSince = 0;
int gLastSp = -1;
bool gSkipSkillLogged = false;
int gTalkN = 0;
int gNoNpcN = 0;
int gDismissN = 0;
int gLastJob = 0;

size_t gOffUiDlgType = kFbUiDlgType;
size_t gOffUiDlgMenuTexts = kFbUiDlgMenuTexts;
void* gUiDlgKlass = nullptr;
void* gUiDlgType = nullptr;
MethodInfoHead* gMiUiDlgOnClickOk = nullptr;
MethodInfoHead* gMiUiDlgOnClickYes = nullptr;
MethodInfoHead* gMiUiDlgSelectMenu = nullptr;
MethodInfoHead* gMiGetGameObject = nullptr;
MethodInfoHead* gMiGoGetActiveSelf = nullptr;
DWORD gLastDlgScanLogMs = 0;

void ReturnLeakedMetadataLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
}

State Cur() { return static_cast<State>(gState.load(std::memory_order_acquire)); }

const char* StateName(State s) {
    switch (s) {
    case State::Idle:
        return "Idle";
    case State::Arm:
        return "Arm";
    case State::GotoFarm:
        return "GotoFarm";
    case State::Farm:
        return "Farm";
    case State::GotoHarbor:
        return "GotoHarbor";
    case State::BoardShip:
        return "BoardShip";
    case State::GotoEllinia:
        return "GotoEllinia";
    case State::EnterLib:
        return "EnterLib";
    case State::JobTalk:
        return "JobTalk";
    case State::WaitJob:
        return "WaitJob";
    case State::WaitSpend:
        return "WaitSpend";
    case State::ExitLib:
        return "ExitLib";
    case State::GotoHangup:
        return "GotoHangup";
    case State::Done:
        return "Done";
    default:
        return "?";
    }
}

void SetMsg(const char* m) { strncpy_s(gMsg, m ? m : "", _TRUNCATE); }

bool InFarm(int mapId) { return mapId > 0 && mapId == gTrip.farmMap; }

bool IsMapleIsland(int mapId) { return mapId > 0 && mapId < 100000000; }

void MapKey(int mapId, char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (mapId <= 0) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, cap, "%d", mapId);
}

bool MapMatches(int want, int have) { return want > 0 && have > 0 && want == have; }

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool UnityAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    __try {
        return ReadPtr(obj, kOffCachedPtr) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ListSize(void* list) {
    if (!LooksLikeHeapPtr(list)) return 0;
    return ReadI32(list, x::runtime::il2cpp_container::OffListSize());
}

void* ListAt(void* list, int index) {
    if (!LooksLikeHeapPtr(list) || index < 0) return nullptr;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    if (!LooksLikeHeapPtr(items)) return nullptr;
    const uintptr_t n =
        *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(items) +
                                      x::runtime::il2cpp_container::OffArrayMaxLength());
    if (static_cast<uintptr_t>(index) >= n) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) +
                                     x::runtime::il2cpp_container::OffArrayData() +
                                     static_cast<size_t>(index) * sizeof(void*));
}

bool ReadIl2CppStringUtf8(void* str, char* out, size_t outCap) {
    if (!out || outCap < 2) return false;
    out[0] = 0;
    if (!str) return false;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const auto* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            if (c < 0x80) {
                out[n++] = static_cast<char>(c);
            } else if (c < 0x800 && n + 2 < outCap) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            } else if (n + 3 < outCap) {
                out[n++] = static_cast<char>(0xE0 | (c >> 12));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MenuHits(const char* utf8, const char* const* keys) {
    if (!utf8 || !utf8[0] || !keys) return false;
    for (int i = 0; keys[i]; ++i) {
        if (strstr(utf8, keys[i])) return true;
    }
    return false;
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return x::runtime::il2cpp::AtRva<Fn>(rva);
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape, const char* plain,
                          const char* hash) {
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

void BindDialogKlass() {
    x::runtime::il2cpp_container::Ensure();
    if (!gUiDlgKlass) {
        gUiDlgKlass =
            x::runtime::il2cpp_prefab::FindClassCached(kUiUtilDialogExClass, kPrefabUtilDialogEx)
                .klass;
    }
    if (!gUiDlgKlass) return;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classGetFieldFromName && e.fieldGetOffset) {
        void* fType = nullptr;
        void* fMenu = nullptr;
        __try {
            fType = e.classGetFieldFromName(gUiDlgKlass, kHashUiDlgType);
            fMenu = e.classGetFieldFromName(gUiDlgKlass, kHashUiDlgMenuTexts);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("char_boot/dlgField");
            fType = nullptr;
            fMenu = nullptr;
        }
        if (fType) {
            const size_t off = e.fieldGetOffset(fType);
            if (off >= 0x10 && off < 0x1000) gOffUiDlgType = off;
        }
        if (fMenu) {
            const size_t off = e.fieldGetOffset(fMenu);
            if (off >= 0x10 && off < 0x1000) gOffUiDlgMenuTexts = off;
        }
    }
    if (!gUiDlgType) gUiDlgType = x::runtime::il2cpp::ClassTypeObjectOnMain(gUiDlgKlass);
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (!gMiUiDlgOnClickOk) {
        constexpr MethodShape kOk{0, TypeKind::Void, true, false, {}};
        gMiUiDlgOnClickOk =
            ResolveMi(gUiDlgKlass, kRvaUiDlgOnClickBtOk, kOk, "OnClickBtOk", nullptr);
    }
    if (!gMiUiDlgOnClickYes) {
        constexpr MethodShape kYes{0, TypeKind::Void, true, false, {}};
        gMiUiDlgOnClickYes =
            ResolveMi(gUiDlgKlass, kRvaUiDlgOnClickBtYes, kYes, nullptr, kHashOnClickBtYes);
    }
    if (!gMiUiDlgSelectMenu) {
        constexpr MethodShape kMenu{1, TypeKind::Void, false, false, {TypeKind::I32}};
        gMiUiDlgSelectMenu =
            ResolveMi(gUiDlgKlass, kRvaUiDlgSelectMenu, kMenu, "SetKeyFocus", kHashSetKeyFocus);
    }
    void* compKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    if (compKlass && !gMiGetGameObject) {
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true, {}};
        gMiGetGameObject = ResolveMi(compKlass, kRvaCompGetGo, kGo, "get_gameObject", nullptr);
    }
    if (goKlass && !gMiGoGetActiveSelf) {
        constexpr MethodShape kAct{0, TypeKind::Bool, true, true, {}};
        gMiGoGetActiveSelf =
            ResolveMi(goKlass, kRvaGoGetActiveSelf, kAct, "get_activeSelf", nullptr);
    }
}

enum class DlgAct : int {
    None = 0,
    Clicked = 1,
    Rejected = 2,
    BadType = 3,
    Seh = 4,
};

struct DlgJob {
    DlgAct act = DlgAct::None;
    int dlgType = -1;
    int menuN = 0;
    int scanN = 0;
    int aliveN = 0;
    int activeN = 0;
    int actYes = 0;
    bool listMode = false;
    bool doClick = false;
    bool callOk = false;
    void* dlg = nullptr;
    const char* const* keys = nullptr;
    char picked[96]{};
    char menuDump[192]{};
    char say[96]{};
    char scanDump[96]{};
};

bool DlgGoIsActive(void* dlg) {
    auto getGo = FnFromMi<FnGetGameObject>(gMiGetGameObject, kRvaCompGetGo);
    auto getActive = FnFromMi<FnGoGetActiveSelf>(gMiGoGetActiveSelf, kRvaGoGetActiveSelf);
    if (!getGo || !getActive) return true;
    void* go = nullptr;
    __try {
        go = getGo(dlg, gMiGetGameObject);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("char_boot/dlgGetGo");
        go = nullptr;
    }
    if (!go) return true;
    bool active = true;
    __try {
        active = getActive(go, gMiGoGetActiveSelf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("char_boot/dlgActiveSelf");
        active = true;
    }
    return active;
}

bool DlgTypeClickable(int dlgType) {
    return dlgType == kUiDlgTypeText || dlgType == kUiDlgTypeYesNo || dlgType == kUiDlgTypeList;
}

int DlgPickScore(int dlgType, bool active, const char* say, bool listMode,
                 const char* const* keys) {
    int score = 0;
    if (active) score += 8;
    if (say && say[0]) score += 10;
    if (MenuHits(say, keys)) score += 20;
    if (dlgType == kUiDlgTypeYesNo) score += 6;
    else if (dlgType == kUiDlgTypeList && listMode) score += 5;
    else if (dlgType == kUiDlgTypeText) score += 2;
    return score;
}

bool DlgLooksLive(void* dlg, int dlgType, const char* say, bool active, const char* const* keys) {
    if (!UnityAlive(dlg)) return false;
    if (!active) return false;
    if (!DlgTypeClickable(dlgType)) return false;
    (void)keys;
    (void)say;
    if (dlgType == kUiDlgTypeYesNo) return true;
    if (dlgType == kUiDlgTypeList) return true;
    if (dlgType != kUiDlgTypeText || !gLastYesMs) return false;
    return true;
}

void DlgJobOnMain(void* user) {
    auto* job = reinterpret_cast<DlgJob*>(user);
    if (!job) return;
    job->act = DlgAct::None;
    job->dlgType = -1;
    job->menuN = 0;
    job->scanN = 0;
    job->aliveN = 0;
    job->activeN = 0;
    job->actYes = 0;
    job->dlg = nullptr;
    job->picked[0] = 0;
    job->menuDump[0] = 0;
    job->say[0] = 0;
    job->scanDump[0] = 0;
    if (!x::runtime::il2cpp::Ensure()) return;
    BindDialogKlass();
    const auto& e = x::runtime::il2cpp::Get();
    FnFindAll findAll = e.findAll ? e.findAll : x::runtime::il2cpp::AtRva<FnFindAll>(kRvaFindAll);
    if (!gUiDlgType || !findAll) return;
    void* arr = nullptr;
    __try {
        arr = findAll(gUiDlgType, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("char_boot/findAllDlg");
        arr = nullptr;
    }
    const int n = LooksLikeHeapPtr(arr)
                      ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                            reinterpret_cast<uint8_t*>(arr) +
                            x::runtime::il2cpp_container::OffArrayMaxLength()))
                      : 0;
    job->scanN = n;
    void* dlg = nullptr;
    int liveType = -1;
    int bestScore = -1;
    char liveSay[96]{};
    for (int i = 0; i < n && i < kDlgScanCap; ++i) {
        void* o = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(arr) + x::runtime::il2cpp_container::OffArrayData() +
            static_cast<size_t>(i) * sizeof(void*));
        if (!UnityAlive(o)) continue;
        ++job->aliveN;
        const bool active = DlgGoIsActive(o);
        if (active) ++job->activeN;
        const int t = ReadI32(o, gOffUiDlgType);
        if (!gLastYesMs && t == kUiDlgTypeText && active) gSayDlg = o;
        char say[96]{};
        (void)ReadIl2CppStringUtf8(ReadPtr(o, kFbUiDlgSayText), say, sizeof(say));
        if (job->aliveN <= 6) {
            char one[24]{};
            snprintf(one, sizeof(one), "%s%d%c%d", job->scanDump[0] ? "," : "", t, active ? 'A' : 'i',
                     say[0] ? 1 : 0);
            strncat_s(job->scanDump, one, _TRUNCATE);
        }
        if (!DlgLooksLive(o, t, say, active, job->keys)) continue;
        int score = DlgPickScore(t, active, say, job->listMode, job->keys);
        if (gLastYesMs && gSayDlg && o == gSayDlg) score += 30;
        if (score <= bestScore) continue;
        bestScore = score;
        dlg = o;
        liveType = t;
        memcpy(liveSay, say, sizeof(liveSay));
    }
    if (!dlg) return;
    job->dlg = dlg;
    job->dlgType = liveType;
    memcpy(job->say, liveSay, sizeof(job->say));
    job->actYes = LooksLikeHeapPtr(ReadPtr(dlg, kOffActYes)) ? 1 : 0;
    if (!job->doClick) return;

    auto clickOk = FnFromMi<FnUiDlgOnClickOk>(gMiUiDlgOnClickOk, kRvaUiDlgOnClickBtOk);
    auto clickYes = FnFromMi<FnUiDlgOnClickOk>(gMiUiDlgOnClickYes, kRvaUiDlgOnClickBtYes);
    auto selectMenu = FnFromMi<FnUiDlgSelectMenu>(gMiUiDlgSelectMenu, kRvaUiDlgSelectMenu);
    if (!clickOk) return;

    if (job->dlgType == kUiDlgTypeList) {
        if (!job->listMode) {
            job->act = DlgAct::None;
            return;
        }
        void* texts = ReadPtr(dlg, gOffUiDlgMenuTexts);
        const int mn = ListSize(texts);
        job->menuN = mn;
        int pick = -1;
        char buf[96]{};
        size_t dumpN = 0;
        for (int i = 0; i < mn && i < 32; ++i) {
            if (!ReadIl2CppStringUtf8(ListAt(texts, i), buf, sizeof(buf))) continue;
            if (dumpN + 1 < sizeof(job->menuDump)) {
                if (dumpN) job->menuDump[dumpN++] = '|';
                const size_t take = strlen(buf);
                if (dumpN + take >= sizeof(job->menuDump)) break;
                memcpy(job->menuDump + dumpN, buf, take);
                dumpN += take;
                job->menuDump[dumpN] = 0;
            }
            if (pick < 0 && MenuHits(buf, job->keys)) {
                pick = i;
                strncpy_s(job->picked, buf, _TRUNCATE);
            }
        }
        if (pick < 0) {
            job->act = DlgAct::Rejected;
            return;
        }
        __try {
            if (selectMenu) selectMenu(dlg, pick, gMiUiDlgSelectMenu);
            clickOk(dlg, gMiUiDlgOnClickOk);
            job->act = DlgAct::Clicked;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("char_boot/listClick");
            job->act = DlgAct::Seh;
        }
        return;
    }

    if (job->dlgType == kUiDlgTypeText) {
        if (!job->say[0] && !gLastYesMs) return;
        __try {
            clickOk(dlg, gMiUiDlgOnClickOk);
            job->act = DlgAct::Clicked;
            strncpy_s(job->picked, "ok", _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("char_boot/okClick");
            job->act = DlgAct::Seh;
        }
        return;
    }

    const bool keyHit = MenuHits(job->say, job->keys);
    if (job->dlgType == kUiDlgTypeYesNo && job->keys && job->say[0] && !keyHit) return;
    const bool wantYes = job->dlgType == kUiDlgTypeYesNo || keyHit;
    if (wantYes) {
        if (!clickYes) {
            job->act = DlgAct::Seh;
            return;
        }
        __try {
            clickYes(dlg, gMiUiDlgOnClickYes);
            job->act = DlgAct::Clicked;
            strncpy_s(job->picked, "yes", _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("char_boot/yesClick");
            job->act = DlgAct::Seh;
        }
        return;
    }
    job->act = DlgAct::BadType;
}

void LogDlgScan(const DlgJob& job, const char* tag) {
    const DWORD now = GetTickCount();
    if (gLastDlgScanLogMs && now - gLastDlgScanLogMs < kDlgScanLogMs) return;
    gLastDlgScanLogMs = now;
    runtime::LogI("CharBoot",
                  "dlg %s n=%d alive=%d active=%d type=%d klass=%d typeObj=%d say=%s act370=%d dump=%s",
                  tag ? tag : "-", job.scanN, job.aliveN, job.activeN, job.dlgType,
                  gUiDlgKlass ? 1 : 0, gUiDlgType ? 1 : 0, job.say[0] ? job.say : "-",
                  job.actYes, job.scanDump[0] ? job.scanDump : "-");
}

DlgJob RunDlg(bool listMode, const char* const* keys, bool doClick) {
    DlgJob job{};
    job.listMode = listMode;
    job.keys = keys;
    job.doClick = doClick;
    job.callOk = x::runtime::managed_main::Call(&DlgJobOnMain, &job, 1500);
    if (!job.callOk) {
        LogDlgScan(job, "call-fail");
        return job;
    }
    if (job.dlgType < 0) LogDlgScan(job, doClick ? "click-miss" : "peek-miss");
    return job;
}

void Publish(notify::NotificationKind kind, const char* title, const char* body) {
    notify::PublishNotification(notify::NotificationEvent{kind, "char-boot", title, body, 5500});
}

VitalsSnap ReadSnap() {
    VitalsSnap s{};
    s.playReady = ports::world::IsPlayReady();
    s.mapId = ports::world::GetMapId();
    x::ui::player::Vitals v{};
    if (x::ui::player::Read(v) && v.ok) {
        s.level = v.level;
        s.job = v.job;
        s.ok = true;
    }
    s.meso = x::ui::player::ReadMoney();
    x::ui::player::BaseApStats ap{};
    if (x::ui::player::ReadBaseApStats(ap) && ap.ok) {
        s.intel = ap.intel;
        s.ap = ap.ap;
        s.ok = true;
    }
    x::ui::player::BaseSpStats sp{};
    if (x::ui::player::ReadBaseSpStats(sp) && sp.ok) s.sp = sp.sp;
    return s;
}

bool OfficialReady(const VitalsSnap& s) {
    if (s.job != 0) return false;
    if (s.level < 8) return false;
    return true;
}

bool UserTrigger(const VitalsSnap& s) {
    if (gTrip.departKind == xcat::kCharBootDepartMeso)
        return s.meso >= static_cast<int64_t>(gTrip.mesoMin) && s.meso >= 0;
    return s.level >= static_cast<int>(gTrip.levelMin);
}

bool ShipFareOk(const VitalsSnap& s) {
    return s.meso >= xcat::kCharBootShipFareMeso && s.meso >= 0;
}

bool ReadyToLeave(const VitalsSnap& s) {
    return OfficialReady(s) && UserTrigger(s) && ShipFareOk(s);
}

bool AutoStatOn() {
    xcat::AutoStatConfig ast{};
    if (!xcat::ReadAutoStat(runtime::GetBinDir(), ast)) return false;
    return ast.enabled != 0 && xcat::AutoStatRatioOk(ast);
}

bool AutoSkillOn() {
    xcat::AutoSkillConfig sk{};
    if (!xcat::ReadAutoSkill(runtime::GetBinDir(), sk)) return false;
    return sk.enabled != 0 && sk.job1Enabled != 0;
}

bool Frozen() { return auto_lie::IsBusy() || !ports::world::IsPlayReady(); }

DWORD WallElapsed(DWORD now) {
    if (now < gStateEnterMs) return 0;
    DWORD acc = gFrozenAcc;
    if (gFrozenSince && now >= gFrozenSince) acc += (now - gFrozenSince);
    const DWORD raw = now - gStateEnterMs;
    return raw > acc ? raw - acc : 0;
}

void NoteFreeze(DWORD now, bool frozen) {
    if (frozen) {
        if (!gFrozenSince) gFrozenSince = now;
    } else if (gFrozenSince) {
        if (now >= gFrozenSince) gFrozenAcc += (now - gFrozenSince);
        gFrozenSince = 0;
    }
}

void ApplyPause(State s) {
    const bool on = (s != State::Idle && s != State::Farm && s != State::Done);
    simple_combat::SetHardPause(simple_combat::HardPauseHolder::CharBoot, on);
    fly::SetExternalPause(on);
}

void PublishStatus(const VitalsSnap* snap) {
    xcat::CharBootStatus st{};
    xcat::CharBootStatusSetDefaults(st);
    strncpy_s(st.state, StateName(Cur()), _TRUNCATE);
    strncpy_s(st.message, gMsg, _TRUNCATE);
    strncpy_s(st.lastWhy, gLastWhy, _TRUNCATE);
    if (snap) {
        st.mapId = snap->mapId > 0 ? static_cast<uint32_t>(snap->mapId) : 0;
        st.level = snap->level;
        st.job = snap->job;
        st.intel = snap->intel;
        st.meso = snap->meso;
        st.ready = ReadyToLeave(*snap) ? 1u : 0u;
    }
    st.hangupMap = static_cast<uint32_t>(gTrip.hangupMap);
    st.writeTickMs = GetTickCount64();
    (void)xcat::WriteCharBootStatus(runtime::GetBinDir(), st);
    gLastStatusMs = GetTickCount();
}

void StopApproachFly() {
    ports::attack::StopWalk();
    if (!gApproachFly) return;
    heli::Disarm(heli::Owner::Fly);
    heli::Release(heli::Owner::Fly);
    if (!fly::IsArmed()) {
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Fly, false);
    }
    gApproachFly = false;
    gTalkLandSince = 0;
}

void ResetTripCounters() {
    gLastGotoMs = 0;
    gStableSince = 0;
    gLastClickMs = 0;
    gLastTalkMs = 0;
    gLastYesMs = 0;
    gShipNextN = 0;
    gYesDlg = nullptr;
    gSayDlg = nullptr;
    gSawDlgThisTalk = false;
    gTalkN = 0;
    gNoNpcN = 0;
    gDismissN = 0;
    gFiredAtMs = 0;
    gApZeroSince = 0;
    gSpHoldSince = 0;
    gLastSp = -1;
    gSkipSkillLogged = false;
    gFarmAccMs = 0;
    gFrozenSince = 0;
    gFrozenAcc = 0;
    gTalkLandSince = 0;
    gNpcStuck = false;
    StopApproachFly();
}

void Enter(State s, const char* why) {
    const State from = Cur();
    gState.store(static_cast<int>(s), std::memory_order_release);
    gStateEnterMs = GetTickCount();
    ResetTripCounters();
    ApplyPause(s);
    SetMsg(why ? why : "");
    if (s == State::Done) {
        Publish(notify::NotificationKind::Success, "转职完成（法师）",
                "已到挂机图。8 级技能点要到 10 级才会加。");
    }
    const VitalsSnap snap = ReadSnap();
    gLastJob = snap.job;
    runtime::LogI("CharBoot", "state %s→%s why=%s map=%d lv=%d job=%d int=%d meso=%lld",
                  StateName(from), StateName(s), why ? why : "", snap.mapId, snap.level, snap.job,
                  snap.intel, static_cast<long long>(snap.meso));
    PublishStatus(&snap);
}

void Fail(const char* why) {
    const State from = Cur();
    if (from == State::Idle) return;
    strncpy_s(gLastWhy, why ? why : "fail", _TRUNCATE);
    travel::RequestStop();
    ResetTripCounters();
    if (gCombatArmed) {
        gCombatArmed = false;
        x::ipc::PayloadControl_PublishSimpleCombat(false);
    }
    simple_combat::SetHardPause(simple_combat::HardPauseHolder::CharBoot, false);
    fly::SetExternalPause(false);
    const VitalsSnap snap = ReadSnap();
    runtime::LogW("CharBoot", "fail why=%s state=%s map=%d lv=%d job=%d int=%d meso=%lld",
                  gLastWhy, StateName(from), snap.mapId, snap.level, snap.job, snap.intel,
                  static_cast<long long>(snap.meso));
    char body[192]{};
    if (snap.job == kJobMage && !gHangupHanded)
        snprintf(body, sizeof(body), "%s。职业已转，补给回图仍是岛上，请再点开始。", gLastWhy);
    else
        strncpy_s(body, gLastWhy, _TRUNCATE);
    Publish(notify::NotificationKind::Warning, "一键起号中止", body);
    gState.store(static_cast<int>(State::Idle), std::memory_order_release);
    SetMsg("");
    PublishStatus(&snap);
}

bool WaitSafeLand(DWORD now) {
    if (simple_combat::IsSafeLandActive()) {
        SetMsg("安全落台中…");
        return false;
    }
    ports::teleport::FlightState st{};
    const bool have = ports::teleport::QueryFlightState(st) && st.ok;
    if (have && st.onFh) return true;
    if (travel::IsActive()) {
        SetMsg("赶路稳图中…");
        return false;
    }
    if (have && !st.onFh) {
        simple_combat::RequestSafeLand("char_boot_wait_airborne");
        SetMsg("安全落台中…");
        return false;
    }
    (void)now;
    return true;
}

bool MapStable(int target, DWORD now) {
    const int cur = ports::world::GetMapId();
    if (!MapMatches(target, cur) || travel::IsActive() || !ports::world::IsPlayReady()) {
        gStableSince = 0;
        return false;
    }
    if (!gStableSince) gStableSince = now;
    return now - gStableSince >= kMapStableMs;
}

bool TravelFailIsHard(travel::FailKind k) {
    return k == travel::FailKind::Unreachable || k == travel::FailKind::FakeFireStop ||
           k == travel::FailKind::BadTarget || k == travel::FailKind::FireStuck;
}

int ExpectedGotoMap() {
    switch (Cur()) {
    case State::GotoFarm:
        return gTrip.farmMap;
    case State::GotoHarbor:
        return kMapHarbor;
    case State::GotoEllinia:
        return kMapEllinia;
    case State::GotoHangup:
        return gTrip.hangupMap;
    default:
        return 0;
    }
}

bool Watchdog(DWORD now, const VitalsSnap& snap) {
    if (Cur() == State::Idle) return false;
    if (!invuln::IsDesired()) {
        Fail("invuln_off");
        return true;
    }
    if (Cur() == State::Farm && !simple_combat::IsEnabled()) {
        Fail("combat_off");
        return true;
    }
    const int ch = channel_hop::LastKnownChannel1Based();
    if (gArmChannel > 0 && ch > 0 && ch != gArmChannel) {
        Fail("scene_interrupt");
        return true;
    }
    if (travel::IsActive()) {
        const int want = ExpectedGotoMap();
        travel::Snapshot ts{};
        travel::QuerySnapshot(ts);
        const int got = atoi(ts.gotoTarget);
        if (want <= 0) {
            Fail("travel_stolen");
            return true;
        }
        if (got > 0 && !MapMatches(want, got)) {
            Fail("travel_stolen");
            return true;
        }
    }
    bool shopReady = false;
    static DWORD sShopChk = 0;
    if (!sShopChk || now - sShopChk >= 1000) {
        sShopChk = now;
        if (shop::ShopReady(shopReady) && shopReady && Cur() != State::Farm) {
            Fail("shop_busy");
            return true;
        }
    }
    (void)now;
    (void)snap;
    return false;
}

enum class GotoTick { Wait, Arrived };

GotoTick TickGoto(int target, DWORD now, DWORD wallMs, const char* tag) {
    if (Frozen()) {
        SetMsg("冻态…");
        return GotoTick::Wait;
    }
    if (MapStable(target, now)) return GotoTick::Arrived;

    travel::Snapshot snap{};
    const bool haveSnap = travel::QuerySnapshot(snap);
    if (haveSnap && !travel::IsActive()) {
        if (snap.failKind == travel::FailKind::CombatOn) {
            Fail("打怪未暂停");
            return GotoTick::Wait;
        }
        if (TravelFailIsHard(snap.failKind)) {
            if (Cur() == State::GotoHangup && snap.failKind == travel::FailKind::Unreachable &&
                ports::world::GetMapId() == kMapLib && !gDidExitLib) {
                Enter(State::ExitLib, "GotoHangup Unreachable，改出馆");
                return GotoTick::Wait;
            }
            char why[64]{};
            snprintf(why, sizeof(why), "travel_%d", static_cast<int>(snap.failKind));
            Fail(why);
            return GotoTick::Wait;
        }
    }

    if (travel::IsActive()) {
        SetMsg("赶路中…");
        if (WallElapsed(now) > wallMs) Fail("timeout");
        return GotoTick::Wait;
    }
    if (!WaitSafeLand(now)) return GotoTick::Wait;
    if (ports::teleport::NativeCooldownRemainingMs() > 0) {
        SetMsg("瞬移冷却…");
        return GotoTick::Wait;
    }
    if (simple_combat::IsFarmingActive()) {
        SetMsg("等待打怪停手…");
        return GotoTick::Wait;
    }
    if (!gLastGotoMs || now - gLastGotoMs > kGotoRetryMs) {
        char key[32]{};
        MapKey(target, key, sizeof(key));
        runtime::LogI("CharBoot", "%s RequestGoto %s", tag ? tag : "Goto", key);
        travel::RequestGoto(key);
        gLastGotoMs = now;
        gStableSince = 0;
    }
    if (WallElapsed(now) > wallMs) Fail("timeout");
    return GotoTick::Wait;
}

bool DismissDialogs(DWORD now) {
    if (gDismissN >= kMaxDismiss) {
        const DlgJob peek = RunDlg(false, nullptr, false);
        if (peek.dlgType >= 0) {
            Fail("dialog_stuck");
            return false;
        }
        return true;
    }
    if (gLastClickMs && now - gLastClickMs < kClickGapMs) return false;
    const DlgJob job = RunDlg(false, nullptr, true);
    if (job.dlgType < 0) return true;
    if (job.dlgType == kUiDlgTypeList) {
        Fail("dialog_stuck");
        return false;
    }
    if (job.act == DlgAct::Clicked) {
        gLastClickMs = now;
        ++gDismissN;
        return false;
    }
    if (job.act == DlgAct::BadType) {
        Fail("BadType");
        return false;
    }
    return true;
}

bool ClickScript(DWORD now, const char* const* keys, const char* failPrefix) {
    const DWORD t = GetTickCount();
    if (gLastTalkMs && t - gLastTalkMs < kDlgSettleMs) return false;
    if (gLastYesMs && t - gLastYesMs < kDlgAfterYesMs) return false;
    if (gLastClickMs && now - gLastClickMs < kClickGapMs) return false;
    const DlgJob job = RunDlg(true, keys, true);
    if (job.dlgType < 0) return false;
    if (job.act == DlgAct::Clicked) {
        gLastClickMs = GetTickCount();
        if (job.dlgType == kUiDlgTypeYesNo) {
            gLastYesMs = GetTickCount();
            gYesDlg = job.dlg;
            gShipNextN = 0;
        } else if (job.dlgType == kUiDlgTypeText && Cur() == State::BoardShip) {
            ++gShipNextN;
            gLastYesMs = GetTickCount();
        }
        const int ret = job.dlg ? ReadI32(job.dlg, kFbUiDlgRet) : -1;
        runtime::LogI("CharBoot", "dialog type=%d pick=%s ret=%d next=%d/%d say=%s", job.dlgType,
                      job.picked[0] ? job.picked : (job.dlgType == 4 ? "?" : "ok"), ret, gShipNextN,
                      kShipNextNeed, job.say[0] ? job.say : "-");
        return true;
    }
    if (job.act == DlgAct::Rejected) {
        runtime::LogW("CharBoot", "List rejected menu=%s", job.menuDump);
        Fail("list_rejected");
        return false;
    }
    if (job.act == DlgAct::BadType) {
        Fail("BadType");
        return false;
    }
    if (job.act == DlgAct::Seh) {
        Fail(failPrefix);
        return false;
    }
    return job.dlgType >= 0;
}

bool StickNpc(const shop::NpcLocate& loc) {
    SetMsg("贴NPC");
    ports::attack::StopWalk();
    if (simple_combat::IsSafeLandActive())
        simple_combat::CancelSafeLand("char_boot_stick_npc");
    if (heli::Bailed()) heli::ClearBailed();

    ports::teleport::FlightState st{};
    const bool have = ports::teleport::QueryFlightState(st) && st.ok;
    const float apY = have ? st.y : loc.playerY;
    // NPC ActorPos 与 LocalUser ActorPos 同字段，就是 AbsPos。
    // BIN 23:02 把桑克斯 y=219 反号成 -219：aimFh=0，人在空中 y≈-210 晃，NOT_STOOD。
    // PetLoot 的 Y 是翻转的（港面 Ap=-287 写成 +287），禁止用它给 NPC 反号。
    const float npcY = loc.y;

    float sx = loc.x, sy = npcY;
    uint32_t fh = 0;
    bool snap = ports::foothold_path::SnapStandAt(loc.x, npcY, &sx, &sy, &fh,
                                                 /*preferFlat=*/true,
                                                 /*avoidWalkJunction=*/false) &&
                fh != 0;
    if (!snap) {
        snap = ports::foothold_path::SnapStandAt(loc.x, npcY, &sx, &sy, &fh, false, false) &&
               fh != 0;
    }
    const float landY = snap ? sy : npcY;

    ports::foothold_path::ColumnHit hits[8]{};
    int colTotal = 0;
    const int colN =
        ports::foothold_path::ProbeColumn(loc.x, npcY, 800, hits, 8, &colTotal);
    char col[192]{};
    int colUsed = 0;
    for (int i = 0; i < colN && colUsed < static_cast<int>(sizeof(col)) - 24; ++i) {
        colUsed += snprintf(col + colUsed, sizeof(col) - static_cast<size_t>(colUsed), " %u@%d",
                            hits[i].fh, hits[i].y);
    }

    ports::travel::PortalInfo p{};
    char name[32]{};
    snprintf(name, sizeof(name), "npc:%d", loc.tpl);
    p.name = name;
    p.x = loc.x;
    p.y = landY;
    p.rectValid = true;
    p.rectL = loc.x - 160.f;
    p.rectR = loc.x + 160.f;
    p.rectT = landY - 80.f;
    p.rectB = landY + 80.f;

    runtime::LogI("CharBoot",
                  "stick npc tpl=%d actor=(%.0f,%.0f) apY=%.0f land=(%.0f,%.0f) fh=%u snap=%d "
                  "rect=(%.0f,%.0f)-(%.0f,%.0f) col=%d/%d%s",
                  loc.tpl, loc.x, loc.y, apY, loc.x, landY, fh, snap ? 1 : 0, p.rectL, p.rectT,
                  p.rectR, p.rectB, colN, colTotal, col[0] ? col : " -");

    std::string res;
    const bool ok = ports::travel::StickToStand(p, res);
    runtime::LogI("CharBoot", "stick npc done ok=%d result=%s", ok ? 1 : 0, res.c_str());
    if (ok && (res == "STOOD" || res == "ALREADY")) {
        gNpcStuck = true;
        return true;
    }
    return false;
}

bool WaitTalkLand(DWORD now) {
    if (simple_combat::IsSafeLandActive())
        simple_combat::CancelSafeLand("char_boot_talk_land");
    ports::teleport::FlightState st{};
    const bool have = ports::teleport::QueryFlightState(st) && st.ok;
    if (have && st.onFh) {
        gTalkLandSince = 0;
        return true;
    }
    if (!gTalkLandSince) gTalkLandSince = now;
    SetMsg("贴脸落地中…");
    return now - gTalkLandSince >= 800;
}

bool TalkNpc(DWORD now, int tpl) {
    (void)now;
    const DWORD t = GetTickCount();
    if (gLastTalkMs && t - gLastTalkMs < kTalkGapMs) return false;
    runtime::LogI("CharBoot", "TalkToNpc tpl=%d", tpl);
    gLastYesMs = 0;
    gShipNextN = 0;
    gYesDlg = nullptr;
    gSayDlg = nullptr;
    gSawDlgThisTalk = false;
    const bool ok = shop::TryTalkNearestNpc(kTalkRangePx, tpl, true);
    gLastTalkMs = GetTickCount();
    ++gTalkN;
    if (!ok) {
        ++gNoNpcN;
        if (gNoNpcN >= 2) {
            Fail("talk_far");
            return false;
        }
        return false;
    }
    gNoNpcN = 0;
    return true;
}

bool HandoffHangup() {
    const int id = ports::world::GetMapId();
    runtime::LogI("CharBoot", "GotoHangup map=%d", gTrip.hangupMap);
    auto_supply::RecordHangupFarmMap("char_boot_hangup");
    char peeked[64]{};
    if (!auto_supply::PeekLastFarmMap(peeked, sizeof(peeked)) || atoi(peeked) != gTrip.hangupMap) {
        Fail("hangup_record_miss");
        return false;
    }
    runtime::LogI("CharBoot", "RecordHangupFarmMap %d (char_boot_hangup) cur=%d", gTrip.hangupMap,
                  id);
    gHangupHanded = true;
    return true;
}

bool ArmSkillConflict(char* why, size_t whyCap) {
    xcat::AutoSkillConfig sk{};
    if (!xcat::ReadAutoSkill(runtime::GetBinDir(), sk)) return false;
    if (!sk.enabled) return false;
    if (sk.job1Enabled && sk.job1 != 0 && sk.job1 != kJobMage) {
        snprintf(why, whyCap, "加技能不是法师");
        return true;
    }
    if (sk.job1Enabled && sk.job1 == 0) {
        snprintf(why, whyCap, "未选 1 转技能书");
        return true;
    }
    return false;
}

bool ClampFarm(int farm, int curMap, char* why, size_t whyCap) {
    if (farm >= 100000000) {
        snprintf(why, whyCap, "farm_cross_plate");
        return true;
    }
    if (farm == kMapHarbor) {
        snprintf(why, whyCap, "farm_is_harbor");
        return true;
    }
    if (auto_supply::IsTownMapIdHeuristic(farm)) {
        snprintf(why, whyCap, "farm_is_town");
        return true;
    }
    if (IsMapleIsland(curMap) && curMap > 0) {
        char a[32]{}, b[32]{};
        MapKey(curMap, a, sizeof(a));
        MapKey(farm, b, sizeof(b));
        if (travel::PathHopCount(a, b) < 0) {
            snprintf(why, whyCap, "farm_unreachable");
            return true;
        }
    }
    return false;
}

bool ClampHangup(int hangup, char* why, size_t whyCap) {
    if (hangup <= 0) {
        snprintf(why, whyCap, "hangup_empty");
        return true;
    }
    if (hangup == kMapLib) {
        snprintf(why, whyCap, "hangup_is_lib");
        return true;
    }
    if (auto_supply::IsTownMapIdHeuristic(hangup)) {
        snprintf(why, whyCap, "hangup_is_town");
        return true;
    }
    char a[32]{}, b[32]{};
    MapKey(kMapEllinia, a, sizeof(a));
    MapKey(hangup, b, sizeof(b));
    if (travel::PathHopCount(a, b) < 0) {
        snprintf(why, whyCap, "hangup_unreachable");
        return true;
    }
    return false;
}

void JumpAfterArm(const VitalsSnap& s) {
    const bool island = IsMapleIsland(s.mapId);
    const bool vic = s.mapId >= 100000000;
    const bool fare = ShipFareOk(s);
    const bool official = OfficialReady(s);

    if (s.job == kJobMage && MapMatches(gTrip.hangupMap, s.mapId)) {
        if (!HandoffHangup()) return;
        if (s.ap == 0 && (s.level < 10 || !AutoSkillOn())) {
            Enter(State::Done, "已是法师且已在挂机图");
            return;
        }
        Enter(State::WaitSpend, "已是法师，等加点");
        return;
    }
    if (s.job == kJobMage && island) {
        Fail("已是法师但还在岛上");
        return;
    }
    if (s.job == kJobMage && s.mapId == kMapLib) {
        Enter(State::WaitSpend, "馆内已是法师");
        return;
    }
    if (s.job == kJobMage && vic) {
        Enter(State::WaitSpend, "维多已是法师");
        return;
    }
    if (s.job != 0 && s.job != kJobMage) {
        Fail("只做法师初心者");
        return;
    }
    if (s.mapId == kMapLib && official) {
        Enter(State::JobTalk, "馆内转职");
        return;
    }
    if (s.mapId == kMapLib && !official) {
        Fail("在汉斯处未达硬门");
        return;
    }
    if (s.mapId == kMapEllinia && official) {
        Enter(State::EnterLib, "进图书馆");
        return;
    }
    if (vic && official) {
        Enter(State::GotoEllinia, "GotoEllinia map=101000000");
        return;
    }
    if (vic && !official) {
        Fail("已在维多未达硬门");
        return;
    }
    if (s.mapId == kMapHarbor && official && fare) {
        Enter(State::BoardShip, "港上乘船");
        return;
    }
    if (s.mapId == kMapHarbor && official && s.meso >= 0 &&
        s.meso < xcat::kCharBootShipFareMeso) {
        Fail("ship_fare");
        return;
    }
    if (s.mapId == kMapHarbor && official && s.meso < 0) {
        Enter(State::BoardShip, "港上乘船（等金币读取）");
        return;
    }
    if (island && ReadyToLeave(s)) {
        Enter(State::GotoHarbor, "GotoHarbor map=60000");
        return;
    }
    if (island && !ReadyToLeave(s)) {
        char whyFarm[48]{};
        snprintf(whyFarm, sizeof(whyFarm), "GotoFarm want=%d", gTrip.farmMap);
        Enter(State::GotoFarm, whyFarm);
        return;
    }
    Fail("arm_no_jump");
}

bool EnsureCombatOn(char* why, size_t whyCap) {
    if (!invuln::IsDesired()) {
        xcat::PayloadControl c{};
        if (!xcat::ReadPayloadControl(runtime::GetBinDir(), c)) xcat::PayloadControlSetDefaults(c);
        c.invuln = 1;
        c.writeTickMs = GetTickCount64();
        if (!xcat::WritePayloadControl(runtime::GetBinDir(), c)) {
            snprintf(why, whyCap, "无法打开无敌");
            return false;
        }
        invuln::SetDesired(true);
    }
    x::ipc::PayloadControl_PublishSimpleCombat(true);
    if (!simple_combat::IsEnabled()) {
        snprintf(why, whyCap, "无法打开自动打怪");
        return false;
    }
    if (!invuln::IsDesired()) {
        snprintf(why, whyCap, "无法打开无敌");
        return false;
    }
    gCombatArmed = true;
    return true;
}

void TickArm(const VitalsSnap& s) {
    char why[96]{};
    if (!s.playReady) {
        Fail("未进图");
        return;
    }
    if (s.job != 0 && s.job != kJobMage) {
        Fail("只做法师初心者");
        return;
    }
    if (ArmSkillConflict(why, sizeof(why))) {
        Fail(why);
        return;
    }
    if (ClampFarm(gTrip.farmMap, s.mapId, why, sizeof(why))) {
        Fail(why);
        return;
    }
    if (ClampHangup(gTrip.hangupMap, why, sizeof(why))) {
        Fail(why);
        return;
    }
    if (auto_supply::IsBusy()) {
        Fail("补给进行中");
        return;
    }
    if (sellbag::IsBusy()) {
        Fail("卖装进行中");
        return;
    }
    if (!EnsureCombatOn(why, sizeof(why))) {
        Fail(why);
        return;
    }
    gArmChannel = channel_hop::LastKnownChannel1Based();
    gHangupHanded = false;
    gDidExitLib = false;
    gShipFareLocked = false;
    runtime::LogI("CharBoot", "Arm ok farm=%d hangup=%d kind=%s n=%u map=%d combat=1",
                  gTrip.farmMap, gTrip.hangupMap,
                  gTrip.departKind == xcat::kCharBootDepartMeso ? "meso" : "level",
                  gTrip.departKind == xcat::kCharBootDepartMeso ? gTrip.mesoMin : gTrip.levelMin,
                  s.mapId);
    JumpAfterArm(s);
}

void TickFarm(DWORD now, const VitalsSnap& s) {
    if (!InFarm(s.mapId)) {
        char why[64]{};
        snprintf(why, sizeof(why), "不在刷级图 cur=%d want=%d", s.mapId, gTrip.farmMap);
        Enter(State::GotoFarm, why);
        return;
    }
    if (ReadyToLeave(s)) {
        Enter(State::GotoHarbor, "GotoHarbor map=60000");
        return;
    } else if (OfficialReady(s) && UserTrigger(s) && !ShipFareOk(s) && s.meso >= 0) {
        SetMsg("船费 150");
    } else {
        SetMsg("刷级中");
    }
    if (!gLastFarmLogMs || now - gLastFarmLogMs >= 5000) {
        gLastFarmLogMs = now;
        runtime::LogI("CharBoot", "farm map=%d lv=%d int=%d meso=%lld ready=%d", s.mapId, s.level,
                      s.intel, static_cast<long long>(s.meso), ReadyToLeave(s) ? 1 : 0);
    }
    if (gTrip.farmTimeoutMin > 0) {
        const DWORD lim = gTrip.farmTimeoutMin * 60000u;
        if (gFarmAccMs >= lim) Fail("farm_timeout");
    }
}

void HotReadConfig();

void TickTalkLoop(DWORD now, const VitalsSnap& s, int npc, const char* const* keys, int successMap,
                  bool successByJob, DWORD wallMs) {
    if (successByJob) {
        if (s.job == kJobMage) {
            runtime::LogI("CharBoot", "job %d→%d", gLastJob, s.job);
            Enter(State::WaitJob, "等转职生效");
            return;
        }
    } else if (s.mapId == successMap) {
        runtime::LogI("CharBoot", "map → %d", s.mapId);
        if (successMap == kMapVictoria) {
            Enter(State::GotoEllinia, "GotoEllinia map=101000000");
            return;
        }
    }
    if (Frozen()) {
        StopApproachFly();
        SetMsg("冻态…");
        return;
    }
    if (travel::IsActive()) {
        StopApproachFly();
        travel::RequestStop();
        SetMsg("等赶路停");
        return;
    }
    const DlgJob peek = RunDlg(true, keys, false);
    HotReadConfig();
    if (Cur() == State::Idle) return;
    if (peek.activeN > 0 || peek.dlgType >= 0) gSawDlgThisTalk = true;
    if (gLastYesMs) {
        StopApproachFly();
        const bool ship = (npc == kNpcSancks);
        if (GetTickCount() - gLastYesMs < kDlgAfterYesMs) {
            SetMsg((ship && gShipNextN >= kShipNextNeed) ? "等开船" : "等下页");
            LogDlgScan(peek, (ship && gShipNextN >= kShipNextNeed) ? "wait-warp" : "wait-next");
            if (WallElapsed(now) > wallMs) Fail("timeout");
            return;
        }
        if (ship && gShipNextN >= kShipNextNeed) {
            SetMsg("等开船");
            LogDlgScan(peek, "wait-warp");
            if (WallElapsed(now) > wallMs) Fail("timeout");
            return;
        }
        if (peek.dlgType >= 0) {
            SetMsg("点对话");
            (void)ClickScript(now, keys, "dialog_seh");
            return;
        }
        SetMsg("等下页");
        LogDlgScan(peek, "wait-pages");
        if (WallElapsed(now) > wallMs) Fail("timeout");
        return;
    }
    if (gSawDlgThisTalk && peek.activeN == 0 && peek.dlgType < 0) {
        gLastTalkMs = 0;
        gYesDlg = nullptr;
        gSayDlg = nullptr;
        gSawDlgThisTalk = false;
        SetMsg("");
        LogDlgScan(peek, "dlg-closed");
    }
    if (peek.dlgType >= 0) {
        StopApproachFly();
        if (peek.dlgType == kUiDlgTypeYesNo && keys && peek.say[0] && !MenuHits(peek.say, keys)) {
            SetMsg("等对话框");
            LogDlgScan(peek, "wait-keys");
            if (WallElapsed(now) > wallMs) Fail("timeout");
            return;
        }
        const DWORD sinceTalk = gLastTalkMs ? (GetTickCount() - gLastTalkMs) : 0;
        if (!gLastTalkMs || sinceTalk < kDlgSettleMs) {
            SetMsg("等对话框");
            LogDlgScan(peek, "wait-settle");
            return;
        }
        SetMsg("点对话");
        (void)ClickScript(now, keys, "dialog_seh");
        return;
    }
    if (gLastTalkMs && GetTickCount() - gLastTalkMs < kTalkWaitDlgMs) {
        SetMsg("等对话框");
        LogDlgScan(peek, "wait-say");
        if (WallElapsed(now) > wallMs) Fail("timeout");
        return;
    }
    if (peek.activeN > 0) {
        SetMsg("等对话框");
        LogDlgScan(peek, "wait-alive");
        if (WallElapsed(now) > wallMs) Fail("timeout");
        return;
    }
    if (gTalkN >= kMaxTalk) {
        Fail("talk_timeout");
        return;
    }
    shop::NpcLocate loc{};
    const bool located = shop::LocateNpcByTemplate(npc, loc) && loc.ok;
    ports::teleport::FlightState st{};
    const bool haveAp = ports::teleport::QueryFlightState(st) && st.ok;
    const float apX = haveAp ? st.x : loc.playerX;
    const float dx = located ? (loc.x - apX) : 0.f;
    const float adx = std::fabs(dx);
    if (gNpcStuck && located && adx > kTalkLeavePx) gNpcStuck = false;
    if (!gNpcStuck) {
        if (!located) {
            if (WallElapsed(now) > wallMs) Fail("timeout");
            return;
        }
        if (haveAp && st.onFh && adx <= kTalkArrivePx) {
            gNpcStuck = true;
        } else if (!StickNpc(loc)) {
            Fail("npc_stick");
            return;
        }
        HotReadConfig();
        if (Cur() == State::Idle) return;
    }
    if (located && !WaitTalkLand(now)) {
        if (WallElapsed(now) > wallMs) Fail("timeout");
        return;
    }
    (void)TalkNpc(now, npc);
    if (WallElapsed(now) > wallMs) Fail("timeout");
}

void TickPortal(DWORD now, const VitalsSnap& s, const char* portal, int successMap, int fromMap,
                DWORD wallMs, const char* noPortal, const char* stickFail, const char* noWarp) {
    if (s.mapId == successMap) {
        if (successMap == kMapLib)
            Enter(State::JobTalk, "map 101000003");
        else if (successMap == kMapEllinia)
            Enter(State::GotoHangup, "出馆");
        return;
    }
    if (Frozen()) {
        SetMsg("冻态…");
        return;
    }
    if (travel::IsActive()) {
        travel::RequestStop();
        SetMsg("等赶路停");
        return;
    }
    if (!DismissDialogs(now)) return;
    if (!WaitSafeLand(now)) return;
    if (simple_combat::IsFarmingActive()) {
        SetMsg("等待打怪停手…");
        return;
    }
    if (gFiredAtMs && now - gFiredAtMs >= kFiredNoWarpMs && s.mapId == fromMap) {
        Fail(noWarp);
        return;
    }
    if (gLastGotoMs && now - gLastGotoMs < kGotoRetryMs) return;

    ports::travel::PortalInfo p{};
    if (!ports::travel::FindPortalByName(portal, p)) {
        Fail(noPortal);
        return;
    }
    runtime::LogI("CharBoot", "FirePortalByName %s", portal);
    std::string res;
    const bool ok = ports::travel::FirePortalByName(portal, res);
    gLastGotoMs = GetTickCount();
    if (!ok && res == "NO_PORTAL") {
        Fail(noPortal);
        return;
    }
    if (res == "NOT_STOOD") {
        if (WallElapsed(now) > wallMs) Fail(stickFail);
        return;
    }
    if (res.rfind("FIRED_", 0) == 0 || res == "MAP_TRANSITION" || res == "MAP_CHANGED") {
        if (!gFiredAtMs) gFiredAtMs = GetTickCount();
        return;
    }
    if (WallElapsed(now) > wallMs) Fail(stickFail);
}

void TickWaitSpend(DWORD now, const VitalsSnap& s) {
    if (MapMatches(gTrip.hangupMap, s.mapId) && s.ap == 0 &&
        (s.level < 10 || !AutoSkillOn())) {
        if (!HandoffHangup()) return;
        Enter(State::Done, "挂机图 AP 已空");
        return;
    }
    const bool astOn = AutoStatOn();
    if (!astOn || s.ap == 0) {
        if (!gApZeroSince) gApZeroSince = now;
    } else {
        gApZeroSince = 0;
    }
    const bool apDone = !astOn || (gApZeroSince && now - gApZeroSince >= 2000);
    bool spDone = true;
    if (s.level < 10) {
        if (!gSkipSkillLogged) {
            gSkipSkillLogged = true;
            runtime::LogI("CharBoot", "WaitSpend skip_skill lv=%d", s.level);
        }
        spDone = true;
    } else if (AutoSkillOn()) {
        if (gLastSp < 0) {
            gLastSp = s.sp;
            gSpHoldSince = now;
        } else if (s.sp != gLastSp) {
            gLastSp = s.sp;
            gSpHoldSince = now;
        }
        spDone = (gSpHoldSince && now - gSpHoldSince >= 5000) || WallElapsed(now) >= 15000;
    }
    SetMsg("等加点/技能");
    if ((apDone && spDone) || WallElapsed(now) >= kWaitSpendMs) {
        if (s.mapId == kMapLib)
            Enter(State::GotoHangup, "GotoHangup");
        else
            Enter(State::GotoHangup, "GotoHangup");
    }
}

void TickDone(DWORD now) {
    ApplyPause(State::Done);
    SetMsg("转职完成");
    if (WallElapsed(now) >= kDoneMs) {
        gState.store(static_cast<int>(State::Idle), std::memory_order_release);
        SetMsg("");
        const VitalsSnap snap = ReadSnap();
        PublishStatus(&snap);
        runtime::LogI("CharBoot", "state Done→Idle");
    }
}

void TickState(DWORD now, const VitalsSnap& s) {
    switch (Cur()) {
    case State::Idle:
        break;
    case State::Arm:
        TickArm(s);
        break;
    case State::GotoFarm:
        if (TickGoto(gTrip.farmMap, now, kGotoIslandMs, "GotoFarm") == GotoTick::Arrived)
            Enter(State::Farm, "GotoFarm → Farm");
        break;
    case State::Farm:
        TickFarm(now, s);
        break;
    case State::GotoHarbor:
        if (TickGoto(kMapHarbor, now, kGotoIslandMs, "GotoHarbor") == GotoTick::Arrived)
            Enter(State::BoardShip, "到港");
        break;
    case State::BoardShip:
        if (!gShipFareLocked) {
            if (s.meso >= 0 && s.meso < xcat::kCharBootShipFareMeso) {
                Fail("ship_fare");
                break;
            }
            if (s.meso < xcat::kCharBootShipFareMeso) {
                SetMsg("等待金币读取");
                break;
            }
            gShipFareLocked = true;
        }
        TickTalkLoop(now, s, kNpcSancks, kShipKeys, kMapVictoria, false, kBoardShipMs);
        break;
    case State::GotoEllinia:
        if (TickGoto(kMapEllinia, now, kGotoVicMs, "GotoEllinia") == GotoTick::Arrived)
            Enter(State::EnterLib, "进图书馆");
        break;
    case State::EnterLib:
        TickPortal(now, s, "jobin00", kMapLib, kMapEllinia, kEnterLibMs, "NO_PORTAL", "STICK_FAIL",
                   "FIRED_NO_WARP");
        break;
    case State::JobTalk:
        TickTalkLoop(now, s, kNpcHans, kHansKeys, 0, true, kJobTalkMs);
        break;
    case State::WaitJob:
        if (s.job == kJobMage) {
            runtime::LogI("CharBoot", "job 0→200");
            Enter(State::WaitSpend, "转职完成，等加点");
        } else if (WallElapsed(now) > kWaitJobMs) {
            Fail("wait_job");
        } else {
            SetMsg("等待职业变更");
        }
        break;
    case State::WaitSpend:
        TickWaitSpend(now, s);
        break;
    case State::ExitLib:
        TickPortal(now, s, "jobout00", kMapEllinia, kMapLib, kExitLibMs, "NO_PORTAL", "STICK_FAIL",
                   "FIRED_NO_WARP");
        if (Cur() == State::GotoHangup) gDidExitLib = true;
        break;
    case State::GotoHangup:
        if (TickGoto(gTrip.hangupMap, now, kGotoVicMs, "GotoHangup") == GotoTick::Arrived) {
            if (!HandoffHangup()) break;
            Enter(State::Done, "到挂机图");
        }
        break;
    case State::Done:
        TickDone(now);
        break;
    }
}

void BootstrapManualSeq(uint32_t seqOnDisk, const char* via) {
    if (gManualSeqBootstrapped) return;
    gManualSeqBootstrapped = true;
    gHandledManualSeq = seqOnDisk;
    runtime::LogI("CharBoot", "bootstrap manualSeq=%u via=%s (ignore pre-inject command)",
                  gHandledManualSeq, via ? via : "?");
}

void HotReadConfig() {
    xcat::CharBootConfig cfg{};
    if (!xcat::ReadCharBoot(runtime::GetBinDir(), cfg)) return;
    if (cfg.writeTickMs == gSeenCfgTick && gSeenCfgTick != 0) {
        if (cfg.manualSeq != gCfg.manualSeq) {
            gCfg.manualSeq = cfg.manualSeq;
            gCfg.manualKind = cfg.manualKind;
            runtime::LogI("CharBoot", "manualSeq catch-up seq=%u kind=%u", gCfg.manualSeq,
                          gCfg.manualKind);
        }
    } else {
        gSeenCfgTick = cfg.writeTickMs;
        gCfg = cfg;
    }
    if (!gManualSeqBootstrapped) BootstrapManualSeq(0, "reload-guard");
    if (gCfg.manualSeq == gHandledManualSeq) return;
    gHandledManualSeq = gCfg.manualSeq;
    if (gCfg.manualKind == xcat::kCharBootManualStop) {
        if (Cur() == State::Done) {
            ResetTripCounters();
            simple_combat::SetHardPause(simple_combat::HardPauseHolder::CharBoot, false);
            fly::SetExternalPause(false);
            gState.store(static_cast<int>(State::Idle), std::memory_order_release);
            SetMsg("");
            const VitalsSnap snap = ReadSnap();
            PublishStatus(&snap);
        } else if (Cur() != State::Idle) {
            Fail("user_stop");
        }
    } else if (gCfg.manualKind == xcat::kCharBootManualStart) {
        if (Cur() != State::Idle) {
            runtime::LogI("CharBoot", "ignore Start while busy state=%s", StateName(Cur()));
            return;
        }
        gTrip.farmMap = static_cast<int>(gCfg.farmMap);
        gTrip.hangupMap = static_cast<int>(gCfg.hangupMap);
        gTrip.departKind = gCfg.departKind;
        gTrip.levelMin = gCfg.levelMin;
        gTrip.mesoMin = gCfg.mesoMin;
        gTrip.farmTimeoutMin = gCfg.farmTimeoutMin;
        gLastWhy[0] = 0;
        Enter(State::Arm, "开始起号");
    }
}

void Tick(DWORD now) {
    HotReadConfig();
    VitalsSnap snap = ReadSnap();
    const bool frozen = Frozen();
    NoteFreeze(now, frozen);
    if (Cur() == State::Farm && !frozen && gLastTickMs && now > gLastTickMs)
        gFarmAccMs += (now - gLastTickMs);
    gLastTickMs = now;

    if (Cur() != State::Idle && Cur() != State::Arm) {
        if (Watchdog(now, snap)) return;
    }
    if (frozen && Cur() != State::Idle && Cur() != State::Arm && Cur() != State::Done) {
        StopApproachFly();
        SetMsg("冻态…");
        if (!gLastStatusMs || now - gLastStatusMs >= kStatusMs) PublishStatus(&snap);
        return;
    }
    TickState(now, snap);
    if (!gLastStatusMs || now - gLastStatusMs >= kStatusMs) {
        snap = ReadSnap();
        PublishStatus(&snap);
    }
}

DWORD WINAPI Worker(LPVOID) {
    runtime::LogI("CharBoot", "worker start");
    while (!gStop.load(std::memory_order_acquire)) {
        Tick(GetTickCount());
        const State s = Cur();
        const DWORD slp =
            (s == State::BoardShip || s == State::JobTalk) ? kApproachTickMs : kTickMs;
        Sleep(slp);
    }
    if (Cur() != State::Idle) {
        travel::RequestStop();
        ResetTripCounters();
        simple_combat::SetHardPause(simple_combat::HardPauseHolder::CharBoot, false);
        fly::SetExternalPause(false);
        gState.store(static_cast<int>(State::Idle), std::memory_order_release);
        SetMsg("");
        const VitalsSnap snap = ReadSnap();
        PublishStatus(&snap);
    }
    runtime::LogI("CharBoot", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    gState.store(static_cast<int>(State::Idle), std::memory_order_release);
    gSeenCfgTick = 0;
    gHandledManualSeq = 0;
    gManualSeqBootstrapped = false;
    gLastWhy[0] = 0;
    SetMsg("");
    xcat::CharBootConfig boot{};
    if (xcat::ReadCharBoot(runtime::GetBinDir(), boot)) {
        gCfg = boot;
        gSeenCfgTick = boot.writeTickMs;
        BootstrapManualSeq(boot.manualSeq, "init");
    } else {
        BootstrapManualSeq(0, "init-empty");
    }
    const VitalsSnap snap = ReadSnap();
    PublishStatus(&snap);
}

void Shutdown() {
    StopWorker();
    gManualSeqBootstrapped = false;
    gSeenCfgTick = 0;
}

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    gThread.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 20000);
        CloseHandle(th);
    }
}

bool IsBusy() { return Cur() != State::Idle; }

}  // namespace x::features::char_boot
