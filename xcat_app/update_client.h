#pragma once

#include <cstdint>
#include <string>

namespace xcat::app {

enum class UpdatePhase {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    ReadyToInstall,
    Installing,
    Failed,
};

struct UpdateSnapshot {
    UpdatePhase phase = UpdatePhase::Idle;
    std::string message;
    std::string manifestUrl;
    std::string latestVersion;
    uint32_t    currentBuildId = 0;
    uint32_t    latestBuildId = 0;
    std::string zipName;
    std::string zipPath;
    // <0 不确定进度（忙碌条）；0..1 确定进度（下载字节）。
    float       progress = -1.f;
    uint64_t    downloadedBytes = 0;
    uint64_t    totalBytes = 0;
};

std::string    UpdateManifestUrlFromServiceUrl(const std::string& serviceUrl);
bool           UpdateBusy();
// 检查/下载/待装/安装：挡启动/退出/守护，并提示需显示进度。
bool           UpdateNeedsVisibleUi();
// 含「已是最新/失败」短时粘性展示，仅用于画进度条（不挡操作）。
bool           UpdateShouldDrawProgressUi();
// 自动接收运维强更：对用户始终开启（无 UI 开关）；Load 会把旧 ini autoReceive 迁成开启。
void           LoadAutoReceiveUpdates(const std::string& payloadBinDir);
bool           AutoReceiveUpdatesEnabled();  // 恒 true；保留供调用方语义查询
// 保留写接口供内部/兼容；忽略 enabled，只写开启。
bool           SetAutoReceiveUpdatesEnabled(const std::string& payloadBinDir, bool enabled);
bool           StartUpdateCheck(const std::string& serviceUrl);  // 检查→有更新则自动下载
bool           StartUpdateDownload();  // 仅下载；下载完成后由 UI 自动安装并重启
// 每 60 秒检查一次运维端发布的强制更新标记；仅对低于目标 build 的客户端生效。
void           UpdateForcePollTick(const std::string& serviceUrl, const std::string& payloadBinDir);
// 原子：若自动安装已就绪则切入 Installing 并后台装包。
bool           TryStartAutoInstall(const std::string& installDir);
// 后台结束游戏并拉起安装脚本（不阻塞 UI 线程）；成功后由主循环 ExitProcess。
bool           StartInstallDownloadedUpdate(const std::string& installDir);
// 安装脚本已拉起：主循环应 ExitProcess，释放目录锁给换包脚本。
bool           ConsumeUpdateProcessExitRequest();
UpdateSnapshot GetUpdateSnapshot();
// 更新脚本落盘的启动器冷启标记：存在则返回 true；必须在完整冷启成功后才清除。
bool           ConsumePostUpdateColdStartRequest(const std::string& payloadBinDir);
bool           ClearPostUpdateColdStartRequest(const std::string& payloadBinDir);
// 消费换包失败通知（state\\update_failed.notify，缺则 %TEMP%\\xcat_update_failed.notify）：
// 删文件并提示（一次性）。
bool           ConsumeUpdateFailedNotify(const std::string& payloadBinDir);

// TWMS 更新/上报默认基址（对齐枫星：公网域名 xcat.work；本机运维仍绑 0.0.0.0:18789）。
inline constexpr const char* kDefaultUpdateServiceUrl = "http://xcat.work:18789/twms";

}  // namespace xcat::app
