// Classic TWMS — 给游戏自带 CrashReporter 的同步上传套上超时，避免它冻住主线程。
// 默认关；应急：XCAT_CRASH_UPLOAD_GUARD=1。
#pragma once

namespace x::features::crash_upload_guard {

// 默认 no-op。仅环境变量 XCAT_CRASH_UPLOAD_GUARD=1 时：后台等 CrashReporter.dll，打 IAT 超时钩。
void Start();
void Stop();

}  // namespace x::features::crash_upload_guard
