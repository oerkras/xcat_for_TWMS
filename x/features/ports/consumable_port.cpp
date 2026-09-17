// Consumable port — Classic TWMS inventory scan + UseRequest via shared main_thread_pump.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "consumable_port.h"

#include "input_port.h"
#include "travel_port.h"
#include "world_port.h"
#include "../../ui/player_vitals.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/mono_clock.h"
#include "../soft_login_probe/soft_login_probe.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::features::ports::consumable {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// 09-10 CharacterData.ItemSlots 直下标。DumpRestoredData 旧枚举 Consume=2；BIN 已钉成 1。
constexpr int kItemTypeConsume = 1;
static_assert(kItemTypeConsume == x::ui::player::item_type::Consume);
constexpr DWORD kJobWaitMs = 1500;
constexpr DWORD kUseMiRetryMs = 3000;
constexpr DWORD kFkmRebindMs = 3000;

// FuncKeyMappedManager · remounted 2026-08-06（与 attack_input_port 钉值一致）
constexpr uint32_t kRvaGetDataByKeyCode = 0x1709be0;
constexpr char kHashGetDataByKeyCode[] =
    "d43c993cf56de6520167d5e1d1992f0b12fc20db1bc4c782ed1f9908aee1851";
constexpr char kFkmClass[] =
    "e41c34f63e74f68108052715f3f4153bb488f7aba1aa6c23707a2536a7c4a8f";
constexpr int32_t kFuncTypeItem = 2;  // FuncType.Item
constexpr char kFuncKeyClass[] =
    "ff247e31dceda987dca78a8f62c2a3787063a7d45e50e426f06aa259d45257c";
constexpr char kHashFkType[] =
    "f8a3feb148e565a5e3482b8769274aacfba89c1e61f2ac6ddcdeed263829b1c";
constexpr char kHashFkValue[] =
    "ee25b5a7da35a2ce381d05c3f23e3205282346cdf9b29cc1e3441a20ed32e63";
constexpr size_t kFbFkType = 0x10;
constexpr size_t kFbFkValue = 0x14;
size_t gOffFkType = kFbFkType;
size_t gOffFkValue = kFbFkValue;
bool gFkFieldTried = false;

// --- 字段防漂移（remount 2026-08-06 · TDI/offset 对齐；WM/FKM/FuncKey 未漂）---
constexpr char kHashWorldManager[] =
    "cba21c42799e50c37623ea5fd88d5c4c9155c2565d25cde936671684e16dff7";
constexpr char kHashCharacterData[] =
    "b0b3ee7ea1f9e3dfe390adcc6c0f8b3ccc6703e20ecab8769f3dea0c661cda9";
constexpr char kHashItemSlotBase[] =
    "f70996d114bf2f4cf0c985796a65ec566369c1014a1edd476a8287f323d72b1";
constexpr char kHashItemSlotBundle[] =
    "c2bf5447cf3e555d11ff3e91b1258711232145693205380dda31081d3637750";
constexpr char kHashWmCharacterData[] =
    "c360c98c279935acfad02204ec9da405a5dc1173561ea0e1047e4dbc580b1b5";
constexpr char kHashCdItemSlots[] =
    "fdceb47c0740b59430fb305f0860087dac0c18a4d861d881f5afe666af2ee73";
constexpr char kHashItemId[] =
    "c81b6812a4e7d60bc3b07ef14041ea4c2399fc7b44abd8869a4e7979d751ae3";
constexpr char kHashBundleNumber[] =
    "cd85e37ec226d0aa2567cfee7f7c9eb77ffce5e379fbf388bd5c64e466c741a";

constexpr size_t kFbWmCharacterData = 0xE0;
constexpr size_t kFbCdItemSlots = 0x40;
constexpr size_t kFbItemId = 0x10;
constexpr size_t kFbBundleNumber = 0x30;  // 09-10：nNumber；0x28 是基类 nPOS
constexpr size_t kFbSlotPos = 0x28;       // ItemSlotBase nPOS

size_t gOffWmCharacterData = kFbWmCharacterData;
size_t gOffCdItemSlots = kFbCdItemSlots;
size_t gOffItemId = kFbItemId;
size_t gOffBundleNumber = kFbBundleNumber;
std::atomic<bool> gFieldOffResolved{false};
char gFieldOffPath[64]{};

// UISlotItem.SendStatChangeItemUseRequest — 药水等属性道具；hashed；TypeDefIndex 488。
// Remount 2026-08-06: ACS class/method rehashed；RVA 未漂（仍 0x6086d0）。
// Evidence: dump.cs static Send* 声明序对齐 CMS（Lottery → StatChange → AntiMacro → PortalScroll…）。
// Resolve: name → method-hash → RVA+kind(void,int,int)。
constexpr char kUiSlotItemClassHash[] =
    "ec0318c8aa58794890f4defe7d2c78f613c8f47baab5551217916eb148020c6";
constexpr char kUseReqMethodHash[] =
    "cfa225b04b2c24e311283466ba0ba571b7af5ec535b40535f04a894b34e1766";
constexpr uint32_t kRvaSendStatChangeItemUseRequest = 0x6086d0;

// UISlotItem.SendPortalScrollUseRequest — 回家/城镇卷（2030xxx）；CMS private static (nPOS,nItemID)。
// TW dump 同簇；RVA 未漂 0x60B2D0。
constexpr char kPortalScrollMethodHash[] =
    "be07cc2d630a0c4c2139463f296575466630af69db50782147c71ff76f73eb8";
constexpr uint32_t kRvaSendPortalScrollUseRequest = 0x60B2D0;

using FnUseRequest = void (*)(int nPos, int itemId, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

void* gKlassSlotItem = nullptr;
MethodInfoHead* gMiUseReq = nullptr;
FnUseRequest gFnUseReq = nullptr;
MethodInfoHead* gMiPortalScroll = nullptr;
FnUseRequest gFnPortalScroll = nullptr;

using FnGetDataByKeyCode = void* (*)(void* self, int32_t key, const void* methodInfo);

void* gFkm = nullptr;
void* gFkmKlass = nullptr;
MethodInfoHead* gMiGetDataByKeyCode = nullptr;
DWORD gLastFkmRebind = 0;
DWORD gLastBindMissLogHp = 0;
DWORD gLastBindMissLogMp = 0;

// CharacterData.GetItem(nTI, nPOS) — 与 final_attack_force 同源（RVA 0x135BC50）。
// GetItem / 直读 _items 是同一块 List；BIN 20:03 type=2 listN=25 filled=0（25 格像 Install）。
constexpr uint32_t kRvaCdGetItem = 0x135BC50;
constexpr char kHashCdGetItem[] =
    "e423704b730739210652e47452bc607108dda102192de83ed6855a7701d6d38";
constexpr int kConsumePosMax = 96;
using FnCdGetItem = void* (*)(void* self, int nTI, int nPos, const void* methodInfo);
FnCdGetItem gCdGetItem = nullptr;
MethodInfoHead* gMiCdGetItem = nullptr;

// DumpRestoredData A/B：GetItemSlotPos(ItemType, itemId)→nPOS（旧 RVA 0x12F32D0）。
// 09-10 dump 同签名 int(ItemType,int) @ 0x1363190。IDA：ItemSlots[nTI] 直下标 + List.get_Item。
constexpr uint32_t kRvaCdGetItemSlotPos = 0x1363190;
constexpr char kHashCdGetItemSlotPos[] =
    "eeafbe4940c08f96459626d541706fb81f8da55caf1de977f8aeb84362632c1";
using FnCdGetItemSlotPos = int (*)(void* self, int nTI, int itemId, const void* methodInfo);
FnCdGetItemSlotPos gCdGetItemSlotPos = nullptr;
MethodInfoHead* gMiCdGetItemSlotPos = nullptr;

// DumpRestoredData：GetItemCount(ItemType, itemId, bool compress=true)。09-10 @ 0x135B530。
constexpr uint32_t kRvaCdGetItemCount = 0x135B530;
constexpr char kHashCdGetItemCount[] =
    "ab2d4cc9bc0de58aa5a4455e36fd166e7a7f686f1a529abccfb1cbfa0d0ca03";
using FnCdGetItemCount = int (*)(void* self, int nTI, int itemId, uint8_t compress,
                                 const void* methodInfo);
FnCdGetItemCount gCdGetItemCount = nullptr;
MethodInfoHead* gMiCdGetItemCount = nullptr;

void ReturnLeakedMetadataLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
}

// UseRequest nPOS vs List index.
// BIN 2026-08-09: bound MP id=2000003 succeeds at pos=listIndex (7), fails when oneBased
// heuristic flips and primary becomes listIndex+1 (8) — wrong POS burns CD; alt=7 then empty.
// Prefer listIndex when index>=1; latch ListIndexIsPos only on pos==listIndex qty-drop
// (never latch PlusOne from alt — mis-attribution permanently flips primary wrong).
enum class ConsumePosMode : int { Unknown = 0, ListIndexIsPos = 1, ListIndexPlusOne = 2 };
std::atomic<int> gConsumePosMode{static_cast<int>(ConsumePosMode::Unknown)};

