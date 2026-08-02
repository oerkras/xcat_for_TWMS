#pragma once
// Classic TWMS — UserPool read port（同图远程玩家数）.
// 真源：dump UserPool / GetRemoteUserCount；禁止用 CCU / 选角 AvatarCount.

#include <cstdint>

namespace x::features::ports::user_pool {

// Resolve Singleton + 读远程人数。成功返回 true；*outCount = GetRemoteUser 字典人数（不含本地）.
bool SampleRemoteUserCount(int* outCount);

void* PeekUserPool();

}  // namespace x::features::ports::user_pool
