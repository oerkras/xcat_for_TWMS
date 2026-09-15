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
    "c55180bcf183a5b10bb74f56544b6900e3a5dae78d2ff5a6c8587b0e4c399fe";
constexpr char kHashCharacterData[] =
    "fc09ef0e0d54c07042a56575d966628802e69f257ba527a57f00748e67313b7";
constexpr char kHashCharacterStat[] =
    "e6e36b4ab8ad4ffd8b8a7a3dcedfa1f5098c06ba2caff3a43a6586044fc48eb";
constexpr char kHashBasicStat[] =
    "fc7b2599233cbdd7d81d64332027eca09f0ee02d15d2976cdefe31f2f8c2ca1";
constexpr char kHashNextLevel[] =
    "a4f16c1be69b8bae5dcc6abde1812fa64e3d61811054bac80335e10b54c75e9";  // TypeDef 1834 Nextlevel
constexpr char kHashItemSlotBase[] =
    "f4124c869a21043b4e4b180bd9d87ad6f53eab2a7c5c1c8cb319ba3fa7ffac1";  // TypeDef 1761
constexpr char kHashItemSlotBundle[] =
    "de4a237f4bd7fa470f0546d39d651f633f8df17c890fd2863084f398eb6c353";  // TypeDef 1762

// WM
constexpr char kHashWmMyUser[] =
    "<e8188e62384d9888cf8dbf795b258a6cdda4933a2b3bba9725997db5ed14b83>k__BackingField";
// 2026-08-06 BIN：旧哈希 c39c747b… 实为 WM+0xA8 的 bool；真 CharacterData* @0xE0。
constexpr char kHashWmCharacterData[] =
    "eefb2ac8c168ccba5b10936a5eeb98b926a6dde1b1d8a63c69f126b3a38abf8";
// BasicStat* @0xE8（旧 a96d3d20… 是 int[] @0xB0）
constexpr char kHashWmBasicStat[] =
    "a69bc16fc0cf5897421595db2ab8cd2b1b3165d697f3c8fb8fff3c053f00550";
// CharacterData
constexpr char kHashCdCharacterStat[] =
    "ffcc7f1b853760d2a81a13f30f848be42c616b43ff40f3fd8e9126eaca7042b";
constexpr char kHashCdItemSlots[] =
    "cff110654d704ceaef1c4f120232f99e2cfe3483145be6eddd4c0968eb8a553";  // List<ItemSlotBase>[]
constexpr char kHashCdSkillRecord[] =
    "a198d4972d2da55dffcbc218f19ffee92d1242763e72f467a97553b3b850a9f";  // Dict<int,int>
constexpr char kHashCdSkillRecordEx[] =
    "e45d489450a19bf3dbef88b01c54613a89b7ccb6d6ede93779166f13a27f313";
constexpr char kHashCdSkillMasterLevel[] =
    "af675c00bb586b4114f0773c8883d94f7bb12c5b53df4f047f6c358004ea817";  // Dict<int,int>
constexpr char kHashCdSkillCooltime[] =
    "cfb115c8103a5dad64a05a04417664beaadefa08111da9416fb103afe46b916";  // Dict<int,ushort>
constexpr char kHashCdSkillCoolTimeOver[] =
    "fdfce06377c667c295cabcb26ced2eba7cc8f37454c70ed32fa9128db9926c2";  // Dict<int,int>
constexpr char kHashWmSecondaryStat[] =
    "b0c0a50e8ae94d0b59b245de1df4ea415c7d0126f496d8e38ab18534cf310a9";  // WM+0xF0；勿用 +0xB8 嵌套 struct
// ItemSlotBase / ItemSlotBundle（08-06 remount：类名已哈希，字段亦哈希）
constexpr char kHashSlotItemId[] =
    "cd9da19da3fc26d1f45ea3ef55b667b2406400081ad46b465c66500a1c5bc2d";  // ItemId @0x10
