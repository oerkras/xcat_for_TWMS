#include "multi_skill_port.h"

#include "attack_input_port.h"
#include "ground_spoof.h"
#include "player_combat_port.h"
#include "skill_port.h"

#include "../attack_accel/attack_accel.h"
#include "../final_attack_force/final_attack_force.h"
#include "../simple_combat/simple_combat.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include "xcat_multiskill_select.h"
#include "xcat_payload_control.h"
#include "xcat_skill_names.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace x::features::ports::multi_skill {
namespace {

std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_gapMs{xcat::kMultiSkillGapDefaultMs};
// 本波选择里勾了攻击技（非「本 tick 可施放」）。skill_anim 跳过蜗牛时仍为 true，
// 避免误走纯普攻 ActionBusy 路径（BIN：sel=2 却 naOnly=1）。
std::atomic<bool> g_selectHasSkill{false};
// 本波等效纯普攻（空蓝回退 / 探针 / 真·只勾普攻）：跟 ActionBusy，禁止 ClearBusy。
// 空蓝时清单仍勾技能，若只看 g_selectHasSkill 会叠 NA → 机枪。
std::atomic<bool> g_naNativeGate{false};
// 带技能时无法边清忙边量 busy_cycle：缺 base6 时抽一刀纯普攻探针校准 naIv。
std::atomic<bool> g_naBusyProbeActive{false};
// 带技能时整波间隔 max(na, skills)；Tick 与 TryCast 共用。
std::atomic<DWORD> g_waveIntervalMs{0};
std::atomic<bool> g_safeStagger{true};
std::atomic<bool> g_sendUseRequest{false};

std::atomic<uint32_t> g_lastScheduled{0};
std::atomic<uint32_t> g_lastSkip{0};
std::atomic<uint32_t> g_lastSpanMs{0};
std::atomic<uint32_t> g_lastSelCount{0};
std::atomic<uint32_t> g_lastCastCount{0};
std::atomic<unsigned long long> g_lastBurstTickMs{0};
std::atomic<DWORD> g_burstBusyUntil{0};
// 多发普攻上次真正接刀成功（b1≥0）；与战斗面板间隔脱钩。
std::atomic<DWORD> g_lastMultiNaOkMs{0};
// 官方地板：学到的 degree=6 基准（≈ GetDamageDelay 的 frameDelay）；0=尚未校准。
// 有效间隔 = base6 × (degree+10)/16（见 attack_accel::EstimateDamageDelayScaleMs）。
std::atomic<DWORD> g_naBaseMsAtDeg6{0};
std::atomic<DWORD> g_nativeNaFloorMs{xcat::kMultiSkillNativeNaFloorBootstrapMs};  // 最近有效间隔缓存
// Prepare 后 ActionLayer delay 表加总（已含 ActionSpeed）；优先于 busy_cycle/base6。
// 注意：这是「上次出手时的缩放 ms」快照；有 g_naActionBaseSum 时 Effective 会按现速重算。
std::atomic<DWORD> g_naTableMs{0};
// 未乘 ActionSpeed 的 delay 正帧加总；ActionSpeed buff 变化时据此重算 naIv。
std::atomic<int> g_naActionBaseSum{0};
std::atomic<DWORD> g_naBusyArmMs{0};  // 接刀成功时刻；busy 自然回到 <0 时结算周期
std::atomic<bool> g_naFloorSkipBusySample{false};  // 本刀已主动 ClearActionBusy，禁止忙锁采样
std::atomic<int> g_naLastDegree{6};

std::mutex g_selectMu;
std::vector<std::string> g_selectCached;
DWORD g_selectLoadedMs = 0;
ULONGLONG g_selectMtime = 0;
constexpr DWORD kSelectPollMs = 250;
// PendingCast.skillId：>0 技能；kPendingNormalAttack 表示普攻（TryFirePrimary）。
constexpr int kPendingNormalAttack = -1;
// CoolTimeOver / 本地 CD 剩余大于此值则跳过施放（防浮点噪声）。
constexpr float kSkipCooldownRemainSec = 0.05f;
// 清忙锁后仍可能吞刀/发包失败：短重试盖住边沿。
constexpr uint8_t kNaMaxRetries = 40;
constexpr DWORD kNaRetryMs = 40;
// send_use_false：短退避（多半是 CD/反连发边沿），次数收紧。
constexpr uint8_t kSkillSoftMaxRetries = 4;
constexpr DWORD kSkillSoftRetryMs = 120;
// 空蓝 / Prepare 粘滞：技能跳过、回退普攻；窗内按纯普攻忙锁出刀，到期再验。
constexpr DWORD kMpNaFallbackMs = 800;
std::atomic<DWORD> g_mpNaFallbackUntil{0};

struct PendingCast {
    int skillId = 0;
    DWORD dueMs = 0;
    uint8_t retries = 0;
    // true：普攻后穿动画接技（SendUseOnly）；false：仅技能波 → DoActive（蜗牛等才有动作/弹道）。
    bool sendUseOnly = false;
};

std::mutex g_queueMu;
std::vector<PendingCast> g_queue;
// 普攻成功后再按 gap 入队；避免「忙锁等普攻」时把蜗牛术一起拖到动画结束。
std::mutex g_postNaMu;
std::vector<int> g_postNaSkills;

// 同技能动作间隔（多数攻击技无 CoolTime，闸门=动画/ActionBusy）。
std::mutex g_skillAnimMu;
std::unordered_map<int, DWORD> g_skillAnimMs;     // skillId → 最近算出的 ms（日志/回落）
std::unordered_map<int, int> g_skillBaseSum;      // skillId → 未乘 ActionSpeed 的 delay 加总
std::unordered_map<int, DWORD> g_skillLastOkMs;    // skillId → 上次成功施放
std::unordered_map<int, DWORD> g_skillBusyArmMs;   // skillId → 开始量 busy 周期
// WZ 无 action/0 的弓技贴脸会走武器挥砍通道；技能侧 QueryActionIndex 常仍报 shoot（BIN
// 844bf0/213e1b），故齐发/串行不听技能 act，而以「弓/弩 + 最近 NA ActionType」为 SSOT。
// 半近时 NA 偶发仍报 shoot：用 bow_melee_latch 迟滞（挥砍/do_false 闩上，连续射击才开）。
// 剑/斧等非弓不强制串行。
std::atomic<DWORD> g_weaponChanLastOkMs{0};
std::atomic<int> g_lastNaActionIdx{-1};
std::atomic<bool> g_bowMeleeLatch{false};
std::atomic<int> g_bowMeleeShootStreak{0};
// 闩锁解开：需连续若干刀射击 NA（避免半近 act 在 9↔22 间抖一下就恢复齐发）。
constexpr int kBowMeleeReleaseShootStreak = 2;

DWORD EffectiveMultiNaIntervalMs();  // 下方定义
void PollNativeNaFloor(DWORD tickNow);  // 下方定义
bool ClearBusyForCast();             // 下方定义
void ClearPostNaSkills();            // 下方定义
bool SkillHasOfflineAction(int skillId);
bool IsWeaponBoundSkill(int skillId);
bool SkillAllowsClearBusyStack(int skillId);
bool PendingSkillsAllowClearBusyStack();
void NoteWeaponChannelOk(DWORD okAt);
void NoteBowNaAction(int actIdx);
void ArmBowMeleeLatch(const char* why, bool dropPending);
void DropBowNoActionSkillsFromQueue();

// 用当前 ActionSpeed 把 baseSum → 闸门 ms；失败返回 0。
DWORD AnimMsFromBaseSumNow(int baseSum) {
    if (baseSum <= 0) return 0;
    void* lu = nullptr;
    int speed = 100;
    if (ports::player_combat::QueryLocalUser(&lu) && lu) {
        (void)attack_accel::QueryActionSpeed(lu, speed);
    }
    const int scaled = attack_accel::ScaleDelayByActionSpeed(baseSum, speed);
    return attack_accel::DelayUnitsToAnimMs(scaled);
}

bool SkillHasOfflineAction(int skillId) {
    if (skillId <= 0) return false;
    int base = 0;
    return attack_accel::LookupOfflineSkillBaseSum(skillId, base) && base > 0;
}

// 弓/弩 + 无离线 action/0 +（闩锁或最近 NA 非射击）→ 与 NA 共用武器通道。
bool IsWeaponBoundSkill(int skillId) {
    if (skillId <= 0) return false;
    if (SkillHasOfflineAction(skillId)) return false;
    if (!final_attack_force::EquippedWeaponIsBowFamily()) return false;
    if (g_bowMeleeLatch.load(std::memory_order_relaxed)) return true;
    const int naAct = g_lastNaActionIdx.load(std::memory_order_relaxed);
    return !attack_accel::IsRangedShootAction(naAct);
}

void NoteWeaponChannelOk(DWORD okAt) {
    if (!okAt) return;
    g_weaponChanLastOkMs.store(okAt, std::memory_order_relaxed);
}

void DropBowNoActionSkillsFromQueue() {
    std::lock_guard<std::mutex> lk(g_queueMu);
    std::vector<PendingCast> kept;
    kept.reserve(g_queue.size());
    for (const PendingCast& q : g_queue) {
        if (q.skillId == kPendingNormalAttack) {
            kept.push_back(q);
            continue;
        }
        if (q.skillId > 0 && SkillHasOfflineAction(q.skillId)) {
            kept.push_back(q);
            continue;
        }
        // 丢掉弓无 action/0 待施技，避免半近齐发队列继续出刀。
    }
    g_queue.swap(kept);
}

void ArmBowMeleeLatch(const char* why, bool dropPending) {
    if (!final_attack_force::EquippedWeaponIsBowFamily()) return;
    const bool was = g_bowMeleeLatch.exchange(true, std::memory_order_acq_rel);
    g_bowMeleeShootStreak.store(0, std::memory_order_relaxed);
    if (dropPending) {
        ClearPostNaSkills();
        DropBowNoActionSkillsFromQueue();
    }
    if (!was) {
        runtime::LogI("MultiSkill", "bow_melee_latch ON (%s)", why ? why : "?");
    }
}

void NoteBowNaAction(int actIdx) {
    if (!final_attack_force::EquippedWeaponIsBowFamily()) {
        g_bowMeleeLatch.store(false, std::memory_order_relaxed);
        g_bowMeleeShootStreak.store(0, std::memory_order_relaxed);
        return;
    }
    if (attack_accel::IsMeleeWeaponAction(actIdx)) {
        // 不在这里清 post-NA：交给随后 EnqueueSkillsAfterNa 按门控跳过。
        ArmBowMeleeLatch("na_melee", /*dropPending=*/false);
        return;
    }
    if (!attack_accel::IsRangedShootAction(actIdx)) return;
    if (!g_bowMeleeLatch.load(std::memory_order_relaxed)) {
        g_bowMeleeShootStreak.store(0, std::memory_order_relaxed);
        return;
    }
    const int streak = g_bowMeleeShootStreak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (streak >= kBowMeleeReleaseShootStreak) {
        g_bowMeleeLatch.store(false, std::memory_order_release);
        g_bowMeleeShootStreak.store(0, std::memory_order_relaxed);
        runtime::LogI("MultiSkill", "bow_melee_latch OFF after %d shoot NA", streak);
    } else {
        runtime::LogI("MultiSkill", "bow_melee_latch hold shootStreak=%d/%d", streak,
                      kBowMeleeReleaseShootStreak);
    }
}

// 有 WZ action/0 → 可 ClearBusy；
// 弓无 action/0 → 闩锁中禁止；否则仅最近 NA 为射击才齐发；
// 非弓无 action → 不强制串行。
bool SkillAllowsClearBusyStack(int skillId) {
    if (skillId <= 0) return false;
    if (SkillHasOfflineAction(skillId)) return true;
    if (!final_attack_force::EquippedWeaponIsBowFamily()) return true;
    if (g_bowMeleeLatch.load(std::memory_order_relaxed)) return false;
    const int naAct = g_lastNaActionIdx.load(std::memory_order_relaxed);
    return attack_accel::IsRangedShootAction(naAct);
}

bool PendingSkillsAllowClearBusyStack() {
    {
        std::lock_guard<std::mutex> lk(g_postNaMu);
        for (int id : g_postNaSkills) {
            if (SkillAllowsClearBusyStack(id)) return true;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        for (const PendingCast& q : g_queue) {
            if (q.skillId > 0 && SkillAllowsClearBusyStack(q.skillId)) return true;
        }
    }
    return false;
}
bool MpNaFallbackActive(DWORD now) {
    const DWORD until = g_mpNaFallbackUntil.load(std::memory_order_relaxed);
    return until != 0 && static_cast<int>(until - now) > 0;
}

// 空蓝 / 施法软拒：丢待施技能 / post-NA，立刻排一刀普攻，并短窗抑制技能。
void EnterMpNaFallback(DWORD now, int skillId, const char* why) {
    ClearPostNaSkills();
    g_mpNaFallbackUntil.store(now + kMpNaFallbackMs, std::memory_order_relaxed);
    g_naNativeGate.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        std::vector<PendingCast> kept;
        kept.reserve(g_queue.size() + 1);
        bool hasNa = false;
        for (const PendingCast& q : g_queue) {
            if (q.skillId == kPendingNormalAttack) {
                kept.push_back(q);
                hasNa = true;
            }
        }
        if (!hasNa) {
            PendingCast na{};
            na.skillId = kPendingNormalAttack;
            na.dueMs = now;
            kept.push_back(na);
        }
        g_queue.swap(kept);
    }
    const DWORD need = now + EffectiveMultiNaIntervalMs() + 50u;
    const DWORD until = g_burstBusyUntil.load();
    if (!until || static_cast<int>(need - until) > 0) g_burstBusyUntil = need;
    runtime::LogW("MultiSkill", "cast_fail id=%d → na_fallback (%s) hold=%ums", skillId,
                  why ? why : "?", kMpNaFallbackMs);
}

void FeedSkillAnimMs(int skillId, DWORD cycleMs, const char* tag) {
    if (skillId <= 0) return;
    if (cycleMs < 120u || cycleMs > 2500u) return;
    DWORD prev = 0;
    DWORD next = cycleMs;
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        const auto it = g_skillAnimMs.find(skillId);
        prev = it == g_skillAnimMs.end() ? 0 : it->second;
        if (tag && (std::strcmp(tag, "action_table") == 0 ||
                    std::strcmp(tag, "action_offline") == 0)) {
            next = cycleMs;
        } else {
            next = !prev ? cycleMs : (prev * 3u + cycleMs) / 4u;
        }
        g_skillAnimMs[skillId] = next;
    }
    runtime::LogI("MultiSkill", "skill_anim id=%d %s cycle=%ums ema=%ums→%ums", skillId,
                  tag ? tag : "?", cycleMs, prev, next);
}

