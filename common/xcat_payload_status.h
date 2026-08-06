#pragma once

// TWMS 载荷状态：Local\XCatPayloadStatus_<hash> SHM。
// v1：仅 CCU；v2：顶栏 5 灯；v3：守护 exp；v4：playReady/wmAlive/sceneState + 人类可读图名；
// v5：sellbag 一键卖状态（对照枫星字段名）；
// v6：kick_sniff 断线边沿（守护干净重拉）；
// v7：引擎帧率锁读回（frame_lock）。

#include <Windows.h>

#include <cstdint>
#include <string>

namespace xcat {

constexpr uint32_t kPayloadStatusMagic = 0x58435450u;  // 'XCTP'
constexpr uint32_t kPayloadStatusVersion = 7u;

// hangup_schedule / guardian_policy hardFailCode：服务器踢线/断线（TWMS 本地码）。
constexpr uint32_t kHardFailServerKick = 1001u;

#pragma pack(push, 1)
struct PayloadStatus {
    uint32_t magic = kPayloadStatusMagic;
    uint32_t version = kPayloadStatusVersion;
    uint64_t writeTickMs = 0;
    long long worldChannelOnline = -1;  // -1 = 尚未采到
    int32_t worldChannelCount = 0;
    uint32_t worldChannelAgeSec = 0;  // 写侧可填 0；读侧可按 writeTickMs 重算

    // v2：顶栏 5 灯（对齐枫星字段名；经典版采样源不同）
    uint32_t ipcHandshake = 0;     // ① payload 存活心跳
    uint32_t gameContextOk = 0;    // ② WorldManager / GameAssembly 可读
    uint32_t localPlayerOk = 0;    // ③ MyUser / LocalCharacterStat 可读
    uint32_t mapId = 0;            // ④ 当前地图 Id；0=未知/未进图
    uint32_t quizCacheRootOk = 0;  // ⑤ 测谎 TypeResolve（Util+TextCaptcha+NonFinite）
    char currentMapName[128]{};    // 人类可读：街道名·地图名；空=未知/未进图

    // v3：守护模式（无经验重拉）所需进度
    uint64_t playerExp = 0;  // CharacterStat.exp；playerExpValid=0 时不可用于判定
    uint32_t playerExpValid = 0;

    // v4：MAP 灯显式门控 + 灭灯 tip 场景短标
    uint32_t playReady = 0;   // IsPlayReady（地图场景 ∧ WM 存活）
    uint32_t wmAlive = 0;     // WorldManager CharacterData/Stat 可读
    int32_t sceneState = -1;  // ports::world::SceneState；-1=Unknown

    // v5：sellbag（对照枫星 PayloadStatus.sellbag*）
    uint32_t sellbagBusy = 0;
    uint32_t sellbagState = 0;  // 0=idle 1=selling 2=done 3=error
    uint32_t sellbagLastBagMask = 0;
    uint32_t sellbagEquipSold = 0;
    uint32_t sellbagEtcSold = 0;
    uint32_t sellbagKept = 0;
    uint32_t sellbagFailed = 0;
    uint64_t sellbagLastRunTickMs = 0;
    char     sellbagMessage[128]{};

    // v6：kick_sniff 断线边沿（launcher 守护干净重拉）
    uint32_t disconnectSeq = 0;       // 每次 Disconnecting/Disconnected +1
    int32_t sessionState = -1;        // 0..3；-1=未知
    int32_t pendingErrorCode = -1;    // Session._pendingErrorCode；-1=未知
    uint32_t sawDisconnect = 0;       // sticky：本会话曾见断线边沿

    // v7：引擎帧率锁（Application.get_targetFrameRate 读回；非显示器 Hz）
    uint32_t frameLockOn = 0;         // 功能开关（desired）
    uint32_t frameLockWant = 0;       // 目标 fps（clamp 后）
    int32_t frameLockReadback = -1;   // 最近一次引擎读回；-1=尚未采到/失败
};
#pragma pack(pop)

void PayloadStatusSetDefaults(PayloadStatus& out);

bool ReadPayloadStatus(const char* binDir, PayloadStatus& out);
bool WritePayloadStatus(const char* binDir, const PayloadStatus& status);

// 读侧新鲜度（CCU 用）：须有有效在线人数且 writeTickMs 未过期。
bool PayloadStatusFresh(const PayloadStatus& st, uint64_t nowMs, uint64_t maxAgeMs = 120000);

// 读侧新鲜度（顶栏灯用）：只看 writeTickMs，不要求已 latch CCU。
bool PayloadStatusHeartbeatFresh(const PayloadStatus& st, uint64_t nowMs,
                                 uint64_t maxAgeMs = 5000);

}  // namespace xcat
