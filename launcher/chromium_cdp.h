#pragma once

// Chromium DevTools Protocol（Chrome / Edge / Chrome++ 共用）最小客户端。
// 仅使用稳定域：Page / Runtime；不依赖 WebView2。

#include <functional>
#include <string>

namespace msc::cdp {

using LogFn = std::function<void(const std::wstring& line)>;

// Gama Pass CDP 专用调试口（与 gamapass_cdp_login 一致）
constexpr int kDefaultRemoteDebugPort = 19222;

struct BrowserProfile {
    std::wstring exe;       // chrome.exe / msedge.exe
    std::wstring userData;  // User Data / Chrome++ Data
};

// 解析本机首选 Chromium 与 User Data（系统默认优先）。
// 官方 Chrome/360 标准目录 → GamaPassCdpProfile 副本；Edge / Chrome++ → 直开日常。
bool ResolvePreferredChromium(BrowserProfile& out, const LogFn& log = nullptr);

// 下一轮 PrepareCdpSafeUserData 强制从日常 User Data 重同步 Cookies/会话
//（落到完整 /login 后调用；日常重新勾选记住后再一键即可吃到新会话）。
void RequestCdpSessionResync();

// 关闭占用该调试口的 Chromium：先 CDP Browser.close，再按 cmdline 精确 TerminateProcess。
// ★ 只杀带 --remote-debugging-port=<port> 的 chrome/msedge/chromium，不动日常无调试口窗口。
// 不清 Cookie / User Data。用户已授权在登录成功后结束该调试实例。
// 返回：是否已无调试口响应（关干净或本来就没开）。
bool CloseRemoteBrowser(int port = kDefaultRemoteDebugPort, const LogFn& log = nullptr);

// Gama Pass 自动登录前防呆：结束会锁住 profile.userData 的日常浏览器（用户已授权）。
// ★ 只杀 Chromium 系且命中目标目录 / 同安装默认同目录的实例；跳过已带本调试口的进程。
// 不清 Cookie、不写回 User Data。返回成功 Terminate 的进程数。
unsigned KillBrowsersBlockingProfile(const BrowserProfile& profile,
                                     int debugPort = kDefaultRemoteDebugPort,
                                     const LogFn& log = nullptr);

// （保留）曾用于 UIA 一键前杀日常浏览器；现 UIA 正道已改为不杀，靠 --new-window + 快照附着。
// ★ 只杀 Chromium 主进程（跳过 --type= 子进程）；不清 Cookie / User Data。勿再接到默认登录路径。
unsigned KillDailyBrowsersForUiaLogin(const std::wstring& preferredExe,
                                      const LogFn& log = nullptr);

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 连接已有调试口；失败返回 false
    bool Connect(int port, const LogFn& log = nullptr);
    // 若未连接：用 profile 启动浏览器（带调试口），再 Connect。
    // 启动前先 KillBrowsersBlockingProfile 释放目录，再探测残留冲突；
    // 冲突或失败时 outFailHint 为人话提示（可空）。
    bool EnsureBrowser(const BrowserProfile& profile, int port, const LogFn& log = nullptr,
                       std::wstring* outFailHint = nullptr);

    // 检测配置目录是否正被「无本调试口」的 Chromium 占用（只读探测，不杀）。
    static bool ProbeUserDataConflict(const BrowserProfile& profile, int debugPort,
                                      std::wstring& outHint, const LogFn& log = nullptr);

    bool IsConnected() const { return ws_ != nullptr; }
    std::wstring BrowserVersion() const { return browserVersion_; }

    // 打开/复用标签并导航
    bool Navigate(const std::wstring& url, const LogFn& log = nullptr);
    // Runtime.evaluate；outResultJson 为 CDP result.result.value 的 JSON 文本（字符串会带引号）
    bool Evaluate(const std::wstring& jsExpression, std::string& outResultJson,
                  const LogFn& log = nullptr);

    // 任意 CDP 方法（Page / Runtime / DOMStorage / Input 等稳定域）
    bool Command(const std::string& method, const std::string& paramsJson, std::string& resultJson,
                 const LogFn& log = nullptr);
    // 当前页 URL
    bool GetUrl(std::wstring& outUrl, const LogFn& log = nullptr);

    // 附着浏览器级调试口并发送 Browser.close（优雅退出整窗）
    bool QuitBrowser(int port = 0, const LogFn& log = nullptr);

    // 关掉多余 about:blank 标签（保留当前附着页），避免二次启动留下空标签
    int CloseExtraBlankPages(const LogFn& log = nullptr);

    // 把当前附着标签拉到前台（避免 Navigate 发生在后台 tab，用户盯着另一扇 about:blank）
    bool ActivateAttachedPage(const LogFn& log = nullptr);

    // 调试口是否仍在响应（/json/version）
    static bool IsPortAlive(int port);

    void Close();

private:
    bool HttpGetLocal(int port, const wchar_t* path, std::string& body);
    bool HttpLocal(int port, const wchar_t* method, const wchar_t* path, std::string& body);
    bool OpenWs(const std::wstring& wsUrl, const LogFn& log);
    bool SendRecv(const std::string& method, const std::string& paramsJson, std::string& resultJson,
                  const LogFn& log);
    bool PickPageWsUrl(int port, std::wstring& outWs, const LogFn& log);

    void* session_ = nullptr;  // HINTERNET
    void* connect_ = nullptr;  // HINTERNET
    void* ws_ = nullptr;       // HINTERNET websocket
    int nextId_ = 1;
    int port_ = 0;
    std::wstring browserVersion_;
    std::wstring pageWsUrl_;
};

}  // namespace msc::cdp
