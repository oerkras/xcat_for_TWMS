#include "gamapass_device_login.h"

#include "chromium_cdp.h"
#include "http_gamapass_login.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <winreg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace msc::launcher {
namespace {

constexpr int kDeviceLoginDebugPort = 19223;
constexpr wchar_t kSiteOrigin[] = L"https://accounts.gamania.com";
constexpr wchar_t kStartUrl[] = L"https://accounts.gamania.com";
constexpr wchar_t kProfileDirName[] = L"GpDeviceLoginProfile";

std::atomic<bool> gBusy{false};

void LogLine(const HttpLoginLogFn& log, const std::wstring& s) {
    if (log) log(s);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& p) {
    if (DirExists(p)) return true;
    return CreateDirectoryW(p.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back((char)c);
        } else if (c == '\n') {
            o += "\\n";
        } else if (c == '\r') {
            o += "\\r";
        } else if (c == '\t') {
            o += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else {
            o.push_back((char)c);
        }
    }
    return o;
}

std::string JsonGetString(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    while (p + 1 < json.size() && (json[p + 1] == ' ' || json[p + 1] == '\t')) ++p;
    if (p + 1 >= json.size() || json[p + 1] != '"') return {};
    size_t i = p + 2;
    std::string out;
    while (i < json.size()) {
        char c = json[i++];
        if (c == '\\' && i < json.size()) {
            char e = json[i++];
            if (e == 'n')
                out.push_back('\n');
            else if (e == 'r')
                out.push_back('\r');
            else if (e == 't')
                out.push_back('\t');
            else
                out.push_back(e);
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

int JsonGetInt(const std::string& json, const char* key, int defVal) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return defVal;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return defVal;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p < json.size() && json[p] == '"') ++p;
    int n = defVal;
    if (p < json.size()) sscanf(json.c_str() + (int)p, "%d", &n);
    return n;
}

bool FileReadAll(const std::wstring& path, std::string& out) {
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool FileWriteAll(const std::wstring& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(body.data(), (std::streamsize)body.size());
    return (bool)f;
}

bool EvalUtf8(msc::cdp::Session& cdp, const std::string& js, std::string& out,
              const HttpLoginLogFn& log) {
    return cdp.Evaluate(Utf8ToWide(js), out, [&](const std::wstring& s) { LogLine(log, s); });
}

bool ResultTruthy(const std::string& json) {
    if (json.find("true") != std::string::npos) return true;
    if (json.find("\"true\"") != std::string::npos) return true;
    return false;
}

std::wstring LeafOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

bool Is360Exe(const std::wstring& exe) {
    std::wstring leaf = LeafOf(exe);
    for (auto& c : leaf) c = (wchar_t)towlower(c);
    return leaf.find(L"360chrome") != std::wstring::npos || leaf.find(L"360se") != std::wstring::npos ||
           leaf.find(L"360browser") != std::wstring::npos;
}

bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsChromePlus(const std::wstring& exe) {
    const std::wstring app = exe.substr(0, exe.find_last_of(L"\\/"));
    return FileExistsW(app + L"\\chrome++.ini") || FileExistsW(app + L"\\version.dll");
}

std::wstring BrowserLabel(const std::wstring& exe) {
    if (exe.empty()) return L"(未找到)";
    if (IsChromePlus(exe)) return L"Chrome++";
    std::wstring leaf = LeafOf(exe);
    for (auto& c : leaf) c = (wchar_t)towlower(c);
    if (leaf == L"msedge.exe") return L"Microsoft Edge";
    if (leaf == L"chrome.exe") return L"Google Chrome";
    return leaf;
}

void PushUniqueExe(std::vector<std::wstring>& out, const std::wstring& p) {
    if (!FileExistsW(p)) return;
    for (const auto& e : out) {
        if (_wcsicmp(e.c_str(), p.c_str()) == 0) return;
    }
    out.push_back(p);
}

void PushExpandedExe(std::vector<std::wstring>& out, const wchar_t* pat) {
    wchar_t exp[MAX_PATH]{};
    if (!ExpandEnvironmentStringsW(pat, exp, MAX_PATH)) return;
    PushUniqueExe(out, exp);
}

void CollectAppPathExe(std::vector<std::wstring>& out, const wchar_t* sub) {
    auto read = [&](HKEY root) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        wchar_t buf[MAX_PATH]{};
        DWORD typ = 0, cb = sizeof(buf);
        if (RegQueryValueExW(k, nullptr, nullptr, &typ, reinterpret_cast<LPBYTE>(buf), &cb) ==
                ERROR_SUCCESS &&
            (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf[0]) {
            wchar_t exp[MAX_PATH]{};
            if (ExpandEnvironmentStringsW(buf, exp, MAX_PATH)) PushUniqueExe(out, exp);
            PushUniqueExe(out, buf);
        }
        RegCloseKey(k);
    };
    read(HKEY_LOCAL_MACHINE);
    read(HKEY_CURRENT_USER);
}

void CollectFallbackExes(std::vector<std::wstring>& out) {
    static const wchar_t* kCands[] = {
        L"%ProgramFiles%\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles(x86)%\\Chrome\\App\\chrome.exe",
        L"%LocalAppData%\\Chrome\\App\\chrome.exe",
        L"C:\\Program Files\\Chrome\\App\\chrome.exe",
        L"C:\\Program Files (x86)\\Chrome\\App\\chrome.exe",
        L"D:\\Chrome\\App\\chrome.exe",
        L"E:\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles%\\Google\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
        L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%LocalAppData%\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t* cand : kCands) PushExpandedExe(out, cand);
    CollectAppPathExe(out, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe");
    CollectAppPathExe(out, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe");
}

bool IsEdgeExe(const std::wstring& exe) {
    return _wcsicmp(LeafOf(exe).c_str(), L"msedge.exe") == 0;
}

bool IsOfficialChromeExe(const std::wstring& exe) {
    return _wcsicmp(LeafOf(exe).c_str(), L"chrome.exe") == 0 && !IsChromePlus(exe) && !Is360Exe(exe);
}

const wchar_t* ProfileLeafForExe(const std::wstring& exe) {
    if (IsChromePlus(exe)) return L"chromeplus";
    if (IsEdgeExe(exe)) return L"edge";
    return L"chrome";
}

GpDeviceLoginBrowserKind ClampBrowserKind(int v) {
    if (v < 0 || v > 3) return GpDeviceLoginBrowserKind::Auto;
    return static_cast<GpDeviceLoginBrowserKind>(v);
}

bool PinDeviceId(msc::cdp::Session& cdp, const std::string& deviceId, const HttpLoginLogFn& log) {
    const auto cdpLog = [&](const std::wstring& s) { LogLine(log, s); };
    const std::string lit = JsonEscape(deviceId);

    std::string bootJs = "if (location.hostname === 'accounts.gamania.com') {"
                         "try { localStorage.setItem('device_id', '" +
                         lit + "'); } catch (e) {} }";
    std::string params = "{\"source\":\"" + JsonEscape(bootJs) + "\",\"runImmediately\":true}";
    std::string res;
    if (!cdp.Command("Page.addScriptToEvaluateOnNewDocument", params, res, cdpLog)) {
        LogLine(log, L"[gp-device-login] addScriptToEvaluateOnNewDocument 失败");
        return false;
    }

    if (!cdp.Command("DOMStorage.enable", "{}", res, cdpLog)) {
        LogLine(log, L"[gp-device-login] DOMStorage.enable 失败（将只靠页面 JS 写入）");
    } else {
        std::string setItem = std::string("{\"storageId\":{\"securityOrigin\":\"https://accounts.gamania.com\",") +
                              "\"isLocalStorage\":true},\"key\":\"device_id\",\"value\":\"" + lit + "\"}";
        if (!cdp.Command("DOMStorage.setDOMStorageItem", setItem, res, cdpLog)) {
            LogLine(log, L"[gp-device-login] DOMStorage.setDOMStorageItem 未生效（页未就绪时常见）");
        }
    }

    std::string pageJs = "(() => { try { localStorage.setItem('device_id', '" + lit +
                         "'); return localStorage.getItem('device_id'); } catch (e) { return ''; } })()";
    std::string got;
    EvalUtf8(cdp, pageJs, got, log);
    LogLine(log, L"[gp-device-login] 已钉 device_id");
    return true;
}

bool WaitDocumentUsable(msc::cdp::Session& cdp, DWORD timeoutMs, const HttpLoginLogFn& log) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeoutMs) {
        std::string r;
        if (EvalUtf8(cdp,
                     "document.readyState === 'interactive' || document.readyState === 'complete'", r,
                     log) &&
            ResultTruthy(r))
            return true;
        Sleep(300);
    }
    LogLine(log, L"[gp-device-login] 等待页面就绪超时");
    return false;
}

bool FillVisibleInput(msc::cdp::Session& cdp, bool password, const std::string& value,
                      const HttpLoginLogFn& log) {
    const char* pick = password
                           ? "all.find(el => (el.type || '').toLowerCase() === 'password' || "
                             "(el.autocomplete || '').toLowerCase() === 'current-password')"
                           : "all.find(el => ['email','tel'].includes((el.type || '').toLowerCase()))"
                             " || all.find(el => ['username','email'].includes((el.autocomplete || '').toLowerCase()))"
                             " || all.find(el => (el.type || 'text').toLowerCase() === 'text')";
    std::string js = std::string("(() => {") +
                     "const visible = el => { const style = getComputedStyle(el), rect = el.getBoundingClientRect();"
                     "return !el.disabled && style.display !== 'none' && style.visibility !== 'hidden'"
                     " && rect.width > 0 && rect.height > 0; };"
                     "const all = [...document.querySelectorAll('input')].filter(visible);"
                     "let input = " +
                     pick + ";" +
                     "if (!input) return false;"
                     "const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;"
                     "setter.call(input, " +
                     "\"" + JsonEscape(value) + "\"" +
                     ");"
                     "input.dispatchEvent(new InputEvent('input', {bubbles: true, inputType: 'insertText', data: null}));"
                     "input.dispatchEvent(new Event('change', {bubbles: true}));"
                     "input.focus();"
                     "return true;"
                     "})()";
    std::string r;
    if (!EvalUtf8(cdp, js, r, log)) return false;
    return ResultTruthy(r);
}

bool EnsureKeepSignedIn(msc::cdp::Session& cdp, const HttpLoginLogFn& log) {
    static const char kJs[] =
        "(() => {"
        "  const normalized = value => (value || '').replace(/\\s+/g, ' ').trim().toLowerCase();"
        "  const keywords = ['记住我','記住我','保持登入','保持登录','remember me',"
        "    'stay signed in','keep me signed in','keep signed in'];"
        "  const description = el => {"
        "    const values = [el.textContent, el.getAttribute('aria-label'), el.getAttribute('name'), el.id,"
        "      el.getAttribute('data-testid')];"
        "    if (el.id) { const label = document.querySelector('label[for=\"'+CSS.escape(el.id)+'\"]');"
        "      if (label) values.push(label.textContent); }"
        "    const wrapping = el.closest('label'); if (wrapping) values.push(wrapping.textContent);"
        "    if (el.parentElement) values.push(el.parentElement.textContent);"
        "    return normalized(values.filter(Boolean).join(' '));"
        "  };"
        "  const visible = el => { const rect = el.getBoundingClientRect(), style = getComputedStyle(el);"
        "    return !el.disabled && rect.width > 0 && rect.height > 0 && style.display !== 'none'"
        "      && style.visibility !== 'hidden'; };"
        "  const controls = [...document.querySelectorAll('input[type=\"checkbox\"], [role=\"checkbox\"]')]"
        "    .filter(visible);"
        "  const control = controls.find(el => keywords.some(k => description(el).includes(k)));"
        "  if (!control) return {found:false, checked:false};"
        "  const isChecked = el => el.matches('input[type=\"checkbox\"]') ? Boolean(el.checked)"
        "    : el.getAttribute('aria-checked') === 'true';"
        "  if (!isChecked(control)) control.click();"
        "  return {found:true, checked:isChecked(control)};"
        "})()";
    std::string r;
    if (!EvalUtf8(cdp, kJs, r, log)) return false;
    return r.find("\"found\":true") != std::string::npos || r.find("found\":true") != std::string::npos;
}

bool ClickPrimaryButton(msc::cdp::Session& cdp, const HttpLoginLogFn& log) {
    static const char kJs[] =
        "(() => {"
        "  const visible = el => { const style = getComputedStyle(el), rect = el.getBoundingClientRect();"
        "    return !el.disabled && style.display !== 'none' && style.visibility !== 'hidden'"
        "      && rect.width > 0 && rect.height > 0; };"
        "  const controls = [...document.querySelectorAll('button, input[type=\"submit\"], [role=\"button\"]')]"
        "    .filter(visible);"
        "  const words = ['登入','登录','繼續','继续','login','sign in','continue','next'];"
        "  const preferred = controls.find(el => el.matches('button[type=\"submit\"], input[type=\"submit\"]'))"
        "    || controls.find(el => words.some(w => (el.innerText || el.value || '').toLowerCase().includes(w)))"
        "    || controls[0];"
        "  if (!preferred) return false;"
        "  preferred.click();"
        "  return true;"
        "})()";
    std::string r;
    if (!EvalUtf8(cdp, kJs, r, log)) return false;
    return ResultTruthy(r);
}

bool HasPasswordInput(msc::cdp::Session& cdp, const HttpLoginLogFn& log) {
    static const char kJs[] =
        "(() => [...document.querySelectorAll('input')].some(el => {"
        "  const rect = el.getBoundingClientRect(), style = getComputedStyle(el);"
        "  return (el.type || '').toLowerCase() === 'password' && !el.disabled"
        "    && rect.width > 0 && rect.height > 0 && style.display !== 'none'"
        "    && style.visibility !== 'hidden';"
        "}))()";
    std::string r;
    if (!EvalUtf8(cdp, kJs, r, log)) return false;
    return ResultTruthy(r);
}

bool HarvestSession(msc::cdp::Session& cdp, GamaPassDeviceLoginAccount& acc,
                    const HttpLoginLogFn& log) {
    static const char kJs[] =
        "(() => { try { const openId = localStorage.getItem('current_user_open_id') || '';"
        "  return JSON.stringify({"
        "    deviceId: localStorage.getItem('device_id') || '',"
        "    openId: openId,"
        "    trackId: localStorage.getItem('track_id') || '',"
        "    user: localStorage.getItem(openId ? ('user_' + openId) : '') || ''"
        "  }); } catch (e) { return '{}'; } })()";
    std::string raw;
    if (!EvalUtf8(cdp, kJs, raw, log)) return false;
    const std::string deviceId = JsonGetString(raw, "deviceId");
    const std::string openId = JsonGetString(raw, "openId");
    const std::string trackId = JsonGetString(raw, "trackId");
    const std::string user = JsonGetString(raw, "user");
    if (!deviceId.empty() && deviceId != acc.deviceId) {
        LogLine(log, L"[gp-device-login] 页面 device_id 与卖家不一致，保留卖家值");
    }
    if (!openId.empty()) acc.openId = openId;
    if (!trackId.empty()) acc.trackId = trackId;
    if (!user.empty()) {
        const std::string tok = JsonGetString(user, "token");
        if (!tok.empty()) acc.userToken = tok;
        const std::string rt = JsonGetString(user, "refreshToken");
        if (!rt.empty()) acc.refreshToken = rt;
    }
    return !acc.openId.empty() || !acc.userToken.empty();
}

bool UrlLooksTwoFactor(msc::cdp::Session& cdp, const HttpLoginLogFn& log) {
    std::wstring url;
    if (!cdp.GetUrl(url, [&](const std::wstring& s) { LogLine(log, s); })) return false;
    std::wstring low = url;
    for (auto& c : low) c = (wchar_t)towlower(c);
    return low.find(L"two-factor") != std::wstring::npos || low.find(L"otp") != std::wstring::npos ||
           low.find(L"2fa") != std::wstring::npos;
}

void CloseHelperAfterSuccess(msc::cdp::Session& cdp, const HttpLoginLogFn& log) {
    LogLine(log, L"[gp-device-login] 登录成功，关闭独立调试窗（先 Browser.close 落盘 Cookie；只动调试口 19223）");
    Sleep(600);
    cdp.Close();
    if (!msc::cdp::CloseRemoteBrowser(kDeviceLoginDebugPort,
                                      [&](const std::wstring& s) { LogLine(log, s); })) {
        LogLine(log, L"[gp-device-login] 独立调试窗未能自动关掉，请手动关那一扇后再点 GAMA PASS自动登录");
    }
}

void RunLogin(GamaPassDeviceLoginAccount acc, std::wstring storePath, HttpLoginLogFn log) {
    struct BusyGuard {
        ~BusyGuard() { gBusy.store(false); }
    } busyGuard;

    if (acc.deviceId.empty()) {
        LogLine(log, L"[gp-device-login] 缺少卖家 device_id，已中止（禁止自造）");
        return;
    }
    LogLine(log, L"[gp-device-login] 使用卖家 device_id（不自造）");
    SaveGamaPassDeviceLoginAccount(storePath, acc);

    msc::cdp::BrowserProfile profile;
    std::wstring label;
    if (!ResolveGamaPassDeviceLoginBrowser(profile.exe, label, log, acc.browserKind)) {
        LogLine(log, L"[gp-device-login] 未找到所选浏览器（需要 Chrome++ / Chrome / Edge，不支持 360）");
        return;
    }
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
        LogLine(log, L"[gp-device-login] 无法解析 LocalAppData");
        return;
    }
    const std::wstring xcat = std::wstring(localApp) + L"\\XCat";
    const std::wstring root = xcat + L"\\" + kProfileDirName;
    profile.userData = root + L"\\" + ProfileLeafForExe(profile.exe);
    if (!EnsureDir(xcat) || !EnsureDir(root) || !EnsureDir(profile.userData)) {
        LogLine(log, L"[gp-device-login] 无法创建独立配置目录");
        return;
    }
    LogLine(log, L"[gp-device-login] 浏览器=" + label + L" exe=" + profile.exe);
    LogLine(log, L"[gp-device-login] 独立配置目录=" + profile.userData +
                     L"（不碰日常 User Data）");

    msc::cdp::Session cdp;
    std::wstring fail;
    if (!cdp.EnsureBrowser(profile, kDeviceLoginDebugPort,
                           [&](const std::wstring& s) { LogLine(log, s); }, &fail)) {
        LogLine(log, L"[gp-device-login] 无法打开调试浏览器 " + fail);
        return;
    }

    std::string ignore;
    cdp.Command("Page.enable", "{}", ignore, [&](const std::wstring& s) { LogLine(log, s); });
    cdp.Command("Runtime.enable", "{}", ignore, [&](const std::wstring& s) { LogLine(log, s); });

    if (!PinDeviceId(cdp, acc.deviceId, log)) return;
    if (!cdp.Navigate(kStartUrl, [&](const std::wstring& s) { LogLine(log, s); })) {
        LogLine(log, L"[gp-device-login] 打开登录页失败");
        return;
    }
    if (!WaitDocumentUsable(cdp, 20000, log)) return;
    PinDeviceId(cdp, acc.deviceId, log);

    if (HarvestSession(cdp, acc, log) && !HasPasswordInput(cdp, log)) {
        SaveGamaPassDeviceLoginAccount(storePath, acc);
        LogLine(log, L"[gp-device-login] 该独立窗口已有会话（device_id 已钉死）。");
        CloseHelperAfterSuccess(cdp, log);
        return;
    }

    const DWORD t0 = GetTickCount();
    bool filled = false;
    bool twoFactor = false;
    while (GetTickCount() - t0 < 45000) {
        if (UrlLooksTwoFactor(cdp, log)) {
            twoFactor = true;
            LogLine(log, L"[gp-device-login] 需要二次验证：请在独立窗口内完成；完成后会自动关窗。");
            break;
        }
        if (HasPasswordInput(cdp, log)) {
            const bool emailOk = FillVisibleInput(cdp, false, acc.email, log);
            const bool passOk = FillVisibleInput(cdp, true, acc.password, log);
            if (emailOk && passOk) {
                EnsureKeepSignedIn(cdp, log);
                if (ClickPrimaryButton(cdp, log)) {
                    filled = true;
                    LogLine(log, L"[gp-device-login] 已提交账密（已尝试勾选保持登入）");
                    break;
                }
            }
        }
        Sleep(500);
    }
    if (!filled && !twoFactor) {
        LogLine(log, L"[gp-device-login] 未找到账密框。请在独立窗口内手动登录；device_id 已预先写入。");
        return;
    }

    const DWORD t1 = GetTickCount();
    bool notedTwoFactor = twoFactor;
    while (GetTickCount() - t1 < 90000) {
        if (UrlLooksTwoFactor(cdp, log)) {
            if (!notedTwoFactor) {
                notedTwoFactor = true;
                LogLine(log, L"[gp-device-login] 需要二次验证：请在独立窗口内完成；完成后会自动关窗。");
            }
        } else if (HarvestSession(cdp, acc, log) && !HasPasswordInput(cdp, log)) {
            SaveGamaPassDeviceLoginAccount(storePath, acc);
            LogLine(log, L"[gp-device-login] 登录完成，已保存 device_id / 会话。不换票、不开游戏。");
            CloseHelperAfterSuccess(cdp, log);
            return;
        }
        Sleep(800);
    }
    SaveGamaPassDeviceLoginAccount(storePath, acc);
    LogLine(log, L"[gp-device-login] 等待登录结果超时，独立窗口保持打开。若窗口里已经登入，下次会复用同一 device_id。");
}

}  // namespace

std::string FormatGamaPassDeviceLoginLine(const GamaPassDeviceLoginAccount& acc) {
    if (!acc.userToken.empty() || !acc.refreshToken.empty()) {
        return acc.email + "----" + acc.emailPassword + "----" + acc.password + "----" + acc.deviceId +
               "----" + acc.userToken + "----" + acc.refreshToken;
    }
    return acc.email + "----" + acc.password + "----" + acc.emailPassword + "----" + acc.deviceId;
}

bool IsHex32DeviceId(const std::string& s) {
    if (s.size() != 32) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

bool ParseGamaPassDeviceLoginLine(const std::string& raw, GamaPassDeviceLoginAccount& out,
                                  std::string& err) {
    out = {};
    err.clear();
    std::string s = raw;
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\t'), s.end());
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) {
        err = "请粘贴卖家账号行";
        return false;
    }

    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t p = s.find("----", start);
        if (p == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, p - start));
        start = p + 4;
    }

    if (parts.size() == 6) {
        out.email = parts[0];
        out.emailPassword = parts[1];
        out.password = parts[2];
        out.deviceId = parts[3];
        out.userToken = parts[4];
        out.refreshToken = parts[5];
    } else if (parts.size() == 4) {
        out.email = parts[0];
        out.password = parts[1];
        out.emailPassword = parts[2];
        out.deviceId = parts[3];
    } else {
        err = "格式须为 账号----密码----邮箱密码----device_id";
        return false;
    }
    if (out.email.empty() || out.email.find('@') == std::string::npos) {
        err = "账号必须是有效邮箱地址";
        return false;
    }
    if (out.password.empty()) {
        err = "Gama Pass 密码不能为空";
        return false;
    }
    if (out.emailPassword.empty()) {
        err = "邮箱密码不能为空";
        return false;
    }
    if (!IsHex32DeviceId(out.deviceId)) {
        err = "device_id 必须是 32 位十六进制（禁止自造、不加横线）";
        return false;
    }
    return true;
}

