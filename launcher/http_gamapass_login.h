#pragma once

// Gama Pass HTTP 登录 → Galaxy beanfun result → 经典版 OTT（无需 WebView2）
// ★ 产品主路径已改为 CDP（HttpGamaPassCdpLoginToOtt）；本 API 保留签名兼容，当前无调用方。
// SelectGameAccount 若被走到：跟 GetGamaPassNickSlot（与 CDP/UI 同一槽）。
// 仅复用本机 Chrome/Edge 已登录会话里的未过期 userToken → 自动选账号；不走账密。
// ★ 绝不调用 /v1/refresh/token（会轮换 refreshToken 冲掉浏览器免登录）；
// ★ 绝不自动打开 OpenID/Galaxy 登录页；★ 不改写 OAuth prompt（与 CDP 红线一致）。

#include "http_beanfun_login.h"

namespace msc::launcher {

// user/pass 忽略（保留签名兼容）；必须已有浏览器/本地会话。
HttpLoginResult HttpGamaPassLoginToOtt(const std::wstring& user, const std::wstring& pass,
                                       HttpLoginLogFn log = nullptr, int timeoutMs = 45000);

// 是否已有可用会话（本地文件或可从 Chrome/Edge Local Storage 导入）
bool HttpGamaPassHasUsableSession();

// 优先 Chrome++ / 便携 Chrome / 官方 Chrome，其次 Edge（供引导打开 URL）
bool HttpGamaPassPreferredBrowserExe(std::wstring& outExe);

// 根据 chrome/msedge.exe 解析 User Data（含 chrome++.ini data_dir）
bool HttpGamaPassResolveUserDataDir(const std::wstring& exePath, std::wstring& outUserDataDir);

}  // namespace msc::launcher
