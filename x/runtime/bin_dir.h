#pragma once

#include <Windows.h>

namespace x::runtime {

const char* GetBinDir();
void GetLogFilePath(char* out, int size);
// 载荷自身基址。DllMain 写入后只读全局；未写入时才 FROM_ADDRESS 填一次。
HMODULE GetImageModule();
void SetImageModule(HMODULE mod);

}  // namespace x::runtime
