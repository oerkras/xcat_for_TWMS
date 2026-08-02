#pragma once
// Consumable port — scan USE inventory + SendStatChangeItemUseRequest (Classic TWMS).
// Main-thread calls via shared x/runtime/main_thread_pump (no private SendWill hook).

namespace x::features::ports::consumable {

enum class PotionKind { Hp, Mp };

struct FindResult {
    int pos = -1;       // nPOS for UseRequest (1-based maple slot)
    int listIndex = -1; // raw List index (for empty-use POS retry)
    int itemId = 0;
    int qty = 0;
    bool ok = false;
};

void Init();
void Shutdown();

// Scan Consume tab. Marshals to Unity main thread (worker must NOT touch managed heap).
bool FindPotion(PotionKind kind, FindResult& out);

// Find + UseRequest in one main-thread job (preferred from autopot worker).
bool FindAndUsePotion(PotionKind kind, FindResult& out);

// 按精确 itemId 找消耗栏并 UseRequest（回城卷等）；成功仅表示已发包且数量可能下降。
bool FindAndUseByItemId(int itemId, FindResult& out);

// UseRequest only (main-thread). Prefer FindAndUsePotion from workers.
bool UseStatChangeItem(const FindResult& fr);
bool UseStatChangeItem(int nPos);

}  // namespace x::features::ports::consumable
