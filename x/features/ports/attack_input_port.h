#pragma once
// attack_input_port — Classic TWMS 自动出刀（唯一真源）
//
// 真源：UserLocal.OnFuncKey(Down/Up, FuncKey)。
// 默认读 A 键（InputSystem.Key.A=15）绑定；空才回退 BasicActionAttack(5/52)。
// BIN：硬编 OnKey(Ctrl) 易假成功且 pktSum=0。
// 朝向 = VecCtrl.SetInput(±1,0) Resolve（禁 L/R OnKey Up SEH）。
// 拟人走路：默认内部输入（InputSystem Keyboard 设备状态，见 unity_kbd_port）。

#include <Windows.h>
#include <cstdint>

namespace x::features::ports::attack {

void Init();
void Shutdown();

void SetAttackVk(WORD vk);
WORD GetAttackVk();

void SetIntervalMs(DWORD ms);
DWORD GetIntervalMs();

// 出刀按键 hold（调试 TAB，默认 5ms）。实际 hold = min(此值, 面板间隔)。
// 攻击加速开启时走 Down+Up 同泵的 pulse 路径（hold=0），此值不参与。
void SetAttackHoldMs(DWORD ms);
DWORD GetAttackHoldMs();

void SetSmartInterval(bool on);
bool GetSmartInterval();

// 记录目标相对 dx；出刀前 ApplyFaceNow → VecCtrl.SetInput(±1,0) Resolve 朝向。
bool FaceToward(float dx);
bool ApplyFaceNow();

// 落地后对齐朝向：用引擎 MoveAction bit0（1=左）对照 dx。
// 已经对着 → 只同步 gLastFaceSign，不脉冲（BIN 06:41 朝向已对再 SetInput 会掐刀）。
// 不一致 → 清缓存后 SetInput。给 Settling 挂台后再调，下一拍才出刀（勿同拍翻面+挥）。
// engineMa<0（没读到）时不动。返回是否真下发了 SetInput。
bool AlignFaceToEngine(float dx, int engineMa);

// 上一次 ApplyFaceNow 的落地情况（只读探针，供 combat.log 逐刀取证）。
//   maOut   引擎 VecCtrl.moveAction；bit0=1 面朝左、0 面朝右。-1=本次未读到。
//   whyOut  0=已下发 SetInput（ma 为同帧实读）
//           1=|dx|<死区跳过  2=sticky 跳过（同号且 |dx| 小）
//           3=无主线程泵    4=job 超时    5=job 失败
// why≥1 时 ma 是**上一次成功下发**时读到的旧值，不能当本刀的引擎朝向用。
void FaceDebug(int* maOut, int* whyOut);

// 上一刀的**引擎判决**：派发前后各读一次动作忙位（LocalUser+ActionBusy，只读，
// 复用 attack_accel::QueryActionBusy 的 hash 防漂偏移）。派发是同步的，所以
// OnFuncKey 返回时忙位已经写好，这一对快照就把「引擎接没接这一刀」钉死了。
//   busy0  派发前，正常 -1（simple_combat 已在忙锁上先拦过一道）
//   busy1  派发后：
//            ≥0  接了；值即攻击动作 id，与攻击包 BODY+11 的 action 同源
//                （实测近战 5/6/7/16/17、蝸牛術 op=52 为 29）
//            -1  被吞了——OnFuncKey 正常返回但引擎没起动作，
//                典型就是没过技能派发里的地面门（站立伪装要治的正是这个）
//            -2  没读到（偏移未就绪 / 越界），别当成上面任何一种
// 注意这不是攻击包本身：包只在 busy1≥0 时才会发出去，要核 wire 字节仍看 send.log。
// 前提：**攻击加速必须关着**（v65 起面板已置灰、下发强制 0）。它的 worker 会往同一个
// 字段写 -1 清忙锁，一旦重开，busy1 就会被冲成 -1，"接了"会被误读成"被吞了"。
void FireOutcomeDebug(int* busy0, int* busy1);

// 站桩输出探针：Apply 后 / OnFuncKey 返回后（Restore 前）的 Ap、Apl。单位 px，取整。
// 未闪时全 0。
void BlinkDebug(int* apx, int* apy, int* aplx, int* aply, int* ap2x, int* ap2y);

// 这一刀是否需要**反向**转身（而非同向重申）。
// 反向会真下发 SetInput 触发转身动作，把同一拍的攻击顶掉——实证见 simple_combat.cpp
// 出刀点的「转身与出刀必须分拍」注释。同向重申无害，故只认换向。
// 调用方应在为真时「本拍只转身、下一拍再挥」。
bool FaceNeedsFlip(float dx);

// 拟人走路：默认灌 Keyboard 设备状态（失焦可走）；出刀/关 F5 前须 StopWalk。inputX 仅 -1/0/1。
// XCAT_WALK_KBD=0 回落 Win32 SendInput（需前台）；XCAT_WALK_KP=1 = PackBit 试验（证伪）。
bool HoldWalk(int inputX);
bool StopWalk();
bool IsWalkHeld();

// 站桩输出：同一泵 job 内把魂闪到 (x,y) 再 OnFuncKey，返回前 Restore。Up 不闪。
struct FireBlink {
    bool on = false;
    float x = 0.f;
    float y = 0.f;
};

// 间隔+松键门控后 OnFuncKey（A 槽绑定优先）。
// 软拒绝（间隔未到 / pendingUp / FireSuppressed）返回 false 且不计 fail；仅 OnFuncKey Down 失败计 fail。
bool TryFirePrimary();

// ignoreCombatInterval=true：跳过面板间隔 SoftBlocked（仍受 pendingUp / suppressed / 泵拥堵）。
// 换锁首刀用：BIN 22:41 近距已 Aim 仍 acq→fire≈240ms，几乎全是间隔残值。
bool TryFirePrimaryEx(bool ignoreCombatInterval);
bool TryFirePrimaryEx(bool ignoreCombatInterval, const FireBlink& blink);

// 技能多发专用：跳过战斗面板间隔 SoftBlocked，改由 multi_skill 固定 NA 间隔门控。
// 仍受 pendingUp / FireSuppressed / 泵拥堵约束；成功仍写入 gLastFireMs（与战斗路径共享冷却痕迹）。
bool TryFirePrimaryForMultiSkill();

void NoteLastFire();

// 先泵松键，再看间隔/pending/suppressed。
bool CanFirePrimary();
// 同上；ignoreCombatInterval=true 时不看面板间隔（换锁首刀与 TryFirePrimaryEx 配对）。
bool CanFirePrimaryEx(bool ignoreCombatInterval);

// Down 未 Up，或距上次成功出刀仍在 recover 窗内。
bool MotionBusy();

// buffs / timed_keys / ExternalPause：禁止新出刀；on 时 ForceRelease。
void SetFireSuppressed(bool on);
bool IsFireSuppressed();

// Hold 后短等：松键完成 + 距上次出刀 ≥ settleMs（默认 32），超时仍返回（不堵死施法）。
bool WaitFireIdle(DWORD timeoutMs, DWORD settleAfterFireMs = 32);

// <0 恢复默认 animBusy；>=0 覆盖前摇占用（攻击加速用）。
void SetAnimBusyOverrideMs(int ms);

// 攻击加速：Down 后同主线程立刻 Up（无 pending 跨 tick）；关闭则恢复 hold 异步松键。
void SetImmediateUp(bool on);
// 遗留 pulse API。站桩输出不再开启。
void SetBurstPulse(bool on);

void TickReleases();
void ForceRelease();

// 泵上只读：攻击槽（默认 A）当前 FuncKey。失败返回 false。
// type = FuncType（1=Skill 2=Item 5=BasicAction）；value = skillId 或 FkmType（普攻=52）。
bool PeekAttackBinding(int32_t* type, int32_t* value);

// Worker 可调：读上次 EnsureAttackFkOnMain 缓存的 t/v。从未解析则 false（t/v=-1）。
// 禁止在 worker 上调 PeekAttackBinding（会 false，且不得为读槽去 InvokeAndWait）。
bool PeekAttackBindingCached(int32_t* type, int32_t* value);

// 必须已在主线程泵上（禁止再 Invoke）。SetInput(0,0) 清走路锁存。
// fill+Doing / Settling 自愈用；ForceRelease 会再包一层 Invoke。
void ClearWalkLatchMainThread();

}  // namespace x::features::ports::attack
