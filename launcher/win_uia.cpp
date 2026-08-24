#include "win_uia.h"

#include <Shlwapi.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#pragma comment(lib, "UIAutomationCore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

namespace msc::uia {
namespace {

void LogLine(const LogFn& log, const std::wstring& s) {
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

bool ElemEnabled(IUIAutomationElement* el) {
    if (!el) return false;
    BOOL en = FALSE;
    if (FAILED(el->get_CurrentIsEnabled(&en))) return true;  // 读失败当可用，避免全跳过
    return en != FALSE;
}

// CTA（繼續 / Gama Pass）合理命中框；过大多半是同名外层容器
bool RectLooksLikeCta(const RECT& r) {
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w < 24 || h < 12) return false;
    if (h > 160 || w > 900) return false;
    if (w * h > 900 * 140) return false;
    return true;
}

std::wstring FlattenWs(std::wstring s) {
    std::wstring o;
    o.reserve(s.size());
    bool sp = false;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') {
            if (!o.empty()) sp = true;
            continue;
        }
        if (sp) {
            o.push_back(L' ');
            sp = false;
        }
        o.push_back(c);
    }
    return o;
}

std::wstring ElemValue(IUIAutomationElement* el) {
    if (!el) return {};
    IUIAutomationValuePattern* vp = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                       (void**)&vp)) ||
        !vp)
        return {};
    BSTR b = nullptr;
    std::wstring s;
    if (SUCCEEDED(vp->get_CurrentValue(&b)) && b) {
        s = BstrToW(b);
        SysFreeString(b);
    }
    vp->Release();
    return s;
}

std::wstring ElemLegacyName(IUIAutomationElement* el) {
    if (!el) return {};
    IUIAutomationLegacyIAccessiblePattern* p = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                       IID_IUIAutomationLegacyIAccessiblePattern, (void**)&p)) ||
        !p)
        return {};
    BSTR b = nullptr;
    std::wstring s;
    if (SUCCEEDED(p->get_CurrentName(&b)) && b) {
        s = BstrToW(b);
        SysFreeString(b);
    }
    p->Release();
    return s;
}

std::wstring ElemAnyName(IUIAutomationElement* el) {
    std::wstring n = FlattenWs(ElemName(el));
    if (!n.empty()) return n;
    n = FlattenWs(ElemLegacyName(el));
    if (!n.empty()) return n;
    return FlattenWs(ElemValue(el));
}

bool NameLooksLikeGpCta(const std::wstring& n) {
    if (n.empty()) return false;
    return StrStrIW(n.c_str(), L"Gama") || StrStrIW(n.c_str(), L"GAMAPASS") ||
           StrStrIW(n.c_str(), L"Sign in with") || StrStrIW(n.c_str(), L"使用 Gama") ||
           (StrStrIW(n.c_str(), L"登入") && n.size() < 40) ||
           (StrStrIW(n.c_str(), L"登录") && n.size() < 40);
}

bool NameLooksLikeBrowserChrome(const std::wstring& n) {
    if (n.empty()) return false;
    const wchar_t* junk[] = {
        L"Google", L"Chrome", L"關閉", L"关闭", L"Close", L"最小化", L"最大化", L"還原", L"还原",
        L"新分頁", L"新标签", L"New tab", L"設定", L"设置", L"Settings", L"擴充", L"扩展",
        L"書籤", L"书签", L"網址", L"位址", L"地址", L"搜尋", L"搜索", L"Address", L"Omnibox",
        L"重新載入", L"重新加载", L"Reload", L"上一頁", L"下一頁", L"Back", L"Forward",
    };
    for (const wchar_t* j : junk) {
        if (StrStrIW(n.c_str(), j)) return true;
    }
    return false;
}

bool RectInContentArea(const RECT& r, const RECT& wr) {
    if (!RectVisible(r) || !RectVisible(wr)) return false;
    const long topBar = wr.top + 120;
    const long bottomBar = wr.bottom - 36;
    return r.top >= topBar && r.bottom <= bottomBar && r.left >= wr.left + 8 &&
           r.right <= wr.right - 8;
}

bool RectLooksLikeLooseCta(const RECT& r) {
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w < 36 || h < 14) return false;
    if (h > 220 || w > 1400) return false;
    return true;
}

bool NameMatch(const std::wstring& name, const wchar_t* part, bool substring) {
    if (!part || !*part) return false;
    const std::wstring a = FlattenWs(name);
    const std::wstring b = FlattenWs(part);
    if (a.empty() || b.empty()) return false;
    if (!substring) return _wcsicmp(a.c_str(), b.c_str()) == 0;
    return StrStrIW(a.c_str(), b.c_str()) != nullptr;
}

bool NameMatchAny(const std::wstring& name, const std::vector<std::wstring>& parts, bool substring) {
    for (const auto& p : parts) {
        if (NameMatch(name, p.c_str(), substring)) return true;
    }
    return false;
}

bool IsInvokePreferType(long t) {
    return t == UIA_ButtonControlTypeId || t == UIA_HyperlinkControlTypeId ||
           t == UIA_ListItemControlTypeId || t == UIA_RadioButtonControlTypeId ||
           t == UIA_CheckBoxControlTypeId || t == UIA_MenuItemControlTypeId ||
           t == UIA_TabItemControlTypeId || t == UIA_DataItemControlTypeId;
}

int CtaTypeTier(long t) {
    if (t == UIA_ButtonControlTypeId || t == UIA_HyperlinkControlTypeId ||
        t == UIA_MenuItemControlTypeId)
        return 3;
    if (IsInvokePreferType(t)) return 2;
    if (t == UIA_CustomControlTypeId || t == UIA_GroupControlTypeId) return 1;
    return 0;  // Text 等叶子
}

int CountAtSigns(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s)
        if (c == L'@') ++n;
    return n;
}

std::wstring MailKeyFromName(const std::wstring& s) {
    const size_t at = s.find(L'@');
    if (at == std::wstring::npos || at == 0) return {};
    size_t b = at;
    while (b > 0) {
        const wchar_t c = s[b - 1];
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
            c == L'.' || c == L'_' || c == L'%' || c == L'+' || c == L'-')
            --b;
        else
            break;
    }
    size_t e = at + 1;
    while (e < s.size()) {
        const wchar_t c = s[e];
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
            c == L'.' || c == L'-')
            ++e;
        else
            break;
    }
    if (e <= at + 1) return {};
    std::wstring key = s.substr(b, e - b);
    for (auto& c : key) c = (wchar_t)towlower(c);
    return key;
}

bool BadAccountCardName(const std::wstring& t) {
    if (t.empty()) return true;
    if (StrStrIW(t.c_str(), L"使用其他") || StrStrIW(t.c_str(), L"其他帳") ||
        StrStrIW(t.c_str(), L"其它帳") || StrStrIW(t.c_str(), L"other account") ||
        StrStrIW(t.c_str(), L"建立") || StrStrIW(t.c_str(), L"Create") ||
        StrStrIW(t.c_str(), L"使用 Gama Pass") || StrStrIW(t.c_str(), L"使用 Gama"))
        return true;
    return false;
}

// Chrome 整窗无障碍树含标签栏/扩展/设置，FindAll(True) 一次可数秒。
// 点选与文案探测只进当前页 Document；地址栏仍走窗口根。
bool DocLooksLikeLoginHost(const std::wstring& name) {
    if (name.empty()) return false;
    return StrStrIW(name.c_str(), L"galaxy") || StrStrIW(name.c_str(), L"beanfun") ||
           StrStrIW(name.c_str(), L"gamania") || StrStrIW(name.c_str(), L"accounts") ||
           StrStrIW(name.c_str(), L"maplestoryclassic") || StrStrIW(name.c_str(), L"maplestory") ||
           StrStrIW(name.c_str(), L"gamapass") || StrStrIW(name.c_str(), L"gama pass") ||
           StrStrIW(name.c_str(), L"openid") || StrStrIW(name.c_str(), L"楓之谷") ||
           StrStrIW(name.c_str(), L"枫之谷");
}

