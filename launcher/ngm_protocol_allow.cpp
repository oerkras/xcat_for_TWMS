#include "ngm_protocol_allow.h"

#include "msc_launch.h"
#include "win_uia.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <UIAutomation.h>
#include <Shlwapi.h>

#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace msc::launcher {
namespace {

void LogLine(const NgmProtocolLogFn& log, const std::wstring& s) {
    if (log) log(s);
}

std::wstring BstrToW(BSTR b) {
    if (!b) return {};
    return std::wstring(b, SysStringLen(b));
}

std::wstring ElemName(IUIAutomationElement* el) {
    if (!el) return {};
    BSTR b = nullptr;
    if (FAILED(el->get_CurrentName(&b)) || !b) return {};
    std::wstring n = BstrToW(b);
    SysFreeString(b);
    return n;
}

long ElemType(IUIAutomationElement* el) {
    if (!el) return 0;
    CONTROLTYPEID t = 0;
    el->get_CurrentControlType(&t);
    return (long)t;
}

RECT ElemRect(IUIAutomationElement* el) {
    RECT r{};
    if (!el) return r;
    el->get_CurrentBoundingRectangle(&r);
    return r;
}

bool RectVisible(const RECT& r) {
    return (r.right - r.left) >= 4 && (r.bottom - r.top) >= 4;
}

long RectArea(const RECT& r) {
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return 0;
    return w * h;
}

bool RectsOverlap(const RECT& a, const RECT& b) {
    RECT t{};
    return IntersectRect(&t, &a, &b) != FALSE;
}

void UnionRectTo(RECT& dst, const RECT& src, bool& have) {
    if (!have) {
        dst = src;
        have = true;
        return;
    }
    if (src.left < dst.left) dst.left = src.left;
    if (src.top < dst.top) dst.top = src.top;
    if (src.right > dst.right) dst.right = src.right;
    if (src.bottom > dst.bottom) dst.bottom = src.bottom;
}

RECT ExpandDialog(RECT r) {
    r.left -= 48;
    r.right += 48;
    r.top -= 24;
    r.bottom += 220;
    return r;
}

// 协议气泡里的标题/勾选行；整页 Document 动辄上千宽，直接丢掉。
bool RectLooksLikeDialogChrome(const RECT& r) {
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w < 60 || h < 10) return false;
    if (w > 720 || h > 360) return false;
    return true;
}

bool RectLooksLikeCheckbox(const RECT& r) {
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w < 36 || h < 12) return false;
    if (h > 56 || w > 780) return false;
    return true;
}

bool RectLooksLikeDialogButton(const RECT& r) {
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w < 28 || h < 14) return false;
    if (h > 64 || w > 480) return false;
    if (w * h > 480 * 64) return false;
    return true;
}

bool HasI(const std::wstring& n, const wchar_t* part) {
    return !n.empty() && part && *part && StrStrIW(n.c_str(), part) != nullptr;
}

bool NameHasAlwaysAllow(const std::wstring& n) {
    return HasI(n, L"始终允许") || HasI(n, L"始終允許") || HasI(n, L"一律允許") ||
           HasI(n, L"Always allow");
}

// 必须是协议窗标题/勾选长句。单独「始终允许」或官网正文里的 Nexon Game Manager 都不算。
bool NameHasStrongProtocolIntent(const std::wstring& n) {
    if (n.empty()) return false;
    if (HasI(n, L"想打开此应用") || HasI(n, L"想開啟此應用") || HasI(n, L"想開啟這個應用程式") ||
        HasI(n, L"想要開啟這個應用程式") || HasI(n, L"想要開啟此應用"))
        return true;
    if (HasI(n, L"wants to open this application") || HasI(n, L"wants to open") ||
        HasI(n, L"trying to open"))
        return true;
    if (HasI(n, L"正在尝试打开") || HasI(n, L"正嘗試開啟") || HasI(n, L"正在嘗試開啟")) return true;
    if (HasI(n, L"打开此类链接") || HasI(n, L"開啟這類連結") || HasI(n, L"開啟這類鏈接") ||
        HasI(n, L"open these types of links"))
        return true;
    if (NameHasAlwaysAllow(n) &&
        (HasI(n, L"打开") || HasI(n, L"開啟") || HasI(n, L"开启") || HasI(n, L"open") ||
         HasI(n, L"链接") || HasI(n, L"連結") || HasI(n, L"links") || HasI(n, L"associated")))
        return true;
    return false;
}

bool NameLooksOpenNgm(const std::wstring& n) {
    if (n.empty()) return false;
    const bool open = HasI(n, L"打开") || HasI(n, L"開啟") || HasI(n, L"开启") || HasI(n, L"Open");
    const bool ngm = HasI(n, L"Nexon") || HasI(n, L"Game Manager") || HasI(n, L"NGM");
    return open && ngm;
}

