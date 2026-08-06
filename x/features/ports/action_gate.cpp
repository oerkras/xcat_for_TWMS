// Classic TWMS — shared short combat pause for buffs / timed_keys.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "action_gate.h"

#include "attack_input_port.h"
#include "skill_port.h"
#include "../simple_combat/simple_combat.h"

#include <atomic>

namespace x::features::ports::action_gate {
namespace {

// 与 ExternalPauseDepth 同形：仅 lockTeleport 的 Hold 可重叠抬 busy。
std::atomic<int> gSkillCastBusyDepth{0};

void AcquireSkillCastBusy() {
    gSkillCastBusyDepth.fetch_add(1, std::memory_order_acq_rel);
}

void ReleaseSkillCastBusy() {
    for (;;) {
        int cur = gSkillCastBusyDepth.load(std::memory_order_acquire);
        if (cur <= 0) {
            gSkillCastBusyDepth.store(0, std::memory_order_release);
            return;
        }
        if (gSkillCastBusyDepth.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) {
            return;
        }
    }
}

}  // namespace

void Hold::Arm(DWORD now, DWORD holdMs, bool lockTeleport) {
    if (!held_) {
        simple_combat::AcquireExternalPause();
        if (lockTeleport) {
            AcquireSkillCastBusy();
            busyHeld_ = true;
        }
        held_ = true;
    } else if (lockTeleport && !busyHeld_) {
        // 续期时升级为禁传（少见；保持 depth 与会话一致）。
        AcquireSkillCastBusy();
        busyHeld_ = true;
    }
    until_ = now + holdMs;
}

void Hold::ReleaseIfDue(DWORD now) {
    if (!held_) return;
    if (static_cast<int>(now - until_) < 0) return;
    // Prepare/警戒动画可能长于固定 Hold：未出态前续窗，避免闸放后立刻贴怪拆层。
    if (busyHeld_ && skill::IsPreparingSkill()) {
        until_ = now + 80;
        return;
    }
    simple_combat::ReleaseExternalPause();
    if (busyHeld_) {
        ReleaseSkillCastBusy();
        busyHeld_ = false;
    }
    held_ = false;
    until_ = 0;
}

void Hold::ReleaseNow() {
    if (!held_) return;
    simple_combat::ReleaseExternalPause();
    if (busyHeld_) {
        ReleaseSkillCastBusy();
        busyHeld_ = false;
    }
    held_ = false;
    until_ = 0;
}

bool IsTeleportBlocked() { return simple_combat::IsTeleportTransit(); }

Block BeginAct(Hold& hold, DWORD now, DWORD holdMs, DWORD settleTimeoutMs,
               DWORD settleAfterFireMs, bool lockTeleport) {
    if (IsTeleportBlocked()) return Block::TeleportTransit;
    hold.Arm(now, holdMs, lockTeleport);
    (void)ports::attack::WaitFireIdle(settleTimeoutMs, settleAfterFireMs);
    if (IsTeleportBlocked()) {
        hold.ReleaseNow();
        return Block::TeleportTransit;
    }
    return Block::Ok;
}

bool IsSkillCastBusy() {
    return gSkillCastBusyDepth.load(std::memory_order_acquire) > 0;
}

bool IsTeleportForbidden() {
    if (IsSkillCastBusy()) return true;
    return skill::IsPreparingSkill();
}

bool IsSkillCastBusyAsideFrom(const Hold* self) {
    const int depth = gSkillCastBusyDepth.load(std::memory_order_acquire);
    if (depth <= 0) return false;
    // 自身未抬 busy 时，任一 depth 都算外来。
    if (self && self->locksTeleport()) return depth > 1;
    return true;
}

}  // namespace x::features::ports::action_gate
