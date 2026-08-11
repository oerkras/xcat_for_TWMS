#pragma once

#include "service_proc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace xcat::ops {

struct OpsState {
    std::wstring repoRoot;
    std::string repoRootUtf8;

    std::wstring nodePath;
    std::wstring pythonPath;
    bool hasNode = false;
    bool hasPython = false;
    bool autoStartPending = true;
    bool autoRestart = true;

    ServiceProc twms;
    ServiceProc publish;
    std::mutex procMu;

    bool twmsHealthOk = false;
    bool twmsReadyOk = false;
    bool publishProbeOk = false;
    bool twmsWanted = false;
    bool publishWanted = false;
    std::string twmsHealthText;
    std::string publishProbeText;
    std::string twmsStatsText;
    std::string twmsRequestText;
    std::string twmsUptimeText;
    std::string diskFreeText;
    std::string serverVersionText;
    std::string latestClientVersionText;
    uint32_t latestClientBuildId = 0;
    std::string forcedClientVersionText;
    uint32_t forcedClientBuildId = 0;

    std::mutex statusMu;
    std::string statusMessage;

    std::atomic<bool> busy{false};
    std::atomic<bool> shuttingDown{false};
    std::thread worker;

    // Main panel tabs: 0=服务与日志, 1=连接列表
    int mainTab = 0;

    // Log viewer
    int logTab = 0;  // 0=twms, 1=publish, 2=helper, 3=access
    bool logAutoScroll = true;
    bool logScrollToBottom = false;  // one-shot after new lines (avoids every-frame SetScrollHereY flicker)
    ULONGLONG lastLogReadMs = 0;
    std::uint64_t logFileSize = 0;
    std::int64_t logFileMtime = 0;
    std::vector<std::string> logLines;
    std::string logPathUtf8;
    std::string logEmptyHint;
    char logFilter[64]{};
    bool logErrorsOnly = false;

    // Connected clients (from /twms/admin/clients)
    struct ConnectedClient {
        std::string ip;
        std::string geo;
        std::string geoStatus;
        std::string machine;
        std::string deviceId;
        std::string device;
        std::string mac;
        std::string token;
        std::string appVersion;
        std::string charName;
        std::string charJobName;
        std::string charMeso;  // 十进制字符串，避免大数精度问题
        int charLevel = 0;
        int charJob = 0;
        uint32_t mapId = 0;
        std::string mapName;
        int channelId = 0;  // UI ch.N，1-based；0=未知
        std::string lastKind;
        std::string lastSeenAt;
        int idleSec = 0;
        int hits = 0;
        int lastStatus = 0;
        int sameIpOnline = 1;
        int knownOnIp = 0;
        bool identified = false;
        bool banned = false;
        bool allowed = false;
        std::string lastDenyAt;
        std::string lastDenyReason;
        std::string lastDenyMatch;
        std::string lastAllowAt;
        std::string gate;  // probe_ok|lease|denied|policy_deny|lease_expired|no_allow|unknown
        int leaseRemainSec = 0;
        int leaseTtlHours = 64;
        // OPS 拉取日志命令状态（服务端队列）
        std::string logFetchId;
        std::string logFetchMode;    // light|full
        std::string logFetchStatus;  // queued|offered|acked
        // 指定设备强更
        std::string forceTargetId;
        std::string forceTargetStatus;
        uint32_t forceTargetBuildId = 0;
    };
    std::vector<ConnectedClient> clients;
    std::string clientsError;
    std::string clientsGeoProvider;
    int clientsCount = 0;
    int clientsTracked = 0;
    int clientsActiveSec = 90;
    bool clientsAutoRefresh = true;
    ULONGLONG lastClientsFetchMs = 0;
    char clientsFilter[96]{};       // 文本：IP/机名/MAC/TOKEN…
    char clientsGateFilter[24]{};   // 门禁 chip：probe_ok|lease|…|__stale__（与文本筛选 AND）
    bool forceOpenIpAlerts = false;
    bool clientsSortIdleFirst = true;  // 空闲少的排前（刚探活的在上）
    bool clientsGroupByToken = true;   // 同 TOKEN（用户唯一标识）合并为可折叠组
    bool clientsGroupByIp = true;      // 同 IP 折叠：TOKEN 内嵌套，或关闭 TOKEN 时顶层按 IP
    // 展开中的 TOKEN 组键（缺省折叠）
    std::set<std::string> clientsGroupExpanded;
    // 展开中的 IP 组键：顶层为 IP；嵌套为 "token\\x1fip"
    std::set<std::string> clientsIpExpanded;

