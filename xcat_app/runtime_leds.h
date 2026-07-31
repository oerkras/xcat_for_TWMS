#pragma once

#include <cstdint>

namespace xcat::app {

// 对齐枫星 5 灯语义；Classic 注入前：部分灯由 WebView/进程探测点亮，其余为设计占位
struct RuntimeLeds {
    bool ipc = false;          // ① IPC / WebView 会话就绪
    bool gameContext = false;  // ② 游戏进程 / GameContext
    bool localPlayer = false;  // ③ LocalPlayer（待注入）
    bool mapOk = false;        // ④ Map（待注入）
    bool quizCache = false;    // ⑤ 测谎缓存位置（待接入）
    unsigned long gamePid = 0;
    uint64_t webReadyTickMs = 0;  // WebView 首次就绪时刻（状态条计时）
};

RuntimeLeds QueryRuntimeLeds();

}  // namespace xcat::app
