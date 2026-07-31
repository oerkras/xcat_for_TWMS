#pragma once

#include <Windows.h>

namespace x::runtime {

const char* GetBinDir();
void GetLogFilePath(char* out, int size);
HMODULE GetImageModule();
void SetImageModule(HMODULE mod);

}  // namespace x::runtime