void FeedSkillBaseSum(int skillId, int baseSum, const char* tag) {
    if (skillId <= 0 || baseSum <= 0) return;
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        // busy 墙钟噪声大：对已有 base 做轻 EMA；离线/层读覆盖为真值。
        if (tag && std::strcmp(tag, "busy_cycle") == 0) {
            const auto it = g_skillBaseSum.find(skillId);
            if (it != g_skillBaseSum.end() && it->second > 0) {
                baseSum = (it->second * 3 + baseSum) / 4;
            }
        }
        g_skillBaseSum[skillId] = baseSum;
    }
    const DWORD ms = AnimMsFromBaseSumNow(baseSum);
    if (ms) FeedSkillAnimMs(skillId, ms, tag);
    else {
        runtime::LogW("MultiSkill", "skill_base id=%d %s baseSum=%d → ms=0", skillId,
                      tag ? tag : "?", baseSum);
    }
}

// 技能时长：离线现算 → 武器通道(=naIv) → 缓存 baseSum×现速 → 旧 ms → NA 地板。
DWORD EffectiveSkillAnimMs(int skillId) {
    if (skillId <= 0) return EffectiveMultiNaIntervalMs();
    void* lu = nullptr;
    DWORD offlineMs = 0;
    if (ports::player_combat::QueryLocalUser(&lu) && lu &&
        attack_accel::LookupOfflineSkillAnimMs(lu, skillId, offlineMs)) {
        return offlineMs;
    }
    // 贴脸挥弓等：层 delay 是武器动作，闸门跟普攻同一通道（只能多不能少）。
    if (IsWeaponBoundSkill(skillId)) {
        return EffectiveMultiNaIntervalMs();
    }
    int baseSum = 0;
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        const auto bit = g_skillBaseSum.find(skillId);
        if (bit != g_skillBaseSum.end()) baseSum = bit->second;
    }
    if (baseSum > 0) {
        const DWORD ms = AnimMsFromBaseSumNow(baseSum);
        if (ms >= 120u) return ms;
    }
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        const auto it = g_skillAnimMs.find(skillId);
        if (it != g_skillAnimMs.end() && it->second >= 120u) return it->second;
    }
    return EffectiveMultiNaIntervalMs();
}

