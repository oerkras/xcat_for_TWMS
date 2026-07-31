#include "log.h"

#include "../../common/xcat_log.h"
#include "../x_version.h"

#include "bin_dir.h"

#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace x::runtime {

void LogInit() {
    xcat::log::Options opts{};
    opts.component = xcat::log::Component::Payload;
    opts.filePath = xcat::log::paths::PayloadLog(GetBinDir());
    opts.debugOutput = true;
    // Payload 高频 Info 按 1 秒窗口批量落盘；Warn/Error 仍由公共日志层立即 flush。
    opts.flushIntervalMs = 1000;
    // 上传器按整文件收取；轮转单文件对齐上限，避免「大文件只上传尾部」造成窗口空洞。
    opts.rotation.maxFileBytes = 512 * 1024;
    opts.rotation.maxBackups = 360;

    xcat::log::Configure(opts);
    xcat::log::BeginSession("payload");
    LogI("Bootstrap", "payload log ver=%s build=0x%08X path=%s", kVersionString, kBuildId,
         opts.filePath.c_str());
}

void LogShutdown() { xcat::log::Shutdown(); }

void LogI(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xcat::log::WriteV(xcat::log::Level::Info, tag, fmt, ap);
    va_end(ap);
}

void LogW(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xcat::log::WriteV(xcat::log::Level::Warn, tag, fmt, ap);
    va_end(ap);
}

void LogE(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xcat::log::WriteV(xcat::log::Level::Error, tag, fmt, ap);
    va_end(ap);
}

void LogWThrottled(uint32_t slot, DWORD intervalMs, const char* tag, const char* fmt, ...) {
    static std::mutex mtx;
    static std::array<DWORD, 256> lastMs{};

    const DWORD now = GetTickCount();
    const uint32_t i = slot % static_cast<uint32_t>(lastMs.size());
    bool skip = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (lastMs[i] && static_cast<int>(now - lastMs[i]) < static_cast<int>(intervalMs))
            skip = true;
        else
            lastMs[i] = now;
    }
    if (skip) return;

    va_list ap;
    va_start(ap, fmt);
    xcat::log::WriteV(xcat::log::Level::Warn, tag, fmt, ap);
    va_end(ap);
}

}  // namespace x::runtime
