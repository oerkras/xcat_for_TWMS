#pragma once

#include <cstddef>
#include <cstdint>

namespace x::features::titlebar::game {

struct Vitals {
    bool ok = false;
    int level = 0;
    int job = 0;
    int hp = 0;
    int mhp = 0;
    int mp = 0;
    int mmp = 0;
    int exp = 0;
    int maxExp = 0;
    long long meso = 0;
    char name[64]{};
};

bool BindApis();
// LOGIN workers 前泵上预热 ItemData* FindClass（BIN 11:56 Titlebar worker → GC Fatal）。
void WarmForLoginWorkers();
bool TryResolveLocalUser();
bool TryResolveItemDataManager();
void* LocalCharacterStat();  // → x::ui::player（WM→CS）
bool LocalUserLooksOk();
void ClearLocalUser();
bool ReadVitals(Vitals& out);  // → x::ui::player::Read

// 消耗栏高价值 → ASCII `id:qty,id:qty`（探活用；无 UI）。
// 含 204/234 卷轴与雷之鏢 2070005。列表可读返回 true（无件则 dst 空串）；不可读 false。
bool FormatWealthScrolls(char* dst, size_t cap);

// 返回本拍背包新增物品的卖价；countIntoRate 为 false 时仅建立基线。
// mpPotConsumedOut：消耗栏蓝瓶数量下降（MpRank>=0）；买药/补货的增加不计。
double UpdateLootDelta(bool countIntoRate, uint64_t* knownOut = nullptr,
                       uint64_t* unknownOut = nullptr, uint64_t* mpPotConsumedOut = nullptr);
void ResetLootBaseline();

const char* JobNameTw(int job);
const char* JobText(int job, char (&buffer)[16]);

}  // namespace x::features::titlebar::game
