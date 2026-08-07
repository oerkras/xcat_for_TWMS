#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "ccu_status.h"

namespace x {
namespace features {
namespace ccu {

void Ccu_Init();
void Ccu_Shutdown();
void Ccu_Tick(DWORD now);
CcuStatus Ccu_GetStatus();
int32_t Ccu_SnapshotWorldId();  // 已 latch 的分区 id；0=无
bool Ccu_ShouldSkipFeed(int32_t worldId);  // 单锁：同区/无 id 则跳过喂数
bool Ccu_NotifySnapshot(long long sum, int channelCount, const char* src, int32_t worldId);
void Ccu_NotifyFillTable(const ChannelFillRow* rows, int n, const char* src, int32_t worldId);
ChannelPickHint Ccu_GetChannelPickHint(int zeroBasedIdx);
void Ccu_MarkChannelRejected(int zeroBasedIdx);

}  // namespace ccu
}  // namespace features
}  // namespace x
