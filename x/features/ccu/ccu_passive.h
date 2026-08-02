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
void Ccu_NotifySnapshot(long long sum, int channelCount, const char* src);
void Ccu_NotifyFillTable(const ChannelFillRow* rows, int n, const char* src);
ChannelPickHint Ccu_GetChannelPickHint(int zeroBasedIdx);
void Ccu_MarkChannelRejected(int zeroBasedIdx);

}  // namespace ccu
}  // namespace features
}  // namespace x
