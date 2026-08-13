#include "app_sound.h"

#include "xcat_log.h"
#include "xcat_sound.h"

#include <thread>

namespace xcat::app::sound {

void Init() { xcat::sound::Init(); }

void Shutdown() { xcat::sound::Shutdown(); }

void UiClick() { xcat::sound::PlayAsync(xcat::sound::Id::UiClick); }

void UiConfirm() { xcat::sound::PlayAsync(xcat::sound::Id::UiConfirm); }

void UiToggle() { xcat::sound::PlayAsync(xcat::sound::Id::UiToggle); }

void UiError() { xcat::sound::PlayAsync(xcat::sound::Id::UiError); }

void UiShutdownAsync(HWND hwnd, UINT doneMsg) {
    if (!hwnd || !doneMsg) return;
    std::thread([hwnd, doneMsg] {
        xcat::log::Info("App", "graceful exit: shutdown sound");
        // 避免退出音效和前一条异步提示音叠加；播完后补短静音，降低关闭 waveOut 时的点击爆音。
        xcat::sound::Shutdown();
        (void)xcat::sound::PlayBlocking(xcat::sound::Id::UiShutdown);
        (void)xcat::sound::PlaySilenceBlocking(60);
        xcat::log::Info("App", "graceful exit: sound done");
        if (!PostMessageW(hwnd, doneMsg, 0, 0)) {
            xcat::log::Warn("App", "graceful exit: PostMessage failed err=%lu",
                            GetLastError());
        }
    }).detach();
}

void LaunchOk() { xcat::sound::PlayAsync(xcat::sound::Id::LaunchOk); }

void LaunchFail() { xcat::sound::PlayAsync(xcat::sound::Id::LaunchFail); }

void IpcReady() { xcat::sound::PlayAsync(xcat::sound::Id::FeatureReady); }

void GameContextReady() { xcat::sound::PlayAsync(xcat::sound::Id::GameContextOk); }

void Notify() { xcat::sound::PlayAsync(xcat::sound::Id::Notify); }

void Alarm() { xcat::sound::PlayAsync(xcat::sound::Id::Alarm); }

void RestrictionAlarm() { xcat::sound::PlayAsync(xcat::sound::Id::RestrictionAlarm); }

void ScrollDrop() { xcat::sound::PlayAsync(xcat::sound::Id::ScrollDrop); }

}  // namespace xcat::app::sound
