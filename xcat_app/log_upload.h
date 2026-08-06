#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xcat::app {

enum class LogUploadPhase {
    Idle,
    Uploading,
    Succeeded,
    Failed,
};

// 轻量：各频道最近约 10 卷（含功能日志）；全量：最多 360 卷。
enum class LogUploadMode {
    Light = 0,
    Full = 1,
};

constexpr size_t kLogUploadBackupsLight = 10;
constexpr size_t kLogUploadBackupsFull = 360;

inline constexpr const char* LogUploadModeLabel(LogUploadMode mode) {
    return mode == LogUploadMode::Full ? "full" : "light";
}

inline size_t LogUploadBackupsForMode(LogUploadMode mode) {
    return mode == LogUploadMode::Full ? kLogUploadBackupsFull : kLogUploadBackupsLight;
}

struct LogUploadRequest {
    std::string url;
    std::string profileId;
    std::string exeBinDir;
    std::string payloadBinDir;
    // 可选备注：客户反馈 BUG 原因；空串合法，服务端写入 catalog.note（device 前）。
    std::string note;
    // 默认轻量；排障可选全量扫齐各频道现存卷。
    LogUploadMode mode = LogUploadMode::Light;
};

// 备注上限：Unicode 码点（与 scripts/log-upload-server.mjs sanitizeUploadNote 对齐）。
constexpr size_t kMaxUploadNoteCodePoints = 500;
// UTF-8 最坏 4 字节/码点；ImGui 输入缓冲用此容量。
constexpr size_t kMaxUploadNoteUtf8Bytes = kMaxUploadNoteCodePoints * 4;

// 去控制字符、压空白、trim、截到 kMaxUploadNoteCodePoints；可空。
std::string NormalizeUploadNote(std::string_view raw);

// 日志上传偏好：user.ini [log_upload]（url + uploadMode + 稳定 deviceId）；旧 xcat_log_upload.ini 只读 migrate。
// deviceId 用于区分同计算机名的多台虚拟机；默认 url 来自 kDefaultUpdateServiceUrl（http://xcat.work:18789/twms）。
// 加载时会把历史 /artale、/chuangshi、/fengxing 服务根与本机 127.0.0.1/localhost 自动重写为 /twms + xcat.work。
struct LogUploadPrefs {
    std::string url;
    LogUploadMode mode = LogUploadMode::Light;
};

struct LogUploadSnapshot {
    LogUploadPhase phase = LogUploadPhase::Idle;
    std::string    message;
    std::string    uploadId;
    uint32_t       httpStatus = 0;
};

struct LogUploadHistoryEntry {
    std::string timeText;
    std::string uploadId;
    std::string message;
    uint32_t    httpStatus = 0;
    uint32_t    files = 0;
};

LogUploadPrefs LoadLogUploadPrefs(const std::string& exeBinDir, const LogUploadPrefs& defaults);
void           SaveLogUploadPrefs(const std::string& exeBinDir, const LogUploadPrefs& prefs);

// 本机稳定身份：计算机名 + deviceId + 本机网卡 MAC（探活/封禁/白名单）。
// deviceId：优先 user.ini；ProgramData 按安装路径分片镜像（抗清目录、多目录不互抢）；
// 整段加锁 + 进程内缓存，防并发首调双 mint / 读失败乱跳。
// TOKEN：调试 TAB 字段，随探活头 X-XCat-Token 上报（对外不说明用途）。
struct ClientHostIdentity {
    std::string machine;
    std::string deviceId;
    // 规范化小写冒号分隔，如 aa:bb:cc:dd:ee:ff；主网卡在前。
    std::vector<std::string> macs;
    std::string token;
};
ClientHostIdentity ResolveClientHostIdentity(const std::string& payloadBinDir);

// 调试 TAB TOKEN：持久化到 user.ini [update] token（换包白名单保留）；空=清除。
constexpr size_t kOpsTokenMaxChars = 48;
std::string      NormalizeOpsToken(std::string_view raw);
std::string      LoadOpsToken(const std::string& payloadBinDir);
bool             SaveOpsToken(const std::string& payloadBinDir, std::string_view raw);

bool                              LogUploadConfigured(const LogUploadRequest& req);
bool                              LogUploadBusy();
bool                              StartLogUpload(LogUploadRequest req);
LogUploadSnapshot                 GetLogUploadSnapshot();
std::vector<LogUploadHistoryEntry> GetLogUploadHistory();

}  // namespace xcat::app
