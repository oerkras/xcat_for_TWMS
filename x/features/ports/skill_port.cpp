// skill_port �?Classic TWMS skill presence / learn / cast for buffs.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "skill_port.h"

#include "player_combat_port.h"
#include "world_port.h"
#include "../skill_max_level/skill_max_level.h"
#include "../../ui/player_vitals.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"

#include "xcat_skill_names.h"

#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::skill {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Unity FindAll / get_gameObject / get_name → x::runtime::il2cpp::kRva*（il2cpp_bind.h SSOT）
// remounted 2026-08-06（GA 重哈希；IDB imagebase 0x7ff848c80000）
// GetSkillLevel：dump 被误标 GetMonsterCardCheckListSize；IDA xref→SkillInfo.GetSkillLevel 实锤。
constexpr uint32_t kRvaGetSkillLevel = 0x106D470;
// 公开总入口 DoActiveSkill(int skillId, uint scanCode=0) —— 经典版对标枫星 UseOnClientImmediate。
constexpr uint32_t kRvaDoActiveSkill = 0x106EC00;
constexpr uint32_t kRvaDoActiveSkillPrepare = 0x10B0BB0;
constexpr uint32_t kRvaSendSkillUseRequest = 0x10C5590;
// SkillInfo.GetSkill(int) → SkillEntry（Singleton；勿用 ItemDataManager）
constexpr uint32_t kRvaSkillInfoGetSkill = 0x1578050;
// SecondaryStat.GetRemainTime(int nSkillID, int tCur) —— 返回剩余 ms（tCur 必须是游戏钟）。
constexpr uint32_t kRvaSecondaryStatGetRemainTime = 0xD67CF0;
// WorldManager.get_GetUpdateTime() —— (int)(_updateTime * 1000)；tXxx_/CoolTimeOver 同此钟。
constexpr uint32_t kRvaWorldManagerGetUpdateTime = 0xDC3D20;
// CharacterData.GetSkillCoolTimeOver / IsExist —— IDA：`[this+0x70]` + Dict.TryGetValue。
// 禁止手扫 entries 容量：BIN 会命中残留脏槽（over≈GetTickCount 量级）。
constexpr uint32_t kRvaGetSkillCoolTimeOver = 0x12E76C0;
constexpr uint32_t kRvaIsExistSkillCoolTimeOver = 0x12E8F40;

// 方法哈希（dump 名；RVA 漂时优先）
constexpr char kHashGetSkillLevel[] =
    "e3f94beec124905fdceee7be877c5006e976256d338613487b632a8015ff251";
constexpr char kHashDoActiveSkill[] =
    "eb301d84ff17b393a674e50db3efe41819693d74d425ee1835d2b166a7db88e";
constexpr char kHashDoActiveSkillPrepare[] =
    "b082cfe019659011df33ecd78be10af7a36fc4720508cdb0e59badaca5c1245";
// SendSkillUseRequest(SkillEntry,int,uint,int,int[],int) —— dump.cs.restored 命名。
constexpr char kHashSendSkillUseRequest[] =
    "b6fe7c0879ab94d06d8281217e7cc1911f4caf7adfa422d8f9dbfab0350cae6";
constexpr char kHashSkillInfoGetSkill[] =
    "d2aafa262375bd237687bbed81c937d7a1bbedd2d79f3b533f190585bbf203d";
constexpr char kHashGetRemainTime[] =
    "bd73b5ae97312d3160dd6a0fd7f0490c5d6589487136272f9b73cb8d56bf1f2";
constexpr char kHashGetUpdateTime[] =
    "fef21c96e8a274f3b1aa04ac1c45bd9c6c4364275902033cf3906e5ffb72bfd";
constexpr char kHashGetSkillCoolTimeOver[] =
    "cd719e624b31699977c8ac50c1573f4de40e85b36e0ffd06b91541c8918ceae";
constexpr char kHashIsExistSkillCoolTimeOver[] =
    "bf1430505cec7a2f93e1ef036aa6e52522be66a9540973a8a45844353955148";
// UserLocal：il2cpp_shape::ResolveUserLocalKlass
// SkillInfo（Singleton；勿与 struct SkillInfo / ItemDataManager 混淆）
constexpr char kSkillInfoClass[] =
    "e4c1bb085eea897cbd36c2ecc9a50b9316187a7ed2fbb7654ad8e162c289c39";
// SkillEntry
constexpr char kSkillEntryClass[] =
    "cf6d6169272f7c4a4dbb084cc7786a67fed9c03d7376babdcb5e5ecdde00eef";
// WM+0xF0 SecondaryStat
constexpr char kSecondaryStatClass[] =
    "fda0a837975e9b385db9604d6689232d1f1783dcfafa16403a92309b5604df3";
// CharacterData（WM+0xE0）
constexpr char kCharacterDataClass[] =
    "d5453e03707efd1001d8348a46ee270f8117468d2f1504fd0dadd0cc7c10468";

// CharacterData / SkillRecord / Cooltime / SecondaryStat / MyUser → x::ui::player（hash 防漂）。
// UserLocal 在身 / Prepare / Pos：EnsureSkillFieldOffsets（明文/hash → field_get_offset）。
// remount 2026-08-06：字段哈希全换；偏移仍 0x330/0x398/0x64/0x240 / SE 0x10/0x18 / SI Dict 0x10
constexpr size_t kFbAffectedList = 0x330;
constexpr size_t kFbPreparingSkillId = 0x398;  // valuetype.SkillID@+0
constexpr size_t kFbAffSkillId = 0x10;         // User.AffectedSkillEntry.nSkillID
constexpr size_t kFbAffStartTime = 0x14;       // tStart
constexpr size_t kFbVisPos = 0x64;             // FieldActorBase.Pos
constexpr size_t kFbLogicalPos = 0x240;        // LocalUser.CurPos
constexpr size_t kFbSkillInfoDict = 0x10;      // SkillInfo 主技能字典
constexpr size_t kFbSkillId = 0x10;            // SkillEntry
constexpr size_t kFbSkillName = 0x18;
constexpr char kFldAffectedList[] = "_listAffectedSkillEntry";
constexpr char kHashAffectedList[] =
    "e069ac89882d70b7bcc31b59dce8344ce819e20e01d24a713448ed3fcbdf4f7";
constexpr char kHashPreparingSkill[] =
    "dec39f8bf2f14a0373c693fe2d400efec8ea70d4452e6540720201867b64dda";
constexpr char kFldAffSkillId[] = "nSkillID";
constexpr char kFldAffStartTime[] = "tStart";
constexpr char kHashAffSkillId[] =
    "ed13762989ecfaf5826b4709ae1182f0e66f4c67e33639b59305df32d06537a";
constexpr char kHashAffStartTime[] =
    "d0696abe75ff63c5f5414e0bdb55650ba4fba50bdde2d5c49587535ab5f1f58";
// 与 invuln / shop_port 同 hash（运行时 meta；dump 或已还原明文 Pos/CurPos）
constexpr char kHashFldVisPos[] =
    "cc96f38a9acbe6b4e8005a2d56a7846324bc67690c2059661962502f74b928a";
constexpr char kHashFldLogicalPos[] =
    "c4adef19821f3737cd477a7840968c11697f4afd8eb8696cafb37d1c297b926";
constexpr char kFldVisPosPlain[] = "Pos";
constexpr char kFldLogicalPosPlain[] = "CurPos";
constexpr char kHashSkillInfoDict[] =
    "f5e87e4bde2ef764aef0ee5887dad4e4b9ddaa65c50673584fcfcca0aceed47";
constexpr char kHashSkillEntryId[] =
    "a99a5742695f0a27f2537db5be71ed1d5ce66e5f137044daa537724d417ce1b";
constexpr char kHashSkillEntryName[] =
    "fa2603294a091b2c69c44cc55e80b8ec69144dad08dd0a487178a4fc457f25b";
constexpr size_t kOffCachedPtr = 0x10;

// Dictionary Entry / Il2CppArray → il2cpp_container SSOT（valuetype 槽按 K/V 择优）
#define kEntrySizeIntIntTight (x::runtime::il2cpp_container::DictEntryStrideIntIntTight())
#define kEntrySizeIntPtr (x::runtime::il2cpp_container::DictEntryStrideIntPtr())
#define kOffArrLen (x::runtime::il2cpp_container::OffArrayMaxLength())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())
#define kValOffTight (x::runtime::il2cpp_container::OffDictEntryValueIntTight())
#define kValOffAlign (x::runtime::il2cpp_container::OffDictEntryValueIntAlign())
#define kOffEntryKey (x::runtime::il2cpp_container::OffDictEntryKey())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())

constexpr DWORD kRebindMs = 3000;
constexpr DWORD kJobWaitMs = 2000;
constexpr float kMinPosAbs = 1.0f;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);
using FnGetSkillLevel = int (*)(void* self, int skillId, const void* methodInfo);
using FnGetSkill = void* (*)(void* self, int skillId, const void* methodInfo);
using FnDoActiveSkill = bool (*)(void* self, int skillId, uint32_t scanCode, const void* methodInfo);
using FnPrepare = bool (*)(void* self, void* skill, int level, uint32_t scanCode,
                           const void* methodInfo);
// bool SendSkillUseRequest(SkillEntry, int pPet, uint pt=0, int nSLV=-1, int[] weapons, int charge=0)
using FnSendSkillUse = bool (*)(void* self, void* skillEntry, int pPet, uint32_t pt, int nSlv,
                                void* weaponItemIds, int chargeSkillId, const void* methodInfo);
