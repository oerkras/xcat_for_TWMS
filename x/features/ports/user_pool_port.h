#pragma once
// Classic TWMS — UserPool read port（同图远程玩家数 / 枚举）.
// 真源：dump UserPool / GetRemoteUserCount；禁止用 CCU / 选角 AvatarCount.

#include <cstdint>

namespace x::features::ports::user_pool {

// 同图威胁采样（遇人策略 / GM·隐身升级用）。
struct RemoteThreatSample {
    int remoteCount = 0;       // UserPool 远程人数
    int adminLikeCount = 0;    // JobCategory 8/9（Manager/Admin；job%1000/100）
    int hideSuspectCount = 0;  // avatarRoot 存在但 activeSelf=false（调用方应在未开「藏人」时才采）
    uint16_t sampleAdminJob = 0;  // 首个 admin-like 的 JobCode（日志用）
};

// Resolve Singleton + 读远程人数。成功返回 true；*outCount = GetRemoteUser 字典人数（不含本地）.
bool SampleRemoteUserCount(int* outCount);

// 一次枚举：人数 + Admin/Manager 职业 +（可选）客户端隐身嫌疑。
// checkAvatarHide=false 时跳过 activeSelf（「隐藏同图其他玩家」开启时必须关，否则全员误报）.
bool SampleRemoteThreat(RemoteThreatSample* out, bool checkAvatarHide);

// 枚举同图远程玩家对象指针（不含 UserLocal）。须在主线程调用（或经 InvokeAndWait）。
// out/cap：写入上限；*outCount 为实际写入数。成功返回 true（无人时 outCount=0 仍算成功）.
bool EnumRemoteUsers(void** out, int cap, int* outCount);

// 本地 UserLocal（pool+0x10）；未绑定返回 nullptr。须在主线程/已 Resolve 后读。
void* PeekUserLocal();

void* PeekUserPool();

}  // namespace x::features::ports::user_pool
