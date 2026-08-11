#pragma once
// Consumable port — PageDown/PageUp bind + Consume inventory + UseRequest (Classic TWMS).
// Main-thread calls via shared x/runtime/main_thread_pump (no private SendWill hook).

namespace x::features::ports::consumable {

enum class PotionKind { Hp, Mp };

struct FindResult {
    int pos = -1;       // nPOS for UseRequest (1-based maple slot)
    int listIndex = -1; // raw List index (for empty-use POS retry)
    int itemId = 0;
    int qty = 0;
    bool ok = false;
    // Bound-resolve miss tag (static literal). Set when ok==false from ResolveBoundPotion /
    // FindAndUseBoundPotion; e.g. empty_bind / soft_reject / not_in_bag / not_item.
    const char* missWhy = nullptr;
};

void Init();
void Shutdown();

// Scan Consume tab. Marshals to Unity main thread (worker must NOT touch managed heap).
// Kept for other callers; autopot uses FindAndUseBoundPotion (PageDown/PageUp bind).
bool FindPotion(PotionKind kind, FindResult& out);

// Find + UseRequest in one main-thread job (whitelist scan). Prefer Bound for autopot.
bool FindAndUsePotion(PotionKind kind, FindResult& out);

// Resolve PageDown(HP)/PageUp(MP) FuncKeyMapped Item bind → consume slot.
// Type must be FuncType.Item(2); 绑了什么就喝什么（无反类 soft-reject）；无扫栏回退。
bool ResolveBoundPotion(bool wantHp, FindResult& out);

// 读绑定消耗品 itemId（背包有/无均可）。未绑 / 非道具 / pump 失败返回 false。
bool PeekBoundPotionItemId(bool wantHp, int& outItemId);

// Resolve bind + SendStatChangeItemUseRequest (autopot main path; like fengxing bind-only).
bool FindAndUseBoundPotion(bool wantHp, FindResult& out);

// 按精确 itemId 找消耗栏并用 SendPortalScrollUseRequest（回家卷軸等）。
// 失败会打 Consumable 日志：bad_code / list_miss / not_found / use_fail / no_consume / pump fail。
// 成功：qty 下降，或发包后 mapId 已变（换图窗背包可读性差时 qty 可能为 -1）。
// 仅发包、数量未降且未换图 → 失败。
bool FindAndUseByItemId(int itemId, FindResult& out);

// UseRequest only (main-thread). Prefer FindAndUsePotion from workers.
bool UseStatChangeItem(const FindResult& fr);
bool UseStatChangeItem(int nPos);

}  // namespace x::features::ports::consumable
