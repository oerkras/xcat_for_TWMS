#include "xcat_log.h"

#include "process_util.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace xcat::log {
namespace {

const char* kLevelLabels[] = {"TRC", "DBG", "I", "W", "E", "OK"};

struct FileSink {
    std::string    path;
    FILE*          file = nullptr;
    size_t         bytesWritten = 0;
    size_t         unflushedBytes = 0;
    DWORD          lastFlushMs = 0;
    RotationPolicy rotation{};
};

constexpr size_t kBufferedFlushBytes = 16 * 1024;

struct State {
    Options                       opts{};
    FileSink                      primary{};  // 唯一落盘：JSONL
    std::map<Component, FileSink> auxFiles{};
    std::mutex                    mtx;
};

State g_state{};

void EnsureLogDir(const std::string& filePath) {
    if (filePath.empty()) return;
    const size_t slash = filePath.find_last_of("\\/");
    if (slash == std::string::npos) return;
    CreateDirectoryUtf8(filePath.substr(0, slash));
}

void CloseSink(FileSink& sink) {
    if (sink.file) {
        fclose(sink.file);
        sink.file = nullptr;
    }
    sink.bytesWritten = 0;
    sink.unflushedBytes = 0;
    sink.lastFlushMs = 0;
}

bool RotateSinkFiles(const FileSink& sink) {
    if (sink.path.empty() || sink.rotation.maxBackups == 0) return false;

    const std::string& base = sink.path;
    DeleteFileUtf8(base + "." + std::to_string(sink.rotation.maxBackups));

    for (size_t i = sink.rotation.maxBackups; i > 1; --i) {
        MoveFileExUtf8(base + "." + std::to_string(i - 1),
                       base + "." + std::to_string(i),
                       MOVEFILE_REPLACE_EXISTING);
    }

    const std::string bak1 = base + ".1";
    // 优先 rename；若被其它进程占读（上传器/编辑器），Move 会失败 → 改 Copy 保备份。
    if (MoveFileExUtf8(base, bak1, MOVEFILE_REPLACE_EXISTING)) return true;

    DeleteFileUtf8(bak1);
    if (CopyFileUtf8(base, bak1, false)) {
        // 原文件仍在：调用方随后 wb 截断；.1 已是完整备份。
        return true;
    }

    char dbg[256]{};
    snprintf(dbg, sizeof(dbg), "[xcat_log] rotate FAIL path=%s err=%lu\n", base.c_str(),
             static_cast<unsigned long>(GetLastError()));
    OutputDebugStringA(dbg);
    return false;
}

void ReopenSinkAppend(FileSink& sink) {
    FopenUtf8(&sink.file, sink.path, L"ab");
    sink.bytesWritten = 0;
    sink.unflushedBytes = 0;
    sink.lastFlushMs = GetTickCount();
    if (!sink.file) return;
    if (fseek(sink.file, 0, SEEK_END) == 0) {
        const long pos = ftell(sink.file);
        sink.bytesWritten = pos > 0 ? static_cast<size_t>(pos) : 0;
    }
}

void OpenSink(FileSink& sink, const std::string& path, const RotationPolicy& rotation) {
    CloseSink(sink);
    sink.path = path;
    sink.rotation = rotation;
    if (path.empty()) return;

    EnsureLogDir(path);
    FopenUtf8(&sink.file, path, L"ab");
    if (!sink.file) return;
    sink.lastFlushMs = GetTickCount();

    if (fseek(sink.file, 0, SEEK_END) == 0) {
        const long pos = ftell(sink.file);
        sink.bytesWritten = pos > 0 ? static_cast<size_t>(pos) : 0;
    }

    if (sink.rotation.enabled && sink.rotation.maxFileBytes > 0 &&
        sink.bytesWritten >= sink.rotation.maxFileBytes) {
        CloseSink(sink);
        if (RotateSinkFiles(sink)) {
            FopenUtf8(&sink.file, path, L"wb");
            sink.bytesWritten = 0;
            sink.unflushedBytes = 0;
            sink.lastFlushMs = GetTickCount();
        } else {
            // 轮转失败绝不能丢文件：继续追加。
            ReopenSinkAppend(sink);
        }
    }
}

void MaybeRotateBeforeWrite(FileSink& sink, size_t incomingBytes) {
    if (!sink.file || !sink.rotation.enabled || sink.rotation.maxFileBytes == 0) return;
    if (sink.bytesWritten + incomingBytes <= sink.rotation.maxFileBytes) return;

    CloseSink(sink);
    if (RotateSinkFiles(sink)) {
        FopenUtf8(&sink.file, sink.path, L"wb");
        sink.bytesWritten = 0;
        sink.unflushedBytes = 0;
        sink.lastFlushMs = GetTickCount();
        return;
    }
    // 旧逻辑：Move 失败仍 wb → 无 .1 且整段日志被截断（SessionGate 刷屏时常见）。
    ReopenSinkAppend(sink);
    // 暂时抬高水位，避免同一行死循环重试轮转；下次超过 2× 上限再试。
    if (sink.file && sink.rotation.maxFileBytes > 0 &&
        sink.bytesWritten >= sink.rotation.maxFileBytes) {
        sink.bytesWritten = sink.rotation.maxFileBytes / 2;
    }
}

std::string FormatLine(Component component, Level level, const char* tag, const char* body) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char prefix[160]{};
    if (!g_state.opts.sessionTag.empty() && component == g_state.opts.component) {
        snprintf(prefix, sizeof(prefix), "[%s][%s][%s]", ComponentLabel(component),
                 g_state.opts.sessionTag.c_str(), tag ? tag : "?");
    } else {
        snprintf(prefix, sizeof(prefix), "[%s][%s]", ComponentLabel(component),
                 tag ? tag : "?");
    }

