#pragma once
// Classic TWMS — shared short combat pause for buffs / timed_keys.
//
// 职责：AcquireExternalPause 生命周期 + WaitFireIdle + 换图途中互斥（map arm / Settling）。
// 不做：施法、插键、视觉层、坐标 heal。
// BUFF Hold 可 lockTeleport（SkillCastBusy depth）：禁 fill+Doing（原生锁移动竞态）。
// 另：IsTeleportForbidden = SkillCastBusy ∪ IsPreparingSkill（手搓 BUFF 也禁传）。
// 定时键只停刀、不抬 busy（药水等不锁移动，避免白挡贴怪/飞）。
// 贴怪 MoveTo 不经 IsTeleportBlocked——由 ExternalPause 停刀后再动作（BIN 0.1.40）。

#include <Windows.h>
#include <cstdint>

namespace x::features::ports::action_gate {

enum class Block : uint8_t {
    Ok = 0,
    TeleportTransit,
};

// 会话内只 Acquire 一次；续期只推 until；失败路径必须 ReleaseNow。
class Hold {
public:
    // lockTeleport：首次 Arm 时是否 AcquireSkillCastBusy（BUFF=true，定时键=false）。
    void Arm(DWORD now, DWORD holdMs, bool lockTeleport = true);
    void ReleaseIfDue(DWORD now);
    void ReleaseNow();
    bool active() const { return held_; }
    bool locksTeleport() const { return busyHeld_; }

private:
    bool held_ = false;
    bool busyHeld_ = false;
    DWORD until_ = 0;
};

// 换图途中？不持闸（map arm / Settling；不含贴怪 MoveTo）。
bool IsTeleportBlocked();

// Arm → WaitFireIdle → 再查途中；若途中则 ReleaseNow 并返回 TeleportTransit。
Block BeginAct(Hold& hold, DWORD now, DWORD holdMs, DWORD settleTimeoutMs,
               DWORD settleAfterFireMs, bool lockTeleport = true);

// SkillCastBusy：depth>0 时传送/飞拒绝。仅由 lockTeleport=true 的 Hold 增减。
bool IsSkillCastBusy();

// 禁瞬移总闸：Hold busy ∪ 引擎 IsPreparingSkill（手搓 BUFF 也覆盖）。
bool IsTeleportForbidden();

// 是否有「别人的」busy Hold：排除 self（已 locksTeleport 且 depth==1 视为仅自身）。
bool IsSkillCastBusyAsideFrom(const Hold* self);

}  // namespace x::features::ports::action_gate