// Only latch ListIndexIsPos (TWMS BIN default). Never latch PlusOne from alt "success":
// delayed qty drop after primary may be mis-attributed to alt and permanently flip primary wrong.
void NoteConsumePosSuccess(int listIndex, int pos) {
    if (listIndex < 0 || pos <= 0) return;
    if (pos == listIndex) {
        gConsumePosMode.store(static_cast<int>(ConsumePosMode::ListIndexIsPos),
                              std::memory_order_relaxed);
    }
}

// primary/alt for UseRequest. outAlt may be -1 when no alternate.
void PickConsumePos(int listIndex, int* outPrimary, int* outAlt) {
    if (!outPrimary || !outAlt) return;
    *outPrimary = -1;
    *outAlt = -1;
    if (listIndex < 0) return;
    const int asIndex = listIndex;
    const int asPlus1 = listIndex + 1;
    // Always prefer listIndex when ≥1 (BIN). PlusOne mode kept for rare explicit latch only —
    // currently never written; branch retained for safe rollback if a real +1 layout appears.
    const int mode = gConsumePosMode.load(std::memory_order_relaxed);
    if (mode == static_cast<int>(ConsumePosMode::ListIndexPlusOne)) {
        *outPrimary = asPlus1;
        *outAlt = (listIndex >= 1) ? asIndex : -1;
        return;
    }
    if (listIndex >= 1) {
        *outPrimary = asIndex;
        *outAlt = asPlus1;
        return;
    }
    // Item at index 0: only +1 is a valid maple POS.
    *outPrimary = asPlus1;
    *outAlt = -1;
}

struct UseJobCtx {
    int pos = 0;
    int itemId = 0;  // 2nd arg: itemId (NOT pPet) — see FuncKey.Value / UISlot call sites
    bool ok = false;
};

// Rank: lower = prefer. -1 = not this kind.
int HpRank(int id) {
    switch (id) {
    // Dedicated HP
    case 2000000:  // 红
    case 2000001:  // 橙
    case 2000002:  // 白
    case 2000007:
    case 2000008:
    case 2000009:
    case 2000013:  // 新手红
    case 2000015:
    case 2000016:
    case 2000020:  // 贵族红
    case 2000022:  // 瑞恩红
        return 0;
    // Dual / super
    case 2000004:  // 特殊
    case 2000005:  // 超级
    case 2000012:
    case 2000019:
    case 2000031:  // 特殊（约50%）
        return 1;
    default:
        return -1;  // 不扫食物/杂项，避免乱用药
    }
}

int MpRank(int id) {
    switch (id) {
    case 2000003:
    case 2000006:
    case 2000010:
    case 2000011:
    case 2000014:
    case 2000017:
    case 2000018:
    case 2000021:
    case 2000023:
    case 2000038:
    case 2000039:
    case 2000045:
    case 2000046:
    case 2000051:
    case 2000052:
        return 0;
    case 2000004:
    case 2000005:
    case 2000012:
    case 2000019:
    case 2000031:
        return 1;
    default:
        break;
    }
    if (id >= 2001000 && id < 2002000) return 0;
    return -1;
}

