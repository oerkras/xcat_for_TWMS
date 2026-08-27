#include "gamapass_device_login.h"

#include "chromium_cdp.h"
#include "gamapass_cdp_login.h"
#include "gamapass_login_phase.h"
#include "gamapass_ticket_harvest.h"
#include "http_gamapass_login.h"
#include "msc_launch.h"
#include "msc_webview_login.h"
#include "ngm_protocol_allow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wincrypt.h>
#include <ShlObj.h>
#include <winreg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace msc::launcher {
namespace {

constexpr int kDeviceLoginDebugPort = 19223;
constexpr wchar_t kProfileDirName[] = L"GpDeviceLoginProfile";
constexpr wchar_t kGalaxyLogin[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/mstc"
    L"?redirect_url=https://maplestoryclassic.beanfun.com/Main";

std::atomic<bool> gBusy{false};
std::atomic<bool> gClearing{false};
std::atomic<bool> gClearFinished{false};
std::mutex gClearMu;
std::wstring gClearErr;

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

std::wstring IsolatedProfileRoot() {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return {};
    return std::wstring(localApp) + L"\\XCat\\" + kProfileDirName;
}

bool IsSafeIsolatedProfileRoot(const std::wstring& root) {
    const std::wstring expected = IsolatedProfileRoot();
    if (expected.empty() || root.empty()) return false;
    if (_wcsicmp(root.c_str(), expected.c_str()) != 0) return false;
    if (root.find(L"GpDeviceLoginProfile") == std::wstring::npos) return false;
    if (root.find(L"User Data") != std::wstring::npos) return false;
    if (root.find(L"Chrome++ Data") != std::wstring::npos) return false;
    return true;
}

void DeleteDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            RemoveDirectoryW(p.c_str());
            DeleteFileW(p.c_str());
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirRecursive(p);
        } else {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(dir.c_str());
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
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(f);
}

bool DpapiProtect(const std::string& plain, std::vector<uint8_t>& out) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, L"xcat-gp-device-login", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        return false;
    }
    out.assign(blob.pbData, blob.pbData + blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

bool DpapiUnprotect(const uint8_t* data, size_t len, std::string& plain) {
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(data);
    in.cbData = static_cast<DWORD>(len);
    DATA_BLOB blob{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &blob)) {
        return false;
    }
    plain.assign(reinterpret_cast<char*>(blob.pbData), blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

void SecureWipeString(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
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
        LogLine(log, L"[gp-device-login] addScriptToEvaluateOnNewDocument 失败（仍继续打开登录页）");
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

bool HasEmailInput(msc::cdp::Session& cdp, const HttpLoginLogFn& log) {
    static const char kJs[] =
        "(() => {"
        "  const visible = el => { const style = getComputedStyle(el), rect = el.getBoundingClientRect();"
        "    return !el.disabled && style.display !== 'none' && style.visibility !== 'hidden'"
        "      && rect.width > 0 && rect.height > 0; };"
        "  const all = [...document.querySelectorAll('input')].filter(visible);"
        "  return Boolean(all.find(el => ['email','tel'].includes((el.type || '').toLowerCase()))"
        "    || all.find(el => ['username','email'].includes((el.autocomplete || '').toLowerCase()))"
        "    || all.find(el => (el.type || 'text').toLowerCase() === 'text'));"
        "})()";
    std::string r;
    if (!EvalUtf8(cdp, kJs, r, log)) return false;
    return ResultTruthy(r);
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
    const int port = cdp.Port() > 0 ? cdp.Port() : kDeviceLoginDebugPort;
    LogLine(log, L"[gp-device-login] 换票完成，关闭独立调试窗（先 Browser.close 落盘 Cookie；只动调试口 " +
                     std::to_wstring(port) + L"）");
    Sleep(600);
    cdp.Close();
    if (!msc::cdp::CloseRemoteBrowser(port, [&](const std::wstring& s) { LogLine(log, s); })) {
        LogLine(log, L"[gp-device-login] 独立调试窗未能自动关掉，请手动关那一扇");
    }
}

struct GpFillState {
    bool emailSubmitted = false;
    bool passwordSubmitted = false;
    bool notedTwoFactor = false;
};

void TryFillGpLoginOnce(msc::cdp::Session& cdp, const GamaPassDeviceLoginAccount& acc,
                        GpFillState& st, const HttpLoginLogFn& log) {
    if (GamaPassLoginCanceled()) return;
    if (st.passwordSubmitted) return;
    std::wstring cur;
    if (cdp.GetUrl(cur, [&](const std::wstring& s) { LogLine(log, s); })) {
        std::wstring low = cur;
        for (auto& c : low) c = (wchar_t)towlower(c);
        if (low.find(L"/login/finished") != std::wstring::npos) return;
    }
    if (UrlLooksTwoFactor(cdp, log)) {
        if (!st.notedTwoFactor) {
            st.notedTwoFactor = true;
            LogLine(log, L"[gp-device-login] 需要二次验证：请在独立窗口内完成；完成后会自动换票开游戏。");
        }
        SetGamaPassUiPhase(GamaPassUiPhase::TwoFactor);
        return;
    }
    if (HasPasswordInput(cdp, log)) {
        SetGamaPassUiPhase(GamaPassUiPhase::FillingForm);
        const bool emailOk = FillVisibleInput(cdp, false, acc.email, log);
        const bool passOk = FillVisibleInput(cdp, true, acc.password, log);
        if (passOk) {
            EnsureKeepSignedIn(cdp, log);
            if (ClickPrimaryButton(cdp, log)) {
                st.passwordSubmitted = true;
                LogLine(log, L"[gp-device-login] 已提交 Gama Pass 密码" +
                                 std::wstring(emailOk ? L"" : L"（本步无邮箱框）"));
            }
        }
        return;
    }
    if (!st.emailSubmitted && HasEmailInput(cdp, log)) {
        SetGamaPassUiPhase(GamaPassUiPhase::FillingForm);
        if (FillVisibleInput(cdp, false, acc.email, log)) {
            EnsureKeepSignedIn(cdp, log);
            if (ClickPrimaryButton(cdp, log)) {
                st.emailSubmitted = true;
                LogLine(log, L"[gp-device-login] 已填邮箱并点继续，等待密码框…");
            }
        }
    }
}

bool TryAttachAlreadyRunningClassic(GamaPassDeviceLoginAccount& acc, const std::wstring& storePath,
                                    const HttpLoginLogFn& log) {
    FILETIME noCutoff{};
    bool dummyNgm = false;
    auto already = GamaPassTryHarvestClassicTicket(noCutoff, dummyNgm, log, L"[gp-device-login]");
    if (!already.ok || !already.ticketFilled) return false;
    SaveGamaPassDeviceLoginAccount(storePath, acc);
    LogLine(log, L"[gp-device-login] 已有经典版在跑（cmdline 有票），跳过 Galaxy/選號，直接接管"
                 L"（避免再點繼續打出 SPGA0001）…");
    if (!msc::weblogin::LaunchClassicAfterTicket(std::move(already.ticket))) {
        LogLine(log, L"[gp-device-login] 已有经典版但接管失败，仍走账密登录");
        return false;
    }
    return true;
}

void LaunchClassicAfterIsolatedTicket(msc::cdp::Session& cdp, GamaPassDeviceLoginAccount& acc,
                                      const std::wstring& storePath, const HttpLoginLogFn& log) {
    if (TryAttachAlreadyRunningClassic(acc, storePath, log)) {
        CloseHelperAfterSuccess(cdp, log);
        return;
    }
    LogLine(log, L"[gp-device-login] 从 Galaxy 点 Gama Pass 换票（select-account 必须走 OAuth，不能直接打开）");
    GpFillState fill;
    auto onLogin = [&](msc::cdp::Session& s, HttpLoginLogFn lg) { TryFillGpLoginOnce(s, acc, fill, lg); };
    const FILETIME sessionNotBefore = GamaPassSessionNotBeforeNow();
    bool sawNgm = false;
    constexpr int kMaxRounds = 3;
    constexpr int kCdpTimeoutMs = 240000;
    HttpLoginResult lr;

    auto abortCanceled = [&]() {
        LogLine(log, L"[gp-device-login] 已取消账密直登（不接管经典版、不杀游戏）");
        const int port = cdp.Port() > 0 ? cdp.Port() : kDeviceLoginDebugPort;
        cdp.Close();
        (void)msc::cdp::CloseRemoteBrowser(port, [&](const std::wstring& s) { LogLine(log, s); });
    };

    auto attachClassic = [&](GalaxyTicket ticket) -> bool {
        if (msc::weblogin::LaunchClassicAfterTicket(std::move(ticket))) return true;
        LogLine(log, L"[gp-device-login] 换票成功，经典版尚未出现，再等一会接管（不重开 Galaxy、不点第二次 Gama Pass）…");
        for (int i = 0; i < 8; ++i) {
            if (GamaPassLoginCanceled()) return false;
            Sleep(1500);
            auto harvested = GamaPassTryHarvestClassicTicket(sessionNotBefore, sawNgm, log,
                                                             L"[gp-device-login]");
            if (harvested.ok && harvested.ticketFilled &&
                msc::weblogin::LaunchClassicAfterTicket(std::move(harvested.ticket))) {
                return true;
            }
        }
        return false;
    };

    for (int round = 1; round <= kMaxRounds; ++round) {
        if (GamaPassLoginCanceled()) {
            abortCanceled();
            return;
        }
        if (round > 1) {
            LogLine(log, L"[gp-device-login] 整轮结束仍未拉起经典版，同独立罐再试第 " +
                             std::to_wstring(round) + L"/" + std::to_wstring(kMaxRounds) +
                             L" 轮（不清 Cookie、不关窗、不 refresh）…");
            fill = {};
            Sleep(2000);
            if (GamaPassLoginCanceled()) {
                abortCanceled();
                return;
            }
            auto late = GamaPassTryHarvestClassicTicket(sessionNotBefore, sawNgm, log,
                                                        L"[gp-device-login]");
            if (late.ok && late.ticketFilled) {
                lr = std::move(late);
                break;
            }
        }
        lr = HttpGamaPassCdpLoginToOttOnConnected(cdp, log, kCdpTimeoutMs,
                                                  cdp.Port() > 0 ? cdp.Port() : kDeviceLoginDebugPort,
                                                  onLogin);
        if (GamaPassLoginCanceled() || lr.error == HttpLoginError::Cancelled) {
            abortCanceled();
            return;
        }
        if (lr.ok && lr.ticketFilled) break;
        const bool retryable =
            lr.error == HttpLoginError::OttMissing || lr.error == HttpLoginError::Network;
        if (!retryable) break;
        // TokenWait 已见 NGM 再重开 Galaxy/點繼續 → beanfun SPGA0001（AA7E 03:43→03:46）
        if (IsNgmProcessRunningCreatedAfter(sessionNotBefore)) {
            LogLine(log, L"[gp-device-login] 本轮已见 NGM，不再重开 Galaxy（防選號閒置 SPGA0001）；"
                         L"只再等经典版 cmdline 票…");
            bool got = false;
            for (int i = 0; i < 20; ++i) {
                if (GamaPassLoginCanceled()) {
                    abortCanceled();
                    return;
                }
                Sleep(1500);
                auto late = GamaPassTryHarvestClassicTicket(sessionNotBefore, sawNgm, log,
                                                            L"[gp-device-login]");
                if (late.ok && late.ticketFilled) {
                    lr = std::move(late);
                    got = true;
                    break;
                }
            }
            if (!got) {
                LogLine(log, L"[gp-device-login] NGM 已启动但经典版仍未出现。请看 NGM/游戏是否被拦，"
                             L"不要再点選號繼續。");
            }
            break;
        }
    }

    if (lr.ok && lr.ticketFilled) {
        SaveGamaPassDeviceLoginAccount(storePath, acc);
        LogLine(log, L"[gp-device-login] 换票成功 uid=" + lr.ticket.userObjectId + L" gid=" +
                         lr.ticket.gid + L"，接管经典版（不调用 NGM）");
        if (!attachClassic(std::move(lr.ticket))) {
            if (GamaPassLoginCanceled()) {
                abortCanceled();
                return;
            }
            LogLine(log, L"[gp-device-login] 换票成功，但未接管到经典版。"
                         L"请确认官网已拉起 Maplestory_Classic.exe 后再点一次（已有窗口不会重填账密）");
        }
        CloseHelperAfterSuccess(cdp, log);
        return;
    }
    LogLine(log, L"[gp-device-login] 换票未完成 [" +
                     Utf8ToWide(HttpLoginErrorName(lr.error)) + L"] " + Utf8ToWide(lr.message) +
                     L"。独立窗口保持打开；下次点同一按钮会复用会话，不再直开 select-account。");
}

bool OpenGalaxyInIsolatedProfile(const msc::cdp::BrowserProfile& profile, const HttpLoginLogFn& log) {
    if (profile.exe.empty() || profile.userData.empty()) return false;
    std::wstring cmd = L"\"";
    cmd += profile.exe;
    cmd += L"\" --user-data-dir=\"";
    cmd += profile.userData;
    cmd += L"\" --force-renderer-accessibility --new-window \"";
    cmd += kGalaxyLogin;
    cmd += L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    LogLine(log, L"[gp-device-login] 把 Galaxy 交给独立罐窗口（无新调试口、不碰日常罐）…");
    if (!CreateProcessW(profile.exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        LogLine(log, L"[gp-device-login] CreateProcess 失败 err=" + std::to_wstring(GetLastError()));
        return false;
    }
    LogLine(log, L"[gp-device-login] 已把 Galaxy 交给独立罐 pid=" + std::to_wstring(pi.dwProcessId));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void HarvestManualIsolatedLogin(const msc::cdp::BrowserProfile& profile,
                                GamaPassDeviceLoginAccount& acc, const std::wstring& storePath,
                                const HttpLoginLogFn& log) {
    SetGamaPassUiPhase(GamaPassUiPhase::ManualLogin);
    LogLine(log, L"[gp-device-login] 调试口未连上，走手动兜底（BUILD187 同款）："
                 L"请在独立窗口内登录；登录成功后请点「回到 GAMA PLAY」。"
                 L"程序会继续找这个按钮、勾选 NGM 协议窗，并接管经典版。");
    const FILETIME sessionNotBefore = GamaPassSessionNotBeforeNow();
    bool sawNgm = false;
    const DWORD t0 = GetTickCount();
    DWORD lastLog = 0;
    DWORD lastUia = 0;
    while ((int)(GetTickCount() - t0) < 240000) {
        if (GamaPassLoginCanceled()) {
            LogLine(log, L"[gp-device-login] 已取消账密直登");
            return;
        }
        NgmProtocolAllowOpts ngmOpts;
        ngmOpts.debugPort = kDeviceLoginDebugPort;
        ngmOpts.cmdNeedle = L"GpDeviceLoginProfile";
        TryAcceptNgmProtocolDialog([&](const std::wstring& s) { LogLine(log, s); }, ngmOpts);
        if (GetTickCount() - lastUia > 2500) {
            lastUia = GetTickCount();
            if (TryUiaClickBackToGamaPlay(0, L"GpDeviceLoginProfile", log)) {
                SetGamaPassUiPhase(GamaPassUiPhase::WaitNgm);
            }
        }
        auto harvested =
            GamaPassTryHarvestClassicTicket(sessionNotBefore, sawNgm, log, L"[gp-device-login]");
        if (sawNgm) SetGamaPassUiPhase(GamaPassUiPhase::WaitClassic);
        if (harvested.ok && harvested.ticketFilled) {
            SaveGamaPassDeviceLoginAccount(storePath, acc);
            LogLine(log, L"[gp-device-login] 换票成功 uid=" + harvested.ticket.userObjectId +
                             L" gid=" + harvested.ticket.gid + L"，接管经典版（不调用 NGM）");
            if (!msc::weblogin::LaunchClassicAfterTicket(std::move(harvested.ticket))) {
                LogLine(log, L"[gp-device-login] 换票成功，经典版尚未出现，再等一会接管…");
                bool attached = false;
                for (int i = 0; i < 8; ++i) {
                    if (GamaPassLoginCanceled()) return;
                    Sleep(1500);
                    auto late = GamaPassTryHarvestClassicTicket(sessionNotBefore, sawNgm, log,
                                                                L"[gp-device-login]");
                    if (late.ok && late.ticketFilled &&
                        msc::weblogin::LaunchClassicAfterTicket(std::move(late.ticket))) {
                        attached = true;
                        break;
                    }
                }
                if (!attached) {
                    LogLine(log, L"[gp-device-login] 换票成功，但未接管到经典版。"
                                 L"请确认官网已拉起 Maplestory_Classic.exe");
                    return;
                }
            }
            LogLine(log, L"[gp-device-login] 换票完成，关闭独立罐窗口（不碰日常浏览器）");
            (void)msc::cdp::KillBrowsersBlockingProfile(
                profile, -1, [&](const std::wstring& s) { LogLine(log, s); });
            return;
        }
        if (GetTickCount() - lastLog > 8000) {
            lastLog = GetTickCount();
            LogLine(log, sawNgm ? L"[gp-device-login] 已见 NGM，仍等经典版 cmdline 票…"
                                : L"[gp-device-login] 请完成登录并点「回到 GAMA PLAY」；"
                                  L"程序会等官网拉起游戏。");
        }
        Sleep(800);
    }
    LogLine(log, L"[gp-device-login] 手动兜底超时。独立窗口保持打开；下次点同一按钮会复用会话。");
}

void RunLogin(GamaPassDeviceLoginAccount acc, std::wstring storePath, HttpLoginLogFn log) {
    struct BusyGuard {
        ~BusyGuard() {
            SetGamaPassUiPhase(GamaPassUiPhase::Idle);
            gBusy.store(false);
        }
    } busyGuard;

    if (acc.deviceId.empty()) {
        LogLine(log, L"[gp-device-login] 缺少卖家 device_id，已中止（禁止自造）");
        return;
    }
    LogLine(log, L"[gp-device-login] 使用卖家 device_id（不自造）");
    if (TryAttachAlreadyRunningClassic(acc, storePath, log)) return;
    SetGamaPassUiPhase(GamaPassUiPhase::OpeningBrowser);
    SaveGamaPassDeviceLoginAccount(storePath, acc);

    msc::cdp::BrowserProfile profile;
    std::wstring label;
    if (!ResolveGamaPassDeviceLoginBrowser(profile.exe, label, log, acc.browserKind)) {
        LogLine(log, L"[gp-device-login] 未找到所选浏览器（需要 Chrome++ / Google Chrome / Microsoft Edge，不支持 360）");
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
        if (GamaPassLoginCanceled()) {
            LogLine(log, L"[gp-device-login] 已取消账密直登");
            return;
        }
        LogLine(log, L"[gp-device-login] 无法打开调试浏览器 " + fail);
        (void)OpenGalaxyInIsolatedProfile(profile, log);
        HarvestManualIsolatedLogin(profile, acc, storePath, log);
        return;
    }
    if (GamaPassLoginCanceled()) {
        LogLine(log, L"[gp-device-login] 已取消账密直登");
        const int port = cdp.Port() > 0 ? cdp.Port() : kDeviceLoginDebugPort;
        cdp.Close();
        (void)msc::cdp::CloseRemoteBrowser(port, [&](const std::wstring& s) { LogLine(log, s); });
        return;
    }

    std::string ignore;
    cdp.Command("Page.enable", "{}", ignore, [&](const std::wstring& s) { LogLine(log, s); });
    cdp.Command("Runtime.enable", "{}", ignore, [&](const std::wstring& s) { LogLine(log, s); });

    if (!PinDeviceId(cdp, acc.deviceId, log)) {
        LogLine(log, L"[gp-device-login] 预钉 device_id 未完全成功，仍走 Galaxy");
    }
    if (GamaPassLoginCanceled()) {
        LogLine(log, L"[gp-device-login] 已取消账密直登");
        const int port = cdp.Port() > 0 ? cdp.Port() : kDeviceLoginDebugPort;
        cdp.Close();
        (void)msc::cdp::CloseRemoteBrowser(port, [&](const std::wstring& s) { LogLine(log, s); });
        return;
    }
    LogLine(log, L"[gp-device-login] 不直接打开 select-account；先 Galaxy 再点 Gama Pass。"
                 L"未登录则在 OAuth 登录页自动填表。");
    LaunchClassicAfterIsolatedTicket(cdp, acc, storePath, log);
}

std::wstring DeviceLoginPlainStorePath(const std::wstring& dpapiPath) {
    constexpr wchar_t kSuf[] = L".dpapi";
    const size_t n = dpapiPath.size();
    if (n >= 6 && _wcsicmp(dpapiPath.c_str() + (n - 6), kSuf) == 0) {
        return dpapiPath.substr(0, n - 6) + L".json";
    }
    return {};
}

bool ParseAccountFromStoreJson(const std::string& body, GamaPassDeviceLoginAccount& out) {
    out = {};
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
        return st + L"\\gp_device_login.dpapi";
    }
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return {};
    const std::wstring root = std::wstring(localApp) + L"\\XCat";
    EnsureDir(root);
    return root + L"\\gp_device_login.dpapi";
}

bool LoadGamaPassDeviceLoginAccount(const std::wstring& storePath, GamaPassDeviceLoginAccount& out) {
    out = {};
    if (storePath.empty()) return false;

    auto parseWipe = [&](std::string& body) -> bool {
        const bool ok = ParseAccountFromStoreJson(body, out);
        SecureWipeString(body);
        return ok;
    };

    std::string raw;
    if (FileReadAll(storePath, raw) && !raw.empty()) {
        std::string body;
        static const char kMagic[] = "GPDL1";
        bool fromPlain = false;
        if (raw.size() >= 5 && raw.compare(0, 5, kMagic) == 0) {
            const bool dec = DpapiUnprotect(reinterpret_cast<const uint8_t*>(raw.data() + 5),
                                            raw.size() - 5, body);
            SecureWipeString(raw);
            if (!dec) return false;
        } else {
            body = std::move(raw);
            fromPlain = true;
        }
        if (!parseWipe(body)) return false;
        if (fromPlain) (void)SaveGamaPassDeviceLoginAccount(storePath, out);
        return true;
    }

    const std::wstring legacy = DeviceLoginPlainStorePath(storePath);
    if (legacy.empty() || !FileReadAll(legacy, raw) || raw.empty()) return false;
    if (!parseWipe(raw)) return false;
    if (SaveGamaPassDeviceLoginAccount(storePath, out)) {
        DeleteFileW(legacy.c_str());
    }
    return true;
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
    std::string plain = o.str();
    std::vector<uint8_t> blob;
    const bool prot = DpapiProtect(plain, blob);
    SecureWipeString(plain);
    if (!prot) return false;
    std::ofstream f(storePath, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("GPDL1", 5);
    f.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
    if (!f) return false;
    const std::wstring legacy = DeviceLoginPlainStorePath(storePath);
    if (!legacy.empty()) DeleteFileW(legacy.c_str());
    return true;
}

bool DeleteGamaPassDeviceLoginAccount(const std::wstring& storePath) {
    if (storePath.empty()) return false;
    const std::wstring legacy = DeviceLoginPlainStorePath(storePath);
    if (!legacy.empty()) DeleteFileW(legacy.c_str());
    DeleteFileW(storePath.c_str());
    return !FileExistsW(storePath) && (legacy.empty() || !FileExistsW(legacy));
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
        // 自动：Chrome++ > 官方 Chrome > Edge。Win 几乎必有 Edge，旧顺序会永远抢在 Chrome++ 前面。
        ok = pick([](const std::wstring& e) { return IsChromePlus(e); }) ||
             pick([](const std::wstring& e) { return IsOfficialChromeExe(e); }) ||
             pick([](const std::wstring& e) { return IsEdgeExe(e); });
        if (!ok) {
            if (!preferred.empty() && Is360Exe(preferred)) {
                LogLine(log, L"[gp-device-login] 系统默认是 360，本模块不支持，请安装 Chrome++ / Google Chrome / Microsoft Edge");
            } else {
                LogLine(log, L"[gp-device-login] 未找到 Chrome++ / Google Chrome / Microsoft Edge");
            }
        }
    }
    return ok;
}

bool IsGamaPassDeviceLoginBusy() { return gBusy.load(); }

bool IsGamaPassDeviceLoginClearing() { return gClearing.load(); }

bool PollGamaPassDeviceLoginClearResult(std::wstring& err) {
    err.clear();
    if (!gClearFinished.exchange(false)) return false;
    {
        std::lock_guard<std::mutex> lock(gClearMu);
        err = gClearErr;
        gClearErr.clear();
    }
    gClearing.store(false);
    return true;
}

bool CancelGamaPassDeviceLogin(HttpLoginLogFn log) {
    RequestGamaPassLoginCancel();
    if (!gBusy.load()) {
        LogLine(log, L"[gp-device-login] 当前没有进行中的账密直登");
        return false;
    }
    // 关调试窗含 WinHttp / 杀进程 / Sleep，绝不能在 ImGui 点击线程做（B237282/MFL：点取消整窗卡死）。
    LogLine(log, L"[gp-device-login] 用户取消：后台关闭独立调试窗（19223），不碰日常浏览器、不杀游戏");
    std::thread([log]() {
        const auto cdpLog = [&](const std::wstring& s) { LogLine(log, s); };
        // debugPort=-1：连带 --remote-debugging-port=19223 的独立罐一并结束。
        // 若先 CloseRemoteBrowser，Browser.close 握手会把取消线程卡住（空罐第一次）。
        const std::wstring root = IsolatedProfileRoot();
        if (IsSafeIsolatedProfileRoot(root)) {
            static const wchar_t* kLeaves[] = {L"chromeplus", L"chrome", L"edge"};
            for (const wchar_t* leaf : kLeaves) {
                msc::cdp::BrowserProfile p;
                p.userData = root + L"\\" + leaf;
                (void)msc::cdp::KillBrowsersBlockingProfile(p, -1, cdpLog);
            }
        }
        (void)msc::cdp::CloseRemoteBrowser(kDeviceLoginDebugPort, cdpLog);
    }).detach();
    return true;
}

bool ClearGamaPassDeviceLoginProfile(HttpLoginLogFn log, std::wstring& err) {
    err.clear();
    if (gBusy.load()) {
        err = L"账密直登进行中，请等结束后再清空";
        return false;
    }
    if (gClearing.load()) {
        err = L"正在清除独立罐，请稍候";
        return false;
    }
    const std::wstring root = IsolatedProfileRoot();
    if (!IsSafeIsolatedProfileRoot(root)) {
        err = L"无法解析独立罐路径，已拒绝清空";
        return false;
    }
    if (gClearing.exchange(true)) {
        err = L"正在清除独立罐，请稍候";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(gClearMu);
        gClearErr.clear();
        gClearFinished.store(false);
    }
    // 关窗 / 杀进程 / 删目录绝不能在 ImGui 点击线程做（点「确认删除」整窗卡死）。
    LogLine(log, L"[gp-device-login] 开始清除独立罐（后台；不碰日常 User Data / Cookie）");
    std::thread([log, root]() {
        const auto cdpLog = [&](const std::wstring& s) { LogLine(log, s); };
        static const wchar_t* kLeaves[] = {L"chromeplus", L"chrome", L"edge"};
        for (const wchar_t* leaf : kLeaves) {
            msc::cdp::BrowserProfile p;
            p.userData = root + L"\\" + leaf;
            (void)msc::cdp::KillBrowsersBlockingProfile(p, -1, cdpLog);
        }
        Sleep(400);
        if (DirExists(root)) DeleteDirRecursive(root);
        if (DirExists(root)) {
            Sleep(400);
            DeleteDirRecursive(root);
        }
        std::wstring doneErr;
        if (DirExists(root)) {
            doneErr = L"独立罐仍被占用，请先关掉账密登录那扇浏览器再试";
            LogLine(log, L"[gp-device-login] 清空失败：目录仍在 " + root);
        } else {
            wchar_t localApp[MAX_PATH]{};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
                EnsureDir(std::wstring(localApp) + L"\\XCat");
            }
            EnsureDir(root);
            LogLine(log, L"[gp-device-login] 已清空独立罐（未动日常 User Data / Cookie）");
        }
        {
            std::lock_guard<std::mutex> lock(gClearMu);
            gClearErr = std::move(doneErr);
        }
        gClearFinished.store(true);
    }).detach();
    return true;
}

std::wstring GamaPassDeviceLoginUserDataDir(const std::wstring& exe) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return {};
    return std::wstring(localApp) + L"\\XCat\\" + kProfileDirName + L"\\" + ProfileLeafForExe(exe);
}

bool StartGamaPassDeviceLogin(const GamaPassDeviceLoginAccount& acc, const std::wstring& storePath,
                              HttpLoginLogFn log, std::wstring& err) {
    (void)log;
    err.clear();
    if (!kGamaPassDeviceLoginEnabled) {
        err = L"账密登录助手尚未开放";
        return false;
    }
    if (gClearing.load()) {
        err = L"正在清除独立罐，请稍候再登录";
        return false;
    }
    if (msc::weblogin::IsBusy()) {
        err = L"GAMA PASS自动登录正在换票。请等它完成后再用账密拉起";
        return false;
    }
    if (gBusy.exchange(true)) {
        err = L"账密登录正在运行";
        return false;
    }
    ResetGamaPassLoginCancel();
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
    SetGamaPassUiPhase(GamaPassUiPhase::OpeningBrowser);
    HttpLoginLogFn sink = [](const std::wstring& line) { msc::weblogin::QueueLog(line); };
    LogLine(sink, L"[gp-device-login] 开始：独立窗口登录并拉起经典版"
                 L"（已有会话则跳过填表；钉卖家 device_id，不调用 NGM）");
    std::thread([acc, storePath, sink]() { RunLogin(acc, storePath, sink); }).detach();
    return true;
}

}  // namespace msc::launcher
