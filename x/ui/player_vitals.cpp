// Shared player vitals — Classic TWMS.
// 真源唯一：WorldManager→CharacterData→CharacterStat（WM BasicStat 抬 mhp/mmp）。
// 字段防漂移：dump 哈希 → field_get_offset；失败回退下方 Hint（remount 2026-08-04）。
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

// --- dump.cs / restored TypeDef（2026-08-04）---
constexpr char kHashWorldManager[] =
    "af1529816d3e158e2939f3c03b4fe68c04930802ea39c8d6567d1fb4865b742";
constexpr char kHashCharacterData[] =
    "e410fb711868f2306f8a6368e8330e0e76e59c504fcdcce6952ceec81952e38";
constexpr char kHashCharacterStat[] =
    "f06c1e01b4fddd601707785f97dd0a798d9b6f32b0bdd35d7289bb77218a4ea";
constexpr char kHashBasicStat[] =
    "b6531d86b1298b77a11682f4d3023027071a1918ef37b7b07a5753ec2fcabcf";
constexpr char kHashNextLevel[] =
    "e0a3375a8abea880d8bdfeffd488e70c14d014382d45d154468bc20ea198777";

// WM
constexpr char kHashWmMyUser[] =
    "<d4428e1b7aab1a1fca5b6951009bf64f2c5cfb39f9a183f582fed4ff3a1aaaa>k__BackingField";
constexpr char kHashWmCharacterData[] =
    "b57ace627893d12b66c4fe3b71ba9ab74056636018d089be72d9c93bab9f5a1";
constexpr char kHashWmBasicStat[] =
    "ff367f3a2d08506773717fe61fd76de085291b7aa6c8f7fd9740cae9150dd5c";
// CharacterData
constexpr char kHashCdCharacterStat[] =
    "ef9ee721714c61ccb4781a5674e197ff0cbe50a034f7c8a804fb03eb71a8d6a";
constexpr char kHashCdItemSlots[] =
    "e756936a01b4cb1ab54a49b063b06337b0f0666ee05ead8415f35a8e278b760";  // List<ItemSlotBase>[]
constexpr char kHashCdSkillRecord[] =
    "ba3361082014233983ead400c9c134963c964ba8ad882f2284d0f3a296c19ff";  // Dict<int,int>
constexpr char kHashCdSkillRecordEx[] =
    "e3d90051458e6b5050457693db084fe9ec5f17cc97ebd23356a35c7d330f515";
constexpr char kHashCdSkillCooltime[] =
    "f8c076a149b98ee307b788e694cf7e9dcd3e1caf26b3242025bd7aea62cff6f";  // Dict<int,ushort>
constexpr char kHashCdSkillCoolTimeOver[] =
    "d6b5690505bd851816423d69e69d0369d89527b802327dc8d3742b61f39ef84";  // Dict<int,int>
constexpr char kHashWmSecondaryStat[] =
    "aae31a6398f01aee6aa0d82bfd982fd5185807939e4a1862b45cd7b3e3e052c";
// ItemSlotBase / ItemSlotBundle（明文类名 + 字段）
constexpr char kFldSlotItemId[] = "ItemId";
constexpr char kFldSlotBundleNumber[] = "nNumber";
// CharacterStat
constexpr char kHashCsName[] =
    "f216de52c6bb5f9ee7a0f4ebe4270cc3377ce61b1e4c7aa3c2e49d08c3da688";
constexpr char kHashCsLevel[] =
    "ac28038f1841b43960798b30d266cb33d510620b8c9cbbeaed67bb41897ff78";
constexpr char kHashCsJob[] =
    "da3698da6d23c8d4aba0e5b1be6f4f8e319f5ae61793ea35fdfcb20e0feb9c8";
constexpr char kHashCsHp[] =
    "a39e66c6936b3fedb87c6cb9c2ef0fe6b7280ed8cbfc627bff32fe9212d27c2";
constexpr char kHashCsMhp[] =
    "e4191a79ddb6c5ea57e6ae58950fe1774fcf5fda36422cf40137b487f1ba72d";
constexpr char kHashCsMp[] =
    "adaceeb57a830269c02c8c34e093eef5d09f07315480772b8196da28b241695";
constexpr char kHashCsMmp[] =
    "f63e4525fe1637d2049a2ff8c1592ac3d42e7ace28385f6668e7c36a3f6de6c";
constexpr char kHashCsExp[] =
    "a955322ec95c0a3a77b40b46c760dde3b2061a00aff4523c3c63a54da23e309";
constexpr char kHashCsMoney[] =
    "c4f0d2ae158f16ee0dbf5f780dc5df5d69b3b85ff4971b0496308559a426f51";
constexpr char kHashCsNextLevel[] =
    "a75e7e06d24f762d1ba80d631f58a7b090ddbfe1e76bdca517468b18d3b8262";
// BasicStat
constexpr char kHashBsNmhp[] =
    "f6a5cf63ddc0537d5d648dfb28bbe7d3e1f73c1989d37976e19375a0b9e80e6";
constexpr char kHashBsNmmp[] =
    "ad1d81abaa50537c6b8cd8db7aaf66ca0c426b35dce16580db5ec0b556cfdc1";
// NextLevel.int[]
constexpr char kHashNextArr[] =
    "b9a7dc9ca20a45d234fe8c8879b7c945d0d18d0a5693e41871c9b38f3edd911";

// Hint（dump 复核；hash 失败时回退）
constexpr size_t kFbWmMyUser = 0x28;
constexpr size_t kFbWmCharacterData = 0xE0;
constexpr size_t kFbWmBasicStat = 0xE8;
constexpr size_t kFbCdCharacterStat = 0x10;
constexpr size_t kFbCdItemSlots = 0x40;
constexpr size_t kFbCdSkillRecord = 0x50;
constexpr size_t kFbCdSkillRecordEx = 0x58;
constexpr size_t kFbCdSkillCooltime = 0x68;
constexpr size_t kFbCdSkillCoolTimeOver = 0x70;
constexpr size_t kFbWmSecondaryStat = 0xF0;
constexpr size_t kFbSlotItemId = 0x10;
constexpr size_t kFbSlotBundleNumber = 0x28;  // ItemSlotBundle.nNumber
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
size_t gOffCdSkillCooltime = kFbCdSkillCooltime;
size_t gOffCdSkillCoolTimeOver = kFbCdSkillCoolTimeOver;
size_t gOffWmSecondaryStat = kFbWmSecondaryStat;
size_t gOffSlotItemId = kFbSlotItemId;
size_t gOffSlotBundleNumber = kFbSlotBundleNumber;
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
        gOffCdSkillCooltime =
            PickOff(FieldOffsetByHash(cd, kHashCdSkillCooltime), kFbCdSkillCooltime, &cdH);
        gOffCdSkillCoolTimeOver = PickOff(FieldOffsetByHash(cd, kHashCdSkillCoolTimeOver),
                                          kFbCdSkillCoolTimeOver, &cdH);
    }
    void* slotBase = x::runtime::il2cpp::FindClass("", "ItemSlotBase");
    void* slotBundle = x::runtime::il2cpp::FindClass("", "ItemSlotBundle");
    bool slotH = false;
    if (slotBase) {
        gOffSlotItemId =
            PickOff(FieldOffsetByHash(slotBase, kFldSlotItemId), kFbSlotItemId, &slotH);
    }
    if (slotBundle) {
        gOffSlotBundleNumber = PickOff(FieldOffsetByHash(slotBundle, kFldSlotBundleNumber),
                                       kFbSlotBundleNumber, &slotH);
    }
    if (cs) {
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
