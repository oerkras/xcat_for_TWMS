#pragma once

#include <cstdint>

namespace xcat::app::game_exit_probe {

// ProcessDead 时区分：有 Application Error(1000) 证据 vs 仅进程消失。
enum class Kind : uint8_t {
    Unknown = 0,           // 事件日志不可读 / 探测失败
    NoCrashEvidence = 1,   // 近期无 Maplestory_Classic Application Error
    CrashEvidence = 2,     // 命中 Faulting application = Maplestory_Classic.exe
};

struct Result {
    Kind kind = Kind::Unknown;
    uint32_t faultingPid = 0;       // 事件里带的 PID；0=未解析到
    uint32_t exceptionCode = 0;     // 如 0xC0000005；0=未解析到
    bool pidMatched = false;        // preferPid!=0 且与 faultingPid 一致
    char faultingModule[96]{};
    char detail[220]{};             // 给人看的一行摘要
};

// lookbackSec：只看最近窗口，避免历史崩溃误判。preferPid=0 则不校验 PID。
Result ProbeRecentClassicFault(uint32_t preferPid, uint32_t lookbackSec = 180);

// 给 Watchdog reason= / 顶栏用的短标签（静态字面量，可长期持有）。
const char* ReasonLabel(Kind kind);

}  // namespace xcat::app::game_exit_probe