bool NameIsShortOpen(const std::wstring& n) {
    return n == L"打开" || n == L"開啟" || n == L"开启" || n == L"Open";
}

bool NameLooksCancel(const std::wstring& n) {
    return n == L"取消" || n == L"Cancel" || n == L"关闭" || n == L"關閉" || n == L"Close";
}

std::wstring ClipName(const std::wstring& n) {
    if (n.size() <= 72) return n;
    return n.substr(0, 72) + L"…";
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
           wcscmp(low, L"chromium.exe") == 0 || wcscmp(low, L"chromeplus.exe") == 0;
}

bool WindowMatchesTarget(HWND hwnd, const NgmProtocolAllowOpts& opts) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!ProcessIsChromiumBrowser(pid)) return false;

    if (opts.hwnd) {
        DWORD want = 0;
        GetWindowThreadProcessId(opts.hwnd, &want);
        // 协议气泡可能是同进程另一顶层窗（含被 login 窗拥有的 #32770）。
        if (want && pid != want) return false;
    }

    const bool needCmd = opts.debugPort > 0 || (opts.cmdNeedle && opts.cmdNeedle[0]);
    if (!needCmd) return true;

    const std::wstring cmd = GetProcessCommandLineW(pid);
    if (opts.debugPort > 0) {
        wchar_t needle[80]{};
        swprintf_s(needle, L"--remote-debugging-port=%d", opts.debugPort);
        if (cmd.find(needle) != std::wstring::npos) return true;
    }
    if (opts.cmdNeedle && opts.cmdNeedle[0] && cmd.find(opts.cmdNeedle) != std::wstring::npos) {
        return true;
    }
    return false;
}

BOOL CALLBACK CollectBrowserHwnds(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    auto* ctx = reinterpret_cast<std::pair<std::vector<HWND>*, const NgmProtocolAllowOpts*>*>(lp);
    if (!ctx || !ctx->first || !ctx->second) return TRUE;
    if (!WindowMatchesTarget(hwnd, *ctx->second)) return TRUE;
    if (ctx->first->size() < 16) ctx->first->push_back(hwnd);
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

bool HasTogglePattern(IUIAutomationElement* el) {
    if (!el) return false;
    IUIAutomationTogglePattern* tog = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_IUIAutomationTogglePattern,
                                       reinterpret_cast<void**>(&tog))) ||
        !tog) {
        return false;
    }
    tog->Release();
    return true;
}

int OpenTypeTier(long t) {
    if (t == UIA_ButtonControlTypeId) return 3;
    if (t == UIA_HyperlinkControlTypeId) return 2;
    if (t == UIA_CustomControlTypeId) return 1;
    return 0;
}

bool BetterAlways(IUIAutomationElement* cand, IUIAutomationElement* best) {
    if (!best) return true;
    const long ct = ElemType(cand);
    const long bt = ElemType(best);
    const int cTier = (ct == UIA_CheckBoxControlTypeId ? 3 : 0) + (HasTogglePattern(cand) ? 2 : 0);
    const int bTier = (bt == UIA_CheckBoxControlTypeId ? 3 : 0) + (HasTogglePattern(best) ? 2 : 0);
    if (cTier != bTier) return cTier > bTier;
    return RectArea(ElemRect(cand)) < RectArea(ElemRect(best));
}

bool BetterOpen(IUIAutomationElement* cand, IUIAutomationElement* best, bool candLong, bool bestLong) {
    if (!best) return true;
    if (candLong != bestLong) return candLong;
    const int cTier = OpenTypeTier(ElemType(cand));
    const int bTier = OpenTypeTier(ElemType(best));
    if (cTier != bTier) return cTier > bTier;
    return RectArea(ElemRect(cand)) < RectArea(ElemRect(best));  // 协议钮小；网页 CTA 大
}

IUIAutomationElementArray* AllDescendants(IUIAutomation* uia, IUIAutomationElement* root) {
    if (!uia || !root) return nullptr;
    IUIAutomationCondition* trueCond = nullptr;
    if (FAILED(uia->CreateTrueCondition(&trueCond)) || !trueCond) return nullptr;
    IUIAutomationElementArray* arr = nullptr;
    const HRESULT hr = root->FindAll(TreeScope_Descendants, trueCond, &arr);
    trueCond->Release();
    if (FAILED(hr) || !arr) return nullptr;
    return arr;
}

struct VisitState {
    HWND hwnd = nullptr;
    bool alwaysDone = false;
    DWORD lastOpenAt = 0;
};

VisitState& Visit() {
    static VisitState s;
    return s;
}

