// Consumable port — Classic TWMS inventory scan + UseRequest via shared main_thread_pump.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "consumable_port.h"

#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <atomic>
#include <cstring>

namespace x::features::ports::consumable {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr size_t kOffWmCharacterData = 0xE0;
constexpr size_t kOffCdItemSlots = 0x40;
constexpr size_t kOffItemId = 0x10;
constexpr size_t kOffBundleNumber = 0x28;
constexpr int kItemTypeConsume = 2;
constexpr DWORD kJobWaitMs = 1500;

// UISlotItem.SendStatChangeItemUseRequest — hashed names; TypeDefIndex still 488.
// Remount 2026-08-03: class/method rehashed; RVA 0x5E4940 → 0x5E3F90.
// Evidence: dump.cs static Send* declaration order + public/private matches CMS
// (Lottery → StatChange → AntiMacro…); FuncKey caller passes (nPOS, Value@+0x14) + null MI.
// Resolve: name → method-hash → RVA+kind(void,int,int) 校验（同类同形 ~11，kind 不唯一兜底）。
constexpr char kUiSlotItemClassHash[] =
    "ea99a706b02d8a8a14c68a9721a6a47ee59ec1dcf854806221b8db66078fd6c";
constexpr char kUseReqMethodHash[] =
    "f4c76544a7279af68ef016af56f05d746a74c18d023529bf9059753d31c05a9";
constexpr uint32_t kRvaSendStatChangeItemUseRequest = 0x5E3F90;

using FnUseRequest = void (*)(int nPos, int pPet, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

void* gKlassSlotItem = nullptr;
MethodInfoHead* gMiUseReq = nullptr;
FnUseRequest gFnUseReq = nullptr;

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
    return ReadI32(list, 0x18);
}

void* ListAt(void* list, int i) {
    if (!list || i < 0) return nullptr;
    void* items = ReadPtr(list, 0x10);
    if (!items) return nullptr;
    return ArrayAt(items, (uintptr_t)i);
}

int ItemQty(void* item) {
    if (!item) return 0;
    const int n = (int)ReadU16(item, kOffBundleNumber);
    if (n > 0) return n;
    return 1;
}

void* GetConsumeList() {
    void* wm = world::GetWorldManager();
    if (!wm) return nullptr;
    void* cd = ReadPtr(wm, kOffWmCharacterData);
    if (!LooksLikeHeapPtr(cd)) return nullptr;
    void* slotsArr = ReadPtr(cd, kOffCdItemSlots);
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
        if (ReadI32(item, kOffItemId) != itemId) continue;
        found = true;
        total += ItemQty(item);
    }
    return found ? total : -1;
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
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            const char* nm = e.methodGetName(raw);
            if (nm && strcmp(nm, name) == 0) {
                mi = reinterpret_cast<MethodInfoHead*>(raw);
                if (mi && mi->methodPointer) return mi;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool ResolveUseMethod() {
    if (gMiUseReq && gMiUseReq->methodPointer) {
        gFnUseReq = reinterpret_cast<FnUseRequest>(gMiUseReq->methodPointer);
        return true;
    }
    if (gFnUseReq) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;

    if (!gKlassSlotItem) {
        gKlassSlotItem = x::runtime::il2cpp::FindClass("", "UISlotItem");
        if (!gKlassSlotItem)
            gKlassSlotItem = x::runtime::il2cpp::FindClass("", kUiSlotItemClassHash);
    }
    if (!gKlassSlotItem) {
        // Last resort: direct RVA call without MethodInfo (static path often tolerates null MI).
        gFnUseReq = x::runtime::il2cpp::AtRva<FnUseRequest>(kRvaSendStatChangeItemUseRequest);
        if (gFnUseReq) {
            x::runtime::LogW("Consumable",
                             "UISlotItem klass miss — UseRequest via RVA 0x%X (null MI)",
                             kRvaSendStatChangeItemUseRequest);
            return true;
        }
        x::runtime::LogW("Consumable", "UISlotItem klass miss");
        return false;
    }

    // 1) 明文 / 方法哈希（哈希漂 RVA 时仍可活）
    const char* via = nullptr;
    MethodInfoHead* mi = FindMethodByName(gKlassSlotItem, "SendStatChangeItemUseRequest", 2);
    if (mi) via = "name";
    if (!mi) {
        mi = FindMethodByName(gKlassSlotItem, kUseReqMethodHash, 2);
        if (mi) via = "hash";
    }

    // 2) RVA + kind 校验。UISlotItem 上 public/private static void(int,int) 很多（~11），
    //    kind  alone 不唯一 → 与 GetSkillLevel 同策略：RVA 主路径，kind 只验形。
    if (!mi) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::ResolvePath;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kUse{2, TypeKind::Void, true, false, {TypeKind::I32, TypeKind::I32}};
        const auto mr = x::runtime::il2cpp_method::FindMethodCached(
            gKlassSlotItem, kRvaSendStatChangeItemUseRequest, kUse);
        mi = reinterpret_cast<MethodInfoHead*>(mr.method);
        if (mi) {
            via = (mr.path == ResolvePath::Kind) ? "kind" : "rva";
        }
    }

    if (mi && mi->methodPointer) {
        gMiUseReq = mi;
        gFnUseReq = reinterpret_cast<FnUseRequest>(mi->methodPointer);
        x::runtime::LogI("Consumable", "UseRequest MI=%p fn=%p via %s", (void*)mi, mi->methodPointer,
                         via ? via : "?");
        return true;
    }

    gFnUseReq = x::runtime::il2cpp::AtRva<FnUseRequest>(kRvaSendStatChangeItemUseRequest);
    if (gFnUseReq) {
        x::runtime::LogW("Consumable", "UseRequest MI miss — calling RVA 0x%X with null MI",
                         kRvaSendStatChangeItemUseRequest);
        return true;
    }
    x::runtime::LogW("Consumable", "SendStatChangeItemUseRequest resolve fail");
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

    const bool oneBased = (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);

    int bestPos = -1;
    int bestIdx = -1;
    int bestId = 0;
    int bestQty = 0;
    int bestRank = 99;
    for (int i = 0; i < n; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        const int id = ReadI32(item, kOffItemId);
        const int rank = (kind == PotionKind::Hp) ? HpRank(id) : MpRank(id);
        if (rank < 0) continue;
        const int qty = ItemQty(item);
        if (qty <= 0) continue;
        const int pos = oneBased ? i : (i + 1);
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
    const bool oneBased = (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);
    for (int i = 0; i < n; ++i) {
        void* item = ListAt(list, i);
        if (!item) continue;
        if (ReadI32(item, kOffItemId) != itemId) continue;
        const int qty = ItemQty(item);
        if (qty <= 0) continue;
        const int pos = oneBased ? i : (i + 1);
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

struct FindUseIdJobCtx {
    int itemId = 0;
    FindResult fr{};
    int qtyBefore = -1;
    int qtyAfter = -1;
    bool found = false;
    bool used = false;
};

void FindUseIdJobOnMain(void* user) {
    auto* ctx = reinterpret_cast<FindUseIdJobCtx*>(user);
    if (!ctx) return;
    if (!FindItemIdOnMain(ctx->itemId, ctx->fr) || !ctx->fr.ok) return;
    ctx->found = true;
    ctx->qtyBefore = QtyOfItemId(ctx->fr.itemId);
    if (!ResolveUseMethod() || !gFnUseReq) return;
    __try {
        gFnUseReq(ctx->fr.pos, ctx->fr.itemId, gMiUseReq);
        ctx->used = true;
        ctx->qtyAfter = QtyOfItemId(ctx->fr.itemId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->used = false;
        x::runtime::LogW("Consumable", "UseById SEH pos=%d id=%d", ctx->fr.pos, ctx->itemId);
    }
}

}  // namespace

void Init() {
    gMiUseReq = nullptr;
    gFnUseReq = nullptr;
    gKlassSlotItem = nullptr;
    x::runtime::LogI("Consumable",
                     "consumable_port ready (shared MainPump; no worker managed reads)");
}

void Shutdown() {
    gMiUseReq = nullptr;
    gFnUseReq = nullptr;
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
        x::runtime::LogI("Consumable", "UseRequest ok pos=%d id=%d qty %d→%d", ctx.fr.pos,
                         ctx.fr.itemId, ctx.qtyBefore, ctx.qtyAfter);
        return true;
    }
    Sleep(220);
    QtyJobCtx q{};
    q.itemId = ctx.fr.itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
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
    Sleep(220);
    q = {};
    q.itemId = ctx.fr.itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        out.pos = alt;
        x::runtime::LogI("Consumable", "UseRequest ok altPos=%d id=%d qty %d→%d", alt,
                         ctx.fr.itemId, ctx.qtyBefore, q.qty);
        return true;
    }
    x::runtime::LogW("Consumable", "UseRequest empty after altPos=%d id=%d qty=%d", alt,
                     ctx.fr.itemId, q.qty);
    return false;
}

bool FindAndUseByItemId(int itemId, FindResult& out) {
    out = {};
    if (itemId <= 0) return false;
    FindUseIdJobCtx ctx{};
    ctx.itemId = itemId;
    if (!x::runtime::main_thread::InvokeAndWait(&FindUseIdJobOnMain, &ctx, kJobWaitMs)) {
        x::runtime::LogW("Consumable", "FindAndUseById pump fail/timeout id=%d", itemId);
        return false;
    }
    out = ctx.fr;
    if (!ctx.found || !ctx.fr.ok || !ctx.used) return false;
    if (ctx.qtyBefore >= 0 && ctx.qtyAfter >= 0 && ctx.qtyAfter < ctx.qtyBefore) {
        x::runtime::LogI("Consumable", "UseById ok pos=%d id=%d qty %d→%d", ctx.fr.pos, itemId,
                         ctx.qtyBefore, ctx.qtyAfter);
        return true;
    }
    Sleep(220);
    QtyJobCtx q{};
    q.itemId = itemId;
    if (x::runtime::main_thread::InvokeAndWait(&QtyJobOnMain, &q, kJobWaitMs) && q.qty >= 0 &&
        ctx.qtyBefore >= 0 && q.qty < ctx.qtyBefore) {
        x::runtime::LogI("Consumable", "UseById ok pos=%d id=%d qty %d→%d (delayed)", ctx.fr.pos,
                         itemId, ctx.qtyBefore, q.qty);
        return true;
    }
    x::runtime::LogW("Consumable", "UseById fired id=%d pos=%d qtyBefore=%d after=%d (maybe empty)",
                     itemId, ctx.fr.pos, ctx.qtyBefore, q.qty);
    return true;
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
                const bool oneBased =
                    (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);
                for (int i = 0; i < n; ++i) {
                    void* item = ListAt(list, i);
                    if (!item) continue;
                    const int pos = oneBased ? i : (i + 1);
                    if (pos != c->pos) continue;
                    c->fr.pos = pos;
                    c->fr.listIndex = i;
                    c->fr.itemId = ReadI32(item, kOffItemId);
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
