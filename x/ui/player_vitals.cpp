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
    "b8ea8013e52dada590b6003b130193bf382fb78e9581ae899270652538d4114";
constexpr char kHashCharacterData[] =
    "a49dae5b55d4270768c375de143fbd802b4c903c6529f0921e6d8736655a348";
constexpr char kHashCharacterStat[] =
    "f82c863b4bb8d44337bded289f308bbb602a909ad603f79f5d89648aae571eb";
constexpr char kHashBasicStat[] =
    "f75a286efbb1904ed9d2de8547e25110a7dc5ba087e1750e5cbf8544569e187";
constexpr char kHashNextLevel[] =
    "c8d77805e4e24a8878c239a1d2a0095d3d0c4aa061bc50fe977e4699ab7d7cd";  // TypeDef 1834 Nextlevel
constexpr char kHashItemSlotBase[] =
    "f9b53f2280c83a615300e6964838cc11928686fde46752d193222ca2428f4f2";  // TypeDef 1837
constexpr char kHashItemSlotBundle[] =
    "b81ce9aaf6724879d7997983bb9bcb454c20855511a8adc0e683852d2e8b065";  // TypeDef 1840

// WM
constexpr char kHashWmMyUser[] =
    "<c1c262eeb2fd710395259b6a9ed860900becf2a5716099c31c863589707a7a0>k__BackingField";
// 2026-08-06 BIN：旧哈希 c39c747b… 实为 WM+0xA8 的 bool；真 CharacterData* @0xE0。
constexpr char kHashWmCharacterData[] =
    "d0c7dbfd8113c5401c35c284cecd471297e1389c07cd988f8454e0dad2e8cd5";
// BasicStat* @0xE8（旧 a96d3d20… 是 int[] @0xB0）
constexpr char kHashWmBasicStat[] =
    "d915e22b61883d43ab9019e8b31522087ef47aa0aed0db6d60f3497614f4732";
// CharacterData
constexpr char kHashCdCharacterStat[] =
    "ef74036091835e6779a41cd9810a1f2d8355d1f875397f09bb15146f8d96446";
constexpr char kHashCdItemSlots[] =
    "fde7df0fa7e3b0079835beca1b115d50a4ff109917a639e5ab969f439722cc5";  // List<ItemSlotBase>[]
constexpr char kHashCdSkillRecord[] =
    "f87a083146ef2faab83b654647ff543f326c7571c335686a85fdc3a497d734b";  // Dict<int,int>
constexpr char kHashCdSkillRecordEx[] =
    "b9018481e4ef042c5bce0c7558c32923c944abedf61919e4bfc2e00a7ef28bc";
constexpr char kHashCdSkillMasterLevel[] =
    "cc86c31c8eded69a97ceb54b4e4489ebc69d00e1d398b2ba55fbd43ddf70dc1";  // Dict<int,int>
constexpr char kHashCdSkillCooltime[] =
    "affe95a08c5e7e61cf4eb7738bb55c73df3d3609114a872ff0256606ee31616";  // Dict<int,ushort>
constexpr char kHashCdSkillCoolTimeOver[] =
    "ce6f9f479c23d4d073018cfaa1893c1fe5ff2260565b489d52a4bd8f2ea6ca7";  // Dict<int,int>
constexpr char kHashWmSecondaryStat[] =
    "c6b015fcb6ef4e81c41a704983383ca00063e895216255eb8b0c458396c07ab";  // WM+0xF0；勿用 +0xB8 嵌套 struct
// ItemSlotBase / ItemSlotBundle（08-06 remount：类名已哈希，字段亦哈希）
constexpr char kHashSlotItemId[] =
    "bc4ee7b098ef21ebc6d8cbab53c844b785a67be64bd506ad5b9ca22b39bbcf1";  // ItemId @0x10
constexpr char kHashSlotBundleNumber[] =
    "f1d9abc840c4778dff3213f94672f76cd6c17808032ab4c817f2c76bb0d9898";  // nNumber @0x28
constexpr char kFldSlotItemId[] = "ItemId";           // restored 明文兜底
constexpr char kFldSlotBundleNumber[] = "nNumber";
// CharacterStat（08-06 remount · TypeDef 1833）
constexpr char kHashCsCharacterId[] =
    "ce3c2f6f4d94ed1e0301d12d81c330e1eda0cdb8165420b89b8ac2ba7fc5be8";  // CharacterID @0x10
constexpr char kHashCsStr[] =
    "ff51aa75687abdaa4881235d913373e6d02c2b69aeea94a7e2fa99ab21be781";  // nSTR @0x3C
constexpr char kHashCsDex[] =
    "bd76edd42850f219f5c607133f060e8d0d9a54158b7540adb5d07379378eb56";  // nDEX @0x3E
constexpr char kHashCsInt[] =
    "d4f38671722275230846437d95dc3b404edcb730c0780eca8dfe9b6b06026d8";  // nINT @0x40
constexpr char kHashCsLuk[] =
    "aa971c2634bf520042761e1456feda07a589b60f669cd4a7b2125578f8276e0";  // nLUK @0x42
constexpr char kHashCsAp[] =
    "a8828b97f66f0a9eba6317ebf8016c5008bf33a360da9cbd896325384a97f0e";  // ap @0x4C
constexpr char kHashCsName[] =
    "b02256291997e9e0ee635ebfe60659219c81d43db37ecaf35c18216605afd3b";  // CharacterName @0x18
constexpr char kHashCsLevel[] =
    "daa9c566a29098609e5d343022c8d4ce61f9b76c94b32ef479cd105d4062de8";  // level @0x38
constexpr char kHashCsJob[] =
    "e3e9110dda0b4f67302711a94a07efe0066540d7e7a8b40b71ff6584e3a4881";  // job @0x3A
constexpr char kHashCsHp[] =
    "f00356efbd651633864826d8ea6f1641e88e3ecfe0dc5783e530393f4109337";  // hp @0x44
constexpr char kHashCsMhp[] =
    "f6988f2a9c7525f484446f08cdaba311bc15de960751c293a847b84c31318a8";  // mhp @0x46
constexpr char kHashCsMp[] =
    "aa453079056071cd0ba4d13eb9a8ae1e030ad4a7bbf84420ea19c6e63836ffd";  // mp @0x48
constexpr char kHashCsMmp[] =
    "eafca1048523d42236e15c9e63af12894834cceed8e8517e850b476c7e9dd05";  // mmp @0x4A
constexpr char kHashCsExp[] =
    "bf87c6ebff59ebf6849cc2ec04ebda2cebeb17b0fe16d0697b8e1783af6a9cd";  // exp @0x50
constexpr char kHashCsMoney[] =
    "fb9dd268a3da559552c2dc82a2ac81dd3c7c483b025ab13a26f6c52e6b075f3";  // money @0x58
constexpr char kHashCsNextLevel[] =
    "f79ea0f3a059a59ab0c1d3c5e82fa609a2e945d161aeb20da4f7fa6c061d196";  // Nextlevel* @0x80
// BasicStat（08-06 remount · TypeDef 1322）
constexpr char kHashBsNmhp[] =
    "a1253b618bba24d6e3b10d513891e0c78806d615d166bfd37bdd15a7b9ffbb8";  // nMHP @0x30
constexpr char kHashBsNmmp[] =
    "e1818fd7aea5d1efe30d6761e4a948badde01fd01f8cb1978c94b0bdb30776f";  // nMMP @0x34
// Nextlevel.int[]（08-06 remount）
constexpr char kHashNextArr[] =
    "aa8dd66a4c65b66a69ec9936d8d4690f2f0949c01064380639586fd5213d1fc";  // @0x10

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
