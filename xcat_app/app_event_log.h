#pragma once

// 启动器本次运行事件：常驻内存环形缓冲，仅展示当前 XCat 进程期间发生的事件。

#include <cstdint>
#include <string>
#include <vector>

namespace xcat::app::eventlog {

enum class Level : uint32_t {
    Info = 0,
    Success = 1,
    Warning = 2,
    Danger = 3,
};

struct Event {
    Level level = Level::Info;
    uint64_t filetime = 0;  // GetSystemTimeAsFileTime()（UTC，100ns since 1601）
    std::string timestamp;  // 本地时间，预格式化给 UI 直接显示。
    std::string key;
    std::string title;
    std::string body;
};

// 初始化本次运行的事件存储；首次拿到有效路径时会清理旧版 events.bin。
void SetStoragePath(const std::string& binDir);

// 记录一条本次运行事件并递增未读计数。kind 取值同 PayloadNotifyKind。
void Record(uint32_t kind, const std::string& key, const std::string& title,
            const std::string& body);

// 倒序（最新在前）拷贝一份快照。
void Snapshot(std::vector<Event>& out);
size_t Count();

uint32_t UnseenCount();
void MarkAllSeen();
void Clear();

bool WindowOpen();
void SetWindowOpen(bool open);
void ToggleWindow();

// 绘制事件窗口（若打开）。在主帧末尾调用。
void DrawWindow(float dpiScale);

}  // namespace xcat::app::eventlog
