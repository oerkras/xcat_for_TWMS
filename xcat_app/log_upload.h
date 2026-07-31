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

// 轻量：旧窗口（约 20 卷）；全量：与 payload 轮转 maxBackups 对齐（360）。
enum class LogUploadMode {
    Light = 0,
    Full = 1,
};

constexpr size_t kLogUploadBackupsLight = 20;
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
    LogUploadMode mode = LogUploadMode::Light;
};

// 备注上限：Unicode 码点（与 scripts/log-upload-server.mjs sanitizeUploadNote 对齐）。
constexpr size_t kMaxUploadNoteCodePoints = 500;
// UTF-8 最坏 4 字节/码点；ImGui 输入缓冲用此容量。
constexpr size_t kMaxUploadNoteUtf8Bytes = kMaxUploadNoteCodePoints * 4;

// 去控制字符、压空白、trim、截到 kMaxUploadNoteCodePoints；可空。
std::string NormalizeUploadNote(std::string_view raw);

// 日志上传偏好：user.ini [log_upload]（url + uploadMode + 稳定 deviceId）；旧 xcat_log_upload.ini 只读 migrate。
// deviceId 用于区分同计算机名的多台虚拟机；默认 url 来自 kDefaultUpdateServiceUrl（/twms）。
// 加载时会把历史 /artale、/chuangshi、/fengxing 服务根自动重写为 /twms。
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

// 本机稳定身份：计算机名 + user.ini deviceId（同机多 VM 靠 deviceId 区分）。
struct ClientHostIdentity {
    std::string machine;
    std::string deviceId;
};
ClientHostIdentity ResolveClientHostIdentity(const std::string& payloadBinDir);

bool                              LogUploadConfigured(const LogUploadRequest& req);
bool                              LogUploadBusy();
bool                              StartLogUpload(LogUploadRequest req);
LogUploadSnapshot                 GetLogUploadSnapshot();
std::vector<LogUploadHistoryEntry> GetLogUploadHistory();

}  // namespace xcat::app
