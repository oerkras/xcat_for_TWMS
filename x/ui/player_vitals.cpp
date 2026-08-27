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
    "da19eb0b093a50825187352f1d062610ff5b23c1f314b9cd8c4218aa9db3bc7";
constexpr char kHashCharacterData[] =
    "a5319803ef38578b7e1149ce0fcdaea9578a09b85581424fc138483df1a5d4c";
constexpr char kHashCharacterStat[] =
    "a9d030168661a61a8c5dd81c2c2ffc69f02ec26db58bb0e8bbc5d1fbe09a602";
constexpr char kHashBasicStat[] =
    "f76f448e63973a576e234c63ce65667c6ad8932fae6bc259ef25ca4e3ddc2e0";
constexpr char kHashNextLevel[] =
    "a1eddf3ba29d04f58f9dd97204303d4ca6d464ec843a1d8a5a7646ad31ee749";  // TypeDef 1834 Nextlevel
constexpr char kHashItemSlotBase[] =
    "bbf51728e4c83ce9b195490e9794b7f6c4693d40932a25c5aa477f3a0fff4ac";  // TypeDef 1837
constexpr char kHashItemSlotBundle[] =
    "c6261119b918d5bdc4384d29708d290aa8777a3541eb7db17b377ecec948355";  // TypeDef 1840

// WM
constexpr char kHashWmMyUser[] =
    "<aa14627d9f4fe9d3642086a9bdb75516742da955d24531087cfb70e3b795d4e>k__BackingField";
// 2026-08-06 BIN：旧哈希 c39c747b… 实为 WM+0xA8 的 bool；真 CharacterData* @0xE0。
constexpr char kHashWmCharacterData[] =
    "e3e20d780229c364edef5a4005e814a139e70d9cf2aeb4ce562f804ac798344";
// BasicStat* @0xE8（旧 a96d3d20… 是 int[] @0xB0）
constexpr char kHashWmBasicStat[] =
    "cd940eb424fc5433b2e490dab62d2be40318f6001b57893a68163eff50a2f3c";
// CharacterData
constexpr char kHashCdCharacterStat[] =
    "e9cb41585fa465765ea0e10834f89aa0493fa5aaef4b22ca0d48b993bbee980";
constexpr char kHashCdItemSlots[] =
    "c9b5f84391ef81cc30d53a0e7afbbe0786611fbf32a6868970f0b4adccc46e0";  // List<ItemSlotBase>[]
constexpr char kHashCdSkillRecord[] =
    "a560d40d61beb3ddcbb10b97d4378a567ba969bb332036e4e7460e83059d8fe";  // Dict<int,int>
constexpr char kHashCdSkillRecordEx[] =
    "c8fb30d31e18251e0c445935d2ff1806734a48c16c7e7fa72aef23d874c520e";
constexpr char kHashCdSkillMasterLevel[] =
    "d53e2a7a0af9152f16f3d36a2f04f7faf25c35ab55b5b5cb2aca2c2b8b673e7";  // Dict<int,int>
constexpr char kHashCdSkillCooltime[] =
    "fa963c233e19654cf6aa90513c30cc0634089cffe713023a49ce4318a8a3017";  // Dict<int,ushort>
constexpr char kHashCdSkillCoolTimeOver[] =
    "ae5e26e651d3babf032e398ce55161ae5496e9db6d38acf8e03a5dfb14b39d5";  // Dict<int,int>
constexpr char kHashWmSecondaryStat[] =
    "cdf73b9d2da8bc0c19c29b7a49e70bb4a05e826a871b11f3b17a9ce2f0d318a";  // WM+0xF0；勿用 +0xB8 嵌套 struct
// ItemSlotBase / ItemSlotBundle（08-06 remount：类名已哈希，字段亦哈希）
constexpr char kHashSlotItemId[] =
    "f56700a8e6da7c5cfce4064c8147894fd79c2050cfc008e8ca37580a080ad42";  // ItemId @0x10