// 不要 FindFirst：Chrome++ 扩展/iframe 的 Document 常排在登录页前面。
// 优先 Name/Value 带 galaxy/beanfun/gamania 的可见块，否则取面积最大的可见 Document。
IUIAutomationElement* FindPageDocument(IUIAutomation* uia, IUIAutomationElement* root) {
    if (!uia || !root) return nullptr;
    VARIANT v{};
    v.vt = VT_I4;
    v.lVal = UIA_DocumentControlTypeId;
    IUIAutomationCondition* cond = nullptr;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
        return nullptr;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    if (FAILED(hr) || !arr) return nullptr;

    int len = 0;
    arr->get_Length(&len);
    IUIAutomationElement* bestHost = nullptr;
    IUIAutomationElement* bestArea = nullptr;
    long bestHostArea = -1;
    long bestVisArea = -1;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const RECT r = ElemRect(el);
        if (!RectVisible(r)) {
            el->Release();
            continue;
        }
        const long area = (r.right - r.left) * (r.bottom - r.top);
        const std::wstring n = ElemName(el);
        std::wstring val;
        IUIAutomationValuePattern* vp = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                              (void**)&vp)) &&
            vp) {
            BSTR b = nullptr;
            if (SUCCEEDED(vp->get_CurrentValue(&b)) && b) {
                val.assign(b, SysStringLen(b));
                SysFreeString(b);
            }
            vp->Release();
        }
        if ((DocLooksLikeLoginHost(n) || DocLooksLikeLoginHost(val)) && area > bestHostArea) {
            if (bestHost) bestHost->Release();
            bestHost = el;
            bestHost->AddRef();
            bestHostArea = area;
        }
        if (area > bestVisArea) {
            if (bestArea) bestArea->Release();
            bestArea = el;
            bestArea->AddRef();
            bestVisArea = area;
        }
        el->Release();
    }
    arr->Release();
    if (bestHost) {
        if (bestArea) bestArea->Release();
        return bestHost;
    }
    return bestArea;
}

constexpr int kNameWalkBudget = 1600;

IUIAutomationElement* WalkFindName(IUIAutomation* uia, IUIAutomationElement* scope,
                                   const wchar_t* namePart, bool substring, bool preferInvokeable) {
    if (!uia || !scope || !namePart || !*namePart) return nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->get_ControlViewWalker(&walker)) || !walker) return nullptr;

    std::vector<IUIAutomationElement*> st;
    IUIAutomationElement* first = nullptr;
    if (SUCCEEDED(walker->GetFirstChildElement(scope, &first)) && first) st.push_back(first);

    IUIAutomationElement* fallback = nullptr;
    int n = 0;
    while (!st.empty() && n < kNameWalkBudget) {
        IUIAutomationElement* cur = st.back();
        st.pop_back();
        ++n;

        const std::wstring name = ElemName(cur);
        const RECT r = ElemRect(cur);
        const bool hit = NameMatch(name, namePart, substring) && RectVisible(r);
        if (hit && (!preferInvokeable || IsInvokePreferType(ElemType(cur)))) {
            for (auto* x : st) x->Release();
            if (fallback) fallback->Release();
            walker->Release();
            return cur;
        }
        if (hit && !fallback) {
            fallback = cur;
            fallback->AddRef();
        }

        IUIAutomationElement* ch = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(cur, &ch)) && ch) {
            while (ch) {
                IUIAutomationElement* next = nullptr;
                (void)walker->GetNextSiblingElement(ch, &next);
                st.push_back(ch);
                ch = next;
            }
        }
        cur->Release();
    }
    for (auto* x : st) x->Release();
    walker->Release();
    return fallback;
}

bool LooksLikeAccountSeedName(const std::wstring& t) {
    if (t.size() < 3 || t.size() > 160) return false;
    if (BadAccountCardName(t)) return false;
    if (t.find(L'@') != std::wstring::npos) return true;
    if (StrStrIW(t.c_str(), L"default-user-avatar")) return true;
    if (StrStrIW(t.c_str(), L"outlook.") || StrStrIW(t.c_str(), L"gmail.") ||
        StrStrIW(t.c_str(), L"hotmail.") || StrStrIW(t.c_str(), L"yahoo.") ||
        StrStrIW(t.c_str(), L"icloud."))
        return true;
    return false;
}

// 对齐 CDP resolveCard + closest：在「恰好 1 个 @」的祖先里取最紧单卡
// BIN 05:37：单账号时一路爬到列表壳，中心点空点；须限高/取最小面积
IUIAutomationElement* ResolveAccountCard(IUIAutomation* uia, IUIAutomationElement* el) {
    if (!uia || !el) return nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->get_ControlViewWalker(&walker)) || !walker) {
        el->AddRef();
        return el;
    }

    const RECT leaf = ElemRect(el);
    IUIAutomationElement* best = el;
    best->AddRef();
    long bestArea = LONG_MAX;
    {
        const RECT br = ElemRect(best);
        bestArea = (std::max)(1L, (br.right - br.left) * (br.bottom - br.top));
    }

    IUIAutomationElement* cur = el;
    cur->AddRef();
    for (int depth = 0; depth < 10; ++depth) {
        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
        cur->Release();
        cur = parent;
        const std::wstring pn = ElemName(cur);
        if (BadAccountCardName(pn) || pn.size() > 160) break;
        if (CountAtSigns(pn) >= 2) break;  // 多账号列表壳

        const RECT r = ElemRect(cur);
        if (!RectVisible(r)) continue;
        const long w = r.right - r.left;
        const long h = r.bottom - r.top;
        const long area = (std::max)(1L, w) * (std::max)(1L, h);
        // 单卡合理尺寸；过大必是列表/页壳（单账号时 Name 仍可能只有 1 个 @）
        if (h > 200 || w > 720 || area > 720 * 200) break;

        const long t = ElemType(cur);
        if (CountAtSigns(pn) >= 1) {
            // 取面积更小的单邮箱祖先（紧卡）；Button/ListItem 同面积优先
            const bool betterSize = area < bestArea;
            const bool betterType =
                area <= bestArea * 12 / 10 &&
                (t == UIA_ButtonControlTypeId || t == UIA_HyperlinkControlTypeId ||
                 t == UIA_ListItemControlTypeId);
            if (betterSize || betterType) {
                if (best) best->Release();
                best = cur;
                best->AddRef();
                bestArea = area;
            }
            if (t == UIA_ButtonControlTypeId || t == UIA_HyperlinkControlTypeId ||
                t == UIA_ListItemControlTypeId) {
                // 已是可点类型且尺寸合理，可停
                if (h <= 160) break;
            }
            continue;
        }
        // 父无 @：再试一层看是否 Button 包住叶子（无邮箱名的外壳）
        if (t == UIA_ButtonControlTypeId || t == UIA_HyperlinkControlTypeId ||
            t == UIA_ListItemControlTypeId) {
            const bool covers =
                r.left <= leaf.left + 4 && r.top <= leaf.top + 4 && r.right >= leaf.right - 4 &&
                r.bottom >= leaf.bottom - 4;
            if (covers && area < bestArea * 3 && h <= 200) {
                if (best) best->Release();
                best = cur;
                best->AddRef();
            }
        }
        break;
    }
    cur->Release();
    walker->Release();
    return best;
}

// Chromium 把「带 click 的 div」暴露成 Custom/Group，无 InvokePattern；
// 但 Blink 仍给可点节点留了 LegacyIAccessible 默认动作（DoDefaultAction ≈ 派发 click，
// React 根委派照样收得到），这是账号卡唯一的非鼠标直调路径。
bool TryLegacyDefaultAction(IUIAutomationElement* el) {
    if (!el) return false;
    IUIAutomationLegacyIAccessiblePattern* leg = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                       IID_IUIAutomationLegacyIAccessiblePattern, (void**)&leg)) ||
        !leg)
        return false;
    // 无默认动作的节点 DoDefaultAction 会「成功」但什么都不做 → 先查动作名
    BSTR act = nullptr;
    bool hasAction = false;
    if (SUCCEEDED(leg->get_CurrentDefaultAction(&act)) && act) {
        hasAction = SysStringLen(act) > 0;
        SysFreeString(act);
    }
    bool ok = false;
    if (hasAction) ok = SUCCEEDED(leg->DoDefaultAction());
    leg->Release();
    return ok;
}