bool SkillAnimBlocking(int skillId, DWORD /*tickNow*/, DWORD* outRemainMs) {
    // 有专属 action：按单技频率。武器通道技（無 action/0 + 层≈普攻）：
    // 與 NA/其它武器通道技共用 lastOk，避免 ClearBusy 叠成 1 秒 6 刀近战。
    if (outRemainMs) *outRemainMs = 0;
    if (skillId <= 0) return false;
    const DWORD t = GetTickCount();
    const bool weaponBound = IsWeaponBoundSkill(skillId);
    DWORD last = 0;
    if (weaponBound) {
        last = g_weaponChanLastOkMs.load(std::memory_order_relaxed);
        if (!last) {
            std::lock_guard<std::mutex> lk(g_skillAnimMu);
            const auto lit = g_skillLastOkMs.find(skillId);
            if (lit != g_skillLastOkMs.end()) last = lit->second;
        }
    } else {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        const auto lit = g_skillLastOkMs.find(skillId);
        if (lit == g_skillLastOkMs.end() || !lit->second) return false;
        last = lit->second;
    }
    if (!last) return false;
    DWORD anim = EffectiveSkillAnimMs(skillId);
    if (!anim) anim = EffectiveMultiNaIntervalMs();
    if (static_cast<DWORD>(t - last) >= anim) return false;
    if (outRemainMs) *outRemainMs = anim - (t - last);
    return true;
}

// 普攻+技能同波：整波间隔 = max(普攻地板, 各技自身动作)，才能 A→技 且互不「借速」。
DWORD WaveIntervalMs(const std::vector<int>& skillIds) {
    DWORD wave = EffectiveMultiNaIntervalMs();
    for (int id : skillIds) {
        const DWORD a = EffectiveSkillAnimMs(id);
        if (a > wave) wave = a;
    }
    return wave;
}

void PollSkillAnimBusy(DWORD /*tickNow*/) {
    const DWORD endAt = GetTickCount();  // 结算用墙钟，不用 Tick 入口的过期 now
    std::vector<std::pair<int, DWORD>> done;
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        if (g_skillBusyArmMs.empty()) return;
    }
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) return;
    int busy = -1;
    if (!attack_accel::QueryActionBusy(lu, busy)) return;
    if (busy >= 0) return;  // 动作未结束
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        for (auto it = g_skillBusyArmMs.begin(); it != g_skillBusyArmMs.end();) {
            if (endAt >= it->second) done.emplace_back(it->first, endAt - it->second);
            it = g_skillBusyArmMs.erase(it);
        }
    }
    int speed = 100;
    (void)attack_accel::QueryActionSpeed(lu, speed);
    for (const auto& p : done) {
        // 墙钟已含当时 ActionSpeed → 反解 base，后续按现速重算（buff 即时）。
        const int base = attack_accel::AnimMsToBaseSum(p.second, speed);
        if (base > 0) {
            FeedSkillBaseSum(p.first, base, "busy_cycle");
        } else {
            FeedSkillAnimMs(p.first, p.second, "busy_cycle");  // 极端回落
        }
    }
}

// 施放成功：离线 base → 实时层反解 base（并识别武器通道）→ busy_cycle。
void NoteSkillCastOk(int skillId, DWORD okAt) {
    if (skillId <= 0) return;
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        g_skillLastOkMs[skillId] = okAt;
    }
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        g_skillBusyArmMs[skillId] = okAt;
        return;
    }
    int offlineBase = 0;
    if (attack_accel::LookupOfflineSkillBaseSum(skillId, offlineBase)) {
        int speed = 100;
        (void)attack_accel::QueryActionSpeed(lu, speed);
        runtime::LogI("MultiSkill", "skill_offline id=%d base=%d speed=%d", skillId, offlineBase,
                      speed);
        FeedSkillBaseSum(skillId, offlineBase, "action_offline");
        return;
    }
    int delaySum = 0;
    if (attack_accel::QueryActionLayerDelaySum(lu, delaySum) && delaySum > 0) {
        int speed = 100;
        (void)attack_accel::QueryActionSpeed(lu, speed);
        const int baseSum = attack_accel::UnscaleDelayByActionSpeed(delaySum, speed);
        const int store = baseSum > 0 ? baseSum : delaySum;
        const int naBase = g_naActionBaseSum.load(std::memory_order_relaxed);
        int skillAct = -1;
        (void)attack_accel::QueryActionIndex(lu, skillAct);
        const int naAct = g_lastNaActionIdx.load(std::memory_order_relaxed);
        const int wt = final_attack_force::QueryEquippedWeaponType();
        // 门控听 NA；skillAct 仅诊断（贴脸时常仍报 22）。
        const bool weaponBound = IsWeaponBoundSkill(skillId);
        const char* kind = weaponBound ? "na_melee_gate" : "na_ranged_gate";
        if (weaponBound) {
            NoteWeaponChannelOk(okAt);
            runtime::LogI("MultiSkill",
                          "skill_melee_serial id=%d skillAct=%d naAct=%d wt=%d sum=%d speed=%d "
                          "base=%d naBase=%d kind=%s (no ClearBusy stack)",
                          skillId, skillAct, naAct, wt, delaySum, speed, store, naBase, kind);
            FeedSkillBaseSum(skillId, store, kind);
            return;
        }
        runtime::LogI("MultiSkill",
                      "skill_ranged_stack id=%d skillAct=%d naAct=%d wt=%d sum=%d speed=%d "
                      "base=%d kind=%s (ClearBusy ok)",
                      skillId, skillAct, naAct, wt, delaySum, speed, store, kind);
        FeedSkillBaseSum(skillId, store, kind);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        g_skillBusyArmMs[skillId] = okAt;
    }
}

void ClearPostNaSkills() {
    std::lock_guard<std::mutex> lk(g_postNaMu);
    g_postNaSkills.clear();
}

// 本波是否还有待施技能（post-NA 或队列里 skillId>0）。空蓝时两者皆空 → 等同纯普攻。
bool HasPendingSkills() {
    {
        std::lock_guard<std::mutex> lk(g_postNaMu);
        if (!g_postNaSkills.empty()) return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        for (const PendingCast& q : g_queue) {
            if (q.skillId > 0) return true;
        }
    }
    return false;
}

