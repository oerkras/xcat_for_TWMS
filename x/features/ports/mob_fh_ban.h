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
// 策略 A（默认）走这条；策略 B 改官方绑台，不走 VTOL。
void SetSpeedScale(float scale);
float SpeedScale();
void SetAntiJitter(bool on);
bool AntiJitterEnabled();
void SetPlayerLead(float vx, float vy);
// 聚拢偏移：吸怪 TAB 自定义落点。custom 关时用同一组默认（X=29，Y=9）。
// X 有符号：正=朝向面前，负=背后。Y = 相对人 AbsPos（更大 Y = 更高）。不贴台。
void SetGatherStandOff(bool custom, int32_t x, int32_t y);
void SetAimJitterPx(float px);
float AimJitterPx();
void QueryGatherStandOff(float* outX, float* outY);
void SetMaxArmed(int n);
void SetArmTimeoutMs(unsigned ms);
void SetActuatorParams(float kp, float dead, float cruiseR, float stationR, float maxCmd,
                       float gravity, float stickCreep, float stickStillV);
// 1X 档速（px/s）：巡航 / 进站 / 悬停。吸速% 再乘。
void SetMotionTiers(float cruiseV, float stationV, float holdV);
float CruiseRadius();
float StationRadius();
// 逐帧位移夹速（怪速举报 prevpos 根治）：把被拽怪每帧位移夹到 ≤ capPx（px/帧），
// 让其永远走客户端「正常移动包」路径、不触发「怪速+10」异常分支。默认开、默认 48px/帧。
// capPx ≤ 0 时只切开关、保留当前上限。真实阈值看 impact 日志的 disp/cap 实测标定。
// 面板入口：吸怪 快攻 TAB「防断」卡（payload v141 mobGatherDispClampOn/DispCapPx）。
void SetDispClamp(bool on, float capPx);
bool DispClampEnabled();
float DispClampCapPx();
// 吸怪策略：0=A IMPACT（现有 SetImpactNext 悬停），1=B 卸台后一帧写 Ap 到落点，不挂台。
// B 不调 SetActive / SetImpactNext / PlantOnFh。切换会 ClearAll。默认 0，不改 A。
void SetStrategy(unsigned strategy);
unsigned Strategy();
// 策略 A 子项「到站落地」：怪水平到站(≤stationR)后松手，交给游戏原生物理自然落台（不再逐帧摘台/
// 定住）；玩家/怪走远(>cruiseR)恢复吸拉。落点由聚拢站距(offX/offY)自调。默认关。
// 面板入口：吸怪 快攻 TAB「防断」卡（payload v144 mobGatherLandOnArrive）。
void SetLandOnArrive(bool on);
bool LandOnArriveEnabled();
// vc 当前是否处于「到站落地」态：吸怪主循环据此跳过摘台/定住，交原生物理。
bool IsLanded(void* vc);
// v145 远怪接力跳（payload mobGatherHopPx）：服务器按「单次连续拉取总距 >~1200px」掐线，
// >px 的怪改为逐跳接力（每跳 ≤px、到中转点驻留 ~450ms 落账再起下一跳）。0=关（直拉）。
// 面板入口：吸怪 快攻 TAB「防断」卡。
void SetHopPx(float px);
float HopPx();
// 读 vc 槽位当前有效瞄点（接力跳时=中转点，否则=站点）。用于一次性速度命令别直瞄远站点。
bool EffectiveAim(void* vc, float* tx, float* ty);
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
