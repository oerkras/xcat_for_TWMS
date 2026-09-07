#pragma once

// 安装身份 SSOT（必须与根 CMakeLists.txt 的 XCAT_*_OUTPUT_NAME / XCAT_PAYLOAD_DIR_NAME 一致）。
// 产物与命名对象不用 XCat 明文，也不冒充 version.dll / winmm.dll 等系统 DLL。
// CMake 目标名仍是 xcat / xcat_probe（仅仓库内部）。

#include <cstddef>
#include <string>

namespace xcat::install {

constexpr const char kLauncherStem[] = "rtapp";
constexpr const wchar_t kLauncherStemW[] = L"rtapp";
constexpr const char kLauncherExe[] = "rtapp.exe";
constexpr const wchar_t kLauncherExeW[] = L"rtapp.exe";

constexpr const char kPayloadDir[] = "rtcache";
constexpr const wchar_t kPayloadDirW[] = L"rtcache";

constexpr const char kPayloadDllStem[] = "rtmod";
constexpr const wchar_t kPayloadDllStemW[] = L"rtmod";
constexpr const char kPayloadDll[] = "rtmod.dll";
constexpr const wchar_t kPayloadDllW[] = L"rtmod.dll";

// %LOCALAPPDATA% 下独立会话罐（CDP / 账密直登副本）。禁止写回日常 User Data。
constexpr const wchar_t kLocalAppLeafW[] = L"7a3c91e0";

constexpr const wchar_t kShmStatusFmt[] = L"Local\\7a3c91e0_st_%08X";
constexpr const wchar_t kShmNotifyFmt[] = L"Local\\7a3c91e0_nt_%08X";
constexpr const wchar_t kShmBuffsFmt[] = L"Local\\7a3c91e0_bf_%08X";
constexpr const wchar_t kShmLampsFmt[] = L"Local\\7a3c91e0_lp_v%u_%08X";
constexpr const wchar_t kEventProbeReadyFmt[] = L"Local\\7a3c91e0_pr_%lu";
constexpr const wchar_t kMutexLauncher[] = L"Local\\7a3c91e0_ln";
constexpr const wchar_t kMutexOps[] = L"Local\\7a3c91e0_ops";
constexpr const wchar_t kMutexIniFmt[] = L"Local\\7a3c91e0_ini_%016llx";

constexpr const wchar_t kWndClass[] = L"RtAppWindow";
constexpr const wchar_t kWndTitle[] = L"rtapp";
constexpr const wchar_t kMsgBoxTitle[] = L"rtapp";
constexpr const wchar_t kHttpUserAgent[] = L"rtapp-Upd/1.0";
constexpr const wchar_t kWndClassOps[] = L"RtOpsWindow";
constexpr const wchar_t kWndTitleOps[] = L"rtapp Ops";

inline std::string JoinPayloadDir(const std::string& exeBinDir) {
    std::string d = exeBinDir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d.push_back('\\');
    d += kPayloadDir;
    return d;
}

inline std::wstring JoinPayloadDllW(const std::wstring& exeDirWithSlash) {
    return exeDirWithSlash + kPayloadDirW + L"\\" + kPayloadDllW;
}

const char* LegacyPayloadDirA();
const wchar_t* LegacyPayloadDirW();
const char* LegacyLauncherStemA();
const char* LegacyPayloadDllA();
const char* LegacyLocalAppLeafA();
const wchar_t* LegacyLocalAppLeafW();
const char* LegacyProgramDataLeafA();
const wchar_t* LegacyProgramDataLeafW();

}  // namespace xcat::install