std::wstring GamaPassDeviceLoginStorePath(const std::wstring& prefsBinDir) {
    if (!prefsBinDir.empty()) {
        const std::wstring st = prefsBinDir + L"\\state";
        EnsureDir(st);
        return st + L"\\gp_device_login.json";
    }
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return {};
    const std::wstring root = std::wstring(localApp) + L"\\XCat";
    EnsureDir(root);
    return root + L"\\gp_device_login.json";
}

bool LoadGamaPassDeviceLoginAccount(const std::wstring& storePath, GamaPassDeviceLoginAccount& out) {
    out = {};
    std::string body;
    if (!FileReadAll(storePath, body) || body.empty()) return false;
    out.email = JsonGetString(body, "email");
    out.password = JsonGetString(body, "password");
    out.emailPassword = JsonGetString(body, "emailPassword");
    if (out.emailPassword.empty()) out.emailPassword = JsonGetString(body, "gamaPassword");
    out.deviceId = JsonGetString(body, "deviceId");
    out.openId = JsonGetString(body, "openId");
    out.userToken = JsonGetString(body, "userToken");
    out.refreshToken = JsonGetString(body, "refreshToken");
    out.trackId = JsonGetString(body, "trackId");
    out.browserKind = ClampBrowserKind(JsonGetInt(body, "browserKind", 0));
    return !out.email.empty() || !out.deviceId.empty();
}

