#pragma once

// GAMA PASS 登录阶段（账密直登 / 日常自动登录共用）。
// worker 写入，UI 线程只读；busy 结束须清回 Idle。

namespace msc::launcher {

enum class GamaPassUiPhase : int {
    Idle = 0,
    OpeningBrowser,  // 开浏览器
    FillingForm,     // 填表
    TwoFactor,       // 二次验证
    ClickGamaPass,   // 点 Gama Pass
    SelectAccount,   // 选账号
    SelectNick,      // 选号（昵称）
    WaitNgm,         // 等 NGM
    WaitClassic,     // 已见 NGM，等经典版
    ManualLogin,     // 请在窗口内登录
    CreateNick,      // 新号：请在本窗建立游戏昵称
};

void SetGamaPassUiPhase(GamaPassUiPhase phase);
GamaPassUiPhase GetGamaPassUiPhase();
// UTF-8，给 ImGui；Idle 返回空串。
const char* GamaPassUiPhaseLabel(GamaPassUiPhase phase);

void ResetGamaPassLoginCancel();
void RequestGamaPassLoginCancel();
bool GamaPassLoginCanceled();

}  // namespace msc::launcher