void EnqueueSkillsAfterNa(DWORD now, uint32_t gap) {
    if (MpNaFallbackActive(now)) {
        ClearPostNaSkills();
        return;
    }
    std::vector<int> skills;
    {
        std::lock_guard<std::mutex> lk(g_postNaMu);
        skills.swap(g_postNaSkills);
    }
    if (skills.empty()) return;

    // 方案1：弓贴脸（最近 NA 非射击）→ 无 action/0 技本波不跟刀，只打 NA，
    // 避免 NA→断魂→二连连续三下挥弓；有离线 action 的技仍可排。
    std::vector<int> kept;
    kept.reserve(skills.size());
    size_t skippedMelee = 0;
    for (int id : skills) {
        if (SkillAllowsClearBusyStack(id)) {
            kept.push_back(id);
        } else {
            ++skippedMelee;
        }
    }
    if (kept.empty()) {
        const int naAct = g_lastNaActionIdx.load(std::memory_order_relaxed);
        runtime::LogI("MultiSkill",
                      "post_na skip melee_gate skipped=%zu naAct=%d latch=%d (NA-only this swing)",
                      skippedMelee, naAct, g_bowMeleeLatch.load(std::memory_order_relaxed) ? 1 : 0);
        return;
    }
    if (skippedMelee) {
        const int naAct = g_lastNaActionIdx.load(std::memory_order_relaxed);
        runtime::LogI("MultiSkill",
                      "post_na partial melee_gate keep=%zu skip=%zu naAct=%d latch=%d", kept.size(),
                      skippedMelee, naAct,
                      g_bowMeleeLatch.load(std::memory_order_relaxed) ? 1 : 0);
    }

    std::vector<PendingCast> add;
    add.reserve(kept.size());
    for (size_t i = 0; i < kept.size(); ++i) {
        PendingCast pc{};
        pc.skillId = kept[i];
        pc.dueMs = now + static_cast<DWORD>((i + 1) * gap);
        // NA 已 OnFuncKey 成功并 ClearActionBusy：此时客户端已空闲。
        // 再走 SendUseOnly 只会 ok_send_use，BIN 上蜗牛无动作/弹道；
        // 单独勾技能生效的是 DoActive（ok_do_active）——post-NA 对齐这条。
        pc.sendUseOnly = false;
        add.push_back(pc);
    }
    const DWORD lastDue = add.back().dueMs;
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        g_queue.insert(g_queue.end(), add.begin(), add.end());
    }
    const DWORD until = g_burstBusyUntil.load();
    const DWORD need = lastDue + gap + 50u;
    if (!until || static_cast<int>(need - until) > 0) {
        g_burstBusyUntil = need;
    }
    runtime::LogI("MultiSkill", "post_na enqueue skills=%zu gap=%u firstDue=+%ums doActive=1",
                  kept.size(), gap, gap);
}

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

void FeedNativeNaFloor(DWORD cycleMs, const char* tag) {
    if (cycleMs < 120u || cycleMs > 2500u) return;
    if (attack_accel::IsDesired()) return;  // 攻击加速清忙锁样本不可信
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) return;

    // busy/ok_gap：墙钟 ≈ 动画 ms（已含当时 ActionSpeed）→ 反解 baseSum，优先于 degree 路径。
    if (tag && (std::strcmp(tag, "busy_cycle") == 0 || std::strcmp(tag, "ok_gap") == 0)) {
        int speed = 100;
        (void)attack_accel::QueryActionSpeed(lu, speed);
        const int sample = attack_accel::AnimMsToBaseSum(cycleMs, speed);
        if (sample > 0) {
            const int prev = g_naActionBaseSum.load(std::memory_order_relaxed);
            const int next = prev > 0 ? (prev * 3 + sample) / 4 : sample;
            g_naActionBaseSum.store(next, std::memory_order_relaxed);
            const DWORD ms = AnimMsFromBaseSumNow(next);
            if (ms >= 120u) {
                g_naTableMs.store(ms, std::memory_order_relaxed);
                g_nativeNaFloorMs.store(ms, std::memory_order_relaxed);
            }
            if (std::strcmp(tag, "busy_cycle") == 0) {
                g_naBusyProbeActive.store(false, std::memory_order_relaxed);
            }
            runtime::LogI("MultiSkill",
                          "native_na_floor %s cycle=%ums speed=%d base=%d→%d eff=%ums",
                          tag, cycleMs, speed, prev, next, ms);
            return;
        }
    }

    // 回落：degree=6 基准（Booster/武器档）；仅当 ActionSpeed 反解失败时用。
    int deg = 6;
    (void)attack_accel::QueryAttackSpeedDegree(lu, deg);
    if (deg < 2) deg = 2;
    if (deg > 10) deg = 10;
    g_naLastDegree.store(deg, std::memory_order_relaxed);

    const DWORD baseSample =
        static_cast<DWORD>((static_cast<uint64_t>(cycleMs) * 16u) / static_cast<uint32_t>(deg + 10));
    if (baseSample < 120u || baseSample > 2500u) return;

    const DWORD prevBase = g_naBaseMsAtDeg6.load(std::memory_order_relaxed);
    const DWORD nextBase = prevBase ? (prevBase * 3u + baseSample) / 4u : baseSample;
    g_naBaseMsAtDeg6.store(nextBase, std::memory_order_relaxed);

    const DWORD prevEff = g_nativeNaFloorMs.load(std::memory_order_relaxed);
    const DWORD nextEff = attack_accel::EstimateDamageDelayScaleMs(lu, nextBase);
    g_nativeNaFloorMs.store(nextEff, std::memory_order_relaxed);
    if (tag && std::strcmp(tag, "busy_cycle") == 0) {
        g_naBusyProbeActive.store(false, std::memory_order_relaxed);
    }
    runtime::LogI("MultiSkill",
                  "native_na_floor %s cycle=%ums deg=%d base6=%ums→%ums eff=%ums→%ums",
                  tag ? tag : "?", cycleMs, deg, prevBase, nextBase, prevEff, nextEff);
}

// 出刀成功后读 ActionLayer delay 表 → 反解 baseSum。成功则返回 true。
bool TryFeedNaFromActionTable() {
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) return false;
    int delaySum = 0;
    if (!attack_accel::QueryActionLayerDelaySum(lu, delaySum) || delaySum <= 0) return false;
    int speed = 100;
    (void)attack_accel::QueryActionSpeed(lu, speed);
    const int baseSum = attack_accel::UnscaleDelayByActionSpeed(delaySum, speed);
    const int store = baseSum > 0 ? baseSum : delaySum;
    g_naActionBaseSum.store(store, std::memory_order_relaxed);
    int actIdx = -1;
    if (attack_accel::QueryActionIndex(lu, actIdx)) {
        g_lastNaActionIdx.store(actIdx, std::memory_order_relaxed);
        NoteBowNaAction(actIdx);
    }
    const DWORD ms = AnimMsFromBaseSumNow(store);
    if (ms >= 120u) {
        g_naTableMs.store(ms, std::memory_order_relaxed);
        g_nativeNaFloorMs.store(ms, std::memory_order_relaxed);
    }
    g_naBusyProbeActive.store(false, std::memory_order_relaxed);
    runtime::LogI("MultiSkill", "na_table sum=%d speed=%d base=%d act=%d → %ums", delaySum, speed,
                  store, actIdx, ms);
    return ms >= 120u;
}

// 多发普攻间隔：baseSum×现速 → 旧 table ms → degree 缩放 base6 → bootstrap。
DWORD EffectiveMultiNaIntervalMs() {
    PollNativeNaFloor(GetTickCount());
    const int baseSum = g_naActionBaseSum.load(std::memory_order_relaxed);
    if (baseSum > 0) {
        const DWORD ms = AnimMsFromBaseSumNow(baseSum);
        if (ms >= 120u) {
            g_naTableMs.store(ms, std::memory_order_relaxed);
            g_nativeNaFloorMs.store(ms, std::memory_order_relaxed);
            return ms;
        }
    }
    const DWORD table = g_naTableMs.load(std::memory_order_relaxed);
    if (table >= 120u) {
        g_nativeNaFloorMs.store(table, std::memory_order_relaxed);
        return table;
    }
    const DWORD base6 = g_naBaseMsAtDeg6.load(std::memory_order_relaxed);
    if (base6) {
        void* lu = nullptr;
        if (ports::player_combat::QueryLocalUser(&lu) && lu) {
            const DWORD scaled = attack_accel::EstimateDamageDelayScaleMs(lu, base6);
            g_nativeNaFloorMs.store(scaled, std::memory_order_relaxed);
            int deg = 6;
            if (attack_accel::QueryAttackSpeedDegree(lu, deg)) {
                g_naLastDegree.store(deg, std::memory_order_relaxed);
            }
            return scaled;
        }
        return base6;  // deg6 自洽点
    }
    return g_nativeNaFloorMs.load(std::memory_order_relaxed);
}

