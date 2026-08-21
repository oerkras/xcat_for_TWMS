// Shared player vitals — Classic TWMS.
// 真源唯一：WorldManager→CharacterData→CharacterStat（WM BasicStat 抬 mhp/mmp）。
// 字段防漂移：dump 哈希 → field_get_offset；失败回退下方 Hint（remount 2026-08-06 晚 · CS/BS/NL/Slot）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "player_vitals.h"

#include "../features/ports/world_port.h"
#include "../runtime/log.h"
#include "../runtime/managed_main.h"
#include "../runtime/il2cpp_bind.h"
#include "../runtime/il2cpp_container.h"
#include "../runtime/il2cpp_shape.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::ui::player {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// --- dump.cs / restored TypeDef（2026-08-06）---
constexpr char kHashWorldManager[] =
    "f05b942aeb569b2c37916e7ee710b3ba74011550adcb611a0b26981331a8321";
constexpr char kHashCharacterData[] =
    "cb1adcfaa2d7a4777b1980f1b967ca8c4bc7c63812245c43c9cf792e4b2b410";
constexpr char kHashCharacterStat[] =
    "a2e691832496fcc913cf932302acf94b7c7c1e675a78d2ac93ac12beaf035d3";
constexpr char kHashBasicStat[] =
    "a750c2d0c59a697596c71dc2318b1465f07e0fc0e65c0071be11d66c52bf3a2";
constexpr char kHashNextLevel[] =
    "e68402578af9548b8086980f42884d360a661724e3102ca248ea28dea6bccba";  // TypeDef 1834 Nextlevel
constexpr char kHashItemSlotBase[] =
    "a685e8d519d8a6d182732393f3ca82c37177616744fec2d3bee2287c1e531ee";  // TypeDef 1837
constexpr char kHashItemSlotBundle[] =
    "e776f8453a3e71f64ae3a1635413380bf069a8c3e6d088d401e1249851302de";  // TypeDef 1840

// WM
constexpr char kHashWmMyUser[] =
    "<a080716de91a209a5bcfdb18c82ae6f03749b6edea5f0249c9a2a958bf2f54b>k__BackingField";
// 2026-08-06 BIN：旧哈希 c39c747b… 实为 WM+0xA8 的 bool；真 CharacterData* @0xE0。
constexpr char kHashWmCharacterData[] =
    "ade0e91001d3248b5f649264d9536e3f60b3f269d9596b8651e77c61da582c6";
// BasicStat* @0xE8（旧 a96d3d20… 是 int[] @0xB0）
constexpr char kHashWmBasicStat[] =
    "a7908bd2b45128976b0fe489c8a106a33351408ab749d7395d34f75828aeb65";
// CharacterData
constexpr char kHashCdCharacterStat[] =
    "cb599b090167bd8786ffc08dfabf7025dd1e46376235b23904a10c505a46d30";
constexpr char kHashCdItemSlots[] =
    "bd8d6cc061546925a4b90fb97643a21c57d17a27a5f2537c3e411c953eaa40c";  // List<ItemSlotBase>[]
constexpr char kHashCdSkillRecord[] =
    "c88481ad4fb1cb169401367aca8c53566ccd5a74f31545903f574aafc1c366d";  // Dict<int,int>
constexpr char kHashCdSkillRecordEx[] =
    "b4ee41457679adc5e1bda6e629dc194492e61bcdffb77765f47997636f70ae1";
constexpr char kHashCdSkillMasterLevel[] =
    "f92a407371e8889a1e1e525d854e1d26826b9e60fca87999c79487692d90083";  // Dict<int,int>
constexpr char kHashCdSkillCooltime[] =
    "c828062737371591a0dcd6e7517637900bdc9bc9f523a99e74b7c446b485c45";  // Dict<int,ushort>
constexpr char kHashCdSkillCoolTimeOver[] =
    "d95a8e633a4e6756a9403376197875eadfdb9f8e9313b6f2503285b60536b3a";  // Dict<int,int>
