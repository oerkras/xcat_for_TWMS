#include "ngm_protocol_allow.h"

#include "win_uia.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <UIAutomation.h>

#include <string>
#include <vector>

namespace msc::launcher {
namespace {

void LogLine(const NgmProtocolLogFn& log, const std::wstring& s) {
    if (log) log(s);
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

IUIAutomationElement* FindFirstNamed(msc::uia::Session& uia, IUIAutomationElement* root,
                                     const wchar_t* const* parts, int n, bool preferInvoke) {
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* el = uia.FindByName(root, parts[i], true, preferInvoke);
        if (el) return el;
    }
    return nullptr;
}

// 必须先确认是协议窗，才能点短「打开 / 開啟 / Open」（Edge 按钮常不带 NGM 全名）。
bool LooksLikeProtocolDialog(msc::uia::Session& uia, IUIAutomationElement* root) {
    static const wchar_t* kMarkers[] = {
        L"想打开此应用",
        L"想開啟此應用",
        L"想開啟這個應用程式",
        L"wants to open this application",
        L"wants to open",
        L"trying to open",
        L"正在尝试打开",
        L"正嘗試開啟",
        L"正在嘗試開啟",
        L"打开此类链接",
        L"開啟這類連結",
        L"開啟這類鏈接",
        L"open these types of links",
        L"始终允许",
        L"始終允許",
        L"一律允許",
        L"Always allow",
        L"打开Nexon",
        L"打开 Nexon",
        L"開啟Nexon",
        L"開啟 Nexon",
        L"Open Nexon",
    };
    IUIAutomationElement* marker =
        FindFirstNamed(uia, root, kMarkers, (int)(sizeof(kMarkers) / sizeof(kMarkers[0])), false);
    if (!marker) return false;
    marker->Release();
    return true;
}

bool AcceptInBrowserWindow(msc::uia::Session& uia, HWND hwnd) {
    IUIAutomationElement* root = uia.ElementFromHwnd(hwnd);
    if (!root) return false;
    if (!LooksLikeProtocolDialog(uia, root)) {
        root->Release();
        return false;
    }

    static const wchar_t* kAlways[] = {
        L"始终允许",
        L"始終允許",
        L"一律允許",
        L"Always allow",
    };
    IUIAutomationElement* always =
        FindFirstNamed(uia, root, kAlways, (int)(sizeof(kAlways) / sizeof(kAlways[0])), true);
    bool did = false;
    if (always) {
        if (ToggleOnIfOff(always)) {
            did = true;
        } else {
            did = uia.InvokeOrClick(always, false);
        }
        always->Release();
    }

    static const wchar_t* kOpenLong[] = {
        L"打开Nexon Game Manager",
        L"打开 Nexon Game Manager",
        L"開啟Nexon Game Manager",
        L"開啟 Nexon Game Manager",
        L"Open Nexon Game Manager",
        L"打开Nexon",
        L"打开 Nexon",
        L"開啟Nexon",
        L"開啟 Nexon",
        L"Open Nexon",
    };
    IUIAutomationElement* openBtn =
        FindFirstNamed(uia, root, kOpenLong, (int)(sizeof(kOpenLong) / sizeof(kOpenLong[0])), true);
    if (openBtn) {
        did = uia.InvokeOrClick(openBtn, false) || did;
        openBtn->Release();
    } else {
        // Edge 协议窗主按钮经常只有「打开 / 開啟 / Open」
        if (uia.ClickLargestExactName(root, L"打开", false) ||
            uia.ClickLargestExactName(root, L"開啟", false) ||
            uia.ClickLargestExactName(root, L"Open", false)) {
            did = true;
        }
    }
    root->Release();
    return did;
}

}  // namespace

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
