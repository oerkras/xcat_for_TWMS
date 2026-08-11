#pragma once

// 最小 Windows UI Automation 封装（GamaPass 日常浏览器点选）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ole2.h>
#include <UIAutomation.h>

#include <functional>
#include <string>
#include <vector>

namespace msc::uia {

using LogFn = std::function<void(const std::wstring& line)>;

struct ElementHit {
    RECT rect{};
    std::wstring name;
    long controlType = 0;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Init(const LogFn& log = nullptr);
    void Close();

    IUIAutomation* Raw() const { return uia_; }

    // 从 HWND 取根元素（调用方 Release）
    IUIAutomationElement* ElementFromHwnd(HWND hwnd) const;

    // Name 精确或包含匹配；preferInvokeable 优先 Button/Hyperlink/ListItem/RadioButton/CheckBox
    IUIAutomationElement* FindByName(IUIAutomationElement* root, const wchar_t* namePart,
                                     bool substring, bool preferInvokeable = true) const;

    // 收集 Name 命中任一别名的可点元素，按屏幕 Y 再 X 排序
    std::vector<ElementHit> CollectNamedHits(IUIAutomationElement* root,
                                             const std::vector<std::wstring>& nameParts,
                                             bool substring) const;

    // 按 ControlType 收集（如 UIA_RadioButtonControlTypeId），带 Name，按 Y/X 排序
    std::vector<ElementHit> CollectByType(IUIAutomationElement* root, long controlTypeId) const;

    bool Invoke(IUIAutomationElement* el) const;
    bool ClickPoint(IUIAutomationElement* el) const;
    // 按控件矩形比例点击；useOfficialCp=false 时只用几何中心（账号卡对齐 CDP，忌 GetClickablePoint 偏到不可点子）
    bool ClickPointAt(IUIAutomationElement* el, float xFrac, float yFrac,
                      bool useOfficialCp = true) const;
    bool TrySelectItem(IUIAutomationElement* el) const;
    // SelectionItem.Select 优先；否则 Invoke；再不行走坐标（真鼠标，仅兜底）
    bool SelectOrClick(IUIAutomationElement* el, bool allowMouse = true) const;
    // 默认：Invoke 成功即返回；仅 Invoke 失败才 SetCursorPos（forceMouse=重试时强制坐标）
    bool InvokeThenClick(IUIAutomationElement* el, bool forceMouse = false) const;
    // 同 InvokeThenClick（账号卡 / 通用可点）
    bool InvokeOrClick(IUIAutomationElement* el, bool forceMouse = false) const;

    bool ClickName(IUIAutomationElement* root, const wchar_t* namePart, bool substring = true,
                   bool forceMouse = false) const;

    // 精确 Name：优先启用中的 Button/链接；默认 Invoke，forceMouse 时坐标兜底优先
    bool ClickLargestExactName(IUIAutomationElement* root, const wchar_t* exactName,
                               bool forceMouse = false) const;

    bool ClickNamedIndex(IUIAutomationElement* root, const std::vector<std::wstring>& nameParts,
                         int index0, bool substring, std::wstring* outName = nullptr,
                         bool forceMouse = false) const;

    // 选账号页：对齐 CDP JsSelectAccount（紧单卡 + 几何点击）；clickVariant 重试换水平落点
    bool ClickAccountCardIndex(IUIAutomationElement* root, int index0,
                               std::wstring* outName = nullptr, int clickVariant = 0) const;

    bool ClickTypeIndex(IUIAutomationElement* root, long controlTypeId, int index0,
                        std::wstring* outName = nullptr, bool forceMouse = false) const;

    bool NameContains(IUIAutomationElement* root, const wchar_t* namePart,
                      bool substring = true) const;

    // 读地址栏 Value（https…）或窗口标题里的域名线索；失败返回空
    std::wstring ReadUrlHint(IUIAutomationElement* root, HWND hwnd = nullptr) const;

private:
    IUIAutomation* uia_ = nullptr;
    bool comInited_ = false;
    LogFn log_;
};

HWND FindBrowserMainHwnd(DWORD pid, const LogFn& log = nullptr);

// 枚举当前所有 Chromium/浏览器顶层窗（可见或最小化）
std::vector<HWND> EnumBrowserTopHwnds();

// 相对启动前快照：优先新出现的窗；多窗时优先标题命中 keywords，否则取 Z 序最前的新窗
HWND FindNewBrowserHwnd(const std::vector<HWND>& beforeLaunch,
                        const std::vector<std::wstring>& titleKeywords,
                        const LogFn& log = nullptr);

// restrictPid!=0 时只在该进程内按标题找；=0 则全局（仅应用于强关键词，勿配弱词）
HWND FindBrowserHwndByTitleKeywords(const std::vector<std::wstring>& keywords,
                                    const LogFn& log = nullptr, DWORD restrictPid = 0);

// 还原最小化、最大化并置前；小窗/最小化时 ClickPoint 屏幕坐标易偏/失效
bool BringToForeground(HWND hwnd);

bool IsBrowserWindowInteractive(HWND hwnd);

}  // namespace msc::uia