constexpr char kHashWmSecondaryStat[] =
    "b8fb4f3a047a62ec5c6819e7761f47c0d8a12864a4c1e65a41115d93be574b5";  // WM+0xF0；勿用 +0xB8 嵌套 struct
// ItemSlotBase / ItemSlotBundle（08-06 remount：类名已哈希，字段亦哈希）
constexpr char kHashSlotItemId[] =
    "f195118d673a188cdcc0ae9b8b221192b6b745a65dbe91ad7fda54ec9ea5b15";  // ItemId @0x10
constexpr char kHashSlotBundleNumber[] =
    "ce3cd4752a111dc7b0fa43e9a1a14332f2382a76befd978793788f404f46c1e";  // nNumber @0x28
constexpr char kFldSlotItemId[] = "ItemId";           // restored 明文兜底
constexpr char kFldSlotBundleNumber[] = "nNumber";
// CharacterStat（08-06 remount · TypeDef 1833）
constexpr char kHashCsCharacterId[] =
    "d0b6a9cc844e2e44bd1ed25ff19416475c475563f51544428c470b9b5943d7e";  // CharacterID @0x10
constexpr char kHashCsStr[] =
    "d47681f54b26640d7b4c3dfb63a3ee2dc66b3096419ec1650f156b74557195d";  // nSTR @0x3C
constexpr char kHashCsDex[] =
    "a4dc8d3834bf4a2ac172a918ca68937b6bcb054109d603f378cd6247f113d42";  // nDEX @0x3E
constexpr char kHashCsInt[] =
    "a2b3c572816388bf86d18ead81adb1efc667003b0585ec604ccb8fc49ddf2a6";  // nINT @0x40
constexpr char kHashCsLuk[] =
    "d39b19f99af8d0e51cdcd0d64992b1a8cc41d1a4a9aa52407f50438587c7cb1";  // nLUK @0x42
constexpr char kHashCsAp[] =
    "a7fd07c8e5ee854c1f1d3c95f60aefa5475b497a93eec1996f7233d6019debf";  // ap @0x4C
constexpr char kHashCsSp[] =
    "d4735655d0c68d7e7b9ee6f76b166dc17e030011a9d0b951e55676150fdfb76";  // sp @0x4E
constexpr char kHashCsName[] =
    "d7e238d693056d8dfd4d6be9267b3c4c9a23072a996a9e6f9a9b11f06163cfa";  // CharacterName @0x18
constexpr char kHashCsLevel[] =
    "e2627fc9008c53aa8acd90a32bf8c4fa007abe583af0186e3e2df41e9224b4b";  // level @0x38
constexpr char kHashCsJob[] =
    "b12479f88a9af36152d8e6a31c2cbb5d927f03a513d4588710153dab9006d59";  // job @0x3A
constexpr char kHashCsHp[] =
    "ed728f57c840f761094e9221728da02dae00ae113da3daef4135c0f95fff5d6";  // hp @0x44
constexpr char kHashCsMhp[] =
    "d1e14f014e7d3fd63af91efd93a49f565a1b821eabfe321c0c7ef7fb40788b5";  // mhp @0x46
constexpr char kHashCsMp[] =
    "fb68961d7cf050990078d4c5be95851902da62d8a16f69ff1c6f94b946ab738";  // mp @0x48
constexpr char kHashCsMmp[] =
    "d4399b4422143ff6a3740805fa4097bc568d83acd68182d0e7c6f171580d9cd";  // mmp @0x4A
constexpr char kHashCsExp[] =
    "a0cdcdcf2202b8e10fef37d7d0de1efd7e8e7037a8d3ddc4cb8aa6d0f6f98f7";  // exp @0x50
constexpr char kHashCsMoney[] =
    "bd8de8cd1629a021dc621ccf69185442dd428c4b88199d96315d10de2dd25b1";  // money @0x58
constexpr char kHashCsNextLevel[] =
    "a5705d504b212af957b900f0a4d4ccce330aa10bb36e4ac45549bc55a886fac";  // Nextlevel* @0x80
