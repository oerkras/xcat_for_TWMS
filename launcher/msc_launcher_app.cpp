// 新楓之谷經典版 · 一键启动器（成品）
// 填入 账号----密码----… → 一键：点 gamania(HK) → 自动填账密 → 抓 OTT → 换票 → NGM

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <shlwapi.h>

#include <wrl.h>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include "msc_launch.h"
#include "ott_ticket_fetch.h"

#include <atomic>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

constexpr wchar_t kGalaxyLoginUrl[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/mstc"
    L"?redirect_url=https://maplestoryclassic.beanfun.com/Main";

// Galaxy 页「Sign in with gamania (HK)」实际跳转目标（日志多次证实）
constexpr wchar_t kBeanfunGhkLoginUrl[] =
    L"https://bfweb.hk.beanfun.com/beanfun_block/bflogin/default.aspx"
    L"?service=610076_T0"
    L"&url=https%3A%2F%2Fgalaxy.games.gamania.com%2Fwebapi%2Fview%2Flogin%2Fresult%2Fmstc%2Fghk";

constexpr UINT_PTR kBusyTimerId = 42;
constexpr UINT_PTR kAutoLoginTimerId = 43;
constexpr UINT kBusyTimeoutMs = 180000;
constexpr UINT kAutoLoginIntervalMs = 800;

enum : int {
    IDC_BTN_ONECLICK = 1001,
    IDC_BTN_CLEAR = 1002,
    IDC_EDIT_LOG = 1003,
    IDC_STATIC_HINT = 1004,
    IDC_STATIC_STATUS = 1005,
    IDC_WEBHOST = 1006,
    IDC_EDIT_ACCOUNT = 1007,
    IDC_STATIC_ACCOUNT = 1008,
};

struct AccountCred {
    std::wstring user;
    std::wstring pass;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND webHost = nullptr;
    HWND editLog = nullptr;
    HWND editAccount = nullptr;
    HWND btnOne = nullptr;
    HWND status = nullptr;

    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navStartToken{};
    EventRegistrationToken navDoneToken{};
    EventRegistrationToken sourceToken{};
    EventRegistrationToken frameToken{};
    HANDLE browserExitEvent = nullptr;
    bool closing = false;

    bool webReady = false;
    std::atomic_bool busy{false};
    std::atomic_bool ottConsumed{false};
    std::atomic_bool cookieCleared{false};
    std::atomic_bool providerClicked{false};
    std::atomic_bool formSubmitted{false};
    std::atomic_bool ssoRecoverTried{false};
    std::atomic<int> cookieAbsentStreak{0};
    DWORD lastProviderNavTick = 0;
    DWORD cookieWaitStartTick = 0;
    DWORD lastFillAttemptTick = 0;
    std::atomic_bool fillStarted{false};
    std::wstring resolvedBeanfunUrl;
    AccountCred cred;
    std::wstring lastAutoLoginStatus;
    DWORD lastWaitLogTick = 0;

    std::mutex frameMu;
    std::vector<ComPtr<ICoreWebView2Frame>> frames;

    std::mutex logMu;
    std::vector<std::wstring> pendingLogs;
    std::wstring logFilePath;
};

AppState g;

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

std::wstring AccountConfigPath() {
    return ExeDir() + L"\\account.txt";
}

std::wstring LegacyAccountConfigPath() {
    return LocalRuntimeDir() + L"\\account.txt";
}

std::wstring ProfileDir() {
    const std::wstring dir = LocalRuntimeDir() + L"\\webview_profile";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool IsPathOnRemoteDrive(const std::wstring& path) {
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.size() >= 2 && path[1] == L':') {
        const wchar_t root[] = {path[0], L':', L'\\', L'\0'};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

void SaveAccountConfig(const std::wstring& line) {
    std::wstring s = line;
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    const std::wstring path = AccountConfigPath();
    std::ofstream f(NarrowUtf8(path), std::ios::binary | std::ios::trunc);
    if (!f) return;
    const std::string utf8 = NarrowUtf8(s);
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

std::wstring ReadAccountFile(const std::wstring& path) {
    std::ifstream f(NarrowUtf8(path), std::ios::binary);
    if (!f) return {};
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ')) raw.pop_back();
    return WidenUtf8(raw);
}

std::wstring LoadAccountConfig() {
    std::wstring line = ReadAccountFile(AccountConfigPath());
    if (!line.empty()) return line;
    line = ReadAccountFile(LegacyAccountConfigPath());
    if (!line.empty()) SaveAccountConfig(line);
    return line;
}

std::string Base64Encode(const std::string& in) {
    static const char* kTbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kTbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kTbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// 账号串：user-pass-… / user----pass----…（连续任意个 '-' 都当分隔；只取前两段）
// 也支持 user:pass / user|pass / user\tpass
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

std::wstring GetEditText(HWND edit) {
    if (!edit) return {};
    const int n = GetWindowTextLengthW(edit);
    std::wstring s(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(edit, s.data(), n + 1);
    return s;
}

std::wstring RedactUrlForLog(std::wstring url) {
    auto redactParam = [&](const wchar_t* key) {
        const std::wstring k = key;
        size_t pos = 0;
        while ((pos = url.find(k, pos)) != std::wstring::npos) {
            size_t start = pos + k.size();
            size_t end = url.find_first_of(L"&#", start);
            if (end == std::wstring::npos) end = url.size();
            if (end > start + 8) url.replace(start + 6, end - start - 6, L"***");
            pos = start + 1;
        }
    };
    redactParam(L"OTT=");
    redactParam(L"WebToken=");
    redactParam(L"otp1=");
    redactParam(L"skey=");
    redactParam(L"akey=");
    const size_t ottPath = url.find(L"OTT:944:Login:");
    if (ottPath != std::wstring::npos && url.size() > ottPath + 20) {
        url.replace(ottPath + 18, url.size() - (ottPath + 18), L"***");
    }
    return url;
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
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP + 1, 0, 0);
}

void FlushLogsToUi() {
    std::vector<std::wstring> lines;
    {
        std::lock_guard<std::mutex> lock(g.logMu);
        lines.swap(g.pendingLogs);
    }
    if (!g.editLog) return;
    for (const auto& line : lines) {
        const int len = GetWindowTextLengthW(g.editLog);
        SendMessageW(g.editLog, EM_SETSEL, len, len);
        std::wstring chunk = line + L"\r\n";
        SendMessageW(g.editLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(chunk.c_str()));
    }
}

void SetBusy(bool busy) {
    g.busy = busy;
    if (g.btnOne) EnableWindow(g.btnOne, busy ? FALSE : TRUE);
    if (g.editAccount) EnableWindow(g.editAccount, busy ? FALSE : TRUE);
    if (g.status) {
        SetWindowTextW(g.status, busy ? L"状态：自动登录 / 换票中…"
                                      : L"状态：空闲（粘贴账号串后点一键启动）");
    }
    if (g.hwnd) {
        if (busy) {
            SetTimer(g.hwnd, kBusyTimerId, kBusyTimeoutMs, nullptr);
            SetTimer(g.hwnd, kAutoLoginTimerId, kAutoLoginIntervalMs, nullptr);
        } else {
            KillTimer(g.hwnd, kBusyTimerId);
            KillTimer(g.hwnd, kAutoLoginTimerId);
        }
    }
}

void LaunchWithOtt(std::wstring ottOrUrl) {
    QueueLog(L"[…] 换票中…");
    msc::launcher::TicketFetchOptions fo;
    fo.ott = std::move(ottOrUrl);
    auto fr = msc::launcher::FetchGalaxyTicketFromOtt(fo);
    if (!fr.ok) {
        QueueLog(L"[FAIL] 换票失败 http=" + std::to_wstring(fr.httpStatus) + L" api=" +
                 std::to_wstring(fr.apiCode) + L" " + WidenUtf8(fr.message));
        PostMessageW(g.hwnd, WM_APP + 2, 0, 0);
        return;
    }
    QueueLog(L"[OK] 换票成功 uid=" + fr.ticket.userObjectId + L" gid=" + fr.ticket.gid +
             L" galaxyId=" + fr.ticket.galaxyGameId);

    QueueLog(L"[…] NGM 拉起并验票…");
    msc::launcher::Options opt;
    opt.ticket = std::move(fr.ticket);
    auto rr = msc::launcher::Run(opt, [](const msc::launcher::Progress& p) {
        QueueLog(L"  " + WidenUtf8(p.message));
    });
    if (!rr.ok) {
        QueueLog(L"[FAIL] 启动失败：" + WidenUtf8(rr.errorMessage));
    } else {
        QueueLog(L"[OK] 游戏已启动 PID=" + std::to_wstring(rr.gamePid) + L" " +
                 WidenUtf8(rr.cmdLineSummary));
    }
    PostMessageW(g.hwnd, WM_APP + 2, 0, 0);
}

bool IsOttCallbackUrl(const std::wstring& url) {
    if (url.find(L"maplestoryclassic.beanfun.com") == std::wstring::npos) return false;
    return url.find(L"OTT=") != std::wstring::npos || url.find(L"OTT:") != std::wstring::npos;
}

bool TryHandleCallbackUrl(const std::wstring& url) {
    if (!IsOttCallbackUrl(url)) return false;
    if (g.ottConsumed.exchange(true)) return true;

    QueueLog(L"[OK] 捕获到登录回跳，开始一键换票启动");
    QueueLog(L"  URL=" + RedactUrlForLog(url));
    std::thread(LaunchWithOtt, url).detach();
    return true;
}

std::wstring CurrentSourceUrl() {
    if (!g.webview) return {};
    LPWSTR uri = nullptr;
    if (FAILED(g.webview->get_Source(&uri)) || !uri) return {};
    std::wstring u = uri;
    CoTaskMemFree(uri);
    return u;
}

// mode: cookie | provider | fill
std::wstring BuildAutoLoginJs(const wchar_t* mode) {
    const std::string uB64 = Base64Encode(NarrowUtf8(g.cred.user));
    const std::string pB64 = Base64Encode(NarrowUtf8(g.cred.pass));
    const std::wstring modeW = mode ? mode : L"fill";

    return std::wstring(L"(function(){try{") +
           L"var MODE='" + modeW + L"';" +
           L"var user=atob('" + WidenUtf8(uB64) + L"');" +
           L"var pass=atob('" + WidenUtf8(pB64) + L"');" +
           L"function T(el){return ((el&&(el.innerText||el.textContent||el.value))||'').replace(/\\s+/g,' ').trim();}"
           L"function fireClick(el){if(!el)return false; try{el.scrollIntoView({block:'center'});}catch(e){}"
           L"  try{el.focus();}catch(e){}"
           L"  ['pointerdown','mousedown','mouseup','click'].forEach(function(t){"
           L"    try{el.dispatchEvent(new MouseEvent(t,{bubbles:true,cancelable:true,view:window}));}catch(e){}});"
           L"  try{el.click();}catch(e){} return true;}"
           L"function allDocs(){var out=[document];"
           L"  try{document.querySelectorAll('iframe,frame').forEach(function(f){"
           L"    try{if(f.contentDocument) out.push(f.contentDocument);}catch(e){}"
           L"  });}catch(e){} return out;}"
           L"function findClickable(doc, parts){"
           L"  var nodes=doc.querySelectorAll('button,a,div[role=button],input[type=button],input[type=submit],"
           L"input[type=image],span,div,label,li');"
           L"  for(var i=0;i<nodes.length;i++){"
           L"    var el=nodes[i]; var t=T(el); if(!t||t.length>80) continue;"
           L"    for(var j=0;j<parts.length;j++){"
           L"      if(t.indexOf(parts[j])>=0){"
           L"        if(el.tagName==='SPAN'||el.tagName==='LABEL'||el.tagName==='DIV'){"
           L"          var b=el.closest('button,a,div[role=button],li'); if(b) el=b;"
           L"        }"
           L"        return el;"
           L"      }"
           L"    }"
           L"  } return null;"
           L"}"
           L"function setVal(el,v){"
           L"  try{el.removeAttribute('readonly'); el.readOnly=false;}catch(e){}"
           L"  el.focus();"
           L"  var proto=Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype,'value');"
           L"  if(proto&&proto.set) proto.set.call(el,v); else el.value=v;"
           L"  try{el.dispatchEvent(new InputEvent('input',{bubbles:true,data:v,inputType:'insertText'}));}catch(e){"
           L"    el.dispatchEvent(new Event('input',{bubbles:true}));}"
           L"  el.dispatchEvent(new Event('change',{bubbles:true}));"
           L"}"

           // ----- cookie only -----
           L"if(MODE==='cookie'){"
           L"  function hasCookieText(t){return t&&(t.indexOf('我知道了')>=0||t.indexOf('I understand')>=0);}"
           L"  function looksCookieBanner(t){"
           L"    if(!t||t.length>600) return false;"
           L"    return (t.indexOf('Cookie')>=0||t.indexOf('cookie')>=0||t.indexOf('瀏覽器紀錄')>=0||t.indexOf('隐私')>=0||t.indexOf('隱私')>=0)"
           L"      && (t.indexOf('我知道了')>=0||t.indexOf('繼續瀏覽')>=0||t.indexOf('同意')>=0);"
           L"  }"
           L"  var hit=false;"
           L"  allDocs().forEach(function(doc){"
           L"    try{"
           L"      var st=doc.createElement('style'); st.id='msc_hide_cookie';"
           L"      st.textContent='[class*=cookie],[id*=cookie],[class*=Cookie],[id*=Cookie]{display:none!important;pointer-events:none!important;}';"
           L"      if(doc.head&&!doc.getElementById('msc_hide_cookie')) doc.head.appendChild(st);"
           L"    }catch(e){}"
           L"    doc.querySelectorAll('button,a,span,div,input').forEach(function(el){"
           L"      var t=T(el);"
           L"      if(t==='我知道了' || (t.length<=20 && hasCookieText(t))){"
           L"        fireClick(el); hit=true;"
           L"        var p=el;"
           L"        for(var i=0;i<8&&p;i++){"
           L"          try{"
           L"            var cs=getComputedStyle(p);"
           L"            var cn=(p.className||'').toString().toLowerCase();"
           L"            if(cs.position==='fixed'||cs.position==='sticky'||cn.indexOf('cookie')>=0||looksCookieBanner(T(p))){"
           L"              p.style.setProperty('display','none','important');"
           L"              p.style.setProperty('visibility','hidden','important');"
           L"              p.style.setProperty('pointer-events','none','important');"
           L"              p.style.setProperty('height','0','important');"
           L"              try{p.remove();}catch(e){} hit=true; break;"
           L"            }"
           L"          }catch(e){} p=p.parentElement;"
           L"        }"
           L"      }"
           L"    });"
           L"    doc.querySelectorAll('div,section,aside,footer').forEach(function(n){"
           L"      var t=T(n); if(!looksCookieBanner(t)) return;"
           L"      if(t.indexOf('Sign in with')>=0) return;"
           L"      n.style.setProperty('display','none','important');"
           L"      n.style.setProperty('pointer-events','none','important');"
           L"      try{n.remove();}catch(e){} hit=true;"
           L"    });"
           L"  });"
           L"  var still=false;"
           L"  allDocs().forEach(function(doc){"
           L"    doc.querySelectorAll('button,a,span,div').forEach(function(el){"
           L"      var t=T(el); if(t==='我知道了'||(t.length<=24&&hasCookieText(t))) still=true;"
           L"      if(looksCookieBanner(t) && t.indexOf('Sign in with')<0) still=true;"
           L"    });"
           L"  });"
           L"  if(still) return hit?'cookie-pending':'cookie-pending-noclick';"
           L"  return hit?'cookie-cleared':'cookie-absent';"
           L"}"


           // ----- resolve beanfun URL from Galaxy page -----
           L"if(MODE==='resolve'){"
           L"  function absUrl(u){try{return new URL(u,location.href).href;}catch(e){return u||'';}}"
           L"  var cands=[];"
           L"  document.querySelectorAll('a[href]').forEach(function(a){"
           L"    var h=absUrl(a.getAttribute('href')||'');"
           L"    var t=T(a).toLowerCase();"
           L"    if(/bfweb\\.hk\\.beanfun\\.com|login\\.hk\\.beanfun\\.com/i.test(h)) cands.push({h:h,t:t,score:0});"
           L"  });"
           L"  document.querySelectorAll('button,a,div[role=button],div,span').forEach(function(el){"
           L"    var t=T(el); var tl=t.toLowerCase();"
           L"    if(tl.indexOf('gamania (hk)')<0 && tl.indexOf('sign in with gamania')<0) return;"
           L"    if(tl.indexOf('gama pass')>=0) return;"
           L"    var a=el.closest('a[href]')||el.querySelector('a[href]');"
           L"    if(a){ var h=absUrl(a.getAttribute('href')||''); if(h) cands.push({h:h,t:tl,score:10}); }"
           L"    ['data-url','data-href','data-link','href'].forEach(function(k){"
           L"      var v=el.getAttribute&&el.getAttribute(k); if(v) cands.push({h:absUrl(v),t:tl,score:8});"
           L"    });"
           L"  });"
           L"  // 扫描内联脚本/属性里的 bfweb default.aspx"
           L"  var html=document.documentElement?document.documentElement.innerHTML:'';"
           L"  var m=html.match(/https?:\\/\\/bfweb\\.hk\\.beanfun\\.com\\/beanfun_block\\/bflogin\\/default\\.aspx[^\\\"'\\s<>]*/i);"
           L"  if(m) cands.push({h:m[0].replace(/&amp;/g,'&'),t:'html',score:5});"
           L"  var best=null,bestScore=-1;"
           L"  cands.forEach(function(c){"
           L"    if(!c.h) return; var s=c.score;"
           L"    if(/ghk|610076|result\\/mstc/i.test(c.h)) s+=5;"
           L"    if(/gama.?pass/i.test(c.h)) s-=20;"
           L"    if(s>bestScore){bestScore=s; best=c.h;}"
           L"  });"
           L"  if(best) return 'beanfun-url:'+best;"
           L"  return 'beanfun-url-missing';"
           L"}"

           // ----- provider only -----
           L"if(MODE==='provider'){"
           L"  var still=false;"
           L"  document.querySelectorAll('button,a,span,div').forEach(function(el){"
           L"    var t=T(el); if(t==='我知道了'||(t.length<=24&&t.indexOf('我知道了')>=0)) still=true;"
           L"  });"
           L"  if(still) return 'cookie-blocks-provider';"
           L"  var prov=null;"
           L"  var nodes=document.querySelectorAll('button,a,div[role=button],div,span');"
           L"  for(var i=0;i<nodes.length;i++){"
           L"    var t=T(nodes[i]);"
           L"    if(t.indexOf('Sign in with gamania (HK)')>=0){ prov=nodes[i]; break; }"
           L"  }"
           L"  if(!prov){ for(var i=0;i<nodes.length;i++){"
           L"    var tl=T(nodes[i]).toLowerCase();"
           L"    if(tl.indexOf('gamania (hk)')>=0 || (tl.indexOf('sign in with gamania')>=0 && tl.indexOf('gama pass')<0)){"
           L"      prov=nodes[i]; break; }"
           L"  }}"
           L"  if(!prov) return 'wait-provider';"
           L"  var target=prov;"
           L"  var b=prov.closest('button,a,div[role=button]'); if(b) target=b;"
           L"  try{target.scrollIntoView({block:'center'});}catch(e){}"
           L"  try{target.focus();}catch(e){}"
           L"  var r=target.getBoundingClientRect();"
           L"  var x=r.left+r.width/2, y=r.top+r.height/2;"
           L"  var topEl=document.elementFromPoint(x,y);"
           L"  if(topEl && T(topEl).indexOf('我知道了')>=0){ fireClick(topEl); return 'cookie-blocks-provider'; }"
           L"  if(topEl){"
           L"    var up=topEl.closest('button,a,div[role=button]');"
           L"    if(up && T(up).toLowerCase().indexOf('gamania')>=0 && T(up).toLowerCase().indexOf('gama pass')<0) target=up;"
           L"    else if(T(topEl).toLowerCase().indexOf('gamania')>=0) target=topEl;"
           L"  }"
           L"  fireClick(target);"
           L"  try{ if(target.tagName==='A' && target.href) location.href=target.href; }catch(e){}"
           L"  return 'provider-click';"
           L"}"

           // ----- fill + remember + submit once -----
           L"var steps=[];"
           L"function tryFill(doc){"
           L"  var modeBtn=findClickable(doc,['帳號密碼','账号密码']);"
           L"  if(modeBtn){ fireClick(modeBtn); steps.push('mode-account'); }"
           L"  doc.querySelectorAll('input[type=checkbox]').forEach(function(c){"
           L"    var t=((c.id||'')+' '+(c.name||'')+' '+T(c.parentElement||c)).toLowerCase();"
           L"    if(t.indexOf('記住')>=0||t.indexOf('记住')>=0||t.indexOf('保持')>=0||"
           L"       t.indexOf('remember')>=0||t.indexOf('keep')>=0||t.indexOf('auto')>=0){"
           L"      if(!c.checked){ c.checked=true; c.dispatchEvent(new Event('change',{bubbles:true})); fireClick(c); }"
           L"      steps.push('remember');"
           L"    }"
           L"  });"
           L"  var inputs=[].slice.call(doc.querySelectorAll('input'));"
           L"  var passEl=null, userEl=null;"
           L"  for(var i=0;i<inputs.length;i++){"
           L"    var el=inputs[i]; var ty=(el.type||'').toLowerCase();"
           L"    var key=((el.name||'')+' '+(el.id||'')+' '+(el.placeholder||'')).toLowerCase();"
           L"    if(ty==='hidden'||ty==='submit'||ty==='button'||ty==='image'||ty==='checkbox'||ty==='radio') continue;"
           L"    if(ty==='password'||key.indexOf('pass')>=0||key.indexOf('pwd')>=0){ if(!passEl) passEl=el; continue; }"
           L"    if(key.indexOf('account')>=0||key.indexOf('email')>=0||key.indexOf('uid')>=0||"
           L"       key.indexOf('user')>=0||key.indexOf('login')>=0||ty==='email'||ty==='text'||ty==='tel'){"
           L"      if(!userEl) userEl=el;"
           L"    }"
           L"  }"
           L"  if(!passEl){ for(var i=0;i<inputs.length;i++){ if((inputs[i].type||'').toLowerCase()==='password'){passEl=inputs[i];break;} } }"
           L"  if(!userEl&&passEl){ var idx=inputs.indexOf(passEl); for(var k=idx-1;k>=0;k--){"
           L"    var ty=(inputs[k].type||'').toLowerCase(); if(ty==='hidden'||ty==='password') continue; userEl=inputs[k]; break; } }"
           L"  if(!passEl||!userEl) return false;"
           L"  setVal(userEl,user); setVal(passEl,pass); steps.push('filled');"
           L"  var form=userEl.closest('form');"
           L"  var btn=findClickable(doc,['登入','登錄','登录','Login','立即登入']);"
           L"  if(!btn&&form) btn=form.querySelector('input[type=submit],button[type=submit],input[type=image],button');"
           L"  if(!btn) btn=doc.querySelector('input[type=submit],input[type=image]');"
           L"  if(btn){ fireClick(btn); steps.push('submit'); }"
           L"  else if(form){ try{form.submit(); steps.push('form-submit');}catch(e){} }"
           L"  else steps.push('filled-only');"
           L"  return true;"
           L"}"
           L"var docs=allDocs();"
           L"for(var i=0;i<docs.length;i++){ if(tryFill(docs[i])) return steps.join('|'); }"
           L"var dump=[]; for(var i=0;i<docs.length;i++){"
           L"  var arr=[]; docs[i].querySelectorAll('input').forEach(function(el){"
           L"    arr.push((el.type||'')+':'+(el.name||el.id||'').slice(0,20)); });"
           L"  dump.push('d'+i+'['+arr.slice(0,8).join(',')+']');"
           L"}"
           L"return 'wait-form|'+dump.join(';');"
           L"}catch(e){return 'err:'+String(e);}})();";
}

void OnAutoLoginResult(LPCWSTR resultJson) {
    if (!resultJson) return;
    std::wstring r = resultJson;
    if (r.size() >= 2 && r.front() == L'"' && r.back() == L'"') r = r.substr(1, r.size() - 2);
    if (r.empty() || r == L"null") return;

    if (r.find(L"cookie-blocks-provider") != std::wstring::npos ||
        r.find(L"cookie-pending") != std::wstring::npos) {
        // 仅标记未清完；绝不能清 cookieWaitStartTick，否则 2s 超时永远不触发
        g.cookieCleared = false;
        g.cookieAbsentStreak = 0;
    }
    if (r.find(L"cookie-cleared") != std::wstring::npos) {
        g.cookieCleared = true;
        g.cookieAbsentStreak = 0;
    }
    if (r.find(L"cookie-absent") != std::wstring::npos) {
        // 横幅可能晚于首屏渲染：连续 3 次 absent 才放行
        if (g.cookieAbsentStreak.fetch_add(1) + 1 >= 3) {
            g.cookieCleared = true;
            g.cookieAbsentStreak = 0;
        }
    }
    // provider 是否成功改由 URL 是否进入 beanfun 判定（见 OnNavigated）

    if (r.rfind(L"beanfun-url:", 0) == 0 || r.find(L"beanfun-url:") != std::wstring::npos) {
        const size_t p = r.find(L"beanfun-url:");
        if (p != std::wstring::npos) {
            std::wstring u = r.substr(p + 12);
            // JSON 可能残留转义
            while (!u.empty() && (u.back() == L'\\' || u.back() == L'"')) u.pop_back();
            if (u.find(L"http") == 0) {
                g.resolvedBeanfunUrl = u;
                QueueLog(L"[auto] 解析到 beanfun URL（动态）");
                if (g.webview && g.busy && !g.providerClicked) {
                    g.lastProviderNavTick = GetTickCount();
                    g.webview->Navigate(u.c_str());
                }
            }
        }
    }
    if (r.find(L"beanfun-url-missing") != std::wstring::npos) {
        QueueLog(L"[auto] 页面未解析到 beanfun URL，将用内置 fallback");
    }

    // 只有真正点了登入才锁死；其它 frame 的 wait-form 不得冲掉已成功 submit
    if (r.find(L"filled|submit") != std::wstring::npos) {
        g.formSubmitted = true;
        g.fillStarted = true;
    } else if (!g.formSubmitted &&
               (r.find(L"wait-form") != std::wstring::npos || r.find(L"err:") != std::wstring::npos ||
                r.find(L"filled-only") != std::wstring::npos)) {
        g.fillStarted = false;
    }

    const bool isWait = r.find(L"wait-form") != std::wstring::npos ||
                        r.find(L"cookie-pending") != std::wstring::npos ||
                        r.find(L"wait-provider") != std::wstring::npos ||
                        r.find(L"cookie-blocks") != std::wstring::npos ||
                        r.find(L"provider-click") != std::wstring::npos;
    if (isWait) {
        const DWORD now = GetTickCount();
        if (g.lastWaitLogTick && (now - g.lastWaitLogTick) < 2500) return;
        g.lastWaitLogTick = now;
    } else if (r == g.lastAutoLoginStatus) {
        return;
    }
    g.lastAutoLoginStatus = r;
    QueueLog(L"[auto] " + r);
}

void ExecuteJsEverywhere(const std::wstring& js) {
    auto handler = Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [](HRESULT, LPCWSTR resultJson) -> HRESULT {
            OnAutoLoginResult(resultJson);
            return S_OK;
        });
    if (g.webview) g.webview->ExecuteScript(js.c_str(), handler.Get());

    std::vector<ComPtr<ICoreWebView2Frame>> frames;
    {
        std::lock_guard<std::mutex> lock(g.frameMu);
        frames = g.frames;
    }
    for (auto& frame : frames) {
        if (!frame) continue;
        ComPtr<ICoreWebView2Frame2> f2;
        if (FAILED(frame.As(&f2)) || !f2) continue;
        f2->ExecuteScript(js.c_str(), handler.Get());
    }
}

std::wstring BuildBeanfunFillJs() {
    const std::string uB64 = Base64Encode(NarrowUtf8(g.cred.user));
    const std::string pB64 = Base64Encode(NarrowUtf8(g.cred.pass));
    return std::wstring(L"(function(){try{") +
           L"var user=atob('" + WidenUtf8(uB64) + L"');" +
           L"var pass=atob('" + WidenUtf8(pB64) + L"');" +
           L"function T(el){return ((el&&(el.innerText||el.textContent||el.value))||'').replace(/\\s+/g,' ').trim();}"
           L"function allDocs(){var out=[document];"
           L"  try{document.querySelectorAll('iframe,frame').forEach(function(f){"
           L"    try{if(f.contentDocument) out.push(f.contentDocument);}catch(e){}"
           L"  });}catch(e){} return out;}"
           L"function setVal(el,v){"
           L"  if(!el) return;"
           L"  try{el.removeAttribute('readonly'); el.readOnly=false; el.disabled=false;}catch(e){}"
           L"  try{el.focus();}catch(e){}"
           L"  try{el.click();}catch(e){}"
           L"  var proto=Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype,'value');"
           L"  if(proto&&proto.set) proto.set.call(el,v); else el.value=v;"
           L"  try{el.dispatchEvent(new InputEvent('input',{bubbles:true,data:v,inputType:'insertText'}));}catch(e){"
           L"    el.dispatchEvent(new Event('input',{bubbles:true}));}"
           L"  el.dispatchEvent(new Event('change',{bubbles:true}));"
           L"}"
           L"function forcePass(el,v){"
           L"  setVal(el,v);"
           L"  if((el.value||'').length>0) return true;"
           L"  try{el.focus(); if(el.select) el.select();"
           L"    if(document.execCommand){ document.execCommand('selectAll',false,null);"
           L"      document.execCommand('insertText',false,v); }"
           L"  }catch(e){}"
           L"  try{el.value=v;}catch(e){}"
           L"  return (el.value||'').length>0;"
           L"}"
           L"function findPass(doc){"
           L"  var el=doc.querySelector('input[type=password],input[name*=pass],input[id*=pass],"
           L"input[name*=pwd],input[id*=pwd],input[name*=Password],input[id*=Password]');"
           L"  if(el) return el;"
           L"  var list=doc.querySelectorAll('input');"
           L"  for(var i=0;i<list.length;i++){"
           L"    var ty=(list[i].type||'').toLowerCase();"
           L"    var key=((list[i].name||'')+' '+(list[i].id||'')+' '+(list[i].placeholder||'')).toLowerCase();"
           L"    if(ty==='password') return list[i];"
           L"    if(key.indexOf('pass')>=0||key.indexOf('pwd')>=0||key.indexOf('密碼')>=0||key.indexOf('密码')>=0) return list[i];"
           L"  } return null;"
           L"}"
           L"function findUser(doc, passEl){"
           L"  var el=doc.querySelector('input#AccountID,input#account,input[name=AccountID],input[name=account],"
           L"input[type=email],input[name*=Account],input[id*=Account],input[name*=email],input[id*=email]');"
           L"  if(el) return el;"
           L"  var inputs=[].slice.call(doc.querySelectorAll('input'));"
           L"  var pi=passEl?inputs.indexOf(passEl):-1;"
           L"  for(var k=(pi>0?pi-1:inputs.length-1);k>=0;k--){"
           L"    var ty=(inputs[k].type||'').toLowerCase();"
           L"    if(ty==='text'||ty==='email'||ty==='tel') return inputs[k];"
           L"  } return null;"
           L"}"
           L"function tryFill(doc){"
           L"  var tabs=doc.querySelectorAll('a,button,li,div,span');"
           L"  for(var i=0;i<tabs.length;i++){ var t=T(tabs[i]);"
           L"    if(t==='帳號密碼'||t==='账号密码'){ try{tabs[i].click();}catch(e){} break; } }"
           L"  doc.querySelectorAll('input[type=checkbox]').forEach(function(c){"
           L"    var t=((c.id||'')+' '+(c.name||'')+' '+T(c.parentElement||c)).toLowerCase();"
           L"    if(t.indexOf('記住')>=0||t.indexOf('记住')>=0||t.indexOf('remember')>=0){"
           L"      if(!c.checked){ c.checked=true; c.dispatchEvent(new Event('change',{bubbles:true})); }"
           L"    }"
           L"  });"
           L"  var passEl=findPass(doc); if(!passEl) return null;"
           L"  var userEl=findUser(doc, passEl); if(!userEl) return 'wait-form|no-user';"
           L"  setVal(userEl,user);"
           L"  if(!forcePass(passEl,pass)) return 'wait-form|pass-empty|u='+(userEl.value||'').length;"
           L"  var btn=null;"
           L"  var nodes=doc.querySelectorAll('button,a,input[type=submit],input[type=image],div[role=button]');"
           L"  for(var i=0;i<nodes.length;i++){"
           L"    var t=T(nodes[i]); if(t==='登入'||t==='登錄'||t==='登录'||t==='Login'){ btn=nodes[i]; break; }"
           L"  }"
           L"  if(!btn){ var f=userEl.closest('form'); if(f) btn=f.querySelector('input[type=submit],button[type=submit],button'); }"
           L"  if(!btn) return 'filled-only|no-btn|u='+(userEl.value||'').length+'|p='+(passEl.value||'').length;"
           L"  try{btn.click();}catch(e){}"
           L"  try{btn.dispatchEvent(new MouseEvent('click',{bubbles:true,cancelable:true,view:window}));}catch(e){}"
           L"  return 'filled|submit|u='+(userEl.value||'').length+'|p='+(passEl.value||'').length;"
           L"}"
           L"var docs=allDocs();"
           L"for(var i=0;i<docs.length;i++){"
           L"  var r=tryFill(docs[i]);"
           L"  if(r===null) continue;"
           L"  return r;"
           L"}"
           // dump 便于诊断：各文档 input 摘要
           L"var dump=[];"
           L"for(var i=0;i<docs.length;i++){"
           L"  var arr=[]; docs[i].querySelectorAll('input').forEach(function(el){"
           L"    arr.push((el.type||'')+':'+(el.name||el.id||el.placeholder||'').toString().slice(0,24));"
           L"  });"
           L"  dump.push('d'+i+'['+arr.slice(0,10).join(',')+']');"
           L"}"
           L"return 'wait-form|no-password|frames='+docs.length+'|'+dump.join(';');"
           L"}catch(e){return 'err:'+String(e);}})();";
}

void RunAutoLoginScript() {
    if (!g.busy || !g.webview || g.ottConsumed) return;

    const std::wstring url = CurrentSourceUrl();
    // 已拿到票 / 回跳：绝不再乱动导航
    if (url.find(L"WebToken=") != std::wstring::npos ||
        url.find(L"Main?OTT") != std::wstring::npos ||
        url.find(L"OTT=") != std::wstring::npos ||
        url.find(L"/login/result/") != std::wstring::npos) {
        return;
    }
    if (g.formSubmitted) return;

    const bool hasCred = !g.cred.user.empty() && !g.cred.pass.empty();
    const bool onGalaxyHost =
        url.find(L"galaxy.games.gamania.com") != std::wstring::npos;
    // 仅登录入口才直达 beanfun；result/WebToken 页禁止回跳
    const bool onGalaxyLoginGate =
        onGalaxyHost &&
        (url.find(L"/login/mstc") != std::wstring::npos ||
         url.find(L"/login/init/") != std::wstring::npos);
    const bool onLoginForm =
        url.find(L"loginform") != std::wstring::npos ||
        (url.find(L"login.hk.beanfun.com") != std::wstring::npos &&
         url.find(L"checkin") == std::wstring::npos);

    // Cookie 条只出现在 Galaxy 登录入口
    if (!g.cookieCleared) {
        if (!onGalaxyLoginGate) {
            g.cookieCleared = true;
        } else {
            const DWORD now = GetTickCount();
            if (!g.cookieWaitStartTick) g.cookieWaitStartTick = now;
            static const wchar_t kCookieJs[] =
                L"(function(){try{"
                L"function T(el){return ((el&&(el.innerText||el.textContent||''))||'').replace(/\\s+/g,' ').trim();}"
                L"var hit=false;"
                L"document.querySelectorAll('button,a,span,div').forEach(function(el){"
                L"  var t=T(el); if(t!=='我知道了'&&t.indexOf('我知道了')<0) return;"
                L"  try{el.click();}catch(e){} hit=true;"
                L"  var p=el; for(var i=0;i<6&&p;i++){"
                L"    try{var s=getComputedStyle(p); if(s.position==='fixed'||s.position==='sticky'){"
                L"      p.style.display='none'; p.style.pointerEvents='none'; try{p.remove();}catch(e){} break;}}catch(e){}"
                L"    p=p.parentElement;}"
                L"});"
                L"var still=false; document.querySelectorAll('button,a,span,div').forEach(function(el){"
                L"  var t=T(el); if(t==='我知道了'||(t.length<30&&t.indexOf('我知道了')>=0)) still=true;});"
                L"if(still) return hit?'cookie-pending':'cookie-pending-noclick';"
                L"return hit?'cookie-cleared':'cookie-absent';"
                L"}catch(e){return 'cookie-err:'+e;}})();";
            g.webview->ExecuteScript(
                kCookieJs, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                               [](HRESULT, LPCWSTR resultJson) -> HRESULT {
                                   OnAutoLoginResult(resultJson);
                                   return S_OK;
                               })
                               .Get());
            if (now - g.cookieWaitStartTick >= 2000) {
                g.cookieCleared = true;
                QueueLog(L"[auto] Cookie 等待超时，强制继续（直达 beanfun）");
            } else {
                return;
            }
        }
    }

    if (onGalaxyLoginGate) {
        const DWORD now = GetTickCount();
        if (!g.lastProviderNavTick || now - g.lastProviderNavTick > 2000) {
            g.lastProviderNavTick = now;
            const wchar_t* dest = g.resolvedBeanfunUrl.empty() ? kBeanfunGhkLoginUrl
                                                               : g.resolvedBeanfunUrl.c_str();
            QueueLog(g.resolvedBeanfunUrl.empty()
                         ? L"[auto] 直达内置 gamania(HK) beanfun 入口"
                         : L"[auto] 直达已解析 beanfun URL");
            g.webview->Navigate(dest);
        }
        return;
    }

    // 账密页：主文档 + 所有 WebView frame 一起填（密码常在 iframe）
    if (onLoginForm && hasCred) {
        const DWORD now = GetTickCount();
        if (g.lastFillAttemptTick && (now - g.lastFillAttemptTick) < 1200) return;
        if (g.fillStarted.exchange(true)) return;
        g.lastFillAttemptTick = now;
        QueueLog(L"[auto] 填账密并点登入…");
        ExecuteJsEverywhere(BuildBeanfunFillJs());
    }
}

void MaybeRecoverSso(const std::wstring& url) {
    if (!g.busy || g.ottConsumed || !g.formSubmitted || g.ssoRecoverTried) return;

    // 登完却掉到 beanfun 首页 / 无 Galaxy 回跳参数 → SSO 断了，重走 Galaxy（有 Cookie 应免密）
    const bool lost =
        (url == L"https://bfweb.hk.beanfun.com/" || url == L"https://bfweb.hk.beanfun.com" ||
         url.find(L"bfweb.hk.beanfun.com/?") != std::wstring::npos ||
         (url.find(L"bfweb.hk.beanfun.com/beanfun_block/bflogin/default.aspx") != std::wstring::npos &&
          url.find(L"url=") == std::wstring::npos && url.find(L"galaxy") == std::wstring::npos));

    if (!lost) return;

    g.ssoRecoverTried = true;
    QueueLog(L"[!] 登录后未回到 Galaxy WebToken 页，正在重走 Galaxy SSO（勾选记住后通常免密）…");
    g.cookieCleared = true;
    g.providerClicked = false;
    g.formSubmitted = false;
    g.fillStarted = false;
    g.lastProviderNavTick = 0;
    g.webview->Navigate(kGalaxyLoginUrl);
}

void OnNavigated(const std::wstring& url, const wchar_t* phase) {
    QueueLog(std::wstring(L"[nav:") + phase + L"] " + RedactUrlForLog(url));

    if (url.find(L"login.hk.beanfun.com") != std::wstring::npos ||
        url.find(L"bfweb.hk.beanfun.com") != std::wstring::npos) {
        if (!g.providerClicked.exchange(true)) {
            QueueLog(L"[OK] 已离开 Galaxy，进入 beanfun 登录流程");
        }
    }

    if (url.find(L"WebToken=") != std::wstring::npos) {
        g.formSubmitted = true;  // 禁止再直达 beanfun 冲掉回跳
        QueueLog(L"[OK] 收到 Galaxy WebToken，等待官网 OTT 回跳…");
    }

    TryHandleCallbackUrl(url);
    MaybeRecoverSso(url);
    if (g.busy && !g.ottConsumed) RunAutoLoginScript();
}

void ResizeWebView() {
    if (!g.controller || !g.webHost) return;
    RECT rc{};
    GetClientRect(g.webHost, &rc);
    g.controller->put_Bounds(rc);
}

void InitWebView() {
    const std::wstring profile = ProfileDir();
    const std::wstring exeDir = ExeDir();
    const bool remoteExe = IsPathOnRemoteDrive(exeDir);
    QueueLog(L"WebView 用户目录：" + profile);
    if (remoteExe) {
        QueueLog(L"[提示] 程序在映射/网络盘上：" + exeDir +
                 L"；账号用程序目录，WebView 配置强制写本机，并加沙箱兼容参数");
    }

    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_EnableTrackingPrevention(FALSE);
    if (remoteExe) {
        options->put_AdditionalBrowserArguments(L"--no-sandbox --disable-gpu-sandbox");
    }

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    wchar_t buf[96]{};
                    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
                    QueueLog(std::wstring(L"[FAIL] WebView2 环境创建失败 hr=") + buf +
                             L"。请安装 Edge WebView2 Runtime（Evergreen）。");
                    return S_OK;
                }
                g.env = env;

                ComPtr<ICoreWebView2Environment5> env5;
                if (SUCCEEDED(env->QueryInterface(IID_PPV_ARGS(&env5))) && env5) {
                    env5->add_BrowserProcessExited(
                        Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                            [](ICoreWebView2Environment*,
                               ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
                                if (g.browserExitEvent) SetEvent(g.browserExitEvent);
                                return S_OK;
                            })
                            .Get(),
                        nullptr);
                }

                env->CreateCoreWebView2Controller(
                    g.webHost,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(hr2) || !controller) {
                                QueueLog(L"[FAIL] WebView2 控制器创建失败");
                                return S_OK;
                            }
                            g.controller = controller;
                            controller->get_CoreWebView2(&g.webview);
                            ResizeWebView();
                            controller->put_IsVisible(TRUE);

                            ComPtr<ICoreWebView2_13> wv13;
                            if (SUCCEEDED(g.webview.As(&wv13)) && wv13) {
                                ComPtr<ICoreWebView2Profile> profile;
                                if (SUCCEEDED(wv13->get_Profile(&profile)) && profile) {
                                    ComPtr<ICoreWebView2Profile3> p3;
                                    if (SUCCEEDED(profile.As(&p3)) && p3) {
                                        p3->put_PreferredTrackingPreventionLevel(
                                            COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_NONE);
                                    }
                                }
                            }

                            g.webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*,
                                       ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        args->get_Uri(&uri);
                                        if (uri) {
                                            const std::wstring url = uri;
                                            CoTaskMemFree(uri);
                                            OnNavigated(url, L"start");
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &g.navStartToken);

                            g.webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2* sender,
                                       ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (sender && SUCCEEDED(sender->get_Source(&uri)) && uri) {
                                            const std::wstring url = uri;
                                            CoTaskMemFree(uri);
                                            OnNavigated(url, L"done");
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &g.navDoneToken);

                            g.webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [](ICoreWebView2* sender,
                                       ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (sender && SUCCEEDED(sender->get_Source(&uri)) && uri) {
                                            const std::wstring url = uri;
                                            CoTaskMemFree(uri);
                                            OnNavigated(url, L"src");
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &g.sourceToken);

                            ComPtr<ICoreWebView2_4> wv4;
                            if (SUCCEEDED(g.webview.As(&wv4)) && wv4) {
                                wv4->add_FrameCreated(
                                    Callback<ICoreWebView2FrameCreatedEventHandler>(
                                        [](ICoreWebView2*,
                                           ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
                                            ComPtr<ICoreWebView2Frame> frame;
                                            args->get_Frame(&frame);
                                            if (!frame) return S_OK;
                                            {
                                                std::lock_guard<std::mutex> lock(g.frameMu);
                                                g.frames.push_back(frame);
                                            }
                                            QueueLog(L"[auto] frame-created");
                                            // 不在这里清空 cookieCleared，避免把状态机打回死循环
                                            ComPtr<ICoreWebView2Frame2> f2;
                                            if (SUCCEEDED(frame.As(&f2)) && f2) {
                                                f2->add_Destroyed(
                                                    Callback<ICoreWebView2FrameDestroyedEventHandler>(
                                                        [raw = frame.Get()](ICoreWebView2Frame*,
                                                                            IUnknown*) -> HRESULT {
                                                            std::lock_guard<std::mutex> lock(
                                                                g.frameMu);
                                                            g.frames.erase(
                                                                std::remove_if(
                                                                    g.frames.begin(), g.frames.end(),
                                                                    [raw](const ComPtr<ICoreWebView2Frame>&
                                                                              f) {
                                                                        return f.Get() == raw;
                                                                    }),
                                                                g.frames.end());
                                                            return S_OK;
                                                        })
                                                        .Get(),
                                                    nullptr);
                                            }
                                            if (g.busy) RunAutoLoginScript();
                                            return S_OK;
                                        })
                                        .Get(),
                                    &g.frameToken);
                            }

                            g.webReady = true;
                            QueueLog(L"[OK] WebView2 就绪。粘贴「邮箱----密码----…」后点一键启动；"
                                     L"将自动点 gamania(HK) 并填入账密。");
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());
}

void StartOneClick() {
    if (g.busy) return;
    if (!g.webReady || !g.webview) {
        MessageBoxW(g.hwnd, L"WebView2 尚未就绪，请稍等几秒。", L"经典版启动器",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    AccountCred cred;
    std::wstring err;
    if (!ParseAccountLine(GetEditText(g.editAccount), cred, err)) {
        MessageBoxW(g.hwnd, err.c_str(), L"经典版启动器", MB_OK | MB_ICONWARNING);
        return;
    }
    g.cred = std::move(cred);
    SaveAccountConfig(GetEditText(g.editAccount));
    g.lastAutoLoginStatus.clear();
    g.lastWaitLogTick = 0;
    g.cookieCleared = false;
    g.providerClicked = false;
    g.formSubmitted = false;
    g.ssoRecoverTried = false;
    g.cookieAbsentStreak = 0;
    g.lastProviderNavTick = 0;
    g.cookieWaitStartTick = 0;
    g.lastFillAttemptTick = 0;
    g.fillStarted = false;
    g.resolvedBeanfunUrl.clear();
    {
        std::lock_guard<std::mutex> lock(g.frameMu);
        g.frames.clear();
    }

    SetBusy(true);
    g.ottConsumed = false;
    QueueLog(L"[…] 一键启动：账号=" + g.cred.user +
             L" ，登录方式=gamania(HK)；将自动关 Cookie、勾选记住帐号、只提交一次");
    g.webview->Navigate(kGalaxyLoginUrl);
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int pad = 10;
    const int topH = 128;
    const int logH = 140;

    MoveWindow(GetDlgItem(hwnd, IDC_STATIC_HINT), pad, pad, w - pad * 2, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_STATIC_ACCOUNT), pad, 40, 70, 24, TRUE);
    MoveWindow(g.editAccount, pad + 70, 38, w - pad * 2 - 70, 26, TRUE);
    MoveWindow(g.btnOne, pad, 74, 180, 36, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR), pad + 190, 74, 100, 36, TRUE);
    MoveWindow(g.status, pad + 300, 80, w - pad * 2 - 300, 24, TRUE);

    const int webTop = topH;
    const int webH = h - topH - logH - pad;
    MoveWindow(g.webHost, pad, webTop, w - pad * 2, webH > 80 ? webH : 80, TRUE);
    MoveWindow(g.editLog, pad, webTop + (webH > 80 ? webH : 80) + 6, w - pad * 2, logH - 6, TRUE);
    ResizeWebView();
}

void BeginGracefulClose(HWND hwnd) {
    if (g.closing) return;
    g.closing = true;
    if (g.editAccount) SaveAccountConfig(GetEditText(g.editAccount));
    QueueLog(L"[…] 正在关闭 WebView 并等待 Cookie 落盘…");
    FlushLogsToUi();

    if (g.controller) {
        g.controller->Close();
        g.controller = nullptr;
    }
    g.webview = nullptr;

    if (g.browserExitEvent) {
        WaitForSingleObject(g.browserExitEvent, 8000);
    } else {
        Sleep(800);
    }
    DestroyWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.browserExitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g.logFilePath = ExeDir() + L"\\launcher.log";

            CreateWindowExW(0, L"STATIC",
                            L"粘贴账号串（邮箱----密码----…，只取前两项）→ 自动选 gamania(HK) 登录 → 换票开游戏",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_HINT)),
                            GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"账号串", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_ACCOUNT)),
                            GetModuleHandleW(nullptr), nullptr);
            g.editAccount = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_ACCOUNT)),
                                            GetModuleHandleW(nullptr), nullptr);
            g.btnOne = CreateWindowExW(0, L"BUTTON", L"一键启动游戏",
                                       WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_ONECLICK)),
                                       GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"BUTTON", L"清空日志", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                            0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CLEAR)),
                            GetModuleHandleW(nullptr), nullptr);
            g.status = CreateWindowExW(0, L"STATIC", L"状态：初始化 WebView2…", WS_CHILD | WS_VISIBLE,
                                       0, 0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_STATUS)),
                                       GetModuleHandleW(nullptr), nullptr);
            g.webHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0,
                                        0, 0, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_WEBHOST)),
                                        GetModuleHandleW(nullptr), nullptr);
            g.editLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                                            ES_READONLY | WS_VSCROLL,
                                        0, 0, 0, 0, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_LOG)),
                                        GetModuleHandleW(nullptr), nullptr);

            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            EnumChildWindows(
                hwnd,
                [](HWND c, LPARAM f) -> BOOL {
                    SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(font));

            Layout(hwnd);
            {
                const std::wstring saved = LoadAccountConfig();
                if (!saved.empty() && g.editAccount) {
                    SetWindowTextW(g.editAccount, saved.c_str());
                    QueueLog(L"[OK] 已加载本地账号配置：" + AccountConfigPath());
                } else {
                    QueueLog(L"账号配置路径：" + AccountConfigPath() + L"（首次粘贴后会自动保存）");
                }
            }
            QueueLog(L"经典版一键启动器（自动 gamania 登录）。日志：" + g.logFilePath);
            InitWebView();
            return 0;
        }
        case WM_SIZE:
            Layout(hwnd);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_BTN_ONECLICK) {
                StartOneClick();
                return 0;
            }
            if (id == IDC_BTN_CLEAR) {
                SetWindowTextW(g.editLog, L"");
                return 0;
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kBusyTimerId && g.busy) {
                QueueLog(L"[FAIL] 超时：未捕获到 OTT 回跳。请看下方网页是否卡在验证码/二次验证。");
                SetBusy(false);
            } else if (wp == kAutoLoginTimerId && g.busy) {
                RunAutoLoginScript();
            }
            return 0;
        case WM_APP + 1:
            FlushLogsToUi();
            return 0;
        case WM_APP + 2:
            SetBusy(false);
            FlushLogsToUi();
            return 0;
        case WM_CLOSE:
            BeginGracefulClose(hwnd);
            return 0;
        case WM_DESTROY:
            if (g.browserExitEvent) {
                CloseHandle(g.browserExitEvent);
                g.browserExitEvent = nullptr;
            }
            // 内存中的密码随进程退出清掉
            g.cred = {};
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MscOneClickLauncher";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"新楓之谷經典版 · 一键启动",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 760,
                                nullptr, nullptr, hi, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
