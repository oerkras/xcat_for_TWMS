#include "ngm_protocol_allow.h"

#include "win_uia.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <UIAutomation.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace msc::launcher {
namespace {

void LogLine(const NgmProtocolLogFn& log, const std::wstring& s) {
    if (log) log(s);
}

bool EnsureDir(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return CreateDirectoryW(p.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool WriteAllAtomic(const std::wstring& path, const std::string& body) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) return false;
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(path.c_str());
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

size_t SkipWs(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i;
}

bool OriginAllowsNgm(const std::string& json, const char* origin) {
    const std::string key = std::string("\"") + origin + "\"";
    size_t p = 0;
    while ((p = json.find(key, p)) != std::string::npos) {
        size_t i = SkipWs(json, p + key.size());
        if (i >= json.size() || json[i] != ':') {
            p += key.size();
            continue;
        }
        i = SkipWs(json, i + 1);
        if (i >= json.size() || json[i] != '{') {
            p += key.size();
            continue;
        }
        const size_t end = json.find('}', i);
        if (end == std::string::npos) return false;
        const std::string obj = json.substr(i, end - i + 1);
        const size_t n = obj.find("\"ngm\"");
        if (n != std::string::npos) {
            const size_t t = obj.find("true", n);
            const size_t f = obj.find("false", n);
            if (t != std::string::npos && (f == std::string::npos || t < f)) return true;
        }
        p += key.size();
    }
    return false;
}

bool InsertNgmAllows(std::string& json) {
    static const char* kOrigins[] = {
        "https://maplestoryclassic.beanfun.com",
        "https://maplestoryclassic.beanfun.com:443",
        "https://galaxy.games.gamania.com",
        "https://galaxy.games.gamania.com:443",
    };
    std::string extra;
    for (const char* origin : kOrigins) {
        if (OriginAllowsNgm(json, origin)) continue;
        if (!extra.empty()) extra += ',';
        extra += "\"";
        extra += origin;
        extra += "\":{\"ngm\":true}";
    }
    if (extra.empty()) return false;

    auto insertAfterBrace = [&](size_t brace) {
        const size_t n = SkipWs(json, brace + 1);
        if (n < json.size() && json[n] == '}') {
            json.insert(brace + 1, extra);
        } else {
            json.insert(brace + 1, extra + ",");
        }
    };

    const size_t pairs = json.find("\"allowed_origin_protocol_pairs\"");
    if (pairs != std::string::npos) {
        const size_t brace = json.find('{', pairs);
        if (brace != std::string::npos) {
            insertAfterBrace(brace);
            return true;
        }
    }
    const size_t ph = json.find("\"protocol_handler\"");
    if (ph != std::string::npos) {
        const size_t brace = json.find('{', ph);
        if (brace != std::string::npos) {
            json.insert(brace + 1, "\"allowed_origin_protocol_pairs\":{" + extra + "},");
            return true;
        }
    }
    const size_t first = json.find('{');
    if (first != std::string::npos) {
        json.insert(first + 1,
                    "\"protocol_handler\":{\"allowed_origin_protocol_pairs\":{" + extra + "}},");
        return true;
    }
    json = "{\"protocol_handler\":{\"allowed_origin_protocol_pairs\":{" + extra + "}}}";
    return true;
}

bool ProcessIsChromiumBrowser(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
    CloseHandle(h);
    if (!ok || !path[0]) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* leaf = slash ? slash + 1 : path;
    wchar_t low[64]{};
    size_t i = 0;
    for (; leaf[i] && i + 1 < 64; ++i) {
        const wchar_t c = leaf[i];
        low[i] = (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
    }
    low[i] = 0;
    return wcscmp(low, L"chrome.exe") == 0 || wcscmp(low, L"msedge.exe") == 0 ||
           wcscmp(low, L"chromium.exe") == 0;
}

BOOL CALLBACK CollectBrowserHwnds(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!ProcessIsChromiumBrowser(pid)) return TRUE;
    auto* v = reinterpret_cast<std::vector<HWND>*>(lp);
    if (v->size() < 12) v->push_back(hwnd);
    return TRUE;
}

bool ToggleOnIfOff(IUIAutomationElement* el) {
    if (!el) return false;
    IUIAutomationTogglePattern* tog = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_IUIAutomationTogglePattern,
                                       reinterpret_cast<void**>(&tog))) ||
        !tog) {
        return false;
    }
    ToggleState st = ToggleState_Off;
    tog->get_CurrentToggleState(&st);
    bool ok = true;
    if (st != ToggleState_On) ok = SUCCEEDED(tog->Toggle());
    tog->Release();
    return ok;
}