// 结算「接刀成功墙钟 → 忙锁自然卸下」。主动 ClearActionBusy 的刀不走这里。
void PollNativeNaFloor(DWORD /*tickNow*/) {
    const DWORD endAt = GetTickCount();
    const DWORD arm = g_naBusyArmMs.load(std::memory_order_relaxed);
    if (!arm) return;
    if (g_naFloorSkipBusySample.load(std::memory_order_relaxed)) {
        g_naBusyArmMs.store(0, std::memory_order_relaxed);
        return;
    }
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) return;
    int busy = -1;
    if (!attack_accel::QueryActionBusy(lu, busy)) return;
    if (busy >= 0) return;  // 仍在动作中
    g_naBusyArmMs.store(0, std::memory_order_relaxed);
    if (endAt >= arm) FeedNativeNaFloor(endAt - arm, "busy_cycle");
}

// 多发主动清忙锁后，用 ok→ok 间隔校准 base@deg6（仅 NA-only；okAt 须为成功瞬间）。
void NoteNaOkInterval(DWORD okAt) {
    const DWORD last = g_lastMultiNaOkMs.load(std::memory_order_relaxed);
    if (last && okAt > last) FeedNativeNaFloor(okAt - last, "ok_gap");
}

// 出刀前清客户端 ActionBusy，让 SendUse/OnFuncKey 不被本地忙锁吞；频率靠间隔/CD。
bool ClearBusyForCast() {
    void* lu = nullptr;
    if (!ports::player_combat::QueryLocalUser(&lu) || !lu) return false;
    return attack_accel::ClearActionBusy(lu);
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
    g_lastMultiNaOkMs = 0;
    g_naBaseMsAtDeg6 = 0;
    g_nativeNaFloorMs = xcat::kMultiSkillNativeNaFloorBootstrapMs;
    g_naTableMs = 0;
    g_naActionBaseSum = 0;
    g_naBusyArmMs = 0;
    g_naFloorSkipBusySample = false;
    g_naLastDegree = 6;
    g_selectHasSkill = false;
    g_naNativeGate = false;
    g_naBusyProbeActive = false;
    g_waveIntervalMs = 0;
    g_weaponChanLastOkMs = 0;
    g_lastNaActionIdx = -1;
    g_bowMeleeLatch = false;
    g_bowMeleeShootStreak = 0;
    {
        std::lock_guard<std::mutex> lk(g_skillAnimMu);
        g_skillAnimMs.clear();
        g_skillBaseSum.clear();
        g_skillLastOkMs.clear();
        g_skillBusyArmMs.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        g_queue.clear();
    }
    runtime::LogI("MultiSkill",
                  "port init (naIv=action_table|busy; skill=table|busy; wave=max; bootstrap=%ums)",
                  xcat::kMultiSkillNativeNaFloorBootstrapMs);
}

void Shutdown() {
    CancelPendingBurstForRetarget();
    g_enabled = false;
}

void SetConfig(bool enabled, uint32_t gapMs, bool safeStagger) {
    const bool was = g_enabled.load(std::memory_order_relaxed);
    g_enabled = enabled;
    g_gapMs = xcat::ClampMultiSkillGapMs(gapMs);
    g_safeStagger = safeStagger;
    // 关闭：丢掉在途波，避免 Tick 空转仍派发残留。
    if (was && !enabled) {
        ClearPostNaSkills();
        {
            std::lock_guard<std::mutex> lk(g_queueMu);
            g_queue.clear();
        }
        g_burstBusyUntil = 0;
        g_naBusyProbeActive.store(false, std::memory_order_relaxed);
        g_naNativeGate.store(false, std::memory_order_relaxed);
        g_bowMeleeLatch.store(false, std::memory_order_relaxed);
        g_bowMeleeShootStreak.store(0, std::memory_order_relaxed);
        runtime::LogI("MultiSkill", "disabled → clear queue/latch");
    }
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
    if (until && static_cast<int>(until - now) > 0) return true;
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        if (!g_queue.empty()) return true;
    }
    // 纯普攻 / 探针 / 空蓝 / 本波无待施技能：跟引擎 ActionBusy，否则叠 NA 机枪。
    // 「本波无技能」不依赖 800ms fallback 窗——空蓝 post/队列空即永久等同纯普攻。
    if (!g_selectHasSkill.load(std::memory_order_relaxed) ||
        g_naBusyProbeActive.load(std::memory_order_relaxed) ||
        g_naNativeGate.load(std::memory_order_relaxed) ||
        MpNaFallbackActive(now) || !HasPendingSkills()) {
        void* lu = nullptr;
        int busy = -1;
        if (ports::player_combat::QueryLocalUser(&lu) && lu &&
            attack_accel::QueryActionBusy(lu, busy) && busy >= 0) {
            return true;
        }
    }
    return false;
}

