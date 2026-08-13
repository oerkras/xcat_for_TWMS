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
// 上次成功换频 / 图内读到的频道（UI 口径 ch.N，1-based）；未知返回 0。
// soft_login RequestRestart 前同步 sticky，避免遇人换频后仍粘进图旧频。
int LastKnownChannel1Based();

// auto_enter 进图 Done：用选中频（1-based）写 known，并重置 WM 前进基线。
// BIN 02:12：soft 进图后 ObserveWm 把 +0x6C 瞬态当成 wm6c_adv → sticky N→N+1。
void SyncKnownAfterEnter(int channelId1Based, const char* why);

// 遇人策略即将换频：把当前频记入「本图短期软拉黑」（落地仍有人 → 优先别再抽回）。
// 手动 F10 不调用；TTL 内重复标记会刷新计时。
void NoteCrowdedChannel();
// 换图清空软拉黑（mapId<=0 仅清；同图 no-op）。
void OnMapChanged(int mapId);

void Tick(DWORD now);

}  // namespace x::features::channel_hop
