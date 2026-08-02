#pragma once
// Classic TWMS 自动补给 — 对齐枫星行为（Il2Cpp 开店/买卖，离线杂货寻店）。
// 开店：TalkToNpc(+模板优先) → UIUtilDialogEx 菜单点「商店」→ ShopReady。
// 非目标：全量 Commodity 货架寻店；飞镖 Charge（暂 stub）。

#include <cstddef>
#include <cstdint>

namespace x::features::auto_supply {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetDesired(bool on);
bool IsDesired();
bool IsBusy();

// F5/挂机开启时调用：把当前野图记为挂机图；城镇不写入/不覆盖。
void RecordHangupFarmMap(const char* reason = nullptr);

bool PeekLastFarmMap(char* out, size_t outCap);
void RequestReturnFarm();
void RequestManualTrip();
void AbortTrip(const char* reason);

enum class TripState : uint32_t {
    Idle = 0,
    Pause = 1,
    GoingTown = 2,
    OpeningShop = 3,
    Selling = 4,
    Buying = 5,
    Returning = 6,
    Cooldown = 7,
};

struct Status {
    TripState state = TripState::Idle;
    bool      busy = false;
    char      message[128]{};
    char      lastFarmMap[64]{};
    char      shopMap[64]{};
    int       equipUsed = 0;
    int       equipCap = 0;
};

void GetStatus(Status& out);

}  // namespace x::features::auto_supply