// BasicStat（08-06 remount · TypeDef 1322）
constexpr char kHashBsNmhp[] =
    "fe0de01fc4db7b74a912689b98a7994dce98127ed516c5060864221424f30fe";  // nMHP @0x30
constexpr char kHashBsNmmp[] =
    "b879a8888a2e021d08bea4a2ea4385037036d2246ade091a896ed0bef6a3dfb";  // nMMP @0x34
// Nextlevel.int[]（08-06 remount）
constexpr char kHashNextArr[] =
    "cf15e10baecba4b32c32df7de44a50c33538099d1615ba666e337660ce8d9c2";  // @0x10

// Hint（dump 复核；hash 失败时回退）— remount 2026-08-06 晚间（CD@0xE0 / BS@0xE8）
constexpr size_t kFbWmMyUser = 0x28;
constexpr size_t kFbWmCharacterData = 0xE0;  // was 0xA8（bool 误标）
constexpr size_t kFbWmBasicStat = 0xE8;      // was 0xB0（int[] 误标）
constexpr size_t kFbCdCharacterStat = 0x10;
constexpr size_t kFbCdItemSlots = 0x40;
constexpr size_t kFbCdSkillRecord = 0x50;
constexpr size_t kFbCdSkillRecordEx = 0x58;
constexpr size_t kFbCdSkillMasterLevel = 0x60;
constexpr size_t kFbCdSkillCooltime = 0x68;
constexpr size_t kFbCdSkillCoolTimeOver = 0x70;
constexpr size_t kFbWmSecondaryStat = 0xF0;  // SecondaryStat*（08-06 dump；旧误标 0xB8 为嵌套 struct）
constexpr size_t kFbSlotItemId = 0x10;
constexpr size_t kFbSlotBundleNumber = 0x28;  // ItemSlotBundle.nNumber
constexpr size_t kFbCsCharacterId = 0x10;
constexpr size_t kFbCsStr = 0x3C;
constexpr size_t kFbCsDex = 0x3E;
constexpr size_t kFbCsInt = 0x40;
constexpr size_t kFbCsLuk = 0x42;
constexpr size_t kFbCsAp = 0x4C;
constexpr size_t kFbCsSp = 0x4E;
constexpr size_t kFbCsName = 0x18;
constexpr size_t kFbCsLevel = 0x38;
constexpr size_t kFbCsJob = 0x3A;
constexpr size_t kFbCsHp = 0x44;
constexpr size_t kFbCsMhp = 0x46;
constexpr size_t kFbCsMp = 0x48;
constexpr size_t kFbCsMmp = 0x4A;
constexpr size_t kFbCsExp = 0x50;
constexpr size_t kFbCsMoney = 0x58;
constexpr size_t kFbCsNextLevel = 0x80;
constexpr size_t kFbBsNmhp = 0x30;
constexpr size_t kFbBsNmmp = 0x34;
constexpr size_t kFbNextArr = 0x10;
constexpr size_t kFbLuName = 0x1B8;  // 无稳定字段哈希；仅 hint

