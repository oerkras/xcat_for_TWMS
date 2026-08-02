#pragma once
// Classic TWMS — manual random channel hop (挂机卡「随机换频」/ F10).
// 对照枫星 manualRejoinSeq UX；执行直调 Field.SendTransferChannelRequest，
// **不开** UIChannelShift / UIGameMenu。不搬 SceneManager.Reload / 命名管道。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::features::channel_hop {

enum class State : unsigned {
    Idle = 0,
    Selecting,
    Confirming,
    Waiting,
};

void Init();
void Shutdown();
void StartWorker();
// Signal-only stop (no join). Safe for DllMain DETACH / loader lock.
void StopWorker();

// Edge-triggered from payload_control (manualRejoinSeq).
void RequestManualRejoin(uint32_t seq);

State GetState();
const char* GetStateName();
bool HasPending();
// 成功/失败冷却剩余 ms；0=可立刻再 hop（遇人策略对齐用）
DWORD CooldownRemainingMs();

void Tick(DWORD now);

}  // namespace x::features::channel_hop