constexpr char kHashSlotBundleNumber[] =
    "cf980c33d088e801890cce0708e51658391028b9ec7096d7446d8ccde91bcc5";  // nNumber @0x30
constexpr char kFldSlotItemId[] = "ItemId";           // restored 明文兜底
constexpr char kFldSlotBundleNumber[] = "nNumber";
// CharacterStat（08-06 remount · TypeDef 1833）
constexpr char kHashCsCharacterId[] =
    "d51f7dcf073c8f913965514e362042add823914e178e0d485203f98e982a363";  // CharacterID @0x10
constexpr char kHashCsStr[] =
    "e6b2758c6d6f3485f1f030d5973aba2ffab1dd7d3c87e649d78e27f95012d01";  // nSTR @0x3C
constexpr char kHashCsDex[] =
    "b2893907f6bde17422877e47499f261b376971db51ea795c25e9a2a9e42a8c9";  // nDEX @0x3E
constexpr char kHashCsInt[] =
    "db7f85d36765658e0b91ad4a1a43f155b4375178726079430fd232ff3f40eae";  // nINT @0x40
constexpr char kHashCsLuk[] =
    "d419aee3405f3b17807021f8ff9d60edb39c3e8125a3faa05579b9e1d4291f2";  // nLUK @0x42
constexpr char kHashCsAp[] =
    "d147649e2debd4a42e0e002cb02e65c8cda6c43ac3f1c73229f0beeb03a8126";  // ap @0x4C
constexpr char kHashCsSp[] =
    "afd645aed2dff07ebd22956aa8cf87320d23f74def695fb3d0b9d56c6e45468";  // sp @0x4E
constexpr char kHashCsName[] =
    "ba0db6204206883982801b52aa21065a168971ccd630eac62968840cb6153c2";  // CharacterName @0x18
constexpr char kHashCsLevel[] =
    "dbf1a56aa2d7d6f2bd3ba05f0cb9e7ca1a8b42d27153ba66a0e041fb57b9a7a";  // level @0x38
constexpr char kHashCsJob[] =
    "f4844a73a82679b9057c42b6ad035fa17e29317ae1d8e78c67076ab2e7583a9";  // job @0x3A
constexpr char kHashCsHp[] =
    "b0327857f306ab95d8e4fcc7badc411a86270c154d25b5a11294f9c1b95f144";  // hp @0x44
constexpr char kHashCsMhp[] =
    "a5e105c4062d8ce5eaccd946a0e92f3c5e59d26e3566d704e02310636c92ebd";  // mhp @0x46
constexpr char kHashCsMp[] =
    "f29f604dd26034efd52a2a04bd912bdd5c28aafbb803869ba489744fe9d0bee";  // mp @0x48
constexpr char kHashCsMmp[] =
    "fe518481b2771cec633e572144d217b5372ec971e669e0210461e30c40a7716";  // mmp @0x4A
constexpr char kHashCsExp[] =
    "a4117f68e6e47c8cded37719f1035c120d89207c73867212bc6a2bdf0e63408";  // exp @0x50
constexpr char kHashCsMoney[] =
    "e8d1d1f11af46e9eb273e85749935ad3ef34dccd146550ed473ed6612d72a1a";  // money @0x58
constexpr char kHashCsNextLevel[] =
    "c2f549ed6c00ccae89ff1a49d28c07b5430c092c3ce843729e76c77c4c51754";  // Nextlevel* @0x80
// BasicStat（08-06 remount · TypeDef 1322）
constexpr char kHashBsNmhp[] =
    "b4b1da11750d95d9e0c0b2ce26020d1680413de4f1a2ec188311fc51934321f";  // nMHP @0x30
constexpr char kHashBsNmmp[] =
    "addfcf03a46b13e7bc999eb8d31e0982ae3a0d386d561081b830493604b194e";  // nMMP @0x34
// Nextlevel.int[]（08-06 remount）
constexpr char kHashNextArr[] =
    "e5778e94ae74c3e60079d21a4f420325de98aef920dc2dd40bf9fc9583cb26b";  // @0x10

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
