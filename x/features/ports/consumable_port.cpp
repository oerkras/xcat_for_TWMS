// Consumable port — Classic TWMS inventory scan + UseRequest via shared main_thread_pump.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "consumable_port.h"

#include "input_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/mono_clock.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::features::ports::consumable {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr int kItemTypeConsume = 2;
constexpr DWORD kJobWaitMs = 1500;
constexpr DWORD kUseMiRetryMs = 3000;
constexpr DWORD kFkmRebindMs = 3000;

// FuncKeyMappedManager · remounted 2026-08-06（与 attack_input_port 钉值一致）
constexpr uint32_t kRvaGetDataByKeyCode = 0x1652600;
constexpr char kHashGetDataByKeyCode[] =
    "f9f41a36e032de163e54e45570ae92982f78e2e068a280be4e696256fe842a4";
constexpr char kFkmClass[] =
    "bccf462f59fa3ac757dd30992984c99e5bda74964a10a02eb5a53a54f02dd61";
constexpr int32_t kFuncTypeItem = 2;  // FuncType.Item
constexpr char kFuncKeyClass[] =
    "aee2472baeb766e84b81b7e54686e57dcb9a913f9773d94886c682c410ab778";
constexpr char kHashFkType[] =
    "b3635d661b985fc4a0eb47a782da14d314fe7933f628cfcbe826b3dd1213349";
constexpr char kHashFkValue[] =
    "ca24f1d7aec9f2b9ad4058e8356b09bfe8801a61e2fcd5cf728ab2e96b68105";
constexpr size_t kFbFkType = 0x10;
constexpr size_t kFbFkValue = 0x14;
size_t gOffFkType = kFbFkType;
size_t gOffFkValue = kFbFkValue;
bool gFkFieldTried = false;

// --- 字段防漂移（remount 2026-08-06 · TDI/offset 对齐；WM/FKM/FuncKey 未漂）---
constexpr char kHashWorldManager[] =
    "acda742ab51e7e2e3003fd2b44fbc00eababde4300ef17ac35b5f4fd01bee68";
constexpr char kHashCharacterData[] =
    "d5453e03707efd1001d8348a46ee270f8117468d2f1504fd0dadd0cc7c10468";
constexpr char kHashItemSlotBase[] =
    "e082912531f0de2015f6705b27ab5d3192e55481d44daaba05ed84b183076a5";
constexpr char kHashItemSlotBundle[] =
    "c8e2810801e0f102a4884f6a39994f988d9e601fcabd68b4c1c61457251a5ae";
constexpr char kHashWmCharacterData[] =
    "bf626b2e3edea540151d6fac0585e42759be6a77b833a03a455c50c8e3f9f9a";
constexpr char kHashCdItemSlots[] =
    "d2c891b1081084763d55e5348620df70c9946eac62b007ad5b0f9089fd18b42";
constexpr char kHashItemId[] =
    "a6ed5f11fa353cae3b4659363eda05ed44cd4461eddf82c7c8491f976cc1d44";
constexpr char kHashBundleNumber[] =
    "ae33d4afd39595ca1db745fe37096eb4e36ee4e5ac2431a6882d631932bf8d4";

constexpr size_t kFbWmCharacterData = 0xE0;
constexpr size_t kFbCdItemSlots = 0x40;
constexpr size_t kFbItemId = 0x10;
constexpr size_t kFbBundleNumber = 0x28;

size_t gOffWmCharacterData = kFbWmCharacterData;
size_t gOffCdItemSlots = kFbCdItemSlots;
size_t gOffItemId = kFbItemId;
size_t gOffBundleNumber = kFbBundleNumber;
std::atomic<bool> gFieldOffResolved{false};
char gFieldOffPath[64]{};

// UISlotItem.SendStatChangeItemUseRequest — 药水等属性道具；hashed；TypeDefIndex 488。
// Remount 2026-08-06: ACS class/method rehashed；RVA 未漂（仍 0x5E59C0）。
// Evidence: dump.cs static Send* 声明序对齐 CMS（Lottery → StatChange → AntiMacro → PortalScroll…）。
// Resolve: name → method-hash → RVA+kind(void,int,int)。
constexpr char kUiSlotItemClassHash[] =
    "f27685e07ea2fa1b39d0080d363376e5da40cc24b4079e358ad20deb45c0277";
constexpr char kUseReqMethodHash[] =
    "e04562d0d3c7e64a42eca834f520be68ae9fe3de1c8f876c9a7628d34b405e7";
constexpr uint32_t kRvaSendStatChangeItemUseRequest = 0x5E59C0;

// UISlotItem.SendPortalScrollUseRequest — 回家/城镇卷（2030xxx）；CMS private static (nPOS,nItemID)。
// TW dump 同簇；RVA 未漂 0x5E8610。
constexpr char kPortalScrollMethodHash[] =
    "abe138eb12140d28a8f318a98c052ba903f8672fda32836c581ba92eda2ba5b";
constexpr uint32_t kRvaSendPortalScrollUseRequest = 0x5E8610;

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