size_t gOffWmMyUser = kFbWmMyUser;
size_t gOffWmCharacterData = kFbWmCharacterData;
size_t gOffWmBasicStat = kFbWmBasicStat;
size_t gOffCdCharacterStat = kFbCdCharacterStat;
size_t gOffCdItemSlots = kFbCdItemSlots;
size_t gOffCdSkillRecord = kFbCdSkillRecord;
size_t gOffCdSkillRecordEx = kFbCdSkillRecordEx;
size_t gOffCdSkillMasterLevel = kFbCdSkillMasterLevel;
size_t gOffCdSkillCooltime = kFbCdSkillCooltime;
size_t gOffCdSkillCoolTimeOver = kFbCdSkillCoolTimeOver;
size_t gOffWmSecondaryStat = kFbWmSecondaryStat;
size_t gOffSlotItemId = kFbSlotItemId;
size_t gOffSlotBundleNumber = kFbSlotBundleNumber;
size_t gOffCsCharacterId = kFbCsCharacterId;
size_t gOffCsStr = kFbCsStr;
size_t gOffCsDex = kFbCsDex;
size_t gOffCsInt = kFbCsInt;
size_t gOffCsLuk = kFbCsLuk;
size_t gOffCsAp = kFbCsAp;
size_t gOffCsSp = kFbCsSp;
size_t gOffCsName = kFbCsName;
size_t gOffCsLevel = kFbCsLevel;
size_t gOffCsJob = kFbCsJob;
size_t gOffCsHp = kFbCsHp;
size_t gOffCsMhp = kFbCsMhp;
size_t gOffCsMp = kFbCsMp;
size_t gOffCsMmp = kFbCsMmp;
size_t gOffCsExp = kFbCsExp;
size_t gOffCsMoney = kFbCsMoney;
size_t gOffCsNextLevel = kFbCsNextLevel;
size_t gOffBsNmhp = kFbBsNmhp;
size_t gOffBsNmmp = kFbBsNmmp;
size_t gOffNextArr = kFbNextArr;
size_t gOffLuName = kFbLuName;

std::atomic<bool> gFieldOffResolved{false};
char gFieldOffPath[96]{};

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);

FnFindAll gFindAll = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
bool gHaveValid = false;

std::atomic<int> gReadyStreak{0};
std::atomic<bool> gReadyLatched{false};

bool PlausibleOff(size_t off) { return off >= 0x10 && off < 0x1000; }