// --- FKM PageDown/PageUp bind (align attack_input_port remount 2026-08-04) ---
void* TryLazyValue(void* lazy) {
    if (!lazy || !LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classStaticData) {
        __try {
            void* p = e.classStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

void* TryResolveFkmSingleton() {
    if (!gFkmKlass) gFkmKlass = x::runtime::il2cpp::FindClass("", kFkmClass);
    if (!gFkmKlass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.runtimeClassInit) {
        __try {
            x::runtime::il2cpp::RuntimeClassInit(gFkmKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void* staticsKlass = gFkmKlass;
    if (e.classParent) {
        void* parent = nullptr;
        __try {
            parent = e.classParent(gFkmKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (parent) {
            if (e.runtimeClassInit) {
                __try {
                    x::runtime::il2cpp::RuntimeClassInit(parent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            staticsKlass = parent;
        }
    }
    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gFkmKlass);
    if (!statics) return nullptr;
    for (size_t s = 0; s < 6; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeHeapPtr(cand)) continue;
        if (ReadPtr(cand, 0) == gFkmKlass) return cand;
        if (!gFkm) return cand;
    }
    return nullptr;
}

bool EnsureFkmOnMain() {
    const DWORD now = x::runtime::NowMs();
    if (gFkm && LooksLikeHeapPtr(gFkm) && ReadPtr(gFkm, 0) && now - gLastFkmRebind < kFkmRebindMs)
        return true;
    gLastFkmRebind = now;
    gFkm = TryResolveFkmSingleton();
    return gFkm != nullptr;
}

void EnsureGetDataByKeyCodeMi() {
    if (gMiGetDataByKeyCode) return;
    if (!gFkmKlass) gFkmKlass = x::runtime::il2cpp::FindClass("", kFkmClass);
    if (!gFkmKlass) return;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kData{1, TypeKind::Ptr, true, true, {TypeKind::Any}};
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        gFkmKlass, kRvaGetDataByKeyCode, kData, "GetDataByKeyCode", kHashGetDataByKeyCode);
    if (mr.method) gMiGetDataByKeyCode = reinterpret_cast<MethodInfoHead*>(mr.method);
}

bool ReadFkFields(void* fk, int32_t* outType, int32_t* outValue) {
    if (!fk || !LooksLikeHeapPtr(fk) || !outType || !outValue) return false;
    int32_t t = 0, v = 0;
    __try {
        t = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(fk) + gOffFkType);
        v = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(fk) + gOffFkValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *outType = t;
    *outValue = v;
    return true;
}

// 绑药路径：用户绑什么就喝什么（仅校验 itemId>0）。扫栏 FindPotion 仍走 HpRank/MpRank。
bool AcceptBoundItemId(int itemId) { return itemId > 0; }

bool FkFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t lo, size_t hi) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, hash);
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
            if (off >= lo && off < hi) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}

void EnsureFkFieldOff() {
    if (gFkFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gFkFieldTried = true;
    void* fk = x::runtime::il2cpp::FindClass("", kFuncKeyClass);
    int hits = 0;
    if (FkFieldOffHit(fk, kHashFkType, kFbFkType, &gOffFkType, 0x10, 0x40)) ++hits;
    if (FkFieldOffHit(fk, kHashFkValue, kFbFkValue, &gOffFkValue, 0x10, 0x40)) ++hits;
    x::runtime::LogI("Consumable", "FuncKey slots path=%s hits=%d/2 fkT=0x%zX fkV=0x%zX",
                     hits == 2 ? "meta" : (hits ? "meta-partial" : "fallback"), hits, gOffFkType,
                     gOffFkValue);
}

void LogConsumeBagSnap(int wantId);

void LogBindMissThrottled(const char* why, bool wantHp, int type, int value) {
    const DWORD now = x::runtime::NowMs();
    DWORD& slot = wantHp ? gLastBindMissLogHp : gLastBindMissLogMp;
    if (slot && static_cast<int>(now - slot) < 30000) return;
    slot = now;
    x::runtime::LogW("Consumable", "bound pot miss key=%s why=%s type=%d value=%d",
                     wantHp ? "PageDown" : "PageUp", why, type, value);
    if (why && std::strcmp(why, "not_in_bag") == 0) LogConsumeBagSnap(value);
}

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
    void* slot = x::runtime::il2cpp::FindClass("", kHashItemSlotBase);
    void* bundle = x::runtime::il2cpp::FindClass("", kHashItemSlotBundle);
    if (!wm && !cd && !slot && !bundle) return;

    bool wmH = false, cdH = false, idH = false, qtyH = false;
    if (wm) {
        gOffWmCharacterData =
            PickOff(FieldOffsetByHash(wm, kHashWmCharacterData), kFbWmCharacterData, &wmH);
    }
    if (cd) {
        gOffCdItemSlots =
            PickOff(FieldOffsetByHash(cd, kHashCdItemSlots), kFbCdItemSlots, &cdH);
    }
    if (slot) {
        gOffItemId = PickOff(FieldOffsetByHash(slot, kHashItemId), kFbItemId, &idH);
    }
    // nNumber 在 Bundle 上；找不到则沿父类 ItemSlotBase 再试一次
    if (bundle || slot) {
        size_t q = 0;
        if (bundle) q = FieldOffsetByHash(bundle, kHashBundleNumber);
        if (!q && slot) q = FieldOffsetByHash(slot, kHashBundleNumber);
        gOffBundleNumber = PickOff(q, kFbBundleNumber, &qtyH);
    }

    snprintf(gFieldOffPath, sizeof(gFieldOffPath), "wm=%s cd=%s id=%s qty=%s",
             wmH ? "hash" : "hint", cdH ? "hash" : "hint", idH ? "hash" : "hint",
             qtyH ? "hash" : "hint");
    gFieldOffResolved.store(true, std::memory_order_release);
    x::runtime::LogI("Consumable", "field off cd=0x%zX slots=0x%zX id=0x%zX qty=0x%zX path=%s",
                     gOffWmCharacterData, gOffCdItemSlots, gOffItemId, gOffBundleNumber,
                     gFieldOffPath);
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

int16_t ReadI16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int ListSize(void* list) {
    if (!list) return 0;
    return ReadI32(list, x::runtime::il2cpp_container::OffListSize());
}

void* ListAt(void* list, int i) {
    if (!list || i < 0) return nullptr;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    if (!items) return nullptr;
    return ArrayAt(items, (uintptr_t)i);
}

int ItemQty(void* item) {
    if (!item) return 0;
    const int n = (int)ReadU16(item, gOffBundleNumber);
    if (n > 0) return n;
    return 1;
}

int SlotNPos(void* item) {
    if (!item) return 0;
    const int p = static_cast<int>(ReadI16(item, kFbSlotPos));
    if (p >= 1 && p <= 96) return p;
    return 0;
}

int PickUsePos(void* item, int listIndex) {
    // GetItem(Consume, nPOS) = list._items[nPOS]（IDA RVA 0x1359CE0：edi 直下标，不减 1）。
    // 官方 FuncKey / UseRequest 传格子 nPOS，不是 List 压缩下标。低格时两者常相等，
    // 所以旧版绑 2000000@pos=1~8 能喝；高格（远程 2001500@listIndex=64）会打空。
    const int fromSlot = SlotNPos(item);
    if (fromSlot > 0) return fromSlot;
    int primary = -1, alt = -1;
    PickConsumePos(listIndex, &primary, &alt);
    (void)alt;
    return primary;
}

int ConsumeType() { return kItemTypeConsume; }

void* GetConsumeList() {
    EnsureFieldOffsets();
    return x::ui::player::GetItemSlotList(ConsumeType());
}

void EnsureCdGetItem() {
    if (gCdGetItem) return;
    void* cdKlass = x::runtime::il2cpp::FindClass("", kHashCharacterData);
    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 2;
    shape.ret = x::runtime::il2cpp_method::TypeKind::Ptr;
    shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[1] = x::runtime::il2cpp_method::TypeKind::I32;
    auto mr = x::runtime::il2cpp_method::FindMethodResolved(cdKlass, kRvaCdGetItem, shape, "GetItem",
                                                            kHashCdGetItem);
    if (mr.method) {
        gMiCdGetItem = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (gMiCdGetItem && gMiCdGetItem->methodPointer)
            gCdGetItem = reinterpret_cast<FnCdGetItem>(gMiCdGetItem->methodPointer);
    }
    if (!gCdGetItem) gCdGetItem = x::runtime::il2cpp::AtRva<FnCdGetItem>(kRvaCdGetItem);
    static bool sLogged = false;
    if (!sLogged && gCdGetItem) {
        sLogged = true;
        x::runtime::LogI("Consumable", "GetItem MI=%p fn=%p", (void*)gMiCdGetItem, (void*)gCdGetItem);
    }
}

void EnsureCdGetItemSlotPos() {
    if (gCdGetItemSlotPos) return;
    void* cdKlass = x::runtime::il2cpp::FindClass("", kHashCharacterData);
    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 2;
    shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[1] = x::runtime::il2cpp_method::TypeKind::I32;
    auto mr = x::runtime::il2cpp_method::FindMethodResolved(cdKlass, kRvaCdGetItemSlotPos, shape,
                                                            "GetItemSlotPos", kHashCdGetItemSlotPos);
    if (mr.method) {
        gMiCdGetItemSlotPos = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (gMiCdGetItemSlotPos && gMiCdGetItemSlotPos->methodPointer)
            gCdGetItemSlotPos =
                reinterpret_cast<FnCdGetItemSlotPos>(gMiCdGetItemSlotPos->methodPointer);
    }
    if (!gCdGetItemSlotPos)
        gCdGetItemSlotPos = x::runtime::il2cpp::AtRva<FnCdGetItemSlotPos>(kRvaCdGetItemSlotPos);
    static bool sLogged = false;
    if (!sLogged && gCdGetItemSlotPos) {
        sLogged = true;
        x::runtime::LogI("Consumable", "GetItemSlotPos MI=%p fn=%p", (void*)gMiCdGetItemSlotPos,
                         (void*)gCdGetItemSlotPos);
    }
}

void EnsureCdGetItemCount() {
    if (gCdGetItemCount) return;
    void* cdKlass = x::runtime::il2cpp::FindClass("", kHashCharacterData);
    x::runtime::il2cpp_method::MethodShape shape{};
    shape.arity = 3;
    shape.ret = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[0] = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[1] = x::runtime::il2cpp_method::TypeKind::I32;
    shape.param[2] = x::runtime::il2cpp_method::TypeKind::Bool;
    auto mr = x::runtime::il2cpp_method::FindMethodResolved(cdKlass, kRvaCdGetItemCount, shape,
                                                            "GetItemCount", kHashCdGetItemCount);
    if (mr.method) {
        gMiCdGetItemCount = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (gMiCdGetItemCount && gMiCdGetItemCount->methodPointer)
            gCdGetItemCount = reinterpret_cast<FnCdGetItemCount>(gMiCdGetItemCount->methodPointer);
    }
    if (!gCdGetItemCount)
        gCdGetItemCount = x::runtime::il2cpp::AtRva<FnCdGetItemCount>(kRvaCdGetItemCount);
    static bool sLogged = false;
    if (!sLogged && gCdGetItemCount) {
        sLogged = true;
        x::runtime::LogI("Consumable", "GetItemCount MI=%p fn=%p", (void*)gMiCdGetItemCount,
                         (void*)gCdGetItemCount);
    }
}

void* GetItemAtTypePos(int nTI, int nPos) {
    if (nTI < 0 || nTI > 6) return nullptr;
    if (nPos < 1 || nPos > kConsumePosMax) return nullptr;
    if (!x::runtime::main_thread::IsOnPumpThread()) return nullptr;
    void* cd = x::ui::player::LocalCharacterData();
    if (!cd) return nullptr;
    EnsureCdGetItem();
    if (!gCdGetItem) return nullptr;
    void* slot = nullptr;
    __try {
        slot = gCdGetItem(cd, nTI, nPos, gMiCdGetItem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("consumable/GetItem");
        slot = nullptr;
    }
    return slot;
}

void* GetItemConsumePos(int nPos) { return GetItemAtTypePos(ConsumeType(), nPos); }

int CallGetItemSlotPos(int nTI, int itemId) {
    if (nTI < 0 || nTI > 6 || itemId <= 0) return 0;
    if (!x::runtime::main_thread::IsOnPumpThread()) return 0;
    void* cd = x::ui::player::LocalCharacterData();
    if (!cd) return 0;
    EnsureCdGetItemSlotPos();
    if (!gCdGetItemSlotPos) return 0;
    int pos = 0;
    __try {
        pos = gCdGetItemSlotPos(cd, nTI, itemId, gMiCdGetItemSlotPos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("consumable/GetItemSlotPos");
        pos = 0;
    }
    return pos;
}

int CallGetItemCount(int nTI, int itemId) {
    if (nTI < 0 || nTI > 6 || itemId <= 0) return 0;
    if (!x::runtime::main_thread::IsOnPumpThread()) return 0;
    void* cd = x::ui::player::LocalCharacterData();
    if (!cd) return 0;
    EnsureCdGetItemCount();
    if (!gCdGetItemCount) return 0;
    int n = 0;
    __try {
        n = gCdGetItemCount(cd, nTI, itemId, 1, gMiCdGetItemCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("consumable/GetItemCount");
        n = 0;
    }
    return n;
}

void CensusOneType(int nTI, int* outSize, int* outFilled, int* outSampleId) {
    if (outSize) *outSize = -1;
    if (outFilled) *outFilled = 0;
    if (outSampleId) *outSampleId = 0;
    void* list = x::ui::player::GetItemSlotList(nTI);
    if (!list) return;
    const int size = ListSize(list);
    if (outSize) *outSize = size;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    const int cap = items ? static_cast<int>(ArrayLen(items)) : 0;
    const int n = cap > size ? cap : size;
    int filled = 0;
    int sample = 0;
    for (int i = 0; i < n && i < 128; ++i) {
        void* slot = items ? ArrayAt(items, static_cast<uintptr_t>(i)) : nullptr;
        if (!slot) continue;
        const int id = ReadI32(slot, gOffItemId);
        if (id <= 0) continue;
        ++filled;
        if (!sample) sample = id;
    }
    if (outFilled) *outFilled = filled;
    if (outSampleId) *outSampleId = sample;
}

// GetItem(Consume, nPOS) 走 _items[nPOS]（数组 max_length，不是 List._size）。
// 只扫 _size 会漏空格后面的格子。ItemSlots[type] 若本身是 T[]，_items 不是堆指针，按数组直扫。
bool ConsumeSlotArray(void** outArr, int* outN) {
    if (outArr) *outArr = nullptr;
    if (outN) *outN = 0;
    void* list = GetConsumeList();
    if (!list) return false;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    void* arr = nullptr;
    int n = 0;
    if (LooksLikeHeapPtr(items)) {
        arr = items;
        const int cap = static_cast<int>(ArrayLen(items));
        const int size = ListSize(list);
        n = cap > size ? cap : size;
        if (n <= 0) n = cap;
    } else {
        arr = list;
        n = static_cast<int>(ArrayLen(list));
    }
    if (!arr || n <= 0 || n > 256) return false;
    if (outArr) *outArr = arr;
    if (outN) *outN = n;
    return true;
}

// 优先官方 GetItem(nPOS)。只扫消耗栏 ItemType=1；2 是 Install，空扫也不改栏。
void ForEachUseBagSlot(void (*fn)(void* slot, int listIndex, void* user), void* user) {
    if (!fn) return;
    EnsureFieldOffsets();
    const int nTI = kItemTypeConsume;
    auto scanType = [&](int type) -> int {
        int hits = 0;
        for (int pos = 1; pos <= kConsumePosMax; ++pos) {
            void* item = GetItemAtTypePos(type, pos);
            if (!item) continue;
            const int id = ReadI32(item, gOffItemId);
            if (id <= 0) continue;
            ++hits;
            fn(item, pos, user);
        }
        return hits;
    };
    if (x::runtime::main_thread::IsOnPumpThread()) {
        if (scanType(nTI) > 0) return;
    }
    auto scanArr = [&](int type) {
        void* list = x::ui::player::GetItemSlotList(type);
        if (!list) return 0;
        void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
        if (!items) return 0;
        const int cap = static_cast<int>(ArrayLen(items));
        const int size = ListSize(list);
        const int n = cap > size ? cap : size;
        int hits = 0;
        for (int i = 0; i < n && i < 256; ++i) {
            void* item = ArrayAt(items, static_cast<uintptr_t>(i));
            if (!item) continue;
            const int id = ReadI32(item, gOffItemId);
            if (id <= 0) continue;
            ++hits;
            fn(item, i, user);
        }
        return hits;
    };
    (void)scanArr(nTI);
}

void LogConsumeBagSnap(int wantId) {
    struct Acc {
        int ids[12]{};
        int qtys[12]{};
        int nfill = 0;
        int nslot = 0;
    } acc{};
    void* list = GetConsumeList();
    const int listN = list ? ListSize(list) : -1;
    int arrN = 0;
    void* arr = nullptr;
    const bool haveArr = ConsumeSlotArray(&arr, &arrN);
    ForEachUseBagSlot(
        [](void* item, int, void* user) {
            auto* a = static_cast<Acc*>(user);
            ++a->nslot;
            if (a->nfill >= 12) return;
            a->ids[a->nfill] = ReadI32(item, gOffItemId);
            a->qtys[a->nfill] = ItemQty(item);
            ++a->nfill;
        },
        &acc);
    char buf[256];
    buf[0] = 0;
    int o = 0;
    for (int i = 0; i < acc.nfill && o < (int)sizeof(buf) - 24; ++i) {
        o += snprintf(buf + o, sizeof(buf) - static_cast<size_t>(o), "%s%d:%d", i ? "," : "",
                      acc.ids[i], acc.qtys[i]);
    }
    x::runtime::LogW("Consumable",
                     "bag snap want=%d type=%d listN=%d arrN=%d filled=%d ids=%s", wantId,
                     ConsumeType(), listN, haveArr ? arrN : -1, acc.nslot, acc.nfill ? buf : "-");
    char census[320];
    census[0] = 0;
    int co = 0;
    EnsureFieldOffsets();
    for (int t = 0; t <= 5 && co < (int)sizeof(census) - 48; ++t) {
        int sz = -1, filled = 0, sample = 0;
        CensusOneType(t, &sz, &filled, &sample);
        co += snprintf(census + co, sizeof(census) - static_cast<size_t>(co), "%s%d:n=%d,f=%d,id=%d",
                       t ? " " : "", t, sz, filled, sample);
    }
    const int pos1 = CallGetItemSlotPos(1, wantId);
    const int pos2 = CallGetItemSlotPos(2, wantId);
    const int cnt1 = CallGetItemCount(1, wantId);
    const int cnt2 = CallGetItemCount(2, wantId);
    x::runtime::LogW("Consumable", "bag census {%s} slotPos t1=%d t2=%d count t1=%d t2=%d", census,
                     pos1, pos2, cnt1, cnt2);
}

int QtyOfItemId(int itemId) {
    if (itemId <= 0) return -1;
    struct QtyAcc {
        int itemId = 0;
        int total = 0;
        bool found = false;
    } acc{itemId, 0, false};
    ForEachUseBagSlot(
        [](void* item, int, void* user) {
            auto* a = static_cast<QtyAcc*>(user);
            if (ReadI32(item, gOffItemId) != a->itemId) return;
            a->found = true;
            a->total += ItemQty(item);
        },
        &acc);
    return acc.found ? acc.total : -1;
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !rva) return nullptr;
    const auto& ex = x::runtime::il2cpp::Get();
    if (!ex.classGetMethods || !x::runtime::il2cpp::GaBase()) return nullptr;
    void* target = x::runtime::il2cpp::AtRva<void*>(rva);
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* miRaw = ex.classGetMethods(cur, &iter);
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
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!ex.classParent) break;
        void* parent = nullptr;
        __try {
            parent = ex.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
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
        const int tryArgc[] = {argc, -1};
        for (int ac : tryArgc) {
            __try {
                mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, ac));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mi = nullptr;
            }
            if (mi && mi->methodPointer) return mi;
        }
    }
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
                          const char* plainName, const char* hashName,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plainName, hashName);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

bool gLoggedUseReqRvaMiss = false;
bool gLoggedPortalRvaMiss = false;
DWORD gLastUseMiRetryMs = 0;
DWORD gLastPortalMiRetryMs = 0;

bool ResolveUseMethod() {
    if (gMiUseReq && gMiUseReq->methodPointer) {
        gFnUseReq = reinterpret_cast<FnUseRequest>(gMiUseReq->methodPointer);
        return true;
    }
    if (gFnUseReq && gMiUseReq) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;

    // 已有 RVA-only：仍允许升级到 MI，但节流，避免每次吃药全量扫方法表。
    const bool hadRvaOnly = (gFnUseReq != nullptr);
    if (hadRvaOnly) {
        const DWORD now = GetTickCount();
        if (gLastUseMiRetryMs && now - gLastUseMiRetryMs < kUseMiRetryMs) return true;
        gLastUseMiRetryMs = now;
    }

    if (!gKlassSlotItem) {
        // 混淆盘无 UISlotItem 明文；哈希优先。
        gKlassSlotItem = x::runtime::il2cpp::FindClass("", kUiSlotItemClassHash);
        if (!gKlassSlotItem)
            gKlassSlotItem = x::runtime::il2cpp::FindClass("", "UISlotItem");
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    // UISlotItem 上 void(int,int) 同形很多 → unique=false，靠 RVA/哈希钉死。
    constexpr MethodShape kUse{2, TypeKind::Void, false, false, {TypeKind::I32, TypeKind::I32}};
    MethodInfoHead* mi = nullptr;
    const char* via = nullptr;
    if (gKlassSlotItem) {
        using x::runtime::il2cpp_method::ResolvePath;
        ResolvePath pUse = ResolvePath::Miss;
        mi = ResolveMi(gKlassSlotItem, kRvaSendStatChangeItemUseRequest, kUse,
                       "SendStatChangeItemUseRequest", kUseReqMethodHash, &pUse);
        static bool sMethodHitsLogged = false;
        if (!sMethodHitsLogged) {
            sMethodHitsLogged = true;
            x::runtime::LogI("Consumable", "methods path=%s hits=%d/1",
                             pUse == ResolvePath::Hash ? "meta"
                             : (pUse != ResolvePath::Miss ? "meta-partial" : "fallback"),
                             pUse == ResolvePath::Hash ? 1 : 0);
        }
        if (mi) via = "ResolveMi";
    }

    if (mi && mi->methodPointer) {
        gMiUseReq = mi;
        gFnUseReq = reinterpret_cast<FnUseRequest>(mi->methodPointer);
        x::runtime::LogI("Consumable", "UseRequest MI=%p fn=%p via %s%s", (void*)mi,
                         mi->methodPointer, via ? via : "?",
                         hadRvaOnly ? " (upgraded from RVA)" : "");
        x::runtime::anchor_lamps::Set("Consumable", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                     via ? via : "MI");
        return true;
    }

    // Last resort: 官方 FuncKey 站点传 null MI；裸 RVA 仅作换版过渡。
    // 已有 RVA fn 时只静默重试 MI 升级，禁止每次吃药刷 MI miss。
    if (!gFnUseReq) {
        gFnUseReq = x::runtime::il2cpp::AtRva<FnUseRequest>(kRvaSendStatChangeItemUseRequest);
    }
    if (gFnUseReq) {
        if (!gLoggedUseReqRvaMiss) {
            x::runtime::LogW("Consumable",
                             "UseRequest MI miss — RVA 0x%X null-MI fallback (klass=%p)",
                             kRvaSendStatChangeItemUseRequest, gKlassSlotItem);
            gLoggedUseReqRvaMiss = true;
        }
        x::runtime::anchor_lamps::Set("Consumable",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded, "RVA nullMI");
        return true;
    }
    x::runtime::LogW("Consumable", "SendStatChangeItemUseRequest resolve fail");
    x::runtime::anchor_lamps::Set("Consumable", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                 "MISS");
    return false;
}

bool EnsureSlotItemKlass() {
    if (gKlassSlotItem) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    gKlassSlotItem = x::runtime::il2cpp::FindClass("", kUiSlotItemClassHash);
    if (!gKlassSlotItem) gKlassSlotItem = x::runtime::il2cpp::FindClass("", "UISlotItem");
    return gKlassSlotItem != nullptr;
}

bool ResolvePortalScrollMethod() {
    if (gMiPortalScroll && gMiPortalScroll->methodPointer) {
        gFnPortalScroll = reinterpret_cast<FnUseRequest>(gMiPortalScroll->methodPointer);
        return true;
    }
    if (gFnPortalScroll && gMiPortalScroll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;

    const bool hadRvaOnly = (gFnPortalScroll != nullptr);
    if (hadRvaOnly) {
        const DWORD now = GetTickCount();
        if (gLastPortalMiRetryMs && now - gLastPortalMiRetryMs < kUseMiRetryMs) return true;
        gLastPortalMiRetryMs = now;
    }

    if (!EnsureSlotItemKlass()) return false;

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kUse{2, TypeKind::Void, false, false, {TypeKind::I32, TypeKind::I32}};
    ResolvePath pPath = ResolvePath::Miss;
    MethodInfoHead* mi =
        ResolveMi(gKlassSlotItem, kRvaSendPortalScrollUseRequest, kUse, "SendPortalScrollUseRequest",
                  kPortalScrollMethodHash, &pPath);
    static bool sPortalHitsLogged = false;
    if (!sPortalHitsLogged) {
        sPortalHitsLogged = true;
        x::runtime::LogI("Consumable", "PortalScroll methods path=%s hits=%d/1",
                         pPath == ResolvePath::Hash ? "meta"
                         : (pPath != ResolvePath::Miss ? "meta-partial" : "fallback"),
                         pPath == ResolvePath::Hash ? 1 : 0);
    }
    if (mi && mi->methodPointer) {
        gMiPortalScroll = mi;
        gFnPortalScroll = reinterpret_cast<FnUseRequest>(mi->methodPointer);
        x::runtime::LogI("Consumable", "PortalScroll MI=%p fn=%p via ResolveMi%s", (void*)mi,
                         mi->methodPointer, hadRvaOnly ? " (upgraded from RVA)" : "");
        return true;
    }
    if (!gFnPortalScroll) {
        gFnPortalScroll = x::runtime::il2cpp::AtRva<FnUseRequest>(kRvaSendPortalScrollUseRequest);
    }
    if (gFnPortalScroll) {
        if (!gLoggedPortalRvaMiss) {
            x::runtime::LogW("Consumable",
                             "PortalScroll MI miss — RVA 0x%X null-MI fallback (klass=%p)",
                             kRvaSendPortalScrollUseRequest, gKlassSlotItem);
            gLoggedPortalRvaMiss = true;
        }
        return true;
    }
    x::runtime::LogW("Consumable", "SendPortalScrollUseRequest resolve fail");
    return false;
}

void UseJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<UseJobCtx*>(user);
    if (!ctx || ctx->pos <= 0 || ctx->itemId <= 0) return;
    bool ok = false;
    __try {
        if (!ResolveUseMethod() || !gFnUseReq) {
            ctx->ok = false;
            return;
        }
        // Official FuncKey site: (nPOS, itemId, /*MethodInfo=*/null) — xor r8,r8.
        // CFF shell; non-null MI risk (same class of failure as KeyTouch).
        gFnUseReq(ctx->pos, ctx->itemId, nullptr);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        x::runtime::LogW("Consumable", "UseRequest SEH pos=%d id=%d", ctx->pos, ctx->itemId);
    }
    ctx->ok = ok;
}

bool InvokeUse(int pos, int itemId) {
    UseJobCtx ctx{};
    ctx.pos = pos;
    ctx.itemId = itemId;
    if (!x::runtime::main_thread::InvokeAndWait(&UseJobOnMain, &ctx, kJobWaitMs)) {
        x::runtime::LogW("Consumable", "UseRequest pump fail/timeout pos=%d id=%d", pos, itemId);
        return false;
    }
    return ctx.ok;
}

// MUST only run on Unity main (pump job). Never call from autopot worker.
bool FindPotionOnMain(PotionKind kind, FindResult& out) {
    out = {};
    if (!world::EnsureBound()) return false;
    struct Best {
        PotionKind kind = PotionKind::Hp;
        int bestPos = -1;
        int bestIdx = -1;
        int bestId = 0;
        int bestQty = 0;
        int bestRank = 99;
        bool bestFromSlot = false;
    } best{};
    best.kind = kind;
    ForEachUseBagSlot(
        [](void* item, int i, void* user) {
            auto* b = static_cast<Best*>(user);
            const int id = ReadI32(item, gOffItemId);
            const int rank = (b->kind == PotionKind::Hp) ? HpRank(id) : MpRank(id);
            if (rank < 0) return;
            const int qty = ItemQty(item);
            if (qty <= 0) return;
            const int fromSlot = SlotNPos(item);
            const int pos = fromSlot > 0 ? fromSlot : PickUsePos(item, i);
            if (pos <= 0) return;
            if (b->bestPos < 0 || rank < b->bestRank ||
                (rank == b->bestRank &&
                 (pos < b->bestPos || (pos == b->bestPos && qty > b->bestQty)))) {
                b->bestPos = pos;
                b->bestIdx = i;
                b->bestId = id;
                b->bestQty = qty;
                b->bestRank = rank;
                b->bestFromSlot = fromSlot > 0;
            }
        },
        &best);
    if (best.bestPos < 0) return false;
    out.pos = best.bestPos;
    out.listIndex = best.bestIdx;
    out.itemId = best.bestId;
    out.qty = best.bestQty;
    out.posFromSlot = best.bestFromSlot;
    out.ok = true;
    return true;
}

struct FindJobCtx {
    PotionKind kind = PotionKind::Hp;
    FindResult* out = nullptr;
    bool ok = false;
};

void FindJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<FindJobCtx*>(user);
    if (!ctx || !ctx->out) return;
    ctx->ok = FindPotionOnMain(ctx->kind, *ctx->out);
}

struct FindUseJobCtx {
    PotionKind kind = PotionKind::Hp;
    FindResult fr{};
    int qtyBefore = -1;
    int qtyAfter = -1;
    bool found = false;
    bool used = false;
};

void FindUseJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<FindUseJobCtx*>(user);
    if (!ctx) return;
    if (!FindPotionOnMain(ctx->kind, ctx->fr) || !ctx->fr.ok) return;
    ctx->found = true;
    ctx->qtyBefore = QtyOfItemId(ctx->fr.itemId);
    if (!ResolveUseMethod() || !gFnUseReq) return;
    __try {
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, nullptr);
        ctx->used = true;
        // Same-frame qty (may not drop yet — server RTT). Still safer than worker WaitQtyDrop.
        ctx->qtyAfter = QtyOfItemId(ctx->fr.itemId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->used = false;
        x::runtime::LogW("Consumable", "FindUse SEH pos=%d id=%d", ctx->fr.pos, ctx->fr.itemId);
    }
}

struct QtyJobCtx {
    int itemId = 0;
    int qty = -1;
};

void QtyJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<QtyJobCtx*>(user);
    if (!ctx || ctx->itemId <= 0) return;
    ctx->qty = QtyOfItemId(ctx->itemId);
}

struct UseOnlyJobCtx {
    FindResult fr{};
    bool ok = false;
};

void UseOnlyJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<UseOnlyJobCtx*>(user);
    if (!ctx || !ctx->fr.ok || ctx->fr.pos <= 0 || ctx->fr.itemId <= 0) return;
    if (!ResolveUseMethod() || !gFnUseReq) return;
    __try {
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, nullptr);
        ctx->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->ok = false;
    }
}

bool FindItemIdOnMain(int itemId, FindResult& out) {
    out = {};
    if (itemId <= 0) return false;
    if (!world::EnsureBound()) return false;
    EnsureFieldOffsets();
    const int t = kItemTypeConsume;
    const int pos = CallGetItemSlotPos(t, itemId);
    if (pos > 0) {
        void* item = GetItemAtTypePos(t, pos);
        int qty = ItemQty(item);
        if (qty <= 0) qty = CallGetItemCount(t, itemId);
        if (qty <= 0) qty = 1;
        out.pos = pos;
        out.listIndex = pos;
        out.itemId = itemId;
        out.qty = qty;
        out.posFromSlot = true;
        out.ok = true;
        static DWORD sPosLog = 0;
        const DWORD now = x::runtime::NowMs();
        if (!sPosLog || static_cast<int>(now - sPosLog) >= 10000) {
            sPosLog = now;
            x::runtime::LogI("Consumable", "GetItemSlotPos type=%d pos=%d id=%d qty=%d", t, pos,
                             itemId, qty);
        }
        return true;
    }
    struct Hit {
        int itemId = 0;
        FindResult* out = nullptr;
        bool ok = false;
    } hit{itemId, &out, false};
    ForEachUseBagSlot(
        [](void* item, int i, void* user) {
            auto* h = static_cast<Hit*>(user);
            if (h->ok || !h->out) return;
            if (ReadI32(item, gOffItemId) != h->itemId) return;
            const int qty = ItemQty(item);
            if (qty <= 0) return;
            const int fromSlot = SlotNPos(item);
            const int pos = fromSlot > 0 ? fromSlot : PickUsePos(item, i);
            if (pos <= 0) return;
            h->out->pos = pos;
            h->out->listIndex = i;
            h->out->itemId = h->itemId;
            h->out->qty = qty;
            h->out->posFromSlot = fromSlot > 0;
            h->out->ok = true;
            h->ok = true;
            if (fromSlot > 0 && fromSlot != i) {
                static DWORD s_nposLog = 0;
                const DWORD now = x::runtime::NowMs();
                if (!s_nposLog || static_cast<int>(now - s_nposLog) >= 10000) {
                    s_nposLog = now;
                    x::runtime::LogI("Consumable", "nPOS=%d listIndex=%d id=%d qty=%d", fromSlot, i,
                                     h->itemId, qty);
                }
            }
        },
        &hit);
    return hit.ok;
}

// MUST only run on Unity main. PageDown=HP / PageUp=MP → FuncType.Item → consume slot.
bool ResolveBoundPotionOnMain(bool wantHp, FindResult& out) {
    out = {};
    auto fail = [&](const char* why, int type, int value, int itemId = 0) {
        out = {};
        out.missWhy = why;
        out.itemId = itemId;
        LogBindMissThrottled(why, wantHp, type, value);
        return false;
    };
    EnsureFkFieldOff();
    if (!EnsureFkmOnMain()) return fail("no_fkm", -1, 0);
    EnsureGetDataByKeyCodeMi();
    auto getData = [&]() -> FnGetDataByKeyCode {
        if (gMiGetDataByKeyCode && gMiGetDataByKeyCode->methodPointer)
            return reinterpret_cast<FnGetDataByKeyCode>(gMiGetDataByKeyCode->methodPointer);
        return x::runtime::il2cpp::AtRva<FnGetDataByKeyCode>(kRvaGetDataByKeyCode);
    }();
    if (!getData) return fail("no_getdata", -1, 0);
    const int32_t unityKey = input::VkToUnityKey(wantHp ? VK_NEXT : VK_PRIOR);
    if (unityKey <= 0) return fail("bad_vk", -1, 0);
    void* fk = nullptr;
    __try {
        fk = getData(gFkm, unityKey, gMiGetDataByKeyCode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fk = nullptr;
        return fail("getdata_seh", -1, 0);
    }
    if (!fk || !LooksLikeHeapPtr(fk)) return fail("empty_bind", 0, 0);
    int32_t type = 0, value = 0;
    if (!ReadFkFields(fk, &type, &value)) return fail("fk_read", -1, 0);
    // FuncType.None(0)+value0 = 未绑；勿写成 not_item。
    if (type == 0 && value == 0) return fail("empty_bind", type, value);
    if (type != kFuncTypeItem) return fail("not_item", type, value, value);
    if (!AcceptBoundItemId(value)) return fail("bad_item", type, value, value);
    if (!FindItemIdOnMain(value, out) || !out.ok) return fail("not_in_bag", type, value, value);
    out.missWhy = nullptr;
    return true;
}

struct BoundResolveJobCtx {
    bool wantHp = true;
    FindResult* out = nullptr;
    bool ok = false;
};

void BoundResolveJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<BoundResolveJobCtx*>(user);
    if (!ctx || !ctx->out) return;
    ctx->ok = ResolveBoundPotionOnMain(ctx->wantHp, *ctx->out);
}

struct FindUseBoundJobCtx {
    bool wantHp = true;
    FindResult fr{};
    int qtyBefore = -1;
    int qtyAfter = -1;
    bool found = false;
    bool used = false;
};

void FindUseBoundJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<FindUseBoundJobCtx*>(user);
    if (!ctx) return;
    if (!ResolveBoundPotionOnMain(ctx->wantHp, ctx->fr) || !ctx->fr.ok) return;
    ctx->found = true;
    ctx->qtyBefore = QtyOfItemId(ctx->fr.itemId);
    if (!ResolveUseMethod() || !gFnUseReq) return;
    __try {
        // Official FuncKey site: (nPOS, itemId, /*MethodInfo=*/null).
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, nullptr);
        ctx->used = true;
        ctx->qtyAfter = QtyOfItemId(ctx->fr.itemId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->used = false;
        x::runtime::LogW("Consumable", "FindUseBound SEH pos=%d id=%d", ctx->fr.pos, ctx->fr.itemId);
    }
}

