#pragma once

#include "../common/xcat_log.h"

#include "../common/process_util.h"

#include <cstdarg>
#include <mutex>
#include <string>

namespace xcat::inject_log {

inline void RegisterFromExeDir(const char* exeBinDir) {
    if (!exeBinDir || !exeBinDir[0]) return;
    xcat::log::RegisterAuxFile(xcat::log::Component::Inject,
                               xcat::log::paths::InjectLog(exeBinDir));
}

inline void RegisterFromDllPath(const std::wstring& absoluteDll) {
    const std::wstring payloadDir = ParentDirWithSlash(absoluteDll);
    const std::wstring exeBin = ParentDirWithSlash(payloadDir);
    RegisterFromExeDir(WideToUtf8(exeBin).c_str());
}

inline void Write(xcat::log::Level level, const char* tag, const char* fmt, va_list ap) {
    xcat::log::WriteAuxV(xcat::log::Component::Inject, level, tag, fmt, ap);
}

inline void Info(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(xcat::log::Level::Info, tag, fmt, ap);
    va_end(ap);
}

inline void Warn(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(xcat::log::Level::Warn, tag, fmt, ap);
    va_end(ap);
}

inline void Error(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(xcat::log::Level::Error, tag, fmt, ap);
    va_end(ap);
}

inline void Ok(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(xcat::log::Level::Ok, tag, fmt, ap);
    va_end(ap);
}

}  // namespace xcat::inject_log
