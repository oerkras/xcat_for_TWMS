#include "lie_log.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <cstdio>

namespace x::features::auto_lie::lie_log {
namespace {

// 与 combat.log 同一套：AppendDbgLogA 内部按路径缓存单句柄、会话首写整代轮转、
// 卷满 512KiB 再轮，多写者共享安全。
void AppendChannel(const char* tag, const char* body) {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return;  // 目录未就绪：这一行只走 x.jsonl
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", bin);
    CreateDirectoryA(dir, nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1100]{};
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%s] %s\r\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, tag ? tag : "?", body ? body : "");
    if (n <= 0) return;
    if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1;
    (void)x::runtime::AppendDbgLogA(dir, "auto_lie.log", line, static_cast<DWORD>(n));
}

}  // namespace

void Line(const char* tag, const char* body) {
    AppendChannel(tag, body);
    x::runtime::LogI(tag, "%s", body);
}

}  // namespace x::features::auto_lie::lie_log
