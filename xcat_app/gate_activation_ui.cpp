#include "gate_activation_ui.h"

#include "app_theme.h"
#include "app_window.h"
#include "imgui_shell.h"

#include "imgui.h"

#include "xcat_start_gate.h"

#include "xcat_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace xcat::app {
namespace {

std::string TrimCopy(const char* s) {
    std::string v = s ? s : "";
    const char* ws = " \t\r\n";
    const size_t b = v.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = v.find_last_not_of(ws);
    return v.substr(b, e - b + 1);
}

// 顶栏：品牌标题 + 可拖拽区 + 右上角关闭「×」。返回 true 表示用户点了关闭（取消激活）。
bool DrawGateTitleBar(AppWindow& app) {
    const AppPalette& p = AppTheme_Palette();
    const float titleH = ImGui::GetFrameHeight() * 1.35f;
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const float fullW = ImGui::GetContentRegionAvail().x;
    const float closeW = titleH;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        const ImU32 top = ImGui::ColorConvertFloat4ToU32(p.titleBarTop);
        const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(p.titleBarBottom);
        dl->AddRectFilledMultiColor(origin, ImVec2(origin.x + fullW, origin.y + titleH), top, top,
                                    bottom, bottom);
        dl->AddLine(ImVec2(origin.x, origin.y + 1.f), ImVec2(origin.x + fullW, origin.y + 1.f),
                    ImGui::ColorConvertFloat4ToU32(p.titleBarLineTop));
        dl->AddLine(ImVec2(origin.x, origin.y + titleH),
                    ImVec2(origin.x + fullW, origin.y + titleH),
                    ImGui::ColorConvertFloat4ToU32(p.titleBarLineBottom));
        dl->AddText(ImVec2(origin.x + pad, origin.y + (titleH - ImGui::GetFontSize()) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(p.brandText), "XCat 启动激活");
    }

    // 拖拽区：占满除关闭按钮外的整条。
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##gate_drag", ImVec2((std::max)(1.f, fullW - closeW), titleH));
    AppWindow_DragFromLastItem(app);

    bool wantClose = false;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + fullW - closeW, origin.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.dangerHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.dangerActive);
    ImGui::PushStyleColor(ImGuiCol_Text, p.captionBtnText);
    if (ImGui::Button("×##gate_close", ImVec2(closeW, titleH))) wantClose = true;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + titleH));
    return wantClose;
}

