#pragma once
// Classic TWMS — UserPool read port（同图远程玩家数 / 枚举）.
// 真源：dump UserPool / GetRemoteUserCount；禁止用 CCU / 选角 AvatarCount.

#include <cstdint>

namespace x::features::ports::user_pool {

// 同图远程角色名落盘上限（遇人日志 / 通知；超出只记人数）。
constexpr int kRemoteNameMax = 12;
constexpr int kRemoteNameLen = 40;  // 13 汉字 UTF-8 + NUL；读失败时用 "#id"

// 同图威胁采样（遇人策略 / GM·隐身升级用）。
struct RemoteThreatSample {
    int remoteCount = 0;       // UserPool 远程人数
    int adminLikeCount = 0;    // JobCategory 8/9（Manager/Admin；job%1000/100）
    int hideSuspectCount = 0;  // avatarRoot 存在但 activeSelf=false（调用方应在未开「藏人」时才采）
    uint16_t sampleAdminJob = 0;  // 首个 admin-like 的 JobCode（日志用）
    int nameCount = 0;         // names[] 有效条数（≤ remoteCount / kRemoteNameMax）
    char names[kRemoteNameMax][kRemoteNameLen]{};  // UTF-8 角色名（User.CharacterName@+0x1A0；失败为 #id）
};

// 读 User / UserRemote 角色名（CharacterName backing @+0x1A0；hash 防漂）。
// 成功写入 UTF-8 到 out；失败 out[0]=0 并返回 false。须在可读堆指针上调用。
bool ReadUserCharacterName(void* user, char* out, int outSz);

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

// 已绑定池上的纯内存快照。
// PeekRemoteUserCount：池未绑定时会 Resolve（可触发 class init）——只许泵上 / 已绑冷路径。
// PeekRemoteUserCountBound：绝不 Resolve；未绑返回 false，调用方 fail-open。
// 战斗 worker 热路径只能走 Bound，禁止走 Resolve 版。
bool PeekRemoteUserCount(int* outCount);
bool PeekRemoteUserCountBound(int* outCount);
bool PeekEnumRemoteUsers(void** out, int cap, int* outCount);

}  // namespace x::features::ports::user_pool
