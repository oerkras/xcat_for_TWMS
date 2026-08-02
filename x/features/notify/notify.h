#pragma once

#include <cstdint>

namespace x::features::notify {

enum class NotificationKind : uint32_t {
    Info = 0,
    Success = 1,
    Warning = 2,
    Danger = 3,
};

struct NotificationEvent {
    NotificationKind kind = NotificationKind::Info;
    const char* key = "";
    const char* title = "";
    const char* body = "";
    uint32_t ttlMs = 4000;
};

using NotifyKind = NotificationKind;
using NotifySpec = NotificationEvent;

void PublishNotification(const NotificationEvent& event);
void PushNotify(const NotifySpec& spec);
// 立刻撤掉 launcher 上同 key 的通知气泡（测谎通过后取消「正在自动测谎中」等）。
void DismissNotification(const char* key);

}  // namespace x::features::notify