bool HasInvokePattern(IUIAutomationElement* el) {
    if (!el) return false;
    IUIAutomationInvokePattern* inv = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern,
                                       (void**)&inv)) ||
        !inv)
        return false;
    inv->Release();
    return true;
}

// 键盘直调：卡可获焦时 SetFocus + Enter（role=button/tabindex 的卡吃这套），仍不动鼠标
bool TryFocusEnter(IUIAutomationElement* el) {
    if (!el) return false;
    BOOL focusable = FALSE;
    if (FAILED(el->get_CurrentIsKeyboardFocusable(&focusable)) || !focusable) return false;
    if (FAILED(el->SetFocus())) return false;
    Sleep(30);
    INPUT k[2]{};
    k[0].type = INPUT_KEYBOARD;
    k[0].ki.wVk = VK_RETURN;
    k[1] = k[0];
    k[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, k, sizeof(INPUT)) == 2;
}

// 卡内找「真能直调」的后代（Invoke 或有默认动作），限定在卡矩形内、取最贴合的一个
IUIAutomationElement* FindDirectActionDescendant(IUIAutomation* uia, IUIAutomationElement* card) {
    if (!uia || !card) return nullptr;
    const RECT cr = ElemRect(card);
    const long cardArea = (std::max)(1L, (cr.right - cr.left) * (cr.bottom - cr.top));

    IUIAutomationCondition* trueCond = nullptr;
    if (FAILED(uia->CreateTrueCondition(&trueCond)) || !trueCond) return nullptr;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = card->FindAll(TreeScope_Descendants, trueCond, &arr);
    trueCond->Release();
    if (FAILED(hr) || !arr) return nullptr;

    IUIAutomationElement* best = nullptr;
    long bestArea = 0;
    int len = 0;
    arr->get_Length(&len);
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const RECT r = ElemRect(el);
        const long area = (std::max)(1L, (r.right - r.left) * (r.bottom - r.top));
        const bool inside = r.left >= cr.left - 2 && r.top >= cr.top - 2 &&
                            r.right <= cr.right + 2 && r.bottom <= cr.bottom + 2;
        // 太小的图标/文字叶子点了也不跳；取覆盖卡面 ≥ 1/4 的那层
        if (!RectVisible(r) || !inside || area * 4 < cardArea) {
            el->Release();
            continue;
        }
        if (!HasInvokePattern(el)) {
            el->Release();
            continue;
        }
        if (area > bestArea) {
            if (best) best->Release();
            best = el;
            bestArea = area;
            continue;
        }
        el->Release();
    }
    arr->Release();
    return best;
}

// 真实鼠标落点命中的是哪个元素？与我们挑的卡比对，判断「是不是选错元素」
// 返回形如 same / Custom:50025#320x64（不同时给出命中者类型+尺寸+有无 Invoke）
std::wstring DescribeHitAt(IUIAutomation* uia, IUIAutomationElement* card, float xFrac) {
    if (!uia || !card) return L"?";
    const RECT r = ElemRect(card);
    if (!RectVisible(r)) return L"?";
    POINT pt{};
    pt.x = r.left + (LONG)((r.right - r.left) * xFrac);
    pt.y = r.top + (r.bottom - r.top) / 2;

    IUIAutomationElement* hit = nullptr;
    if (FAILED(uia->ElementFromPoint(pt, &hit)) || !hit) return L"none";
    BOOL same = FALSE;
    uia->CompareElements(card, hit, &same);
    std::wstring out;
    if (same) {
        out = L"same";
    } else {
        const RECT hr = ElemRect(hit);
        out = L"t" + std::to_wstring(ElemType(hit)) + L"#" +
              std::to_wstring(hr.right - hr.left) + L"x" + std::to_wstring(hr.bottom - hr.top) +
              (HasInvokePattern(hit) ? L"+inv" : L"-inv");
        const std::wstring hn = ElemName(hit);
        if (!hn.empty()) out += L":" + hn.substr(0, 24);
    }
    hit->Release();
    return out;
}

// BIN 06:47 实锤：卡是 Button(t50000) 且有 InvokePattern，Invoke 返回成功却不跳转
// （Blink 模拟 click 触发不了处理器）；同轮几何点击一次即进昵称页。
// 故几何优先，直调（Invoke / Legacy / 卡内后代 / 焦点+Enter）仅作几何失败兜底。
// outHow / DescribeHitAt 用于 BIN 核对「落点下是不是同一元素」。
bool ActivateAccountCard(const Session* self, IUIAutomationElement* card, int clickVariant,
                         IUIAutomation* uia, std::wstring* outHow) {
    if (!self || !card) return false;
    auto mark = [&](const std::wstring& how) {
        if (outHow) *outHow = how;
        return true;
    };

    const float xfs[] = {0.50f, 0.22f, 0.72f};
    const float xf = xfs[(clickVariant >= 0 ? clickVariant : 0) % 3];
    const std::wstring hit = DescribeHitAt(uia, card, xf);
    if (self->ClickPointAt(card, xf, 0.5f, /*useOfficialCp=*/false)) return mark(L"pt|hit=" + hit);

    if (self->Invoke(card)) return mark(L"inv");
    if (TryLegacyDefaultAction(card)) return mark(L"legacy");
    if (IUIAutomationElement* kid = FindDirectActionDescendant(uia, card)) {
        const bool ok = self->Invoke(kid);
        kid->Release();
        if (ok) return mark(L"kid-inv");
    }
    if (TryFocusEnter(card)) return mark(L"focus-enter");
    if (outHow) *outHow = L"fail|hit=" + hit;
    return false;
}

// 邮箱/昵称常是 Text 叶子；点叶子 Invoke「成功」但不跳转。上溯到「紧贴」可点父（调用方 Release）
// ★ 禁止爬到整页外框 Custom/Group（BIN 02:33：账号卡点成选中外框，需手点才进昵称页）
IUIAutomationElement* PreferClickableAncestor(IUIAutomation* uia, IUIAutomationElement* el) {
    if (!uia || !el) return nullptr;
    if (IsInvokePreferType(ElemType(el))) {
        el->AddRef();
        return el;
    }
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->get_ControlViewWalker(&walker)) || !walker) {
        el->AddRef();
        return el;
    }

    const RECT leaf = ElemRect(el);
    const long leafW = (std::max)(1L, leaf.right - leaf.left);
    const long leafH = (std::max)(1L, leaf.bottom - leaf.top);
    const long leafArea = leafW * leafH;

    IUIAutomationElement* bestInvoke = nullptr;
    long bestInvokeArea = LONG_MAX;
    IUIAutomationElement* bestTight = nullptr;
    long bestTightArea = LONG_MAX;

    IUIAutomationElement* cur = el;
    cur->AddRef();
    for (int depth = 0; depth < 10; ++depth) {
        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
        cur->Release();
        cur = parent;
        const long t = ElemType(cur);
        const RECT r = ElemRect(cur);
        if (!RectVisible(r)) continue;
        const bool covers =
            r.left <= leaf.left + 2 && r.top <= leaf.top + 2 && r.right >= leaf.right - 2 &&
            r.bottom >= leaf.bottom - 2;
        if (!covers) continue;

        const long w = r.right - r.left;
        const long h = r.bottom - r.top;
        const long area = (std::max)(1L, w) * (std::max)(1L, h);
        // 外框特征：面积远大于叶子 / 高度像整页容器 → 跳过
        if (area > leafArea * 20 || h > (std::max)(leafH * 6, 140L) || w > leafW * 8) continue;

        if (IsInvokePreferType(t)) {
            if (area < bestInvokeArea) {
                if (bestInvoke) bestInvoke->Release();
                bestInvoke = cur;
                bestInvoke->AddRef();
                bestInvokeArea = area;
            }
            continue;
        }
        if (t == UIA_CustomControlTypeId || t == UIA_GroupControlTypeId) {
            if (area < bestTightArea) {
                if (bestTight) bestTight->Release();
                bestTight = cur;
                bestTight->AddRef();
                bestTightArea = area;
            }
        }
    }
    cur->Release();
    walker->Release();

    if (bestInvoke) {
        if (bestTight) bestTight->Release();
        return bestInvoke;
    }
    if (bestTight) return bestTight;
    el->AddRef();
    return el;
}

