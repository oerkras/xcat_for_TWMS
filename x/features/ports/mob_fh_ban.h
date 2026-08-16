#pragma once
// mob_fh_ban — 吸怪：按实例拦 VecCtrlMob 重挂台，让作动器维持位置。
// 不改 fly_fh_ban（那份只认 LocalUser klass）。全图改 Mob 虚表语义仍否决；
// 本钩装在 Mob klass 上，但只对白名单 vc 禁 CD/CDF。

#include <cstdint>

namespace x::features::ports::mob_fh_ban {

void Init();
void Shutdown();

// 已在 MainPump 上时调用（禁止再套 InvokeAndWait）。sampleVc 取 klass。
bool EnsureInstalledOnPump(void* sampleVc);

bool Arm(void* vc, void* mob, int32_t id, float aimX, float aimY);
void Disarm(void* vc);
void ClearFh(void* vc);
void ClearAll();
bool IsArmed(void* vc);
int ArmedCount();
int CopyArmedIds(int32_t* out, int cap);
// ClearAll 递增；清怪重连用来区分「死光」和「hold/落地被清空」。
uint32_t WipeGeneration();

// F5 旋翼翻版的怪侧 VTOL（CDF 里 SetImpactNext，不接 heli_rotor 单例）。
// 100% = 巡航 620 px/s。10X 走 F5 同款死拍。防抖 = 到位软钉。
void SetSpeedScale(float scale);
float SpeedScale();
void SetAntiJitter(bool on);
bool AntiJitterEnabled();
void SetPlayerLead(float vx, float vy);
// 聚拢偏移：吸怪 TAB 自定义落点。custom 关时用同一组默认（X=29，Y=9）。
// X 有符号：正=朝向面前，负=背后。Y = 相对人 AbsPos（更大 Y = 更高）。不贴台。
void SetGatherStandOff(bool custom, int32_t x, int32_t y);
void QueryGatherStandOff(float* outX, float* outY);
void SetMaxArmed(int n);
void SetArmTimeoutMs(unsigned ms);
void SetActuatorParams(float kp, float dead, float cruiseR, float stationR, float maxCmd,
                       float gravity, float stickCreep, float stickStillV);
// 1X 档速（px/s）：巡航 / 进站 / 悬停。吸速% 再乘。
void SetMotionTiers(float cruiseV, float stationV, float holdV);
float CruiseRadius();
float StationRadius();
// 到位圈 / 到位 Kp / ≥5X 刹车(ms) / 死区下滑切断(px/s)。
void SetSettleTune(float settleErr, float kpSettle, float brakeMs, float coastVy);
// 刷新所有白名单瞄准点 + 人速度前馈。CDF / worker ~17ms 调用；不进泵。
// ma = VecCtrl.MoveAction（faceLeft = ma&1）；聚拢点 = 面前站距，不贴台。
void TickPlayerAim(float x, float y, float vx, float vy, int ma = -1, float* outX = nullptr,
                   float* outY = nullptr);
unsigned LastAimDtMs();
void ComputeSetVelocity(float x, float y, float vx, float vy, float aimX, float aimY,
                        unsigned sinceMs, float* setVx, float* setVy);

// 作动器持有期间不因「到人高度」放行重挂。清超时 / 死对象 / Ap 归零；liveIds 非空时顺手剔快照里已经没了的。
void SweepStale(float playerY);
void SweepStale(float playerY, const int32_t* liveIds, int nLive);
void SweepLand(float playerY);  // 兼容旧名 → SweepStale

}  // namespace x::features::ports::mob_fh_ban
