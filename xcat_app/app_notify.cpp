#include "app_notify.h"

#include "app_dpi.h"
#include "app_event_log.h"
#include "app_sound.h"
#include "app_theme.h"
#include "imgui_log_sanitize.h"

#include "process_util.h"
#include "xcat_config_ini.h"
#include "xcat_payload_notify.h"
#include "xcat_scroll_voice.h"

#include "imgui.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xcat::app::notify {
namespace {

constexpr uint32_t kDefaultTtlMs = 4200;
constexpr size_t kMaxNotifications = 5;
constexpr uint32_t kRepeatSuppressMs = 30000;
constexpr uint32_t kNotifySoundThrottleMs = 800;
constexpr float kNotifyBubbleScale = 1.15f;

enum class Kind : uint32_t {
    Info = 0,
    Success = 1,
    Warning = 2,
    Danger = 3,
};

struct QueuedNotification {
    Kind kind = Kind::Info;
    std::string key;
    std::string title;
    std::string body;
    uint32_t ttlMs = kDefaultTtlMs;
};

struct NotifyItem {
    Kind kind = Kind::Info;
    std::string key;
    std::string title;
    std::string body;
    uint32_t createdMs = 0;
    uint32_t updatedMs = 0;
    uint32_t ttlMs = kDefaultTtlMs;
};

struct RecentNotifySignature {
    Kind kind = Kind::Info;
    std::string key;
    std::string title;
    std::string body;
    uint32_t lastMs = 0;
};

struct NotifyLayout {
    float w = 0.f;
    float h = 0.f;
    float x = 0.f;
    float textX = 0.f;
    float textWrapW = 0.f;
    float titleY = 0.f;
    float bodyY = 0.f;
    bool showBody = false;
};

std::vector<NotifyItem> g_items;
std::vector<RecentNotifySignature> g_recentSignatures;
std::vector<QueuedNotification> g_pending;
std::mutex g_externalMtx;
std::vector<QueuedNotification> g_external;
uint32_t g_lastNotifySoundMs = 0;
uint32_t g_lastPayloadNotifySeq = 0;
uint32_t g_lastPayloadNotifyEpoch = 0;
bool g_notifyBacklogSkipped = false;
std::atomic<bool> g_notifySoundMuted{true};
// 用户点击关闭后短抑制：同 key 在窗内不因 payload 刷新立刻弹回；到期后可再亮。
constexpr uint32_t kUserDismissSuppressMs = 10000;
std::unordered_map<std::string, uint32_t> g_userDismissUntil;

bool WriteNotifySoundMuted(const std::string& prefsBinDir, bool muted) {
    if (prefsBinDir.empty()) return false;
    const std::string path = xcat::UserConfigIniPath(prefsBinDir.c_str());
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(xcat::Utf8ToWide(path)).parent_path(), ec);
    if (ec) return false;

    return xcat::UpdateIniFile(path.c_str(), [&](xcat::IniStore& ini) {
        xcat::IniSetU32(ini, "meta", "version", static_cast<uint32_t>(xcat::kUserConfigIniVersion));
        xcat::IniSetU32(ini, "notify", "version", 1u);
        xcat::IniSetBool(ini, "notify", "soundMuted", muted);
        xcat::IniSetU64(ini, "notify", "writeTickMs", GetTickCount64());
    });
}

bool IsPayloadOwnedAlarmKey(const std::string& key) {
    // 测谎进行中/测试报警：专用 Alarm 由 payload 周期播放，面板只负责气泡，避免双进程叠播。
    // GM/隐身：同模式（payload 播 Alarm，不受通知静音影响）。
    return key == "auto-lie-detected" || key == "auto-lie-alarm-test" ||
           key == "encounter-gm-threat" || key == "encounter-gm-hop";
}

bool IsRepeatExemptNotifyKey(const std::string& key) {
    // 清怪/主动软重连一轮 15–20s，短于 30s 签名去重；不豁免则第二轮 fire 有日志没气泡。
    return key == "mob-gather-clear" || key == "mob-gather-soft" || key == "soft-login-ok";
}

bool IsScrollDropNotifyKey(const std::string& key) {
    return key.rfind("petloot-scroll", 0) == 0;
}

bool IsPickupSuccessNotifyKey(const std::string& key) {
    return key.rfind("petloot-picked", 0) == 0;
}