    char line[1400]{};
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s]%s %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             static_cast<unsigned>(st.wMilliseconds), LevelLabel(level), prefix, body);
    return line;
}

void WriteRawToSink(FileSink& sink, const std::string& text, bool forceFlush) {
    if (!sink.file) return;
    MaybeRotateBeforeWrite(sink, text.size());
    if (sink.file) {
        fputs(text.c_str(), sink.file);
        sink.bytesWritten += text.size();
        sink.unflushedBytes += text.size();

        const DWORD now = GetTickCount();
        const uint32_t intervalMs = g_state.opts.flushIntervalMs;
        const bool intervalDue =
            intervalMs == 0 || !sink.lastFlushMs ||
            now - sink.lastFlushMs >= intervalMs;
        if (forceFlush || intervalDue || sink.unflushedBytes >= kBufferedFlushBytes) {
            fflush(sink.file);
            sink.unflushedBytes = 0;
            sink.lastFlushMs = now;
        }
    }
}

void AppendJsonEscaped(std::string& out, const char* s) {
    if (!s) return;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8]{};
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);  // UTF-8 字节原样保留
            }
        }
    }
}

// 结构化一行：{"ts":"ISO","lvl":"I","comp":"Payload","tag":"...","sess":"...","msg":"..."}
std::string FormatJsonLine(Component component, Level level, const char* tag, const char* body) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[40]{};
    snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, static_cast<unsigned>(st.wMilliseconds));

    std::string out;
    out.reserve(256);
    out += "{\"ts\":\"";
    out += ts;
    out += "\",\"lvl\":\"";
    out += LevelLabel(level);
    out += "\",\"comp\":\"";
    out += ComponentLabel(component);
    out += "\",\"tag\":\"";
    AppendJsonEscaped(out, tag ? tag : "?");
    if (!g_state.opts.sessionTag.empty() && component == g_state.opts.component) {
        out += "\",\"sess\":\"";
        AppendJsonEscaped(out, g_state.opts.sessionTag.c_str());
    }
    out += "\",\"msg\":\"";
    AppendJsonEscaped(out, body);
    out += "\"}\n";
    return out;
}

}  // namespace

