#include "lie_stats.h"

#include "lie_log.h"
#include "../kick_sniff/kick_sniff.h"
#include "../ports/user_pool_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace x::features::auto_lie::lie_stats {
namespace {

constexpr int kMaxRows = 128;  // 角色数 × 2 题型；满了就不再新增，避免异常名把表撑爆

std::mutex gMu;  // follower 的埋点可能落在帧脉冲线程上，与 worker 并发
bool gLoaded = false;
bool gSuppressed = false;
std::vector<Row> gRows;

// 每题一闩：Seen 时上闩，首个 Outcome 落闸，Closed 时若仍开着补记 missed。
bool gPending[2]{};

// 同题去重：记上次记账的题目实例与时刻。同一实例在此窗口内重开算同一道题。
// 真题两道之间的间隔是几十分钟量级，60 秒不会把连发的新题误并掉。
constexpr DWORD kSameQuizWindowMs = 60000;
uint64_t gLastToken[2]{};
DWORD gLastTouchMs[2]{};

// 上一笔落的是不是 missed。用来回滚「答完之前面板被关、随后同一实例重开并答成」这一种误记：
// 那种情形下 NotifyClosed 先把题判死，真正的 answer-sent 反而被 gPending 闸吞掉。
// 实证：BIN aa29bc 08-10 22:45，samples=278/330 时 ui-closed 记 missed，250ms 后面板重开、
// samples 从 291 接着走到 330/330 交卷 —— 那题其实答成了，表上却挂着 missed。
bool gLastMissed[2]{};
char gLastMissedName[2][kNameLen]{};
// 上一笔未答是不是掉线那种：回滚时要知道该扣哪个计数器。
bool gLastDropped[2]{};

// 开题那一刻的断线序号。结算时若已变，说明这道题从弹出到收尾之间断过线，
// 未答的账要算在网络头上而不是我们头上（见 ApplyOutcomeLocked）。
// DisconnectSeq 是 relaxed 原子、单调递增，任意线程直接读，不必同步。
uint32_t gSeenDiscSeq[2]{};

SnapshotFn gSnapshot[2]{};

const char* KindTag(Kind k) { return k == Kind::Mouse ? "mouse" : "text"; }

// 统计这条线的日志全走分流频道：x.jsonl 会被高频通道冲掉，auto_lie.log 留得住。
void LogLine(const char* fmt, ...) {
    char buf[400]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lie_log::Line("AutoLie", buf);
}

std::string StatsPath() {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return {};
    return std::string(bin) + "state\\lie_stats.tsv";
}

// 纯偏移读（pool+0x10 与 CharacterName backing），worker 上安全；不碰托管调用。
//
// 必须先自己确认 pool 已绑：PeekUserLocal 在 gPool 为空时会替你走 ResolveUserPool，
// 那里面有 FindClass 和 il2cpp_runtime_class_init —— 按仓规只许在主泵上做。
// 记账全在 auto_lie 的 worker 上跑（follower::Tick / auto_lie::TickImpl，泵上的
// LieFramePulse 一个统计埋点都没有），所以这条路绝不能放它去 Resolve。
// 真机上 pool 早被泵上的模块绑好了，这个前置检查平时不拦任何东西，只堵住冷启动
// 那一小段窗口；取不到就记 (unknown)，一个名字不值得拿主线程冒险。
bool ReadLocalCharName(char* out, int outSz) {
    if (!out || outSz <= 0) return false;
    out[0] = '\0';
    if (!x::features::ports::user_pool::PeekUserPool()) return false;
    __try {
        void* local = x::features::ports::user_pool::PeekUserLocal();
        if (!local) return false;
        if (!x::features::ports::user_pool::ReadUserCharacterName(local, out, outSz)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

void CurrentName(char* out, int outSz) {
    if (!ReadLocalCharName(out, outSz)) {
        strncpy_s(out, static_cast<size_t>(outSz), "(unknown)", _TRUNCATE);
        return;
    }
    // 制表符/换行会把 TSV 撕开；角色名理论上不含，异常值就地消毒。
    for (char* p = out; *p; ++p) {
        if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
    }
}

void LoadLocked() {
    if (gLoaded) return;
    const std::string path = StatsPath();
    // bin 目录还没就绪：别置已载标记，否则后续记账会拿空表覆盖掉已有战绩。
    if (path.empty()) return;
    gLoaded = true;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ss(line);
        std::string name, kind, seen, ans, missed, to, last, passed, failed, dropped;
        if (!std::getline(ss, name, '\t')) continue;
        if (!std::getline(ss, kind, '\t')) continue;
        if (!std::getline(ss, seen, '\t')) continue;
        if (!std::getline(ss, ans, '\t')) continue;
        if (!std::getline(ss, missed, '\t')) continue;
        if (!std::getline(ss, to, '\t')) continue;
        if (!std::getline(ss, last, '\t')) last = "0";
        // 旧版的表没有后面这几列（服端判定、掉线是后加的），读不到就当 0。
        if (!std::getline(ss, passed, '\t')) passed = "0";
        if (!std::getline(ss, failed, '\t')) failed = "0";
        if (!std::getline(ss, dropped, '\t')) dropped = "0";
        if (name.empty() || static_cast<int>(gRows.size()) >= kMaxRows) continue;
        Row r{};
        strncpy_s(r.name, name.c_str(), _TRUNCATE);
        r.kind = (kind == "mouse") ? Kind::Mouse : Kind::Text;
        r.seen = static_cast<uint32_t>(strtoul(seen.c_str(), nullptr, 10));
        r.answered = static_cast<uint32_t>(strtoul(ans.c_str(), nullptr, 10));
        r.missed = static_cast<uint32_t>(strtoul(missed.c_str(), nullptr, 10));
        r.timeout = static_cast<uint32_t>(strtoul(to.c_str(), nullptr, 10));
        r.lastSec = _strtoi64(last.c_str(), nullptr, 10);
        r.passed = static_cast<uint32_t>(strtoul(passed.c_str(), nullptr, 10));
        r.failed = static_cast<uint32_t>(strtoul(failed.c_str(), nullptr, 10));
        r.dropped = static_cast<uint32_t>(strtoul(dropped.c_str(), nullptr, 10));
        gRows.push_back(r);
    }
}

void SaveLocked() {
    const std::string path = StatsPath();
    if (path.empty()) return;
    std::string body =
        "#ver\t3\n#char\tkind\tseen\tanswered\tmissed\ttimeout\tlastSec\tpassed\tfailed\tdropped\n";
    char buf[256]{};
    for (const Row& r : gRows) {
        snprintf(buf, sizeof(buf), "%s\t%s\t%u\t%u\t%u\t%u\t%lld\t%u\t%u\t%u\n", r.name,
                 KindTag(r.kind), r.seen, r.answered, r.missed, r.timeout,
                 static_cast<long long>(r.lastSec), r.passed, r.failed, r.dropped);
        body += buf;
    }
    // 先写临时件再替换，避免掉电/崩溃留半截表。
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f.good()) return;
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
    }
}

void ApplyOutcomeLocked(const char* name, Kind kind, Outcome outcome);

Row* FindOrAddLocked(const char* name, Kind kind) {
    for (Row& r : gRows) {
        if (r.kind == kind && strcmp(r.name, name) == 0) return &r;
    }
    if (static_cast<int>(gRows.size()) >= kMaxRows) return nullptr;
    Row r{};
    strncpy_s(r.name, name, _TRUNCATE);
    r.kind = kind;
    gRows.push_back(r);
    return &gRows.back();
}

// missed 现场落盘。追加进单文件而不是每次一个新件：missed 稀有（aa29bc 16 题里 1 次）、
// 每条约 1KB，不轮转也长不起来，且一个文件就能顺时间读完整段历史。
// PruneLieEvents 只清 .jpg/.png 且跳过目录，不会把它删掉。
void AppendMissedFileLocked(const std::string& text) {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return;
    const std::string dir = std::string(bin) + "state\\lie_events";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::ofstream f(dir + "\\missed.txt", std::ios::binary | std::ios::app);
    if (!f.is_open()) return;
    f << text;
}

// tag = missed / dropped；discDelta = 本题进行期间的断线次数（>0 即已改判 dropped）。
void DumpMissedLocked(const char* name, Kind kind, const Row& r, const char* tag,
                      uint32_t discDelta) {
    // provider 在锁内被回调，约定见 lie_stats.h：只读原子量、不碰 il2cpp、不回调本模块。
    char snap[1400]{};
    if (SnapshotFn fn = gSnapshot[static_cast<int>(kind)]) fn(snap, sizeof(snap));

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char head[512]{};
    snprintf(head, sizeof(head),
             "===== %s %04u-%02u-%02u %02u:%02u:%02u tick=%lu =====\r\n"
             "kind=%s char=%s disconnects-during-quiz=%u\r\n"
             "totals seen=%u ans=%u miss=%u to=%u drop=%u\r\n",
             tag, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             static_cast<unsigned long>(GetTickCount()), KindTag(kind), name, discDelta, r.seen,
             r.answered, r.missed, r.timeout, r.dropped);

    AppendMissedFileLocked(std::string(head) + (snap[0] ? snap : "(no snapshot provider)") +
                           "\r\n\r\n");
}

// 回滚紧邻的那笔 missed，并把闩重新扣上，让随后真正的 outcome 能落账。
// missed.txt 里同步追加一条撤销说明：那份取证是逐条追加的历史，不能只改 tsv 让两处对不上账。
void RevokeLastMissedLocked(Kind kind, DWORD now) {
    const int idx = static_cast<int>(kind);
    const bool wasDropped = gLastDropped[idx];
    gLastMissed[idx] = false;
    gLastDropped[idx] = false;
    gPending[idx] = true;
    // 题还在进行中，去重窗口从此刻重新起算，覆盖被连续打断多次的情形。
    gLastTouchMs[idx] = now;
    if (!gLastMissedName[idx][0]) return;
    Row* r = FindOrAddLocked(gLastMissedName[idx], kind);
    if (!r) return;
    // 扣回上一笔落在哪个计数器上的那一次。
    if (wasDropped) {
        if (r->dropped > 0) --r->dropped;
    } else if (r->missed > 0) {
        --r->missed;
    }
    SaveLocked();

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char note[512]{};
    snprintf(note, sizeof(note),
             "===== %s REVOKED %04u-%02u-%02u %02u:%02u:%02u tick=%lu =====\r\n"
             "kind=%s char=%s reason=same-instance reopen (panel redraw mid-answer)\r\n"
             "totals seen=%u ans=%u miss=%u to=%u drop=%u\r\n\r\n",
             wasDropped ? "dropped" : "missed", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, static_cast<unsigned long>(GetTickCount()), KindTag(kind), r->name,
             r->seen, r->answered, r->missed, r->timeout, r->dropped);
    AppendMissedFileLocked(note);
    LogLine("stats %s %s revoked char=%s (seen=%u ans=%u miss=%u to=%u drop=%u) -> relatch",
            KindTag(kind), wasDropped ? "dropped" : "missed", r->name, r->seen, r->answered,
            r->missed, r->timeout, r->dropped);
}

void ApplyOutcomeLocked(const char* name, Kind kind, Outcome outcome) {
    const int idx = static_cast<int>(kind);
    gPending[idx] = false;
    // 定性也算一次「接触」：去重窗口从这里往后算，覆盖答完后 UI 才重绘的情形。
    gLastTouchMs[idx] = GetTickCount();

    // 未答的账先分清该算谁的：开题之后断过线，这题就是被网络冲掉的，不是我们没答上。
    // 只改判 Missed——answered / timeout 都已有明确归因，掉线不该来抢它们的账。
    const uint32_t discNow = kick_sniff::DisconnectSeq();
    const uint32_t discAtSeen = gSeenDiscSeq[idx];
    if (outcome == Outcome::Missed && discNow != discAtSeen) outcome = Outcome::Dropped;

    Row* r = FindOrAddLocked(name, kind);
    if (!r) return;
    const char* tag = "answered";
    if (outcome == Outcome::Answered) {
        ++r->answered;
    } else if (outcome == Outcome::Timeout) {
        ++r->timeout;
        tag = "timeout";
    } else if (outcome == Outcome::Dropped) {
        ++r->dropped;
        tag = "dropped";
    } else {
        ++r->missed;
        tag = "missed";
    }
    r->lastSec = static_cast<int64_t>(::time(nullptr));
    SaveLocked();
    // 记住这笔未答是哪一种：同一实例若在去重窗口内重开，要按种类回滚（见 RevokeLastMissedLocked）。
    const bool unanswered = (outcome == Outcome::Missed || outcome == Outcome::Dropped);
    gLastMissed[idx] = unanswered;
    gLastDropped[idx] = (outcome == Outcome::Dropped);
    strncpy_s(gLastMissedName[idx], r->name, _TRUNCATE);
    // 账目先落盘再取证，快照里的 totals 与 tsv 一致。
    if (unanswered) DumpMissedLocked(name, kind, *r, tag, discNow - discAtSeen);
    LogLine("stats %s %s char=%s (seen=%u ans=%u miss=%u to=%u drop=%u) disc=%u->%u",
            KindTag(kind), tag, name, r->seen, r->answered, r->missed, r->timeout, r->dropped,
            discAtSeen, discNow);
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lk(gMu);
    LoadLocked();
}

void SetSnapshotProvider(Kind kind, SnapshotFn fn) {
    std::lock_guard<std::mutex> lk(gMu);
    gSnapshot[static_cast<int>(kind)] = fn;
}

void SetSuppressed(bool on) {
    std::lock_guard<std::mutex> lk(gMu);
    if (gSuppressed == on) return;
    gSuppressed = on;
    // 切换时清闩：免得干跑开的题在恢复记账后被补记 missed。
    gPending[0] = false;
    gPending[1] = false;
    // 去重记忆一并清掉：干跑用的实例地址不该把恢复后的第一道真题误判成同题。
    gLastToken[0] = gLastToken[1] = 0;
    gLastTouchMs[0] = gLastTouchMs[1] = 0;
    gLastMissed[0] = gLastMissed[1] = false;
    gLastDropped[0] = gLastDropped[1] = false;
    gLastMissedName[0][0] = gLastMissedName[1][0] = '\0';
    gSeenDiscSeq[0] = gSeenDiscSeq[1] = 0;
}

void RecordSeen(Kind kind, uint64_t token) {
    const int idx = static_cast<int>(kind);
    char name[kNameLen]{};
    CurrentName(name, sizeof(name));
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(gMu);
    if (gSuppressed) return;
    LoadLocked();
    if (!gLoaded) return;  // 目录未就绪：宁可丢这一条，也不能把没载入的表写回盘
    // 同一实例在窗口内重开 = 同题 UI 重绘，不是新题：seen 不重复记。
    //
    // 重开有两种，靠上一笔 outcome 区分：
    //   · 答完之后才重绘（常态）—— 已落 answered，闩也已闸，这里挡掉 seen 就够了。
    //   · 答完之前被关又开（BIN aa29bc 08-10 22:45）—— NotifyClosed 已经把题判成 missed，
    //     可它随后就答成了（samples 从 291 接着走到 330/330）。那笔 missed 是误记，
    //     回滚掉并重新上闩，否则真正的 answer-sent 会被 gPending 闸吞掉。
    if (token != 0 && token == gLastToken[idx] && gLastTouchMs[idx] != 0 &&
        (now - gLastTouchMs[idx]) < kSameQuizWindowMs) {
        LogLine("stats seen %s dedupe token=%llx age=%ums lastMissed=%d", KindTag(kind),
                static_cast<unsigned long long>(token),
                static_cast<unsigned>(now - gLastTouchMs[idx]), gLastMissed[idx] ? 1 : 0);
        if (gLastMissed[idx]) RevokeLastMissedLocked(kind, now);
        return;
    }
    gLastToken[idx] = token;
    gLastTouchMs[idx] = now;
    // 上一题没等到收尾就又开一题（中途关功能 / 掉线重进）：先把旧的补记 missed，别丢账。
    // 注意顺序——这一笔结算要用**上一题**开题时的断线快照，所以必须赶在下面刷新它之前。
    if (gPending[idx]) ApplyOutcomeLocked(name, kind, Outcome::Missed);
    gSeenDiscSeq[idx] = kick_sniff::DisconnectSeq();
    // 换题了：上一题的 missed 已成定局，不能再被这一题的面板重绘回滚掉。
    gLastMissed[idx] = false;
    gLastDropped[idx] = false;
    gPending[idx] = true;
    Row* r = FindOrAddLocked(name, kind);
    if (!r) return;
    ++r->seen;
    r->lastSec = static_cast<int64_t>(::time(nullptr));
    SaveLocked();
    LogLine("stats seen %s char=%s total=%u", KindTag(kind), name, r->seen);
}

void RecordOutcome(Kind kind, Outcome outcome) {
    const int idx = static_cast<int>(kind);
    char name[kNameLen]{};
    CurrentName(name, sizeof(name));
    std::lock_guard<std::mutex> lk(gMu);
    if (gSuppressed) return;
    LoadLocked();
    if (!gLoaded) return;
    if (!gPending[idx]) return;  // 本题已定性，后续 outcome 忽略
    ApplyOutcomeLocked(name, kind, outcome);
}

void RecordVerdict(Kind kind, bool passed) {
    char name[kNameLen]{};
    CurrentName(name, sizeof(name));
    std::lock_guard<std::mutex> lk(gMu);
    if (gSuppressed) return;
    LoadLocked();
    if (!gLoaded) return;
    // 不碰 gPending：判定常在 answered 落账之后才回来，跟本题的闩是两条独立的线。
    Row* r = FindOrAddLocked(name, kind);
    if (!r) return;
    if (passed)
        ++r->passed;
    else
        ++r->failed;
    r->lastSec = static_cast<int64_t>(::time(nullptr));
    SaveLocked();
    LogLine("stats %s verdict %s char=%s (pass=%u fail=%u / seen=%u ans=%u)", KindTag(kind),
            passed ? "PASS" : "FAIL", name, r->passed, r->failed, r->seen, r->answered);
}

void NotifyClosed(Kind kind) {
    const int idx = static_cast<int>(kind);
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gPending[idx]) return;
    }
    // 开过题却没有任何 outcome：UI 已关而我们没答上，计 missed。
    RecordOutcome(kind, Outcome::Missed);
}

}  // namespace x::features::auto_lie::lie_stats
