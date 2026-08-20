// 新楓之谷經典版 · 登录/换票会话（嵌入 xcat_app）
// GAMA PASS UIA / HTTP Beanfun；已移除 WebView2。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <shlwapi.h>

#include "msc_launch.h"
#include "ott_ticket_fetch.h"
#include "msc_webview_login.h"
#include "http_beanfun_login.h"
#include "http_gamapass_login.h"
#include "gamapass_cdp_login.h"
#include "gamapass_uia_login.h"
#include "chromium_cdp.h"
#include "inject_after_launch.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace msc::weblogin {

constexpr wchar_t kClassicMainUrl[] = L"https://maplestoryclassic.beanfun.com/Main";

struct AccountCred {
    std::wstring user;
    std::wstring pass;
};

struct AppState {
    HWND hwnd = nullptr;
    bool closing = false;

    AuthStrategy authStrategy = AuthStrategy::GamaPassAuto;
    CaptchaUiMode captchaUi = CaptchaUiMode::OpenBrowser;
    std::atomic_bool busy{false};
    AccountCred cred;

    std::mutex logMu;
    std::vector<std::wstring> pendingLogs;
    std::wstring logFilePath;
    LogCallback onLog;
};

AppState g;

void QueueLog(const std::wstring& line);
void OpenClassicMainInBrowser();

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string NarrowUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring LocalRuntimeDir() {
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
        dir = std::wstring(base) + L"\\xcat_msc";
        CoTaskMemFree(base);
    } else {
        wchar_t tmp[MAX_PATH]{};
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) {
            dir = std::wstring(tmp) + L"xcat_msc";
        } else {
            dir = ExeDir() + L"\\xcat_msc_local";
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring AccountConfigPath() { return ExeDir() + L"\\account.txt"; }
std::wstring AuthStrategyPath() { return ExeDir() + L"\\auth_strategy.txt"; }
std::wstring CaptchaUiPath() { return ExeDir() + L"\\captcha_ui.txt"; }

AuthStrategy LoadAuthStrategyFromDisk() {
    std::ifstream f(NarrowUtf8(AuthStrategyPath()), std::ios::binary);
    if (!f) return AuthStrategy::HttpFirst;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
        raw.pop_back();
    for (char& c : raw) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (raw == "gama_pass" || raw == "gamapass" || raw == "gama_pass_auto" || raw == "3")
        return AuthStrategy::GamaPassAuto;
    // http_first / http_only / webview_only / 0|1|2 → 统一 HTTP
    return AuthStrategy::HttpFirst;
}

void SaveAuthStrategyToDisk(AuthStrategy s) {
    const char* v = (s == AuthStrategy::GamaPassAuto) ? "gama_pass" : "http_first";
    std::ofstream f(NarrowUtf8(AuthStrategyPath()), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << v;
}

CaptchaUiMode LoadCaptchaUiFromDisk() {
    std::ifstream f(NarrowUtf8(CaptchaUiPath()), std::ios::binary);
    if (!f) return CaptchaUiMode::OpenBrowser;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
        raw.pop_back();
    for (char& c : raw) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // 历史 silent / 数值 2 → 不开浏览器；其余（含 browser_only）→ 开浏览器
    if (raw == "silent" || raw == "silent_fallback" || raw == "no_browser" || raw == "2")
        return CaptchaUiMode::NoBrowser;
    return CaptchaUiMode::OpenBrowser;
}

void SaveCaptchaUiToDisk(CaptchaUiMode m) {
    const char* v = (m == CaptchaUiMode::NoBrowser) ? "silent" : "popup_on_captcha";
    std::ofstream f(NarrowUtf8(CaptchaUiPath()), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << v;
}

std::wstring LegacyAccountPurgePath() { return LocalRuntimeDir() + L"\\account.txt"; }

void PurgeLegacyAccountFile() {
    const std::wstring path = LegacyAccountPurgePath();
    if (path.empty()) return;
    DeleteFileW(path.c_str());
}

void SaveAccountConfig(const std::wstring& line) {
    std::wstring s = line;
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    s.erase(std::remove(s.begin(), s.end(), L'\n'), s.end());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    const std::wstring path = AccountConfigPath();
    std::ofstream f(NarrowUtf8(path), std::ios::binary | std::ios::trunc);
    if (!f) return;
    const std::string utf8 = NarrowUtf8(s);
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    PurgeLegacyAccountFile();
}

std::wstring ReadAccountFile(const std::wstring& path) {
    std::ifstream f(NarrowUtf8(path), std::ios::binary);
    if (!f) return {};
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
        raw.pop_back();
    return WidenUtf8(raw);
}

std::wstring LoadAccountConfig() {
    std::wstring line = ReadAccountFile(AccountConfigPath());
    if (line.empty()) {
        const std::wstring legacy = ReadAccountFile(LegacyAccountPurgePath());
        if (!legacy.empty()) {
            SaveAccountConfig(legacy);
            QueueLog(L"[OK] 已将旧 AppData 账号迁到程序目录并清除 LocalAppData 副本：" +
                     AccountConfigPath());
            line = ReadAccountFile(AccountConfigPath());
        }
    }
    PurgeLegacyAccountFile();
    return line;
}

bool ParseAccountLine(const std::wstring& raw, AccountCred& out, std::wstring& err) {
    std::wstring s = raw;
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    s.erase(std::remove(s.begin(), s.end(), L'\n'), s.end());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t')) ++start;
    s = s.substr(start);
    if (s.empty()) {
        err = L"请先粘贴账号信息（邮箱-密码-…，横线个数不限）";
        return false;
    }

    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        while (j < s.size() && s[j] != L'-') ++j;
        if (j > i) parts.push_back(s.substr(i, j - i));
        if (j >= s.size()) break;
        while (j < s.size() && s[j] == L'-') ++j;
        i = j;
    }

    if (parts.size() >= 2 && !parts[0].empty() && !parts[1].empty()) {
        out.user = parts[0];
        out.pass = parts[1];
        return true;
    }

    for (wchar_t sep : {L'|', L'\t', L':'}) {
        size_t p = s.find(sep);
        if (p != std::wstring::npos && p > 0 && p + 1 < s.size()) {
            out.user = s.substr(0, p);
            out.pass = s.substr(p + 1);
            return true;
        }
    }
    err = L"无法解析账号串。格式：邮箱-密码-其它…（横线个数不限，只使用前两项）";
    return false;
}

void AppendFileLog(const std::wstring& line) {
    if (g.logFilePath.empty()) return;
    std::ofstream f(NarrowUtf8(g.logFilePath), std::ios::app);
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[64];
    sprintf_s(ts, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    f << ts << NarrowUtf8(line) << '\n';
}

void QueueLog(const std::wstring& line) {
    AppendFileLog(line);
    {
        std::lock_guard<std::mutex> lock(g.logMu);
        g.pendingLogs.push_back(line);
    }
    if (g.hwnd) PostMessageW(g.hwnd, kMsgFlushLogs, 0, 0);
}

void FlushLogsToUi() {
    std::vector<std::wstring> batch;
    {
        std::lock_guard<std::mutex> lock(g.logMu);
        batch.swap(g.pendingLogs);
    }
    if (batch.empty()) return;
    for (const auto& line : batch) {
        if (g.onLog) g.onLog(line);
    }
}

void SetBusy(bool busy) {
    g.busy = busy;
    if (!g.hwnd) return;
    if (!busy) {
        KillTimer(g.hwnd, kHttpBusyTimerId);
        return;
    }
    SetTimer(g.hwnd, kHttpBusyTimerId, kHttpBusyTimeoutMs, nullptr);
}

void OpenClassicMainInBrowser() {
    const HINSTANCE hr =
        ShellExecuteW(nullptr, L"open", kClassicMainUrl, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hr) <= 32) {
        QueueLog(L"[FAIL] 无法打开浏览器，请手动访问 " + std::wstring(kClassicMainUrl));
    } else {
        QueueLog(L"[提示] 已打开官网，请在浏览器完成验证码/登录后回到本程序再点一键");
    }
}

bool LaunchWithTicket(msc::launcher::GalaxyTicket ticket, bool attachExistingClassic = false) {
    QueueLog(attachExistingClassic ? L"[…] 换票完成：优先接管已有经典版，否则 NGM 拉起…"
                                   : L"[…] NGM 拉起并验票…");
    msc::launcher::Options opt;
    opt.ticket = std::move(ticket);
    opt.attachExistingClassic = attachExistingClassic;
    auto rr = msc::launcher::Run(opt, [](const msc::launcher::Progress& p) {
        QueueLog(L"  " + WidenUtf8(p.message));
    });
    bool injectOk = false;
    if (!rr.ok) {
        QueueLog(L"[FAIL] 启动失败：" + WidenUtf8(rr.errorMessage));
    } else {
        QueueLog(L"[OK] 游戏就绪 PID=" + std::to_wstring(rr.gamePid) + L" " +
                 WidenUtf8(rr.cmdLineSummary));
        QueueLog(L"[…] 一键注入：等待 GameAssembly 后 LoadLibraryW…");
        xcat::twms_inject::Options iopt;
        iopt.pid = rr.gamePid;
        auto ir = xcat::twms_inject::InjectIntoClassic(iopt, [](const std::wstring& line) {
            QueueLog(line);
        });
        if (ir.ok) {
            QueueLog(L"[OK] 一键启动并注入完成");
            injectOk = true;
        } else {
            QueueLog(L"[FAIL] 注入未完成：" + WidenUtf8(ir.message));
        }
    }
    PostMessageW(g.hwnd, kMsgIdle, 0, 0);
    return injectOk;
}

void StartOneClickWithLine(const std::wstring& accountLine, std::wstring& err) {
    err.clear();
    if (g.busy) {
        err = L"正在登录中";
        return;
    }

    if (g.authStrategy == AuthStrategy::GamaPassAuto) {
        g.cred = {};
        SetBusy(true);
        QueueLog(std::wstring(kHttpBusyTag) + L" GAMA PASS 浏览器点选换票中…");
        QueueLog(L"[…] GAMA PASS：UIA 点选换票（无调试口）。"
                 L"始终走日常浏览器，不因账密助手 device_id 改道独立罐。不写回日常罐。");
        QueueLog(L"[提示] 一键会新开登录窗。助手不会自动衔接换票；请先助手登完再点自动登录。"
                 L"不调用 refresh。");

        std::thread([]() {
            const bool usable = msc::launcher::HttpGamaPassHasUsableSession();
            QueueLog(usable ? L"[探测] Gama Pass：有未过期 userToken（仅探测，本轮仍走 UIA）"
                            : L"[探测] Gama Pass：无可用未过期 userToken（仅探测，本轮仍走 UIA）");

            auto lr = msc::launcher::HttpGamaPassUiaLoginToOtt(
                [](const std::wstring& line) { QueueLog(line); });
            if (lr.ok && lr.ticketFilled) {
                QueueLog(std::wstring(kHttpTicketOkTag) + L" GAMA PASS 换票成功，正在开游戏…");
                QueueLog(L"[OK] 换票成功 uid=" + lr.ticket.userObjectId + L" gid=" + lr.ticket.gid);
                // 成功收口已由 LaunchWithTicket 写「[OK] 一键启动并注入完成」；
                // 此处勿再打同句，否则 launch_panel 会双响/双气泡。
                if (!LaunchWithTicket(std::move(lr.ticket), /*attachExistingClassic=*/true)) {
                    QueueLog(L"[…] 换票/启动已结束");
                }
                return;
            }

            QueueLog(L"[FAIL] 登录失败 [" +
                     WidenUtf8(msc::launcher::HttpLoginErrorName(lr.error)) + L"] " +
                     WidenUtf8(lr.message));
            QueueLog(L"[提示] GAMA PASS UIA 未完成。请在弹出的日常浏览器窗口内登录并勾选记住。"
                     L"不会调用 refresh/token，也不会改走账密助手独立罐。");
            PostMessageW(g.hwnd, kMsgIdle, 0, 0);
        }).detach();
        return;
    }

    AccountCred cred;
    if (!ParseAccountLine(accountLine, cred, err)) return;
    g.cred = std::move(cred);
    SaveAccountConfig(accountLine);

    SetBusy(true);
    QueueLog(std::wstring(kHttpBusyTag) + L" HTTP 登录换票中…");
    QueueLog(std::wstring(L"[…] HTTP 登录换票：账号=") + g.cred.user + L" 验证码UI=" +
             WidenUtf8(CaptchaUiModeLabel(g.captchaUi)));

    const AccountCred credCopy = g.cred;
    const CaptchaUiMode captchaCopy = g.captchaUi;
    std::thread([credCopy, captchaCopy]() {
        auto lr = msc::launcher::HttpBeanfunLoginToOtt(
            credCopy.user, credCopy.pass,
            [](const std::wstring& line) { QueueLog(line); });
        if (lr.ok && lr.ticketFilled) {
            QueueLog(std::wstring(kHttpTicketOkTag) + L" HTTP 换票成功，正在开游戏…");
            QueueLog(L"[OK] HTTP 换票成功 uid=" + lr.ticket.userObjectId + L" gid=" +
                     lr.ticket.gid);
            LaunchWithTicket(std::move(lr.ticket));
            return;
        }

        QueueLog(L"[FAIL] HTTP 登录失败 [" +
                 WidenUtf8(msc::launcher::HttpLoginErrorName(lr.error)) + L"] " +
                 WidenUtf8(lr.message));

        const bool needWebVerify =
            lr.error == msc::launcher::HttpLoginError::CaptchaRequired ||
            lr.error == msc::launcher::HttpLoginError::DualVerifyRequired;

        if (needWebVerify) {
            QueueLog(std::wstring(kNeedWebVerifyTag) +
                     L" 请自行到网页完成验证码/二次验证后，再回来点「一键启动」。"
                     L"官网：" +
                     std::wstring(kClassicMainUrl));
            if (captchaCopy != CaptchaUiMode::NoBrowser) {
                OpenClassicMainInBrowser();
            }
        }

        PostMessageW(g.hwnd, kMsgIdle, 0, 0);
    }).detach();
}

bool Init(HWND msgHwnd, LogCallback onLog) {
    if (!msgHwnd) return false;
    g.hwnd = msgHwnd;
    g.onLog = std::move(onLog);
    g.closing = false;
    g.authStrategy = LoadAuthStrategyFromDisk();
    g.captchaUi = LoadCaptchaUiFromDisk();
    g.logFilePath = ExeDir() + L"\\launcher.log";
    QueueLog(L"经典版登录会话已嵌入 xcat_app（无 WebView2）。日志：" + g.logFilePath);
    QueueLog(L"账号配置：" + AccountConfigPath());
    QueueLog(std::wstring(L"取票策略：") + WidenUtf8(AuthStrategyLabel(g.authStrategy)) +
             L"（bin/auth_strategy.txt）");
    QueueLog(std::wstring(L"验证码UI：") + WidenUtf8(CaptchaUiModeLabel(g.captchaUi)) +
             L"（bin/captcha_ui.txt）");
    return true;
}

bool IsBusy() { return g.busy.load(); }
bool CanStartOneClick() { return true; }

AuthStrategy GetAuthStrategy() { return g.authStrategy; }

void SetAuthStrategy(AuthStrategy s) {
    // 历史 HttpOnly / webview_only(2) → 统一 HTTP
    if (s != AuthStrategy::GamaPassAuto) s = AuthStrategy::HttpFirst;
    const bool changed = (g.authStrategy != s);
    g.authStrategy = s;
    SaveAuthStrategyToDisk(s);
    if (changed) {
        QueueLog(std::wstring(L"[OK] 取票策略已切换为 ") + WidenUtf8(AuthStrategyLabel(s)));
    }
}

const char* AuthStrategyLabel(AuthStrategy s) {
    switch (s) {
        case AuthStrategy::GamaPassAuto:
            return "GAMA PASS自动登录";
        case AuthStrategy::HttpFirst:
        case AuthStrategy::HttpOnly:
        default:
            return "HTTP";
    }
}

CaptchaUiMode GetCaptchaUiMode() { return g.captchaUi; }

void SetCaptchaUiMode(CaptchaUiMode m) {
    if (m != CaptchaUiMode::NoBrowser) m = CaptchaUiMode::OpenBrowser;
    g.captchaUi = m;
    SaveCaptchaUiToDisk(m);
    QueueLog(std::wstring(L"[OK] 验证码UI已切换为 ") + WidenUtf8(CaptchaUiModeLabel(m)));
}

const char* CaptchaUiModeLabel(CaptchaUiMode m) {
    return (m == CaptchaUiMode::NoBrowser) ? "不开浏览器" : "开浏览器";
}

int GetGamaPassNickSlot() { return msc::launcher::GetGamaPassNickSlot(); }

void SetGamaPassNickSlot(int slot1Based) {
    const int before = msc::launcher::GetGamaPassNickSlot();
    msc::launcher::SetGamaPassNickSlot(slot1Based);
    const int after = msc::launcher::GetGamaPassNickSlot();
    if (after != before) {
        QueueLog(L"[OK] 将使用第 " + std::to_wstring(after) + L" 个游戏昵称");
    }
}

int GetGamaPassAccountSlot() { return msc::launcher::GetGamaPassAccountSlot(); }

void SetGamaPassAccountSlot(int slot1Based) {
    const int before = msc::launcher::GetGamaPassAccountSlot();
    msc::launcher::SetGamaPassAccountSlot(slot1Based);
    const int after = msc::launcher::GetGamaPassAccountSlot();
    if (after != before) {
        QueueLog(L"[OK] 将登录第 " + std::to_wstring(after) + L" 个账号");
    }
}

bool StartOneClick(const std::wstring& accountLine, std::wstring& err) {
    StartOneClickWithLine(accountLine, err);
    return err.empty();
}

bool TryParseAccountLine(const std::wstring& accountLine, std::wstring& err) {
    AccountCred cred;
    return ParseAccountLine(accountLine, cred, err);
}

void OnTimer(UINT_PTR id) {
    if (id == kHttpBusyTimerId && g.busy) {
        // C2799 22:08：UIA 干净重开仍在跑，看门狗先打 FAIL 并 SetBusy(false)，
        // 线程随后 cmdline 接管并注入——界面已显示失败，且可再点一键开第二路。
        // 看门狗不取消 worker，因此只续期，真正收口仍走成功/失败的 kMsgIdle。
        QueueLog(std::wstring(kHttpTimeoutTag) +
                 L" 换票已超过5分钟仍在进行，继续等（不中断、不改忙碌态）…");
        SetTimer(g.hwnd, kHttpBusyTimerId, kHttpBusyTimeoutMs, nullptr);
    }
}

void OnFlushLogs() { FlushLogsToUi(); }
void OnIdle() { SetBusy(false); FlushLogsToUi(); }

void Shutdown() {
    if (g.closing) return;
    g.closing = true;
    const bool wasBusy = g.busy.exchange(false);
    QueueLog(L"[…] 正在关闭登录会话…");
    FlushLogsToUi();
    if (g.hwnd) KillTimer(g.hwnd, kHttpBusyTimerId);
    // 换票中途退出：收掉 CDP 调试浏览器，避免残留 about:blank
    if (wasBusy || msc::cdp::Session::IsPortAlive(msc::cdp::kDefaultRemoteDebugPort)) {
        QueueLog(L"[…] 关闭登录用浏览器（CDP）…");
        FlushLogsToUi();
        msc::cdp::CloseRemoteBrowser(msc::cdp::kDefaultRemoteDebugPort,
                                     [](const std::wstring& line) { QueueLog(line); });
        FlushLogsToUi();
    }
    g.cred = {};
    g.onLog = {};
}

std::wstring LoadSavedAccountLine() { return LoadAccountConfig(); }
void SaveAccountLine(const std::wstring& line) { SaveAccountConfig(line); }

}  // namespace msc::weblogin