struct RawHit {
    IUIAutomationElement* el = nullptr;
    ElementHit info;
};

void SortHits(std::vector<RawHit>& hits) {
    std::sort(hits.begin(), hits.end(), [](const RawHit& a, const RawHit& b) {
        if (a.info.rect.top != b.info.rect.top) return a.info.rect.top < b.info.rect.top;
        return a.info.rect.left < b.info.rect.left;
    });
}

}  // namespace

Session::Session() = default;

Session::~Session() { Close(); }

bool Session::Init(const LogFn& log) {
    log_ = log;
    Close();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        comInited_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        comInited_ = false;  // already init other mode; still usable
    } else {
        LogLine(log_, L"[uia] CoInitializeEx 失败 hr=" + std::to_wstring((long)hr));
        return false;
    }
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                          (void**)&uia_);
    if (FAILED(hr) || !uia_) {
        LogLine(log_, L"[uia] CoCreateInstance CUIAutomation 失败 hr=" + std::to_wstring((long)hr));
        Close();
        return false;
    }
    return true;
}

void Session::Close() {
    if (uia_) {
        uia_->Release();
        uia_ = nullptr;
    }
    if (comInited_) {
        CoUninitialize();
        comInited_ = false;
    }
}

IUIAutomationElement* Session::ElementFromHwnd(HWND hwnd) const {
    if (!uia_ || !hwnd) return nullptr;
    IUIAutomationElement* el = nullptr;
    if (FAILED(uia_->ElementFromHandle(hwnd, &el))) return nullptr;
    return el;
}

IUIAutomationElement* Session::PageDocument(IUIAutomationElement* root) const {
    if (!uia_ || !root) return nullptr;
    return FindPageDocument(uia_, root);
}

IUIAutomationElement* Session::FindByName(IUIAutomationElement* root, const wchar_t* namePart,
                                          bool substring, bool preferInvokeable) const {
    if (!uia_ || !root || !namePart || !*namePart) return nullptr;

    // 与 161 相同：整窗 FindAll。C2799 曾改成只搜 FindPageDocument，
    // 官方 Chrome 会搜错 Document，点选全部落空（4E82 164/165）。
    IUIAutomationCondition* cond = nullptr;
    HRESULT hr;
    if (substring) {
        hr = uia_->CreateTrueCondition(&cond);
    } else {
        VARIANT v{};
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(namePart);
        hr = uia_->CreatePropertyCondition(UIA_NamePropertyId, v, &cond);
        VariantClear(&v);
    }
    if (FAILED(hr) || !cond) return nullptr;

    IUIAutomationElementArray* arr = nullptr;
    hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    if (FAILED(hr) || !arr) return nullptr;

    int len = 0;
    arr->get_Length(&len);
    IUIAutomationElement* best = nullptr;
    IUIAutomationElement* fallback = nullptr;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        if (!NameMatch(n, namePart, substring)) {
            el->Release();
            continue;
        }
        const RECT r = ElemRect(el);
        if (!RectVisible(r)) {
            el->Release();
            continue;
        }
        if (preferInvokeable && IsInvokePreferType(ElemType(el))) {
            if (best) best->Release();
            best = el;
            break;
        }
        if (!fallback) fallback = el;
        else el->Release();
    }
    arr->Release();
    if (best) {
        if (fallback) fallback->Release();
        return best;
    }
    return fallback;
}

std::vector<ElementHit> Session::CollectNamedHits(IUIAutomationElement* root,
                                                  const std::vector<std::wstring>& nameParts,
                                                  bool substring) const {
    std::vector<ElementHit> out;
    if (!uia_ || !root || nameParts.empty()) return out;

    IUIAutomationCondition* trueCond = nullptr;
    if (FAILED(uia_->CreateTrueCondition(&trueCond)) || !trueCond) return out;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, trueCond, &arr);
    trueCond->Release();
    if (FAILED(hr) || !arr) return out;

    int len = 0;
    arr->get_Length(&len);
    std::vector<RawHit> hits;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        if (!NameMatchAny(n, nameParts, substring)) {
            el->Release();
            continue;
        }
        const RECT r = ElemRect(el);
        if (!RectVisible(r)) {
            el->Release();
            continue;
        }
        if (!IsInvokePreferType(ElemType(el)) && ElemType(el) != UIA_TextControlTypeId &&
            ElemType(el) != UIA_GroupControlTypeId && ElemType(el) != UIA_CustomControlTypeId) {
            el->Release();
            continue;
        }
        RawHit h;
        h.el = el;
        h.info.rect = r;
        h.info.name = n;
        h.info.controlType = ElemType(el);
        hits.push_back(std::move(h));
    }
    arr->Release();
    SortHits(hits);
    // Dedup roughly same rect
    for (auto& h : hits) {
        bool dup = false;
        for (const auto& e : out) {
            if (std::abs(e.rect.top - h.info.rect.top) < 8 &&
                std::abs(e.rect.left - h.info.rect.left) < 8) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(h.info);
        h.el->Release();
    }
    return out;
}

std::vector<ElementHit> Session::CollectByType(IUIAutomationElement* root,
                                               long controlTypeId) const {
    std::vector<ElementHit> out;
    if (!uia_ || !root) return out;
    VARIANT v{};
    v.vt = VT_I4;
    v.lVal = controlTypeId;
    IUIAutomationCondition* cond = nullptr;
    if (FAILED(uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
        return out;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    if (FAILED(hr) || !arr) return out;
    int len = 0;
    arr->get_Length(&len);
    std::vector<RawHit> hits;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const RECT r = ElemRect(el);
        if (!RectVisible(r)) {
            el->Release();
            continue;
        }
        RawHit h;
        h.el = el;
        h.info.rect = r;
        h.info.name = ElemName(el);
        h.info.controlType = controlTypeId;
        hits.push_back(std::move(h));
    }
    arr->Release();
    SortHits(hits);
    for (auto& h : hits) {
        out.push_back(h.info);
        h.el->Release();
    }
    return out;
}

bool Session::Invoke(IUIAutomationElement* el) const {
    if (!el) return false;
    IUIAutomationInvokePattern* inv = nullptr;
    HRESULT hr = el->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern,
                                         (void**)&inv);
    if (FAILED(hr) || !inv) return false;
    hr = inv->Invoke();
    inv->Release();
    return SUCCEEDED(hr);
}

bool Session::ClickPoint(IUIAutomationElement* el) const {
    return ClickPointAt(el, 0.5f, 0.5f);
}

bool Session::ClickPointAt(IUIAutomationElement* el, float xFrac, float yFrac,
                           bool useOfficialCp) const {
    if (!el) return false;
    if (xFrac < 0.f) xFrac = 0.f;
    if (xFrac > 1.f) xFrac = 1.f;
    if (yFrac < 0.f) yFrac = 0.f;
    if (yFrac > 1.f) yFrac = 1.f;

    const RECT r = ElemRect(el);
    if (!RectVisible(r)) return false;
    POINT pt{};
    pt.x = r.left + (LONG)((r.right - r.left) * xFrac);
    pt.y = r.top + (LONG)((r.bottom - r.top) * yFrac);

    // 居中时可选官方 clickable point；账号卡须关闭（BIN 05:37 点空）
    if (useOfficialCp && xFrac > 0.4f && xFrac < 0.6f && yFrac > 0.4f && yFrac < 0.6f) {
        POINT cp{};
        BOOL got = FALSE;
        if (SUCCEEDED(el->GetClickablePoint(&cp, &got)) && got) pt = cp;
    }

    // 点完把指针放回原处，减少「鼠标被接管」的体感
    POINT saved{};
    const bool hasSaved = GetCursorPos(&saved) != FALSE;

    if (!SetCursorPos(pt.x, pt.y)) return false;
    Sleep(25);

    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(1, &down, sizeof(INPUT)) != 1) {
        if (hasSaved) SetCursorPos(saved.x, saved.y);
        return false;
    }
    Sleep(35);
    const bool ok = SendInput(1, &up, sizeof(INPUT)) == 1;
    if (hasSaved) SetCursorPos(saved.x, saved.y);
    return ok;
}

