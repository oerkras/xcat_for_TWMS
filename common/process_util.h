#pragma once

#include <Windows.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace xcat {

bool ResolveAbsolutePath(const std::wstring& path, std::wstring& out);
std::wstring ParentDirWithSlash(const std::wstring& path);
std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(std::string_view text);
std::string Utf8ClampBytes(std::string_view src, size_t maxBytes);

// binDir + relative（自动补反斜杠）；任一为空则返回另一方。
std::string JoinBinPath(const char* binDir, const char* relative);

bool CreateDirectoryUtf8(const std::string& path);
bool DeleteFileUtf8(const std::string& path);
bool MoveFileExUtf8(const std::string& from, const std::string& to, DWORD flags);
bool CopyFileUtf8(const std::string& from, const std::string& to, bool failIfExists);
errno_t FopenUtf8(FILE** file, const std::string& path, const wchar_t* mode);

// 按进程名结束全部匹配实例（用于「退出 XCat 和游戏」）
unsigned KillProcessesByExeName(std::wstring_view exeName);

bool IsProcessAlive(DWORD pid);

}  // namespace xcat
