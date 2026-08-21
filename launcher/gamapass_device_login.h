#pragma once

// Gama Pass 账密登录助手（独立模块：不换票、不开游戏）。
// 专用 Chromium profile + CDP：在 SPA 启动前钉死 accounts.gamania.com 的 localStorage.device_id。
// 不碰日常 User Data、不调用 /v1/refresh/token、不改写 OAuth prompt。
// GAMA PASS自动登录不读取本模块落盘的 device_id，始终走日常浏览器罐。

#include "http_beanfun_login.h"

#include <string>

namespace msc::launcher {

// 总开关：false = UI 不开放、Start 直接拒绝（不会开独立 user-data-dir）。
inline constexpr bool kGamaPassDeviceLoginEnabled = false;

enum class GpDeviceLoginBrowserKind : int {
    Auto = 0,        // Chrome++ > Chrome > Edge
    ChromePlus = 1,
    Chrome = 2,
    Edge = 3,
};

struct GamaPassDeviceLoginAccount {
    std::string email;          // 账号 / mail_address
    std::string password;       // Gama Pass 密码（填 accounts）
    std::string emailPassword;  // 邮箱密码（本页不填）
    std::string deviceId;       // 32 位 hex，禁止自造
    std::string openId;
    std::string userToken;
    std::string refreshToken;
    std::string trackId;
    GpDeviceLoginBrowserKind browserKind = GpDeviceLoginBrowserKind::Auto;
};

// 四段卖家行：账号----密码----邮箱密码----device_id
// 六段 LoginRecord：mail_address----mail_password----gama_password----device_id----userToken----refreshToken
// 注意：四段第 2 段是 Gama 密码；六段第 2 段是邮箱密码、第 3 段才是 Gama 密码。
bool ParseGamaPassDeviceLoginLine(const std::string& raw, GamaPassDeviceLoginAccount& out,
                                  std::string& err);
std::string FormatGamaPassDeviceLoginLine(const GamaPassDeviceLoginAccount& acc);

// 落盘：XCat_data/state/gp_device_login.json（密码明文，与 account.txt 同级风险）
std::wstring GamaPassDeviceLoginStorePath(const std::wstring& prefsBinDir);
bool LoadGamaPassDeviceLoginAccount(const std::wstring& storePath,
                                    GamaPassDeviceLoginAccount& out);
bool SaveGamaPassDeviceLoginAccount(const std::wstring& storePath,
                                    const GamaPassDeviceLoginAccount& acc);

// 解析将使用的浏览器 exe（Chrome++ / Chrome / Edge；排除 360）
bool ResolveGamaPassDeviceLoginBrowser(std::wstring& outExe, std::wstring& outLabel,
                                       HttpLoginLogFn log = nullptr,
                                       GpDeviceLoginBrowserKind kind = GpDeviceLoginBrowserKind::Auto);

bool IsGamaPassDeviceLoginBusy();

// 后台线程：开独立窗口填账密并钉 device_id。成功后关独立调试窗；二次验证 / 失败则保持打开。
bool StartGamaPassDeviceLogin(const GamaPassDeviceLoginAccount& acc,
                              const std::wstring& storePath, HttpLoginLogFn log,
                              std::wstring& err);

}  // namespace msc::launcher
