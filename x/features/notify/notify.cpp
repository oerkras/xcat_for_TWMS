#include "notify.h"

#include "../../runtime/bin_dir.h"

#include "../../../common/xcat_payload_notify.h"

#include <string>

namespace x::features::notify {
namespace {

const char* SafeText(const char* value) {
    return value ? value : "";
}

std::string EventKey(const NotificationEvent& event) {
    const char* key = SafeText(event.key);
    if (key[0]) return key;
    return SafeText(event.title);
}

}  // namespace

void PublishNotification(const NotificationEvent& event) {
    const char* binDir = x::runtime::GetBinDir();
    if (!binDir || !binDir[0]) return;

    const std::string key = EventKey(event);
    xcat::EnqueuePayloadNotify(binDir, static_cast<uint32_t>(event.kind), key.c_str(),
                               SafeText(event.title), SafeText(event.body), event.ttlMs);
}

void PushNotify(const NotifySpec& spec) {
    PublishNotification(spec);
}

void DismissNotification(const char* key) {
    if (!key || !key[0]) return;
    PublishNotification(NotificationEvent{NotificationKind::Info, key, "", "",
                                          xcat::kPayloadNotifyDismissTtlMs});
}

}  // namespace x::features::notify