struct FindUseIdJobCtx {
    int itemId = 0;
    FindResult fr{};
    int qtyBefore = -1;
    int qtyAfter = -1;
    int mapIdBefore = 0;
    int mapIdAfter = 0;
    bool found = false;
    bool used = false;
    bool resolveMiss = false;
    bool seh = false;
    bool listMiss = false;  // GetConsumeList / size 异常
};

void FindUseIdJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<FindUseIdJobCtx*>(user);
    if (!ctx) return;
    if (!world::EnsureBound()) {
        ctx->listMiss = true;
        return;
    }
    if (!FindItemIdOnMain(ctx->itemId, ctx->fr) || !ctx->fr.ok) return;
    ctx->found = true;
    ctx->qtyBefore = QtyOfItemId(ctx->fr.itemId);
    // 回家/城镇卷必须走 PortalScroll；StatChange 只服务药水，会 no-op 且不扣数量。
    if (!ResolvePortalScrollMethod() || !gFnPortalScroll) {
        ctx->resolveMiss = true;
        return;
    }
    ctx->mapIdBefore = travel::CurrentMapId();
    __try {
        // 与 FuncKey / UseJobOnMain 一致：第三参传 null MI。
        gFnPortalScroll(ctx->fr.pos, ctx->fr.itemId, nullptr);
        ctx->used = true;
        // 换图窗内消耗栏常短暂不可读 → qtyAfter=-1（BIN b19da8）；仍以 mapId 旁证成功。
        ctx->qtyAfter = QtyOfItemId(ctx->fr.itemId);
        ctx->mapIdAfter = travel::CurrentMapId();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->used = false;
        ctx->seh = true;
        x::runtime::LogW("Consumable", "PortalScroll SEH pos=%d id=%d", ctx->fr.pos, ctx->itemId);
    }
}

}  // namespace

