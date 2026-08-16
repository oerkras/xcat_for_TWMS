#pragma once

#include <Windows.h>

#include <cstdint>

namespace x::runtime {

void LogInit();
void LogShutdown();
void LogI(const char* tag, const char* fmt, ...);
void LogW(const char* tag, const char* fmt, ...);
void LogE(const char* tag, const char* fmt, ...);

// slot 0..255 独立节流表；intervalMs 内同 slot 只写一条。I / W 两套表互不占用。
void LogIThrottled(uint32_t slot, DWORD intervalMs, const char* tag, const char* fmt, ...);
void LogWThrottled(uint32_t slot, DWORD intervalMs, const char* tag, const char* fmt, ...);

}  // namespace x::runtime
