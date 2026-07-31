#pragma once

#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xcat::log {

enum class Component {
    Launcher,
    Payload,
    Inject,
    App,
};

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Ok,
};

using LineCallback = std::function<void(const char* line)>;

struct RotationPolicy {
    bool   enabled = true;
    size_t maxFileBytes = 2 * 1024 * 1024;  // 单文件上限，默认 2 MiB
    size_t maxBackups = 20;                 // 保留 .1 .. .N；payload 在 LogInit 另配更大备份数
};

struct Options {
    Component       component = Component::Launcher;
    // 唯一落盘路径（JSONL）。GUI/调试回调仍推送可读文本行，但不另写 .log。
    std::string       filePath;
    std::string       sessionTag;
    bool              debugOutput = true;
    // 0=每行立即 fflush；>0=普通日志在后续写入时按该间隔/缓冲阈值批量落盘。
    // Warn/Error/Ok 与关闭/轮转始终立即落盘。
    uint32_t          flushIntervalMs = 0;
    LineCallback      onLine;
    RotationPolicy    rotation{};
};

namespace paths {

std::string LauncherLog(const char* exeBinDir);
// payloadBinDir 为 XCat_data/；日志写在 payload 侧 bin/XCat_data/logs/x.jsonl
std::string PayloadLog(const char* payloadBinDir);
std::string InjectLog(const char* exeBinDir);

// 归一到 JSONL：foo.log → foo.jsonl；已是 .jsonl 则原样返回；其它后缀则追加 .jsonl
std::string Jsonl(const std::string& path);
// 归一到旧文本日志路径（仅供上传/兼容捡遗留文件）：foo.jsonl → foo.log
std::string TextLog(const std::string& path);

}  // namespace paths

const char* LevelLabel(Level level);
const char* ComponentLabel(Component component);

void Configure(const Options& opts);
void RegisterAuxFile(Component component, const std::string& filePath);
void Shutdown();
void SetLineCallback(LineCallback cb);
void SetSessionTag(const char* tag);
void BeginSession(const char* bannerLabel);

void WriteV(Level level, const char* tag, const char* fmt, va_list ap);
void WriteAuxV(Component component, Level level, const char* tag, const char* fmt, va_list ap);
void Write(Level level, const char* tag, const char* fmt, ...);
void WriteAux(Component component, Level level, const char* tag, const char* fmt, ...);

void Trace(const char* tag, const char* fmt, ...);
void Debug(const char* tag, const char* fmt, ...);
void Info(const char* tag, const char* fmt, ...);
void Warn(const char* tag, const char* fmt, ...);
void Error(const char* tag, const char* fmt, ...);
void Ok(const char* tag, const char* fmt, ...);

std::vector<std::string> ReadTailLines(const char* path, size_t maxLines,
                                       size_t readBytes = 65536);

}  // namespace xcat::log