void Init() {
    gMiUseReq = nullptr;
    gFnUseReq = nullptr;
    gMiPortalScroll = nullptr;
    gFnPortalScroll = nullptr;
    gKlassSlotItem = nullptr;
    gFkm = nullptr;
    gFkmKlass = nullptr;
    gMiGetDataByKeyCode = nullptr;
    gLastFkmRebind = 0;
    gLastBindMissLogHp = 0;
    gLastBindMissLogMp = 0;
    gCdGetItem = nullptr;
    gMiCdGetItem = nullptr;
    gCdGetItemSlotPos = nullptr;
    gMiCdGetItemSlotPos = nullptr;
    gCdGetItemCount = nullptr;
    gMiCdGetItemCount = nullptr;
    gFkFieldTried = false;
    gOffFkType = kFbFkType;
    gOffFkValue = kFbFkValue;
    gLoggedUseReqRvaMiss = false;
    gLoggedPortalRvaMiss = false;
    gLastUseMiRetryMs = 0;
    gLastPortalMiRetryMs = 0;
    gConsumePosMode.store(static_cast<int>(ConsumePosMode::Unknown), std::memory_order_relaxed);
    gFieldOffResolved.store(false, std::memory_order_release);
    gOffWmCharacterData = kFbWmCharacterData;
    gOffCdItemSlots = kFbCdItemSlots;
    gOffItemId = kFbItemId;
    gOffBundleNumber = kFbBundleNumber;
    EnsureFieldOffsets();
    x::runtime::LogI("Consumable",
                     "consumable_port ready (shared MainPump; field anti-drift + UseRequest/"
                     "PortalScroll + PageDown/PageUp bind)");
    // 急切绑定：启动即解析 UseRequest / PortalScroll（不必等第一次用药/用卷）
    (void)ResolveUseMethod();
    (void)ResolvePortalScrollMethod();
}

