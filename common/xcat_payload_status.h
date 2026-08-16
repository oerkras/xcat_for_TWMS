#pragma once

// TWMS 载荷状态：Local\XCatPayloadStatus_<hash> SHM。
// v1：仅 CCU；v2：顶栏 5 灯；v3：守护 exp；v4：playReady/wmAlive/sceneState + 人类可读图名；
// v5：sellbag 一键卖状态（对照枫星字段名）；
// v6：kick_sniff 断线边沿（守护干净重拉）；
// v7：引擎帧率锁读回（frame_lock）；
// v8：soft_login 试连观察窗（抑制踢线干净重拉）；
// v9：角色快照（名/等级/职业/背包金 → launcher 探活头 → 运维台）；
// v10：当前频道（1-based ch.N）→ 探活头 → 运维台（地图 Id 已在 v2 mapId）。
// v11：原生 MapDataInfo.IsTown（守护主城无经验豁免；绕过拍卖强制写前的真值）。
// v12：高价值卷轴快照（消耗栏 204/234 → launcher 探活头 → 运维台；客户端无 UI）。
// v13：吸怪定时软重连倒计时 → ImGui 顶栏。

#include <Windows.h>

#include <cstdint>
#include <string>

namespace xcat {

constexpr uint32_t kPayloadStatusMagic = 0x58435450u;  // 'XCTP'
constexpr uint32_t kPayloadStatusVersion = 13u;

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

    // v8：soft_login_probe 试连观察窗（守护推迟踢线干净重拉）
    uint32_t softLoginHold = 0;       // 1=观察中，勿因 disconnectSeq 立刻重拉
    uint32_t softLoginResult = 0;     // 0=无 1=试连成功(Connected) 2=失败/超时

    // v9：角色快照（进图可读时填；未进角色 playerCharValid=0）
    uint32_t playerCharValid = 0;
    int32_t playerLevel = 0;
    int32_t playerJob = 0;
    int64_t playerMeso = 0;
    char playerName[64]{};
    char playerJobName[48]{};

    // v10：当前频道（UI ch.N，1-based；0=未知/未进图）。地图见 mapId。
    int32_t channelId = 0;

    // v11：原生 IsTown（拍卖绕过强制写 1 时仍报备份原值）。
    uint32_t mapIsTown = 0;       // 1=城镇（仅 map_info.town=1；非原生 IsTown / 非 %1000000）
    uint32_t mapIsTownValid = 0;  // 1=已采到 mapId；0=未知

    // v12：高价值卷轴（ASCII `id:qty,id:qty`；valid=0 表示本拍未采到，探活勿覆盖服务端旧值）
    uint32_t playerWealthScrollsValid = 0;
    char playerWealthScrolls[384]{};

    // v13：吸怪「X秒后触发软重连」墙钟（payload 写、顶栏读）
    uint32_t softReloginOn = 0;        // 勾选
    uint32_t softReloginPaused = 0;    // hold / 落地静默冻钟
    uint32_t softReloginRemainMs = 0;  // 剩余；0xFFFFFFFF=勾了未起表
    uint32_t softReloginNeedMs = 0;    // 间隔
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