bool IsHighValueDropNotifyKey(const std::string& key) {
    return IsScrollDropNotifyKey(key) || key.rfind("petloot-equip", 0) == 0 ||
           IsPickupSuccessNotifyKey(key);
}

bool IsUnmutableCriticalAlarmKey(const std::string& key) {
    // 测谎/GM/限制 Debuff：强制响；装备/卷軸掉落叮咚同策略（不被通知静音关掉）
    return IsPayloadOwnedAlarmKey(key) || key == "restriction-suspicious-debuff" ||
           IsHighValueDropNotifyKey(key);
}

bool IsUserDismissSuppressed(const std::string& key, uint32_t now) {
    const auto it = g_userDismissUntil.find(key);
    if (it == g_userDismissUntil.end()) return false;
    if (static_cast<int>(now - it->second) >= 0) {
        g_userDismissUntil.erase(it);
        return false;
    }
    return true;
}

void MarkUserDismissed(const std::string& key, uint32_t now) {
    if (key.empty()) return;
    g_userDismissUntil[key] = now + kUserDismissSuppressMs;
}

void ClearUserDismiss(const std::string& key) {
    if (key.empty()) return;
    g_userDismissUntil.erase(key);
}

uint32_t NowMs() { return GetTickCount(); }

ImU32 ColorForKind(Kind kind, float alpha) {
    const int a = static_cast<int>(std::clamp(alpha, 0.f, 1.f) * 255.f);
    switch (kind) {
    case Kind::Success: return IM_COL32(64, 220, 128, a);
    case Kind::Warning: return IM_COL32(255, 188, 64, a);
    case Kind::Danger: return IM_COL32(255, 92, 86, a);
    case Kind::Info:
    default: return IM_COL32(98, 178, 255, a);
    }
}

bool SameNotification(const NotifyItem& item, const QueuedNotification& queued, uint32_t ttlMs) {
    return item.kind == queued.kind && item.title == queued.title && item.body == queued.body &&
           item.ttlMs == ttlMs;
}

bool SameSignature(const RecentNotifySignature& recent, Kind kind, const std::string& key,
                   const std::string& title, const std::string& body) {
    return recent.kind == kind && recent.key == key && recent.title == title && recent.body == body;
}

void PruneRecentSignatures(uint32_t now) {
    g_recentSignatures.erase(
        std::remove_if(g_recentSignatures.begin(), g_recentSignatures.end(),
                       [now](const RecentNotifySignature& recent) {
                           return now - recent.lastMs > kRepeatSuppressMs;
                       }),
        g_recentSignatures.end());
}

bool WasRecentlyShown(Kind kind, const std::string& key, const std::string& title,
                      const std::string& body, uint32_t now) {
    PruneRecentSignatures(now);
    for (const RecentNotifySignature& recent : g_recentSignatures) {
        if (SameSignature(recent, kind, key, title, body)) return true;
    }
    return false;
}

void RememberShown(Kind kind, const std::string& key, const std::string& title,
                   const std::string& body, uint32_t now) {
    PruneRecentSignatures(now);
    for (RecentNotifySignature& recent : g_recentSignatures) {
        if (SameSignature(recent, kind, key, title, body)) {
            recent.lastMs = now;
            return;
        }
    }
    RecentNotifySignature recent{};
    recent.kind = kind;
    recent.key = key;
    recent.title = title;
    recent.body = body;
    recent.lastMs = now;
    g_recentSignatures.emplace_back(std::move(recent));
}

int ParseScrollItemIdFromBody(const std::string& body) {
    // fillName: "品名（2040001）"；批量取第一只。
    const std::string lp = "\xEF\xBC\x88";
    const std::string rp = "\xEF\xBC\x89";
    const size_t a = body.find(lp);
    if (a == std::string::npos) return 0;
    const size_t b = body.find(rp, a + lp.size());
    if (b == std::string::npos || b <= a + lp.size()) return 0;
    const std::string num = body.substr(a + lp.size(), b - (a + lp.size()));
    int id = 0;
    for (char c : num) {
        if (c < '0' || c > '9') return 0;
        id = id * 10 + (c - '0');
        if (id > 99999999) return 0;
    }
    return id;
}

