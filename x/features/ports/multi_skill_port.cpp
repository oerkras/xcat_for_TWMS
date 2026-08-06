#include "multi_skill_port.h"

#include "attack_input_port.h"
#include "skill_port.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include "xcat_multiskill_select.h"
#include "xcat_payload_control.h"
#include "xcat_skill_names.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace x::features::ports::multi_skill {
namespace {

std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_gapMs{xcat::kMultiSkillGapDefaultMs};
std::atomic<bool> g_safeStagger{true};
std::atomic<bool> g_sendUseRequest{false};

std::atomic<uint32_t> g_lastScheduled{0};
std::atomic<uint32_t> g_lastSkip{0};
std::atomic<uint32_t> g_lastSpanMs{0};
std::atomic<uint32_t> g_lastSelCount{0};
std::atomic<uint32_t> g_lastCastCount{0};
std::atomic<unsigned long long> g_lastBurstTickMs{0};
std::atomic<DWORD> g_burstBusyUntil{0};

std::mutex g_selectMu;
std::vector<std::string> g_selectCached;
DWORD g_selectLoadedMs = 0;
ULONGLONG g_selectMtime = 0;
constexpr DWORD kSelectPollMs = 250;
// PendingCast.skillId：>0 技能；kPendingNormalAttack 表示普攻（TryFirePrimary）。
constexpr int kPendingNormalAttack = -1;
constexpr uint8_t kNaMaxRetries = 8;
constexpr DWORD kNaRetryMs = 40;

struct PendingCast {
    int skillId = 0;
    DWORD dueMs = 0;
    uint8_t retries = 0;
};

std::mutex g_queueMu;
std::vector<PendingCast> g_queue;

ULONGLONG FileMtimeOrZero(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER u{};
    u.LowPart = data.ftLastWriteTime.dwLowDateTime;
    u.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return u.QuadPart;
}

bool SameStringVector(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

uint32_t EffectiveGapMs() {
    uint32_t gap = xcat::ClampMultiSkillGapMs(g_gapMs.load());
    if (g_safeStagger.load() && gap < 120u) gap = 120u;
    return gap;
}

bool ReloadSelectionFromDiskLocked(DWORD now) {
    const bool firstLoad = (g_selectLoadedMs == 0);
    const char* bin = runtime::GetBinDir();
    const std::string path = xcat::MultiSkillSelectPath(bin);
    const ULONGLONG mt = FileMtimeOrZero(path);
    std::vector<std::string> codes;
    xcat::ReadMultiSkillSelect(bin, codes);

    g_selectLoadedMs = now;
    const bool changed = firstLoad || mt != g_selectMtime || !SameStringVector(codes, g_selectCached);
    g_selectMtime = mt;
    if (!changed) return false;
    g_selectCached.swap(codes);

    std::string dbg = "selection n=" + std::to_string(g_selectCached.size()) + " codes=[";
    for (size_t i = 0; i < g_selectCached.size(); ++i) {
        if (i) dbg += ',';
        dbg += g_selectCached[i];
    }
    dbg += "]";
    runtime::LogI("MultiSkill", "%s", dbg.c_str());
    return true;
}

void EnsureSelectionFresh() {
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_selectMu);
    if (!g_selectLoadedMs || static_cast<DWORD>(now - g_selectLoadedMs) >= kSelectPollMs) {
        ReloadSelectionFromDiskLocked(now);
    }
}

int ParseSkillId(const std::string& code) {
    if (!xcat::IsNumericSkillId(code.c_str())) return 0;
    return atoi(code.c_str());
}

void SetOut(char* out, int outSz, const char* msg) {
    if (!out || outSz <= 0) return;
    strncpy_s(out, outSz, msg ? msg : "", _TRUNCATE);
}

}  // namespace

void Init() {
    g_enabled = false;
    g_gapMs = xcat::kMultiSkillGapDefaultMs;
    g_safeStagger = true;
    g_burstBusyUntil = 0;
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        g_queue.clear();
    }
    runtime::LogI("MultiSkill", "port init (Prepare via skill_port; no family expand)");
}

void Shutdown() {
    CancelPendingBurstForRetarget();
    g_enabled = false;
}

void SetConfig(bool enabled, uint32_t gapMs, bool safeStagger) {
    g_enabled = enabled;
    g_gapMs = xcat::ClampMultiSkillGapMs(gapMs);
    g_safeStagger = safeStagger;
}