bool AcceptInBrowserWindow(msc::uia::Session& uia, HWND hwnd, const NgmProtocolLogFn& log) {
    IUIAutomationElement* root = uia.ElementFromHwnd(hwnd);
    if (!root) return false;
    IUIAutomation* raw = uia.Raw();
    IUIAutomationElementArray* arr = AllDescendants(raw, root);
    if (!arr) {
        root->Release();
        return false;
    }

    int len = 0;
    arr->get_Length(&len);
    RECT dialog{};
    bool haveDialog = false;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        const RECT r = ElemRect(el);
        if (NameHasStrongProtocolIntent(n) && RectVisible(r) && RectLooksLikeDialogChrome(r)) {
            UnionRectTo(dialog, r, haveDialog);
        }
        el->Release();
    }
    if (!haveDialog) {
        if (Visit().hwnd == hwnd) Visit() = {};
        arr->Release();
        root->Release();
        return false;
    }
    dialog = ExpandDialog(dialog);

    IUIAutomationElement* always = nullptr;
    IUIAutomationElement* openBtn = nullptr;
    bool openLong = false;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        const RECT r = ElemRect(el);
        const long t = ElemType(el);
        if (!RectVisible(r) || !RectsOverlap(r, dialog) || NameLooksCancel(n)) {
            el->Release();
            continue;
        }
        if (NameHasAlwaysAllow(n) && RectLooksLikeCheckbox(r) &&
            (t == UIA_CheckBoxControlTypeId || t == UIA_CustomControlTypeId ||
             t == UIA_ButtonControlTypeId || HasTogglePattern(el))) {
            if (BetterAlways(el, always)) {
                if (always) always->Release();
                always = el;
                continue;
            }
        }
        const bool lng = NameLooksOpenNgm(n);
        const bool sh = NameIsShortOpen(n);
        if ((lng || sh) && RectLooksLikeDialogButton(r) && OpenTypeTier(t) > 0) {
            if (BetterOpen(el, openBtn, lng, openLong)) {
                if (openBtn) openBtn->Release();
                openBtn = el;
                openLong = lng;
                continue;
            }
        }
        el->Release();
    }
    arr->Release();
    root->Release();

    auto& vis = Visit();
    if (vis.hwnd != hwnd) {
        vis = {};
        vis.hwnd = hwnd;
    }

    bool did = false;
    if (always && !vis.alwaysDone) {
        const std::wstring n = ClipName(ElemName(always));
        bool ok = ToggleOnIfOff(always);
        if (!ok) {
            // 没有 TogglePattern 时只点一次，禁止每秒 Invoke 把勾选来回拨。
            ok = uia.ClickPointAt(always, 0.08f, 0.5f, false) || uia.ClickPoint(always);
        }
        vis.alwaysDone = true;
        did = ok || did;
        LogLine(log, std::wstring(L"[ngm-protocol] 勾选始终允许 ok=") + (ok ? L"1" : L"0") +
                         L" name=" + n);
    } else if (always) {
        // 已勾过：若被关掉则再拨开，但绝不再 Invoke。
        ToggleOnIfOff(always);
    }

    const DWORD now = GetTickCount();
    if (openBtn && (vis.lastOpenAt == 0 || now - vis.lastOpenAt >= 2500u)) {
        const std::wstring n = ClipName(ElemName(openBtn));
        const RECT r = ElemRect(openBtn);
        const bool ok = uia.Invoke(openBtn) || uia.ClickPoint(openBtn);
        vis.lastOpenAt = now;
        did = ok || did;
        LogLine(log, std::wstring(L"[ngm-protocol] 点打开 ok=") + (ok ? L"1" : L"0") + L" name=" +
                         n + L" " + std::to_wstring(r.right - r.left) + L"x" +
                         std::to_wstring(r.bottom - r.top));
    }

    if (always) always->Release();
    if (openBtn) openBtn->Release();
    return did;
}

}  // namespace

bool TryAcceptNgmProtocolDialog(const NgmProtocolLogFn& log, const NgmProtocolAllowOpts& opts) {
    static DWORD sLastTry = 0;
    const DWORD now = GetTickCount();
    if (sLastTry != 0 && now - sLastTry < 800u) return false;
    sLastTry = now;

    std::vector<HWND> hwnds;
    std::pair<std::vector<HWND>*, const NgmProtocolAllowOpts*> ctx{&hwnds, &opts};
    EnumWindows(CollectBrowserHwnds, reinterpret_cast<LPARAM>(&ctx));
    if (opts.hwnd && IsWindow(opts.hwnd) && IsWindowVisible(opts.hwnd)) {
        bool have = false;
        for (HWND h : hwnds) {
            if (h == opts.hwnd) {
                have = true;
                break;
            }
        }
        if (!have && WindowMatchesTarget(opts.hwnd, opts)) hwnds.insert(hwnds.begin(), opts.hwnd);
    }
    if (hwnds.empty()) return false;

    msc::uia::Session uia;
    if (!uia.Init(nullptr) || !uia.Raw()) return false;

    bool did = false;
    for (HWND hwnd : hwnds) {
        if (AcceptInBrowserWindow(uia, hwnd, log)) {
            did = true;
            break;
        }
    }
    return did;
}

}  // namespace msc::launcher