void PlayNotifySound(uint32_t now, const std::string& key, const std::string& body) {
    if (g_notifySoundMuted.load(std::memory_order_acquire) &&
        !IsUnmutableCriticalAlarmKey(key)) {
        return;
    }
    // payload 已周期播 Alarm：面板侧跳过，只保留 restriction 等面板专责音。
    if (IsPayloadOwnedAlarmKey(key)) return;
    if (IsPickupSuccessNotifyKey(key)) {
        const int itemId = ParseScrollItemIdFromBody(body);
        xcat::sound::PlayPickupSuccessAnnounce(itemId);
        return;
    }
    if (IsScrollDropNotifyKey(key) || key.rfind("petloot-equip", 0) == 0) {
        // 不节流丢弃：口播走 Interrupt，密掉只留最新一条。静音开关管不住（见 Unmutable）。
        const int itemId = ParseScrollItemIdFromBody(body);
        xcat::sound::PlayScrollDropAnnounce(itemId);
        return;
    }
    if (g_lastNotifySoundMs != 0 &&
        static_cast<int>(now - g_lastNotifySoundMs) < static_cast<int>(kNotifySoundThrottleMs)) {
        return;
    }
    g_lastNotifySoundMs = now;
    if (key == "restriction-suspicious-debuff")
        sound::RestrictionAlarm();
    else
        sound::Notify();
}

void PruneExpiredNotifications(uint32_t now) {
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(),
                                 [now](const NotifyItem& item) {
                                     return now - item.updatedMs > item.ttlMs + 500u;
                                 }),
                  g_items.end());
}

void EnqueueNotification(const QueuedNotification& queued) {
    const uint32_t now = NowMs();
    PruneExpiredNotifications(now);

    const std::string& key = queued.key.empty() ? queued.title : queued.key;
    if (queued.ttlMs == xcat::kPayloadNotifyDismissTtlMs) {
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(),
                                     [&key](const NotifyItem& item) { return item.key == key; }),
                      g_items.end());
        ClearUserDismiss(key);
        return;
    }
    // 用户刚点掉：短抑制内不因同 key 刷新立刻弹回（替代把整段卡死在 WasRecentlyShown 30s）。
    if (IsUserDismissSuppressed(key, now)) return;

    const uint32_t ttlMs = queued.ttlMs ? queued.ttlMs : kDefaultTtlMs;
    for (size_t i = 0; i < g_items.size(); ++i) {
        auto& item = g_items[i];
        if (item.key == key) {
            if (SameNotification(item, queued, ttlMs)) {
                item.updatedMs = now;
                return;
            }
            item.kind = queued.kind;
            item.title = queued.title;
            item.body = queued.body;
            item.updatedMs = now;
            item.ttlMs = ttlMs;
            RememberShown(item.kind, item.key, item.title, item.body, now);
            eventlog::Record(static_cast<uint32_t>(item.kind), item.key, item.title, item.body);
            PlayNotifySound(now, item.key, item.body);
            if (i != 0) {
                NotifyItem updated = std::move(item);
                g_items.erase(g_items.begin() + static_cast<std::ptrdiff_t>(i));
                g_items.insert(g_items.begin(), std::move(updated));
            }
            return;
        }
    }

    // 持续刷新的测谎关键气泡：不走 30s 签名去重（否则点关后长时间无法再亮）。
    // 清怪/主动软重连：轮次短于 30s，同样必须每轮都能亮。
    if (!IsPayloadOwnedAlarmKey(key) && !IsRepeatExemptNotifyKey(key) &&
        WasRecentlyShown(queued.kind, key, queued.title, queued.body, now)) {
        return;
    }

    NotifyItem item{};
    item.kind = queued.kind;
    item.key = key;
    item.title = queued.title;
    item.body = queued.body;
    item.createdMs = now;
    item.updatedMs = now;
    item.ttlMs = ttlMs;
    RememberShown(item.kind, item.key, item.title, item.body, now);
    eventlog::Record(static_cast<uint32_t>(item.kind), item.key, item.title, item.body);
    PlayNotifySound(now, item.key, item.body);
    g_items.insert(g_items.begin(), std::move(item));
    if (g_items.size() > kMaxNotifications) g_items.resize(kMaxNotifications);
}

