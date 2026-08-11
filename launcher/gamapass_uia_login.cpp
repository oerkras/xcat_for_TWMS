#include "gamapass_uia_login.h"

#include "gamapass_cdp_login.h"
#include "gamapass_ticket_harvest.h"
#include "http_gamapass_login.h"
#include "win_uia.h"

#include <Shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace msc::launcher {
namespace {

constexpr wchar_t kGalaxyLogin[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/mstc"
    L"?redirect_url=https://maplestoryclassic.beanfun.com/Main";

constexpr wchar_t kLogTag[] = L"[gamapass-uia]";

HttpLoginResult Fail(HttpLoginError e, const std::string& msg) {
    HttpLoginResult r;
    r.ok = false;
    r.error = e;
    r.message = msg;
    return r;
}

HttpLoginResult FailOauth(const std::string& msg) {
    HttpLoginResult r = Fail(HttpLoginError::Network, msg);
    r.accountsOauthError = true;
    return r;
}

void Log(const HttpLoginLogFn& log, const std::wstring& s) {
    if (log) log(s);
}

bool LaunchDailyBrowser(const std::wstring& exe, const std::wstring& url, DWORD* outPid,
                        const HttpLoginLogFn& log, std::vector<HWND>* outBeforeHwnds) {
    if (outPid) *outPid = 0;
    if (exe.empty() || url.empty()) return false;

    // ★ 启动前快照：日常已开多窗时，只认 CreateProcess/--new-window 之后新出现的顶层窗
    if (outBeforeHwnds) *outBeforeHwnds = msc::uia::EnumBrowserTopHwnds();

    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\" --force-renderer-accessibility --new-window \"";
    cmd += url;
    cmd += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    Log(log, std::wstring(kLogTag) + L" 启动日常浏览器（无调试口、无副本）… beforeWindows=" +
                 std::to_wstring(outBeforeHwnds ? outBeforeHwnds->size() : 0));
    if (!CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        Log(log, std::wstring(kLogTag) + L" CreateProcess 失败 err=" +
                     std::to_wstring(GetLastError()));
        return false;
    }
    if (outPid) *outPid = pi.dwProcessId;
    Log(log, std::wstring(kLogTag) + L" 已拉起 pid=" + std::to_wstring(pi.dwProcessId));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

HWND WaitBrowserHwnd(DWORD launchPid, int waitMs, const HttpLoginLogFn& log, DWORD* outAttachPid,
                     const std::vector<HWND>& beforeLaunch) {
    if (outAttachPid) *outAttachPid = launchPid;
    const DWORD t0 = GetTickCount();
    const std::vector<std::wstring> keysPrimary = {
        L"galaxy.games.gamania",
        L"accounts.gamania",
        L"login.beanfun",
        L"maplestoryclassic.beanfun",
        L"Gama Pass",
        L"login/init",
        L"login/mstc",
        L"select-account",
        L"OTT:",
    };

    HANDLE hProc = nullptr;
    if (launchPid) {
        hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, launchPid);
        if (!hProc) {
            Log(log, std::wstring(kLogTag) + L" 启动 pid=" + std::to_wstring(launchPid) +
                         L" 已不可打开（多半单例移交），将优先认「启动后新窗」");
        }
    }

    bool loggedNew = false;
    while ((int)(GetTickCount() - t0) < waitMs) {
        HWND h = nullptr;

        // 0) 启动 pid 仍存活时：优先其顶层新窗（杀光旧 Chrome 后 CreateProcess pid 即主进程）
        if (!h && launchPid) {
            HANDLE alive = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, launchPid);
            if (alive) {
                CloseHandle(alive);
                HWND cand = msc::uia::FindBrowserMainHwnd(launchPid, nullptr);
                if (cand) {
                    bool wasBefore = false;
                    for (HWND b : beforeLaunch) {
                        if (b == cand) {
                            wasBefore = true;
                            break;
                        }
                    }
                    if (!wasBefore) {
                        h = cand;
                        if (!loggedNew) {
                            loggedNew = true;
                            Log(log, std::wstring(kLogTag) + L" 优先附着启动 pid 新窗");
                        }
                    }
                }
            }
        }

        // 1) ★ 优先：相对启动前快照的新顶层窗（避免挂到日常多标签旧窗）
        if (!h) {
            h = msc::uia::FindNewBrowserHwnd(beforeLaunch, keysPrimary, nullptr);
            if (h && !loggedNew) {
                loggedNew = true;
                Log(log, std::wstring(kLogTag) + L" 优先附着启动后新窗（非日常旧窗）");
            }
        }

        // 2) 强标题，但必须是启动后新窗（禁止挂到日常里已开着的 Galaxy 旧标签窗）
        if (!h) {
            HWND cand =
                msc::uia::FindBrowserHwndByTitleKeywords(keysPrimary, nullptr, /*restrictPid=*/0);
            if (cand) {
                bool wasBefore = false;
                for (HWND b : beforeLaunch) {
                    if (b == cand) {
                        wasBefore = true;
                        break;
                    }
                }
                if (!wasBefore) {
                    h = cand;
                    if (!loggedNew) {
                        loggedNew = true;
                        Log(log, std::wstring(kLogTag) + L" 优先附着启动后新窗（标题命中）");
                    }
                }
            }
        }

        // 2b) 多新窗且标题仍空：超过 2.5s 取前台/Z 序新窗，避免空等超时
        if (!h && (int)(GetTickCount() - t0) > 2500) {
            const auto nowHwnds = msc::uia::EnumBrowserTopHwnds();
            std::vector<HWND> news;
            for (HWND c : nowHwnds) {
                bool known = false;
                for (HWND b : beforeLaunch) {
                    if (b == c) {
                        known = true;
                        break;
                    }
                }
                if (!known) news.push_back(c);
            }
            HWND fg = GetForegroundWindow();
            for (HWND c : news) {
                if (c == fg) {
                    h = c;
                    break;
                }
            }
            if (!h && news.size() == 1) h = news.front();
            if (!h && !news.empty()) h = news.front();
            if (h && !loggedNew) {
                loggedNew = true;
                Log(log, std::wstring(kLogTag) + L" 优先附着启动后新窗（超时兜底）");
            }
        }

        // 3) 仅当新窗尚未出现时，才用 launchPid 顶层窗——且必须是「启动后新出现」的 hwnd
        if (!h && launchPid) {
            HWND cand = msc::uia::FindBrowserMainHwnd(launchPid, nullptr);
            if (cand) {
                bool wasBefore = false;
                for (HWND b : beforeLaunch) {
                    if (b == cand) {
                        wasBefore = true;
                        break;
                    }
                }
                if (!wasBefore) h = cand;
                else if ((int)(GetTickCount() - t0) > 2000 && !loggedNew) {
                    Log(log, std::wstring(kLogTag) +
                                 L" 跳过 CreateProcess pid 上的旧日常窗，继续等 --new-window 新窗…");
                    loggedNew = true;  // 只打一次
                }
            }
        }

        if (h) {
            DWORD attachPid = 0;
            GetWindowThreadProcessId(h, &attachPid);
            if (outAttachPid && attachPid) *outAttachPid = attachPid;
            wchar_t title[512]{};
            GetWindowTextW(h, title, 512);
            msc::uia::BringToForeground(h);
            Log(log, std::wstring(kLogTag) + L" 已附着浏览器窗 hwnd=" +
                         std::to_wstring((uintptr_t)h) + L" pid=" + std::to_wstring(attachPid) +
                         L" title=" + std::wstring(title).substr(0, 80));
            if (hProc) CloseHandle(hProc);
            return h;
        }
        Sleep(80);
    }
    if (hProc) CloseHandle(hProc);
    Log(log, std::wstring(kLogTag) +
                 L" 等待浏览器窗口超时（未找到相对启动前的新 Chrome 窗 / Galaxy 标题）");
    return nullptr;
}

void SoftCloseHwnd(HWND hwnd, const HttpLoginLogFn& log) {
    if (!hwnd || !IsWindow(hwnd)) return;
    Log(log, std::wstring(kLogTag) + L" 关闭本轮登录窗（WM_CLOSE）…");
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

enum class UiaStage : int {
    WaitGp = 0,
    WaitAcc,
    WaitNick,
    WaitContinue,
    WaitTicket,
    ManualLogin,
};

const wchar_t* UiaStageName(UiaStage s) {
    switch (s) {
        case UiaStage::WaitGp: return L"WaitGp";
        case UiaStage::WaitAcc: return L"WaitAcc";
        case UiaStage::WaitNick: return L"WaitNick";
        case UiaStage::WaitContinue: return L"WaitContinue";
        case UiaStage::WaitTicket: return L"WaitTicket";
        case UiaStage::ManualLogin: return L"ManualLogin";
    }
    return L"?";
}

bool LooksLikeLoginForm(msc::uia::Session& uia, IUIAutomationElement* root) {
    if (!root) return false;
    // 繁体站主文案「密碼」；简中/英仅兜底（勿单独用「使用其他帳號」误伤选号页）
    return uia.NameContains(root, L"密碼", true) || uia.NameContains(root, L"Password", true) ||
           uia.NameContains(root, L"密码", true);
}

bool LooksLikeSelectAccount(msc::uia::Session& uia, IUIAutomationElement* root) {
    if (!root) return false;
    if (uia.NameContains(root, L"select-account", true)) return true;
    if (uia.NameContains(root, L"選擇帳號", true) || uia.NameContains(root, L"Select account", true) ||
        uia.NameContains(root, L"选择账号", true))
        return true;
    // 须同时有邮箱样式可点项 + Gamania/帳號语境，避免 Chrome 設定页误判
    if (!uia.NameContains(root, L"gamania", true) && !uia.NameContains(root, L"Gama", true) &&
        !uia.NameContains(root, L"帳號", true) && !uia.NameContains(root, L"account", true) &&
        !uia.NameContains(root, L"账号", true))
        return false;
    auto hits = uia.CollectNamedHits(root, {L"@"}, true);
    return hits.size() >= 1 && !LooksLikeLoginForm(uia, root);
}

// 选账号页强信号（不依赖暱稱判定，避免循环）——文案以繁体为准
bool HasSelectAccountChrome(msc::uia::Session& uia, IUIAutomationElement* root) {
    if (!root) return false;
    return uia.NameContains(root, L"使用 Gama Pass 登入", true) ||
           uia.NameContains(root, L"使用其他帳號登入", true) ||
           uia.NameContains(root, L"select-account", true) ||
           uia.NameContains(root, L"使用其他账号登录", true);
}

bool LooksLikeNickPage(msc::uia::Session& uia, IUIAutomationElement* root) {
    if (!root) return false;
    // 帳號卡页优先：标题/其它帳號链接在树里时绝不当暱稱页
    if (HasSelectAccountChrome(uia, root)) return false;
    if (uia.NameContains(root, L"使用其他帳號", true) ||
        uia.NameContains(root, L"使用其他账号", true))
        return false;
    // 繁体：遊戲暱稱 / 選擇遊戲暱稱 / 建立遊戲暱稱
    const bool nickLabel = uia.NameContains(root, L"遊戲暱稱", true) ||
                           uia.NameContains(root, L"選擇遊戲暱稱", true) ||
                           uia.NameContains(root, L"游戏昵称", true) ||
                           uia.NameContains(root, L"选择游戏昵称", true);
    const bool createNick = uia.NameContains(root, L"建立遊戲暱稱", true) ||
                            uia.NameContains(root, L"建立游戏昵称", true);
    return nickLabel || createNick;
}

bool LooksLikeSelectAccountPage(msc::uia::Session& uia, IUIAutomationElement* root) {
    if (!root) return false;
    if (HasSelectAccountChrome(uia, root)) return true;
    if (LooksLikeNickPage(uia, root)) return false;
    return LooksLikeSelectAccount(uia, root);
}

bool LooksLikeAccountsOauthError(msc::uia::Session& uia, IUIAutomationElement* root, HWND hwnd) {
    wchar_t title[512]{};
    if (hwnd && IsWindow(hwnd)) GetWindowTextW(hwnd, title, 512);
    if (title[0] && StrStrIW(title, L"accounts.gamania") && StrStrIW(title, L"error")) return true;
    if (!root) return false;
    if (uia.NameContains(root, L"登入流程逾時", true) ||
        uia.NameContains(root, L"登录流程超时", true) ||
        uia.NameContains(root, L"01004", true))
        return true;
    if ((uia.NameContains(root, L"發生錯誤", true) || uia.NameContains(root, L"发生错误", true) ||
         uia.NameContains(root, L"Something went wrong", true)) &&
        (uia.NameContains(root, L"gamania", true) || uia.NameContains(root, L"Gama", true) ||
         (title[0] && StrStrIW(title, L"gamania"))))
        return true;
    return false;
}

bool IsJunkClickName(const std::wstring& name) {
    if (name.empty()) return true;
    // 账号卡日志会拼 |WxH|t…|v…|how；只判名字段，别把诊断后缀当垃圾（BIN 07:49：
    // 点卡成功 → size>80 被拒 → lastAccClickAt 未记 → 40ms 狂点账号列表）
    const size_t pipe = name.find(L'|');
    const std::wstring stem = (pipe == std::wstring::npos) ? name : name.substr(0, pipe);
    if (stem.empty()) return true;
    const wchar_t* junk[] = {
        L"Google", L"Chrome", L"崩潰", L"崩溃", L"統計", L"统计", L"使用情況", L"使用情况",
        L"報告",   L"报告",   L"cookie", L"Cookie", L"隱私", L"隐私", L"設定", L"设置",
        L"Settings", L"擴充", L"扩展", L"書籤", L"书签", L"新分頁", L"新标签", L"網址",
        L"位址",   L"地址",   L"搜尋", L"搜索", L"還原", L"还原", L"最大化", L"最小化",
        L"關閉",   L"关闭",   L"Close",
    };
    for (const wchar_t* j : junk) {
        if (stem.find(j) != std::wstring::npos) return true;
    }
    return stem.size() > 80;  // 过长多半是说明文字/复选框文案
}

bool LooksLikeGalaxyReady(msc::uia::Session& uia, IUIAutomationElement* root) {
    if (!root) return false;
    // 优先完整 CTA；仅有文案「Gama Pass」时按钮可能尚未可点（BIN 02:54 首点空枪）
    if (uia.NameContains(root, L"Sign in with Gama Pass", true)) return true;
    if (uia.NameContains(root, L"Sign in with", true) &&
        (uia.NameContains(root, L"Gama Pass", true) || uia.NameContains(root, L"GamaPass", true)))
        return true;
    return false;
}

enum class UrlKind : int {
    Unknown = 0,
    GalaxyLogin,     // galaxy …/login/mstc
    SelectAccount,   // accounts … select-account
    NickOrGame,      // 选昵称 / SelectGameAccount
    OauthError,      // accounts … /error
    ResultOrMain,    // result/mstc 或经典官网 Main
    OtherAuth,       // 其它 gamania/beanfun 过渡页
};

const wchar_t* UrlKindName(UrlKind k) {
    switch (k) {
        case UrlKind::GalaxyLogin: return L"GalaxyLogin";
        case UrlKind::SelectAccount: return L"SelectAccount";
        case UrlKind::NickOrGame: return L"NickOrGame";
        case UrlKind::OauthError: return L"OauthError";
        case UrlKind::ResultOrMain: return L"ResultOrMain";
        case UrlKind::OtherAuth: return L"OtherAuth";
        default: return L"Unknown";
    }
}

std::wstring ToLowerCopy(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// maple 仅在「当前页主机」时算 Result；redirect_url= 查询值里的 maple 不算
bool MapleIsDocumentHost(const std::wstring& uLower) {
    const size_t posMaple = uLower.find(L"maplestoryclassic.beanfun");
    if (posMaple == std::wstring::npos) return false;
    // 同串还带 galaxy 主机 → 一定是 Galaxy 页（maple 只在 redirect）
    if (uLower.find(L"galaxy.games.gamania") != std::wstring::npos) return false;
    const size_t posRedirect = uLower.find(L"redirect_url");
    if (posRedirect != std::wstring::npos && posMaple > posRedirect) return false;
    return true;
}

UrlKind ClassifyUrlHint(const std::wstring& hint) {
    if (hint.empty()) return UrlKind::Unknown;
    const std::wstring u = ToLowerCopy(hint);
    auto has = [&](const wchar_t* p) { return u.find(p) != std::wstring::npos; };

    if (has(L"accounts.gamania") && (has(L"/error") || has(L"error?"))) return UrlKind::OauthError;
    if (has(L"select-account") || has(L"selectaccount")) return UrlKind::SelectAccount;
    // 禁用裸 nickname/gamename/gameaccount：标题/隐私文案易误伤 → 假 NickOrGame
    if (has(L"selectgameaccount") || has(L"gamapasslogin/select")) return UrlKind::NickOrGame;

    // Galaxy 启动页常带 ?redirect_url=https://maplestoryclassic.beanfun...
    // BIN 01:09：若先匹配 maplestoryclassic → 误判 ResultOrMain → 跳过点 Gama Pass
    if (has(L"galaxy.games.gamania")) {
        if (has(L"login/result") || has(L"access_token=")) return UrlKind::ResultOrMain;
        return UrlKind::GalaxyLogin;
    }
    if (has(L"login/result") || has(L"access_token=")) return UrlKind::ResultOrMain;
    if (MapleIsDocumentHost(u)) return UrlKind::ResultOrMain;
    if (has(L"使用 gama pass 登入") || has(L"使用其他帳號")) return UrlKind::SelectAccount;
    if (has(L"遊戲暱稱") || has(L"選擇遊戲暱稱") || has(L"游戏昵称")) return UrlKind::NickOrGame;
    if (has(L"accounts.gamania") || has(L"openid.beanfun") || has(L"login.beanfun"))
        return UrlKind::OtherAuth;
    return UrlKind::Unknown;
}

// 自动登录全程保前台+最大化：每轮轮询强制还原/最大化+置顶（ClickPoint 依赖屏幕坐标）
bool KeepLoginBrowserForeground(HWND hwnd, const HttpLoginLogFn& log, DWORD* lastFgLog) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    const bool wasIconic = IsIconic(hwnd) != FALSE;
    const bool notMax = IsZoomed(hwnd) == FALSE;
    const HWND fore = GetForegroundWindow();
    const bool notFore = (fore != hwnd);

    if (wasIconic || notMax || notFore || !msc::uia::IsBrowserWindowInteractive(hwnd)) {
        msc::uia::BringToForeground(hwnd);
        if (lastFgLog) {
            const DWORD now = GetTickCount();
            // 最小化/未最大化必打日志；丢前台节流，避免刷屏
            if (wasIconic || notMax || now - *lastFgLog > 3000) {
                *lastFgLog = now;
                if (wasIconic)
                    Log(log, std::wstring(kLogTag) + L" 登录窗被最小化，已强制最大化并置前");
                else if (notMax)
                    Log(log, std::wstring(kLogTag) + L" 登录窗未最大化，已强制最大化并置前");
                else
                    Log(log, std::wstring(kLogTag) + L" 登录窗不在前台，已强制置前");
            }
        }
    }
    // 即便已在前台且最大化，也周期性再顶一次，防止被 xcat/其它窗抢走
    else if (lastFgLog) {
        const DWORD now = GetTickCount();
        if (now - *lastFgLog > 4000) {
            *lastFgLog = now;
            msc::uia::BringToForeground(hwnd);
        }
    }

    return msc::uia::IsBrowserWindowInteractive(hwnd);
}

}  // namespace

static HttpLoginResult HttpGamaPassUiaLoginToOttOnce(HttpLoginLogFn log, int timeoutMs) {
    const int nickSlot = GetGamaPassNickSlot();
    const int accountSlot = GetGamaPassAccountSlot();
    Log(log, std::wstring(kLogTag) + L" 开始：日常浏览器 UIA 点选（不调用 refresh/token）；账号=" +
                 std::to_wstring(accountSlot) + L" 昵称=" + std::to_wstring(nickSlot));
    Log(log, std::wstring(kLogTag) +
                 L" 自动登录期间将强制保持浏览器登录窗最大化并在前台（最小化/窗口化/被挡都会自动拉回）。");
    Log(log, std::wstring(kLogTag) +
                 L" 快轮询+兜底：clickPoll=40ms cooldown=150ms；GP/账号卡/昵称/繼續均可重试；"
                 L"校验地址栏/标题阶段");

    std::wstring exe;
    if (!HttpGamaPassPreferredBrowserExe(exe) || exe.empty()) {
        return Fail(HttpLoginError::BadInput,
                    "未找到 Chrome/Edge。请安装官方 Chrome、Edge 或 Chrome++ 后再试。");
    }
    Log(log, std::wstring(kLogTag) + L" 浏览器=" + exe);

    // ★ 不结束日常浏览器：靠启动前 hwnd 快照 + --new-window 只附着本轮新窗，避免误杀会话 Cookie。
    Log(log, std::wstring(kLogTag) +
                 L" 启动前不结束已开浏览器；用 --new-window + 快照只挂本轮登录窗（不清 Cookie）");

    DWORD launchPid = 0;
    std::vector<HWND> beforeHwnds;
    if (!LaunchDailyBrowser(exe, kGalaxyLogin, &launchPid, log, &beforeHwnds)) {
        return Fail(HttpLoginError::Network, "无法启动日常浏览器打开 Galaxy 登录页");
    }

    DWORD attachPid = launchPid;
    HWND hwnd = WaitBrowserHwnd(launchPid, 40000, log, &attachPid, beforeHwnds);
    if (!hwnd) {
        return Fail(HttpLoginError::Network,
                    "未找到浏览器窗口（UIA 附着失败）。请确认 Edge/Chrome 能弹出新窗打开 Galaxy 后重试一键；"
                    "不会关闭已开浏览器。");
    }
    launchPid = attachPid ? attachPid : launchPid;
    DWORD lastFgLog = 0;
    KeepLoginBrowserForeground(hwnd, log, &lastFgLog);

    msc::uia::Session uia;
    if (!uia.Init([&](const std::wstring& s) { Log(log, s); })) {
        return Fail(HttpLoginError::Network, "UI Automation 初始化失败");
    }

    // 等 Galaxy/Gama Pass 进无障碍树：有控件立刻开点，最多 ~0.5s
    {
        const DWORD settleCap = GetTickCount() + 500;
        while ((int)(GetTickCount() - settleCap) < 0) {
            KeepLoginBrowserForeground(hwnd, log, &lastFgLog);
            IUIAutomationElement* probe = uia.ElementFromHwnd(hwnd);
            if (probe) {
                const bool readyUi = LooksLikeGalaxyReady(uia, probe) ||
                                    LooksLikeSelectAccountPage(uia, probe) ||
                                    LooksLikeNickPage(uia, probe);
                probe->Release();
                if (readyUi) break;
            }
            Sleep(30);
        }
    }

    const FILETIME sessionNotBefore = GamaPassSessionNotBeforeNow();
    bool sawNgmHint = false;
    bool gpClicked = false;
    bool accClicked = false;
    bool nickSelected = false;
    bool continueClicked = false;
    bool manualTipShown = false;
    DWORD lastClickAt = 0;
    DWORD lastStageLog = 0;
    DWORD lastUrlLog = 0;
    DWORD lastGpClickAt = 0;
    DWORD lastAccClickAt = 0;
    DWORD selectUrlSeenAt = 0;  // 地址栏真正到 select-account 的时刻（跳转在途不算）
    DWORD selectDomSeenAt = 0;  // 首次在 DOM 上看到选账号页（地址栏抖动时的兜底计时）
    bool accGateLogged = false;    // 一次性：首点被门禁挡住
    bool accNoCardLogged = false;  // 一次性：选账号页上没解析出卡
    DWORD lastNickClickAt = 0;
    DWORD lastContinueClickAt = 0;
    int gpRetryCount = 0;
    int accRetryCount = 0;
    int nickRetryCount = 0;
    int continueRetryCount = 0;
    UiaStage stage = UiaStage::WaitGp;
    std::wstring lastUrlHint;

    const DWORD t0 = GetTickCount();
    constexpr DWORD kClickCooldownMs = 150;
    constexpr DWORD kPollClickMs = 40;
    constexpr DWORD kPollTicketMs = 100;
    constexpr DWORD kGpRetryMs = 900;         // BIN 02:54：仍停 Galaxy 时旧值 2.2s 空等偏慢
    constexpr DWORD kGpRetryTransitMs = 1800; // 已离开 Galaxy、过渡页稍宽
    constexpr DWORD kGpForceRetryMs = 4000;
    constexpr DWORD kUrlGalaxyGraceMs = 5500;  // 点 GP 后地址栏仍短暂 Galaxy，禁止误纠偏回 WaitGp
    constexpr DWORD kUrlNickGraceMs = 5500;    // 点繼續后地址栏仍短暂 SelectGameAccount
    constexpr DWORD kAccRetryMs = 2500;       // 点卡后仍停选账号页 → 重点卡
    // BIN 06:15/06:38：卡已进树但地址栏仍 oauth2/authorize，首点落在即将被替换的页上 → 必废
    constexpr DWORD kAccSettleMs = 320;       // 地址栏到 select-account 后再等一拍才首点
    constexpr DWORD kAccStarveMs = 1200;      // 地址栏读数抖动时的兜底：见页 1.2s 必点
    constexpr DWORD kNickRetryMs = 3000;      // 点昵称后未到繼續/跳转 → 重选
    constexpr DWORD kContinueRetryMs = 5000;  // BIN 03:14/03:23：首点后 2.5s 仍 Nick 多为跳转中，误重试
    constexpr DWORD kContinueRetryMsAgain = 2800;  // 第 2 次及以后仍未离开再重点
    constexpr DWORD kContinueReadyMs = 400;   // 选昵称后等按钮启用再点（BIN：同秒點繼續空成功）
    constexpr int kMaxGpRetry = 4;
    constexpr int kMaxAccRetry = 5;
    constexpr int kMaxNickRetry = 4;
    constexpr int kMaxContinueRetry = 4;

    auto tryHarvest = [&]() -> HttpLoginResult {
        return GamaPassTryHarvestClassicTicket(sessionNotBefore, sawNgmHint, log, kLogTag);
    };

    auto clickGamaPassButton = [&](IUIAutomationElement* rootEl, bool retry) -> bool {
        // 点前强制置前（即使纯 Invoke，前台也更稳）
        msc::uia::BringToForeground(hwnd);
        Sleep(30);

        // 首点：UIA Invoke；重试：forceMouse 真鼠标兜底
        const bool forceMouse = retry;
        const wchar_t* exacts[] = {L"Sign in with Gama Pass", L"Gama Pass", L"GamaPass",
                                   L"GAMA PASS"};
        for (const wchar_t* n : exacts) {
            if (!uia.ClickLargestExactName(rootEl, n, forceMouse)) continue;
            // 首点再 Invoke 一次压偶发空枪（仍不动鼠标）
            if (!retry) {
                Sleep(80);
                (void)uia.ClickLargestExactName(rootEl, n, /*forceMouse=*/false);
            }
            gpClicked = true;
            lastClickAt = GetTickCount();
            lastGpClickAt = lastClickAt;
            if (retry) ++gpRetryCount;
            stage = UiaStage::WaitAcc;
            Log(log, std::wstring(kLogTag) + (retry ? L" click-gamapass|retry|" : L" click-gamapass|") +
                         n + (retry ? (L"#" + std::to_wstring(gpRetryCount)) : L"") +
                         (forceMouse ? L"|mouse" : L"|uia"));
            Log(log, std::wstring(kLogTag) +
                         (retry ? L" 状态→WaitAcc（retry-gp，选账号未出现）"
                                : L" 状态→WaitAcc（clicked-gp）"));
            return true;
        }
        const wchar_t* fuzzy[] = {L"Sign in with Gama Pass", L"Gama Pass", L"GamaPass", L"GAMAPASS",
                                  L"Gama pass", L"GAMA PASS"};
        for (const wchar_t* n : fuzzy) {
            if (!uia.ClickName(rootEl, n, true, forceMouse)) continue;
            if (!retry) {
                Sleep(80);
                (void)uia.ClickName(rootEl, n, true, false);
            }
            gpClicked = true;
            lastClickAt = GetTickCount();
            lastGpClickAt = lastClickAt;
            if (retry) ++gpRetryCount;
            stage = UiaStage::WaitAcc;
            Log(log, std::wstring(kLogTag) + (retry ? L" click-gamapass|retry|" : L" click-gamapass|") +
                         n + (retry ? (L"#" + std::to_wstring(gpRetryCount)) : L"") +
                         (forceMouse ? L"|mouse" : L"|uia"));
            Log(log, std::wstring(kLogTag) +
                         (retry ? L" 状态→WaitAcc（retry-gp，选账号未出现）"
                                : L" 状态→WaitAcc（clicked-gp）"));
            return true;
        }
        return false;
    };

    auto stagePollMs = [&]() -> DWORD {
        return (stage == UiaStage::WaitTicket) ? kPollTicketMs : kPollClickMs;
    };

    while ((int)(GetTickCount() - t0) < timeoutMs) {
        {
            auto harvested = tryHarvest();
            if (harvested.ok && harvested.ticketFilled) {
                SoftCloseHwnd(hwnd, log);
                return harvested;
            }
        }

        if (!IsWindow(hwnd)) {
            DWORD ap = launchPid;
            // 中途丢窗：用空快照，避免误把「一直存在的日常窗」当新窗；仍优先强标题
            hwnd = WaitBrowserHwnd(launchPid, 8000, log, &ap, std::vector<HWND>{});
            if (ap) launchPid = ap;
            if (!hwnd) {
                Sleep(kPollClickMs);
                continue;
            }
            KeepLoginBrowserForeground(hwnd, log, &lastFgLog);
        }

        // 自动登录全程：每轮强制保前台（先于 UIA 读树/点选）
        const bool ready = KeepLoginBrowserForeground(hwnd, log, &lastFgLog);

        IUIAutomationElement* root = uia.ElementFromHwnd(hwnd);
        if (!root) {
            Sleep(stagePollMs());
            continue;
        }

        const DWORD now = GetTickCount();
        const bool canClick = ready && (now - lastClickAt) >= kClickCooldownMs;

        // 地址栏 / 标题阶段校验（与无障碍文案双保险）
        {
            const std::wstring urlHint = uia.ReadUrlHint(root, hwnd);
            const UrlKind urlKind = ClassifyUrlHint(urlHint);
            if ((!urlHint.empty() && urlHint != lastUrlHint) || now - lastUrlLog > 3000) {
                lastUrlLog = now;
                if (!urlHint.empty()) lastUrlHint = urlHint;
                Log(log, std::wstring(kLogTag) + L" 页面|" + UrlKindName(urlKind) + L"|@" +
                             UiaStageName(stage) + L"| " +
                             (urlHint.empty() ? L"(无URL)" : urlHint.substr(0, 96)));
            }
            if (urlKind == UrlKind::OauthError ||
                LooksLikeAccountsOauthError(uia, root, hwnd)) {
                Log(log, std::wstring(kLogTag) + L" 检测到 accounts/error（OAuth 失败），停止点选");
                root->Release();
                SoftCloseHwnd(hwnd, log);
                return FailOauth("Gama Pass OAuth 失败（accounts/error）。"
                                 "请重新一键；在日常浏览器窗口内登录并勾选记住（不会清 Cookie）。");
            }
            // URL 纠偏：阶段与真实页不一致时拉回
            // BIN 23:09：点 GP 后地址栏仍短暂 GalaxyLogin，立刻纠偏→WaitGp 会误再点一次 GP
            // BIN 01:37：点繼續后地址栏仍短暂 NickOrGame，纠偏→WaitNick 会卡死
            // BIN 01:40：WaitAcc 仍停 Galaxy 时纠偏→WaitGp 会提前再点 GP，打断尚未完成的跳转
            const bool gpNavGrace =
                lastGpClickAt && (now - lastGpClickAt) < kUrlGalaxyGraceMs;
            const bool continueNavGrace =
                lastContinueClickAt && (now - lastContinueClickAt) < kUrlNickGraceMs;
            // WaitAcc 停在 Galaxy = 点 GP 后等跳转；重试交给下方 kGpRetryMs，勿 URL 纠偏抢点
            if (urlKind == UrlKind::GalaxyLogin &&
                (stage == UiaStage::WaitNick || stage == UiaStage::WaitContinue ||
                 stage == UiaStage::WaitTicket) &&
                !LooksLikeSelectAccountPage(uia, root) && !LooksLikeNickPage(uia, root) &&
                !gpNavGrace) {
                stage = UiaStage::WaitGp;
                gpClicked = false;
                accClicked = false;
                nickSelected = false;
                continueClicked = false;
                Log(log, std::wstring(kLogTag) + L" URL=GalaxyLogin，纠偏→WaitGp（可重试 GP）");
            } else if (urlKind == UrlKind::SelectAccount &&
                       (stage == UiaStage::WaitGp || stage == UiaStage::WaitNick ||
                        stage == UiaStage::WaitContinue || stage == UiaStage::WaitTicket ||
                        stage == UiaStage::ManualLogin)) {
                stage = UiaStage::WaitAcc;
                gpClicked = true;
                nickSelected = false;
                continueClicked = false;
                Log(log, std::wstring(kLogTag) + L" URL=SelectAccount，纠偏→WaitAcc");
            } else if (urlKind == UrlKind::NickOrGame &&
                       (stage == UiaStage::WaitGp || stage == UiaStage::WaitAcc) &&
                       !continueNavGrace) {
                // 不从 WaitTicket 纠偏回 WaitNick：點繼續后 URL 常短暂仍为 SelectGameAccount
                stage = UiaStage::WaitNick;
                accClicked = true;
                gpClicked = true;
                Log(log, std::wstring(kLogTag) + L" URL=NickOrGame，纠偏→WaitNick");
            } else if (urlKind == UrlKind::ResultOrMain && stage != UiaStage::WaitTicket) {
                stage = UiaStage::WaitTicket;
                continueClicked = true;
                Log(log, std::wstring(kLogTag) + L" URL=ResultOrMain，纠偏→WaitTicket");
            }
        }

        // 硬门禁：选账号页优先于昵称页（禁止未离卡就进 WaitNick）
        if (LooksLikeLoginForm(uia, root) && !LooksLikeSelectAccountPage(uia, root) &&
            !LooksLikeNickPage(uia, root)) {
            if (stage != UiaStage::ManualLogin && stage != UiaStage::WaitTicket) {
                stage = UiaStage::ManualLogin;
                Log(log, std::wstring(kLogTag) + L" 状态→ManualLogin");
            }
        } else if (LooksLikeSelectAccountPage(uia, root) &&
                   (stage == UiaStage::WaitGp || stage == UiaStage::WaitAcc ||
                    stage == UiaStage::WaitNick || stage == UiaStage::ManualLogin)) {
            if (stage != UiaStage::WaitAcc) {
                stage = UiaStage::WaitAcc;
                nickSelected = false;
                continueClicked = false;
                Log(log, std::wstring(kLogTag) + L" 状态→WaitAcc（选账号页）");
            }
            if (accClicked) {
                accClicked = false;
                Log(log, std::wstring(kLogTag) + L" 仍在选账号页，撤回 accClicked，重试点卡");
            }
        } else if (LooksLikeNickPage(uia, root)) {
            if (!accClicked) {
                accClicked = true;
                gpClicked = true;
                Log(log, std::wstring(kLogTag) + L" 已进入选昵称页，确认账号卡跳转成功");
            }
            if (stage != UiaStage::WaitNick && stage != UiaStage::WaitContinue &&
                stage != UiaStage::WaitTicket) {
                stage = UiaStage::WaitNick;
                Log(log, std::wstring(kLogTag) + L" 状态→WaitNick（已确认离开账号卡页）");
            }
        }

        if (stage == UiaStage::ManualLogin) {
            if (!manualTipShown) {
                manualTipShown = true;
                Log(log, std::wstring(kLogTag) +
                             L" 请在本日常浏览器窗口内登录 Gama Pass 并勾选记住；"
                             L"出现选账号后会继续自动点选。未调用 refresh、未清 Cookie。");
            }
            if (now - lastStageLog > 8000) {
                lastStageLog = now;
                Log(log, std::wstring(kLogTag) + L" ManualLogin：等待本窗登录…");
            }
            root->Release();
            Sleep(stagePollMs());
            continue;
        }

        if (!ready && stage != UiaStage::WaitTicket && now - lastStageLog > 5000) {
            lastStageLog = now;
            Log(log, std::wstring(kLogTag) + L" 登录窗仍不可见/最小化，持续强制还原置前…");
        }

        if (canClick && stage == UiaStage::WaitGp && !gpClicked) {
            if (LooksLikeSelectAccountPage(uia, root)) {
                gpClicked = true;
                lastGpClickAt = now;
                stage = UiaStage::WaitAcc;
                Log(log, std::wstring(kLogTag) + L" 已在选账号页，跳过 GP 点击 →WaitAcc");
            } else if (!LooksLikeGalaxyReady(uia, root)) {
                // 页面 URL 到了但按钮未进树：空点会「成功」却不跳转，再空等重试（BIN 02:17）
                if (now - lastStageLog > 1500) {
                    lastStageLog = now;
                    Log(log, std::wstring(kLogTag) + L" 等待 Gama Pass 按钮就绪…");
                }
            } else if (!clickGamaPassButton(root, /*retry=*/false)) {
                if (now - lastStageLog > 2000) {
                    lastStageLog = now;
                    Log(log, std::wstring(kLogTag) + L" 等待 Gama Pass 按钮… @" +
                                 UiaStageName(stage));
                }
            }
        } else if (canClick && stage == UiaStage::WaitAcc && !accClicked) {
            const bool onSelect = LooksLikeSelectAccountPage(uia, root);
            const bool onNick = LooksLikeNickPage(uia, root);
            const DWORD sinceGp = lastGpClickAt ? (now - lastGpClickAt) : (now - lastClickAt);
            const UrlKind urlNow = ClassifyUrlHint(uia.ReadUrlHint(root, hwnd));
            const bool urlStillGalaxy = (urlNow == UrlKind::GalaxyLogin);
            // URL 仍 Galaxy 时尽快重试；其它过渡页稍宽
            const DWORD gpRetryNeed = urlStillGalaxy ? kGpRetryMs : kGpRetryTransitMs;

            // BIN：最小化打断 / 首点无效后仍停 Galaxy → 尽快重试 GP
            if (!onSelect && !onNick && sinceGp >= gpRetryNeed) {
                const bool stillOnGalaxy =
                    urlStillGalaxy || LooksLikeGalaxyReady(uia, root) ||
                    uia.NameContains(root, L"Sign in with", true);
                if (stillOnGalaxy || sinceGp >= kGpForceRetryMs) {
                    if (gpRetryCount >= kMaxGpRetry) {
                        if (now - lastStageLog > 4000) {
                            lastStageLog = now;
                            Log(log, std::wstring(kLogTag) + L" GP 重试已达上限，仍等选账号页…");
                        }
                    } else {
                    Log(log, std::wstring(kLogTag) +
                                 L" 选账号页未出现，重试 Gama Pass… sinceGp=" +
                                 std::to_wstring(sinceGp) + L"ms");
                    gpClicked = false;
                    if (clickGamaPassButton(root, /*retry=*/true)) {
                        root->Release();
                        Sleep(stagePollMs());
                        continue;
                    }
                    stage = UiaStage::WaitGp;
                    lastClickAt = now;
                    root->Release();
                    Sleep(stagePollMs());
                    continue;
                    }
                }
            }

            // 跳转在途（URL 还在 authorize/其它过渡页）时卡片已可见但点了必废：
            // 等地址栏真到 select-account 且稳定一拍再首点（BIN 06:15/06:38 的 v0 全废）
            if (urlNow == UrlKind::SelectAccount && !selectUrlSeenAt) selectUrlSeenAt = now;
            if (onSelect && !selectDomSeenAt) selectDomSeenAt = now;
            // 计时器只记不清；settle 不再要求「当前帧仍是 SelectAccount」（URL/标题抖动会饿死首点）
            // DOM 见页兜底：最迟 kAccStarveMs 必点（BIN 07:13/07:28）
            const bool accSettled =
                (selectUrlSeenAt && (now - selectUrlSeenAt) >= kAccSettleMs) ||
                (selectDomSeenAt && (now - selectDomSeenAt) >= kAccStarveMs);

            // 点卡成功后留时间给跳转；超时仍停选账号页再重点
            const bool allowAccClick =
                (!lastAccClickAt || (now - lastAccClickAt) >= kAccRetryMs) &&
                (lastAccClickAt || accSettled);
            if (onSelect && lastAccClickAt && allowAccClick && accRetryCount < kMaxAccRetry &&
                now - lastStageLog > 0) {
                if (now - lastAccClickAt >= kAccRetryMs)
                    Log(log, std::wstring(kLogTag) + L" 账号卡点击后未跳转，重试点卡 #" +
                                 std::to_wstring(accRetryCount + 1));
            }

            std::wstring hitName;
            const int idx = (std::max)(0, accountSlot - 1);
            bool clicked = false;
            // 选账号页：紧单卡尺寸过滤 + 几何点击；日志带 WxH 便于核对选区
            if (allowAccClick && onSelect) {
                clicked = uia.ClickAccountCardIndex(root, idx, &hitName, accRetryCount);
                // 只要选出了候选并尝试过激活（hitName 非空），就必须记 cooldown——
                // 成功/失败/曾误 junk 都不许 40ms 狂点（BIN 07:49 + review High）
                if (clicked || !hitName.empty()) {
                    lastClickAt = now;
                    lastAccClickAt = now;
                    ++accRetryCount;
                }
                if (clicked) {
                    Log(log, std::wstring(kLogTag) + L" click-account-card|slot" +
                                 std::to_wstring(accountSlot) + L"|" + hitName.substr(0, 96) +
                                 L"|card（待确认跳转）");
                } else if (!hitName.empty()) {
                    Log(log, std::wstring(kLogTag) + L" click-account-card|fail|" +
                                 hitName.substr(0, 96) + L"（已记 cooldown）");
                } else if (!accNoCardLogged) {
                    accNoCardLogged = true;
                    Log(log, std::wstring(kLogTag) +
                                 L" 首点未发出：选账号页未解析出账号卡（种子/尺寸过滤全落空）");
                }
            } else if (onSelect && !allowAccClick && !accGateLogged) {
                accGateLogged = true;
                Log(log, std::wstring(kLogTag) + L" 首点被门禁挡住：url=" + UrlKindName(urlNow) +
                             L" urlAge=" +
                             std::to_wstring(selectUrlSeenAt ? (now - selectUrlSeenAt) : 0) +
                             L" domAge=" +
                             std::to_wstring(selectDomSeenAt ? (now - selectDomSeenAt) : 0));
            }
            if (!clicked && now - lastStageLog > 5000) {
                lastStageLog = now;
                if (onSelect)
                    Log(log, std::wstring(kLogTag) + L" 等待选账号卡片…settled=" +
                                 (accSettled ? L"1" : L"0") + L" url=" + UrlKindName(urlNow));
                else
                    Log(log, std::wstring(kLogTag) +
                                 L" 等待选账号页出现（仍可重试 Gama Pass）… sinceGp=" +
                                 std::to_wstring(sinceGp) + L"ms");
            }
        } else if (canClick && stage == UiaStage::WaitNick && !nickSelected) {
            std::wstring hitName;
            const int idx = (std::max)(0, nickSlot - 1);
            // SelectionItem / 点卡片左侧勾选圆；禁止点「ABC」文字中心（会拖选）
            bool clicked = uia.ClickTypeIndex(root, UIA_RadioButtonControlTypeId, idx, &hitName) ||
                           uia.ClickTypeIndex(root, UIA_ListItemControlTypeId, idx, &hitName) ||
                           uia.ClickTypeIndex(root, UIA_CheckBoxControlTypeId, idx, &hitName);
            if (clicked) {
                nickSelected = true;
                lastClickAt = now;
                lastNickClickAt = now;
                ++nickRetryCount;
                stage = UiaStage::WaitContinue;
                Log(log, std::wstring(kLogTag) + L" nick-selected|slot" +
                             std::to_wstring(nickSlot) + L"|" + hitName.substr(0, 40));
                Log(log, std::wstring(kLogTag) + L" 状态→WaitContinue");
            } else if (now - lastStageLog > 5000) {
                lastStageLog = now;
                Log(log, std::wstring(kLogTag) + L" 等待选昵称…");
            }
        } else if (stage == UiaStage::WaitNick && nickSelected) {
            // 兜底：已选昵称却停在 WaitNick（URL 误纠偏）→ 拉回点繼續
            stage = continueClicked ? UiaStage::WaitTicket : UiaStage::WaitContinue;
            Log(log, std::wstring(kLogTag) + L" WaitNick 已选昵称，纠偏→" + UiaStageName(stage));
        } else if (canClick && stage == UiaStage::WaitContinue) {
            // 點繼續后仍停昵称页 → 撤回再点（首点宽限更长，避免跳转中误重试）
            const DWORD contNeed = (continueRetryCount <= 1) ? kContinueRetryMs : kContinueRetryMsAgain;
            if (continueClicked && LooksLikeNickPage(uia, root) && lastContinueClickAt &&
                (now - lastContinueClickAt) >= contNeed &&
                continueRetryCount < kMaxContinueRetry) {
                Log(log, std::wstring(kLogTag) + L" 繼續后未跳转，重试繼續 #" +
                             std::to_wstring(continueRetryCount + 1) + L"（已等" +
                             std::to_wstring(now - lastContinueClickAt) + L"ms）");
                continueClicked = false;
                stage = UiaStage::WaitContinue;
                // 勿刷新 lastNickClickAt：否则又卡 kContinueReadyMs，且易打断进行中提交
            }

            if (!continueClicked) {
                // 选昵称后短暂等待「繼續」启用；过早点会假成功（BIN 01:43/02:05）
                if (lastNickClickAt && (now - lastNickClickAt) < kContinueReadyMs) {
                    root->Release();
                    Sleep(stagePollMs());
                    continue;
                }

                // 仅多次失败后再勾昵称；每次重试都重选可能打断首点已提交的跳转（BIN 03:23）
                if (continueRetryCount >= 2 && LooksLikeNickPage(uia, root)) {
                    const int idx = (std::max)(0, nickSlot - 1);
                    std::wstring ignore;
                    (void)uia.ClickTypeIndex(root, UIA_RadioButtonControlTypeId, idx, &ignore);
                    Sleep(150);
                }

                // 点前再置前
                msc::uia::BringToForeground(hwnd);

                const bool forceMouse = continueRetryCount > 0;
                auto clickContinueBtn = [&]() -> bool {
                    if (uia.ClickLargestExactName(root, L"繼續", forceMouse)) return true;
                    if (uia.ClickLargestExactName(root, L"Continue", forceMouse)) return true;
                    if (uia.ClickLargestExactName(root, L"確認", forceMouse)) return true;
                    return uia.ClickName(root, L"繼續", true, forceMouse) ||
                           uia.ClickName(root, L"確認", true, forceMouse) ||
                           uia.ClickName(root, L"Continue", true, forceMouse) ||
                           uia.ClickName(root, L"继续", true, forceMouse) ||
                           uia.ClickName(root, L"确认", true, forceMouse);
                };

                bool clicked = clickContinueBtn();
                if (clicked) {
                    continueClicked = true;
                    lastClickAt = now;
                    lastContinueClickAt = now;
                    ++continueRetryCount;
                    stage = UiaStage::WaitTicket;
                    Log(log, std::wstring(kLogTag) + L" nick-continue|slot" +
                                 std::to_wstring(nickSlot) + L"|#" +
                                 std::to_wstring(continueRetryCount) +
                                 (forceMouse ? L"|mouse" : L"|uia"));
                    Log(log, std::wstring(kLogTag) + L" 状态→WaitTicket");
                } else if (nickSelected && lastNickClickAt &&
                           (now - lastNickClickAt) >= kNickRetryMs &&
                           nickRetryCount < kMaxNickRetry && continueRetryCount == 0 &&
                           LooksLikeNickPage(uia, root)) {
                    Log(log, std::wstring(kLogTag) + L" 未出现繼續，重选昵称 #" +
                                 std::to_wstring(nickRetryCount + 1));
                    nickSelected = false;
                    stage = UiaStage::WaitNick;
                    lastClickAt = now;
                } else if (now - lastStageLog > 2000) {
                    lastStageLog = now;
                    Log(log, std::wstring(kLogTag) + L" 等待「繼續」…");
                }
            }
        } else if (stage == UiaStage::WaitTicket) {
            // WaitTicket 但 URL/文案仍像昵称页 → 拉回重试繼續（不要重选昵称）
            const DWORD contNeed = (continueRetryCount <= 1) ? kContinueRetryMs : kContinueRetryMsAgain;
            if ((LooksLikeNickPage(uia, root) ||
                 ClassifyUrlHint(uia.ReadUrlHint(root, hwnd)) == UrlKind::NickOrGame) &&
                continueClicked && lastContinueClickAt &&
                (now - lastContinueClickAt) >= contNeed &&
                continueRetryCount < kMaxContinueRetry) {
                Log(log, std::wstring(kLogTag) + L" WaitTicket 仍在昵称页，撤回繼續重试（已等" +
                             std::to_wstring(now - lastContinueClickAt) + L"ms）");
                continueClicked = false;
                stage = UiaStage::WaitContinue;
            } else if (now - lastStageLog > 8000) {
                lastStageLog = now;
                Log(log, sawNgmHint ? std::wstring(kLogTag) + L" TokenWait：已见 NGM，仍等经典版…"
                                    : std::wstring(kLogTag) + L" TokenWait：等待官网拉起经典版…");
            }
        }

        root->Release();
        Sleep(stagePollMs());
    }

    {
        auto harvested = tryHarvest();
        if (harvested.ok && harvested.ticketFilled) {
            SoftCloseHwnd(hwnd, log);
            return harvested;
        }
    }

    if (stage == UiaStage::ManualLogin) {
        SoftCloseHwnd(hwnd, log);
        return Fail(HttpLoginError::BadInput,
                    "Gama Pass 需要先在日常浏览器窗口内登录（勾选记住）。"
                    "程序没有调用 refresh、也没有清除 Cookie。请重新一键并在弹出窗完成登录。");
    }

    Log(log, std::wstring(kLogTag) + L" 超时 @" + UiaStageName(stage));
    SoftCloseHwnd(hwnd, log);
    return Fail(HttpLoginError::OttMissing, "UIA 点选超时，未捕获经典版 cmdline 票");
}

bool ShouldUiaCleanRestart(const HttpLoginResult& r) {
    if (r.ok) return false;
    // accounts/error、点选超时、附着失败（日常已开 Chrome 单例移交常见）→ 关窗/重开 Galaxy
    if (r.accountsOauthError) return true;
    if (r.error == HttpLoginError::OttMissing) return true;
    if (r.error == HttpLoginError::Network &&
        (r.message.find("UIA") != std::string::npos ||
         r.message.find("附着") != std::string::npos))
        return true;
    return false;
}

HttpLoginResult HttpGamaPassUiaLoginToOtt(HttpLoginLogFn log, int timeoutMs) {
    constexpr int kMaxCleanRestart = 3;  // 关登录窗后重开 Galaxy，最多 3 次
    HttpLoginResult last;
    for (int restart = 0;; ++restart) {
        last = HttpGamaPassUiaLoginToOttOnce(log, timeoutMs);
        if (last.ok || !ShouldUiaCleanRestart(last)) return last;
        if (restart >= kMaxCleanRestart) {
            Log(log, std::wstring(kLogTag) + L" 已达干净重开上限（" +
                         std::to_wstring(kMaxCleanRestart) +
                         L" 次），不再自动重试（请手动重新一键）");
            return last;
        }
        // ★ SoftClose 已在 Once 内做过；下一轮 Once 会再结束已开主进程后重开 Galaxy。
        // 不清 Cookie、不 refresh、不改 prompt。
        Log(log, std::wstring(kLogTag) + L" 失败收口：关闭登录窗后干净重开（uia-clean-restart " +
                     std::to_wstring(restart + 1) + L"/" + std::to_wstring(kMaxCleanRestart) +
                     L"）…");
        Sleep(1500);
    }
}

}  // namespace msc::launcher
