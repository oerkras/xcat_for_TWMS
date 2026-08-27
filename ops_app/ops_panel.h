#pragma once

#include "service_proc.h"

#include <atomic>
#include <cstdint>
#include <deque>
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
    uint32_t latestClientBuildId = 0;  // artifacts/release/latest.json = 最新构建
    std::string forcedClientVersionText;
    uint32_t forcedClientBuildId = 0;
    // 对外允许更新通道（/twms/admin/update-channels）；与最新构建拆开。
    struct UpdatePackageRow {
        uint32_t buildId = 0;
        std::string version;
        std::string zipName;
    };
    struct UpdateChannelOverride {
        std::string uid;
        std::string token;
        uint32_t buildId = 0;
        std::string version;
    };
    bool updateChannelsConfigured = false;
    uint32_t allowedClientBuildId = 0;
    std::string allowedClientVersionText;
    uint32_t lastBuiltClientBuildId = 0;
    std::string lastBuiltClientVersionText;
    std::vector<UpdatePackageRow> updatePackages;
    std::vector<UpdateChannelOverride> updateChannelGroups;
    std::vector<UpdateChannelOverride> updateChannelTokens;
    std::string updateChannelsError;
    ULONGLONG lastUpdateChannelsFetchMs = 0;
    int updateChannelCombo = -1;
    uint32_t updateChannelPendingBuildId = 0;

    std::mutex statusMu;
    std::string statusMessage;

    std::atomic<bool> busy{false};
    std::atomic<bool> shuttingDown{false};
    std::thread worker;

    // Main panel tabs: 0=服务与日志, 1=连接与访问, 2=利润监控, 3=台数配额, 4=签卡
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
        std::string uid;  // 签名 TOKEN 里的 uid（服务端验签得来，不可伪造）
        long long gateExp = 0;  // 卡到期（unix 秒，0=永不过期；服务端验签透出）
        std::string appVersion;
        std::string charName;
        std::string charJobName;
        std::string charMeso;  // 十进制字符串，避免大数精度问题
        std::string wealthScrolls;  // ASCII id:qty,id:qty
        bool hasWealthScrolls = false;
        bool hasRates = false;
        long long expPerMin = 0;
        long long mesoPerMin = 0;
        int charLevel = 0;
        int charJob = 0;
        uint32_t mapId = 0;
        std::string mapName;
        int channelId = 0;  // UI ch.N，1-based；0=未知
        int worldId = 0;    // 登录闩分区；0=未知
        std::string worldName;
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
    bool clientsStrictToken = false;  // 服务端严格模式：无有效签名 TOKEN 直接拒（本地镜像，供开关回显）
    ULONGLONG lastClientsFetchMs = 0;
    char clientsFilter[96]{};       // 文本：IP/机名/MAC/TOKEN…
    char clientsGateFilter[24]{};   // 门禁 chip：probe_ok|lease|…|__stale__（与文本筛选 AND）
    bool forceOpenIpAlerts = false;
    bool clientsSortIdleFirst = true;  // 空闲少的排前（刚探活的在上）
    bool clientsGroupByToken = true;   // 同人折叠：签卡 uid 优先，旧调试 TOKEN 兜底
    bool clientsGroupByIp = true;      // 同 IP 折叠：TOKEN 内嵌套，或关闭 TOKEN 时顶层按 IP
    // 展开中的 TOKEN 组键（缺省折叠）
    std::set<std::string> clientsGroupExpanded;
    // 展开中的 IP 组键：顶层为 IP；嵌套为 "token\\x1fip"
    std::set<std::string> clientsIpExpanded;

    // 背包金 / 利润监控：按人（uid > 旧 TOKEN）折线 + 角色流水（防转移）
    struct MesoDashPoint {
        ULONGLONG wallMs = 0;
        unsigned long long meso = 0;
    };
    struct MesoDashSeries {
        std::string token;  // 分组键：签卡 uid 优先，否则旧调试 TOKEN
        std::string uid;    // 非空=已签卡（与 token 相同）；空=仍按旧 TOKEN
        std::string chars;
        int sessions = 0;
        bool online = false;
        bool visible = true;
        unsigned long long lastMeso = 0;
        unsigned long long dartQty = 0;  // 名下角色雷之鏢合计（底账）
        bool hasScroll = false;          // 名下有 204/234
        std::string wealthText;          // 名单「高价值」列（短）；空=无
        std::string wealthTip;           // 悬停竖排全量
        ULONGLONG lastAlertMs = 0;
        std::deque<MesoDashPoint> points;
    };
    struct MesoUnit {
        std::string key;
        std::string token;  // 旧调试 TOKEN（可空）
        std::string uid;    // 签卡 uid（可空）
        std::string charName;
        std::string deviceId;
        std::string machine;
        unsigned long long lastMeso = 0;
        std::string lastScrolls;
        ULONGLONG lastSeenMs = 0;
        bool online = false;
        bool sampled = false;
        bool scrollsSampled = false;
    };
    struct MesoEvent {
        ULONGLONG wallMs = 0;
        std::string kind;  // outflow|token_xfer|char_move|reconnect_drop|inflow|scroll_*
        std::string token;
        std::string charName;
        std::string peerToken;
        std::string peerChar;
        unsigned long long before = 0;
        unsigned long long after = 0;
        unsigned long long mag = 0;
        std::string note;
        int itemId = 0;  // 卷轴/雷之鏢；0=金币流水或旧记录
    };
    std::vector<MesoDashSeries> mesoDashSeries;
    std::deque<MesoDashPoint> mesoDashTotal;
    std::vector<MesoUnit> mesoUnits;
    std::deque<MesoEvent> mesoEvents;
    int mesoDashWindowMin = 30;  // 10 / 30 / 60 / 180 / 1440 / 10080
    char mesoDashFilter[64]{};
    bool mesoDashOnlyDart = false;    // 名单仅有雷之鏢
    bool mesoDashOnlyScroll = false;  // 名单仅有卷轴；与有鏢同时开=并集
    bool mesoDashShowTotal = true;
    bool mesoDashShowOffline = true;
    bool mesoDashYFromZero = true;
    int mesoDashNoToken = 0;
    int mesoDashHoverSeries = -1;  // legend 悬停高亮；-1 无
    unsigned long long mesoAlertMin = 100000;  // 默认 10 万：低于此不记流水
    int mesoEventView = 0;  // 0=异常 1=全部 2=进账 3=高价值
    float mesoUiPlotH = 0.f;     // 折线区高度；0=按窗口推
    float mesoUiLegendW = 0.f;   // 右侧名单宽度；0=按窗口推

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

    // 客户端历史台账（/twms/admin/client-history）：落盘、跨重启保留。
    // 在线表只看得到「此刻在线」，这里才看得到「谁两天没上线、快掉 64h 租约了」。
    struct ClientHistoryRow {
        std::string ip;
        std::string machine;
        std::string deviceId;
        std::string uid;
        std::string appVersion;
        std::string charName;
        std::string lastSeenAt;
        std::string lastAllowAt;
        std::string lastDenyReason;
        std::string lastDenyMatch;
        long long lastSeenSec = 0;    // 距上次见到多久
        long long leaseRemainSec = 0; // 0=已过期/从未放行
        bool online = false;
    };
    std::vector<ClientHistoryRow> clientHistory;
    std::string clientHistoryError;
    int clientHistoryTotal = 0;
    ULONGLONG lastClientHistoryFetchMs = 0;
    int clientHistoryDays = 30;
    bool clientHistoryOnlyRisk = true;  // 只看租约快过期的（默认开，这是本表主要用途）

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
    // 卡级吊销名单（access 快照的 revokedJti）：只废单张卡，与 bans（封人/封设备）分开。
    std::set<std::string> revokedJti;
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

    // Device quota (from /twms/admin/quota) —— gate/1 个人签名 TOKEN 按 uid 台数配额
    struct QuotaDeviceRow {
        std::string deviceId;
        std::string lastSeen;
        long long idleSec = -1;  // 服务端算的「多久没见」；-1=时间戳无法解析
    };
    struct QuotaUserRow {
        std::string uid;
        int max = 0;           // 显式上限；0=未设（回落 defaultMax）
        int effectiveMax = 0;  // 生效上限；0=不限
        int used = 0;
        std::vector<QuotaDeviceRow> devices;
        char maxInput[12]{};   // 每行编辑框缓冲
    };
    std::vector<QuotaUserRow> quotaUsers;
    bool quotaEnabled = false;   // 服务端有公钥才启用
    int quotaDefaultMax = 0;     // 0=默认不限
    int quotaAgingDays = 0;      // 0=不老化释放
    std::string quotaError;
    std::string quotaPathText;
    ULONGLONG lastQuotaFetchMs = 0;
    char quotaFilter[64]{};
    char quotaNewUidInput[64]{};
    // 默认预填 100：测试期要的是「宽松但有天花板」，留空才是真不限（0 与留空等价）。
    // 卡是纯 bearer token，上限为 0 时一张卡外流可以无声扩散到任意台数。
    char quotaNewMaxInput[12]{"100"};
    std::set<std::string> quotaExpanded;  // 展开设备明细的 uid
    // 清僵尸名额：重装系统会换 deviceId，旧名额永久占位（agingDays 默认 0 不自动回收）
    char quotaIdleDaysInput[8]{"30"};  // 判定僵尸的闲置天数阈值
    bool quotaReleaseConfirmOpen = false;
    std::string quotaReleaseConfirmUid;  // 空=全部用户
    int quotaReleaseConfirmDays = 0;
    int quotaReleaseConfirmCount = 0;

    // 签卡：gate/1 启动 TOKEN 在线签发（/twms/admin/gate-sign，本机私钥离线签）
    char gateSignUidInput[64]{};
    char gateSignDaysInput[8]{};
    char gateSignQuotaInput[8]{"100"};  // 签卡即配额：台数上限，默认 100（空=不动该 uid 现有上限）
    char gateSignNoteInput[80]{};  // 签卡备注（落台账）
    std::string gateSignToken;  // 最近签发的整张 TOKEN（发给成员启动时粘贴）
    std::string gateSignInfo;   // "uid=… · 到期 …"
    std::string gateSignError;

    // 签发台账（/twms/admin/cards）—— 发过哪些卡、给谁、到期、是否已吊销
    struct GateCardRow {
        std::string id;
        // 卡号：签进 TOKEN payload 的 jti，可按它单独废这一张卡。
        // 空 = 该行签发于卡号机制上线前（老台账行），只能按 uid 封整个人。
        std::string jti;
        std::string uid;
        long long iss = 0;
        long long exp = 0;  // 0=永不过期
        int days = 0;
        std::string note;
        std::string by;
        std::string at;        // ISO 签发时间
        std::string token;     // 签发的完整 TOKEN（老台账条目可能为空）
        bool banned = false;   // 交叉 access bans：该 uid 是否已吊销
        // 同 uid 已有更晚签发的卡（续签/重签会追加新行）。旧卡在有效期内仍可用，
        // 但发给成员应发最新那张，故默认折叠并禁掉复制入口，避免发错过期卡。
        bool superseded = false;
        // 这一张卡被单独废掉（access.revokedJti 命中 id/jti）。区别于 banned（整个 uid 被封）。
        bool cardRevoked = false;
        char renewInput[8]{};  // 每行续期天数编辑框
    };
    std::vector<GateCardRow> gateCards;
    std::string gateCardsError;
    ULONGLONG lastGateCardsFetchMs = 0;
    char gateCardsFilter[64]{};
    bool gateCardsShowHistory = false;  // 默认每 uid 只显示最新一张

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