bool AcceptInBrowserWindow(msc::uia::Session& uia, HWND hwnd) {
    IUIAutomationElement* root = uia.ElementFromHwnd(hwnd);
    if (!root) return false;

    IUIAutomationElement* marker = uia.FindByName(root, L"想打开此应用", true, false);
    if (!marker) marker = uia.FindByName(root, L"wants to open this application", true, false);
    if (!marker) marker = uia.FindByName(root, L"始终允许", true, true);
    if (!marker) marker = uia.FindByName(root, L"Always allow", true, true);
    if (!marker) {
        root->Release();
        return false;
    }
    marker->Release();

    IUIAutomationElement* always = uia.FindByName(root, L"始终允许", true, true);
    if (!always) always = uia.FindByName(root, L"Always allow", true, true);
    bool did = false;
    if (always) {
        if (ToggleOnIfOff(always)) {
            did = true;
        } else {
            did = uia.InvokeOrClick(always, false);
        }
        always->Release();
    }

    IUIAutomationElement* openBtn = uia.FindByName(root, L"打开Nexon Game Manager", true, true);
    if (!openBtn) openBtn = uia.FindByName(root, L"打开 Nexon Game Manager", true, true);
    if (!openBtn) openBtn = uia.FindByName(root, L"Open Nexon Game Manager", true, true);
    if (openBtn) {
        did = uia.InvokeOrClick(openBtn, false) || did;
        openBtn->Release();
    }
    root->Release();
    return did;
}

}  // namespace

void SeedNgmProtocolAllowlist(const std::wstring& userDataDir, const NgmProtocolLogFn& log) {
    if (userDataDir.empty()) return;
    const std::wstring def = userDataDir + L"\\Default";
    if (!EnsureDir(userDataDir) || !EnsureDir(def)) return;
    const std::wstring path = def + L"\\Preferences";
    std::string json;
    if (!ReadAll(path, json) || json.empty()) json = "{}";
    if (json.size() > 16u * 1024u * 1024u) {
        LogLine(log, L"[ngm-protocol] Preferences 过大，跳过写入始终允许");
        return;
    }
    if (!InsertNgmAllows(json)) return;
    if (!WriteAllAtomic(path, json)) {
        LogLine(log, L"[ngm-protocol] 无法写入独立罐 Preferences（始终允许 ngm）");
        return;
    }
    LogLine(log, L"[ngm-protocol] 已写入独立罐：始终允许官网打开 NGM（ngm://）");
}

bool TryAcceptNgmProtocolDialog(const NgmProtocolLogFn& log) {
    static DWORD sLastTry = 0;
    static bool sLoggedOk = false;
    const DWORD now = GetTickCount();
    if (sLastTry != 0 && now - sLastTry < 800u) return false;
    sLastTry = now;

    std::vector<HWND> hwnds;
    EnumWindows(CollectBrowserHwnds, reinterpret_cast<LPARAM>(&hwnds));
    if (hwnds.empty()) return false;

    msc::uia::Session uia;
    if (!uia.Init(nullptr) || !uia.Raw()) return false;

    bool did = false;
    for (HWND hwnd : hwnds) {
        if (AcceptInBrowserWindow(uia, hwnd)) {
            did = true;
            break;
        }
    }
    if (did && !sLoggedOk) {
        sLoggedOk = true;
        LogLine(log, L"[ngm-protocol] 已勾选始终允许并打开 NGM");
    }
    return did;
}

}  // namespace msc::launcher