bool Session::TrySelectItem(IUIAutomationElement* el) const {
    if (!el) return false;
    IUIAutomationSelectionItemPattern* sel = nullptr;
    if (FAILED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern,
                                       (void**)&sel)) ||
        !sel)
        return false;
    const HRESULT hr = sel->Select();
    if (FAILED(hr)) {
        sel->Release();
        return false;
    }
    // Select 空成功时可能没勾上；必须读回 IsSelected（BIN：ITEM 要选对）
    Sleep(30);
    BOOL selected = FALSE;
    const HRESULT hrSel = sel->get_CurrentIsSelected(&selected);
    sel->Release();
    if (FAILED(hrSel)) return true;  // 读不到时保守认 Select 成功
    return selected != FALSE;
}

bool Session::SelectOrClick(IUIAutomationElement* el, bool allowMouse) const {
    if (!el) return false;
    // ★ 只操作传入的这一项，禁止 PreferClickableAncestor 后再 Select
    //   （父级 List/Group 也有 SelectionItem 时会选错整组/外框）
    if (TrySelectItem(el)) return true;

    if (Invoke(el)) {
        // Invoke 后若仍是可选项，再确认已勾选
        IUIAutomationSelectionItemPattern* sel = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                              IID_IUIAutomationSelectionItemPattern,
                                              (void**)&sel)) &&
            sel) {
            Sleep(30);
            BOOL selected = FALSE;
            sel->get_CurrentIsSelected(&selected);
            sel->Release();
            if (selected) return true;
            // Invoke 成功但未选中 → 继续坐标兜底
        } else {
            return true;  // 无 SelectionItem 的 Invoke（纯按钮）算成功
        }
    }

    if (!allowMouse) return false;

    // 兜底：真鼠标点这一项左侧勾选区（不点祖先外框）
    const long t = ElemType(el);
    const float xf = (t == UIA_RadioButtonControlTypeId || t == UIA_ListItemControlTypeId ||
                      t == UIA_CheckBoxControlTypeId)
                         ? 0.12f
                         : 0.5f;
    return ClickPointAt(el, xf, 0.5f);
}

bool Session::InvokeThenClick(IUIAutomationElement* el, bool forceMouse) const {
    if (!el) return false;
    if (forceMouse) {
        if (ClickPoint(el)) return true;
        return Invoke(el);
    }
    // 主路径：只走 UIA Invoke，不动系统光标
    if (Invoke(el)) return true;
    return ClickPoint(el);
}

bool Session::InvokeOrClick(IUIAutomationElement* el, bool forceMouse) const {
    return InvokeThenClick(el, forceMouse);
}

bool Session::ClickName(IUIAutomationElement* root, const wchar_t* namePart, bool substring,
                        bool forceMouse) const {
    IUIAutomationElement* el = FindByName(root, namePart, substring, true);
    if (!el) return false;
    const long t = ElemType(el);
    bool ok = false;
    if (t == UIA_RadioButtonControlTypeId || t == UIA_ListItemControlTypeId ||
        t == UIA_CheckBoxControlTypeId) {
        if (forceMouse) {
            ok = ClickPointAt(el, 0.12f, 0.5f) || SelectOrClick(el, true);
        } else {
            ok = SelectOrClick(el, true);
        }
    } else if (t == UIA_ButtonControlTypeId || t == UIA_HyperlinkControlTypeId ||
               t == UIA_MenuItemControlTypeId) {
        ok = InvokeThenClick(el, forceMouse);
    } else {
        IUIAutomationElement* parent = PreferClickableAncestor(uia_, el);
        IUIAutomationElement* use = parent ? parent : el;
        const long pt = ElemType(use);
        if (pt == UIA_RadioButtonControlTypeId || pt == UIA_ListItemControlTypeId ||
            pt == UIA_CheckBoxControlTypeId) {
            ok = forceMouse ? (ClickPointAt(use, 0.12f, 0.5f) || SelectOrClick(use, true))
                            : SelectOrClick(use, true);
        } else if (pt == UIA_ButtonControlTypeId || pt == UIA_HyperlinkControlTypeId) {
            ok = InvokeThenClick(use, forceMouse);
        } else {
            ok = InvokeOrClick(use, forceMouse);
        }
        if (parent) parent->Release();
    }
    el->Release();
    return ok;
}

bool Session::ClickLargestExactName(IUIAutomationElement* root, const wchar_t* exactName,
                                    bool forceMouse) const {
    if (!uia_ || !root || !exactName || !*exactName) return false;

    // 与 161 相同：整窗 FindAll(True) + get_CurrentName 精确比对。
    // Name PropertyCondition 在官方 Chrome 上会漏掉实际 Name 能读到的按钮（4E82）。
    IUIAutomationCondition* trueCond = nullptr;
    if (FAILED(uia_->CreateTrueCondition(&trueCond)) || !trueCond) return false;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, trueCond, &arr);
    trueCond->Release();
    if (FAILED(hr) || !arr) return false;

    int len = 0;
    arr->get_Length(&len);
    IUIAutomationElement* best = nullptr;
    int bestTier = -1;
    long bestArea = -1;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        if (n != exactName) {
            el->Release();
            continue;
        }
        const RECT r = ElemRect(el);
        if (!RectVisible(r) || !RectLooksLikeCta(r)) {
            el->Release();
            continue;
        }
        if (!ElemEnabled(el)) {
            el->Release();
            continue;
        }
        const long t = ElemType(el);
        const int tier = CtaTypeTier(t);
        const long a = (r.right - r.left) * (r.bottom - r.top);
        const bool better = (tier > bestTier) || (tier == bestTier && a > bestArea);
        if (better) {
            if (best) best->Release();
            best = el;
            bestTier = tier;
            bestArea = a;
        } else {
            el->Release();
        }
    }
    arr->Release();
    if (!best) return false;

    IUIAutomationElement* target = PreferClickableAncestor(uia_, best);
    IUIAutomationElement* use = target ? target : best;
    bool ok = InvokeOrClick(use, forceMouse);
    if (!ok && use != best) ok = InvokeOrClick(best, forceMouse);
    if (target) target->Release();
    best->Release();
    return ok;
}

bool Session::ClickLargestContentCta(IUIAutomationElement* root, HWND hwnd, std::wstring* outName,
                                     bool forceMouse) const {
    if (!uia_ || !root) return false;
    RECT wr{};
    if (hwnd && IsWindow(hwnd)) GetWindowRect(hwnd, &wr);
    if (!RectVisible(wr)) return false;

    IUIAutomationElement* bestGp = nullptr;
    IUIAutomationElement* bestLoose = nullptr;
    long bestGpArea = -1;
    long bestLooseArea = -1;
    std::wstring bestGpName;
    std::wstring bestLooseName;

    auto considerType = [&](long typeId) {
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = typeId;
        IUIAutomationCondition* cond = nullptr;
        if (FAILED(uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
            return;
        IUIAutomationElementArray* arr = nullptr;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond, &arr)) || !arr) {
            cond->Release();
            return;
        }
        cond->Release();
        int len = 0;
        arr->get_Length(&len);
        if (len > 120) len = 120;
        for (int i = 0; i < len; ++i) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            const RECT r = ElemRect(el);
            if (!RectInContentArea(r, wr) || !RectLooksLikeLooseCta(r) || !ElemEnabled(el)) {
                el->Release();
                continue;
            }
            const std::wstring n = ElemAnyName(el);
            if (NameLooksLikeBrowserChrome(n) && !NameLooksLikeGpCta(n)) {
                el->Release();
                continue;
            }
            const long a = (r.right - r.left) * (r.bottom - r.top);
            if (NameLooksLikeGpCta(n)) {
                if (a > bestGpArea) {
                    if (bestGp) bestGp->Release();
                    bestGp = el;
                    bestGpArea = a;
                    bestGpName = n;
                    continue;
                }
            } else if (a > bestLooseArea) {
                if (bestLoose) bestLoose->Release();
                bestLoose = el;
                bestLooseArea = a;
                bestLooseName = n.empty() ? L"(empty)" : n;
                continue;
            }
            el->Release();
        }
        arr->Release();
    };
    considerType(UIA_ButtonControlTypeId);
    considerType(UIA_HyperlinkControlTypeId);

    IUIAutomationElement* use = bestGp ? bestGp : bestLoose;
    const std::wstring hit = bestGp ? bestGpName : bestLooseName;
    if (bestGp && bestLoose && bestGp != bestLoose) bestLoose->Release();
    if (!use) return false;
    if (outName) *outName = hit.substr(0, 80);
    bool ok = InvokeOrClick(use, forceMouse);
    use->Release();
    return ok;
}