void DrainPayloadNotifyIpc(const std::string& binDir) {
    if (binDir.empty()) return;
    xcat::PayloadNotifyEvent batch[16]{};
    const size_t n =
        xcat::DrainPayloadNotify(binDir.c_str(), &g_lastPayloadNotifySeq, batch, 16);
    for (size_t i = 0; i < n; ++i) {
        QueuedNotification queued{};
        const unsigned int k = batch[i].kind > static_cast<uint32_t>(Kind::Danger)
                                   ? static_cast<unsigned int>(Kind::Info)
                                   : batch[i].kind;
        queued.kind = static_cast<Kind>(k);
        queued.key = batch[i].key;
        queued.title = batch[i].title;
        queued.body = batch[i].body;
        queued.ttlMs = batch[i].ttlMs ? batch[i].ttlMs : kDefaultTtlMs;
        g_pending.emplace_back(std::move(queued));
    }
}

void SyncAndSkipNotifyBacklog(const std::string& binDir) {
    if (binDir.empty()) return;
    uint32_t epoch = 0;
    if (xcat::PeekPayloadNotifyEpoch(binDir.c_str(), &epoch)) {
        if (epoch != g_lastPayloadNotifyEpoch) {
            g_lastPayloadNotifyEpoch = epoch;
            g_lastPayloadNotifySeq = 0;
            g_notifyBacklogSkipped = false;
        }
    }
    // 首次接通：跳过历史积压，只收之后新事件（对照仓同策略）。
    if (!g_notifyBacklogSkipped) {
        xcat::SkipPayloadNotifyBacklog(binDir.c_str(), &g_lastPayloadNotifySeq);
        g_notifyBacklogSkipped = true;
    }
}

void DrainPending() {
    for (const QueuedNotification& queued : g_pending) {
        EnqueueNotification(queued);
    }
    g_pending.clear();
}

void DrainExternal() {
    std::vector<QueuedNotification> batch;
    {
        std::lock_guard<std::mutex> lk(g_externalMtx);
        if (g_external.empty()) return;
        batch.swap(g_external);
    }
    for (auto& queued : batch) {
        g_pending.emplace_back(std::move(queued));
    }
}

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

NotifyLayout ComputeNotifyLayout(const NotifyItem& item, const ImVec2& display, float s, bool compact,
                                 float displayMaxW, ImFont* font, float fontSize) {
    const float padL = 32.f * s;
    const float padR = 16.f * s;
    const float padT = compact ? 10.f * s : 9.f * s;
    const float padB = compact ? 10.f * s : 10.f * s;
    const float titleBodyGap = 4.f * s;
    const float minBubbleW = 160.f * s;
    const float maxBubbleW =
        compact ? std::min(280.f * s, displayMaxW) : std::min(520.f * s, displayMaxW);
    const float maxTextWrapW = std::max(32.f * s, maxBubbleW - padL - padR);

    const char* title = item.title.c_str();
    const char* body = item.body.c_str();
    const bool showBody = !compact && body[0] != '\0';

    ImVec2 titleSize = font->CalcTextSizeA(fontSize, FLT_MAX, maxTextWrapW, title);
    ImVec2 bodySize =
        showBody ? font->CalcTextSizeA(fontSize, FLT_MAX, maxTextWrapW, body) : ImVec2(0.f, 0.f);

    float contentW = titleSize.x;
    if (showBody) contentW = std::max(contentW, bodySize.x);
    const float w = std::clamp(contentW + padL + padR, minBubbleW, maxBubbleW);
    const float textWrapW = std::max(32.f * s, w - padL - padR);

    if (textWrapW + 0.5f < maxTextWrapW) {
        titleSize = font->CalcTextSizeA(fontSize, FLT_MAX, textWrapW, title);
        if (showBody) bodySize = font->CalcTextSizeA(fontSize, FLT_MAX, textWrapW, body);
    }

    float contentH = titleSize.y;
    if (showBody) contentH += titleBodyGap + bodySize.y;

    const float minH = std::max(28.f * s, fontSize + padT + padB);
    const float h = std::max(minH, contentH + padT + padB);

    NotifyLayout layout{};
    layout.w = w;
    layout.h = h;
    layout.x = (display.x - w) * 0.5f;
    layout.textX = layout.x + padL;
    layout.textWrapW = textWrapW;
    layout.titleY = padT;
    layout.bodyY = padT + titleSize.y + titleBodyGap;
    layout.showBody = showBody;
    return layout;
}

