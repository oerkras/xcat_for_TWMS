#pragma once

#include "service_proc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
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

    // Connected clients (from /twms/admin/clients)
    struct ConnectedClient {
        std::string ip;
        std::string geo;
        std::string machine;
        std::string device;
        std::string appVersion;
        std::string lastKind;
        std::string lastSeenAt;
        int idleSec = 0;
        int hits = 0;
        int lastStatus = 0;
        int sameIpOnline = 1;
        int knownOnIp = 0;
        bool identified = false;
    };
    std::vector<ConnectedClient> clients;
    std::string clientsError;
    std::string clientsGeoProvider;
    int clientsCount = 0;
    int clientsTracked = 0;
    int clientsActiveSec = 90;
    bool clientsAutoRefresh = true;
    ULONGLONG lastClientsFetchMs = 0;

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

}  // namespace xcat::ops