std::wstring Session::DumpVisibleCtas(IUIAutomationElement* root, HWND hwnd, int maxItems) const {
    if (!uia_ || !root) return L"cta-dump|no-root";
    RECT wr{};
    if (hwnd && IsWindow(hwnd)) GetWindowRect(hwnd, &wr);
    int content = 0;
    int bar = 0;
    int shown = 0;
    std::wstring bits;
    auto dumpType = [&](long typeId) {
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = typeId;
        IUIAutomationCondition* cond = nullptr;
        if (FAILED(uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
            return;
        IUIAutomationElementArray* arr = nullptr;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond, &arr)) || !arr) {
            cond->Release();
            return;
        }
        cond->Release();
        int len = 0;
        arr->get_Length(&len);
        if (len > 120) len = 120;
        for (int i = 0; i < len; ++i) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            const RECT r = ElemRect(el);
            if (!RectVisible(r)) {
                el->Release();
                continue;
            }
            const std::wstring n = ElemAnyName(el);
            const bool inC = RectInContentArea(r, wr);
            if (inC) ++content;
            else ++bar;
            if (inC && shown < maxItems) {
                ++shown;
                bits += L" [";
                bits += n.empty() ? L"?" : n.substr(0, 28);
                bits += L"|";
                bits += std::to_wstring(r.right - r.left);
                bits += L"x";
                bits += std::to_wstring(r.bottom - r.top);
                bits += L"]";
            }
            el->Release();
        }
        arr->Release();
    };
    dumpType(UIA_ButtonControlTypeId);
    dumpType(UIA_HyperlinkControlTypeId);
    std::wstring out = L"cta-dump|content=";
    out += std::to_wstring(content);
    out += L"|chrome=";
    out += std::to_wstring(bar);
    out += bits.empty() ? L"| (内容区无 Button/链接)" : bits;
    return out;
}

bool Session::ClickNamedIndex(IUIAutomationElement* root, const std::vector<std::wstring>& nameParts,
                              int index0, bool substring, std::wstring* outName,
                              bool forceMouse) const {
    if (!uia_ || !root || index0 < 0 || nameParts.empty()) return false;

    IUIAutomationCondition* trueCond = nullptr;
    if (FAILED(uia_->CreateTrueCondition(&trueCond)) || !trueCond) return false;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, trueCond, &arr);
    trueCond->Release();
    if (FAILED(hr) || !arr) return false;

    int len = 0;
    arr->get_Length(&len);
    std::vector<RawHit> hits;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        if (!NameMatchAny(n, nameParts, substring)) {
            el->Release();
            continue;
        }
        const RECT r = ElemRect(el);
        if (!RectVisible(r)) {
            el->Release();
            continue;
        }
        RawHit h;
        h.el = el;
        h.info.rect = r;
        h.info.name = n;
        h.info.controlType = ElemType(el);
        hits.push_back(std::move(h));
    }
    arr->Release();
    SortHits(hits);
    std::vector<RawHit> uniq;
    for (auto& h : hits) {
        bool dup = false;
        for (const auto& u : uniq) {
            if (std::abs(u.info.rect.top - h.info.rect.top) < 8 &&
                std::abs(u.info.rect.left - h.info.rect.left) < 8) {
                dup = true;
                break;
            }
        }
        if (!dup) uniq.push_back(std::move(h));
        else h.el->Release();
    }
    bool ok = false;
    if (index0 < (int)uniq.size()) {
        if (outName) *outName = uniq[index0].info.name;
        IUIAutomationElement* target = PreferClickableAncestor(uia_, uniq[index0].el);
        ok = InvokeOrClick(target ? target : uniq[index0].el, forceMouse);
        if (target) target->Release();
    }
    for (auto& h : uniq) h.el->Release();
    return ok;
}

bool Session::ClickAccountCardIndex(IUIAutomationElement* root, int index0, std::wstring* outName,
                                    int clickVariant) const {
    if (!uia_ || !root || index0 < 0) return false;

    // 已是 Document 就不要再找子 Document（iframe 会搜偏）。窗口根才提页。
    CONTROLTYPEID rootType = 0;
    root->get_CurrentControlType(&rootType);
    IUIAutomationElement* page =
        (rootType == UIA_DocumentControlTypeId) ? nullptr : FindPageDocument(uia_, root);

    struct AccHit {
        IUIAutomationElement* el = nullptr;
        std::wstring name;
        std::wstring mailKey;
        RECT rect{};
        long area = 0;
        int mailCount = 0;
        int bonus = 0;
    };

    std::vector<AccHit> uniq;
    auto collectFrom = [&](IUIAutomationElement* scope) {
        for (auto& h : uniq) h.el->Release();
        uniq.clear();
        if (!scope) return;

        IUIAutomationCondition* trueCond = nullptr;
        if (FAILED(uia_->CreateTrueCondition(&trueCond)) || !trueCond) return;
        IUIAutomationElementArray* arr = nullptr;
        HRESULT hr = scope->FindAll(TreeScope_Descendants, trueCond, &arr);
        trueCond->Release();
        if (FAILED(hr) || !arr) return;

        int len = 0;
        arr->get_Length(&len);
        std::vector<AccHit> raw;
        for (int i = 0; i < len; ++i) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            const std::wstring n = ElemName(el);
            if (!LooksLikeAccountSeedName(n)) {
                el->Release();
                continue;
            }
            const RECT seedR = ElemRect(el);
            const long seedW = seedR.right - seedR.left;
            const long seedH = seedR.bottom - seedR.top;
            const int seedAt = CountAtSigns(n);

            IUIAutomationElement* card = nullptr;
            // 种子本身已是「单卡尺寸」→ 不再上溯（防单账号爬进列表壳，BIN 05:37/05:50）
            if (seedAt == 1 && seedH >= 40 && seedH <= 160 && seedW >= 100 && seedW <= 640 &&
                RectVisible(seedR)) {
                card = el;
                card->AddRef();
                el->Release();
            } else {
                card = ResolveAccountCard(uia_, el);
                el->Release();
            }
            if (!card) continue;
            const std::wstring ct = ElemName(card);
            if (BadAccountCardName(ct) || CountAtSigns(ct) >= 2) {
                card->Release();
                continue;
            }
            const RECT r = ElemRect(card);
            if (!RectVisible(r)) {
                card->Release();
                continue;
            }
            const long w = r.right - r.left;
            const long hgt = r.bottom - r.top;
            const long area = w * hgt;
            if (area < 80 || hgt > 180 || w > 680) {
                card->Release();
                continue;
            }
            AccHit hit;
            hit.el = card;
            hit.name = ct.empty() ? n : ct;
            hit.mailKey = MailKeyFromName(hit.name);
            if (hit.mailKey.empty()) hit.mailKey = MailKeyFromName(n);
            hit.rect = r;
            hit.area = area;
            hit.mailCount = CountAtSigns(hit.name);
            if (hit.mailCount == 1) hit.bonus += 30;
            if (hit.name.find(L'@') != std::wstring::npos) hit.bonus += 10;
            if (StrStrIW(hit.name.c_str(), L"default-user-avatar")) hit.bonus += 5;
            if (hgt >= 48 && hgt <= 120) hit.bonus += 15;
            raw.push_back(std::move(hit));
        }
        arr->Release();

        for (auto& h : raw) {
            int idx = -1;
            for (int j = 0; j < (int)uniq.size(); ++j) {
                auto& u = uniq[j];
                const bool sameMail = !h.mailKey.empty() && h.mailKey == u.mailKey;
                const bool nearPos = std::abs(h.rect.top - u.rect.top) < 12 &&
                                     std::abs(h.rect.left - u.rect.left) < 12;
                const bool nested =
                    (h.rect.left <= u.rect.left + 2 && h.rect.top <= u.rect.top + 2 &&
                     h.rect.right >= u.rect.right - 2 && h.rect.bottom >= u.rect.bottom - 2) ||
                    (u.rect.left <= h.rect.left + 2 && u.rect.top <= h.rect.top + 2 &&
                     u.rect.right >= h.rect.right - 2 && u.rect.bottom >= h.rect.bottom - 2);
                if (sameMail || nearPos || nested) {
                    idx = j;
                    break;
                }
            }
            if (idx < 0) {
                uniq.push_back(std::move(h));
                continue;
            }
            auto& cur = uniq[idx];
            bool preferH = false;
            if (h.mailCount == 1 && cur.mailCount != 1)
                preferH = true;
            else if (h.mailCount != 1 && cur.mailCount == 1)
                preferH = false;
            else if (h.mailCount == 1 && cur.mailCount == 1)
                preferH = (h.area < cur.area) || (h.area == cur.area && h.bonus > cur.bonus);
            else if (h.bonus > cur.bonus || (h.bonus == cur.bonus && h.area < cur.area))
                preferH = true;
            if (preferH) {
                cur.el->Release();
                cur = std::move(h);
            } else {
                h.el->Release();
            }
        }
        std::sort(uniq.begin(), uniq.end(), [](const AccHit& a, const AccHit& b) {
            if (a.rect.top != b.rect.top) return a.rect.top < b.rect.top;
            return a.rect.left < b.rect.left;
        });
    };

    collectFrom(page ? page : root);
    if (uniq.empty() && page) collectFrom(root);
    if (page) page->Release();

    bool ok = false;
    if (index0 < (int)uniq.size()) {
        const auto& pick = uniq[index0];
        std::wstring how;
        ok = ActivateAccountCard(this, pick.el, clickVariant, uia_, &how);
        if (outName) {
            *outName = pick.name.substr(0, 40) + L"|" +
                       std::to_wstring(pick.rect.right - pick.rect.left) + L"x" +
                       std::to_wstring(pick.rect.bottom - pick.rect.top) + L"|t" +
                       std::to_wstring(ElemType(pick.el)) + L"|v" + std::to_wstring(clickVariant) +
                       L"|" + (how.empty() ? std::wstring(L"none") : how);
        }
    }
    for (auto& h : uniq) h.el->Release();
    return ok;
}

