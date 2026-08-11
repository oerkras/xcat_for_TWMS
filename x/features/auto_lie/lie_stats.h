#pragma once
// Classic TWMS — 测谎作答统计（按角色累计，落 state\lie_stats.tsv）。
//
// 口径说明（两套数，别混着看）：
//
// 一、本工具的作答情况。游戏没有累计字段也没有查询接口（UIAntiMacro* 六个类型含 Util 117
//     都是实例级、UI 一销毁就没），所以这几项只记我们自己干得如何：
//   seen     题目 UI 打开过一次
//   answered 我们把答案交出去了（知识题=OnOk 提交成功；轨迹题=采满点或服端已回结果）
//   missed   UI 关闭时我们没能作答（轨迹题建图失败、坏 ans、提交失败等）
//   timeout  知识题 LLM 未在限时内给出答案
//   dropped  开题到收尾之间掉过线 —— 题是被网络冲掉的，不是我们没答上（详见 Outcome::Dropped）
//
// 二、服端判定。`_isSuccess`（TW +0x108）在 `_isResultRecv` 置位后就是官方结果，交卷后的
//     淡出期读得到 —— 早先以为「客户端拿不到判定」，是漏了这个字段：
//   passed   服端判通过
//   failed   服端判失败
//
// 两套数不该相等：answered 只说明 330 个点交出去了，交上去的轨迹合不合格是另一回事。
// BIN aa29bc 08-10 22:45 就是活例——采到 84% 时谓词降级被误判成面板关闭，Abort 把光标弹回
// 答题前的位置，游戏那几帧照采，于是交上去的轨迹里带着一段人为瞬移，但 answered 照记。
// answered 与 passed 的差额就是这类脏轨迹的量。

#include <cstdint>

namespace x::features::auto_lie::lie_stats {

enum class Kind { Text = 0, Mouse = 1 };

// Dropped 不需要调用方判断：记账时若发现「开题之后掉过线」，Missed 会自动改判成它
// （见 .cpp 的 kick_sniff::DisconnectSeq 快照）。分出来是因为两者该怪的东西完全不同——
// missed 是我们的锅，dropped 是网络的。BIN d43e77 查客户 08-11 05:20 那次报障时，
// 全天 814 次真断线、05:00~05:20 二十分钟里就断了 25 次（平均 48 秒一次），
// 而测谎要连续采满 330 点才能交卷；这种账不分开，工具的作答能力就永远看不清。
enum class Outcome { Answered = 0, Missed = 1, Timeout = 2, Dropped = 3 };

constexpr int kNameLen = 32;

struct Row {
    char name[kNameLen]{};
    Kind kind = Kind::Text;
    uint32_t seen = 0;
    uint32_t answered = 0;
    uint32_t missed = 0;
    uint32_t timeout = 0;
    int64_t lastSec = 0;  // 末次更新的 unix 秒（GetTickCount 重启归零，不能用）
    // 服端判定。列排在 lastSec 之后（TSV 追加在末尾），旧表读进来就是 0，不必迁移。
    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t dropped = 0;  // 同上，新列一律追加在末尾
};

// 载入已有统计；重复调用安全。
void Init();

// missed 现场取数。统计模块不认识 follower / auto_lie 的内部状态，只在补记 missed 的那一刻
// 回调一次，把文本连同当时的账目落进 state\lie_events\missed.txt。
//
// 为什么落在 lie_events：那个目录不参与轮转。BIN aa29bc（16 遇到 / 15 已答 / 1 未答）全靠它
// 才留下 17 份映射证据，而 x.jsonl 11 卷只剩最近 39 分钟，missed 究竟是哪一次、当时卡在哪，
// 无从可查——这个接口就是为了补上那个盲区。
//
// 实现方约定（两条都是硬的）：
//   · 只读已缓存的原子量。回调线程不定（可能是帧脉冲线程），绝不能碰 il2cpp 托管调用。
//   · 不得反过来调 lie_stats 的任何函数：回调发生在记账锁内，会自锁。
using SnapshotFn = void (*)(char* out, int outSz);
void SetSnapshotProvider(Kind kind, SnapshotFn fn);

// 干跑（不真提交）期间挂起记账，免得测试把真实战绩冲淡。
void SetSuppressed(bool on);

// 题目 UI 打开。同一道题只算一次（内部按 kind 上闩，直到 NotifyClosed）。
//
// token 传题目实例地址（拿不到给 0）：同一实例在短窗内 close→open 视为**同一道题的 UI 重绘**，
// 不重复记账。BIN 82a4b0 就吃过这个亏——14:55:41 答完，14:55:45 同一实例的 UI 又开了一次
// （follow moves 一点没涨，samples 还是上一题的 330），一道题被记成 seen=2 answered=2。
void RecordSeen(Kind kind, uint64_t token = 0);

// 本题落幕；仅首个 outcome 生效，之后的忽略。
void RecordOutcome(Kind kind, Outcome outcome);

// 题目 UI 关闭：解闩。若开过题却没有任何 outcome，自动补记 missed。
void NotifyClosed(Kind kind);

// 服端判定到手（读 `_isSuccess`）。与 RecordOutcome 无关、不受本题闩约束：判定往往在
// answered 之后才回来。调用方负责一题只报一次（follower 用 gVerdictLogged 保证）。
void RecordVerdict(Kind kind, bool passed);

}  // namespace x::features::auto_lie::lie_stats