void Shutdown() {
    gMiUseReq = nullptr;
    gFnUseReq = nullptr;
    gMiPortalScroll = nullptr;
    gFnPortalScroll = nullptr;
    gFkm = nullptr;
    gFkmKlass = nullptr;
    gMiGetDataByKeyCode = nullptr;
    gCdGetItem = nullptr;
    gMiCdGetItem = nullptr;
    gCdGetItemSlotPos = nullptr;
    gMiCdGetItemSlotPos = nullptr;
    gCdGetItemCount = nullptr;
    gMiCdGetItemCount = nullptr;
    gLoggedUseReqRvaMiss = false;
    gLoggedPortalRvaMiss = false;
    gLastUseMiRetryMs = 0;
    gLastPortalMiRetryMs = 0;
    gConsumePosMode.store(static_cast<int>(ConsumePosMode::Unknown), std::memory_order_relaxed);
}

bool FindPotion(PotionKind kind, FindResult& out) {
    out = {};
    FindJobCtx ctx{};
    ctx.kind = kind;
    ctx.out = &out;
    if (!x::runtime::main_thread::InvokeAndWait(&FindJobOnMain, &ctx, kJobWaitMs)) {
        x::runtime::LogW("Consumable", "FindPotion pump fail/timeout");
        return false;
    }
    return ctx.ok;
}

