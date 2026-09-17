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
    "cba21c42799e50c37623ea5fd88d5c4c9155c2565d25cde936671684e16dff7";
constexpr char kHashCharacterData[] =
    "b0b3ee7ea1f9e3dfe390adcc6c0f8b3ccc6703e20ecab8769f3dea0c661cda9";
constexpr char kHashCharacterStat[] =
    "b2b8bf9af199f117a239e2a496a72f9d29e58cb48d882af4c3de1d802b8a0c4";
constexpr char kHashBasicStat[] =
    "d05fafd803e5944e7bc93d32f4902f87a1236896d60b0660429e683aebf4f6f";
constexpr char kHashNextLevel[] =
    "af6b39e5bbb2a7f04578ac0c6dd6dd5e06d9bcc465be6fad6c36d8c2f0d8571";  // TypeDef 1834 Nextlevel
constexpr char kHashItemSlotBase[] =
    "f70996d114bf2f4cf0c985796a65ec566369c1014a1edd476a8287f323d72b1";  // TypeDef 1761
constexpr char kHashItemSlotBundle[] =
    "c2bf5447cf3e555d11ff3e91b1258711232145693205380dda31081d3637750";  // TypeDef 1762

// WM
constexpr char kHashWmMyUser[] =
    "<f05b550317985a2eaa8fadb32ffffbe82cdaf841dba68e5d35d2dcf99ed6a13>k__BackingField";
// 2026-08-06 BIN：旧哈希 c39c747b… 实为 WM+0xA8 的 bool；真 CharacterData* @0xE0。
constexpr char kHashWmCharacterData[] =
    "c360c98c279935acfad02204ec9da405a5dc1173561ea0e1047e4dbc580b1b5";
// BasicStat* @0xE8（旧 a96d3d20… 是 int[] @0xB0）
constexpr char kHashWmBasicStat[] =
    "a014b1d70c65c029aa27d1ff3cff2c7cc4f5b8f1322c4b66006576cde41127f";
// CharacterData
constexpr char kHashCdCharacterStat[] =
    "de7875a59acba7406d5c9bfa1b33af59ca6d4eea81521da12afaaf6fb388e71";
constexpr char kHashCdItemSlots[] =
    "fdceb47c0740b59430fb305f0860087dac0c18a4d861d881f5afe666af2ee73";  // List<ItemSlotBase>[]
constexpr char kHashCdSkillRecord[] =
    "fea16fb978951097c31df5f1dea708854895a36be2d8e6347f49b46135c6575";  // Dict<int,int>
constexpr char kHashCdSkillRecordEx[] =
    "cd0ee652e15ebfc90082d1dfa57194fed7c40b45431def425c1c516aa40f705";
constexpr char kHashCdSkillMasterLevel[] =
    "bc1cc2c58ebe509e460a1fa4ab1b142e6c30e89823eef717e2016be25793737";  // Dict<int,int>
constexpr char kHashCdSkillCooltime[] =
    "d529282a841b96ee2656cb158b667cb3303c9cbd65135d5493d347ffa001770";  // Dict<int,ushort>
constexpr char kHashCdSkillCoolTimeOver[] =
    "d183a6f0ef1e99af992e130fa0b78573a8ed9d4d503d763cf8686af605a7e1f";  // Dict<int,int>
constexpr char kHashWmSecondaryStat[] =
    "c36eec52194719506c3bd095d3e61cd6f5995ca63c53c9e230651a6a7e2a6f3";  // WM+0xF0；勿用 +0xB8 嵌套 struct
// ItemSlotBase / ItemSlotBundle（08-06 remount：类名已哈希，字段亦哈希）
constexpr char kHashSlotItemId[] =
    "c81b6812a4e7d60bc3b07ef14041ea4c2399fc7b44abd8869a4e7979d751ae3";  // ItemId @0x10
constexpr char kHashSlotBundleNumber[] =
    "cd85e37ec226d0aa2567cfee7f7c9eb77ffce5e379fbf388bd5c64e466c741a";  // nNumber @0x30
