#pragma once

#include <cstdint>

namespace xcat::app {

// 对齐枫星 5 灯语义。
// 注入前：IPC≈WebView 就绪，GameContext≈进程在；注入后：以后三者以 PayloadStatus SHM 为准。
struct RuntimeLeds {
    bool ipc = false;          // ① WebView 就绪 或 payload ipcHandshake
    bool gameContext = false;  // ② 游戏进程 或 payload gameContextOk
    bool localPlayer = false;  // ③ MyUser / LocalCharacterStat
    bool mapOk = false;        // ④ 显式 playReady（IsPlayReady）
    bool quizCache = false;    // ⑤ 测谎 TypeResolve（UIAntiMacroUtil + 两类 UI）
    unsigned long gamePid = 0;
    uint64_t webReadyTickMs = 0;  // WebView 首次就绪时刻（状态条计时）
    int mapId = 0;
    char currentMapName[128]{};  // 街道名·地图名（UTF-8）
    bool playReady = false;
    bool wmAlive = false;
    int sceneState = -1;  // ports::world::SceneState
};

// prefsBinDir：XCat_data 路径；空则只做注入前探测。
RuntimeLeds QueryRuntimeLeds(const char* prefsBinDir = nullptr);

}  // namespace xcat::app