using FnGetRemainTime = int (*)(void* self, int skillId, int tCur, const void* methodInfo);
using FnGetUpdateTime = int (*)(const void* methodInfo);
using FnGetSkillCoolTimeOver = int (*)(void* self, int skillId, const void* methodInfo);
using FnIsExistSkillCoolTimeOver = bool (*)(void* self, int skillId, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;
FnGetSkillLevel gGetSkillLevel = nullptr;
FnGetSkill gGetSkill = nullptr;
FnDoActiveSkill gDoActiveSkill = nullptr;
FnPrepare gPrepare = nullptr;
FnSendSkillUse gSendSkillUse = nullptr;
FnGetRemainTime gGetRemainTime = nullptr;
FnGetUpdateTime gGetUpdateTime = nullptr;
FnGetSkillCoolTimeOver gGetSkillCoolTimeOver = nullptr;
FnIsExistSkillCoolTimeOver gIsExistSkillCoolTimeOver = nullptr;

void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gSkillInfoKlass = nullptr;
void* gSkillInfo = nullptr;
void* gLocalUserKlass = nullptr;
void* gSecondaryStatKlass = nullptr;
void* gWorldManagerKlass = nullptr;
void* gCharacterDataKlass = nullptr;
MethodInfoHead* gMiGetSkillLevel = nullptr;
MethodInfoHead* gMiGetSkill = nullptr;
MethodInfoHead* gMiDoActiveSkill = nullptr;
MethodInfoHead* gMiPrepare = nullptr;
MethodInfoHead* gMiSendSkillUse = nullptr;
MethodInfoHead* gMiGetRemainTime = nullptr;
MethodInfoHead* gMiGetUpdateTime = nullptr;
MethodInfoHead* gMiGetSkillCoolTimeOver = nullptr;
MethodInfoHead* gMiIsExistSkillCoolTimeOver = nullptr;

DWORD gLastLuRebind = 0;
DWORD gLastSiRebind = 0;
DWORD gLastDictDiagMs = 0;
DWORD gLastAffDiagMs = 0;
DWORD gLastRemainDiagMs = 0;
DWORD gLastCdDiagMs = 0;
std::atomic<bool> gBound{false};

// CoolTimeOver 对 1001/1002 等会返回脏绝对时刻 → corrupt→0；
// 施放成功后用表内冷却做本地倒数（GetTickCount），与有效 Over 取 max。
std::mutex gLocalCdMu;
std::unordered_map<int, DWORD> gLocalCdEndTick;  // skillId → 到期 GetTickCount
std::unordered_map<int, float> gTableCdSec;      // skillId → 表内总 CD 秒
bool gTableCdTried = false;

float TableCooltimeSec(int skillId);
void NoteLocalCooldownAfterCast(int skillId);
float LocalCooldownRemainSec(int skillId);

size_t gOffAffectedList = kFbAffectedList;
size_t gOffPreparingSkillId = kFbPreparingSkillId;
size_t gOffAffSkillId = kFbAffSkillId;
size_t gOffAffStartTime = kFbAffStartTime;
size_t gOffVisPos = kFbVisPos;
size_t gOffLogicalPos = kFbLogicalPos;
size_t gOffSkillInfoDict = kFbSkillInfoDict;
size_t gOffSkillId = kFbSkillId;
size_t gOffSkillName = kFbSkillName;
bool gSkillFieldOffTried = false;

void EnsureSkillFieldOffsets();

struct CastJob {
    int skillId = 0;
    bool preferSendUse = false;  // 可选：先试 SendSkillUseRequest
    bool ok = false;
    bool notReady = false;
    char reason[48]{};
};

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint16_t ReadU16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
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

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

void* KlassStaticFields(void* klass) {
    if (!klass || !gClassStaticData) return nullptr;
    __try {
        return gClassStaticData(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    // Lazy<T>：value 常见 @+0x08/+0x10/+0x18/+0x20/+0x28（对齐 drop_pool）。
    const size_t offs[] = {0x08, 0x10, 0x18, 0x20, 0x28};
    for (size_t off : offs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

bool UnityObjectAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    if (!ReadPtr(obj, 0)) return false;
    return ReadPtr(obj, kOffCachedPtr) != nullptr;
}

// Worker-safe MyUser check. GetGoName → managed call → GC "unknown thread".
bool LooksLikeMyUserAlive(void* user) { return UnityObjectAlive(user); }

bool ReadIl2CppString(void* strObj, char* out, int outSz) {
    if (!strObj || !out || outSz <= 0) return false;
    out[0] = 0;
    __try {
        const int len = ReadI32(strObj, 0x10);
        if (len <= 0 || len > 200) return false;
        const char16_t* chars =
            reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(strObj) + 0x14);
        int n = 0;
        for (int i = 0; i < len && n + 1 < outSz; ++i) {
            const char16_t c = chars[i];
            if (c < 128)
                out[n++] = static_cast<char>(c);
            else if (c < 0x800 && n + 2 < outSz) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            } else if (n + 3 < outSz) {
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

int ListSize(void* list) {
    if (!list) return 0;
    return ReadI32(list, x::runtime::il2cpp_container::OffListSize());
}

void* ListItems(void* list) {
    return ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
}

void* ArrayAtPtr(void* arr, int i) {
    if (!arr || i < 0) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
                                         static_cast<size_t>(i) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ResolveApi() {
    if (gGA && gFindAll && gGetSkillLevel && gPrepare && gGetSkill) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gCompGo = e.compGo;
    gObjName = e.objName;
    gClassGetMethods = e.classGetMethods;
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gGetSkillLevel = x::runtime::il2cpp::AtRva<FnGetSkillLevel>(kRvaGetSkillLevel);
    gDoActiveSkill = x::runtime::il2cpp::AtRva<FnDoActiveSkill>(kRvaDoActiveSkill);
    gPrepare = x::runtime::il2cpp::AtRva<FnPrepare>(kRvaDoActiveSkillPrepare);
    gSendSkillUse = x::runtime::il2cpp::AtRva<FnSendSkillUse>(kRvaSendSkillUseRequest);
    gGetSkill = x::runtime::il2cpp::AtRva<FnGetSkill>(kRvaSkillInfoGetSkill);
    gGetRemainTime =
        x::runtime::il2cpp::AtRva<FnGetRemainTime>(kRvaSecondaryStatGetRemainTime);
    gGetUpdateTime =
        x::runtime::il2cpp::AtRva<FnGetUpdateTime>(kRvaWorldManagerGetUpdateTime);
    gGetSkillCoolTimeOver =
        x::runtime::il2cpp::AtRva<FnGetSkillCoolTimeOver>(kRvaGetSkillCoolTimeOver);
    gIsExistSkillCoolTimeOver =
        x::runtime::il2cpp::AtRva<FnIsExistSkillCoolTimeOver>(kRvaIsExistSkillCoolTimeOver);
    return gFindAll && gGetSkillLevel && gDoActiveSkill && gPrepare && gGetSkill;
}

bool LocalUserStillAlive() {
    // Worker-safe: raw reads only. GetGoName → GC "Collecting from unknown thread".
    if (!gLocalUser || !UnityObjectAlive(gLocalUser)) return false;
    void* mu = x::ui::player::LocalMyUser();
    if (UnityObjectAlive(mu) && mu != gLocalUser) return false;
    if (UnityObjectAlive(mu) && mu == gLocalUser) return true;
    __try {
        const float vx = ReadF32(gLocalUser, gOffVisPos);
        const float vy = ReadF32(gLocalUser, gOffVisPos + 4);
        const float lx = ReadF32(gLocalUser, gOffLogicalPos);
        const float ly = ReadF32(gLocalUser, gOffLogicalPos + 4);
        if (fabsf(vx) < kMinPosAbs && fabsf(vy) < kMinPosAbs && fabsf(lx) < kMinPosAbs &&
            fabsf(ly) < kMinPosAbs)
            return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryResolveLocalUser(DWORD now) {
    if (LocalUserStillAlive()) return true;

    bool forceRebind = false;
    if (gLocalUser) {
        void* mu = x::ui::player::LocalMyUser();
        if (LooksLikeHeapPtr(mu) && mu != gLocalUser) forceRebind = true;
    }
    gLocalUser = nullptr;

    // InterStage / 卸图：禁 FindAll；MyUser 未就绪则等 PlayReady（对齐黑屏主线程让路）。
    if (!world::IsPlayReady()) return false;

    if (!forceRebind && gLastLuRebind && now - gLastLuRebind < kRebindMs) return false;
    gLastLuRebind = now;

    // 优先 WM.MyUser（与 drop/combat/travel / vitals 同真源）。禁 GetGoName（worker）。
    if (world::EnsureBound()) {
        void* mu = x::ui::player::LocalMyUser();
        if (LooksLikeMyUserAlive(mu)) {
            gLocalUser = mu;
            runtime::LogI("SkillPort", "LocalUser ACCEPT wm.MyUser=%p", gLocalUser);
            return true;
        }
    }

    if (!gFindAll) return false;
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType) return false;
    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!arr) return false;
    const int n = ReadI32(arr, kOffArrLen);
    void* best = nullptr;
    for (int i = 0; i < n && i < 64; ++i) {
        void* cand = ArrayAtPtr(arr, i);
        // FindAll 已按 UserLocal 类型过滤；worker 上只做存活校验，禁 GetGoName。
        if (!LooksLikeMyUserAlive(cand)) continue;
        best = cand;
        break;
    }
    gLocalUser = best;
    if (gLocalUser) runtime::LogI("SkillPort", "LocalUser ACCEPT FindAll=%p", gLocalUser);
    return gLocalUser != nullptr;
}

bool LooksLikeSkillInfo(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gSkillInfoKlass && !ObjKlassIs(cand, gSkillInfoKlass)) return false;
    // SkillInfo 主技能字典（meta；fb@+0x10）
    return LooksLikeHeapPtr(ReadPtr(cand, gOffSkillInfoDict));
}

void* ResolveSkillInfoSingleton(DWORD now) {
    EnsureSkillFieldOffsets();
    // 同 DropPool：离图/换图空窗撕 singleton 是常态；清负缓存免进图 3s 盲区，勿刷 W。
    if (!world::IsPlayReady()) {
        gSkillInfo = nullptr;
        gLastSiRebind = 0;
        return nullptr;
    }
    // MyUser 暂空 = 换图中（与 DropPort LocalUser 同口径）。
    {
        void* mu = x::ui::player::LocalMyUser();
        if (!LooksLikeHeapPtr(mu)) {
            gSkillInfo = nullptr;
            gLastSiRebind = 0;
            return nullptr;
        }
    }

    if (LooksLikeSkillInfo(gSkillInfo)) return gSkillInfo;
    // 仅玩法就绪 miss 短退避；勿把失败指针再 return 出去。
    if (gLastSiRebind && now - gLastSiRebind < kRebindMs && !gSkillInfo) return nullptr;
    gLastSiRebind = now;
    gSkillInfo = nullptr;
    if (!gSkillInfoKlass) gSkillInfoKlass = FindClass(kSkillInfoClass);
    if (!gSkillInfoKlass) return nullptr;
    // No RuntimeClassInit on caller thread — EnsureBound/resolve often run on workers
    // (buffs/multi_skill). Nested managed_main::Call from CastJobFn (already on main)
    // would also deadlock the pump. Read statics only; game cctor owns init.

    void* staticsKlass = gSkillInfoKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gSkillInfoKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) staticsKlass = parent;
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gSkillInfoKlass);

    void* best = nullptr;
    if (statics) {
        // Singleton<T>.Lazy 通常在 parent statics +0；扫宽对齐 drop_pool。
        for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
            void* lazy = ReadPtr(statics, s);
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (LooksLikeSkillInfo(cand)) {
                best = cand;
                break;
            }
        }
    }

    // 再扫 SkillInfo 自身 statics（部分构建 Lazy 不在 parent）。
    if (!best) {
        void* own = KlassStaticFields(gSkillInfoKlass);
        if (own && own != statics) {
            for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
                void* lazy = ReadPtr(own, s);
                void* cand = TryLazyValue(lazy);
                if (!cand) cand = lazy;
                if (LooksLikeSkillInfo(cand)) {
                    best = cand;
                    break;
                }
            }
        }
    }

    if (best) {
        gSkillInfo = best;
        runtime::LogI("SkillPort", "SkillInfo ACCEPT si=%p", gSkillInfo);
        return gSkillInfo;
    }
    // 玩法就绪仍扫不到才告警；换图空窗已在入口 return。
    runtime::LogWThrottled(31, 15000, "SkillPort", "SkillInfo resolve miss statics=%p klass=%p",
                           statics, gSkillInfoKlass);
    return nullptr;
}

void* CharacterData() { return x::ui::player::LocalCharacterData(); }

bool PlausibleUserOff(size_t off) { return off >= 0x10 && off < 0x1000; }

size_t FieldOffWalk(void* klass, const char* name) {
    if (!klass || !name || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    for (void* k = klass; k;) {
        if (e.classGetFieldFromName && e.fieldGetOffset) {
            void* field = nullptr;
            __try {
                field = e.classGetFieldFromName(k, name);
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
                if (PlausibleUserOff(off)) return off;
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

void* FindAffectedEntryKlass(void* ulKlass) {
    if (!ulKlass) return nullptr;
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (!ga) return nullptr;
    using FnClassGetNestedTypes = void* (*)(void* klass, void** iter);
    auto nested = reinterpret_cast<FnClassGetNestedTypes>(
        GetProcAddress(ga, "il2cpp_class_get_nested_types"));
    const auto& e = x::runtime::il2cpp::Get();
    if (!nested || !e.classGetFieldFromName) return nullptr;
    for (void* k = ulKlass; k;) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* nk = nested(k, &iter);
                if (!nk) break;
                void* fId = nullptr;
                void* fStart = nullptr;
                __try {
                    fId = e.classGetFieldFromName(nk, kHashAffSkillId);
                    if (!fId) fId = e.classGetFieldFromName(nk, kFldAffSkillId);
                    fStart = e.classGetFieldFromName(nk, kHashAffStartTime);
                    if (!fStart) fStart = e.classGetFieldFromName(nk, kFldAffStartTime);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    fId = nullptr;
                    fStart = nullptr;
                }
                if (fId && fStart) return nk;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return nullptr;
}

void EnsureSkillFieldOffsets() {
    // 冷启动可能早于 UserLocal / nested AffEntry；不满 hits 时允许重试（勿一次钉死）。
    static int sLastHits = -1;
    constexpr int kExpect = 9;
    if (gSkillFieldOffTried && sLastHits >= kExpect) return;
    if (!x::runtime::il2cpp::Ensure()) {
        if (!gSkillFieldOffTried) {
            runtime::LogW("SkillPort", "field off: bind miss — dump fallback");
        }
        gSkillFieldOffTried = true;
        return;
    }
    x::runtime::il2cpp_container::Ensure();
    void* ul = gLocalUserKlass;
    if (!ul) ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (ul) gLocalUserKlass = ul;

    int hits = 0;
    auto apply = [&](size_t got, size_t fb, size_t* out) {
        if (got) {
            *out = got;
            ++hits;
        } else {
            *out = fb;
        }
    };
    auto resolve2 = [&](void* k, const char* a, const char* b, size_t fb, size_t* out) {
        size_t got = FieldOffWalk(k, a);
        if (!got && b) got = FieldOffWalk(k, b);
        apply(got, fb, out);
    };

    if (ul) {
        resolve2(ul, kHashAffectedList, kFldAffectedList, kFbAffectedList, &gOffAffectedList);
        apply(FieldOffWalk(ul, kHashPreparingSkill), kFbPreparingSkillId, &gOffPreparingSkillId);
        resolve2(ul, kHashFldVisPos, kFldVisPosPlain, kFbVisPos, &gOffVisPos);
        resolve2(ul, kHashFldLogicalPos, kFldLogicalPosPlain, kFbLogicalPos, &gOffLogicalPos);

        void* affKlass = FindAffectedEntryKlass(ul);
        if (affKlass) {
            resolve2(affKlass, kHashAffSkillId, kFldAffSkillId, kFbAffSkillId, &gOffAffSkillId);
            resolve2(affKlass, kHashAffStartTime, kFldAffStartTime, kFbAffStartTime,
                     &gOffAffStartTime);
        }
    } else if (!gSkillFieldOffTried) {
        runtime::LogW("SkillPort", "field off: UserLocal klass miss — user fb only");
    }

    if (!gSkillInfoKlass) gSkillInfoKlass = FindClass(kSkillInfoClass);
    if (gSkillInfoKlass) {
        apply(FieldOffWalk(gSkillInfoKlass, kHashSkillInfoDict), kFbSkillInfoDict, &gOffSkillInfoDict);
    }
    void* seKlass = FindClass(kSkillEntryClass);
    if (seKlass) {
        apply(FieldOffWalk(seKlass, kHashSkillEntryId), kFbSkillId, &gOffSkillId);
        apply(FieldOffWalk(seKlass, kHashSkillEntryName), kFbSkillName, &gOffSkillName);
    }

    gSkillFieldOffTried = true;
    if (hits != sLastHits) {
        sLastHits = hits;
        runtime::LogI(
            "SkillPort",
            "field off hits=%d/%d aff=0x%zX prep=0x%zX pos=0x%zX cur=0x%zX siDict=0x%zX "
            "seId=0x%zX seName=0x%zX",
            hits, kExpect, gOffAffectedList, gOffPreparingSkillId, gOffVisPos, gOffLogicalPos,
            gOffSkillInfoDict, gOffSkillId, gOffSkillName);
    }
}

int DictIntIntCount(void* dict) {
    if (!dict) return 0;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    const int count = ReadI32(dict, x::runtime::il2cpp_container::OffDictCount());
    const int freeCount = ReadI32(dict, x::runtime::il2cpp_container::OffDictFreeCount());
    int n = count - freeCount;
    if (n < 0) n = count;
    return n;
}

bool LooksLikePlayerSkillId(int id) {
    // 初心者 1000+；职业技通常 7~8 位。排除明显脏键。
    return id >= 1000 && id <= 99999999;
}

bool LooksLikeSkillLevel(int lv) { return lv > 0 && lv <= 60; }

int ScoreIntIntLayout(void* entries, int len, size_t stride, size_t valOff) {
    int score = 0;
    if (!entries || len <= 0 || stride < 16) return 0;
    const int n = len < 4096 ? len : 4096;
    for (int i = 0; i < n; ++i) {
        uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, stride);
        if (!e) continue;
        __try {
            const int hash = *reinterpret_cast<int*>(e + kOffEntryHash);
            if (hash < 0) continue;
            const int key = *reinterpret_cast<int*>(e + kOffEntryKey);
            const int val = *reinterpret_cast<int*>(e + valOff);
            if (LooksLikePlayerSkillId(key) && LooksLikeSkillLevel(val)) ++score;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return score;
}

// Walk Dictionary<int,int>；自动在 tight(value@12) / align(value@16) 间择优。
template <typename Fn>
void ForEachDictIntInt(void* dict, Fn&& fn) {
    if (!dict) return;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, x::runtime::il2cpp_container::OffDictEntries());
    if (!entries) return;
    const int len = ReadI32(entries, kOffArrLen);
    if (len <= 0) return;

    const int scoreTight = ScoreIntIntLayout(entries, len, kEntrySizeIntIntTight, kValOffTight);
    const int scoreAlign = ScoreIntIntLayout(entries, len, kEntrySizeIntPtr, kValOffAlign);
    const size_t stride = (scoreAlign > scoreTight) ? kEntrySizeIntPtr : kEntrySizeIntIntTight;
    const size_t valOff = (stride == kEntrySizeIntPtr) ? kValOffAlign : kValOffTight;

    for (int i = 0; i < len && i < 4096; ++i) {
        uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, stride);
        if (!e) continue;
        __try {
            const int hash = *reinterpret_cast<int*>(e + kOffEntryHash);
            if (hash < 0) continue;
            const int key = *reinterpret_cast<int*>(e + kOffEntryKey);
            const int val = *reinterpret_cast<int*>(e + valOff);
            if (key <= 0) continue;
            fn(key, val);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGA) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(AtRva<void*>(rva));
    // 本类 + 父类链（DoActive/Prepare 可能声明在 UserLocal 基类上）。
    void* cur = klass;
    for (int depth = 0; cur && depth < 16; ++depth) {
        void* iter = nullptr;
        for (;;) {
            void* miRaw = nullptr;
            __try {
                miRaw = gClassGetMethods(cur, &iter);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            if (reinterpret_cast<uintptr_t>(mi->methodPointer) == want) return mi;
        }
        if (!gClassParent) break;
        void* parent = nullptr;
        __try {
            parent = gClassParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (!parent || parent == cur) break;
        cur = parent;
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
                          const char* plain, const char* hash,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

void EnsureMethodInfos() {
    if (!gLocalUserKlass) gLocalUserKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (!gSkillInfoKlass) gSkillInfoKlass = FindClass(kSkillInfoClass);
    if (!gSecondaryStatKlass) gSecondaryStatKlass = FindClass(kSecondaryStatClass);
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;

    int hashHits = 0;
    auto fill = [&](MethodInfoHead*& slot, void* klass, uint32_t rva, const MethodShape& shape,
                    const char* plain, const char* hash) {
        if (slot || !klass) return;
        ResolvePath path = ResolvePath::Miss;
        slot = ResolveMi(klass, rva, shape, plain, hash, &path);
        if (slot && path == ResolvePath::Hash) ++hashHits;
    };

    if (gLocalUserKlass) {
        constexpr MethodShape kLv{1, TypeKind::I32, true, true, {TypeKind::I32}};
        fill(gMiGetSkillLevel, gLocalUserKlass, kRvaGetSkillLevel, kLv, "GetSkillLevel",
             kHashGetSkillLevel);
        constexpr MethodShape kDo{2, TypeKind::Bool, true, true, {TypeKind::I32, TypeKind::U32}};
        fill(gMiDoActiveSkill, gLocalUserKlass, kRvaDoActiveSkill, kDo, "DoActiveSkill",
             kHashDoActiveSkill);
        constexpr MethodShape kPrep{
            3, TypeKind::Bool, true, true, {TypeKind::Ptr, TypeKind::I32, TypeKind::U32}};
        fill(gMiPrepare, gLocalUserKlass, kRvaDoActiveSkillPrepare, kPrep, "DoActiveSkillPrepare",
             kHashDoActiveSkillPrepare);
        constexpr MethodShape kSend{6,
                                    TypeKind::Bool,
                                    true,
                                    true,
                                    {TypeKind::Ptr, TypeKind::I32, TypeKind::U32, TypeKind::I32}};
        fill(gMiSendSkillUse, gLocalUserKlass, kRvaSendSkillUseRequest, kSend,
             "SendSkillUseRequest", kHashSendSkillUseRequest);
        if (!gMiSendSkillUse)
            gMiSendSkillUse = FindMethodByRva(gLocalUserKlass, kRvaSendSkillUseRequest);
        if (gMiSendSkillUse && gMiSendSkillUse->methodPointer) {
            gSendSkillUse = reinterpret_cast<FnSendSkillUse>(gMiSendSkillUse->methodPointer);
        }
    }
    {
        constexpr MethodShape kGet{1, TypeKind::Ptr, true, false, {TypeKind::I32}};
        fill(gMiGetSkill, gSkillInfoKlass, kRvaSkillInfoGetSkill, kGet, "GetSkill",
             kHashSkillInfoGetSkill);
    }
    {
        constexpr MethodShape kRem{2, TypeKind::I32, true, false, {TypeKind::I32, TypeKind::I32}};
        fill(gMiGetRemainTime, gSecondaryStatKlass, kRvaSecondaryStatGetRemainTime, kRem,
             "GetRemainTime", kHashGetRemainTime);
    }
    if (!gWorldManagerKlass)
        gWorldManagerKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    {
        constexpr MethodShape kGut{0, TypeKind::I32, false, false};
        fill(gMiGetUpdateTime, gWorldManagerKlass, kRvaWorldManagerGetUpdateTime, kGut,
             "GetUpdateTime", kHashGetUpdateTime);
    }
    if (!gCharacterDataKlass) gCharacterDataKlass = FindClass(kCharacterDataClass);
    if (!gCharacterDataKlass) {
        void* cd = CharacterData();
        if (LooksLikeHeapPtr(cd)) gCharacterDataKlass = ReadPtr(cd, 0);
    }
    if (gCharacterDataKlass) {
        constexpr MethodShape kOver{1, TypeKind::I32, true, false, {TypeKind::I32}};
        fill(gMiGetSkillCoolTimeOver, gCharacterDataKlass, kRvaGetSkillCoolTimeOver, kOver,
             "GetSkillCoolTimeOver", kHashGetSkillCoolTimeOver);
        constexpr MethodShape kEx{1, TypeKind::Bool, true, false, {TypeKind::I32}};
        fill(gMiIsExistSkillCoolTimeOver, gCharacterDataKlass, kRvaIsExistSkillCoolTimeOver, kEx,
             "IsExistSkillCoolTimeOver", kHashIsExistSkillCoolTimeOver);
    }
    // 函数指针与 MI 对齐（防只更 RVA 常量）。
    if (gMiGetSkillLevel) gGetSkillLevel = FnFromMi<FnGetSkillLevel>(gMiGetSkillLevel, kRvaGetSkillLevel);
    if (gMiDoActiveSkill) gDoActiveSkill = FnFromMi<FnDoActiveSkill>(gMiDoActiveSkill, kRvaDoActiveSkill);
    if (gMiPrepare) gPrepare = FnFromMi<FnPrepare>(gMiPrepare, kRvaDoActiveSkillPrepare);
    if (gMiGetSkill) gGetSkill = FnFromMi<FnGetSkill>(gMiGetSkill, kRvaSkillInfoGetSkill);
    if (gMiGetRemainTime)
        gGetRemainTime = FnFromMi<FnGetRemainTime>(gMiGetRemainTime, kRvaSecondaryStatGetRemainTime);
    if (gMiGetUpdateTime)
        gGetUpdateTime = FnFromMi<FnGetUpdateTime>(gMiGetUpdateTime, kRvaWorldManagerGetUpdateTime);
    if (gMiGetSkillCoolTimeOver)
        gGetSkillCoolTimeOver =
            FnFromMi<FnGetSkillCoolTimeOver>(gMiGetSkillCoolTimeOver, kRvaGetSkillCoolTimeOver);
    if (gMiIsExistSkillCoolTimeOver)
        gIsExistSkillCoolTimeOver = FnFromMi<FnIsExistSkillCoolTimeOver>(
            gMiIsExistSkillCoolTimeOver, kRvaIsExistSkillCoolTimeOver);

    static bool sLogged = false;
    const int hits = (gMiGetSkillLevel ? 1 : 0) + (gMiDoActiveSkill ? 1 : 0) + (gMiPrepare ? 1 : 0) +
                     (gMiSendSkillUse ? 1 : 0) + (gMiGetSkill ? 1 : 0) + (gMiGetRemainTime ? 1 : 0) +
                     (gMiGetUpdateTime ? 1 : 0) + (gMiGetSkillCoolTimeOver ? 1 : 0) +
                     (gMiIsExistSkillCoolTimeOver ? 1 : 0);
    if (!sLogged && hits > 0) {
        sLogged = true;
        x::runtime::LogI("Skill",
                         "methods path=%s hits=%d/9 hash=%d lv=%d do=%d prep=%d send=%d get=%d "
                         "remain=%d upd=%d cool=%d exist=%d",
                         hashHits >= 8 ? "meta" : (hashHits ? "meta-partial" : "rva/kind"), hits,
                         hashHits, gMiGetSkillLevel ? 1 : 0, gMiDoActiveSkill ? 1 : 0,
                         gMiPrepare ? 1 : 0, gMiSendSkillUse ? 1 : 0, gMiGetSkill ? 1 : 0,
                         gMiGetRemainTime ? 1 : 0, gMiGetUpdateTime ? 1 : 0,
                         gMiGetSkillCoolTimeOver ? 1 : 0, gMiIsExistSkillCoolTimeOver ? 1 : 0);
    }
}

void SetJobReason(CastJob* job, const char* why) {
    if (!job) return;
    strncpy_s(job->reason, why ? why : "fail", _TRUNCATE);
}

// 游戏逻辑钟：WorldManager.GetUpdateTime = (int)(_updateTime * 1000)。
// tXxx_ / SkillCoolTimeOver / GetRemainTime(tCur) 均用此钟；禁止用 GetTickCount 当 tCur。
int GameUpdateTimeMsImpl() {
    EnsureMethodInfos();
    if (gGetUpdateTime) {
        int t = 0;
        __try {
            t = gGetUpdateTime(gMiGetUpdateTime);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            t = 0;
        }
        if (t > 0) return t;
    }
    if (!gWorldManagerKlass)
        gWorldManagerKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    void* sf = KlassStaticFields(gWorldManagerKlass);
    if (!sf) return 0;
    float sec = 0.f;
    __try {
        sec = *reinterpret_cast<float*>(sf);  // static _updateTime @0x0
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!(sec > 0.f) || !(sec < 2.0e6f)) return 0;
    const double ms = static_cast<double>(sec) * 1000.0;
    if (ms <= 0.0 || ms >= 2.0e9) return 0;
    return static_cast<int>(ms);
}

bool DictTryGetInt(void* dict, int key, int* outVal) {
    if (!dict || !outVal || key <= 0) return false;
    bool hit = false;
    ForEachDictIntInt(dict, [&](int k, int v) {
        if (!hit && k == key) {
            *outVal = v;
            hit = true;
        }
    });
    return hit;
}

// CoolTimeOver 的 value 是绝对到期 ms（大整数），不能用「像技能等级」打分。
// 仅扫 dict.count（非 entries.max_length），避免容量槽残留脏 key/value。
bool DictTryGetIntSkillKey(void* dict, int key, int* outVal) {
    if (!dict || !outVal || key <= 0) return false;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, x::runtime::il2cpp_container::OffDictEntries());
    if (!entries) return false;
    const int capacity = ReadI32(entries, kOffArrLen);
    if (capacity <= 0) return false;
    // 扫容量但只认 hashCode>=0 的活槽；脏槽常见 hash=-1。API 优先，本路径仅兜底。
    const size_t strides[] = {kEntrySizeIntIntTight, kEntrySizeIntPtr};
    const size_t valOffs[] = {kValOffTight, kValOffAlign};
    for (int pass = 0; pass < 2; ++pass) {
        const size_t stride = strides[pass];
        const size_t valOff = valOffs[pass];
        const int n = capacity < 4096 ? capacity : 4096;
        for (int i = 0; i < n; ++i) {
            uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, stride);
            if (!e) continue;
            __try {
                if (*reinterpret_cast<int*>(e + kOffEntryHash) < 0) continue;
                if (*reinterpret_cast<int*>(e + kOffEntryKey) != key) continue;
                *outVal = *reinterpret_cast<int*>(e + valOff);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    return false;
}

// 官方 CharacterData.GetSkillCoolTimeOver（TryGetValue）。exist=false → 不在 CD。
bool CoolTimeOverFromApi(void* cd, int skillId, int* outOver, bool* outExist) {
    if (outExist) *outExist = false;
    if (outOver) *outOver = 0;
    if (!cd || skillId <= 0) return false;
    EnsureMethodInfos();
    if (!gGetSkillCoolTimeOver || !gMiGetSkillCoolTimeOver) return false;
    __try {
        if (gIsExistSkillCoolTimeOver && gMiIsExistSkillCoolTimeOver) {
            const bool exist =
                gIsExistSkillCoolTimeOver(cd, skillId, gMiIsExistSkillCoolTimeOver);
            if (outExist) *outExist = exist;
            if (!exist) return true;  // API 可用且明确不在 CD
        }
        const int over = gGetSkillCoolTimeOver(cd, skillId, gMiGetSkillCoolTimeOver);
        if (outOver) *outOver = over;
        if (outExist && over > 0) *outExist = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// tXxx_ = 绝对到期（GetUpdateTime 钟 ms）。剩余 = (t - tCur)/1000。
// 旧逻辑用 GetTickCount 当 tCur → rem 为负，再把 raw 当「剩余 ms」→ UI 冻在 124s。
float RemainFromSecondaryStat(int skillId) {
    if (skillId <= 0) return 0.f;
    void* ss = x::ui::player::LocalSecondaryStat();
    if (!LooksLikeHeapPtr(ss)) return 0.f;
    if (!gSecondaryStatKlass) gSecondaryStatKlass = FindClass(kSecondaryStatClass);
    if (gSecondaryStatKlass && !ObjKlassIs(ss, gSecondaryStatKlass)) return 0.f;

    constexpr float kMaxBuffRemainSec = 600.f;
    constexpr int kMaxRemainMs = 600000;
    const int tCur = GameUpdateTimeMsImpl();
    if (tCur <= 0) return 0.f;

    auto remFromExpire = [&](int expire) -> float {
        if (expire <= 0) return 0.f;
        const int remMs = expire - tCur;
        if (remMs > 0 && remMs <= kMaxRemainMs) return static_cast<float>(remMs) / 1000.f;
        return 0.f;
    };

    float best = 0.f;
    float fromApi = 0.f;
    int apiRaw = 0;
    if (gGetRemainTime) {
        EnsureMethodInfos();
        __try {
            apiRaw = gGetRemainTime(ss, skillId, tCur, gMiGetRemainTime);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            apiRaw = 0;
        }
        // GetRemainTime 在正确 tCur 下返回剩余 ms（非绝对时刻）。
        if (apiRaw > 0 && apiRaw <= kMaxRemainMs)
            fromApi = static_cast<float>(apiRaw) / 1000.f;
        if (fromApi > best) best = fromApi;
    }

    float fromScan = 0.f;
    int hitT = 0;
    int hitOff = 0;
    __try {
        for (size_t off = 0x10; off + 8 < 0x600; off += 4) {
            const size_t mod = off % 16u;
            if (mod != 0x08 && mod != 0x04) continue;
            const int r = ReadI32(ss, off);
            if (r != skillId) continue;
            const int t = ReadI32(ss, off + 4);
            const float sec = remFromExpire(t);
            if (sec <= 0.f) continue;
            if (sec > fromScan) {
                fromScan = sec;
                hitT = t;
                hitOff = static_cast<int>(off);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (fromScan > best) best = fromScan;
    if (best > kMaxBuffRemainSec) best = kMaxBuffRemainSec;

    const DWORD now = GetTickCount();
    if (best > 0.01f && now - gLastRemainDiagMs >= 8000) {
        gLastRemainDiagMs = now;
        runtime::LogW("SkillPort",
                      "remain id=%d best=%.1f api=%.1f scan=%.1f t=%d off=0x%X tCur=%d apiRaw=%d",
                      skillId, best, fromApi, fromScan, hitT, hitOff, tCur, apiRaw);
    }
    return best;
}

// 从 skill_names.tsv Desc 解析「再使用冷卻時間：N分鐘/秒」等；失败再回落已知新手技。
void EnsureTableCooltime() {
    if (gTableCdTried) return;
    gTableCdTried = true;

    // 表未就绪时仍可先用新手技硬表（经典版 1001/1002）。
    gTableCdSec[1001] = 120.f;
    gTableCdSec[1002] = 60.f;

    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return;
    std::string path = bin;
    if (path.back() != '\\' && path.back() != '/') path += '\\';
    path += "dataservice\\skill_names.tsv";

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        runtime::LogW("SkillPort", "table cd: open fail %s", path.c_str());
        return;
    }
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
        raw.erase(0, 3);

    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    int parsed = 0;
    size_t lineStart = 0;
    while (lineStart <= raw.size()) {
        size_t lineEnd = raw.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = raw.size();
        std::string line = raw.substr(lineStart, lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lineStart = lineEnd + 1;
        if (line.empty() || line[0] == '#') {
            if (lineEnd >= raw.size()) break;
            continue;
        }
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        const std::string code = line.substr(0, t1);
        const std::string desc = line.substr(t2 + 1);
        if (code.empty() || !isDigit(code[0])) continue;
        int skillId = 0;
        for (char c : code) {
            if (!isDigit(c)) {
                skillId = 0;
                break;
            }
            skillId = skillId * 10 + (c - '0');
            if (skillId > 2000000000) {
                skillId = 0;
                break;
            }
        }
        if (skillId <= 0) continue;

        // 找「…冷卻時間 / 等待時間」后的数字+单位（UTF-8；工程 /utf-8）。
        const char* keys[] = {"再使用冷卻時間", "在使用等待時間", "使用等待時間", "冷卻時間"};
        size_t hit = std::string::npos;
        for (const char* k : keys) {
            hit = desc.find(k);
            if (hit != std::string::npos) break;
        }
        if (hit == std::string::npos) continue;
        size_t i = hit;
        while (i < desc.size() && !isDigit(desc[i])) ++i;
        if (i >= desc.size()) continue;
        int num = 0;
        while (i < desc.size() && isDigit(desc[i])) {
            num = num * 10 + (desc[i] - '0');
            ++i;
            if (num > 100000) break;
        }
        if (num <= 0 || num > 100000) continue;
        while (i < desc.size() && (desc[i] == ' ' || desc[i] == '\t')) ++i;
        float mul = 1.f;
        if (desc.compare(i, strlen("小時"), "小時") == 0 ||
            desc.compare(i, strlen("小时"), "小时") == 0)
            mul = 3600.f;
        else if (desc.compare(i, strlen("分鐘"), "分鐘") == 0 ||
                 desc.compare(i, strlen("分钟"), "分钟") == 0)
            mul = 60.f;
        else if (desc.compare(i, strlen("秒"), "秒") == 0)
            mul = 1.f;
        else
            continue;
        const float sec = static_cast<float>(num) * mul;
        if (sec < 0.5f || sec > 7200.f * 4.f) continue;
        gTableCdSec[skillId] = sec;
        ++parsed;
        if (lineEnd >= raw.size()) break;
    }
    runtime::LogI("SkillPort", "table cd loaded n=%d (+hard 1001/1002)", parsed);
}

float TableCooltimeSec(int skillId) {
    if (skillId <= 0) return 0.f;
    EnsureTableCooltime();
    const auto it = gTableCdSec.find(skillId);
    return it == gTableCdSec.end() ? 0.f : it->second;
}

void NoteLocalCooldownAfterCast(int skillId) {
    const float dur = TableCooltimeSec(skillId);
    if (dur < 0.5f) return;
    const DWORD now = GetTickCount();
    const DWORD end = now + static_cast<DWORD>(dur * 1000.f + 0.5f);
    {
        std::lock_guard<std::mutex> lock(gLocalCdMu);
        gLocalCdEndTick[skillId] = end;
    }
    runtime::LogI("SkillPort", "local cd confirm id=%d dur=%.0fs endTick=%u", skillId, dur, end);
}

float LocalCooldownRemainSec(int skillId) {
    if (skillId <= 0) return 0.f;
    const DWORD now = GetTickCount();
    DWORD end = 0;
    {
        std::lock_guard<std::mutex> lock(gLocalCdMu);
        const auto it = gLocalCdEndTick.find(skillId);
        if (it == gLocalCdEndTick.end()) return 0.f;
        end = it->second;
        if (static_cast<int>(end - now) <= 0) {
            gLocalCdEndTick.erase(it);
            return 0.f;
        }
    }
    return static_cast<float>(end - now) / 1000.f;
}

float CooltimeRemainSec(int skillId) {
    void* cd = CharacterData();
    if (!cd || skillId <= 0) return LocalCooldownRemainSec(skillId);
    const int tCur = GameUpdateTimeMsImpl();
    const DWORD now = GetTickCount();
    const int tick = static_cast<int>(now);
    float fromGame = 0.f;
    const char* gameTag = nullptr;

    auto remFromOver = [&](int over, const char* tag) -> float {
        if (over <= 0) return 0.f;
        // 优先游戏钟；若 rem 离谱且 over 贴近 GetTickCount，改用 tick 钟。
        int remMs = (tCur > 0) ? (over - tCur) : 0;
        const char* clock = "update";
        if (!(remMs > 0 && remMs <= 7200000)) {
            const int remTick = over - tick;
            if (remTick > 0 && remTick <= 7200000) {
                remMs = remTick;
                clock = "tick";
            } else if (remMs <= 0 && tCur > 0) {
                if (now - gLastCdDiagMs >= 8000) {
                    gLastCdDiagMs = now;
                    runtime::LogW("SkillPort",
                                  "cd id=%d over=%d tCur=%d remMs=%d (%s expired→0)", skillId,
                                  over, tCur, remMs, tag);
                }
                return 0.f;
            } else {
                if (now - gLastCdDiagMs >= 8000) {
                    gLastCdDiagMs = now;
                    runtime::LogW("SkillPort",
                                  "cd id=%d over=%d tCur=%d tick=%d remMs=%d (%s corrupt→0)",
                                  skillId, over, tCur, tick, remMs, tag);
                }
                return 0.f;
            }
        }
        const float sec = static_cast<float>(remMs) / 1000.f;
        if (now - gLastCdDiagMs >= 8000) {
            gLastCdDiagMs = now;
            runtime::LogW("SkillPort", "cd id=%d over=%d rem=%.1f (%s/%s)", skillId, over, sec,
                          tag, clock);
        }
        return sec;
    };

    // 1) 官方 GetSkillCoolTimeOver / IsExist（Dict.TryGetValue，不扫脏槽）。
    {
        int over = 0;
        bool exist = false;
        if (CoolTimeOverFromApi(cd, skillId, &over, &exist)) {
            if (exist) {
                fromGame = remFromOver(over, "CoolTimeOverApi");
                if (fromGame > 0.01f) gameTag = "CoolTimeOverApi";
            }
        } else {
            // 2) 手扫 Over 字典（仅 API 未绑定时；count 约束见 DictTryGetIntSkillKey）。
            void* overDict = ReadPtr(cd, x::ui::player::OffSkillCoolTimeOver());
            if (overDict && tCur > 0) {
                int overScan = 0;
                if (DictTryGetIntSkillKey(overDict, skillId, &overScan) && overScan > 0) {
                    fromGame = remFromOver(overScan, "CoolTimeOverScan");
                    if (fromGame > 0.01f) gameTag = "CoolTimeOverScan";
                }
            } else if (overDict && tCur <= 0 && now - gLastCdDiagMs >= 8000) {
                gLastCdDiagMs = now;
                runtime::LogW("SkillPort", "cd id=%d tCur=0 skip CoolTimeOver→ushort", skillId);
            }

            // 3) SkillCooltime ushort：仅 Over 路径全不可用时兜底。
            if (fromGame <= 0.01f) {
                void* dict = ReadPtr(cd, x::ui::player::OffSkillCooltime());
                if (dict) {
                    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
                    void* entries = ReadPtr(dict, x::runtime::il2cpp_container::OffDictEntries());
                    if (entries) {
                        const int capacity = ReadI32(entries, kOffArrLen);
                        if (capacity > 0) {
                            struct Cand {
                                size_t stride;
                                size_t valOff;
                                bool asU16;
                            };
                            const Cand cands[] = {
                                {kEntrySizeIntIntTight, kValOffTight, true},
                                {kEntrySizeIntPtr, kValOffAlign, true},
                                {kEntrySizeIntIntTight, kValOffTight, false},
                                {kEntrySizeIntPtr, kValOffAlign, false},
                            };
                            auto readVal = [&](const Cand& c, uint8_t* e) -> int {
                                if (c.asU16)
                                    return static_cast<int>(*reinterpret_cast<uint16_t*>(e + c.valOff));
                                return *reinterpret_cast<int*>(e + c.valOff);
                            };
                            auto saneCd = [](int v) -> bool { return v > 0 && v <= 600; };
                            int bestScore = -1;
                            size_t bestIdx = 0;
                            for (size_t ci = 0; ci < sizeof(cands) / sizeof(cands[0]); ++ci) {
                                const Cand& c = cands[ci];
                                int score = 0;
                                for (int i = 0; i < capacity && i < 256; ++i) {
                                    uint8_t* e =
                                        x::runtime::il2cpp_container::DictEntryAt(entries, i, c.stride);
                                    if (!e) continue;
                                    __try {
                                        if (*reinterpret_cast<int*>(e + kOffEntryHash) < 0) continue;
                                        const int key = *reinterpret_cast<int*>(e + kOffEntryKey);
                                        if (!LooksLikePlayerSkillId(key)) continue;
                                        if (saneCd(readVal(c, e))) ++score;
                                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                                    }
                                }
                                if (score > bestScore) {
                                    bestScore = score;
                                    bestIdx = ci;
                                }
                            }
                            const Cand& c = cands[bestIdx];
                            for (int i = 0; i < capacity && i < 4096; ++i) {
                                uint8_t* e =
                                    x::runtime::il2cpp_container::DictEntryAt(entries, i, c.stride);
                                if (!e) continue;
                                __try {
                                    if (*reinterpret_cast<int*>(e + kOffEntryHash) < 0) continue;
                                    if (*reinterpret_cast<int*>(e + kOffEntryKey) != skillId) continue;
                                    const int v = readVal(c, e);
                                    if (v <= 0) break;
                                    float sec = static_cast<float>(v);
                                    if (v > 600 && v <= 600000) sec = static_cast<float>(v) / 1000.f;
                                    if (sec > 0.f && sec <= 7200.f) {
                                        fromGame = sec;
                                        gameTag = "SkillCooltime ushort fallback";
                                        if (now - gLastCdDiagMs >= 8000) {
                                            gLastCdDiagMs = now;
                                            runtime::LogW("SkillPort",
                                                          "cd id=%d rem=%.1f (SkillCooltime ushort "
                                                          "fallback)",
                                                          skillId, sec);
                                        }
                                    }
                                    break;
                                } __except (EXCEPTION_EXECUTE_HANDLER) {
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 官方 exist=false / corrupt→0 时，用本地施放记点兜底（1001=120s / 1002=60s 等）。
    const float fromLocal = LocalCooldownRemainSec(skillId);
    float best = fromGame;
    const char* bestTag = gameTag ? gameTag : "game0";
    if (fromLocal > best + 0.01f) {
        best = fromLocal;
        bestTag = "localCast";
    }
    if (best > 0.01f && now - gLastCdDiagMs >= 8000) {
        gLastCdDiagMs = now;
        runtime::LogW("SkillPort", "cd id=%d rem=%.1f game=%.1f local=%.1f (%s)", skillId, best,
                      fromGame, fromLocal, bestTag);
    }
    return best;
}

void CastJobFnBody(CastJob* job) {
    if (!job) return;
    job->ok = false;
    job->notReady = false;
    job->reason[0] = 0;
    if (!gLocalUser || !gDoActiveSkill) {
        SetJobReason(job, "no_api");
        return;
    }
    EnsureMethodInfos();

    // 可选直发：SendSkillUseRequest(SkillEntry, pPet=0, pt=0, nSLV=level| -1, weapons=null, charge=0)
    // 失败不中断 —— 继续走下方 DoActive 原路径，避免可选开关搞挂施放。
    if (job->preferSendUse && gSendSkillUse) {
        void* si = ResolveSkillInfoSingleton(GetTickCount());
        void* entry = nullptr;
        int level = 0;
        if (LooksLikeHeapPtr(si) && gGetSkill && gGetSkillLevel) {
            __try {
                level = gGetSkillLevel(gLocalUser, job->skillId, gMiGetSkillLevel);
                entry = gGetSkill(si, job->skillId, gMiGetSkill);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                entry = nullptr;
                level = 0;
            }
            // 与公开 GetSkillLevel 同源：防 raw RVA 绕过 skill_max_level
            if (level > 0) {
                level = x::features::skill_max_level::AdjustLevelIfForced(job->skillId, level);
            }
        }
        if (LooksLikeHeapPtr(entry)) {
            const int nSlv = level > 0 ? level : -1;
            bool sendOk = false;
            __try {
                sendOk = gSendSkillUse(gLocalUser, entry, /*pPet=*/0, /*pt=*/0u, nSlv,
                                       /*weapons=*/nullptr, /*charge=*/0, gMiSendSkillUse);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                SetJobReason(job, "seh_send_use");
                // fall through to DoActive
                sendOk = false;
            }
            if (sendOk) {
                job->ok = true;
                SetJobReason(job, gMiSendSkillUse ? "ok_send_use" : "ok_send_use_mi0");
                return;
            }
            // 保留 send 失败痕迹后再走 DoActive（reason 会被覆盖）。
        }
    }

    // 主路径：DoActiveSkill(skillId) —— 对标枫星 UseOnClientImmediate 的公开总入口。
    // 内部按技能类型分发 Melee/Shoot/Magic/Prepare/StatChange，再走到 SendSkillUseRequest。
    bool ok = false;
    __try {
        ok = gDoActiveSkill(gLocalUser, job->skillId, 0u, gMiDoActiveSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetJobReason(job, "seh_do_active");
        return;
    }
    if (ok) {
        job->ok = true;
        SetJobReason(job, gMiDoActiveSkill ? "ok_do_active" : "ok_do_active_mi0");
        return;
    }

    // DoActive 拒施：常见已有 BUFF / CD / 状态不允许。新手技 1001/1002 等
    // SkillInfo.GetSkill 常空，Prepare 回退会误报 no_entry 并触发 buffs 指数退避。
    float remain = 0.f;
    if (IsSkillActive(job->skillId, &remain)) {
        job->ok = true;  // 视为已在身上，上层走 verify/assumed
        SetJobReason(job, "already_active");
        return;
    }

    // 回退 Prepare：无 MethodInfo 时直调易 SEH（BIN LOG 大量 seh_prepare），直接放弃。
    if (!gPrepare) {
        SetJobReason(job, gMiDoActiveSkill ? "do_false_no_prep" : "do_false_mi0_noprep");
        job->notReady = true;
        return;
    }
    if (!gMiPrepare) {
        SetJobReason(job, "do_false_skip_prep_mi0");
        job->notReady = true;
        return;
    }
    if (!gGetSkill || !gGetSkillLevel) {
        SetJobReason(job, "do_false_no_api");
        job->notReady = true;
        return;
    }
    void* si = ResolveSkillInfoSingleton(GetTickCount());
    if (!LooksLikeHeapPtr(si)) {
        SetJobReason(job, "do_false_no_si");
        job->notReady = true;
        return;
    }
    void* entry = nullptr;
    int level = 0;
    __try {
        level = gGetSkillLevel(gLocalUser, job->skillId, gMiGetSkillLevel);
        entry = gGetSkill(si, job->skillId, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetJobReason(job, "seh_resolve");
        return;
    }
    if (level > 0) {
        level = x::features::skill_max_level::AdjustLevelIfForced(job->skillId, level);
    }
    if (level <= 0) {
        SetJobReason(job, "no_level");
        job->notReady = true;
        return;
    }
    if (!LooksLikeHeapPtr(entry)) {
        // DoActive 已 false 且无 entry：软拒绝，勿当硬 fail（避免 no_entry 退避风暴）。
        SetJobReason(job, "do_false_no_entry");
        job->notReady = true;
        return;
    }
    __try {
        ok = gPrepare(gLocalUser, entry, level, 0u, gMiPrepare);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetJobReason(job, "seh_prepare");
        return;
    }
    job->ok = ok;
    if (ok) {
        SetJobReason(job, "ok_prepare");
    } else {
        SetJobReason(job, "prepare_false");
        job->notReady = true;
    }
}

// 只读视觉探针包在 SEH 体外，避免 C2712（__try 与 C++ 析构冲突）。
void CastJobFn(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("skill.Cast");
    auto* job = reinterpret_cast<CastJob*>(user);
    if (!job) return;
    player_combat::VisualSnap pre{};
    const bool preOk = player_combat::QueryVisualSnap(pre);
    char note[96]{};
    std::snprintf(note, sizeof(note), "skill=%d", job->skillId);
    player_combat::LogDoActiveVisProbe("pre", note, nullptr);
    CastJobFnBody(job);
    std::snprintf(note, sizeof(note), "skill=%d ok=%d nr=%d reason=%s", job->skillId,
                  job->ok ? 1 : 0, job->notReady ? 1 : 0,
                  job->reason[0] ? job->reason : "-");
    player_combat::LogDoActiveVisProbe("post", note, preOk && pre.ok ? &pre : nullptr);
}

}  // namespace

void Init() {
    gBound = ResolveApi();
    EnsureMethodInfos();
    runtime::LogI("SkillPort",
                  "init bound=%d DoActive=0x%X Prepare=0x%X SendUse=0x%X getSkill=0x%X getLv=0x%X "
                  "miDo=%p miP=%p miSend=%p miLv=%p miGet=%p",
                  gBound.load() ? 1 : 0, kRvaDoActiveSkill, kRvaDoActiveSkillPrepare,
                  kRvaSendSkillUseRequest, kRvaSkillInfoGetSkill, kRvaGetSkillLevel,
                  (void*)gMiDoActiveSkill, (void*)gMiPrepare, (void*)gMiSendSkillUse,
                  (void*)gMiGetSkillLevel, (void*)gMiGetSkill);
}

void Shutdown() {
    gLocalUser = nullptr;
    gSkillInfo = nullptr;
    gMiGetSkillLevel = nullptr;
    gMiGetSkill = nullptr;
    gMiDoActiveSkill = nullptr;
    gMiPrepare = nullptr;
    gMiSendSkillUse = nullptr;
    gMiGetRemainTime = nullptr;
    gMiGetUpdateTime = nullptr;
    gMiGetSkillCoolTimeOver = nullptr;
    gMiIsExistSkillCoolTimeOver = nullptr;
    gGetRemainTime = nullptr;
    gGetUpdateTime = nullptr;
    gGetSkillCoolTimeOver = nullptr;
    gIsExistSkillCoolTimeOver = nullptr;
    gCharacterDataKlass = nullptr;
    gWorldManagerKlass = nullptr;
    {
        std::lock_guard<std::mutex> lock(gLocalCdMu);
        gLocalCdEndTick.clear();
    }
    gBound = false;
}

bool EnsureBound() {
    const DWORD now = GetTickCount();
    if (!ResolveApi()) return false;
    const bool lu = TryResolveLocalUser(now);
    const bool wm = world::EnsureBound();
    if (lu || wm) EnsureSkillFieldOffsets();
    (void)ResolveSkillInfoSingleton(now);
    gBound = lu;
    return lu && wm;
}

bool Ready() { return gBound && LocalUserStillAlive(); }

int ListActiveSkills(ActiveSkill* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!EnsureBound() || !gLocalUser) return 0;
    void* list = ReadPtr(gLocalUser, gOffAffectedList);
    if (list) x::runtime::il2cpp_container::RefineFromListInstance(list);
    const int n = ListSize(list);
    void* items = ListItems(list);
    int w = 0;
    const int arrLen = items ? ReadI32(items, kOffArrLen) : 0;
    int lim = n;
    if (lim < 0) lim = 0;
    if (arrLen > 0 && lim > arrLen) lim = arrLen;
    for (int i = 0; i < lim && i < 256 && w < cap; ++i) {
        void* e = ArrayAtPtr(items, i);
        if (!LooksLikeHeapPtr(e)) continue;
        const int id = ReadI32(e, gOffAffSkillId);
        if (id <= 0) continue;
        out[w].skillId = id;
        out[w].startTime = ReadI32(e, gOffAffStartTime);
        ++w;
    }
    if (w == 0) {
        const DWORD now = GetTickCount();
        if (now - gLastAffDiagMs >= 10000) {
            gLastAffDiagMs = now;
            void* ss = x::ui::player::LocalSecondaryStat();
            void* e0 = (n > 0) ? ArrayAtPtr(items, 0) : nullptr;
            const int id0 = LooksLikeHeapPtr(e0) ? ReadI32(e0, gOffAffSkillId) : 0;
            runtime::LogW("SkillPort",
                          "aff empty list=%p size=%d items=%p arrLen=%d lu=%p ss=%p e0=%p id0=%d",
                          list, n, items, arrLen, gLocalUser, ss, e0, id0);
        }
    }
    return w;
}

bool IsSkillActive(int skillId, float* outRemainSec) {
    if (outRemainSec) *outRemainSec = 0.f;
    if (skillId <= 0) return false;
    ActiveSkill buf[kMaxActiveSkills]{};
    const int n = ListActiveSkills(buf, kMaxActiveSkills);
    for (int i = 0; i < n; ++i) {
        if (buf[i].skillId != skillId) continue;
        const float rem = RemainFromSecondaryStat(skillId);
        if (outRemainSec) *outRemainSec = rem;
        return true;
    }
    // 辅证：肉眼有图标时 AffectedList 偶发仍空，SecondaryStat.rXxx_==skillId 可验身。
    const float ssRem = RemainFromSecondaryStat(skillId);
    if (ssRem > 0.f) {
        if (outRemainSec) *outRemainSec = ssRem;
        return true;
    }
    return false;
}

bool IsPreparingSkill(int* outSkillId) {
    if (outSkillId) *outSkillId = 0;
    if (!EnsureBound() || !LooksLikeHeapPtr(gLocalUser)) return false;
    int sid = 0;
    __try {
        sid = ReadI32(gLocalUser, gOffPreparingSkillId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // 与 CMS IsPreparingSkill 同形：SkillID==0 表示未在 Prepare/警戒施法态。
    if (sid == 0) return false;
    if (outSkillId) *outSkillId = sid;
    return true;
}

int GetSkillLevel(int skillId) {
    if (skillId <= 0 || !EnsureBound() || !gLocalUser || !gGetSkillLevel) return 0;
    EnsureMethodInfos();
    int lv = 0;
    __try {
        lv = gGetSkillLevel(gLocalUser, skillId, gMiGetSkillLevel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    // 双保险：即便 skill_port 仍握着未钩 RVA，也能抬满级（与 Hook B 同源）。
    return x::features::skill_max_level::AdjustLevelIfForced(skillId, lv);
}

void* GetSkillEntry(int skillId) {
    if (skillId <= 0) return nullptr;
    if (!EnsureBound()) return nullptr;
    EnsureMethodInfos();
    void* si = ResolveSkillInfoSingleton(GetTickCount());
    if (!LooksLikeHeapPtr(si) || !gGetSkill) return nullptr;
    void* entry = nullptr;
    __try {
        entry = gGetSkill(si, skillId, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(entry) ? entry : nullptr;
}

bool ResolveSkillName(int skillId, char* out, int outSz) {
    if (!out || outSz <= 0) return false;
    out[0] = 0;
    if (skillId <= 0) return false;

    char idBuf[32]{};
    snprintf(idBuf, sizeof(idBuf), "%d", skillId);

    // offline-first：稳定繁中、少碰 Il2Cpp 字符串偏移；表外再 RUNTIME。
    {
        const xcat::SkillNamesPack& pack = xcat::GetSharedSkillNames(x::runtime::GetBinDir());
        const char* offline = xcat::SkillNameLookupById(pack, skillId);
        if (offline && offline[0]) {
            strncpy_s(out, static_cast<size_t>(outSz), offline, _TRUNCATE);
            return true;
        }
    }

    const DWORD now = GetTickCount();
    void* si = ResolveSkillInfoSingleton(now);
    EnsureMethodInfos();
    if (!si || !gGetSkill) {
        strncpy_s(out, static_cast<size_t>(outSz), idBuf, _TRUNCATE);
        return false;
    }
    void* entry = nullptr;
    __try {
        entry = gGetSkill(si, skillId, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        entry = nullptr;
    }
    if (!LooksLikeHeapPtr(entry)) {
        strncpy_s(out, static_cast<size_t>(outSz), idBuf, _TRUNCATE);
        return false;
    }
    void* nameObj = ReadPtr(entry, gOffSkillName);
    if (ReadIl2CppString(nameObj, out, outSz) && out[0] && std::strcmp(out, idBuf) != 0) {
        return true;
    }
    strncpy_s(out, static_cast<size_t>(outSz), idBuf, _TRUNCATE);
    return false;
}

int ListLearnedSkills(SkillInfoLite* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!EnsureBound()) return 0;
    void* cd = CharacterData();
    if (!cd) return 0;
    EnsureMethodInfos();

    ActiveSkill active[kMaxActiveSkills]{};
    const int activeN = ListActiveSkills(active, kMaxActiveSkills);
    auto isActive = [&](int id) {
        for (int i = 0; i < activeN; ++i)
            if (active[i].skillId == id) return true;
        return false;
    };
    auto already = [&](int id, int n) {
        for (int j = 0; j < n; ++j)
            if (out[j].skillId == id) return true;
        return false;
    };
    auto pushLearned = [&](int key, int val, int& w) {
        if (w >= cap) return;
        if (!LooksLikePlayerSkillId(key) || !LooksLikeSkillLevel(val)) return;
        if (already(key, w)) return;
        SkillInfoLite& s = out[w];
        s = {};
        s.skillId = key;
        s.level = val;
        s.learned = true;
        s.active = isActive(key);
        s.remainBuffSec = s.active ? RemainFromSecondaryStat(key) : 0.f;
        s.remainCooldownSec = CooltimeRemainSec(key);
        const float tableCd = TableCooltimeSec(key);
        s.cooldownSec = tableCd > 0.01f ? tableCd : s.remainCooldownSec;
        snprintf(s.code, sizeof(s.code), "%d", key);
        ResolveSkillName(key, s.name, sizeof(s.name));
        ++w;
    };

    int w = 0;
    void* skillRec = ReadPtr(cd, x::ui::player::OffSkillRecord());
    void* skillRecEx = ReadPtr(cd, x::ui::player::OffSkillRecordEx());
    ForEachDictIntInt(skillRec, [&](int key, int val) { pushLearned(key, val, w); });
    ForEachDictIntInt(skillRecEx, [&](int key, int val) { pushLearned(key, val, w); });

    // 故意不再扫 SkillInfo._dictionarySkill + GetSkillLevel：
    // BIN 曾出现 SkillRecord listed=3，fallback 再 hit=4 → 把技能书不显示的活动/隐藏技
    // （1009 竹竿天擊等）塞进多发列表。列表以角色 SkillRecord/Ex 为准，对齐技能书已学。

    const DWORD now = GetTickCount();
    if (!gLastDictDiagMs || static_cast<DWORD>(now - gLastDictDiagMs) > 15000u) {
        gLastDictDiagMs = now;
        runtime::LogI("SkillPort", "SkillRecord count=%d ex=%d listed=%d (record-only)",
                      DictIntIntCount(skillRec), DictIntIntCount(skillRecEx), w);
    }

    for (int i = 0; i < activeN && w < cap; ++i) {
        const int id = active[i].skillId;
        if (already(id, w)) continue;
        const int lv = GetSkillLevel(id);
        if (lv <= 0 && !LooksLikePlayerSkillId(id)) continue;
        SkillInfoLite& s = out[w];
        s = {};
        s.skillId = id;
        s.level = lv > 0 ? lv : 1;
        s.learned = lv > 0;
        s.active = true;
        s.remainBuffSec = RemainFromSecondaryStat(id);
        s.remainCooldownSec = CooltimeRemainSec(id);
        const float tableCd = TableCooltimeSec(id);
        s.cooldownSec = tableCd > 0.01f ? tableCd : s.remainCooldownSec;
        snprintf(s.code, sizeof(s.code), "%d", id);
        ResolveSkillName(id, s.name, sizeof(s.name));
        ++w;
    }
    return w;
}

float GetSkillCooldownRemainSec(int skillId) {
    if (skillId <= 0 || !EnsureBound()) return LocalCooldownRemainSec(skillId);
    return CooltimeRemainSec(skillId);
}

float GetSkillCooldownDurationSec(int skillId) {
    if (skillId <= 0) return 0.f;
    return TableCooltimeSec(skillId);
}

int GetGameUpdateTimeMs() { return GameUpdateTimeMsImpl(); }

bool CastSkill(int skillId, bool* notReady, char* outReason, int reasonSz) {
    auto setReason = [&](const char* r) {
        if (outReason && reasonSz > 0) strncpy_s(outReason, reasonSz, r ? r : "", _TRUNCATE);
    };
    if (notReady) *notReady = false;
    if (skillId <= 0) {
        setReason("bad_id");
        return false;
    }
    if (!EnsureBound() || !LocalUserStillAlive()) {
        if (notReady) *notReady = true;
        setReason("no_lu");
        return false;
    }
    // 主路径 DoActiveSkill 不依赖 SkillInfo；SI 仅 Prepare 回退需要（CastJobFn 内再解析）。
    // 此前硬门禁 no_si 会在 SI 单例未绑定时把全部施放挡死（BIN LOG 70× reason=no_si）。
    (void)ResolveSkillInfoSingleton(GetTickCount());
    EnsureMethodInfos();
    if (!runtime::main_thread::Ensure()) {
        if (notReady) *notReady = true;
        setReason("pump_fail");
        return false;
    }
    CastJob job{};
    job.skillId = skillId;
    if (!runtime::main_thread::InvokeAndWait(&CastJobFn, &job, kJobWaitMs,
                                            runtime::main_thread::JobPrio::High)) {
        if (notReady) *notReady = true;
        setReason("invoke_timeout");
        return false;
    }
    if (notReady) *notReady = job.notReady && !job.ok;
    setReason(job.reason[0] ? job.reason : (job.ok ? "ok" : "fail"));
    // 不在此处 NoteLocalCooldown：DoActive 假 ok 会种出假 CD；由 ConfirmLocalCooldown（verify ok）记。
    return job.ok;
}

bool CastSkillPreferSendUse(int skillId, bool* notReady, char* outReason, int reasonSz) {
    auto setReason = [&](const char* r) {
        if (outReason && reasonSz > 0) strncpy_s(outReason, reasonSz, r ? r : "", _TRUNCATE);
    };
    if (notReady) *notReady = false;
    if (skillId <= 0) {
        setReason("bad_id");
        return false;
    }
    if (!EnsureBound() || !LocalUserStillAlive()) {
        if (notReady) *notReady = true;
        setReason("no_lu");
        return false;
    }
    (void)ResolveSkillInfoSingleton(GetTickCount());
    EnsureMethodInfos();
    if (!runtime::main_thread::Ensure()) {
        if (notReady) *notReady = true;
        setReason("pump_fail");
        return false;
    }
    CastJob job{};
    job.skillId = skillId;
    job.preferSendUse = true;
    if (!runtime::main_thread::InvokeAndWait(&CastJobFn, &job, kJobWaitMs,
                                            runtime::main_thread::JobPrio::High)) {
        if (notReady) *notReady = true;
        setReason("invoke_timeout");
        return false;
    }
    if (notReady) *notReady = job.notReady && !job.ok;
    setReason(job.reason[0] ? job.reason : (job.ok ? "ok" : "fail"));
    return job.ok;
}

void ConfirmLocalCooldown(int skillId) {
    if (skillId <= 0) return;
    NoteLocalCooldownAfterCast(skillId);
}

}  // namespace x::features::ports::skill
