#pragma once

#include <cstdint>

// mob_gather_port — Classic TWMS · 吸怪（首页）
//
// 只对本地正在模拟的怪（vcAct≠0 且 Ap 非原点）：统一卸台，CDF 里按 F5 旋翼控制律
// SET 落地速度吸到面前地面并悬停。≥5X 抄 F5 死拍。尸体 / 池槽复用不拉。
// 默认站立吸怪不占 heli_rotor；「先飞到最密堆再吸」/「软重连后返回原位」才用 Owner::Gather。
// 控权申请（TAB 选项，默认关）：泵上调官方 Mob.ApplyControl(tCur)，不钩 calc_priority、
// 不造 206、不写 0xE8。服端批 280 后才变本地模拟。
// 产品 = 经典版，不是枫星。文案 = 吸怪方便打，禁止写成防抢。
// 公开图 MobCtrl 常全 -1；以本地物理是否在跑当权威代理，不靠 ctrl>0。

namespace x::features::ports::mob_gather {

void Init();
void Shutdown();

void SetEnabled(bool on);
bool IsEnabled();
bool IsHoldActive();
// 遇人策略「遇人停吸」：不改 TAB 开关；上升沿卸白名单，期间不收怪。
void SetEncounterPause(bool on);
bool IsEncounterPaused();
// 新收半径（px）。已入白名单的怪飞出仍维持。站桩输出选怪圈对齐这个数。
float GatherRadiusPx();

void SetSpeedPct(unsigned pct);
void SetAntiJitter(bool on);
void SetMaxHold(unsigned n);
void SetFarInFlight(unsigned n);
void SetRadiusPx(unsigned px);
void SetLayerYPx(unsigned px);
void SetDyLimPx(unsigned px);
void SetWalkDx(unsigned px);
void SetFeetExemptPx(unsigned px);
void SetHoldTimeoutMs(unsigned ms);
void SetRecruitIntervalMs(unsigned ms);
unsigned RecruitIntervalMs();
void SetAimIntervalMs(unsigned ms);
unsigned AimIntervalMs();
void SetIgnoreQuiet(bool on);
void SetQuietDelayMs(unsigned ms);
void SetApplyCtrl(bool on);
// 首页挂机「主动软重连」。不绑吸怪；试连未开则只起表不拆会话。
// 出过刀 = 欠一次 hangup 清加速 FLAG：第一刀才起表；出刀后关 F5 仍走完这一轮。
// 没出过刀：关 F5 不计时，满包可直接卖。勾选可单独开（不绑出刀）。
// freeze 临时关 F5 只停表（未出刀时），不清落地闸 / 不清欠 hangup。
// 卖装/赶路冻钟。重连在途 / AwaitLand / 卖装优先 hold 不起下一轮表。
// 重连在途 / 出过刀未 hangup 禁自动卖装。
void SetSoftRelogin(bool on, unsigned sec);
void TickSoftRelogin();
bool IsSoftReloginWanted();
// 成功出刀：钉「欠 hangup」。TryFirePrimaryEx / NoteLastFire（forge）调用。
void NoteAttackDirty();
// 重连在途 / AwaitLand / 出过刀未落地 禁卖。没出过刀、人已在图里：满包可直接出门。
// 倒计时将尽也推迟，避免出门撞上下一轮拆会话。
bool SoftReloginAllowsAutoSell();
// hangup 已开火、补给还没拍板：F5 不准恢复出刀（卖装优先于打怪）。
bool HangupCombatHold();
void ReleaseHangupCombatHold(const char* why);
// 顶栏倒计时：on=勾选 / 出过刀欠 hangup / 瞬移找怪+F5；paused=hold/卖装赶路/静默冻钟；
// remainMs=剩余（未起表 0xFFFFFFFF）。
void QuerySoftReloginClock(unsigned* on, unsigned* paused, unsigned* remainMs, unsigned* needMs);
void SetClearRelogin(bool on);
void TickClearRelogin();
// 「先飞到最密堆再吸」：默认关。关=站立吸怪；开=人飞到簇后再 Arm。
void SetSeekCluster(bool on);
void TickSeekCluster();
bool IsSeekingCluster();
// 「软重连后返回原位」：与寻簇互斥。F5 开打怪 / 面板「记录人物坐标」记 AbsPos；回图后飞回该点再吸。
void SetHomeReturn(bool on);
void SetHomePos(int32_t x, int32_t y, int32_t mapId, bool valid, bool hasMap);
void TickHomeReturn();
void TickHomeRecord();
bool RecordHomeNow();
void RecordHomeOnF5();
void TickDyLimRamp();

struct OneshotResult {
    int considered = 0;
    int pushed = 0;
    const char* why = "";
};

// 必须在 worker 上调：内部 InvokeAndWait 一批；泵失败按未开启降级。
bool TryPushOneshot(OneshotResult* out);
bool TryPushPeriodic(OneshotResult* out);
void TickLandSweep();
void TickHoldWatch();

}  // namespace x::features::ports::mob_gather