float NotifyAlpha(const NotifyItem& item, uint32_t now) {
    const uint32_t age = now - item.updatedMs;
    const float inT = EaseOutCubic(std::min(age / 220.f, 1.f));
    const uint32_t remaining = item.ttlMs > age ? item.ttlMs - age : 0;
    const float outT = remaining < 450 ? remaining / 450.f : 1.f;
    return std::clamp(inT * outT, 0.f, 1.f);
}

void DrawOne(ImDrawList* dl, const NotifyItem& item, const NotifyLayout& layout, float y, float alpha,
             float s, ImFont* font, float fontSize) {
    if (alpha <= 0.01f) return;

    const ImVec2 p0(layout.x, y);
    const ImVec2 p1(layout.x + layout.w, y + layout.h);
    const float radius = std::min(layout.h * 0.5f, layout.w * 0.5f);

    const auto& pal = xcat::app::AppTheme_Palette();
    auto withAlpha = [alpha](const ImVec4& c) -> ImU32 {
        return IM_COL32(static_cast<int>(c.x * 255.f), static_cast<int>(c.y * 255.f),
                        static_cast<int>(c.z * 255.f), static_cast<int>(c.w * 255.f * alpha));
    };
    dl->AddRectFilled(ImVec2(p0.x + 2.f * s, p0.y + 5.f * s), ImVec2(p1.x + 2.f * s, p1.y + 5.f * s),
                      withAlpha(pal.toastShadow), radius);
    dl->AddRectFilled(p0, p1, withAlpha(pal.toastBg), radius);
    dl->AddRect(p0, p1, withAlpha(pal.toastBorder), radius, 0, 1.1f * s);

    const ImU32 accent = ColorForKind(item.kind, alpha);
    const float dotR = 5.f * s;
    dl->AddCircleFilled(ImVec2(p0.x + 18.f * s, p0.y + layout.h * 0.5f), dotR, accent, 18);

    const ImU32 titleCol = withAlpha(pal.toastTitle);
    const ImU32 bodyCol = withAlpha(pal.toastBody);
    dl->AddText(font, fontSize, ImVec2(layout.textX, p0.y + layout.titleY), titleCol, item.title.c_str(),
                nullptr, layout.textWrapW);
    if (layout.showBody) {
        dl->AddText(font, fontSize, ImVec2(layout.textX, p0.y + layout.bodyY), bodyCol, item.body.c_str(),
                    nullptr, layout.textWrapW);
    }
}

void DismissItemByKey(const std::string& key) {
    if (key.empty()) return;
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(),
                                 [&key](const NotifyItem& item) { return item.key == key; }),
                  g_items.end());
    MarkUserDismissed(key, NowMs());
}

}  // namespace

void LoadNotifyPrefs(const std::string& prefsBinDir) {
    if (prefsBinDir.empty()) {
        g_notifySoundMuted.store(true, std::memory_order_release);
        return;
    }
    xcat::IniStore ini{};
    const std::string path = xcat::UserConfigIniPath(prefsBinDir.c_str());
    const bool loaded = xcat::LoadIniFile(path.c_str(), ini);
    bool muted = true;
    const bool hasKey = loaded && xcat::IniGetBool(ini, "notify", "soundMuted", muted);
    if (!hasKey) {
        muted = true;
        (void)WriteNotifySoundMuted(prefsBinDir, true);
    }
    g_notifySoundMuted.store(muted, std::memory_order_release);
}

bool NotifySoundMuted() { return g_notifySoundMuted.load(std::memory_order_acquire); }

bool SetNotifySoundMuted(const std::string& prefsBinDir, bool muted) {
    if (!WriteNotifySoundMuted(prefsBinDir, muted)) return false;
    g_notifySoundMuted.store(muted, std::memory_order_release);
    return true;
}

float TopBarMuteToggleWidth(float buttonH) {
    (void)buttonH;
    const bool muted = NotifySoundMuted();
    const char* label = muted ? "静音" : "有声";
    const ImVec2 text = ImGui::CalcTextSize(label);
    return std::max(AppDpi_Px(26.f), text.x + ImGui::GetStyle().FramePadding.x * 2.f);
}