void LogBindMissThrottled(const char* why, bool wantHp, int type, int value) {
    const DWORD now = x::runtime::NowMs();
    DWORD& slot = wantHp ? gLastBindMissLogHp : gLastBindMissLogMp;
    if (slot && static_cast<int>(now - slot) < 10000) return;
    slot = now;
    x::runtime::LogW("Consumable", "bound pot miss key=%s why=%s type=%d value=%d",
                     wantHp ? "PageDown" : "PageUp", why, type, value);
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

void* GetConsumeList() {
    EnsureFieldOffsets();
    void* wm = world::GetWorldManager();
    if (!wm) return nullptr;
    void* cd = ReadPtr(wm, gOffWmCharacterData);
    if (!LooksLikeHeapPtr(cd)) return nullptr;
    void* slotsArr = ReadPtr(cd, gOffCdItemSlots);
    if (!LooksLikeHeapPtr(slotsArr)) return nullptr;
    const uintptr_t n = ArrayLen(slotsArr);
    if (n <= (uintptr_t)kItemTypeConsume) return nullptr;
    return ArrayAt(slotsArr, (uintptr_t)kItemTypeConsume);
}

int QtyOfItemId(int itemId) {
    if (itemId <= 0) return -1;
    void* list = GetConsumeList();
    if (!list) return -1;
    const int n = ListSize(list);
    int total = 0;
    bool found = false;
    for (int i = 0; i < n && i < 256; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        if (ReadI32(item, gOffItemId) != itemId) continue;
        found = true;
        total += ItemQty(item);
    }
    return found ? total : -1;
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
    void* list = GetConsumeList();
    if (!list) return false;
    const int n = ListSize(list);
    if (n <= 0 || n > 256) return false;

    int bestPos = -1;
    int bestIdx = -1;
    int bestId = 0;
    int bestQty = 0;
    int bestRank = 99;
    for (int i = 0; i < n; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        const int id = ReadI32(item, gOffItemId);
        const int rank = (kind == PotionKind::Hp) ? HpRank(id) : MpRank(id);
        if (rank < 0) continue;
        const int qty = ItemQty(item);
        if (qty <= 0) continue;
        int pos = -1, alt = -1;
        PickConsumePos(i, &pos, &alt);
        (void)alt;
        if (pos <= 0) continue;
        if (bestPos < 0 || rank < bestRank ||
            (rank == bestRank && (pos < bestPos || (pos == bestPos && qty > bestQty)))) {
            bestPos = pos;
            bestIdx = i;
            bestId = id;
            bestQty = qty;
            bestRank = rank;
        }
    }
    if (bestPos < 0) return false;
    out.pos = bestPos;
    out.listIndex = bestIdx;
    out.itemId = bestId;
    out.qty = bestQty;
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
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, gMiUseReq);
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
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, gMiUseReq);
        ctx->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->ok = false;
    }
}

bool FindItemIdOnMain(int itemId, FindResult& out) {
    out = {};
    if (itemId <= 0) return false;
    if (!world::EnsureBound()) return false;
    void* list = GetConsumeList();
    if (!list) return false;
    const int n = ListSize(list);
    if (n <= 0 || n > 256) return false;
    for (int i = 0; i < n; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        if (ReadI32(item, gOffItemId) != itemId) continue;
        const int qty = ItemQty(item);
        if (qty <= 0) continue;
        int pos = -1, alt = -1;
        PickConsumePos(i, &pos, &alt);
        (void)alt;
        if (pos <= 0) continue;
        out.pos = pos;
        out.listIndex = i;
        out.itemId = itemId;
        out.qty = qty;
        out.ok = true;
        return true;
    }
    return false;
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
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, gMiUseReq);
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
    void* list = GetConsumeList();
    if (!list) {
        ctx->listMiss = true;
        return;
    }
    const int n = ListSize(list);
    if (n <= 0 || n > 256) {
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
    __try {
        // 与 FuncKey / UseJobOnMain 一致：第三参传 null MI。
        gFnPortalScroll(ctx->fr.pos, ctx->fr.itemId, nullptr);
        ctx->used = true;
        ctx->qtyAfter = QtyOfItemId(ctx->fr.itemId);
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
    Sleep(280);
    QtyJobCtx q{};
    q.itemId = ctx.fr.itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        NoteConsumePosSuccess(ctx.fr.listIndex, ctx.fr.pos);
        x::runtime::LogI("Consumable",
                         "UseRequest(bound) ok pos=%d id=%d qty %d→%d (delayed) key=%s", ctx.fr.pos,
                         ctx.fr.itemId, ctx.qtyBefore, q.qty, wantHp ? "PageDown" : "PageUp");
        return true;
    }

    // One alt-POS retry entirely via main jobs (still no worker managed reads).
    // Prefer path already uses listIndex; alt covers rare ListIndexPlusOne layouts.
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
    Sleep(280);
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
    Sleep(220);
    QtyJobCtx q{};
    q.itemId = itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        x::runtime::LogI("Consumable", "PortalScroll ok pos=%d id=%d qty %d→%d (delayed)", ctx.fr.pos,
                         itemId, ctx.qtyBefore, q.qty);
        return true;
    }
    // 已发包但数量未降：对回城卷视为失败（勿冒充 ok 让 AutoSupply 干等后改走路）
    x::runtime::LogW("Consumable",
                     "PortalScroll no_consume id=%d pos=%d qtyBefore=%d after=%d → fail", itemId,
                     ctx.fr.pos, ctx.qtyBefore, q.qty);
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
                void* list = GetConsumeList();
                if (!list) return;
                const int n = ListSize(list);
                if (n <= 0 || n > 256) return;
                for (int i = 0; i < n; ++i) {
                    void* item = ListAt(list, i);
                    if (!item) continue;
                    int primary = -1, alt = -1;
                    PickConsumePos(i, &primary, &alt);
                    if (primary != c->pos && alt != c->pos) continue;
                    c->fr.pos = c->pos;
                    c->fr.listIndex = i;
                    c->fr.itemId = ReadI32(item, gOffItemId);
                    c->fr.qty = ItemQty(item);
                    c->fr.ok = c->fr.itemId > 0;
                    c->ok = c->fr.ok;
                    break;
                }
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

}  // namespace x::features::ports::consumable
