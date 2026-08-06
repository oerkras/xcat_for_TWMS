#pragma once
// Classic TWMS — UserPool read port（同图远程玩家数 / 枚举）.
// 真源：dump UserPool / GetRemoteUserCount；禁止用 CCU / 选角 AvatarCount.

#include <cstdint>

namespace x::features::ports::user_pool {

// Resolve Singleton + 读远程人数。成功返回 true；*outCount = GetRemoteUser 字典人数（不含本地）.
bool SampleRemoteUserCount(int* outCount);

// 枚举同图远程玩家对象指针（不含 UserLocal）。须在主线程调用（或经 InvokeAndWait）。
// out/cap：写入上限；*outCount 为实际写入数。成功返回 true（无人时 outCount=0 仍算成功）.
bool EnumRemoteUsers(void** out, int cap, int* outCount);

// 本地 UserLocal（pool+0x10）；未绑定返回 nullptr。须在主线程/已 Resolve 后读。
void* PeekUserLocal();

void* PeekUserPool();

}  // namespace x::features::ports::user_pool