bool DrawTopBarMuteToggle(const std::string& prefsBinDir, float buttonH) {
    const bool muted = NotifySoundMuted();
    const char* label = muted ? "静音" : "有声";
    const float btnW = TopBarMuteToggleWidth(buttonH);
    if (prefsBinDir.empty()) ImGui::BeginDisabled();
    if (muted) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.22f, 0.10f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.30f, 0.14f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.36f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.86f, 0.48f, 1.f));
    }
    const bool clicked = ImGui::Button(label, ImVec2(btnW, buttonH));
    if (muted) ImGui::PopStyleColor(4);
    if (prefsBinDir.empty()) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("未配置偏好目录，无法保存通知静音设置。");
        }
        return false;
    }
    if (!clicked) {
        ImGui::SetItemTooltip(muted ? "普通通知提示音已关闭；关键警报仍会响。"
                                    : "点击关闭普通通知提示音；测谎/GM/限制 Debuff/卷軸掉落无法静音。");
        return false;
    }
    if (!SetNotifySoundMuted(prefsBinDir, !muted)) return false;
    sound::UiToggle();
    return true;
}

void Reset() {
    g_items.clear();
    g_recentSignatures.clear();
    g_pending.clear();
    {
        std::lock_guard<std::mutex> lk(g_externalMtx);
        g_external.clear();
    }
    g_lastNotifySoundMs = 0;
    g_lastPayloadNotifySeq = 0;
    g_lastPayloadNotifyEpoch = 0;
    g_notifyBacklogSkipped = false;
    g_userDismissUntil.clear();
}

void Poll(const std::string& prefsBinDir) {
    if (!prefsBinDir.empty()) eventlog::SetStoragePath(prefsBinDir);
    SyncAndSkipNotifyBacklog(prefsBinDir);
    DrainPayloadNotifyIpc(prefsBinDir);
    DrainExternal();
    DrainPending();
}

void PushLocal(unsigned int kind, const char* key, const char* title, const char* body,
               unsigned int ttlMs) {
    QueuedNotification queued{};
    const unsigned int k = kind > static_cast<unsigned int>(Kind::Danger)
                               ? static_cast<unsigned int>(Kind::Info)
                               : kind;
    queued.kind = static_cast<Kind>(k);
    queued.key = key && key[0] ? key : (title ? title : "");
    queued.title = title ? title : "";
    queued.body = body ? body : "";
    queued.ttlMs = ttlMs ? ttlMs : kDefaultTtlMs;
    std::lock_guard<std::mutex> lk(g_externalMtx);
    g_external.emplace_back(std::move(queued));
}

void Draw(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x <= 1.f || display.y <= 1.f) return;

    const uint32_t now = NowMs();
    PruneExpiredNotifications(now);
    if (g_items.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = font->FontSize;
    const float s = (dpiScale > 0.f ? dpiScale : AppDpi_Scale()) * kNotifyBubbleScale;
    const float displayMaxW = std::max(160.f * s, display.x - 40.f * s);
    const float topSafe = 18.f * s;
    const float stackGap = 8.f * s;

    const int n = std::min<int>(static_cast<int>(g_items.size()), 3);
    float y = topSafe;
    std::vector<std::string> clickedKeys;
    clickedKeys.reserve(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        const NotifyItem& item = g_items[static_cast<size_t>(i)];
        const float alpha = NotifyAlpha(item, now);
        if (alpha <= 0.01f) continue;

        NotifyItem vis = item;
        vis.title = xcat::app::SanitizeImGuiLogLine(item.title);
        vis.body = xcat::app::SanitizeImGuiLogLine(item.body);
        const NotifyLayout layout =
            ComputeNotifyLayout(vis, display, s, false, displayMaxW, font, fontSize);
        const uint32_t age = now - item.updatedMs;
        const float inT = EaseOutCubic(std::min(age / 220.f, 1.f));
        const float drawY = y - (1.f - inT) * 18.f * s;
        DrawOne(dl, vis, layout, drawY, alpha, s, font, fontSize);

        // 透明命中窗：TTL 自动消失之外，点击气泡也可关掉。
        char wndId[64]{};
        snprintf(wndId, sizeof(wndId), "##xcat_notify_hit_%d", i);
        ImGui::SetNextWindowPos(ImVec2(layout.x, drawY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(layout.w, layout.h), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.f);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        if (ImGui::Begin(wndId, nullptr, flags)) {
            ImGui::InvisibleButton("##dismiss", ImVec2(layout.w, layout.h));
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("点击关闭");
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                clickedKeys.push_back(item.key);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        y += layout.h + stackGap;
    }

    for (const std::string& key : clickedKeys) DismissItemByKey(key);
}

}  // namespace xcat::app::notify
