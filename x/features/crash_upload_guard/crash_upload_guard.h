// Classic TWMS — 给游戏自带 CrashReporter 的同步上传套上超时，避免它冻住主线程。
#pragma once

namespace x::features::crash_upload_guard {

// 起一个低频后台线程等 CrashReporter.dll 出现，出现即打 IAT 钩子。可重复调用。
void Start();
void Stop();

}  // namespace x::features::crash_upload_guard