bool Session::ClickTypeIndex(IUIAutomationElement* root, long controlTypeId, int index0,
                             std::wstring* outName, bool forceMouse) const {
    if (!uia_ || !root || index0 < 0) return false;
    VARIANT v{};
    v.vt = VT_I4;
    v.lVal = controlTypeId;
    IUIAutomationCondition* cond = nullptr;
    if (FAILED(uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
        return false;
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    if (FAILED(hr) || !arr) return false;
    int len = 0;
    arr->get_Length(&len);
    std::vector<RawHit> hits;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        const std::wstring n = ElemName(el);
        if (n.find(L"建立") != std::wstring::npos || n.find(L"Create") != std::wstring::npos) {
            el->Release();
            continue;
        }
        // 过滤浏览器壳/无关可选项，避免 slot1 点到错误 ITEM
        if (n.find(L"Google") != std::wstring::npos || n.find(L"Chrome") != std::wstring::npos ||
            n.find(L"設定") != std::wstring::npos || n.find(L"设置") != std::wstring::npos ||
            n.find(L"Settings") != std::wstring::npos || n.find(L"書籤") != std::wstring::npos ||
            n.find(L"扩展") != std::wstring::npos || n.find(L"擴充") != std::wstring::npos) {
            el->Release();
            continue;
        }
        const RECT r = ElemRect(el);
        if (!RectVisible(r)) {
            el->Release();
            continue;
        }
        // 昵称行合理高度；过大像整页容器
        const long h = r.bottom - r.top;
        const long w = r.right - r.left;
        if (h > 160 || w > 1000 || h < 10) {
            el->Release();
            continue;
        }
        // 必须真有 SelectionItem，否则索引会混进不可选节点
        IUIAutomationSelectionItemPattern* sip = nullptr;
        const bool hasSel =
            SUCCEEDED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                              IID_IUIAutomationSelectionItemPattern,
                                              (void**)&sip)) &&
            sip;
        if (sip) sip->Release();
        if (!hasSel && controlTypeId != UIA_RadioButtonControlTypeId) {
            el->Release();
            continue;
        }
        RawHit hit;
        hit.el = el;
        hit.info.rect = r;
        hit.info.name = n;
        hits.push_back(std::move(hit));
    }
    arr->Release();
    SortHits(hits);
    // 同位置去重，保留先出现者
    std::vector<RawHit> uniq;
    for (auto& h : hits) {
        bool dup = false;
        for (const auto& u : uniq) {
            if (std::abs(u.info.rect.top - h.info.rect.top) < 6 &&
                std::abs(u.info.rect.left - h.info.rect.left) < 6) {
                dup = true;
                break;
            }
        }
        if (!dup) uniq.push_back(std::move(h));
        else h.el->Release();
    }
    bool ok = false;
    if (index0 < (int)uniq.size()) {
        if (outName) *outName = uniq[index0].info.name;
        if (forceMouse) {
            ok = ClickPointAt(uniq[index0].el, 0.12f, 0.5f) ||
                 SelectOrClick(uniq[index0].el, true);
        } else {
            ok = SelectOrClick(uniq[index0].el, true);
        }
    }
    for (auto& h : uniq) h.el->Release();
    return ok;
}

bool Session::NameContains(IUIAutomationElement* root, const wchar_t* namePart,
                           bool substring) const {
    IUIAutomationElement* el = FindByName(root, namePart, substring, false);
    if (!el) return false;
    el->Release();
    return true;
}

std::wstring Session::ReadUrlHint(IUIAutomationElement* root, HWND hwnd) const {
    auto looksUrl = [](const std::wstring& s) -> bool {
        if (s.size() < 8) return false;
        return StrStrIW(s.c_str(), L"https://") || StrStrIW(s.c_str(), L"http://") ||
               StrStrIW(s.c_str(), L"chrome-error") ||
               StrStrIW(s.c_str(), L"galaxy.games.gamania") ||
               StrStrIW(s.c_str(), L"accounts.gamania") ||
               StrStrIW(s.c_str(), L"beanfun.com") ||
               StrStrIW(s.c_str(), L"maplestoryclassic");
    };

    // 1) 地址栏 Edit ValuePattern（Chrome/Edge 开无障碍后常见）
    if (uia_ && root) {
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = UIA_EditControlTypeId;
        IUIAutomationCondition* cond = nullptr;
        if (SUCCEEDED(uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) && cond) {
            IUIAutomationElementArray* arr = nullptr;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int len = 0;
                arr->get_Length(&len);
                for (int i = 0; i < len && i < 40; ++i) {
                    IUIAutomationElement* el = nullptr;
                    if (FAILED(arr->GetElement(i, &el)) || !el) continue;
                    const std::wstring n = ElemName(el);
                    IUIAutomationValuePattern* vp = nullptr;
                    std::wstring val;
                    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                                                          IID_IUIAutomationValuePattern,
                                                          (void**)&vp)) &&
                        vp) {
                        BSTR b = nullptr;
                        if (SUCCEEDED(vp->get_CurrentValue(&b)) && b) {
                            val.assign(b, SysStringLen(b));
                            SysFreeString(b);
                        }
                        vp->Release();
                    }
                    el->Release();
                    // 经典版 / TWMS：繁中 Chrome 为「網址列」「位址和搜尋列」；简中/英兜底
                    const bool omniboxName =
                        StrStrIW(n.c_str(), L"網址") || StrStrIW(n.c_str(), L"位址") ||
                        StrStrIW(n.c_str(), L"搜尋") || StrStrIW(n.c_str(), L"地址") ||
                        StrStrIW(n.c_str(), L"搜索") || StrStrIW(n.c_str(), L"Address") ||
                        StrStrIW(n.c_str(), L"Search") || StrStrIW(n.c_str(), L"omnibox") ||
                        n.empty();
                    if (!val.empty() && looksUrl(val) &&
                        (omniboxName || StrStrIW(val.c_str(), L"http") ||
                         StrStrIW(val.c_str(), L"galaxy.games") ||
                         StrStrIW(val.c_str(), L"accounts.gamania") ||
                         StrStrIW(val.c_str(), L"beanfun"))) {
                        arr->Release();
                        cond->Release();
                        return val;
                    }
                }
                arr->Release();
            }
            cond->Release();
        }
    }

    // 2) 窗口标题兜底（常含域名/页面名）
    if (hwnd && IsWindow(hwnd)) {
        wchar_t title[512]{};
        GetWindowTextW(hwnd, title, 512);
        if (title[0] && looksUrl(title)) return title;
        if (title[0]) return title;  // 仍返回标题供阶段启发式
    }
    return {};
}