namespace paths {

std::string LauncherLog(const char* exeBinDir) {
    if (!exeBinDir || !exeBinDir[0]) return "logs\\launcher.jsonl";
    std::string p(exeBinDir);
    if (p.back() != '\\' && p.back() != '/') p += '\\';
    return p + "logs\\launcher.jsonl";
}

std::string PayloadLog(const char* payloadBinDir) {
    if (!payloadBinDir || !payloadBinDir[0]) return "logs\\x.jsonl";
    std::string p(payloadBinDir);
    if (p.back() != '\\' && p.back() != '/') p += '\\';
    return p + "logs\\x.jsonl";
}

std::string InjectLog(const char* exeBinDir) {
    if (!exeBinDir || !exeBinDir[0]) return "logs\\inject.jsonl";
    std::string p(exeBinDir);
    if (p.back() != '\\' && p.back() != '/') p += '\\';
    return p + "logs\\inject.jsonl";
}

std::string Jsonl(const std::string& path) {
    if (path.empty()) return {};
    const std::string jsonl = ".jsonl";
    if (path.size() >= jsonl.size() &&
        path.compare(path.size() - jsonl.size(), jsonl.size(), jsonl) == 0) {
        return path;
    }
    const std::string log = ".log";
    if (path.size() >= log.size() &&
        path.compare(path.size() - log.size(), log.size(), log) == 0) {
        return path.substr(0, path.size() - log.size()) + ".jsonl";
    }
    return path + ".jsonl";
}

std::string TextLog(const std::string& path) {
    if (path.empty()) return {};
    const std::string log = ".log";
    if (path.size() >= log.size() &&
        path.compare(path.size() - log.size(), log.size(), log) == 0) {
        return path;
    }
    const std::string jsonl = ".jsonl";
    if (path.size() >= jsonl.size() &&
        path.compare(path.size() - jsonl.size(), jsonl.size(), jsonl) == 0) {
        return path.substr(0, path.size() - jsonl.size()) + ".log";
    }
    return path + ".log";
}

}  // namespace paths

const char* LevelLabel(Level level) {
    const auto idx = static_cast<size_t>(level);
    if (idx >= sizeof(kLevelLabels) / sizeof(kLevelLabels[0])) return "?";
    return kLevelLabels[idx];
}

const char* ComponentLabel(Component component) {
    switch (component) {
    case Component::Launcher: return "Launcher";
    case Component::Payload: return "Payload";
    case Component::Inject: return "Inject";
    case Component::App: return "App";
    default: return "?";
    }
}

void Configure(const Options& opts) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    CloseSink(g_state.primary);
    g_state.opts = opts;
    if (!opts.filePath.empty()) {
        OpenSink(g_state.primary, opts.filePath, opts.rotation);
    }
}

void RegisterAuxFile(Component component, const std::string& filePath) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    auto it = g_state.auxFiles.find(component);
    if (it != g_state.auxFiles.end()) {
        CloseSink(it->second);
        g_state.auxFiles.erase(it);
    }
    if (filePath.empty()) return;
    FileSink sink{};
    OpenSink(sink, filePath, g_state.opts.rotation);
    g_state.auxFiles[component] = std::move(sink);
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    CloseSink(g_state.primary);
    for (auto& kv : g_state.auxFiles) CloseSink(kv.second);
    g_state.auxFiles.clear();
    g_state.opts = {};
}

void SetLineCallback(LineCallback cb) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.opts.onLine = std::move(cb);
}

void SetSessionTag(const char* tag) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.opts.sessionTag = tag ? tag : "";
}

void BeginSession(const char* bannerLabel) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[64]{};
    snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    char body[160]{};
    snprintf(body, sizeof(body), "=== xcat %s %s ===", bannerLabel ? bannerLabel : "session",
             ts);
    const std::string banner = std::string(body) + "\n";

    LineCallback cb;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        cb = g_state.opts.onLine;
        if (g_state.opts.debugOutput) OutputDebugStringA(banner.c_str());
        WriteRawToSink(g_state.primary,
                       FormatJsonLine(g_state.opts.component, Level::Info, "Session", body), true);
    }
    if (cb) cb(banner.c_str());
}

