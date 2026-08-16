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
// 上次成功换频 / 图内读到的频道（玩家 UI ch.N = 列表 id + 1）；未知返回 0。
int LastKnownChannel1Based();
// 给人看的 ch.N：sticky 列表 id + 1，否则 LastKnown；未知 0。禁止把 0 画成频道。
int DisplayChannel1Based();

// auto_enter 进图 Done：参数是 SelectChannel / WM+0x6C 列表 id（不是 UI ch.N）。
// 内部 known 与该 id 对齐；UI = id+1。BIN 08-15：id=39 → 頻道 40。
void SyncKnownAfterEnter(int channelId1Based, const char* why);

// 遇人策略即将换频：把当前频记入「本图短期软拉黑」（落地仍有人 → 优先别再抽回）。
// 手动 F10 不调用；TTL 内重复标记会刷新计时。
void NoteCrowdedChannel();
// 换图清空软拉黑（mapId<=0 仅清；同图 no-op）。
void OnMapChanged(int mapId);

void Tick(DWORD now);

}  // namespace x::features::channel_hop
