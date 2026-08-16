#pragma once

#include <functional>
#include <string>

struct AppWindow;

namespace xcat::app {

// gate/1 交互激活（ImGui 白天样式）：复用已建好的主窗渲染栈，在进入真正面板前弹激活框，
// 让用户粘贴个人签名 TOKEN。验签通过则写激活缓存并返回 true；用户点「退出」或关窗返回 false。
// 期间临时切到白天主题，返回前恢复用户原主题（不改盘上偏好）。
bool RunGateActivation(AppWindow& app, const std::string& binDir, const std::string& deviceId,
                       const char* extraError = nullptr);

// 启动在线门（gate/2 设备封禁粘性、gate/3 在线租约）专用：把可能阻塞数十秒的探活（运维不可达时
// 会串行走 直连→代理→AliDNS→按 IP 回退）丢到后台线程执行，UI 线程同时泵一个「正在检查授权…」帧。
// 避免窗口建好却因主线程被 WinHTTP 阻塞而始终不刷新，表现为「后台运行、无界面」。
// 返回 work() 的结果（true=该门要求拒启退出）。窗口在校验期间被关闭时仍会等 work 收尾后返回。
bool RunStartupGateWithModal(AppWindow& app, const char* statusText,
                             const std::function<bool()>& work);

}  // namespace xcat::app