// 把主窗临时缩成居中小对话框（逻辑像素，按 DPI 缩放）。用于门控/校验这类过场态，
// 避免直接铺满窄高的启动器窗口显成一条竖长条。
void CenterWindowAsDialog(AppWindow& app, float logicalW, float logicalH) {
    const float s = app.dpiScale > 0.1f ? app.dpiScale : 1.f;
    const int cw = static_cast<int>(logicalW * s);
    const int ch = static_cast<int>(logicalH * s);
    int cx, cy;
    MONITORINFO mi{sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        cx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - cw) / 2;
        cy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - ch) / 2;
    } else {
        cx = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
        cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    }
    SetWindowPos(app.hwnd, nullptr, cx, cy, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

bool RunGateActivation(AppWindow& app, const std::string& binDir, const std::string& deviceId,
                       const char* extraError) {
    // 临时切白天主题（不落盘）；返回前恢复用户原偏好。
    const AppThemeMode savedMode = AppTheme_Mode();
    AppTheme_SetMode(xcat::ui::ThemePreference::Light);
    AppTheme_Commit(app.hwnd);

    float clearColor[4]{};
    AppWindow_GetClearColor(clearColor);

    // 激活阶段把整窗缩成紧凑居中对话框（铺满大启动器窗会显空）；成功后恢复原尺寸。
    RECT origRect{};
    GetWindowRect(app.hwnd, &origRect);
    {
        const float s = app.dpiScale > 0.1f ? app.dpiScale : 1.f;
        const int cw = static_cast<int>(480.f * s);
        const int ch = static_cast<int>(312.f * s);
        int cx, cy;
        MONITORINFO mi{sizeof(mi)};
        HMONITOR mon = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            cx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - cw) / 2;
            cy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - ch) / 2;
        } else {
            cx = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
            cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
        }
        SetWindowPos(app.hwnd, nullptr, cx, cy, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    char tokenBuf[1400]{};
    std::string errorText = extraError ? extraError : "";
    bool focusOnce = false;
    bool shown = false;
    bool result = false;
    bool loop = true;

    xcat::log::Info("Auth", "gate/1 activation UI shown (ImGui light)");

    while (loop && app.running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) app.running = false;
        }
        if (!app.running) break;

        const ULONGLONG frameStart = GetTickCount64();
        AppWindow_BeginFrame(app, clearColor);

        bool submit = false;
        bool cancel = false;
        {
            ui::LauncherFrame frame(0.f);
            if (frame.visible) {
                if (DrawGateTitleBar(app)) cancel = true;

                ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.6f));
                ImGui::TextWrapped("本软件仅供内部使用。请粘贴管理员发放的启动 TOKEN 以激活本机。");
                ImGui::TextDisabled("激活成功后本机免输。若反复出现此框，请向管理员索取现卡。");
                ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.4f));

                ImGui::TextUnformatted("启动 TOKEN");
                ImGui::SetNextItemWidth(-1.f);
                if (!focusOnce) {
                    ImGui::SetKeyboardFocusHere();
                    focusOnce = true;
                }
                if (ImGui::InputText("##gate_token", tokenBuf, sizeof(tokenBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    submit = true;
                }

                if (!errorText.empty()) {
                    ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.2f));
                    ImGui::TextColored(ImVec4(0.82f, 0.10f, 0.10f, 1.f), "%s", errorText.c_str());
                }

                ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.6f));
                const float btnW = ImGui::GetFontSize() * 6.5f;
                const float btnH = ImGui::GetFrameHeight() * 1.35f;
                if (ImGui::Button("激活并进入", ImVec2(btnW, btnH))) submit = true;
                ImGui::SameLine();
                if (ImGui::Button("退出", ImVec2(btnW * 0.72f, btnH))) cancel = true;
            }
        }

        if (submit) {
            const std::string token = TrimCopy(tokenBuf);
            if (token.empty()) {
                errorText = "请先粘贴启动 TOKEN。";
            } else {
                gate::TokenClaims claims;
                if (gate::CommitActivation(binDir, deviceId, token, claims)) {
                    result = true;
                    loop = false;
                } else {
                    errorText = "TOKEN 无效或已过期，请核对后重试（如无 TOKEN 请联系管理员）。";
                }
            }
        }
        if (cancel) {
            result = false;
            loop = false;
        }

        AppWindow_EndFrame(app);

        if (!shown) {
            AppWindow_Show(app);
            SetForegroundWindow(app.hwnd);
            shown = true;
        }

        constexpr ULONGLONG kUiFrameBudgetMs = 33;
        const ULONGLONG elapsed = GetTickCount64() - frameStart;
        if (elapsed < kUiFrameBudgetMs) Sleep(static_cast<DWORD>(kUiFrameBudgetMs - elapsed));
    }

    // 恢复用户原主题（不落盘）。
    AppTheme_SetMode(savedMode);
    AppTheme_Commit(app.hwnd);

    if (!app.running) result = false;  // 关窗 → 视同取消
    if (result) {
        // 激活成功：恢复启动器窗口尺寸/位置，交回主循环。
        SetWindowPos(app.hwnd, nullptr, origRect.left, origRect.top,
                     origRect.right - origRect.left, origRect.bottom - origRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return result;
}

bool RunStartupGateWithModal(AppWindow& app, const char* statusText,
                             const std::function<bool()>& work) {
    if (!work) return false;

    std::atomic<bool> done{false};
    bool result = false;
    std::thread worker([&] {
        result = work();
        done.store(true, std::memory_order_release);
    });

    // 快路径：服务器在场 / 无粘性时探活通常 <100ms 就返回。先短等一小会，若很快结束就
    // 完全不弹窗、不动窗口尺寸，维持原启动观感、避免无谓的缩放闪烁。
    const ULONGLONG graceUntilMs = GetTickCount64() + 180;
    while (!done.load(std::memory_order_acquire) && GetTickCount64() < graceUntilMs) {
        Sleep(10);
    }
    if (done.load(std::memory_order_acquire)) {
        worker.join();
        return result;
    }

    // 慢路径（运维不可达等）：把窗口缩成居中小对话框，泵「正在检查授权…」帧。
    // 不在此恢复原尺寸——由调用方在放行进主面板前统一恢复，避免两道门之间反复缩放闪烁。
    CenterWindowAsDialog(app, 460.f, 300.f);

    float clearColor[4]{};
    AppWindow_GetClearColor(clearColor);
    bool shown = false;
    const ULONGLONG startMs = GetTickCount64();

    while (!done.load(std::memory_order_acquire) && app.running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) app.running = false;
        }
        if (!app.running) break;

        const ULONGLONG frameStart = GetTickCount64();
        AppWindow_BeginFrame(app, clearColor);
        {
            ui::LauncherFrame frame(0.f);
            if (frame.visible) {
                if (DrawGateTitleBar(app)) {
                    // 校验过场态：右上角「×」等同关闭启动器（随后 join 等探活收尾再退）。
                    PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
                }

                const int phase = static_cast<int>((frameStart - startMs) / 350) % 4;
                char dots[4] = {0};
                for (int i = 0; i < phase; ++i) dots[i] = '.';

                const ImVec2 avail = ImGui::GetContentRegionAvail();
                ImGui::Dummy(ImVec2(0.f, (std::max)(0.f, avail.y * 0.34f)));

                char line[128]{};
                _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s", statusText, dots);
                const float tw = ImGui::CalcTextSize(line).x;
                ImGui::SetCursorPosX((std::max)(0.f, (ImGui::GetWindowWidth() - tw) * 0.5f));
                ImGui::TextUnformatted(line);

                const char* hint = "正在联系服务器，请稍候（可拖动或关闭本窗）。";
                const float hw = ImGui::CalcTextSize(hint).x;
                ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.4f));
                ImGui::SetCursorPosX((std::max)(0.f, (ImGui::GetWindowWidth() - hw) * 0.5f));
                ImGui::TextDisabled("%s", hint);
            }
        }
        AppWindow_EndFrame(app);

        if (!shown) {
            AppWindow_Show(app);
            SetForegroundWindow(app.hwnd);
            shown = true;
        }

        constexpr ULONGLONG kUiFrameBudgetMs = 33;
        const ULONGLONG elapsed = GetTickCount64() - frameStart;
        if (elapsed < kUiFrameBudgetMs) Sleep(static_cast<DWORD>(kUiFrameBudgetMs - elapsed));
    }

    // 用户在校验过场态点了关闭 / 关窗：后台探活可能仍卡在 WinHTTP 完整回退（数十秒）。
    // 关闭=退出启动器，直接结束进程，不为已放弃的探活 join 干等；互斥量随进程退出自动释放。
    if (!app.running) {
        xcat::log::Info("Auth", "startup gate closed by user during netcheck; exit now");
        worker.detach();
        ExitProcess(0);
    }

    // 探活正常收尾（有界超时）：join 回收后台线程，避免其访问已析构的栈上捕获。
    worker.join();
    return result;
}

}  // namespace xcat::app
