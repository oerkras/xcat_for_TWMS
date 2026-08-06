#pragma once

// MapleStory Classic (TW) · NGM 启动骨架
// 对照：xcat_for_fengxing/launcher/{launcher_core,process_util,launcher_run}
// 差异：passarg 不是 /gameid+/publishid+/sid，而是 Galaxy 换票四元组。
//
// 实机 cmdline（2026-07-30 · PID 采证）：
//   "…\Maplestory_Classic.exe" <userObjectID> <userSessionToken> <gid> <galaxy_GameId>
// NGM 把 -passarg 拆成 exe 后的 4 个位置参数（不是保留 -passarg: 开关）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace msc::launcher {

enum class Stage {
    Init,
    FindingNgm,
    LaunchingGame,
    WaitingForGame,
    Done,
    Failed,
    BlockedNeedsTicket,  // 缺票或票字段非法（含引号等）
};

enum class NgmMode {
    Launch,   // 官网 JS LaunchGameOnlyPassArgs 默认
    Restore,  // 老项目枫星现行；经典版是否适用待实机验证
};

// 来自 POST /api/Login/GetOneTimeWebInfo 成功体（字段名与官网 float-btns.js 一致）
struct GalaxyTicket {
    std::wstring userObjectId;      // ottRes.data.userObjectID
    std::wstring userSessionToken;  // ottRes.data.userSessionToken
    std::wstring gid;               // ottRes.data.gid
    std::wstring galaxyGameId;      // ottRes.data.galaxy_GameId
    std::wstring ngmGameId;         // ottRes.data.game → NGM -game:'...'
};

// 从进程 cmdline 解析出的四元组（无 exe 路径）
struct ClassicPassArgs {
    std::wstring userObjectId;
    std::wstring userSessionToken;
    std::wstring gid;
    std::wstring galaxyGameId;
    bool ok = false;
};

struct Options {
    GalaxyTicket ticket;
    NgmMode      mode = NgmMode::Launch;
    std::wstring gameExeName = L"Maplestory_Classic.exe";
    int          waitGameSec = 90;
    bool         dryRunDeepLinkOnly = false;
    // 策略 B：已有经典版则跳过 NGM、直接接管 PID 注入。
    // 优先 cmdline 四元组匹配本票；否则接管任一存活实例（官网 CDP 常已先拉起且票可能不同）。
    // 仅建议 GAMA PASS 等「浏览器可能已自启」路径打开；HTTP 冷启默认 false，避免误接管残留进程。
    bool attachExistingClassic = false;
    // 接管时的最大进程年龄（秒）：更老的实例按「上一局残留」处理，不接管（0=不限）。
    // 换票登录刚完成时，本局客户端必然远新于此窗口。
    int attachMaxAgeSec = 1800;
};

struct Progress {
    Stage         stage = Stage::Init;
    std::string   message;
    unsigned long gamePid = 0;
};

struct Result {
    bool          ok = false;
    Stage         finalStage = Stage::Init;
    std::string   errorMessage;
    unsigned long gamePid = 0;
    std::string   deepLinkSummary;  // 脱敏
    std::string   cmdLineSummary;   // 脱敏后的游戏 cmdline
};

using ProgressCallback = std::function<void(const Progress&)>;

std::wstring BuildNgmPassarg(const GalaxyTicket& t);
std::wstring BuildNgmDeepLink(const GalaxyTicket& t, NgmMode mode);

bool TicketLooksUsable(const GalaxyTicket& t);
std::string FormatDeepLinkForLog(const std::wstring& deepLink);
std::string FormatCmdLineForLog(const std::wstring& cmdLine);

// Win10+：NtQueryInformationProcess(ProcessCommandLineInformation=60)
// 仅需 PROCESS_QUERY_LIMITED_INFORMATION（本机 PEB ReadProcessMemory 会被拒）
std::wstring GetProcessCommandLineW(DWORD pid);

std::vector<std::wstring> SplitCommandLineArgs(const std::wstring& cmdLine);
ClassicPassArgs ParseClassicPassArgs(const std::wstring& cmdLine);
bool CmdMatchesGalaxyTicket(const std::wstring& cmdLine, const GalaxyTicket& ticket);

std::wstring FindNgmPath();  // 多路径：进程 / ngm:// / 固定目录 / 注册表 / 经典版旁路
bool         IsNgmProcessRunning();
// 仅认创建时间 ≥ notBefore 的 NGM（滤残留进程；notBefore 可为零=等同 IsNgmProcessRunning）
bool         IsNgmProcessRunningCreatedAfter(const FILETIME& notBefore);
bool         EnsureNgmRunning();

// 查找可接管的经典版 PID。ticketMatched=true 表示 cmdline 与 ticket 四元组一致。
// createdNotBefore 非空时：只认创建时间 ≥ *createdNotBefore 的进程（滤残留）。
// outUnmatched：存活且通过年龄过滤、但 cmdline 未匹配本票的实例数；>1 说明无法判定
// 哪个属于本次登录，调用方不应猜测接管（会串到别的账号）。
unsigned long FindExistingClassicPid(const GalaxyTicket& ticket,
                                     const wchar_t* exeName = L"Maplestory_Classic.exe",
                                     std::wstring* outCmd = nullptr,
                                     bool* ticketMatched = nullptr,
                                     const FILETIME* createdNotBefore = nullptr,
                                     int* outUnmatched = nullptr);

Result Run(const Options& opts, ProgressCallback cb = nullptr);

}  // namespace msc::launcher
