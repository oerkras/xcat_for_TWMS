#pragma once

#include <string>

namespace xcat::app::notify {

void Reset();
void Poll(const std::string& prefsBinDir);
void Draw(float dpiScale);

// 本地通知入口：线程安全，可从后台线程调用。
// kind：0=Info 1=Success 2=Warning 3=Danger；ttlMs=0 用默认。
void PushLocal(unsigned int kind, const char* key, const char* title, const char* body,
               unsigned int ttlMs = 0);

// 通知提示音偏好：user.ini [notify] soundMuted（默认静音；缺键首次 Load 落盘 1）。
void LoadNotifyPrefs(const std::string& prefsBinDir);
bool NotifySoundMuted();
bool SetNotifySoundMuted(const std::string& prefsBinDir, bool muted);

// 顶栏「通知静音」开关；返回是否成功写入 user.ini。
bool DrawTopBarMuteToggle(const std::string& prefsBinDir, float buttonH);
float TopBarMuteToggleWidth(float buttonH);

}  // namespace xcat::app::notify