    struct ForceTargetPending {
        std::string id;
        std::string status;
        std::string machine;
        std::string deviceId;
        std::string mac;
        std::string note;
        std::string at;
        uint32_t buildId = 0;
    };
    std::vector<ForceTargetPending> forceTargetQueue;
    std::string forceTargetQueueError;

    struct AccessDenyHit {
        std::string at;
        std::string ip;
        std::string machine;
        std::string deviceId;
        std::string mac;
        std::string token;
        std::string reason;
        std::string match;
        std::string mode;
    };
    std::vector<AccessDenyHit> recentDenies;

    struct IpMultiDeviceAlert {
        std::string ip;
        std::string geo;
        int deviceCount = 0;
        std::string summary;  // 简要机名/Id 列表
    };
    std::vector<IpMultiDeviceAlert> ipAlerts;
    int ipAlertCount = 0;

    // Device access policy (from /twms/admin/bans|access)
    struct BannedDevice {
        std::string key;
        std::string machine;
        std::string deviceId;
        std::string mac;
        std::string token;
        std::string device;
        std::string reason;
        std::string bannedAt;
    };
    std::vector<BannedDevice> bans;
    std::vector<BannedDevice> allows;
    std::string bansError;
    int bansCount = 0;
    int allowsCount = 0;
    // deny=黑名单（默认）；allow=仅白名单
    std::string accessMode = "deny";
    ULONGLONG lastBansFetchMs = 0;
    char banMachineInput[96]{};
    char banDeviceIdInput[80]{};
    char banMacInput[40]{};
    char banPassInput[56]{};
    char banReasonInput[160]{};
    char allowMachineInput[96]{};
    char allowDeviceIdInput[80]{};
    char allowMacInput[40]{};
    char allowPassInput[56]{};
    char allowReasonInput[160]{};

    ULONGLONG lastProbeMs = 0;
    ULONGLONG lastDiskMs = 0;
    ULONGLONG lastReleaseReadMs = 0;
    ULONGLONG twmsDownSinceMs = 0;
    ULONGLONG publishDownSinceMs = 0;
    ULONGLONG twmsHealthySinceMs = 0;
    ULONGLONG publishHealthySinceMs = 0;
    ULONGLONG twmsBackoffMs = 8000;
    ULONGLONG publishBackoffMs = 8000;
    int twmsRestartCount = 0;
    int publishRestartCount = 0;
};

void OpsState_Init(OpsState& st);
void OpsState_Shutdown(OpsState& st);
void OpsState_Tick(OpsState& st);
void OpsPanel_Draw(OpsState& st);

void Ops_RequestStartAll(OpsState& st);
void Ops_RequestStartTwms(OpsState& st);
void Ops_RequestStartPublish(OpsState& st);
void Ops_RequestStopAll(OpsState& st);
void Ops_RequestStopTwms(OpsState& st);
void Ops_RequestStopPublish(OpsState& st);
void Ops_RequestRestartTwms(OpsState& st);
void Ops_RequestRestartPublish(OpsState& st);
void Ops_RequestSyncPublish(OpsState& st);
void Ops_RequestForceClientUpdate(OpsState& st);
void Ops_RequestClearForceClientUpdate(OpsState& st);

}  // namespace xcat::ops
