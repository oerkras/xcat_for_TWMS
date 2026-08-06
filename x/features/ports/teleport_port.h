#pragma once
// teleport_port — Classic TWMS 瞬移（唯一产品路径：fill+Doing）
//
// TeleportNativeSkillCall = 手填 UserLocal.Teleport + Mp XY +（可选 CurFh）+ TryDoingTeleport
//
// 禁止再引入：ImpactNext / Attr=4 旁路 / SyncRelPosOnly settle / RestoreWalkable /
// Register 技能包 / 钉 Transform / VisualLeash。F6 点飞与贴怪一律走本 API。

#include <cstdint>

namespace x::features::ports::teleport {

bool EnsureBound();

// 进图物理就绪：PlayReady ∧ 本图 FH 缓存/图对齐 ∧ LocalUser+VecCtrl 可读。
// 未就绪时禁止 fill+Doing（防旧图台/RelPos 造成视觉假到位）。
bool IsPhysicsReadyForNative();

// 短距：自选 ~140px 落点（朝向/键位 + SnapStand）。
bool TeleportNativeSkillCall();
// 长距：绝对坐标。snapStand=true 贴地种台（fh==0 硬拒 fill+Doing）；false=点飞原样（允许 fh0）。
bool TeleportNativeSkillCall(float landX, float landY, uint32_t plantFhId, bool snapStand = true);
void SetNativeCooldownMs(uint32_t ms);
// 从「现在」起强制自冷 ms（写入 lastOk=now）。补给开趟冷却窗用，挡住战斗残留 Doing。
void ForceNativeCooldownMs(uint32_t ms);
// 清自冷时钟（不改 CD 长度）。换图落地勿再立刻 Clear——改用 ForceNativeCooldownMs。
void ClearNativeSelfCd();

// 距下次可 fill+Doing 的剩余 ms；0=已就绪（未跳过则也是 0）。
uint32_t NativeCooldownRemainingMs();

// 最近一次 native ok 后的收态窗（buffs/timed_keys 勿插 DoActive/键）。
// quietMs 默认 220：覆盖 tp_fire（settle=0）离开 Settling 后的发包缝。
bool IsPostTeleportQuiet(uint32_t quietMs = 220);

// 必须已在主线程泵上。清 InputX + 零 Ap.V/RelPos.V。
// replant=true：另种 CurFh+RelPos（仅 Ap 仍贴近 land 时）；Ap 已漂时禁止 RelPos 拧回 land（BIN 39722a 软重载）。
bool StabilizeFootholdMainThread(float landX, float landY, uint32_t fhId, bool replant = true);

// 必须已在主线程。只清 InputX + 零速度，不拆 CurFh（同点 nan 交接用，避免无谓 land_miss）。
bool ClearMotionLatchMainThread();

}  // namespace x::features::ports::teleport