bool CancelPendingBurstForRetarget() {
    bool hadPending = false;
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        // 换靶只丢掉未发出的普攻；已入队的技能（post-NA 蜗牛等）保留，
        // 否则 ClearLock 一刷就把 +gap 的技能清光（BIN：NA ok → 100ms cancelled）。
        size_t keep = 0;
        for (size_t i = 0; i < g_queue.size(); ++i) {
            if (g_queue[i].skillId == kPendingNormalAttack) {
                hadPending = true;
                continue;
            }
            if (keep != i) g_queue[keep] = g_queue[i];
            ++keep;
        }
        if (keep != g_queue.size()) hadPending = true;
        g_queue.resize(keep);
    }
    // 普攻还没成功就换靶：撤掉尚未入队的 post-NA 清单。
    ClearPostNaSkills();
    const bool wasBusy = g_burstBusyUntil.load() != 0;
    if (g_queue.empty()) g_burstBusyUntil = 0;
    if (hadPending) {
        runtime::LogI("MultiSkill", "pending NA cancelled (skills kept)");
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

    // 勾选层：是否含普攻 / 攻击技（与本 tick 是否 skill_anim 可施放无关）。
    bool wantNa = false;
    bool selectHasSkill = false;
    for (const std::string& code : sel) {
        if (xcat::IsNormalAttackCode(code.c_str())) {
            wantNa = true;
            continue;
        }
        if (ParseSkillId(code) > 0) selectHasSkill = true;
    }
    g_selectHasSkill.store(selectHasSkill, std::memory_order_relaxed);

    std::vector<int> skillIds;
    skillIds.reserve(sel.size());
    uint32_t mpSkip = 0;
    x::ui::player::Vitals vitals{};
    const bool vitalsOk = x::ui::player::Read(vitals) && vitals.ok;
    // 空蓝窗到期后若已能负担任一勾选技，解除抑制。
    if (MpNaFallbackActive(now) && vitalsOk) {
        bool canAfford = false;
        for (const std::string& code : sel) {
            if (xcat::IsNormalAttackCode(code.c_str())) continue;
            const int id = ParseSkillId(code);
            if (id <= 0) continue;
            const int mpCon = ports::skill::GetSkillMpCon(id);
            if (mpCon <= 0 || vitals.mp >= mpCon) {
                canAfford = true;
                break;
            }
        }
        if (canAfford) {
            g_mpNaFallbackUntil.store(0, std::memory_order_relaxed);
            runtime::LogI("MultiSkill", "mp_gate clear (mp=%d can_afford)", vitals.mp);
        }
    }
    const bool mpFallback = MpNaFallbackActive(now);
    const auto& skillPack = xcat::GetSharedSkillNames(runtime::GetBinDir());
    for (const std::string& code : sel) {
        if (xcat::IsNormalAttackCode(code.c_str())) continue;
        if (mpFallback) {
            ++skip;
            continue;
        }
        const int id = ParseSkillId(code);
        if (id <= 0) {
            ++skip;  // junk
            continue;
        }
        // 用户已勾选：keepIfSelected，避免目录类型误判把蜗牛等踢出。
        if (!xcat::SkillLooksLikeAttackCandidate(skillPack, code.c_str(), /*keepIfSelected=*/true)) {
            ++skip;
            continue;
        }
        const int lv = ports::skill::GetSkillLevel(id);
        if (lv <= 0) {
            ++skip;
            continue;
        }
        // 空蓝预检：不够蓝的技不入队（DoActive 也会 short_mp；此处提前回退普攻）。
        const int mpCon = ports::skill::GetSkillMpCon(id);
        if (vitalsOk && mpCon > 0 && vitals.mp < mpCon) {
            ++skip;
            ++mpSkip;
            runtime::LogI("MultiSkill", "TryCast skip id=%d reason=short_mp need=%d have=%d", id,
                          mpCon, vitals.mp);
            continue;
        }
        // 同技能动作闸：无普攻时直接跳过；有普攻则仍入 post-NA（接刀后 Tick 短等），
        // 才能稳定 A→技→A→技，避免隔波只打 A。
        DWORD animRem = 0;
        if (SkillAnimBlocking(id, now, &animRem)) {
            if (!wantNa) {
                ++skip;
                runtime::LogI("MultiSkill", "TryCast skip id=%d reason=skill_anim rem=%ums", id,
                              animRem);
                continue;
            }
            runtime::LogI("MultiSkill", "TryCast post_na id=%d skill_anim rem=%ums (enqueue anyway)",
                          id, animRem);
        }
        // 只发不在 CD 的：有表 CD 的技仍走 CoolTimeOver。
        const float cdRem = ports::skill::GetSkillCooldownRemainSec(id);
        if (cdRem > kSkipCooldownRemainSec) {
            ++skip;
            runtime::LogI("MultiSkill", "TryCast skip id=%d reason=cooldown rem=%.2fs", id, cdRem);
            continue;
        }
        skillIds.push_back(id);
    }

    // 勾选技全因空蓝跳过 / 空蓝抑制窗：强制回退普攻（哪怕清单没勾普攻）。
    if (skillIds.empty() && selectHasSkill && (mpSkip > 0 || mpFallback)) {
        if (!wantNa) {
            wantNa = true;
            runtime::LogW("MultiSkill", "TryCast mp_empty → force NA (mp=%d skipped=%u hold=%d)",
                          vitalsOk ? vitals.mp : -1, mpSkip, mpFallback ? 1 : 0);
        }
        if (mpSkip > 0) {
            g_mpNaFallbackUntil.store(now + kMpNaFallbackMs, std::memory_order_relaxed);
        }
    }

    // 带技能但尚未读到 Action 表 / busy_cycle：本波只打普攻、不清忙，量官方出刀周期。
    // 空蓝回退时不做探针（本波本来就只打 NA）。有表后不再探针。
    bool floorProbe = false;
    if (wantNa && selectHasSkill && !g_naActionBaseSum.load(std::memory_order_relaxed) &&
        !g_naTableMs.load(std::memory_order_relaxed) &&
        !g_naBaseMsAtDeg6.load(std::memory_order_relaxed) && !attack_accel::IsDesired() &&
        mpSkip == 0 && !MpNaFallbackActive(now)) {
        floorProbe = true;
        skillIds.clear();
        g_naBusyProbeActive.store(true, std::memory_order_relaxed);
        runtime::LogI("MultiSkill", "na_floor_probe arm (need action_table/busy_cycle for naIv)");
    } else if (g_naActionBaseSum.load(std::memory_order_relaxed) ||
               g_naTableMs.load(std::memory_order_relaxed) ||
               g_naBaseMsAtDeg6.load(std::memory_order_relaxed)) {
        g_naBusyProbeActive.store(false, std::memory_order_relaxed);
    }

    // 纯普攻 = 没勾技能；探针波 / 空蓝回退临时等同纯普攻出刀路径。
    const bool mpNaOnly = wantNa && skillIds.empty() && (mpSkip > 0 || MpNaFallbackActive(now));
    const bool naOnly = (wantNa && !selectHasSkill) || floorProbe || mpNaOnly;
    g_naNativeGate.store(naOnly, std::memory_order_relaxed);
    const DWORD naIv = EffectiveMultiNaIntervalMs();
    // 同波：max(普攻, 本波技能自身间隔) → A→技 对齐更慢的一侧，蜗牛不跟普攻借速。
    const DWORD waveIv = naOnly ? naIv : WaveIntervalMs(skillIds);
    g_waveIntervalMs.store(waveIv, std::memory_order_relaxed);
    DWORD naDue = now;
    if (wantNa) {
        // 纯普攻/探针：立刻入队，Tick 里等 ActionBusy；
        // 带技能：按 waveIv 限频。
        if (!naOnly) {
            const DWORD lastNa = g_lastMultiNaOkMs.load();
            if (lastNa && static_cast<DWORD>(now - lastNa) < waveIv) {
                naDue = lastNa + waveIv;
            }
        }
        PendingCast pc{};
        pc.skillId = kPendingNormalAttack;
        pc.dueMs = naDue;
        pending.push_back(pc);
    }

    if (wantNa) {
        const uint32_t skillN = static_cast<uint32_t>(skillIds.size());
        {
            std::lock_guard<std::mutex> lk(g_postNaMu);
            g_postNaSkills.swap(skillIds);
        }
        // pending 仅 NA；技能数计入 scheduled 预估（实际入队在接刀后）。
        const uint32_t scheduled = 1u + skillN;
        const uint32_t span = skillN ? skillN * gap : 0;
        {
            std::lock_guard<std::mutex> lk(g_queueMu);
            g_queue.swap(pending);
        }
        // 纯普攻/探针切勿把 gap 加进 burst。
        g_burstBusyUntil = naOnly ? (naDue + 50u) : (naDue + span + gap + 50u);
        g_lastScheduled = scheduled;
        g_lastSkip = skip;
        g_lastSpanMs = span;
        g_lastSelCount = static_cast<uint32_t>(sel.size());
        g_lastCastCount = scheduled;
        g_lastBurstTickMs = GetTickCount64();

        runtime::LogI("MultiSkill",
                      "TryCast ok scheduled=%u (na+post) skip=%u span=%ums gap=%u naIv=%u "
                      "wave=%ums sel=%u naOnly=%d hasSkill=%d probe=%d",
                      scheduled, skip, span, gap, naIv, waveIv, g_lastSelCount.load(),
                      naOnly ? 1 : 0, selectHasSkill ? 1 : 0, floorProbe ? 1 : 0);
        SetOut(out, outSz, "ok");
        Tick();
        return true;
    }

    // 无普攻：技能立刻按 gap 串发（DoActive，见 PendingCast.sendUseOnly=false）。
    g_naNativeGate.store(false, std::memory_order_relaxed);
    uint32_t idx = 0;
    for (int id : skillIds) {
        PendingCast pc{};
        pc.skillId = id;
        pc.dueMs = now + idx * gap;
        pc.sendUseOnly = false;
        pending.push_back(pc);
        ++idx;
    }
    ClearPostNaSkills();

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

    const DWORD firstDue = pending.front().dueMs;
    const DWORD lastDue = pending.back().dueMs;
    const uint32_t span =
        (pending.size() > 1) ? static_cast<uint32_t>(lastDue - firstDue) : 0;
    const uint32_t scheduled = static_cast<uint32_t>(pending.size());
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        g_queue.swap(pending);
    }
    g_burstBusyUntil = lastDue + gap + 50;
    g_lastScheduled = scheduled;
    g_lastSkip = skip;
    g_lastSpanMs = span;
    g_lastSelCount = static_cast<uint32_t>(sel.size());
    g_lastCastCount = scheduled;
    g_lastBurstTickMs = GetTickCount64();

    runtime::LogI("MultiSkill",
                  "TryCast ok scheduled=%u skip=%u span=%ums gap=%u naIv=%u sel=%u", scheduled,
                  skip, span, gap, naIv, g_lastSelCount.load());
    SetOut(out, outSz, "ok");
    Tick();
    return true;
}