constexpr char kHashSlotBundleNumber[] =
    "a65edea5c27bbc72d78dc4c7ad442105edca118b2b95b5d50fc40ece4591f1b";  // nNumber @0x28
constexpr char kFldSlotItemId[] = "ItemId";           // restored 明文兜底
constexpr char kFldSlotBundleNumber[] = "nNumber";
// CharacterStat（08-06 remount · TypeDef 1833）
constexpr char kHashCsCharacterId[] =
    "bf4159b04cef27ab5b72b48b33cf87dfa885e43f77834f2021ac9e9c495bc99";  // CharacterID @0x10
constexpr char kHashCsStr[] =
    "a15a371bb3450c6e1fc2f42833c5d5abf14fc16cda3118dcf4fe1168c992977";  // nSTR @0x3C
constexpr char kHashCsDex[] =
    "dd32c74345c1e656ab8ba79d155f4892190f2dae720d48fa0c539e6c92168f8";  // nDEX @0x3E
constexpr char kHashCsInt[] =
    "db2756a4d3e9c219f0f83e6d730b411841b75613969f1bf77fcce2a4c10e587";  // nINT @0x40
constexpr char kHashCsLuk[] =
    "bf3af14bbb9c4736774314ec1b221444b7058a2875339f7067016ca66fd500d";  // nLUK @0x42
constexpr char kHashCsAp[] =
    "af969a355e22ebe28d5d685d237d05ee1fd5e7e902cc092a711c2a2c5cab69a";  // ap @0x4C
constexpr char kHashCsSp[] =
    "f2dcf4cc91e4eb9b0ab253dfb0d09acfcbec444151f5856cf43b3916d63ec8d";  // sp @0x4E
constexpr char kHashCsName[] =
    "c2cb5aff8435cfa50600dd97f69238f28d5139d60033c76b3aef8645710a57d";  // CharacterName @0x18
constexpr char kHashCsLevel[] =
    "ff425ec8aeb9a69a30d86a6f79dbc3e3171055b86570053177641fd5b78fe82";  // level @0x38
constexpr char kHashCsJob[] =
    "dda835cf3a55a0e22b35863995610089005c1ca644bc1b99ae1473e01872600";  // job @0x3A
constexpr char kHashCsHp[] =
    "c6184d60929f589d48a8bd277d0847a27501a92216cbb30c714cdb449a8c7c4";  // hp @0x44
constexpr char kHashCsMhp[] =
    "fea9d42a1f87dc4d0bd396172e95fa4cc0dc0df1653e4f9cd07c498eda79337";  // mhp @0x46
constexpr char kHashCsMp[] =
    "bd916722163349d556d3c248cdcc968250467ad0c627b2a7a2a7b3fd0cd16bd";  // mp @0x48
constexpr char kHashCsMmp[] =
    "f7598ffd859dd8c64a99087888acb2344230602b5feb8076856cc08ea972659";  // mmp @0x4A
constexpr char kHashCsExp[] =
    "c704c700bcc11a3d71436fa5e3d7a9eef18927112d473dbb95bd59fac04ae61";  // exp @0x50
constexpr char kHashCsMoney[] =
    "fbfe5bcd331f24d1a25940c2322405c3ff10b765c61ffa5a9f9791c12b9391d";  // money @0x58
constexpr char kHashCsNextLevel[] =
    "d7bf4a329ca2ed1d6e49381ba5e4cbaa8564ef9e9792a7584ab1458e118c132";  // Nextlevel* @0x80
// BasicStat（08-06 remount · TypeDef 1322）
constexpr char kHashBsNmhp[] =
    "b44e3e8dd4988ec6d4d3a51c1f73d65499b44046928c867978781a70de221be";  // nMHP @0x30
constexpr char kHashBsNmmp[] =
    "ab62439a3f9b485b923e3eb71c2e4f608232f8a343be8c703c26ec71bca794e";  // nMMP @0x34
// Nextlevel.int[]（08-06 remount）
constexpr char kHashNextArr[] =
    "f89ab1b52588744c3722f448279f1a9398e736e4a6dd0badf4e8430aec74f75";  // @0x10

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
