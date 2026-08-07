#pragma once

// Classic TWMS / 经典版 — Galaxy PlayerPrefs 只读探针（默认关）。
// 目的：踢线回登录前后核对 Galaxy_UserSessionToken 等是否还在，服务软重进可行性。
//
// 武装（任一即可）：
//   · 首页勾选「软重连试连」→ PayloadControl.softLoginProbe（同时写 galaxyTokenProbe）
//   · 空文件 galaxy_token_probe.on（kick.log / xcat.dll 同目录）— 仅采证、不软重连
//   · 环境变量 GALAXY_TOKEN_PROBE=1 — 同上
//
// 日志：与 kick.log 同目录的 galaxy_token.log；断线边沿也会在 kick.log 留一行摘要。
// 红线：绝不写完整 userSessionToken（只打 len + 前缀）；不写 PlayerPrefs、不清登录态。

namespace x::features::galaxy_token_probe {

void Init();
void Shutdown();

// UI / payload_control 下发；与 marker、env 合并判定 IsArmed。
void SetEnabled(bool on);

// True when UI / marker / env armed.
bool IsArmed();

// Queue a Unity-main-thread PlayerPrefs sample. Safe from kick_sniff worker.
// why: "connected" / "disconnect" / "manual" …
void RequestSample(const char* why);

}  // namespace x::features::galaxy_token_probe