bool FindAndUsePotion(PotionKind kind, FindResult& out) {
    out = {};
    FindUseJobCtx ctx{};
    ctx.kind = kind;
    if (!x::runtime::main_thread::InvokeAndWait(&FindUseJobOnMain, &ctx, kJobWaitMs)) {
        x::runtime::LogW("Consumable", "FindAndUse pump fail/timeout");
        return false;
    }
    out = ctx.fr;
    if (!ctx.found || !ctx.fr.ok) return false;
    if (!ctx.used) return false;

    // Server may lag; wait on worker WITHOUT touching managed, then one main-thread qty read.
    if (ctx.qtyBefore >= 0 && ctx.qtyAfter >= 0 && ctx.qtyAfter < ctx.qtyBefore) {
        NoteConsumePosSuccess(ctx.fr.listIndex, ctx.fr.pos);
        x::runtime::LogI("Consumable", "UseRequest ok pos=%d id=%d qty %d→%d", ctx.fr.pos,
                         ctx.fr.itemId, ctx.qtyBefore, ctx.qtyAfter);
        return true;
    }
    Sleep(280);
    QtyJobCtx q{};
    q.itemId = ctx.fr.itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        NoteConsumePosSuccess(ctx.fr.listIndex, ctx.fr.pos);
        x::runtime::LogI("Consumable", "UseRequest ok pos=%d id=%d qty %d→%d (delayed)",
                         ctx.fr.pos, ctx.fr.itemId, ctx.qtyBefore, q.qty);
        return true;
    }

    // One alt-POS retry entirely via main jobs (still no worker managed reads).
    int alt = -1;
    if (ctx.fr.listIndex >= 0) {
        if (ctx.fr.pos == ctx.fr.listIndex + 1)
            alt = ctx.fr.listIndex;
        else if (ctx.fr.pos == ctx.fr.listIndex)
            alt = ctx.fr.listIndex + 1;
    }
    if (alt < 0 || alt == ctx.fr.pos) {
        x::runtime::LogW("Consumable", "UseRequest empty id=%d pos=%d qtyBefore=%d after=%d",
                         ctx.fr.itemId, ctx.fr.pos, ctx.qtyBefore, q.qty);
        return false;
    }
    x::runtime::LogW("Consumable", "UseRequest empty id=%d pos=%d; retry altPos=%d", ctx.fr.itemId,
                     ctx.fr.pos, alt);
    UseOnlyJobCtx retry{};
    retry.fr = ctx.fr;
    retry.fr.pos = alt;
    if (!x::runtime::main_thread::InvokeAndWait(&UseOnlyJobOnMain, &retry, kJobWaitMs) ||
        !retry.ok) {
        return false;
    }
    Sleep(280);
    q = {};
    q.itemId = ctx.fr.itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        out.pos = alt;
        NoteConsumePosSuccess(ctx.fr.listIndex, alt);
        x::runtime::LogI("Consumable", "UseRequest ok altPos=%d id=%d qty %d→%d", alt,
                         ctx.fr.itemId, ctx.qtyBefore, q.qty);
        return true;
    }
    x::runtime::LogW("Consumable", "UseRequest empty after altPos=%d id=%d qty=%d", alt,
                     ctx.fr.itemId, q.qty);
    return false;
}

bool ResolveBoundPotion(bool wantHp, FindResult& out) {
    out = {};
    if (x::runtime::main_thread::IsCongested() ||
        x::features::soft_login_probe::IsGameplayQuiet() ||
        x::features::soft_login_probe::IsReconnectInFlight() ||
        !x::runtime::main_thread::IsPumpTicking(400)) {
        out.missWhy = "quiet";
        return false;
    }
    BoundResolveJobCtx ctx{};
    ctx.wantHp = wantHp;
    ctx.out = &out;
    if (!x::runtime::main_thread::InvokeAndWait(&BoundResolveJobOnMain, &ctx, kJobWaitMs)) {
        out.missWhy = "pump";
        x::runtime::LogW("Consumable", "ResolveBoundPotion pump fail/timeout key=%s",
                         wantHp ? "PageDown" : "PageUp");
        return false;
    }
    return ctx.ok;
}

bool PeekBoundPotionItemId(bool wantHp, int& outItemId) {
    outItemId = 0;
    FindResult fr{};
    (void)ResolveBoundPotion(wantHp, fr);
    if (fr.itemId <= 0) return false;
    // ok / not_in_bag / qty=0：都是合法 Item 绑定；其它 miss（empty_bind/not_item/…）不算。
    if (fr.ok) {
        outItemId = fr.itemId;
        return true;
    }
    if (fr.missWhy && std::strcmp(fr.missWhy, "not_in_bag") == 0) {
        outItemId = fr.itemId;
        return true;
    }
    return false;
}

