#include "gamapass_login_phase.h"

#include <atomic>

namespace msc::launcher {
namespace {

std::atomic<int> gPhase{static_cast<int>(GamaPassUiPhase::Idle)};
std::atomic<bool> gCancel{false};

}  // namespace

void SetGamaPassUiPhase(GamaPassUiPhase phase) {
    gPhase.store(static_cast<int>(phase), std::memory_order_release);
}

GamaPassUiPhase GetGamaPassUiPhase() {
    return static_cast<GamaPassUiPhase>(gPhase.load(std::memory_order_acquire));
}

const char* GamaPassUiPhaseLabel(GamaPassUiPhase phase) {
    switch (phase) {
        case GamaPassUiPhase::Idle: return "";
        case GamaPassUiPhase::OpeningBrowser: return "登录·开浏览器";
        case GamaPassUiPhase::FillingForm: return "登录·填表";
        case GamaPassUiPhase::TwoFactor: return "登录·二次验证";
        case GamaPassUiPhase::ClickGamaPass: return "登录·点Gama Pass";
        case GamaPassUiPhase::SelectAccount: return "登录·选账号";
        case GamaPassUiPhase::SelectNick: return "登录·选号";
        case GamaPassUiPhase::WaitNgm: return "登录·等NGM";
        case GamaPassUiPhase::WaitClassic: return "登录·等经典版";
        case GamaPassUiPhase::ManualLogin: return "登录·请登录";
        case GamaPassUiPhase::CreateNick: return "登录·请建立昵称";
    }
    return "GAMA PASS登录中";
}

void ResetGamaPassLoginCancel() { gCancel.store(false, std::memory_order_release); }

void RequestGamaPassLoginCancel() { gCancel.store(true, std::memory_order_release); }

bool GamaPassLoginCanceled() { return gCancel.load(std::memory_order_acquire); }

}  // namespace msc::launcher