void Tick() {
    // 普攻 Down 后异步 Up：多发 worker 也要泵（simple_combat 关着/切图时仍可能挂着 pending Up）。
    ports::attack::TickReleases();
    // 未开启：不轮询 na/skill 忙锁、不派发队列（关开关时 SetConfig 已清空）。
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    const DWORD nowPoll = GetTickCount();
    PollNativeNaFloor(nowPoll);
    PollSkillAnimBusy(nowPoll);

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
            auto requeueNa = [&](const char* why) {
                if (pc.retries >= kNaMaxRetries) {
                    ClearPostNaSkills();
                    g_naBusyProbeActive.store(false, std::memory_order_relaxed);
                    runtime::LogW("MultiSkill",
                                  "cast NormalAttack ok=0 give_up after %u retries (%s)",
                                  kNaMaxRetries, why ? why : "?");
                    return;
                }
                PendingCast again = pc;
                again.retries = static_cast<uint8_t>(pc.retries + 1);
                again.dueMs = now + kNaRetryMs;
                {
                    std::lock_guard<std::mutex> lk(g_queueMu);
                    // 技能在接刀后才入队；此处队列里通常只剩未到期项，不必整体平移。
                    g_queue.push_back(again);
                }
                const DWORD until = g_burstBusyUntil.load();
                if (until) {
                    g_burstBusyUntil = until + kNaRetryMs;
                } else {
                    g_burstBusyUntil = again.dueMs + 50u;
                }
                runtime::LogI("MultiSkill", "cast NormalAttack defer=%s retry=%u/%u in %ums",
                              why ? why : "?", again.retries, kNaMaxRetries, kNaRetryMs);
            };

            // 清忙仅当：勾了技能 + 本波 post-NA/队列里真有技。空蓝无技 → 永久等同纯普攻。
            const bool withSkills = g_selectHasSkill.load(std::memory_order_relaxed) &&
                                    !g_naBusyProbeActive.load(std::memory_order_relaxed) &&
                                    !g_naNativeGate.load(std::memory_order_relaxed) &&
                                    !MpNaFallbackActive(now) && HasPendingSkills();

            if (withSkills) {
                // 先让上一技 busy_cycle 结完，再清忙——否则量不到蜗牛自己的间隔。
                PollSkillAnimBusy(now);
                {
                    bool measuring = false;
                    {
                        std::lock_guard<std::mutex> lk(g_skillAnimMu);
                        measuring = !g_skillBusyArmMs.empty();
                    }
                    if (measuring) {
                        void* luBusy = nullptr;
                        int busy = -1;
                        if (ports::player_combat::QueryLocalUser(&luBusy) && luBusy &&
                            attack_accel::QueryActionBusy(luBusy, busy) && busy >= 0) {
                            requeueNa("skill_busy");
                            continue;
                        }
                        PollSkillAnimBusy(now);  // busy 已落，补结算
                    }
                }
                const DWORD lastNa = g_lastMultiNaOkMs.load();
                DWORD waveIv = g_waveIntervalMs.load(std::memory_order_relaxed);
                if (!waveIv) waveIv = EffectiveMultiNaIntervalMs();
                // 武器通道技与 NA 共用节奏：也受 weaponChan 约束。
                const DWORD lastWpn = g_weaponChanLastOkMs.load(std::memory_order_relaxed);
                const DWORD naIv = EffectiveMultiNaIntervalMs();
                if (lastWpn && naIv && static_cast<DWORD>(now - lastWpn) < naIv) {
                    requeueNa("weapon_channel");
                    continue;
                }
                if (lastNa && static_cast<DWORD>(now - lastNa) < waveIv) {
                    requeueNa("wave_interval");
                    continue;
                }
                // 仅当后续技有专属 action / 已证实非武器通道时才 ClearBusy（斷魂贴脸禁止）。
                const bool clearBusy = PendingSkillsAllowClearBusyStack();
                g_naFloorSkipBusySample.store(clearBusy, std::memory_order_relaxed);
                g_naBusyArmMs.store(0, std::memory_order_relaxed);
                if (clearBusy) {
                    (void)ClearBusyForCast();
                } else {
                    // 跟 ActionBusy：贴脸挥弓与普攻串行，避免 1s 内叠 6 刀。
                    void* luBusy = nullptr;
                    int busy = -1;
                    if (ports::player_combat::QueryLocalUser(&luBusy) && luBusy &&
                        attack_accel::QueryActionBusy(luBusy, busy) && busy >= 0) {
                        requeueNa("action_busy");
                        continue;
                    }
                    g_naFloorSkipBusySample.store(false, std::memory_order_relaxed);
                }
            } else {
                // 纯普攻/探针：等引擎 ActionBusy，不清忙；用成功瞬间打戳量 busy_cycle。
                void* luBusy = nullptr;
                int busy = -1;
                if (ports::player_combat::QueryLocalUser(&luBusy) && luBusy &&
                    attack_accel::QueryActionBusy(luBusy, busy) && busy >= 0) {
                    requeueNa("action_busy");
                    continue;
                }
                g_naFloorSkipBusySample.store(false, std::memory_order_relaxed);
            }

            const bool ok = ports::attack::TryFirePrimaryForMultiSkill();
            int spV = 0, b0 = -2, b1 = -2;
            uint32_t spFh = 0;
            ground_spoof::FireDebug(&spV, &spFh);
            ports::attack::FireOutcomeDebug(&b0, &b1);
            int deg = -1;
            {
                void* luDeg = nullptr;
                if (ports::player_combat::QueryLocalUser(&luDeg)) {
                    (void)attack_accel::QueryAttackSpeedDegree(luDeg, deg);
                }
            }
            auto onNaOk = [&](const char* tag) {
                const DWORD okAt = GetTickCount();  // 出手成功墙钟，禁用 Tick 入口 now
                const bool gotTable = TryFeedNaFromActionTable();
                NoteWeaponChannelOk(okAt);  // 普攻占用武器出刀通道
                const bool clearBusyAfter =
                    withSkills && PendingSkillsAllowClearBusyStack();
                if (clearBusyAfter) {
                    (void)ClearBusyForCast();
                } else if (!gotTable) {
                    // 表读失败才走忙锁边沿 / ok_gap 回落
                    g_naBusyArmMs.store(okAt, std::memory_order_relaxed);
                    NoteNaOkInterval(okAt);
                } else {
                    g_naBusyArmMs.store(0, std::memory_order_relaxed);
                }
                g_lastMultiNaOkMs.store(okAt, std::memory_order_relaxed);
                EnqueueSkillsAfterNa(okAt, EffectiveGapMs());
                if (!withSkills) g_burstBusyUntil = 0;
                // 战斗空刀观察：以真实 NA 挥出为起点（勿在 TryCast 排程时 Arm）。
                x::features::simple_combat::NotifyMultiNormalAttackFired();
                runtime::LogI("MultiSkill",
                              "cast NormalAttack ok=1 path=OnFuncKey %s sp=%d spfh=%u b0=%d b1=%d "
                              "naIv=%u wave=%ums deg=%d clearBusy=%d nativeGate=%d table=%d",
                              tag ? tag : "", spV, spFh, b0, b1, EffectiveMultiNaIntervalMs(),
                              g_waveIntervalMs.load(std::memory_order_relaxed), deg,
                              clearBusyAfter ? 1 : 0,
                              g_naNativeGate.load(std::memory_order_relaxed) ? 1 : 0,
                              gotTable ? 1 : 0);
            };
            if (ok && b1 >= 0) {
                onNaOk("");
                continue;
            }
            if (ok && b1 == -1) {
                // OnFuncKey 返回了，但引擎没起动作（吞刀）；按失败重试。
                requeueNa("swallowed");
                continue;
            }
            if (ok && b1 == -2) {
                onNaOk("(busy_unread)");
                continue;
            }
            // pendingUp / OnFuncKey 硬失败（已跳过战斗 SoftBlocked）
            requeueNa(ok ? "unknown" : "soft_or_fail");
            continue;
        }
        auto requeueSkill = [&](const char* why, DWORD delayMs, uint8_t maxRetries) {
            if (pc.retries >= maxRetries) {
                runtime::LogW("MultiSkill", "cast id=%d give_up after %u retries (%s)",
                              pc.skillId, maxRetries, why ? why : "?");
                EnterMpNaFallback(now, pc.skillId, why ? why : "give_up");
                return;
            }
            PendingCast again = pc;
            again.retries = static_cast<uint8_t>(pc.retries + 1);
            again.dueMs = now + delayMs;
            {
                std::lock_guard<std::mutex> lk(g_queueMu);
                g_queue.push_back(again);
            }
            const DWORD until = g_burstBusyUntil.load();
            const DWORD need = again.dueMs + 50u;
            if (!until || static_cast<int>(need - until) > 0) g_burstBusyUntil = need;
            runtime::LogI("MultiSkill", "cast id=%d defer=%s retry=%u/%u in %ums", pc.skillId,
                          why ? why : "?", again.retries, maxRetries, delayMs);
        };

        // 到期再检：动作间隔（主）+ CoolTimeOver（有表 CD 时）。
        if (MpNaFallbackActive(now)) {
            EnterMpNaFallback(now, pc.skillId, "hold");
            continue;
        }
        PollSkillAnimBusy(now);
        DWORD animRem = 0;
        if (SkillAnimBlocking(pc.skillId, now, &animRem)) {
            requeueSkill("skill_anim", animRem > 40u ? animRem : 40u, kNaMaxRetries);
            continue;
        }
        const float cdRem = ports::skill::GetSkillCooldownRemainSec(pc.skillId);
        if (cdRem > kSkipCooldownRemainSec) {
            runtime::LogI("MultiSkill", "cast id=%d skip reason=cooldown rem=%.2fs", pc.skillId,
                          cdRem);
            continue;
        }

        // 上一技 busy 还在量：等自然结束，禁止预清忙把采样砍断。
        bool measuring = false;
        {
            std::lock_guard<std::mutex> lk(g_skillAnimMu);
            measuring = !g_skillBusyArmMs.empty();
        }
        if (measuring) {
            void* luBusy = nullptr;
            int busy = -1;
            if (ports::player_combat::QueryLocalUser(&luBusy) && luBusy &&
                attack_accel::QueryActionBusy(luBusy, busy) && busy >= 0) {
                requeueSkill("skill_busy", 40u, kNaMaxRetries);
                continue;
            }
            PollSkillAnimBusy(now);
        }

        // 专属 action 可 ClearBusy；弓无 action/0 仅在最近 NA 为射击时齐发，否则跟武器通道。
        const bool stackSkills = g_selectHasSkill.load(std::memory_order_relaxed) &&
                                 !g_naNativeGate.load(std::memory_order_relaxed) &&
                                 !MpNaFallbackActive(now) && !measuring &&
                                 SkillAllowsClearBusyStack(pc.skillId);
        if (stackSkills) {
            (void)ClearBusyForCast();
        } else if (!measuring) {
            // 武器通道 / 未分类：等 ActionBusy 自然结束再出下一刀。
            void* luBusy = nullptr;
            int busy = -1;
            if (ports::player_combat::QueryLocalUser(&luBusy) && luBusy &&
                attack_accel::QueryActionBusy(luBusy, busy) && busy >= 0) {
                requeueSkill("action_busy", 40u, kNaMaxRetries);
                continue;
            }
        }

        bool notReady = false;
        char reason[32]{};
        const bool ok =
            pc.sendUseOnly
                ? ports::skill::CastSkillSendUseOnly(pc.skillId, &notReady, reason, sizeof(reason))
                // 多发攻击：DoActive 拒施不进 Prepare（有蓝 prepare_false 短重试已证扰战斗）
                : ports::skill::CastSkill(pc.skillId, &notReady, reason, sizeof(reason),
                                          /*noPrepareFallback=*/true);
        if (ok) {
            const DWORD okAt = GetTickCount();
            NoteSkillCastOk(pc.skillId, okAt);
            if (IsWeaponBoundSkill(pc.skillId)) {
                NoteWeaponChannelOk(okAt);
            }
            if (stackSkills) {
                (void)ClearBusyForCast();  // 给队列下一技留空闲，同 NA→技
            }
            ports::skill::ConfirmLocalCooldown(pc.skillId, 0.f);
            // 叠技：burst 只覆盖 gap/队列排水，单技重复间隔由 SkillAnimBlocking 管。
            // 若这里写成 okAt+全长 anim，6 技同波会被「最后一技 anim」拖死下一波。
            const DWORD holdGap = EffectiveGapMs();
            DWORD holdMs = holdGap;
            if (!stackSkills) {
                const DWORD animMs = EffectiveSkillAnimMs(pc.skillId);
                holdMs = animMs;
                if (holdGap > holdMs) holdMs = holdGap;
                if (holdMs < 450u) holdMs = 450u;
            } else if (holdMs < 50u) {
                holdMs = 50u;
            }
            {
                std::lock_guard<std::mutex> lk(g_queueMu);
                if (!g_queue.empty() && stackSkills) {
                    // 同波后续技还在队列：IsBurstBusy 已因非空队列挡 TryCast，不必拉长 burst。
                    holdMs = holdGap > 50u ? holdGap : 50u;
                }
            }
            const DWORD hold = okAt + holdMs;
            const DWORD until = g_burstBusyUntil.load();
            if (!until || static_cast<int>(hold - until) > 0) g_burstBusyUntil = hold;
        } else if (notReady) {
            const bool mpGate = (std::strcmp(reason, "short_mp") == 0) ||
                                (std::strcmp(reason, "mp_vitals_fail") == 0) ||
                                (std::strcmp(reason, "mp_con_fail") == 0);
            const bool prepFail = (std::strcmp(reason, "prepare_false") == 0) ||
                                  (std::strcmp(reason, "seh_prepare") == 0);
            if (mpGate || prepFail) {
                // 空蓝 / 仍误入 Prepare：立刻回退普攻，禁止 120ms×N 软重试。
                EnterMpNaFallback(now, pc.skillId, reason);
            } else if (std::strcmp(reason, "send_use_false") == 0 ||
                       std::strncmp(reason, "do_false", 8) == 0) {
                // 弓无 action/0 + do_false：半近假齐发边角 → 闩上并丢掉跟刀，改走 NA-only。
                if (std::strncmp(reason, "do_false", 8) == 0 &&
                    !SkillHasOfflineAction(pc.skillId) &&
                    final_attack_force::EquippedWeaponIsBowFamily()) {
                    ArmBowMeleeLatch("do_false", /*dropPending=*/true);
                } else {
                    // DoActive 边沿拒施：短等再试 DoActive（已禁 Prepare），耗尽则 na_fallback。
                    requeueSkill(reason[0] ? reason : "cast_false", kSkillSoftRetryMs,
                                 kSkillSoftMaxRetries);
                }
            }
        }
        // 站立伪装取证（见 ground_spoof.h::CastDebug）：sp=1 才代表台确实种上了。
        int spV = 0;
        uint32_t spFh = 0;
        ground_spoof::CastDebug(&spV, &spFh);
        runtime::LogI("MultiSkill",
                      "cast id=%d ok=%d notReady=%d reason=%s sendUse=%d "
                      "clearBusy=%d anim=%ums sp=%d spfh=%u",
                      pc.skillId, ok ? 1 : 0, notReady ? 1 : 0, reason[0] ? reason : "-",
                      pc.sendUseOnly ? 1 : 0, stackSkills ? 1 : 0,
                      EffectiveSkillAnimMs(pc.skillId), spV, spFh);
    }

    if (!IsBurstBusy()) g_burstBusyUntil = 0;
}

}  // namespace x::features::ports::multi_skill