bool FindAndUseBoundPotion(bool wantHp, FindResult& out) {
    out = {};
    FindUseBoundJobCtx ctx{};
    ctx.wantHp = wantHp;
    if (!x::runtime::main_thread::InvokeAndWait(&FindUseBoundJobOnMain, &ctx, kJobWaitMs)) {
        out.missWhy = "pump";
        x::runtime::LogW("Consumable", "FindAndUseBound pump fail/timeout key=%s",
                         wantHp ? "PageDown" : "PageUp");
        return false;
    }
    out = ctx.fr;
    if (!ctx.found || !ctx.fr.ok) return false;
    if (!ctx.used) return false;

    if (ctx.qtyBefore >= 0 && ctx.qtyAfter >= 0 && ctx.qtyAfter < ctx.qtyBefore) {
        NoteConsumePosSuccess(ctx.fr.listIndex, ctx.fr.pos);
        x::runtime::LogI("Consumable", "UseRequest(bound) ok pos=%d id=%d qty %d→%d key=%s",
                         ctx.fr.pos, ctx.fr.itemId, ctx.qtyBefore, ctx.qtyAfter,
                         wantHp ? "PageDown" : "PageUp");
        return true;
    }
    // 双段 delayed：首段短、失败再补一拍（BIN 多数首段即 ok；过长会拖慢连喝）。
    for (int pass = 0; pass < 2; ++pass) {
        Sleep(pass == 0 ? 200 : 180);
        QtyJobCtx q{};
        q.itemId = ctx.fr.itemId;
        if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
            ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
            NoteConsumePosSuccess(ctx.fr.listIndex, ctx.fr.pos);
            x::runtime::LogI("Consumable",
                             "UseRequest(bound) ok pos=%d id=%d qty %d→%d (delayed%s) key=%s",
                             ctx.fr.pos, ctx.fr.itemId, ctx.qtyBefore, q.qty,
                             pass == 0 ? "" : "2", wantHp ? "PageDown" : "PageUp");
            return true;
        }
    }
    QtyJobCtx q{};
    q.itemId = ctx.fr.itemId;
    q.qty = -1;
    (void)x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs);

    // 格子 nPOS 已钉死，或 listIndex 作 nPOS 已验证：alt=listIndex+1 只会空烧 CD。
    const int mode = gConsumePosMode.load(std::memory_order_relaxed);
    const bool skipAlt =
        ctx.fr.posFromSlot ||
        mode == static_cast<int>(ConsumePosMode::ListIndexIsPos) ||
        (ctx.fr.listIndex >= 1 && ctx.fr.pos == ctx.fr.listIndex);
    if (skipAlt) {
        x::runtime::LogW("Consumable",
                         "UseRequest(bound) empty id=%d pos=%d qtyBefore=%d after=%d (skip alt)",
                         ctx.fr.itemId, ctx.fr.pos, ctx.qtyBefore, q.qty);
        return false;
    }

    // One alt-POS retry entirely via main jobs (still no worker managed reads).
    int alt = -1;
    if (ctx.fr.listIndex >= 0) {
        if (ctx.fr.pos == ctx.fr.listIndex + 1)
            alt = ctx.fr.listIndex;
        else if (ctx.fr.pos == ctx.fr.listIndex)
            alt = ctx.fr.listIndex + 1;
    }
    if (alt < 0 || alt == ctx.fr.pos) {
        x::runtime::LogW("Consumable", "UseRequest(bound) empty id=%d pos=%d qtyBefore=%d after=%d",
                         ctx.fr.itemId, ctx.fr.pos, ctx.qtyBefore, q.qty);
        return false;
    }
    x::runtime::LogW("Consumable", "UseRequest(bound) empty id=%d pos=%d; retry altPos=%d",
                     ctx.fr.itemId, ctx.fr.pos, alt);
    UseOnlyJobCtx retry{};
    retry.fr = ctx.fr;
    retry.fr.pos = alt;
    if (!x::runtime::main_thread::InvokeAndWait(&UseOnlyJobOnMain, &retry, kJobWaitMs) ||
        !retry.ok) {
        return false;
    }
    Sleep(180);
    q = {};
    q.itemId = ctx.fr.itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        out.pos = alt;
        NoteConsumePosSuccess(ctx.fr.listIndex, alt);
        x::runtime::LogI("Consumable", "UseRequest(bound) ok altPos=%d id=%d qty %d→%d", alt,
                         ctx.fr.itemId, ctx.qtyBefore, q.qty);
        return true;
    }
    x::runtime::LogW("Consumable", "UseRequest(bound) empty after altPos=%d id=%d qty=%d", alt,
                     ctx.fr.itemId, q.qty);
    return false;
}

bool FindAndUseByItemId(int itemId, FindResult& out) {
    out = {};
    if (itemId <= 0) {
        x::runtime::LogW("Consumable", "FindAndUseById bad_code id=%d", itemId);
        return false;
    }
    FindUseIdJobCtx ctx{};
    ctx.itemId = itemId;
    if (!x::runtime::main_thread::InvokeAndWait(&FindUseIdJobOnMain, &ctx, kJobWaitMs)) {
        x::runtime::LogW("Consumable", "FindAndUseById pump fail/timeout id=%d", itemId);
        return false;
    }
    out = ctx.fr;
    if (ctx.listMiss) {
        x::runtime::LogW("Consumable", "FindAndUseById list_miss id=%d (consume list unbound/empty)",
                         itemId);
        return false;
    }
    if (!ctx.found || !ctx.fr.ok) {
        x::runtime::LogW("Consumable", "FindAndUseById not_found id=%d", itemId);
        return false;
    }
    if (!ctx.used) {
        if (ctx.resolveMiss) {
            x::runtime::LogW("Consumable",
                             "FindAndUseById use_fail id=%d pos=%d qty=%d (PortalScroll unresolved)",
                             itemId, ctx.fr.pos, ctx.fr.qty);
        } else if (ctx.seh) {
            x::runtime::LogW("Consumable",
                             "FindAndUseById use_fail id=%d pos=%d qty=%d (PortalScroll SEH)", itemId,
                             ctx.fr.pos, ctx.fr.qty);
        } else {
            x::runtime::LogW("Consumable", "FindAndUseById use_fail id=%d pos=%d qty=%d", itemId,
                             ctx.fr.pos, ctx.fr.qty);
        }
        return false;
    }
    if (ctx.qtyBefore >= 0 && ctx.qtyAfter >= 0 && ctx.qtyAfter < ctx.qtyBefore) {
        x::runtime::LogI("Consumable", "PortalScroll ok pos=%d id=%d qty %d→%d", ctx.fr.pos, itemId,
                         ctx.qtyBefore, ctx.qtyAfter);
        return true;
    }
    // BIN b19da8：发包当拍已 104000100→104000000，但换图窗 qtyAfter=-1 → 旧逻辑误判 no_consume。
    if (ctx.mapIdBefore > 0 && ctx.mapIdAfter > 0 && ctx.mapIdAfter != ctx.mapIdBefore) {
        x::runtime::LogI("Consumable",
                         "PortalScroll ok pos=%d id=%d map %d→%d (qty %d→%d; map_changed)",
                         ctx.fr.pos, itemId, ctx.mapIdBefore, ctx.mapIdAfter, ctx.qtyBefore,
                         ctx.qtyAfter);
        return true;
    }
    Sleep(220);
    QtyJobCtx q{};
    q.itemId = itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        x::runtime::LogI("Consumable", "PortalScroll ok pos=%d id=%d qty %d→%d (delayed)", ctx.fr.pos,
                         itemId, ctx.qtyBefore, q.qty);
        return true;
    }
    // delayed 复核时图可能已变（qty 仍读不到）：再认一次 map 旁证。
    const int mapNow = travel::CurrentMapId();
    if (ctx.mapIdBefore > 0 && mapNow > 0 && mapNow != ctx.mapIdBefore) {
        x::runtime::LogI("Consumable",
                         "PortalScroll ok pos=%d id=%d map %d→%d (delayed map_changed; qtyAfter=%d)",
                         ctx.fr.pos, itemId, ctx.mapIdBefore, mapNow, q.qty);
        return true;
    }
    // 已发包、数量未降且未换图：真失败（勿冒充 ok 让 AutoSupply 干等）。
    x::runtime::LogW("Consumable",
                     "PortalScroll no_consume id=%d pos=%d qtyBefore=%d after=%d map=%d→%d/%d → fail",
                     itemId, ctx.fr.pos, ctx.qtyBefore, q.qty, ctx.mapIdBefore, ctx.mapIdAfter,
                     mapNow);
    return false;
}

bool UseStatChangeItem(const FindResult& fr) {
    if (!fr.ok || fr.pos <= 0 || fr.itemId <= 0) return false;
    UseOnlyJobCtx ctx{};
    ctx.fr = fr;
    if (!x::runtime::main_thread::InvokeAndWait(&UseOnlyJobOnMain, &ctx, kJobWaitMs)) {
        x::runtime::LogW("Consumable", "UseRequest pump fail/timeout pos=%d id=%d", fr.pos,
                         fr.itemId);
        return false;
    }
    if (!ctx.ok) return false;
    x::runtime::LogI("Consumable", "UseRequest fired pos=%d id=%d (no qty verify)", fr.pos,
                     fr.itemId);
    return true;
}

bool UseStatChangeItem(int nPos) {
    if (nPos <= 0) return false;
    FindResult fr{};
    if (!FindPotion(PotionKind::Hp, fr) || fr.pos != nPos) {
        FindResult mp{};
        if (!FindPotion(PotionKind::Mp, mp) || mp.pos != nPos) {
            // Last resort: main job scan by pos
            struct ByPosCtx {
                int pos = 0;
                FindResult fr{};
                bool ok = false;
            } bp{};
            bp.pos = nPos;
            auto job = [](void* user) {
                auto* c = reinterpret_cast<ByPosCtx*>(user);
                ForEachUseBagSlot(
                    [](void* item, int i, void* user2) {
                        auto* c2 = static_cast<ByPosCtx*>(user2);
                        if (c2->ok) return;
                        if (PickUsePos(item, i) != c2->pos) return;
                        c2->fr.pos = c2->pos;
                        c2->fr.listIndex = i;
                        c2->fr.itemId = ReadI32(item, gOffItemId);
                        c2->fr.qty = ItemQty(item);
                        c2->fr.ok = c2->fr.itemId > 0;
                        c2->ok = c2->fr.ok;
                    },
                    c);
            };
            if (!x::runtime::main_thread::InvokeAndWait(job, &bp, kJobWaitMs) || !bp.ok) {
                x::runtime::LogW("Consumable", "UseRequest by-pos: no item at pos=%d", nPos);
                return false;
            }
            return UseStatChangeItem(bp.fr);
        }
        fr = mp;
    }
    return UseStatChangeItem(fr);
}

bool IsMpPotionItem(int itemId) { return MpRank(itemId) >= 0; }

}  // namespace x::features::ports::consumable
