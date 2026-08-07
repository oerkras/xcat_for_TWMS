#pragma once
#include <Windows.h>
#include <cstdint>

#include "ccu_status.h"

namespace x {
namespace features {
namespace ccu {

// TWMS Classic：登录频道页或 auto_enter 喂数；同分区一次，换分区可更新。
void Init();
void Shutdown();
void StartWorker();
void StopWorker();

CcuStatus GetCcuStatus();
bool HasSnapshot();
int32_t SnapshotWorldId();  // 当前展示对应的分区；0=尚未采到
bool ShouldSkipFeed(int32_t worldId);  // 已有快照且同区或 worldId==0
// 返回是否写入成功；FillTable 应仅在成功后调用。
bool NotifyWorldChannelSnapshot(long long sum, int channelCount, const char* src, int32_t worldId);
void NotifyChannelFillTable(const ChannelFillRow* rows, int n, const char* src, int32_t worldId);
ChannelPickHint GetChannelPickHint(int zeroBasedIdx);
void MarkChannelRejected(int zeroBasedIdx);

}  // namespace ccu
}  // namespace features
}  // namespace x