constexpr char kFldSlotItemId[] = "ItemId";           // restored 明文兜底
constexpr char kFldSlotBundleNumber[] = "nNumber";
// CharacterStat（08-06 remount · TypeDef 1833）
constexpr char kHashCsCharacterId[] =
    "efe148f9f70133f610f70d6733ff17e4e6794061aeeb73952b127bc242a80fc";  // CharacterID @0x10
constexpr char kHashCsStr[] =
    "c3a7272439cb274c3b50b920c3c6b48515a478a1602cb166ecb443acd0724dd";  // nSTR @0x3C
constexpr char kHashCsDex[] =
    "ff361a2270a56444d5de7b4f28f216dce5016833ee6f30390fbe597f36ab77f";  // nDEX @0x3E
constexpr char kHashCsInt[] =
    "eb39ba0f15c22d3754c51a39518aacbb0aee97bd0e0f6c62de87258cefc5afc";  // nINT @0x40
constexpr char kHashCsLuk[] =
    "f1e10ea8e725ca8c13fa18b1f2cce1d65efaa318c9a40d54d992d5f44eb39b1";  // nLUK @0x42
constexpr char kHashCsAp[] =
    "ba2ff65b18e6b7bf1c0d4d9045d12492ade661902db140b6910fccc05377277";  // ap @0x4C
constexpr char kHashCsSp[] =
    "f349c9eef073b42c7c3697502d2385fca09aff0539b14c4fae6d4c9179ccef7";  // sp @0x4E
constexpr char kHashCsName[] =
    "f5c93474139fb5a3623566f6ac5e00b601076256f03fdcbe80aeb7f3ace03ed";  // CharacterName @0x18
constexpr char kHashCsLevel[] =
    "f4295ca06340b8c64ee83ce5973188e5574ac217994166c72cc787cd2f21ea5";  // level @0x38
constexpr char kHashCsJob[] =
    "c375f51afce2cce7c04035d9fe64ca983d2812981457006a3cea2faee12d714";  // job @0x3A
constexpr char kHashCsHp[] =
    "c1126cf1fdefa980fbfb6e66df701ea807e54f8d1a21c84d2c525823cb01dbd";  // hp @0x44
constexpr char kHashCsMhp[] =
    "cee0fa5124f585c08f83ae03533e2672a145e11036d8ef37c79eeab402483be";  // mhp @0x46
constexpr char kHashCsMp[] =
    "edb7f97fb9028bfba1c10bc385c45e8e5473f3b316bbacc1dabbf6b21cb9ffa";  // mp @0x48
constexpr char kHashCsMmp[] =
    "dc5196a91c740f73276051f12805f668723f86148d2e914b74f955ab7d935ef";  // mmp @0x4A
constexpr char kHashCsExp[] =
    "e585c54e78aee0fdaf9695fba857ed831bef7f8f79f68f563bb513581776e29";  // exp @0x50
constexpr char kHashCsMoney[] =
    "cd13e73a0da19df018e6ead431a1b5dba4851ed3aff4b800b84155a184a12f7";  // money @0x58
constexpr char kHashCsNextLevel[] =
    "de9ec4932513f272008f6bdbc7f68198475d0f6e61437eea9b5d1ad08bbf490";  // Nextlevel* @0x80
// BasicStat（08-06 remount · TypeDef 1322）
constexpr char kHashBsNmhp[] =
    "bc3990c5d4a19052520a8869e0fe86a0cec8b7c149c82d752bf90c3ee487426";  // nMHP @0x30
constexpr char kHashBsNmmp[] =
    "b35f8612ce99f5595ee0075ae13da3c7fc1ede1a29a189a4ea490f497b5acc9";  // nMMP @0x34
// Nextlevel.int[]（08-06 remount）
constexpr char kHashNextArr[] =
    "f07accb8c8943a52de69e033d298ac502b6bba42af2042504bf78a2720e2513";  // @0x10

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
constexpr size_t kFbSlotBundleNumber = 0x30;  // ItemSlotBundle.nNumber（0x28 现为基类 nPOS）
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
    // 09-10：ItemSlots[nTI] 直下标。Consume=1（BIN GetItemSlotPos）。商店 UITab 勿套到这里。
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
