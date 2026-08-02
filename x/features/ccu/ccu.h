#pragma once
#include <Windows.h>
#include <cstdint>

#include "ccu_status.h"

namespace x {
namespace features {
namespace ccu {

// TWMS Classic：只认 auto_enter PickLeast 喂数（本进程一次）；无登录页被动 Probe。
void Init();
void Shutdown();
void StartWorker();
void StopWorker();

CcuStatus GetCcuStatus();
void NotifyWorldChannelSnapshot(long long sum, int channelCount, const char* src);
// 登录选频时每频人数表（与总和快照同次喂入；供 channel_hop 优先未满）
void NotifyChannelFillTable(const ChannelFillRow* rows, int n, const char* src);
ChannelPickHint GetChannelPickHint(int zeroBasedIdx);
void MarkChannelRejected(int zeroBasedIdx);

}  // namespace ccu
}  // namespace features
}  // namespace x
