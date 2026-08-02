#pragma once
// Classic TWMS sellbag — 开 NPC 店后一键卖装/其他 + 名称模糊白名单。

#include "../../../common/xcat_sellbag.h"

#include <cstdint>

namespace x::features::sellbag {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

bool RequestSell(uint32_t bagMask);
// 行程驱动：同 RequestSell，但抑制完成/失败弹窗（由调用方汇总）。
bool RequestSellQuiet(uint32_t bagMask);
void Abort(const char* reason);
bool IsBusy();

struct Status {
    bool     busy = false;
    uint32_t state = 0;  // 0=idle 1=selling 2=done 3=error
    uint32_t lastBagMask = 0;
    uint32_t equipSold = 0;
    uint32_t etcSold = 0;
    uint32_t kept = 0;
    uint32_t failed = 0;
    int64_t  mesoGained = 0;  // 本轮金币差；未知则 0 且 mesoGainedValid=0
    uint32_t mesoGainedValid = 0;
    char     message[128]{};
    uint64_t lastRunTickMs = 0;
};

void GetStatus(Status& out);
void PublishStatusToShm();

}  // namespace x::features::sellbag