void SetSendUseRequest(bool on) { g_sendUseRequest = on; }
bool GetSendUseRequest() { return g_sendUseRequest.load(); }

bool IsEnabled() { return g_enabled.load(); }
bool GetSafeStagger() { return g_safeStagger.load(); }
uint32_t GetGapMs() { return EffectiveGapMs(); }

bool HasSelection() {
    EnsureSelectionFresh();
    std::lock_guard<std::mutex> lk(g_selectMu);
    for (const std::string& c : g_selectCached) {
        if (xcat::IsNormalAttackCode(c.c_str()) || ParseSkillId(c) > 0) return true;
    }
    return false;
}

void ReloadSelectionNow() {
    std::lock_guard<std::mutex> lk(g_selectMu);
    ReloadSelectionFromDiskLocked(GetTickCount());
}

void GetLastBurst(uint32_t& scheduled, uint32_t& skip, uint32_t& spanMs, uint32_t& selCount,
                  uint32_t& castCount, unsigned long long& tickMs) {
    scheduled = g_lastScheduled.load();
    skip = g_lastSkip.load();
    spanMs = g_lastSpanMs.load();
    selCount = g_lastSelCount.load();
    castCount = g_lastCastCount.load();
    tickMs = g_lastBurstTickMs.load();
}

bool IsBurstBusy() {
    const DWORD now = GetTickCount();
    const DWORD until = g_burstBusyUntil.load();
    if (!until) return false;
    if (static_cast<int>(until - now) > 0) return true;
    // also busy while queue non-empty
    std::lock_guard<std::mutex> lk(g_queueMu);
    return !g_queue.empty();
}

bool CancelPendingBurstForRetarget() {
    bool hadPending = false;
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        hadPending = !g_queue.empty();
        g_queue.clear();
    }
    const bool wasBusy = g_burstBusyUntil.load() != 0;
    g_burstBusyUntil = 0;
    // ClearLock 很频繁；空队列不刷 log，避免 BIN 噪音。
    if (hadPending) {
        runtime::LogI("MultiSkill", "pending burst cancelled");
    }
    return hadPending || wasBusy;
}

bool TryCast(char* out, int outSz) {
    if (!g_enabled.load()) {
        SetOut(out, outSz, "disabled");
        return false;
    }
    if (IsBurstBusy()) {
        SetOut(out, outSz, "busy");
        return false;
    }

    EnsureSelectionFresh();
    std::vector<std::string> sel;
    {
        std::lock_guard<std::mutex> lk(g_selectMu);
        sel = g_selectCached;
    }
    if (sel.empty()) {
        SetOut(out, outSz, "no_select");
        return false;
    }

    const uint32_t gap = EffectiveGapMs();
    const DWORD now = GetTickCount();
    std::vector<PendingCast> pending;
    pending.reserve(sel.size());
    uint32_t skip = 0;
    uint32_t idx = 0;

    const auto& skillPack = xcat::GetSharedSkillNames(runtime::GetBinDir());
    for (const std::string& code : sel) {
        if (xcat::IsNormalAttackCode(code.c_str())) {
            PendingCast pc{};
            pc.skillId = kPendingNormalAttack;
            pc.dueMs = now + idx * gap;
            pending.push_back(pc);
            ++idx;
            continue;
        }
        const int id = ParseSkillId(code);
        if (id <= 0) {
            ++skip;  // junk
            continue;
        }
        // 防御：勾选里若残留辅助技，串发跳过（归 BUFF 页）。
        if (!xcat::SkillLooksLikeAttackCandidate(skillPack, code.c_str(), /*keepIfSelected=*/false)) {
            ++skip;
            continue;
        }
        const int lv = ports::skill::GetSkillLevel(id);
        if (lv <= 0) {
            ++skip;
            continue;
        }
        PendingCast pc{};
        pc.skillId = id;
        pc.dueMs = now + idx * gap;
        pending.push_back(pc);
        ++idx;
    }

    if (pending.empty()) {
        g_lastScheduled = 0;
        g_lastSkip = skip;
        g_lastSpanMs = 0;
        g_lastSelCount = static_cast<uint32_t>(sel.size());
        g_lastCastCount = 0;
        g_lastBurstTickMs = GetTickCount64();
        SetOut(out, outSz, "no_castable");
        runtime::LogW("MultiSkill", "TryCast no_castable sel=%zu skip=%u", sel.size(), skip);
        return false;
    }

    const uint32_t span = (pending.size() > 1) ? static_cast<uint32_t>((pending.size() - 1) * gap) : 0;
    const uint32_t scheduled = static_cast<uint32_t>(pending.size());
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        g_queue.swap(pending);
    }
    g_burstBusyUntil = now + span + gap + 50;
    g_lastScheduled = scheduled;
    g_lastSkip = skip;
    g_lastSpanMs = span;
    g_lastSelCount = static_cast<uint32_t>(sel.size());
    g_lastCastCount = scheduled;
    g_lastBurstTickMs = GetTickCount64();

    runtime::LogI("MultiSkill", "TryCast ok scheduled=%u skip=%u span=%ums gap=%u sel=%u",
                  scheduled, skip, span, gap, g_lastSelCount.load());
    SetOut(out, outSz, "ok");
    Tick();
    return true;
}

