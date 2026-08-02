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

// 短距：自选 ~140px 落点（朝向/键位 + SnapStand）。
bool TeleportNativeSkillCall();
// 长距：绝对坐标。snapStand=true 贴地种台（fh==0 硬拒 fill+Doing）；false=点飞原样（允许 fh0）。
bool TeleportNativeSkillCall(float landX, float landY, uint32_t plantFhId, bool snapStand = true);
void SetNativeCooldownMs(uint32_t ms);
// 从「现在」起强制自冷 ms（写入 lastOk=now）。补给开趟冷却窗用，挡住战斗残留 Doing。
void ForceNativeCooldownMs(uint32_t ms);
// 清自冷时钟（不改 CD 长度）。换图落地立刻放行 fill+Doing。
void ClearNativeSelfCd();

// 距下次可 fill+Doing 的剩余 ms；0=已就绪（未跳过则也是 0）。
uint32_t NativeCooldownRemainingMs();

}  // namespace x::features::ports::teleport
