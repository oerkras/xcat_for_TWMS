#pragma once

// Gama Pass 账密登录：独立罐填账密（已有会话则跳过）→ 同一扇窗 Galaxy 换票 → 接管经典版。
// 专用 Chromium profile + CDP：在 SPA 启动前钉死 accounts.gamania.com 的 localStorage.device_id。
// 不碰日常 User Data、不调用 /v1/refresh/token、不改写 OAuth prompt。
// GAMA PASS自动登录不读取本模块落盘的 device_id，始终走日常浏览器罐。

#include "http_beanfun_login.h"

#include <string>

namespace msc::launcher {

// 总开关：false = UI 不开放、Start 直接拒绝（不会开独立 user-data-dir）。
inline constexpr bool kGamaPassDeviceLoginEnabled = true;

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

// 落盘：XCat_data/state/gp_device_login.dpapi（Windows DPAPI，仅本机当前用户可解）。
// 旧明文 gp_device_login.json 读入后改写成 dpapi 并删除。
std::wstring GamaPassDeviceLoginStorePath(const std::wstring& prefsBinDir);
bool LoadGamaPassDeviceLoginAccount(const std::wstring& storePath,
                                    GamaPassDeviceLoginAccount& out);
bool SaveGamaPassDeviceLoginAccount(const std::wstring& storePath,
                                    const GamaPassDeviceLoginAccount& acc);
// 删除 dpapi 与旧明文 json。文件本就不在也算成功。
bool DeleteGamaPassDeviceLoginAccount(const std::wstring& storePath);

// 解析将使用的浏览器 exe（Chrome++ / Chrome / Edge；排除 360）
bool ResolveGamaPassDeviceLoginBrowser(std::wstring& outExe, std::wstring& outLabel,
                                       HttpLoginLogFn log = nullptr,
                                       GpDeviceLoginBrowserKind kind = GpDeviceLoginBrowserKind::Auto);

bool IsGamaPassDeviceLoginBusy();
// 用户取消账密直登：置取消标志并关掉独立调试窗（port 19223）。不碰日常浏览器、不杀游戏。
bool CancelGamaPassDeviceLogin(HttpLoginLogFn log = nullptr);

// 只清独立罐 %LOCALAPPDATA%\XCat\GpDeviceLoginProfile（chromeplus/chrome/edge）。
// 先关调试口 19223，再按 --user-data-dir 释放锁；不碰日常 User Data / Cookie。
// 账密 dpapi 由调用方 DeleteGamaPassDeviceLoginAccount 另删。
bool ClearGamaPassDeviceLoginProfile(HttpLoginLogFn log, std::wstring& err);

// 后台线程：独立窗填账密（已有会话则跳过）→ 同窗换票 → 接管经典版。
// 二次验证 / 换票失败则保持窗口打开，下次点同一按钮不再重填。
bool StartGamaPassDeviceLogin(const GamaPassDeviceLoginAccount& acc,
                              const std::wstring& storePath, HttpLoginLogFn log,
                              std::wstring& err);

}  // namespace msc::launcher
