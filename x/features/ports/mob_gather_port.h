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
// 「吸怪 快攻」TAB「快攻」卡「主动软重连」。不绑吸怪；试连未开则只起表不拆会话。
// 出过刀 = 欠一次 hangup 清加速 FLAG：第一刀才起表；出刀后关 F5 仍走完这一轮。
// 没出过刀：关 F5 不计时，满包可直接卖。勾选可单独开（不绑出刀）。
// freeze 临时关 F5 只停表（未出刀时），不清落地闸 / 不清欠 hangup。
// 脏会话到点必须洗：卖装/赶路/换图/开店不得清钟、不得冻秒数闸。重连在途 / AwaitLand /
// 卖装优先 hold 不起下一轮表。测谎答题中推迟拆会话，关题后强制拆一次；起号仍冻。
// F5 出过刀后 hangup 窗口硬期限：遇人换频 / 寻簇飞不冻钟，到点中止 hop 再拆。
// 「吸怪 快攻」TAB「快攻」卡「主动软重连」= 秒数闸。瞬移找怪+F5 默认强制开秒数（面板置灰，不改落盘）。
// 调试 TAB hangupUnbindF5 可解除该防呆，秒数闸只跟该勾选。
void SetSoftRelogin(bool on, unsigned sec);
void SetHangupUnbindF5(bool on);
// 同卡「出刀软重连」= 累计出刀闸。两勾独立；都开则先到先拆。
void SetHangupFires(bool on, unsigned n);
// 调试 TAB ws888 解锁后才给标题/顶栏报刀数；不影响出刀闸本身。
void SetHangupFiresUiUnlocked(bool on);
void TickSoftRelogin();
bool IsSoftReloginWanted();
// 立刻走主动软重连（遇人换新频 / 到点 hangup）。成功则欠落地清 FLAG。
bool FireProactiveHangup(const char* why);
// 卖装/补给等要出门：若本轮出过刀或刀数已到期，立刻软重连清 FLAG，调用方必须等落地。
// 已在拆/未出过刀：不重复拆。返回 true=先等 hangup，false=可以出门。
bool HangupBeforeOtherAction(const char* why);
// 与 combat.log「fire id=」同一拍 +1（那次峰值 2030 就是按分钟数这些行）。
// 自组发出 / 多发 NA 真正挥出同样 +1。不看 ActionBusy、不看命中。
void NoteHangupFire();
// 成功出刀：钉「欠 hangup」清加速 FLAG。TryFirePrimaryEx / NoteLastFire（forge）调用。
void NoteAttackDirty();
    // 落地后累计出刀已到阈值：必须先拆会话清 FLAG，卖装/赶路/补给不得插队。
// 到点必须 CloseSession；禁止只停手、禁止图内假落地清零。
bool HangupFiresDue();
// 重连在途 / AwaitLand / 出过刀未落地 禁卖。没出过刀、人已在图里：满包可直接出门。
// 倒计时将尽也推迟，避免出门撞上下一轮拆会话。
bool SoftReloginAllowsAutoSell();
// 已拆未落地或软重连在途：卖装必须让路，落地再开趟。
bool HangupWashInFlight();
// hangup 已开火、补给还没拍板：F5 不准恢复出刀（卖装优先于打怪）。
bool HangupCombatHold();
void ReleaseHangupCombatHold(const char* why);
// 顶栏倒计时：on=勾选 / 出过刀欠 hangup / 瞬移找怪+F5；paused=重连/测谎/起号冻钟；
// remainMs=剩余（未起表 0xFFFFFFFF）。
void QuerySoftReloginClock(unsigned* on, unsigned* paused, unsigned* remainMs, unsigned* needMs);
// 落地后累计出刀：count=本轮已出；need=阈值（0=关/未解锁，不画刀数）。
void QueryHangupFires(unsigned* count, unsigned* need);
// BIN/观测：累计与阈值，不因标题解锁而把 need 藏成 0。闸关则 need=0。
void QueryHangupFiresRaw(unsigned* count, unsigned* need);
void SetClearRelogin(bool on);
void TickClearRelogin();
// 「先飞到最密堆再吸」：默认关。关=站立吸怪；开=人飞到簇后再 Arm。
void SetSeekCluster(bool on);
// v146 远怪自动巡点：寻簇的跨层/全图版。没持怪时不限 |dY| 找全图最密堆、人飞过去就地吸。
// 动机：服务器按「离怪原位总位移 >~1200px」掐线，远怪拉不得，只能人过去。与 homeReturn 互斥。
void SetPatrolFar(bool on);
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