bool SaveGamaPassDeviceLoginAccount(const std::wstring& storePath,
                                    const GamaPassDeviceLoginAccount& acc) {
    if (storePath.empty()) return false;
    std::ostringstream o;
    o << "{\n"
      << "  \"email\":\"" << JsonEscape(acc.email) << "\",\n"
      << "  \"password\":\"" << JsonEscape(acc.password) << "\",\n"
      << "  \"emailPassword\":\"" << JsonEscape(acc.emailPassword) << "\",\n"
      << "  \"deviceId\":\"" << JsonEscape(acc.deviceId) << "\",\n"
      << "  \"openId\":\"" << JsonEscape(acc.openId) << "\",\n"
      << "  \"userToken\":\"" << JsonEscape(acc.userToken) << "\",\n"
      << "  \"refreshToken\":\"" << JsonEscape(acc.refreshToken) << "\",\n"
      << "  \"trackId\":\"" << JsonEscape(acc.trackId) << "\",\n"
      << "  \"browserKind\":" << static_cast<int>(acc.browserKind) << "\n"
      << "}\n";
    return FileWriteAll(storePath, o.str());
}

bool ResolveGamaPassDeviceLoginBrowser(std::wstring& outExe, std::wstring& outLabel,
                                       HttpLoginLogFn log, GpDeviceLoginBrowserKind kind) {
    outExe.clear();
    outLabel.clear();
    kind = ClampBrowserKind(static_cast<int>(kind));
    std::wstring preferred;
    HttpGamaPassPreferredBrowserExe(preferred);
    std::vector<std::wstring> cands;
    if (!preferred.empty() && !Is360Exe(preferred)) cands.push_back(preferred);
    CollectFallbackExes(cands);

    auto pick = [&](auto pred) -> bool {
        for (const auto& e : cands) {
            if (Is360Exe(e)) continue;
            if (pred(e)) {
                outExe = e;
                outLabel = BrowserLabel(e);
                return true;
            }
        }
        return false;
    };

    bool ok = false;
    if (kind == GpDeviceLoginBrowserKind::ChromePlus) {
        ok = pick([](const std::wstring& e) { return IsChromePlus(e); });
        if (!ok) LogLine(log, L"[gp-device-login] 未找到 Chrome++（chrome++.ini / version.dll）");
    } else if (kind == GpDeviceLoginBrowserKind::Chrome) {
        ok = pick([](const std::wstring& e) { return IsOfficialChromeExe(e); });
        if (!ok) LogLine(log, L"[gp-device-login] 未找到 Google Chrome");
    } else if (kind == GpDeviceLoginBrowserKind::Edge) {
        ok = pick([](const std::wstring& e) { return IsEdgeExe(e); });
        if (!ok) LogLine(log, L"[gp-device-login] 未找到 Microsoft Edge");
    } else {
        ok = pick([](const std::wstring& e) { return IsChromePlus(e); }) ||
             pick([](const std::wstring& e) { return IsOfficialChromeExe(e); }) ||
             pick([](const std::wstring& e) { return IsEdgeExe(e); });
        if (!ok) {
            if (!preferred.empty() && Is360Exe(preferred)) {
                LogLine(log, L"[gp-device-login] 系统默认是 360，本模块不支持，请安装 Chrome++ / Chrome / Edge");
            } else {
                LogLine(log, L"[gp-device-login] 未找到 Chrome++ / Chrome / Edge");
            }
        }
    }
    return ok;
}

bool IsGamaPassDeviceLoginBusy() { return gBusy.load(); }

std::wstring GamaPassDeviceLoginUserDataDir(const std::wstring& exe) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return {};
    return std::wstring(localApp) + L"\\XCat\\" + kProfileDirName + L"\\" + ProfileLeafForExe(exe);
}

bool StartGamaPassDeviceLogin(const GamaPassDeviceLoginAccount& acc, const std::wstring& storePath,
                              HttpLoginLogFn log, std::wstring& err) {
    err.clear();
    if (gBusy.exchange(true)) {
        err = L"账密登录助手正在运行";
        return false;
    }
    if (acc.email.empty() || acc.password.empty()) {
        gBusy.store(false);
        err = L"请粘贴卖家账号行（账号----密码----邮箱密码----device_id）";
        return false;
    }
    if (acc.deviceId.empty()) {
        gBusy.store(false);
        err = L"缺少 device_id。卖家行必须带第 4 段，禁止自造";
        return false;
    }
    LogLine(log, L"[gp-device-login] 开始：独立窗口登录（钉卖家 device_id，不换票、不开游戏）");
    std::thread([acc, storePath, log]() { RunLogin(acc, storePath, log); }).detach();
    return true;
}

}  // namespace msc::launcher