void WriteV(Level level, const char* tag, const char* fmt, va_list ap) {
    char body[1024]{};
    vsnprintf(body, sizeof(body), fmt, ap);

    LineCallback cb;
    std::string line;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        line = FormatLine(g_state.opts.component, level, tag, body);
        cb = g_state.opts.onLine;
        const bool forceFlush = level >= Level::Warn;
        // 可读行只给 GUI/调试器；磁盘只写一份 JSONL。
        if (g_state.opts.debugOutput) OutputDebugStringA(line.c_str());
        WriteRawToSink(g_state.primary,
                       FormatJsonLine(g_state.opts.component, level, tag, body), forceFlush);
    }
    if (cb) cb(line.c_str());
}

void WriteAuxV(Component component, Level level, const char* tag, const char* fmt, va_list ap) {
    char body[1024]{};
    vsnprintf(body, sizeof(body), fmt, ap);

    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        const bool forceFlush = level >= Level::Warn;
        const std::string json = FormatJsonLine(component, level, tag, body);
        // 有独立 aux 文件时只写 aux（inject.jsonl），不再镜像进主 JSONL，避免双写。
        // 未 RegisterAuxFile 时回落到主 sink，保证事件不丢。
        FileSink* sink = nullptr;
        auto it = g_state.auxFiles.find(component);
        if (it != g_state.auxFiles.end()) {
            sink = &it->second;
        } else if (g_state.primary.file) {
            sink = &g_state.primary;
        }
        if (!sink) return;
        if (g_state.opts.debugOutput) {
            OutputDebugStringA(FormatLine(component, level, tag, body).c_str());
        }
        WriteRawToSink(*sink, json, forceFlush);
    }
}

void WriteAux(Component component, Level level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteAuxV(component, level, tag, fmt, ap);
    va_end(ap);
}

void Write(Level level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(level, tag, fmt, ap);
    va_end(ap);
}

void Trace(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(Level::Trace, tag, fmt, ap);
    va_end(ap);
}

void Debug(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(Level::Debug, tag, fmt, ap);
    va_end(ap);
}

void Info(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(Level::Info, tag, fmt, ap);
    va_end(ap);
}

void Warn(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(Level::Warn, tag, fmt, ap);
    va_end(ap);
}

void Error(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(Level::Error, tag, fmt, ap);
    va_end(ap);
}

void Ok(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(Level::Ok, tag, fmt, ap);
    va_end(ap);
}

std::vector<std::string> ReadTailLines(const char* path, size_t maxLines, size_t readBytes) {
    std::vector<std::string> out;
    if (!path || !path[0] || maxLines == 0) return out;
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty() || GetFileAttributesW(widePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return out;

    std::ifstream f(std::filesystem::path(widePath), std::ios::binary | std::ios::ate);
    if (!f) return out;

    const std::streamoff end = f.tellg();
    if (end <= 0) return out;

    const std::streamoff start =
        end > static_cast<std::streamoff>(readBytes) ? end - static_cast<std::streamoff>(readBytes)
                                                     : 0;
    f.seekg(start);
    std::string chunk(static_cast<size_t>(end - start), '\0');
    if (!f.read(chunk.data(), chunk.size())) return out;

    if (start > 0) {
        const size_t nl = chunk.find('\n');
        if (nl != std::string::npos) chunk.erase(0, nl + 1);
    }

    size_t pos = 0;
    while (pos < chunk.size()) {
        size_t nl = chunk.find('\n', pos);
        if (nl == std::string::npos) nl = chunk.size();
        std::string line = chunk.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
        pos = nl + 1;
    }

    if (out.size() > maxLines) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(maxLines));
    }
    return out;
}

}  // namespace xcat::log