namespace {

struct EnumCtx {
    DWORD pid = 0;
    HWND best = nullptr;
    std::vector<std::wstring> keywords;
    LogFn log;
};

struct EnumAllCtx {
    std::vector<HWND>* out = nullptr;
};

bool IsBrowserClass(const wchar_t* cls) {
    if (!cls) return false;
    return _wcsicmp(cls, L"Chrome_WidgetWin_1") == 0 ||
           _wcsicmp(cls, L"Chrome_WidgetWin_0") == 0 ||
           _wcsicmp(cls, L"MozillaWindowClass") == 0;
}

BOOL CALLBACK EnumPidProc(HWND hwnd, LPARAM lp) {
    auto* ctx = (EnumCtx*)lp;
    if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    if (!IsBrowserClass(cls)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    ctx->best = hwnd;
    return FALSE;
}

BOOL CALLBACK EnumTitleProc(HWND hwnd, LPARAM lp) {
    auto* ctx = (EnumCtx*)lp;
    if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (ctx->pid) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
    }
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    if (!IsBrowserClass(cls)) return TRUE;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!title[0]) return TRUE;
    for (const auto& k : ctx->keywords) {
        if (StrStrIW(title, k.c_str())) {
            ctx->best = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

BOOL CALLBACK EnumAllBrowserProc(HWND hwnd, LPARAM lp) {
    auto* ctx = (EnumAllCtx*)lp;
    if (!ctx || !ctx->out) return FALSE;
    if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    if (!IsBrowserClass(cls)) return TRUE;
    ctx->out->push_back(hwnd);
    return TRUE;
}

bool TitleMatchesAny(HWND hwnd, const std::vector<std::wstring>& keys) {
    if (!hwnd || keys.empty()) return false;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!title[0]) return false;
    for (const auto& k : keys) {
        if (StrStrIW(title, k.c_str())) return true;
    }
    return false;
}

}  // namespace

HWND FindBrowserMainHwnd(DWORD pid, const LogFn& log) {
    if (!pid) return nullptr;
    EnumCtx ctx;
    ctx.pid = pid;
    ctx.log = log;
    EnumWindows(EnumPidProc, (LPARAM)&ctx);
    if (ctx.best) LogLine(log, L"[uia] 找到浏览器窗 pid=" + std::to_wstring(pid));
    return ctx.best;
}

std::vector<HWND> EnumBrowserTopHwnds() {
    std::vector<HWND> out;
    EnumAllCtx ctx;
    ctx.out = &out;
    EnumWindows(EnumAllBrowserProc, (LPARAM)&ctx);
    return out;
}

HWND FindNewBrowserHwnd(const std::vector<HWND>& beforeLaunch,
                        const std::vector<std::wstring>& titleKeywords, const LogFn& log) {
    const auto now = EnumBrowserTopHwnds();
    std::vector<HWND> news;
    news.reserve(now.size());
    for (HWND h : now) {
        bool known = false;
        for (HWND b : beforeLaunch) {
            if (b == h) {
                known = true;
                break;
            }
        }
        if (!known) news.push_back(h);
    }
    if (news.empty()) return nullptr;

    for (HWND h : news) {
        if (TitleMatchesAny(h, titleKeywords)) {
            LogLine(log, L"[uia] 命中启动后新窗（标题匹配 Galaxy/账号）");
            return h;
        }
    }
    // 标题尚未变成域名时：唯一新窗多半就是 --new-window 拉起的 Galaxy
    if (news.size() == 1) {
        LogLine(log, L"[uia] 命中启动后唯一新窗（标题可能仍为无标题）");
        return news.front();
    }
    // 多个无标题新窗：先别瞎抓 Z 序——等调用方下一轮；若已等过则取前台
    HWND fg = GetForegroundWindow();
    for (HWND h : news) {
        if (h == fg) {
            LogLine(log, L"[uia] 命中启动后新窗（当前前台，多新窗）");
            return h;
        }
    }
    // 仍无标题匹配：返回 nullptr，让 WaitBrowserHwnd 多等几轮（避免挂到空白壳窗）
    return nullptr;
}

HWND FindBrowserHwndByTitleKeywords(const std::vector<std::wstring>& keywords, const LogFn& log,
                                    DWORD restrictPid) {
    EnumCtx ctx;
    ctx.keywords = keywords;
    ctx.pid = restrictPid;
    ctx.log = log;
    EnumWindows(EnumTitleProc, (LPARAM)&ctx);
    if (ctx.best) {
        if (restrictPid)
            LogLine(log, L"[uia] 按标题命中浏览器窗（pid限定）");
        else
            LogLine(log, L"[uia] 按标题命中浏览器窗（强关键词全局）");
    }
    return ctx.best;
}

bool IsBrowserWindowInteractive(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    return (r.right - r.left) >= 64 && (r.bottom - r.top) >= 64;
}

bool BringToForeground(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    DWORD targetPid = 0;
    const DWORD targetTid = GetWindowThreadProcessId(hwnd, &targetPid);
    // 允许本进程把目标浏览器拉到前台（自动登录硬需求）
    if (targetPid) AllowSetForegroundWindow(targetPid);
    AllowSetForegroundWindow(ASFW_ANY);

    // GAMA PASS 账号卡靠几何 SendInput：窗口化过小易裁切/点空 → 一律最大化
    if (IsIconic(hwnd) || !IsZoomed(hwnd))
        ShowWindow(hwnd, SW_MAXIMIZE);
    else
        ShowWindow(hwnd, SW_SHOW);

    HWND fore = GetForegroundWindow();
    const DWORD foreTid = fore ? GetWindowThreadProcessId(fore, nullptr) : 0;
    const DWORD selfTid = GetCurrentThreadId();
    const bool attachFore = foreTid && foreTid != selfTid;
    const bool attachTarget = targetTid && targetTid != selfTid && targetTid != foreTid;
    if (attachFore) AttachThreadInput(selfTid, foreTid, TRUE);
    if (attachTarget) AttachThreadInput(selfTid, targetTid, TRUE);

    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    if (attachTarget) AttachThreadInput(selfTid, targetTid, FALSE);
    if (attachFore) AttachThreadInput(selfTid, foreTid, FALSE);

    // 仍非前台时再试一次（某些机子第一次会被策略挡掉）
    if (GetForegroundWindow() != hwnd) {
        SetForegroundWindow(hwnd);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    // 置前后再确认一次最大化（偶发第一次被忽略）
    if (!IsZoomed(hwnd) && !IsIconic(hwnd)) ShowWindow(hwnd, SW_MAXIMIZE);
    return IsBrowserWindowInteractive(hwnd);
}

}  // namespace msc::uia
