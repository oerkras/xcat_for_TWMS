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
// 同轮询会先查 /update/access.json：
// - 服务器可达：以服务端判决为准（拒绝→粘性落盘；放行→清粘性并续在线租约）
// - 服务器不可达：有粘性拒绝则继续拦；无粘性则看本地在线租约（过期则退出）
// 家用间歇运维：客户端须在租约窗口内成功探活过；运维机可大部分时间关机，
// 封禁时开机等在线客户端命中即可；长期掐运维服/断网会在租约过期后无法继续用。
void           UpdateForcePollTick(const std::string& serviceUrl, const std::string& payloadBinDir);
// 原子：若自动安装已就绪则切入 Installing 并后台装包。
bool           TryStartAutoInstall(const std::string& installDir);
// 后台结束游戏并拉起安装脚本（不阻塞 UI 线程）；成功后由主循环 ExitProcess。
bool           StartInstallDownloadedUpdate(const std::string& installDir);
// 安装脚本已拉起：主循环应 ExitProcess，释放目录锁给换包脚本。
bool           ConsumeUpdateProcessExitRequest();
// 运维门禁退出（弹窗对用户统一「网络错误」；内部用 kind/退出码区分）。
// AccessDeny → 进程退出码 2；OnlineLease（连不上/租约失效）→ 3。
// 弹窗前会结束 Maplestory_Classic.exe，避免只退启动器、注入 DLL 仍可用。
enum class AccessGateExitKind : int {
    None = 0,
    AccessDeny = 2,
    OnlineLease = 3,
};
// 消费门禁退出（须主线程）：尚无主窗时同步弹「网络错误 (2|3)」；有主窗时优先走
// PostMessage(WM_XCAT_ACCESS_GATE)，本函数仅作兜底。
AccessGateExitKind ConsumeAccessGateExitRequest();
inline int AccessGateExitCode(AccessGateExitKind kind) {
    return kind == AccessGateExitKind::None ? 0 : static_cast<int>(kind);
}
// 主窗创建后登记：之后 gate 退出经 PostMessage(WM_XCAT_ACCESS_GATE) 在 WndProc 弹窗。
void           SetAccessGateUiHwnd(void* hwnd /* HWND */);
// WndProc 调用：弹窗并返回退出码（调用方 ExitProcess）。
int            HandleAccessGateUiMessage(unsigned long long wParam);
// 标题栏关窗：本会话 AccessDeny / 门禁 pending / 本机粘性 → 也杀游戏。
bool           ShouldKillGameOnLauncherClose(const std::string& payloadBinDir);
void           StopGameForAccessGateExit();
// 启动时若存在本机粘性拒绝：先探运维 access。
// 远端已放行（解禁）→清粘性并续约，返回 false；仍拒绝 / 探活不可达 → gate/2，返回 true。
bool           EnforceStickyDeviceAccessOnStartup(const std::string& serviceUrl,
                                                  const std::string& payloadBinDir);
// 启动在线租约门禁：租约有效则放行（并后台立刻探活，deny 则请求退出）；
// 否则同步探 /update/access.json。放行并续约→返回 false；拒绝/探活失败→返回 true；
// 探活失败时：若从未用过首次宽限则发 1h 临时租约并放行，否则请求 gate/3。
// 启动先 Quick 再（仅不可达时）补完整探活；宽限先写租约再打标，读租约取双副本 max(until)。
bool           EnforceOnlineLeaseGateOnStartup(const std::string& serviceUrl,
                                              const std::string& payloadBinDir);
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