void Tick() {
    // 普攻 Down 后异步 Up：多发 worker 也要泵，不能只靠 simple_combat。
    ports::attack::TickReleases();

    std::vector<PendingCast> due;
    const DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        size_t keep = 0;
        for (size_t i = 0; i < g_queue.size(); ++i) {
            if (static_cast<int>(g_queue[i].dueMs - now) <= 0) {
                due.push_back(g_queue[i]);
            } else {
                if (keep != i) g_queue[keep] = g_queue[i];
                ++keep;
            }
        }
        g_queue.resize(keep);
        if (g_queue.empty() && due.empty()) {
            // release busy early when drained
            if (g_burstBusyUntil.load() && static_cast<int>(g_burstBusyUntil.load() - now) <= 0) {
                g_burstBusyUntil = 0;
            }
        }
    }

    for (const PendingCast& pc : due) {
        if (pc.skillId == kPendingNormalAttack) {
            // 普攻只走 OnFuncKey 正路组包（与攻击加速同路径）。
            // 不接 attack_rpc Create(50) 手搓 BODY：2026-08-04 BIN 半秒连发即断线。
            // 「技能发包直发」仅作用于技能 SendSkillUseRequest，不影响 NA。
            const bool ok = ports::attack::TryFirePrimary();
            if (ok) {
                runtime::LogI("MultiSkill", "cast NormalAttack ok=1 path=OnFuncKey");
                continue;
            }
            // 间隔 / pendingUp：短延期重入队，并平移后续 due，避免技能抢先于重试普攻。
            if (pc.retries < kNaMaxRetries) {
                PendingCast again = pc;
                again.retries = static_cast<uint8_t>(pc.retries + 1);
                again.dueMs = now + kNaRetryMs;
                {
                    std::lock_guard<std::mutex> lk(g_queueMu);
                    for (PendingCast& q : g_queue) {
                        q.dueMs += kNaRetryMs;
                    }
                    g_queue.push_back(again);
                }
                const DWORD until = g_burstBusyUntil.load();
                if (until) {
                    g_burstBusyUntil = until + kNaRetryMs;
                } else {
                    g_burstBusyUntil = again.dueMs + 50u;
                }
                runtime::LogI("MultiSkill", "cast NormalAttack ok=0 retry=%u/%u in %ums",
                              again.retries, kNaMaxRetries, kNaRetryMs);
            } else {
                runtime::LogW("MultiSkill", "cast NormalAttack ok=0 give_up after %u retries",
                              kNaMaxRetries);
            }
            continue;
        }
        bool notReady = false;
        char reason[32]{};
        const bool ok =
            g_sendUseRequest.load()
                ? ports::skill::CastSkillPreferSendUse(pc.skillId, &notReady, reason, sizeof(reason))
                : ports::skill::CastSkill(pc.skillId, &notReady, reason, sizeof(reason));
        runtime::LogI("MultiSkill", "cast id=%d ok=%d notReady=%d reason=%s sendUse=%d", pc.skillId,
                      ok ? 1 : 0, notReady ? 1 : 0, reason[0] ? reason : "-",
                      g_sendUseRequest.load() ? 1 : 0);
    }

    if (!IsBurstBusy()) g_burstBusyUntil = 0;
}

}  // namespace x::features::ports::multi_skill