size_t FieldOffsetByHash(void* klass, const char* nameHash) {
    if (!klass || !nameHash || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    for (void* k = klass; k;) {
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
                if (PlausibleOff(off)) return off;
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
                    if (!nm || std::strcmp(nm, nameHash) != 0) continue;
                    size_t off = 0;
                    __try {
                        off = e.fieldGetOffset(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        off = 0;
                    }
                    if (PlausibleOff(off)) return off;
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
    if (!x::runtime::il2cpp::Ensure()) return;

    void* wm = x::runtime::il2cpp::FindClass("", kHashWorldManager);
    void* cd = x::runtime::il2cpp::FindClass("", kHashCharacterData);
    void* cs = x::runtime::il2cpp::FindClass("", kHashCharacterStat);
    void* bs = x::runtime::il2cpp::FindClass("", kHashBasicStat);
    void* nl = x::runtime::il2cpp::FindClass("", kHashNextLevel);
    if (!wm && !cd && !cs && !bs) return;  // 类表未齐，勿锁死 hint

    bool wmH = false, cdH = false, csH = false, bsH = false, nlH = false;
    if (wm) {
        gOffWmMyUser = PickOff(FieldOffsetByHash(wm, kHashWmMyUser), kFbWmMyUser, &wmH);
        gOffWmCharacterData =
            PickOff(FieldOffsetByHash(wm, kHashWmCharacterData), kFbWmCharacterData, &wmH);
        gOffWmBasicStat =
            PickOff(FieldOffsetByHash(wm, kHashWmBasicStat), kFbWmBasicStat, &wmH);
        gOffWmSecondaryStat =
            PickOff(FieldOffsetByHash(wm, kHashWmSecondaryStat), kFbWmSecondaryStat, &wmH);
    }
    if (cd) {
        gOffCdCharacterStat =
            PickOff(FieldOffsetByHash(cd, kHashCdCharacterStat), kFbCdCharacterStat, &cdH);
        gOffCdItemSlots =
            PickOff(FieldOffsetByHash(cd, kHashCdItemSlots), kFbCdItemSlots, &cdH);
        gOffCdSkillRecord =
            PickOff(FieldOffsetByHash(cd, kHashCdSkillRecord), kFbCdSkillRecord, &cdH);
        gOffCdSkillRecordEx =
            PickOff(FieldOffsetByHash(cd, kHashCdSkillRecordEx), kFbCdSkillRecordEx, &cdH);
        gOffCdSkillMasterLevel = PickOff(FieldOffsetByHash(cd, kHashCdSkillMasterLevel),
                                         kFbCdSkillMasterLevel, &cdH);
        gOffCdSkillCooltime =
            PickOff(FieldOffsetByHash(cd, kHashCdSkillCooltime), kFbCdSkillCooltime, &cdH);
        gOffCdSkillCoolTimeOver = PickOff(FieldOffsetByHash(cd, kHashCdSkillCoolTimeOver),
                                          kFbCdSkillCoolTimeOver, &cdH);
    }
    void* slotBase = x::runtime::il2cpp::FindClass("", kHashItemSlotBase);
    if (!slotBase) slotBase = x::runtime::il2cpp::FindClass("", "ItemSlotBase");
    void* slotBundle = x::runtime::il2cpp::FindClass("", kHashItemSlotBundle);
    if (!slotBundle) slotBundle = x::runtime::il2cpp::FindClass("", "ItemSlotBundle");
    bool slotH = false;
    if (slotBase) {
        size_t got = FieldOffsetByHash(slotBase, kHashSlotItemId);
        if (!got) got = FieldOffsetByHash(slotBase, kFldSlotItemId);
        gOffSlotItemId = PickOff(got, kFbSlotItemId, &slotH);
    }
    if (slotBundle) {
        size_t got = FieldOffsetByHash(slotBundle, kHashSlotBundleNumber);
        if (!got) got = FieldOffsetByHash(slotBundle, kFldSlotBundleNumber);
        gOffSlotBundleNumber = PickOff(got, kFbSlotBundleNumber, &slotH);
    }
    if (cs) {
        gOffCsCharacterId =
            PickOff(FieldOffsetByHash(cs, kHashCsCharacterId), kFbCsCharacterId, &csH);
        gOffCsStr = PickOff(FieldOffsetByHash(cs, kHashCsStr), kFbCsStr, &csH);
        gOffCsDex = PickOff(FieldOffsetByHash(cs, kHashCsDex), kFbCsDex, &csH);
        gOffCsInt = PickOff(FieldOffsetByHash(cs, kHashCsInt), kFbCsInt, &csH);
        gOffCsLuk = PickOff(FieldOffsetByHash(cs, kHashCsLuk), kFbCsLuk, &csH);
        gOffCsAp = PickOff(FieldOffsetByHash(cs, kHashCsAp), kFbCsAp, &csH);
        gOffCsSp = PickOff(FieldOffsetByHash(cs, kHashCsSp), kFbCsSp, &csH);
        gOffCsName = PickOff(FieldOffsetByHash(cs, kHashCsName), kFbCsName, &csH);
        gOffCsLevel = PickOff(FieldOffsetByHash(cs, kHashCsLevel), kFbCsLevel, &csH);
        gOffCsJob = PickOff(FieldOffsetByHash(cs, kHashCsJob), kFbCsJob, &csH);
        gOffCsHp = PickOff(FieldOffsetByHash(cs, kHashCsHp), kFbCsHp, &csH);
        gOffCsMhp = PickOff(FieldOffsetByHash(cs, kHashCsMhp), kFbCsMhp, &csH);
        gOffCsMp = PickOff(FieldOffsetByHash(cs, kHashCsMp), kFbCsMp, &csH);
        gOffCsMmp = PickOff(FieldOffsetByHash(cs, kHashCsMmp), kFbCsMmp, &csH);
        gOffCsExp = PickOff(FieldOffsetByHash(cs, kHashCsExp), kFbCsExp, &csH);
        gOffCsMoney = PickOff(FieldOffsetByHash(cs, kHashCsMoney), kFbCsMoney, &csH);
        gOffCsNextLevel =
            PickOff(FieldOffsetByHash(cs, kHashCsNextLevel), kFbCsNextLevel, &csH);
    }
    if (bs) {
        gOffBsNmhp = PickOff(FieldOffsetByHash(bs, kHashBsNmhp), kFbBsNmhp, &bsH);
        gOffBsNmmp = PickOff(FieldOffsetByHash(bs, kHashBsNmmp), kFbBsNmmp, &bsH);
    }
    if (nl) {
        gOffNextArr = PickOff(FieldOffsetByHash(nl, kHashNextArr), kFbNextArr, &nlH);
    }

    snprintf(gFieldOffPath, sizeof(gFieldOffPath), "wm=%s cd=%s cs=%s bs=%s nl=%s slot=%s",
             wmH ? "hash" : "hint", cdH ? "hash" : "hint", csH ? "hash" : "hint",
             bsH ? "hash" : "hint", nlH ? "hash" : "hint", slotH ? "hash" : "hint");
    gFieldOffResolved.store(true, std::memory_order_release);
    x::runtime::LogI(
        "Vitals",
        "field off money=0x%zX itemSlots=0x%zX skillRec=0x%zX coolOver=0x%zX ss=0x%zX path=%s",
        gOffCsMoney, gOffCdItemSlots, gOffCdSkillRecord, gOffCdSkillCoolTimeOver,
        gOffWmSecondaryStat, gFieldOffPath);
}

int16_t ReadI16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int64_t ReadI64(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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

uint32_t ReadU32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadIl2CppStringUtf8(void* str, char* out, size_t outSz) {
    if (!str || !out || outSz == 0) return false;
    out[0] = 0;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const wchar_t* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        const int n = WideCharToMultiByte(CP_UTF8, 0, chars, len, out, (int)outSz - 1, nullptr,
                                          nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BindFindAll() {
    if (gFindAll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    gFindAll = x::runtime::il2cpp::Get().findAll;
    return gFindAll != nullptr;
}

int ReadNextLevelExp(void* cs, int level) {
    void* next = ReadPtr(cs, gOffCsNextLevel);
    if (!next || level < 1) return 0;
    void* arr = ReadPtr(next, gOffNextArr);
    if (!arr) return 0;
    const uintptr_t n = ArrayLen(arr);
    if (n == 0 || (uintptr_t)level >= n) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(arr) + x::runtime::il2cpp_container::OffArrayData() +
            (uintptr_t)level * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void TryResolveLocalUser() {
    // SSOT = WM.MyUser。禁止「有缓存就永不重绑」——换图后旧指针会错乱。
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    void* myUser = LooksLikeHeapPtr(wm) ? ReadPtr(wm, gOffWmMyUser) : nullptr;
    if (LooksLikeHeapPtr(myUser)) {
        gLocalUser = myUser;
        return;
    }
    gLocalUser = nullptr;
    if (!x::features::ports::world::IsPlayReady()) return;
    if (!BindFindAll()) return;
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType || !gFindAll) return;
    void* luArr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 2000);
    const uintptr_t ln = ArrayLen(luArr);
    void* luBest = nullptr;
    for (uintptr_t i = 0; i < ln && i < 8; ++i) {
        void* lu = ArrayAt(luArr, i);
        if (!lu) continue;
        void* name = ReadPtr(lu, gOffLuName);
        char tmp[64]{};
        if (name && ReadIl2CppStringUtf8(name, tmp, sizeof(tmp))) {
            luBest = lu;
            break;
        }
        if (!luBest) luBest = lu;
    }
    gLocalUser = luBest;
}

void EnrichFromBasicStat(Vitals& out) {
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    void* bs = LooksLikeHeapPtr(wm) ? ReadPtr(wm, gOffWmBasicStat) : nullptr;
    if (!LooksLikeHeapPtr(bs)) return;
    out.mhp = (std::max)(out.mhp, ReadI32(bs, gOffBsNmhp));
    out.mmp = (std::max)(out.mmp, ReadI32(bs, gOffBsNmmp));
}

bool ReadFromCharacterStat(Vitals& out, void* cs) {
    out = {};
    if (!LooksLikeHeapPtr(cs)) return false;
    out.level = static_cast<int>(ReadU8(cs, gOffCsLevel));
    out.job = static_cast<int>(ReadI16(cs, gOffCsJob));
    out.hp = static_cast<int>(ReadI16(cs, gOffCsHp));
    out.mhp = static_cast<int>(ReadI16(cs, gOffCsMhp));
    out.mp = static_cast<int>(ReadI16(cs, gOffCsMp));
    out.mmp = static_cast<int>(ReadI16(cs, gOffCsMmp));
    out.exp = ReadI32(cs, gOffCsExp);
    out.meso = ReadI64(cs, gOffCsMoney);
    out.maxExp = ReadNextLevelExp(cs, out.level);

    EnrichFromBasicStat(out);

    void* nameObj = ReadPtr(cs, gOffCsName);
    if (!nameObj || !ReadIl2CppStringUtf8(nameObj, out.name, sizeof(out.name))) {
        TryResolveLocalUser();
        if (gLocalUser) {
            void* luName = ReadPtr(gLocalUser, gOffLuName);
            (void)ReadIl2CppStringUtf8(luName, out.name, sizeof(out.name));
        }
    }
    out.ok = out.level >= 1 && out.mhp > 0;
    return out.ok;
}

bool IsTrusted(const Vitals& v) {
    if (!v.ok) return false;
    if (v.hp == 0 && v.mp == 0) return false;
    if (v.hp <= 0 && v.mhp > 0) return false;  // dead
    if (v.mhp < 10) return false;
    return true;
}

}  // namespace

void Init() {
    gReadyStreak.store(0);
    gReadyLatched.store(false);
    gLocalUser = nullptr;
    gHaveValid = false;
    gFieldOffResolved.store(false, std::memory_order_release);
    EnsureFieldOffsets();
    x::runtime::LogI("Vitals", "player_vitals ready (WM→CharacterStat; field anti-drift)");
}

void Shutdown() {
    ClearBind();
    ClearReadyLatch();
}

void* LocalCharacterData() {
    EnsureFieldOffsets();
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    if (!LooksLikeHeapPtr(wm)) return nullptr;
    void* cd = ReadPtr(wm, gOffWmCharacterData);
    return LooksLikeHeapPtr(cd) ? cd : nullptr;
}

void* LocalCharacterStat() {
    EnsureFieldOffsets();
    void* cd = LocalCharacterData();
    if (!cd) return nullptr;
    void* cs = ReadPtr(cd, gOffCdCharacterStat);
    return LooksLikeHeapPtr(cs) ? cs : nullptr;
}

bool ReadBaseApStats(BaseApStats& out) {
    out = {};
    EnsureFieldOffsets();
    void* cs = LocalCharacterStat();
    if (!LooksLikeHeapPtr(cs)) return false;
    out.characterId = ReadU32(cs, gOffCsCharacterId);
    out.str = ReadI16(cs, gOffCsStr);
    out.dex = ReadI16(cs, gOffCsDex);
    out.intel = ReadI16(cs, gOffCsInt);
    out.luk = ReadI16(cs, gOffCsLuk);
    out.ap = ReadI16(cs, gOffCsAp);
    out.job = ReadI16(cs, gOffCsJob);
    out.ok = true;
    return true;
}

bool ReadBaseSpStats(BaseSpStats& out) {
    out = {};
    EnsureFieldOffsets();
    void* cs = LocalCharacterStat();
    if (!LooksLikeHeapPtr(cs)) return false;
    out.characterId = ReadU32(cs, gOffCsCharacterId);
    out.level = static_cast<int>(ReadU8(cs, gOffCsLevel));
    out.job = static_cast<int>(ReadI16(cs, gOffCsJob));
    out.sp = ReadI16(cs, gOffCsSp);
    out.ok = true;
    return true;
}

int64_t ReadMoney() {
    void* cs = LocalCharacterStat();
    if (!cs) return -1;
    __try {
        return *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(cs) + gOffCsMoney);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* GetItemSlotList(int invType) {
    if (invType < 0) return nullptr;
    void* cd = LocalCharacterData();
    if (!cd) return nullptr;
    void* slotsArr = ReadPtr(cd, gOffCdItemSlots);
    if (!LooksLikeHeapPtr(slotsArr)) return nullptr;
    const uintptr_t n = ArrayLen(slotsArr);
    if (static_cast<uintptr_t>(invType) >= n) return nullptr;
    void* list = ArrayAt(slotsArr, static_cast<uintptr_t>(invType));
    return LooksLikeHeapPtr(list) ? list : nullptr;
}

size_t OffSlotItemId() {
    EnsureFieldOffsets();
    return gOffSlotItemId;
}

size_t OffSlotBundleNumber() {
    EnsureFieldOffsets();
    return gOffSlotBundleNumber;
}

size_t OffSkillRecord() {
    EnsureFieldOffsets();
    return gOffCdSkillRecord;
}

size_t OffSkillRecordEx() {
    EnsureFieldOffsets();
    return gOffCdSkillRecordEx;
}

size_t OffSkillMasterLevel() {
    EnsureFieldOffsets();
    return gOffCdSkillMasterLevel;
}

size_t OffSkillCooltime() {
    EnsureFieldOffsets();
    return gOffCdSkillCooltime;
}

size_t OffSkillCoolTimeOver() {
    EnsureFieldOffsets();
    return gOffCdSkillCoolTimeOver;
}

void* LocalSecondaryStat() {
    EnsureFieldOffsets();
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    if (!LooksLikeHeapPtr(wm)) return nullptr;
    void* ss = ReadPtr(wm, gOffWmSecondaryStat);
    return LooksLikeHeapPtr(ss) ? ss : nullptr;
}

void* LocalMyUser() {
    EnsureFieldOffsets();
    void* wm = x::features::ports::world::PeekWorldManager();
    if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
    if (!LooksLikeHeapPtr(wm)) return nullptr;
    void* mu = ReadPtr(wm, gOffWmMyUser);
    return LooksLikeHeapPtr(mu) ? mu : nullptr;
}

size_t OffWmMyUser() {
    EnsureFieldOffsets();
    return gOffWmMyUser;
}

size_t OffWmCharacterData() {
    EnsureFieldOffsets();
    return gOffWmCharacterData;
}

size_t OffCdCharacterStat() {
    EnsureFieldOffsets();
    return gOffCdCharacterStat;
}

bool Read(Vitals& out) {
    out = {};
    EnsureFieldOffsets();
    void* cs = LocalCharacterStat();
    if (!ReadFromCharacterStat(out, cs)) {
        if (gHaveValid) {
            ClearBind();
            ClearReadyLatch();
        }
        return false;
    }
    gHaveValid = true;
    return true;
}

bool ResolveAndRead(Vitals& out, DWORD /*now*/, bool /*forceRebind*/) { return Read(out); }

bool ReadCached(Vitals& out) { return Read(out); }

void ClearBind() {
    gLocalUser = nullptr;
    gHaveValid = false;
}

bool HasBind() { return LocalCharacterStat() != nullptr; }

void NoteSample(const Vitals& v, DWORD /*now*/) {
    if (!IsTrusted(v)) {
        gReadyStreak.store(0);
        gReadyLatched.store(false);
        return;
    }
    // 单次可信即 latch。登录/商城由调用方 IsPlayReady / land grace 挡住，不必再堆 3 拍。
    gReadyStreak.store(1);
    gReadyLatched.store(true);
}

void ClearReadyLatch() {
    gReadyStreak.store(0);
    gReadyLatched.store(false);
}

bool IsReadyLatched() { return gReadyLatched.load(); }

int HpPct(const Vitals& v) {
    if (v.mhp <= 0) return -1;
    return (int)((int64_t)v.hp * 100 / v.mhp);
}

int MpPct(const Vitals& v) {
    if (v.mmp <= 0) return -1;
    return (int)((int64_t)v.mp * 100 / v.mmp);
}

bool IsDead(const Vitals& v) { return v.ok && v.mhp > 0 && v.hp <= 0; }

}  // namespace x::ui::player
